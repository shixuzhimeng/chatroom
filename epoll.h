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

class NBSocket {
public:
    NBSocket() : fd_(-1) {}
    explicit NBSocket(int fd) : fd_(fd) {}
    ~NBSocket() {
        if(fd_ > 0) {
            close(fd_);
        }
    }

    bool Socket(uint16_t port) {
        fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if(fd_ < 0) {
            return false;
        }

        int opt = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htonl(port);

        if(bind(fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            return false;
        }

        if(listen(fd_, 128) < 0) {
            return false;
        }
        
        return true;

    }
    int Accept() {
        sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        
        int client_fd = accept(fd_, (sockaddr*)&client_addr, &len);
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

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

        return epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == 0;
    }

    bool mod(int fd, uint32_t events, void* ptr = nullptr) {
        epoll_event ev;
        ev.events = events;
        ev.data.ptr = ptr;

        return epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0;
    }

    bool del(int fd) {
        return epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
    }

    int wait(std::vector<epoll_event>& events, int timeout = -1) {
        events.resize(1024);
        int n = epoll_wait(epfd_, events.data(), events.size(), timeout);
        
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


private:
    std::vector<char> buffer_;
    size_t read_index = 0;
};


class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
    using Callback = std::function<void(std::shared_ptr<TcpConnection>)>;

    TcpConnection(int fd) : fd_(fd), closed_(false) {
        output_buffer.Append("Hello from server", 19);
    }

    ~TcpConnection() {
        if(fd_ > 0) {
            close(fd_);
        }
    }

    Buffer& inputBuffer() {
        return input_buffer;
    }

    Buffer& outputBuffer() {
        return output_buffer;
    }

    void setReadCallBack(Callback cb) {
        read_cb = cb;
    }

    void setWriteCallBack(Callback cb) {
        write_cb = cb;
    }

    void setCloseCallBack(Callback cb) {
        close_cb = cb;
    }
    
    void handleRead() {
        char buf[1024];
        ssize_t n = read(fd_, buf, sizeof(buf));
        if(n > 0) {
            input_buffer.Append(buf, n);
        }
        else if(n == 0) {
            handleClose();
        }
    }

    void handleWrite() {
        size_t len = output_buffer.readBytes();
        if(len > 0) {
            ssize_t n = write(fd_, output_buffer.peek(), len);
            if(n > 0) {
                output_buffer.costBytes(n);
            }
        }
        if(output_buffer.readBytes() == 0 && write_cb) {
            write_cb(shared_from_this());
        }
    }

    void handleClose() {
        closed_ = true;
        if(close_cb) {
            close_cb(shared_from_this());
        }
    }

    void send(const std::string& msg) {
        output_buffer.Append(msg.data(), msg.size());
    }


    int fd() const {
        return fd_;
    }

private:
    int fd_;
    bool closed_;
    Buffer input_buffer;
    Buffer output_buffer;
    Callback read_cb;
    Callback write_cb;
    Callback close_cb;
};