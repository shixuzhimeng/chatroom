#pragma once

#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <cstring>
#include <functional>
#include <memory>
#include <fcntl.h>
#include "logging.h"
#include <string>
#include "thread_pool.h"
#include "TLS/TLS.h"

class EpollWrapper {
public:
    EpollWrapper() {
        epfd_ = epoll_create1(0);
        if(epfd_ < 0) {
            LOG_FATAL << "epoll_creat failed: " << strerror(errno);
        }
    }
    ~EpollWrapper() {
        if(epfd_ > 0) {
            close(epfd_);
        }
    }

    bool add(int fd, uint32_t events, void* ptr = nullptr) {
        epoll_event ev;
        ev.events = events;
        ev.data.ptr = ptr;

        if(epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            LOG_ERROR << "epoll_ctl_add failed for fd :" << fd << ":" << strerror(errno);
            return false;
        }

        return true;
    }

    bool mod(int fd, uint32_t events, void* ptr = nullptr) {
        epoll_event ev;
        ev.events = events;
        ev.data.ptr = ptr;

        if(epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
            LOG_ERROR << "epoll_ctl_mod failed for fd" << fd << ":" << strerror(errno);
            return false;
        }

        return true;
    }

    bool del(int fd) {
        if(epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) < 0) {
            LOG_ERROR << "epoll_ctl_del failed for fd" << fd << ":" << strerror(errno);
            return false;
        }
        return true;
    }

    int wait(std::vector<epoll_event>& events, int timeout = -1) {
        events.resize(1024);
        int n = epoll_wait(epfd_, events.data(), events.size(), timeout);
        
        if(n < 0) {
            if(errno != EINTR) {
                LOG_ERROR << "epoll_wait failed: " << strerror(errno);
            }
            return n;
        }


        if(n > 0) {
            events.resize(n);
        }

        return n;
    }

    int fd() const{
        return epfd_;
    }

private:
    int epfd_;
};


class Buffer {
public:
    void Append(const char* data, size_t len) {
        buffer_.insert(buffer_.end(), data, data + len);
    }

    size_t readBytes() const{
        return buffer_.size() - read_index;
    }

    size_t allBytes() const{
        return buffer_.size();
    }

    const char* peek() const {
        if (readBytes() == 0) return nullptr;
        return buffer_.data() + read_index;
    }
    
    void costBytes(size_t len) {
        if(len < readBytes()) {
            read_index += len;
        }
        else {
            costall();
        }
    }

    void costall() {
        buffer_.clear();
        read_index = 0;
    }
    
    std::string toString() const {
        return std::string(peek(), readBytes());
    }

    size_t size() const{
        return buffer_.size();
    }

private:
    std::vector<char> buffer_;
    size_t read_index = 0;
};


class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    using Callback = std::function<void(std::shared_ptr<TcpConnection>)>;
    using MessageCallback = std::function<void(std::shared_ptr<TcpConnection>, Buffer&)>;
    std::chrono::steady_clock::time_point last_active_time;

    TcpConnection(int fd, int id = 0) : fd_(fd), connection_id(id), closed_(false), context(nullptr) {
        last_active_time = std::chrono::steady_clock::now();
        LOG_DEBUG << "Connection created: fd" << fd << std::endl;
    }

    ~TcpConnection() {
        if(fd_ > 0) {
            close(fd_);
            LOG_DEBUG << "Connection closed: fd" << fd_ << std::endl;
        }
    }

    Buffer& inputBuffer() {
        return input_buffer;
    }

    Buffer& outputBuffer() {
        return output_buffer;
    }

    void setMessageCallback(MessageCallback cb) {
        message_cb = cb;
    }

    void setWriteCallBack(Callback cb) {
        write_cb = cb;
    }

    void setCloseCallBack(Callback cb) {
        close_cb = cb;
    }
    
    void setContext (void* ctx) {
        context = ctx;
    };

    void* getContext() {
        return context;
    }

    uint64_t getUserID() const{
        return reinterpret_cast<uint64_t>(context);
    }

    void handleRead() {
        LOG_INFO << "handle read called";
        char buf[1024 * 64];
        while(true) {
            ssize_t n;
            if (use_tls_ && tls_socket_) {
                // 如果是TLS，需要处理握手
                if (!tls_handshaked_) {
                    int ret = tls_socket_->accept();
                    if (ret == 0) {
                        tls_handshaked_ = true;
                        LOG_DEBUG << "TLS handshake completed";
                    } else if (ret == -2) {
                        // 需要重试
                        break;
                    } else {
                        handleClose();
                        return ;
                    }
                    continue;
                }
                n = tls_socket_->read(buf, sizeof(buf));
            }
            else {
                n = read(fd_, buf, sizeof(buf));
            }

            if (n > 0) {
                input_buffer.Append(buf, n);
            } else if (n == 0) {
                LOG_INFO << "Connected closed by :fd:" << fd_;
                if(!closed_) {
                    closed_ = true;
                    if(close_cb) {
                        close_cb(shared_from_this());
                    }
                }
                return ;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                LOG_ERROR << "Read error: " << strerror(errno);
                if(!closed_) {
                    closed_ = true;
                    if(close_cb) {
                        close_cb(shared_from_this());
                    }
                }
                return ;
            }
        }
        if(!closed_ && input_buffer.readBytes() > 0) {
            if(message_cb) {
                message_cb(shared_from_this(), input_buffer);
            }
        }
    }

    void handleWrite() {
        while (output_buffer.readBytes() > 0) {
            ssize_t n;
            if (use_tls_ && tls_socket_ && tls_handshaked_) {
                n = tls_socket_->write(output_buffer.peek(), output_buffer.readBytes());
            } else {
                n = write(fd_, output_buffer.peek(), output_buffer.readBytes());
            }
            if (n > 0) {
                output_buffer.costBytes(n);
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                LOG_ERROR << "Write error: " << strerror(errno);
                handleClose();
                break;
            }
        }

        if (!closed_ && output_buffer.readBytes() == 0 && write_cb) {
            write_cb(shared_from_this());
        }
    }

    void handleClose() {
        if(!closed_) {
            closed_ = true;
            LOG_INFO << "Connection closed: fd " << fd_ << std::endl;
            if(close_cb) {
                close_cb(shared_from_this());
            }
        }
    }

    void send(const std::string& msg) {
        if(!closed_) {
            output_buffer.Append(msg.data(), msg.size());
            handleWrite();
        }
    }

    void send(const char* data, size_t len) {
        if(!closed_) {
            output_buffer.Append(data, len);
            handleWrite();
        }
    }

    void sendToBuffer(const std::string& msg) {
        if(!closed_) {
            output_buffer.Append(msg.data(), msg.size());
        }
    }
    
    void sendToBuffer(const char* data, size_t len) {
        if(!closed_) {
            output_buffer.Append(data, len);
        }
    }

    int fd() const {
        return fd_;
    }

    int id() const {
        return connection_id;
    }

    bool isClosed() {
        return closed_;
    }

    void uploadActiveTime() {
        last_active_time = std::chrono::steady_clock::now();
    }

    bool isTimeout(int timeout_seconds) {
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_active_time);

        return duration.count() > timeout_seconds;
    }

    void enableTLS(SSL_CTX* ctx) {
        if (!ctx) return;
        
        tls_socket_ = std::make_unique<TLSsocket>();
        if (tls_socket_->init(ctx, fd_)) {
            use_tls_ = true;
            tls_handshaked_ = false;
            LOG_INFO << "TLS enabled for connection " << connection_id;
        }
        else {
            LOG_ERROR << "Failed to enable TLS for connection " << connection_id;
            tls_socket_.reset();
        }
    }

private:
    int fd_;
    int connection_id;
    bool closed_;
    Buffer input_buffer;
    Buffer output_buffer;
    Callback write_cb;
    Callback close_cb;
    MessageCallback message_cb;
    void* context;

    std::unique_ptr<TLSsocket> tls_socket_;
    bool use_tls_ = false;
    bool tls_handshaked_ = false;
};