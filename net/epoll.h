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
#include "friend/OnlineManager.h"
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
    //std::chrono::steady_clock::time_point last_active_time;

    TcpConnection(int fd, int id = 0, EpollWrapper* epoll = nullptr) : fd_(fd), connection_id(id), closed_(false), context(nullptr) {
        //last_active_time = std::chrono::steady_clock::now();
        LOG_DEBUG << "Connection created: fd" << fd << std::endl;
    }

    ~TcpConnection() {
        if(fd_ > 0) {
            close(fd_);
            LOG_DEBUG << "Connection closed: fd" << fd_ << std::endl;
        }
    }

    // void updateActivityTime() {
    //     last_active_time = std::chrono::steady_clock::now();
    // }

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

    // 由 SubReactor 注入，用于在输出缓冲有/无数据时注册/注销 EPOLLOUT
    void setEpollUpdateCallback(std::function<void(bool)> cb) {
        epoll_update_cb_ = cb;
    }

    // 返回当前尚未写出到 socket 的字节数（用于背压判断）
    size_t outputBufferSize() const {
        return output_buffer.readBytes();
    }

    void setCloseCallBack(Callback cb) {
        close_cb = cb;
    }

    // 用户上下文清理回调：连接关闭时通知上层移除 user_id 映射
    void setUserCloseCallback(std::function<void(uint64_t)> cb) {
        user_close_cb_ = cb;
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

    void setThreadPool(ThreadPool* pool) {
        thread_pool_ = pool;
    }

    void handleRead() {
        //last_active_time = std::chrono::steady_clock::now();
        char buf[1024 * 64];
        while(true) {
            ssize_t n;
            if (use_tls_ && tls_socket_) {
                if (!tls_handshaked_) {
                    int ret = tls_socket_->accept();
                    if (ret == 0) {
                        tls_handshaked_ = true;
                        LOG_DEBUG << "TLS handshake completed";
                    } else if (ret == -2) {
                        break;
                    } else {
                        //last_active_time = std::chrono::steady_clock::now();
                        handleClose();
                        return;
                    }
                    continue;
                }
                n = tls_socket_->read(buf, sizeof(buf));
            } else {
                n = read(fd_, buf, sizeof(buf));
            }

            if (n > 0) {
                input_buffer.Append(buf, n);
                //last_active_time = std::chrono::steady_clock::now();
            } else if (n == 0) {
                LOG_INFO << "Connection closed by peer, fd=" << fd_;
                //last_active_time = std::chrono::steady_clock::now();
                handleClose();
                return;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                //last_active_time = std::chrono::steady_clock::now();
                break;
            } else {
                //last_active_time = std::chrono::steady_clock::now();
                LOG_ERROR << "Read error: " << strerror(errno);
                handleClose();
                return;
            }
        }
        
        //    从而保证登录/认证消息不被业务积压饿死。
        if(!closed_ && input_buffer.readBytes() > 0) {
            if (message_cb) {
                message_cb(shared_from_this(), input_buffer);
            }
        }
    }

    void handleWrite() {
        //last_active_time = std::chrono::steady_clock::now();

        bool need_callback = false;
        bool do_close = false;
        
        const int MAX_WRITE_ATTEMPTS = 20;
        const size_t MAX_WRITE_BYTES = 512 * 1024;
        int write_attempts = 0;
        size_t total_written = 0;
        
        {
            std::lock_guard<std::mutex> lock(output_mutex_);

            while (output_buffer.readBytes() > 0 && 
                write_attempts < MAX_WRITE_ATTEMPTS && 
                total_written < MAX_WRITE_BYTES) {
                ssize_t n;
                if (use_tls_ && tls_socket_ && tls_handshaked_) {
                    n = tls_socket_->write(output_buffer.peek(), output_buffer.readBytes());
                } else {
                    n = write(fd_, output_buffer.peek(), output_buffer.readBytes());
                }
                if (n > 0) {
                    output_buffer.costBytes(n);
                    total_written += n;
                    write_attempts++;
                    //last_active_time = std::chrono::steady_clock::now();
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    //last_active_time = std::chrono::steady_clock::now();
                    break;
                } else {
                    LOG_ERROR << "Write error: " << strerror(errno);
                    do_close = true;
                    break;
                }
            }

            if (!do_close && !closed_) {
                if (output_buffer.readBytes() == 0) {
                    setEpollWrite(false);
                    need_callback = true;
                } else {
                    setEpollWrite(true);
                }
            }
        }

        if (do_close) {
            handleClose();
            return;
        }

        if (need_callback && write_cb) {
            write_cb(shared_from_this());
        }
    }

    void handleClose() {
        if(closed_) {
            return ;
        }

        closed_ = true;
        LOG_INFO << "Closing connection fd = " << fd_;

        // 先调用 close_cb（此时 fd 仍有效），再关闭 fd，否则 close_cb 拿到的是 -1，无法清理映射
        if(context) {
            uint64_t user_id = reinterpret_cast<uint64_t>(context);
            
            LOG_INFO << "Cleaning up user " << user_id << " from connection";
            
            // 从 OnlineManager 移除
            OnlineManager::getInstance().removeUser(user_id);
            
            // 更新数据库为离线
            UserDAO dao;
            dao.updateUserStatus(user_id, 0);
            
            // 清理连接映射
            if(close_cb) {
                close_cb(shared_from_this());
            }
            
            // 通知上层清理 user_id 映射
            if(user_close_cb_) {
                user_close_cb_(user_id);
            }
            
            context = nullptr;
        }

        if(fd_ > 0) {
            close(fd_);
            fd_ = -1;
            LOG_DEBUG << "Success closed fd " << fd_;
        }

        if(tls_socket_) {
            tls_socket_.reset();
        }
    }

    void send(const std::string& msg) {
        send(msg.data(), msg.size());
    }

    void send(const char* data, size_t len) {
        if(closed_) return;
        
        //last_active_time = std::chrono::steady_clock::now();
        
        bool was_empty = false;
        {
            std::lock_guard<std::mutex> lock(output_mutex_);
            was_empty = (output_buffer.readBytes() == 0);
            
            // 16MB 上限保护
            if (output_buffer.readBytes() + len > 16 * 1024 * 1024) {
                LOG_WARN << "Buffer overflow, fd=" << fd_;
                return;
            }
            
            output_buffer.Append(data, len);
        }
        
        // 只有缓冲区之前为空时才立即发送
        if (was_empty) {
            handleWrite();
        } else if (!write_enabled_) {
            setEpollWrite(true);
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
       // last_active_time = std::chrono::steady_clock::now();
    }

    // bool isTimeout(int timeout_seconds) {
    //     auto now = std::chrono::steady_clock::now();
    //     auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_active_time);

    //     return duration.count() > timeout_seconds;
    // }

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
    std::function<void(uint64_t)> user_close_cb_;
    void* context;
    ThreadPool* thread_pool_ = nullptr;

    std::unique_ptr<TLSsocket> tls_socket_;
    bool use_tls_ = false;
    bool tls_handshaked_ = false;

    std::function<void(bool)> epoll_update_cb_;
    bool write_enabled_ = false;

    std::mutex output_mutex_;

    void setEpollWrite(bool enable) {
        if (enable == write_enabled_) {
            return;
        }
        write_enabled_ = enable;
        if (epoll_update_cb_) {
            epoll_update_cb_(enable);
        }
    }
};