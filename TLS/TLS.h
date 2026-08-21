#pragma once

#include <openssl/ssl.h>
#include <sys/socket.h>
#include <openssl/err.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <string>
#include <memory>
#include <vector>
#include "../logging.h"
#include <arpa/inet.h>

class TLSContext {
public:
    TLSContext() : ctx_(nullptr) {}

    //初始化
    bool init(const std::string& cert_file, const std::string& key_file) {
        // 初始化SSL库
        SSL_library_init();
        // 加载所有的加密算法
        OpenSSL_add_all_algorithms();
        // 加载错误的信息
        SSL_load_error_strings();

        // 创建SSL上下文
        const SSL_METHOD* method = TLS_server_method();
        ctx_ = SSL_CTX_new(method);

        // 证书和密钥
        // 加载证书
        if(SSL_CTX_use_certificate_file(ctx_, cert_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
            LOG_ERROR << "SSL_CTX_use_certificate_file failed";
            return false;
        }
        //加载私钥
        if(SSL_CTX_use_PrivateKey_file(ctx_, key_file.c_str(), SSL_FILETYPE_PEM) <= 0) {
            LOG_ERROR << "SSL_CTX_use_certificate_file failed";
            return false;
        }

        // 匹配证书和私钥
        if(!SSL_CTX_check_private_key(ctx_)) {
            LOG_ERROR << "Private key does not match certificate";
            return false;
        }

        return true;
    }

    SSL_CTX* get() const { return ctx_; }
    
    ~TLSContext() {
        if(ctx_) {
            SSL_CTX_free(ctx_);
        }
        EVP_cleanup();
    }

private:
    SSL_CTX* ctx_;   // OpenSSL 上下文句柄
};

// 在已建立的TCP上添加TLS加密
class TLSsocket {
public:
    TLSsocket() : ssl_(nullptr), fd_(-1) {}

    ~TLSsocket() {
        close();
    }
    
    bool init(SSL_CTX* ctx, int fd) {
        fd_ = fd;
        // 从上下文创建SSL对象
        ssl_ = SSL_new(ctx);
        // 绑定socket到SSL对象
        SSL_set_fd(ssl_, fd_);

        // 设置非阻塞模式
        int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        return true;
    }

    int accept() {
        // 执行TLS握手
        int ret = SSL_accept(ssl_);
        if(ret == 1) {
            return 0;
        }
        else {
            int err = SSL_get_error(ssl_, ret);
            if(err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                return -2;
            }
            else {
                LOG_ERROR << "SSL_accept failed: " << err;
                return -1;
            }
        }
    }

    // 解密读取数据
    int read(void* buf, int len) {
        return SSL_read(ssl_, buf, len);
    }

    // 加密发送数据
    int write(const void* buf, int len) {
        return SSL_write(ssl_, buf, len);
    }
    
    void close() {
        if(ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if(fd_ > 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd() const { return fd_; }

    SSL* ssl() const { return ssl_; }

private:
    SSL* ssl_;
    int fd_;
};

//TLS 监听
class TLSListener {
public:
    TLSListener() : listen_fd_(-1), ctx_(nullptr) {}

    bool init(uint64_t port, const std::string& ip = "0.0.0.0") {
        listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if(listen_fd_ < 0) {
            LOG_ERROR << "socket failed";
            return false;
        }

        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if(ip == "0.0.0.0") {
            addr.sin_addr.s_addr = INADDR_ANY;
        }
        else {
            inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        }
        
        if(bind(listen_fd_, (sockaddr*)&addr, sizeof(addr) < 0)) {
            LOG_ERROR << "bind failed";
            return false;
        }

        if(listen(listen_fd_, 128) < 0) {
            LOG_ERROR << "listen failed";
            return false;
        }

        LOG_INFO << "TLS listener started on " << ip << ":" << port;
        return true;
    }

    void setSSLContext(SSL_CTX* ctx) {
        ctx_ = ctx;
    }

    int accept() {
        sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);

        int client_fd = accept4(listen_fd_, (sockaddr*)&client_addr, &len, SOCK_NONBLOCK);
        
        if(client_fd < 0) {
            if(errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR << "accept failed";
            }
            return -1;
        }
        return client_fd;
    }

    int fd() const { return listen_fd_; }

    ~TLSListener() {
        if (listen_fd_ > 0) {
            ::close(listen_fd_);
        }
    }
private:
    int listen_fd_;
    SSL_CTX* ctx_;
};