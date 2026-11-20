// SPDX-License-Identifier: GPL-2.0-only
// TCP helpers, kept apart from stream.cc so that statically linked programs
// (which only use Unix sockets) do not pull in getaddrinfo.
#include "transport/stream.h"

#include <cstring>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>

namespace dnuma {

Status tcp_listen(const std::string& bind_addr, uint16_t port, Fd* out, uint16_t* bound_port) {
    Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd.valid()) return Status::sys("socket");
    int one = 1;
    setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (bind_addr.empty()) sa.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (inet_pton(AF_INET, bind_addr.c_str(), &sa.sin_addr) != 1)
        return Status::err(EINVAL, "bad bind address '%s'", bind_addr.c_str());
    if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0)
        return Status::sys("bind %s:%u", bind_addr.c_str(), port);
    if (::listen(fd.get(), 64) != 0) return Status::sys("listen");
    if (bound_port) {
        socklen_t len = sizeof sa;
        getsockname(fd.get(), reinterpret_cast<sockaddr*>(&sa), &len);
        *bound_port = ntohs(sa.sin_port);
    }
    *out = std::move(fd);
    return Status::ok();
}

void tcp_connect_async(Reactor& reactor, const std::string& host, uint16_t port, uint64_t timeout_ms,
                       std::function<void(Status, Fd, std::string)> done) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0 || !res) {
        done(Status::err(EHOSTUNREACH, "cannot resolve '%s': %s", host.c_str(), gai_strerror(rc)), Fd(), "");
        return;
    }
    char ipbuf[INET_ADDRSTRLEN] = "";
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr, ipbuf, sizeof ipbuf);
    std::string ip = ipbuf;
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        done(Status::sys("socket"), Fd(), ip);
        return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc == 0) {
        done(Status::ok(), Fd(fd), ip);
        return;
    }
    if (errno != EINPROGRESS) {
        Status st = Status::sys("connect %s:%u", ip.c_str(), port);
        ::close(fd);
        done(st, Fd(), ip);
        return;
    }
    struct Pending {
        Reactor::TimerId timer = 0;
        bool finished = false;
    };
    auto p = std::make_shared<Pending>();
    auto finish = [&reactor, fd, p, done, ip, port](Status st) {
        if (p->finished) return;
        p->finished = true;
        reactor.del_fd(fd);
        reactor.cancel_timer(p->timer);
        if (st.is_ok()) {
            done(st, Fd(fd), ip);
        } else {
            ::close(fd);
            done(st, Fd(), ip);
        }
    };
    p->timer = reactor.add_timer(timeout_ms * 1000000ull, [finish, ip, port] {
        finish(Status::err(ETIMEDOUT, "connect to %s:%u timed out", ip.c_str(), port));
    });
    (void)reactor.add_fd(fd, EPOLLOUT, [finish, fd, ip, port](uint32_t) {
        int soerr = 0;
        socklen_t len = sizeof soerr;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
        if (soerr) finish(Status::err(soerr, "connect to %s:%u failed", ip.c_str(), port));
        else finish(Status::ok());
    });
}

} // namespace dnuma
