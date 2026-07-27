#pragma once
#include "epoll.h"
#include "logging.h"
#include <vector>
#include <atomic>
#include <thread>
#include <memory>
#include <unordered_map>
#include <functional>

class Reactor {
public:
    using ConnectionPtr = std::shared_ptr<TcpConnection>;
    virtual ~Reactor() = default;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void addConnection(int fd) = 0;
    virtual void removeConnection(int fd) = 0;
};

class SubReactor : public Reactor {
public:
    SubReactor(int id, ThreadPool* thread_pool = nullptr) 
        : id_(id), running_(false), thread_pool_(thread_pool) {
        epoll_fd_ = epoll_create1(0);
        if (epoll_fd_ < 0) {
            LOG_FATAL << "SubReactor " << id_ << " epoll_create failed";
        }
        LOG_DEBUG << "SubReactor " << id_ << " created";
    }
    
    ~SubReactor() {
        stop();
        if (epoll_fd_ > 0) close(epoll_fd_);
    }
    
    void start() override {
        if (running_) return;
        running_ = true;
        
        if (thread_pool_) {
            thread_pool_->enqueue([this]() { eventLoop(); });
        } else {
            eventLoop();
        }
        LOG_INFO << "SubReactor " << id_ << " started";
    }
    
    void stop() override {
        running_ = false;
        LOG_INFO << "SubReactor " << id_ << " stopping";
    }
    
    void addConnection(int fd) override {
        auto conn = std::make_shared<TcpConnection>(fd, fd);
        conn->setMessageCallback([this](ConnectionPtr c, Buffer& buf) {
            if (message_cb_) message_cb_(c, buf);
        });
        conn->setCloseCallBack([this](ConnectionPtr c) {
            removeConnection(c->fd());
        });
        
        epoll_event ev;
        ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        ev.data.ptr = conn.get();
        
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            LOG_ERROR << "SubReactor " << id_ << " add connection failed: " 
                     << strerror(errno);
            return;
        }
        
        connections_[fd] = conn;
        LOG_DEBUG << "SubReactor " << id_ << " add connection " << fd;
    }
    
    void removeConnection(int fd) override {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        connections_.erase(fd);
        LOG_DEBUG << "SubReactor " << id_ << " remove connection " << fd;
    }
    
    void setMessageCallback(typename TcpConnection::MessageCallback cb) {
        message_cb_ = cb;
    }
    
    int id() const { return id_; }
    size_t connectionCount() const { return connections_.size(); }
    
private:
    void eventLoop() {
        std::vector<epoll_event> events(1024);
        
        while (running_) {
            int n = epoll_wait(epoll_fd_, events.data(), events.size(), 100);
            
            if (n < 0) {
                if (errno == EINTR) continue;
                LOG_ERROR << "SubReactor " << id_ << " epoll_wait failed: " 
                         << strerror(errno);
                break;
            }
            
            for (int i = 0; i < n; ++i) {
                auto* conn = static_cast<TcpConnection*>(events[i].data.ptr);
                if (!conn || conn->isClosed()) continue;
                
                if (events[i].events & (EPOLLIN | EPOLLRDHUP)) {
                    conn->handleRead();
                }
                if (events[i].events & EPOLLOUT) {
                    conn->handleWrite();
                }
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    conn->handleClose();
                }
            }
        }
        
        for (auto& pair : connections_) {
            pair.second->handleClose();
        }
        
        connections_.clear();
        LOG_INFO << "SubReactor " << id_ << " event loop stopped";
    }
    
    int id_;
    int epoll_fd_;
    std::atomic<bool> running_;
    ThreadPool* thread_pool_;
    std::unordered_map<int, ConnectionPtr> connections_;
    typename TcpConnection::MessageCallback message_cb_;
};

class MainReactor : public Reactor {
public:
    MainReactor(const std::string& host, uint16_t port, int sub_count = 4,
                ThreadPool* thread_pool = nullptr)
        : host_(host), port_(port), sub_count_(sub_count),
          running_(false), next_sub_(0), thread_pool_(thread_pool) {
        
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
        
        epoll_fd_ = epoll_create1(0);
        if (epoll_fd_ < 0) {
            LOG_FATAL << "MainReactor epoll_create failed: " << strerror(errno);
        }
        
        epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = listen_fd_;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev);
        
        for (int i = 0; i < sub_count_; ++i) {
            auto sub = std::make_unique<SubReactor>(i, thread_pool_);
            sub_reactors_.push_back(std::move(sub));
        }
        
        LOG_INFO << "MainReactor created on " << host_ << ":" << port_ 
                 << " with " << sub_count_ << " sub-reactors";
    }
    
    ~MainReactor() {
        stop();
        if (listen_fd_ > 0) close(listen_fd_);
        if (epoll_fd_ > 0) close(epoll_fd_);
    }
    
    void start() override {
        if (running_) return;
        running_ = true;
        
        for (auto& sub : sub_reactors_) {
            sub->start();
        }
        
        if (thread_pool_) {
            thread_pool_->enqueue([this]() { EventLoop(); });
        } else {
            EventLoop();
        }
        
        LOG_INFO << "MainReactor started";
    }
    
    void stop() override {
        running_ = false;
        for (auto& sub : sub_reactors_) {
            sub->stop();
        }
        LOG_INFO << "MainReactor stopped";
    }
    
    void addConnection(int fd) override {
        int idx = next_sub_++ % sub_count_;
        sub_reactors_[idx]->addConnection(fd);
    }
    
    void removeConnection(int fd) override {}
    
    void setMessageCallback(typename TcpConnection::MessageCallback cb) {
        for (auto& sub : sub_reactors_) {
            sub->setMessageCallback(cb);
        }
    }
    
    std::vector<size_t> getSubReactorStats() const {
        std::vector<size_t> stats;
        for (const auto& sub : sub_reactors_) {
            stats.push_back(sub->connectionCount());
        }
        return stats;
    }
    
private:
    void EventLoop() {
        std::vector<epoll_event> events(1024);
        
        while (running_) {
            int n = epoll_wait(epoll_fd_, events.data(), events.size(), 100);
            
            if (n < 0) {
                if (errno == EINTR) continue;
                LOG_ERROR << "MainReactor epoll_wait failed: " << strerror(errno);
                break;
            }
            
            for (int i = 0; i < n; ++i) {
                if (events[i].data.fd == listen_fd_) {
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
                        
                        char client_ip[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
                        LOG_DEBUG << "New connection from " << client_ip 
                                  << ":" << ntohs(client_addr.sin_port);
                        
                        addConnection(client_fd);
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
    int sub_count_;
    std::atomic<bool> running_;
    std::atomic<int> next_sub_;
    ThreadPool* thread_pool_;
    std::vector<std::unique_ptr<SubReactor>> sub_reactors_;
};