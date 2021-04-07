
// SPDX-FileCopyrightText: © 2021 Georg Sauthoff <mail@gms.tf>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "receiver.hh"

#include <ixxx/posix.hh>
#include <ixxx/linux.hh>
#include <ixxx/socket.hh>
#include <ixxx/pthread.hh>
#include <ixxx/util.hh>
#include <ixxx/pthread_util.hh>

#include <stdexcept>
#include <iostream>
#include <string.h>

#include <sys/epoll.h>


uint64_t Field::read_uint(const unsigned char *b, size_t l) const
{
    if (off + size > l)
        throw std::runtime_error("buffer too small for reading an integer");

    uint64_t r = 0;
    switch (size) {
        case 1:
            r = b[off];
            break;
        case 2:
            {
                uint16_t i;
                memcpy(&i, b + off, sizeof i);
                r = i;
            }
            break;
        case 4:
            {
                uint32_t i;
                memcpy(&i, b + off, sizeof i);
                r = i;
            }
            break;
        case 8:
            {
                uint64_t i;
                memcpy(&i, b + off, sizeof i);
                r = i;
            }
            break;
    }
    return r;
}



static std::string read_msg(const unsigned char *buf, size_t l,
        size_t error_msg_off, size_t error_msg_len)
{
    if (error_msg_off + error_msg_len > l)
        throw std::runtime_error("error message length out of bounds");
    std::string r(reinterpret_cast<const char*>(buf) + error_msg_off, error_msg_len);
    return r;
}

unsigned Receiver_Config::receive_next(int fd, unsigned char *buf, size_t buf_size) const
{
    ssize_t n = ixxx::util::read_all(fd, buf, len.off + len.size);
    if (!n) {
        throw std::underflow_error("early EOF on one conections");
    }
    if (n != len.off + len.size) {
        throw std::runtime_error("short read on one conections");
    }
    unsigned l = len.read_uint(buf, n);
    if (l + len.off + len.size > buf_size) {
        throw std::runtime_error("message too long");
    }
    if (l <= len.off + len.size) {
        throw std::runtime_error("message too short");
    }
    size_t x = l - len.off - len.size;
    n = ixxx::util::read_all(fd, buf + len.off + len.size, x);
    if (n != ssize_t(x)) {
        throw std::runtime_error("couldn't read complete message");
    }
    unsigned t = tag.read_uint(buf, l);
    if (t == error_tag) {
        std::string msg = read_msg(buf, l, error_msg_off,
                error_msg_len.read_uint(buf, l));
        throw std::runtime_error("Received error: " + msg);
    }
    return t;
}


void *Receiver::main()
{
    ixxx::util::FD efd ( ixxx::linux::epoll_create1(0) );
    struct epoll_event ev = { .events = EPOLLIN, .data = { .fd = pipe_out_fd } };
    ixxx::linux::epoll_ctl(efd, EPOLL_CTL_ADD, pipe_out_fd, &ev);

    unsigned char buf[64*1024];


    struct epoll_event evs[16];
    for (;;) {
        int k = ixxx::linux::epoll_wait(efd, evs, sizeof evs / sizeof evs[0], -1);
        for (int i = 0; i < k; ++i) {
            int fd = evs[i].data.fd;
            if (fd == pipe_out_fd) {
                int conn_fd = -1;
                size_t n = ixxx::posix::read(fd, &conn_fd, sizeof conn_fd);
                if (!n) {
                    // i.e. one sender closed its pipe write-end due to an error
                    // thus closing all registered connections to let other sender-threads
                    // fail, as well
                    std::cerr << "Receiver: pipe closed - closing all connections ...\n";
                    for (int x : conn_fds) {
                        std::cerr << "    closing conn " << x << '\n';
                        // we are ignoring errors here since we need to make sure
                        // to close _all_ connections to terminate the senders
                        // (and we are on an error path, anyways)
                        close(x);
                    }
                    return nullptr;
                }
                if (n != sizeof conn_fd) {
                    throw std::runtime_error("Receiver: short read on pipe");
                }
                struct epoll_event ev = { .events = EPOLLIN | EPOLLRDHUP, .data = { .fd = conn_fd } };
                ixxx::linux::epoll_ctl(efd, EPOLL_CTL_ADD, conn_fd, &ev);
                conn_fds.insert(conn_fd);
            } else {
                if (evs[i].events & (EPOLLHUP | EPOLLRDHUP)) {
                    // i.e. sender-thread shut its connection down, or server shut it down
                    conn_fds.erase(fd);
                    std::cout << "Closing conn_fd: " << fd <<  "\n";
                    ixxx::posix::close(fd);
                    if (conn_fds.empty())
                        return nullptr;
                } else {
                    try {
                        cfg.receive_next(fd, buf, sizeof buf);
                        ++receive_count;
                    } catch (const std::underflow_error &e) {
                        conn_fds.erase(fd);
                        std::cout << "Closing after EOF, conn_fd: " << fd <<  "\n";
                        ixxx::posix::close(fd);
                        if (conn_fds.empty())
                            return nullptr;
                    }
                }
            }
        }
    }
    return nullptr;
}

static void *receiver_main(void *x)
{
    Receiver *r = static_cast<Receiver*>(x);
    try {
        return r->main();
    } catch (std::exception &e) {
        close(r->pipe_out_fd);
        std::cerr << "Receiver failed: " << e.what() << '\n';
        return (void*)-1;
    }
}


void Receiver::spawn(bool affinity)
{
    ixxx::util::Pthread_Attr attr;

    if (affinity) {
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        CPU_SET(core, &cpus);
        ixxx::posix::pthread_attr_setaffinity_np(attr.ibute(), sizeof cpus, &cpus);
    }

    ixxx::posix::pthread_create(&thread_id, attr.ibute(), receiver_main,
            static_cast<void*>(this));
}


