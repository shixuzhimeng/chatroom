#pragma once

#include "epoll.h"
#include "tool/logging.h"
#include <vector>
#include <atomic>
#include <thread>
#include <memory>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include "tool/tool.h"
#include "thread_pool.h"
#include "JSON/Config.h"

class Reactor {
public:
    using ConnectionPtr = std::shared_ptr<TcpConnection>;
    virtual ~Reactor() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void addConnection(int fd) = 0;
    virtual void removeConnection(int fd) = 0;
};

class SubReactor : public Reactor, public std::enable_shared_from_this<SubReactor> {
public:
    SubReactor(int id, ThreadPool* thread_pool = nullptr) 
        : id_(id), running_(false), thread_pool_(thread_pool),
          epoll_fd_(-1), wakeup_fd_(-1) {
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            LOG_FATAL << "SubReactor " << id_ << " epoll_create failed";
        }
        // 创建 eventfd 用于唤醒
        wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wakeup_fd_ < 0) {
            LOG_FATAL << "SubReactor " << id_ << " eventfd failed";
        }
        epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = wakeup_fd_;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev) < 0) {
            LOG_FATAL << "SubReactor " << id_ << " add wakeup fd failed";
        }
        LOG_DEBUG << "SubReactor " << id_ << " created";
    }
    
    ~SubReactor() {
        stop();
        if (epoll_fd_ > 0) close(epoll_fd_);
        if (wakeup_fd_ > 0) close(wakeup_fd_);
    }
    
    void start() override {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return;
        }
        if (thread_pool_) {
            auto self = shared_from_this();
            thread_pool_->enqueue([self]() { self->eventLoop(); });
        } else {
            eventLoop();
        }
        LOG_INFO << "SubReactor " << id_ << " started";
    }
    
    void stop() override {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return;
        }
        wakeup();  // 唤醒 epoll_wait
        LOG_INFO << "SubReactor " << id_ << " stopping";
    }
    
    void addConnection(int fd) override {
        LOG_INFO << "subadd connection";
        


        if(fd < 0) {
            LOG_ERROR << "Invalid fd: " << fd;
            return ;
        }
        
        auto conn = std::make_shared<TcpConnection>(fd);
        //conn->updateActivityTime();
        conn->setThreadPool(thread_pool_);

        // 使用 weak_ptr 避免循环引用
        std::weak_ptr<SubReactor> weak_self = shared_from_this();
        conn->setMessageCallback([weak_self](ConnectionPtr c, Buffer& buf) {
            auto self = weak_self.lock();
            if (self && self->message_cb_) {
                self->message_cb_(c, buf);
            }
        });
        conn->setCloseCallBack([weak_self](ConnectionPtr c) {
            auto self = weak_self.lock();
            if (self) {
                self->removeConnection(c->fd());
            }
        });

        // 输出缓冲有/无数据时动态注册/注销 EPOLLOUT
        conn->setEpollUpdateCallback([weak_self, fd](bool add_write) {
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            epoll_event ev;
            ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
            if (add_write) {
                ev.events |= EPOLLOUT;
            }
            ev.data.fd = fd;
            epoll_ctl(self->epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
        });
        
        // 设置非阻塞
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        
        epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        ev.data.fd = fd;
        
        std::lock_guard<std::mutex> lock(conn_mutex_);
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            LOG_ERROR << "SubReactor " << id_ << " add connection failed: " 
                     << strerror(errno);
            return;
        }
        connections_[fd] = conn;

        if(connection_created_cb_) {
            connection_created_cb_(fd, conn);
        }
        LOG_DEBUG << "SubReactor " << id_ << " add connection " << fd;
    }
    
    void removeConnection(int fd) override {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        connections_.erase(fd);
        LOG_DEBUG << "SubReactor " << id_ << " remove connection " << fd;
    }
    
    void setConnectionCreatedCallback(std::function<void(int, ConnectionPtr)> cb) {
        connection_created_cb_ = cb;
    }

    void setMessageCallback(typename TcpConnection::MessageCallback cb) {
        message_cb_ = cb;
    }
    
    int id() const { return id_; }
    size_t connectionCount() const {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        return connections_.size();
    }
    
    void wakeup() {
        if (wakeup_fd_ > 0) {
            uint64_t one = 1;
            ssize_t n = write(wakeup_fd_, &one, sizeof(one));
            if (n < 0 && errno != EAGAIN) {
                LOG_ERROR << "wakeup write failed: " << strerror(errno);
            }
        }
    }
    
private:
    void eventLoop() {
        std::vector<epoll_event> events(1024);
        LOG_INFO << "SubReactor " << id_ << " event loop started";
        
        while (running_) {
            // 等待事件
            int n = epoll_wait(epoll_fd_, events.data(), events.size(), 100);
            if (n < 0) {
                if (errno == EINTR) continue;
                LOG_ERROR << "SubReactor " << id_ << " epoll_wait failed: " 
                         << strerror(errno);
                break;
            }
            
            //LOG_INFO << "subReactor " << id_ << " wait"; 

            // 就绪事件处理
            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;
                if (fd == wakeup_fd_) {
                    // 消耗唤醒事件
                    uint64_t dummy;
                    read(wakeup_fd_, &dummy, sizeof(dummy));
                    LOG_INFO << "subreactor " << id_ << "wake up";
                    continue;
                }
                
                ConnectionPtr conn;
                {
                    std::lock_guard<std::mutex> lock(conn_mutex_);
                    auto it = connections_.find(fd);
                    if (it != connections_.end()) {
                        conn = it->second;
                    }
                }
                if (!conn || conn->isClosed()) {
                    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
                    std::lock_guard<std::mutex> lock(conn_mutex_);
                    connections_.erase(fd);
                    LOG_DEBUG << "SubReactor " << id_ << " cleaned up closed connection " << fd;
                    continue;
                }
                
                if (events[i].events & (EPOLLIN | EPOLLRDHUP)) {
                    conn->handleRead();
                    if(conn->isClosed()) {
                        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
                        std::lock_guard<std::mutex> lock(conn_mutex_);
                        connections_.erase(fd);
                        LOG_DEBUG << "SubReactor " << id_ << " remove closed connect " << fd << "after read";
                    }
                }
                if (!conn->isClosed() && (events[i].events & EPOLLOUT)) {
                    conn->handleWrite();
                }
                if (!conn->isClosed() && (events[i].events & (EPOLLERR | EPOLLHUP))) {
                    conn->handleClose();
                }
            }
            // 超时检测
            // int64_t now = tool::getTimestamp();
            // if(now - last_check_time > 10000) {
            //     last_check_time = now;
            //     int timeout_check = Config::getInstance().getInt("server.timeout", 60);
            //     std::vector<ConnectionPtr> timed_out;
            //     {
            //         std::lock_guard<std::mutex> lock(conn_mutex_);
            //         for(auto it = connections_.begin(); it != connections_.end(); ) {
            //             auto& conn = it->second;
            //             if(conn && conn->isTimeout(timeout_check)) {
            //                 LOG_INFO << "Connection timeout: fd = " << conn->fd();
            //                 timed_out.push_back(conn);
            //                 it = connections_.erase(it);
            //             }
            //             else {
            //                 ++it;
            //             }
            //         }
            //     }
                // 锁外关闭，避免handleClose -> close_cb -> removeConnection重入死锁
                // for(auto& conn : timed_out) {
                //     conn->handleClose();
                // }
            // }
        }
        
        // 清理所有连接
        {
            std::vector<ConnectionPtr> to_close;
            {
                std::lock_guard<std::mutex> lock(conn_mutex_);
                for (auto& pair : connections_) {
                    to_close.push_back(pair.second);
                }
                connections_.clear();
            }
            // 锁外关闭，避免handleClose -> close_cb -> removeConnection重入死锁
            for (auto& conn : to_close) {
                conn->handleClose();
            }
        }
        LOG_INFO << "SubReactor " << id_ << " event loop stopped";
    }
    
    int id_;
    int epoll_fd_;
    int wakeup_fd_;
    std::atomic<bool> running_;
    ThreadPool* thread_pool_;
    mutable std::mutex conn_mutex_;
    std::unordered_map<int, ConnectionPtr> connections_;
    typename TcpConnection::MessageCallback message_cb_;
    std::function<void(int, ConnectionPtr)> connection_created_cb_;
    int64_t last_check_time = 0;

};

class MainReactor : public Reactor, public std::enable_shared_from_this<MainReactor> {
public:
    MainReactor(const std::string& host, uint16_t port, int sub_count = 4,
                ThreadPool* thread_pool = nullptr)
        : host_(host), port_(port), sub_count_(sub_count),
          running_(false), next_sub_(0), thread_pool_(thread_pool),
          listen_fd_(-1), epoll_fd_(-1), wakeup_fd_(-1) {
        
        listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (listen_fd_ < 0) {
            LOG_FATAL << "MainReactor socket creation failed: " << strerror(errno);
        }
        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        if (host_ == "0.0.0.0") {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else {
            inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);
        }
        if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            LOG_FATAL << "MainReactor bind failed: " << strerror(errno);
        }
        if (listen(listen_fd_, SOMAXCONN) < 0) {
            LOG_FATAL << "MainReactor listen failed: " << strerror(errno);
        }
        
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            LOG_FATAL << "MainReactor epoll_create failed: " << strerror(errno);
        }
        // 唤醒 fd
        wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wakeup_fd_ < 0) {
            LOG_FATAL << "MainReactor eventfd failed";
        }
        epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = wakeup_fd_;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev);
        ev.data.fd = listen_fd_;
        ev.events = EPOLLIN | EPOLLET;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev);
        
        // 创建 SubReactor
        for (int i = 0; i < sub_count_; ++i) {
            auto sub = std::make_shared<SubReactor>(i, thread_pool_);
            sub_reactors_.push_back(sub);
        }
        
        LOG_INFO << "MainReactor created on " << host_ << ":" << port_ 
                 << " with " << sub_count_ << " sub-reactors";
    }
    
    ~MainReactor() {
        stop();
        if (listen_fd_ > 0) close(listen_fd_);
        if (epoll_fd_ > 0) close(epoll_fd_);
        if (wakeup_fd_ > 0) close(wakeup_fd_);
    }
    
    void start() override {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return;
        }
        for (auto& sub : sub_reactors_) {
            sub->start();
        }
        if (thread_pool_) {
            auto self = shared_from_this();
            thread_pool_->enqueue([self]() { self->EventLoop(); });
        } else {
            EventLoop();
        }
        LOG_INFO << "MainReactor started";
    }
    
    void stop() override {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return;
        }
        wakeup();
        for (auto& sub : sub_reactors_) {
            sub->stop();
        }
        LOG_INFO << "MainReactor stopped";
    }
    
    void addConnection(int fd) override {
        LOG_INFO << "mainaddconnection";
        if (sub_count_ <= 0) {
            LOG_ERROR << "No sub-reactors available";
            close(fd);
            return;
        }
        int idx = next_sub_.fetch_add(1) % sub_count_;
        if (idx < static_cast<int>(sub_reactors_.size())) {
            sub_reactors_[idx]->addConnection(fd);
        }
        else {
            close(fd);
        }
    }
    
    void removeConnection(int fd) override {};
    
    void setMessageCallback(typename TcpConnection::MessageCallback cb) {
        for (auto& sub : sub_reactors_) {
            sub->setMessageCallback(cb);
        }
    }
    
    void wakeup() {
        if (wakeup_fd_ > 0) {
            uint64_t one = 1;
            write(wakeup_fd_, &one, sizeof(one));
        }
    }
    
    std::vector<size_t> getSubReactorStats() const {
        std::vector<size_t> stats;
        for (const auto& sub : sub_reactors_) {
            stats.push_back(sub->connectionCount());
        }
        return stats;
    }
    
    void setConnectionHandler(std::function<void(int)> handler) {
        connection_handler_ = handler;
    }
    
    void addConnectionToReactor(int fd) {
        LOG_INFO << "[MainReactor] addConnectionToReactor fd=" << fd;
        if (sub_count_ <= 0) {
            LOG_ERROR << "No sub-reactors available";
            close(fd);
            return;
        }
        int idx = next_sub_.fetch_add(1) % sub_count_;
        if (idx < static_cast<int>(sub_reactors_.size())) {
            sub_reactors_[idx]->addConnection(fd);
        } else {
            close(fd);
        }
    }

    std::vector<std::shared_ptr<SubReactor>>& getSubReactors() {
        return sub_reactors_;
    }

private:
    void EventLoop() {
        std::vector<epoll_event> events(1024);
        LOG_INFO << "MainReactor event loop started";
        
        while (running_) {
            int n = epoll_wait(epoll_fd_, events.data(), events.size(), 100);
            if (n < 0) {
                if (errno == EINTR) continue;
                LOG_ERROR << "MainReactor epoll_wait failed: " << strerror(errno);
                break;
            }
            
            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;
                if (fd == wakeup_fd_) {
                    uint64_t dummy;
                    read(wakeup_fd_, &dummy, sizeof(dummy));
                    continue;
                }
                if (fd == listen_fd_) {
                    while (true) {
                        sockaddr_in client_addr;
                        socklen_t len = sizeof(client_addr);
                        int client_fd = accept4(listen_fd_, (sockaddr*)&client_addr,
                                               &len, SOCK_NONBLOCK);
                        if (client_fd < 0) {
                            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                LOG_ERROR << "accept failed: " << strerror(errno);
                            }
                            break;
                        }
                        LOG_INFO << "New connection";
                        if(connection_handler_) {
                            connection_handler_(client_fd);
                        }
                        else {
                            addConnection(client_fd);
                        }
                        char client_ip[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
                        LOG_DEBUG << "New connection from " << client_ip 
                                  << ":" << ntohs(client_addr.sin_port);
                    }
                }
            }
        }
        LOG_INFO << "MainReactor event loop stopped";
    }
    
    std::string host_;
    uint16_t port_;
    int listen_fd_;
    int epoll_fd_;
    int wakeup_fd_;
    int sub_count_;
    std::atomic<bool> running_;
    std::atomic<int> next_sub_;
    ThreadPool* thread_pool_;
    std::vector<std::shared_ptr<SubReactor>> sub_reactors_;
    std::function<void(int)> connection_handler_;
};