#pragma once

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <functional>
#include <vector>
#include <atomic>
#include <mutex>
#include <memory>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "../logging.h"
#include <sys/epoll.h>

class TLSClient {
public:
    using MessageCallback = std::function<void(const std::vector<char>&)>;

    TLSClient() : fd_(-1), connected_(false), 
                            ssl_ctx_(nullptr), ssl_(nullptr),
                            use_tls_(false), tls_handshaked_(false) {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
    }

    ~TLSClient() {
        disconnect();
        cleanupTLS();
    }

    bool connect(const std::string& host, uint16_t port, bool use_tls, const std::string& cert_file, const std::string& key_file) {
        use_tls_ = use_tls;
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if(fd_ < 0) {
            LOG_ERROR << "socket failed: " << strerror(errno);
            return false;
        }

        int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if(inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
            LOG_ERROR << "Invalid IP";
            close(fd_);
            fd_ = -1;
            return false;
        }

        int ret = ::connect(fd_, (sockaddr*)&addr, sizeof(addr));
        if(ret < 0 && errno != EINPROGRESS) {
            LOG_ERROR << "connect failed: " << strerror(errno);
            close(fd_);
            fd_ = -1;
            return false;
        }

        if(ret == 0) {
            fcntl(fd_, F_SETFL, flags);
            LOG_INFO << "connected success";
            goto connected;
        }

        {
            int epfd = epoll_create1(0);
            if(epfd < 0) {
                LOG_ERROR << "epoll_create1 failed";
                close(fd_);
                fd_ = -1;
                return false;
            }
            struct epoll_event ev;
            ev.events = EPOLLOUT;
            ev.data.fd = fd_;
            if(epoll_ctl(epfd, EPOLL_CTL_ADD, fd_, &ev) < 0) {
                LOG_ERROR << "epoll_ctl failed";
                close(fd_);
                fd_ = -1;
                close(epfd);
                return false;
            }
            struct epoll_event events[1];
            int nfds = epoll_wait(epfd, events, 1, 5000);
            close(epfd);
            if(nfds <= 0) {
                if (nfds == 0) LOG_ERROR << "connect timeout";
                else LOG_ERROR << "epoll_wait failed";
                close(fd_);
                fd_ = -1;
                return false;
            }
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &len);
            if(error != 0) {
                LOG_ERROR << "connect error: " << strerror(error);
                close(fd_);
                fd_ = -1;
                return false;
            }
            fcntl(fd_, F_SETFL, flags);
        }

    connected:
        if(use_tls_) {
            if(!setupTLS(cert_file, key_file)) {
                LOG_ERROR << "TLS setup failed";
                close(fd_);
                fd_ = -1;
                return false;
            }
            LOG_INFO << "TLS connection established";
        }

        connected_ = true;
        LOG_INFO << "Connected to " << host << ":" << port;
        return true;
    }

    void disconnect() {
        connected_ = false;
        if(ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
            tls_handshaked_ = false;
        }
        if(fd_ > 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    void setMessageCallback(MessageCallback cb) {
        message_cb_ = cb;
    }

    int getfd() const {
        return fd_;
    }

    bool send(const char* data, size_t len) {
        if(!connected_ || fd_ < 0) return false;
        std::lock_guard<std::mutex> lock(send_mutex_);
        size_t sent = 0;
        while (sent < len) {
            int ret = writeData(data + sent, len - sent);
            if(ret <= 0) {
                if(ret == 0 || (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    connected_ = false;
                    return false;
                }
                usleep(1000);
                continue;
            }
            sent += ret;
        }
        return true;
    }

    int readData(char* buf, int len) {
        if(use_tls_ && ssl_ && tls_handshaked_) {
            return SSL_read(ssl_, buf, len);
        }
        else{
            return read(fd_, buf, len);
        }
    }

    int writeData(const char* buf, int len) {
        if(use_tls_ && ssl_ && tls_handshaked_) {
            return SSL_write(ssl_, buf, len);
        }
        else{
            return write(fd_, buf, len);
        }
    }

    bool setNonBlocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1) return false;
        return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
    }

    bool isConnected() const {
        return connected_;
    }

    bool isTLS() const {
        return use_tls_ && tls_handshaked_;
    }

    void handleRead() {
        if(!connected_ || fd_ < 0) {
            return ;
        }
        char temp[64 * 1024];
        int n = readData(temp, sizeof(temp));

        if(n > 0) {
            recv_buffer_.insert(recv_buffer_.end(), temp, temp + n);
            processMessage();
        }
        else if(n == 0) {
            LOG_INFO << "Server closed connected";
            disconnect();
            if(close_cb_) {
                close_cb_();
            }
        }
        else {
            if(errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR << "read error: " << strerror(errno);
                disconnect();
                if(close_cb_) {
                    close_cb_();
                }
            }
        }
    }
private:
    int fd_;
    std::atomic<bool> connected_;
    MessageCallback message_cb_;
    std::vector<char> recv_buffer_;
    std::mutex send_mutex_;
    std::function<void()> close_cb_;

    SSL_CTX* ssl_ctx_;
    SSL* ssl_;
    bool use_tls_;
    bool tls_handshaked_;

    void processMessage() {
        while(recv_buffer_.size() >= sizeof(uint32_t)) {
            // 读取 total_len
            uint32_t total_len;
            memcpy(&total_len, recv_buffer_.data(), sizeof(uint32_t));
            total_len = ntohl(total_len);

            if(total_len < 4 || total_len > 10 * 1024 * 1024) {
                LOG_ERROR << "Invalid message length: " << total_len;
                recv_buffer_.clear();
                disconnect();
                return;
            }

            // 完整数据包大小 = 4(total_len) + 4(header_len) + total_len
            // 需要至少 8 字节来读取 header_len
            if (recv_buffer_.size() < sizeof(uint32_t) * 2) {
                break;
            }
            
            // 读取 header_len
            uint32_t header_len;
            memcpy(&header_len, recv_buffer_.data() + sizeof(uint32_t), sizeof(uint32_t));
            header_len = ntohl(header_len);
            
            // 验证 header_len 是否合理
            if (header_len < 4 || header_len > 1024) {
                LOG_ERROR << "Invalid header_len: " << header_len;
                recv_buffer_.clear();
                disconnect();
                return;
            }
            
            // 【关键修改】完整数据包大小
            size_t packet_size = sizeof(uint32_t) * 2 + total_len;
            
            if(recv_buffer_.size() >= packet_size) {
                std::vector<char> msg(recv_buffer_.begin(), 
                                    recv_buffer_.begin() + packet_size);
                if(message_cb_) {
                    message_cb_(msg);
                }
                recv_buffer_.erase(recv_buffer_.begin(), 
                                recv_buffer_.begin() + packet_size);
                LOG_DEBUG << "Processed packet, remaining buffer size: " << recv_buffer_.size();
            }
            else {
                break;
            }
        }
    }

    bool setupTLS(const std::string& cert_file, const std::string& key_file) {
        if (ssl_ctx_) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = nullptr;
        }
        const SSL_METHOD* method = TLS_client_method();
        ssl_ctx_ = SSL_CTX_new(method);
        if (!ssl_ctx_) {
            LOG_ERROR << "SSL_CTX_new failed";
            return false;
        }
        // if (!cert_file.empty() && !key_file.empty()) {
        //     if (SSL_CTX_use_certificate_file(ssl_ctx_, cert_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
        //         LOG_ERROR << "SSL_CTX_use_certificate_file failed";
        //         return false;
        //     }
        //     if (SSL_CTX_use_PrivateKey_file(ssl_ctx_, key_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
        //         LOG_ERROR << "SSL_CTX_use_PrivateKey_file failed";
        //         return false;
        //     }
        //     if (!SSL_CTX_check_private_key(ssl_ctx_)) {
        //         LOG_ERROR << "Private key does not match certificate";
        //         return false;
        //     }
        // }
        SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);

        ssl_ = SSL_new(ssl_ctx_);
        if (!ssl_) {
            LOG_ERROR << "SSL_new failed";
            return false;
        }
        SSL_set_fd(ssl_, fd_);

        int ret = SSL_connect(ssl_);
        if (ret != 1) {
            int err = SSL_get_error(ssl_, ret);
            LOG_ERROR << "SSL_connect failed: " << err;
            return false;
        }
        tls_handshaked_ = true;
        return true;
    }

    void cleanupTLS() {
        if (ssl_ctx_) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = nullptr;
        }
    }
};