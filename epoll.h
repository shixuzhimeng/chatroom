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
#include "reactor.h"
#include "thread_pool.h"

class NBSocket {
public:
    NBSocket() : fd_(-1) {}
    explicit NBSocket(int fd) : fd_(fd) {}
    ~NBSocket() {
        if(fd_ > 0) {
            close(fd_);
        }
    }

    bool Socket(uint16_t port, const std::string& ip = "0.0.0.0") {
        fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if(fd_ < 0) {
            LOG_ERROR << "socket failed" << std::endl;
            return false;
        }

        int opt = 1;
        if(setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            LOG_INFO << "setsockopt failed";
        }

        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if(inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
            LOG_ERROR << "Invalid IP address:" << ip;
            return false;
        }

        if(bind(fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            LOG_ERROR << "bind failed" << std::endl;
            return false;
        }

        if(listen(fd_, 128) < 0) {
            LOG_ERROR << "listen faield" << std::endl;
            return false;
        }
        
        LOG_INFO << "Seerver listening on" << ip << ":" << port << std::endl;

        return true;

    }
    int Accept() {
        sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        
        int client_fd = accept(fd_, (sockaddr*)&client_addr, &len);
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        if(client_fd < 0) {
            if(errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR << "accept failed " << strerror(errno); 
            }
            return -1;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        LOG_DEBUG << "Accepted connection from " << client_ip << ":" << ntohs(client_addr.sin_port);

        return client_fd;
    }

    int fd() const{
        return fd_;
    }


private:
    int fd_;
};


class EpollWrapper {
public:
    EpollWrapper() {
        epfd_ = epoll_create1(0);
        if(epfd_ < 0) {
            LOG_FATAL << "epoll_creat failed: " << strerror(errno);
        }
    }
    ~EpollWrapper() {
        if(epfd_ < 0) {
            close(epfd_);
        }
    }

    bool add(int fd, uint32_t events, void* ptr = nullptr) {
        epoll_event ev;
        ev.events = events;
        ev.data.ptr = ptr;

        if(epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev)) {
            LOG_ERROR << "epoll_ctl_add failed for fd :" << fd << ":" << strerror(errno);
            return false;
        }

        return true;
    }

    bool mod(int fd, uint32_t events, void* ptr = nullptr) {
        epoll_event ev;
        ev.events = events;
        ev.data.ptr = ptr;

        if(epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0) {
            LOG_ERROR << "epoll_ctl_mod failed for fd" << fd << ":" << strerror(errno);
            return false;
        }

        return true;
    }

    bool del(int fd) {
        if(epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) == 0) {
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

    TcpConnection(int fd, int id = 0) : fd_(fd), connection_id(id), closed_(false), context(nullptr) {
        output_buffer.Append("Hello from server", 17);
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
    
    void handleRead() {
        char buf[1024];
        while(true) {
            ssize_t n = read(fd_, buf, sizeof(buf));
            if(n > 0) {
                input_buffer.Append(buf, n);
            }
            else if(n == 0) {
                handleClose();
                break;
            }
            else if(errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            else {
                LOG_ERROR << "Read error" << strerror(errno);
                handleClose();
                break;
            }
        }
        if(!closed_ && input_buffer.readBytes() > 0) {
            if(message_cb) {
                message_cb(shared_from_this(), input_buffer);
            }
        }
    }

    void handleWrite() {
        while(output_buffer.readBytes() > 0) {
            ssize_t n = write(fd_, output_buffer.peek(), output_buffer.readBytes());
            if(n > 0) {
                output_buffer.costBytes(n);
            }
            else if(errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            else {
                LOG_ERROR << "Write error" << strerror(errno);
                handleClose();
                break;
            }
        }
        if(!closed_ && output_buffer.readBytes() == 0) {
            if(write_cb) {
                write_cb(shared_from_this());
            }
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
};