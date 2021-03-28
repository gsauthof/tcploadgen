
// SPDX-FileCopyrightText: © 2021 Georg Sauthoff <mail@gms.tf>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "client.hh"

#include <ixxx/posix.hh>
#include <ixxx/pthread.hh>
#include <ixxx/socket.hh>
#include <ixxx/linux.hh>
#include <ixxx/util.hh>
#include <ixxx/pthread_util.hh>

#include <unordered_map>
#include <sstream>
#include <iostream>

#include <assert.h>
#include <string.h> // memcpy

#include <sys/epoll.h> // epoll_event
#include <sys/timerfd.h> // TFD_TIMER_ABSTIME


static std::unordered_map<std::string_view, Operator> str2op_map = {
    { "inc", Operator::INCREMENT }
};
Operator str2operator(const std::string_view &s)
{
    return str2op_map[s];
}


static void increment_uint(unsigned char *v, unsigned size)
{
    switch (size) {
        case 1:
            ++*v;
            break;
        case 2:
            {
                uint16_t i;
                memcpy(&i, v, size);
                ++i;
                memcpy(v, &i, size);
            }
            break;
        case 4:
            {
                uint32_t i;
                memcpy(&i, v, size);
                ++i;
                memcpy(v, &i, size);
            }
            break;
        case 8:
            {
                uint64_t i;
                memcpy(&i, v, size);
                ++i;
                memcpy(v, &i, size);
            }
            break;
    }
}


void Packet::apply_variables(const Var_Decls &decls, const Vars &global, Vars &local)
{
    for (unsigned i = 0; i < sizeof vars / sizeof vars[0]; ++i) {
        if (!vars[i])
            break;

        unsigned k = vars[i] - 1;
        unsigned n = sizeof global.v / sizeof global.v[0];
        assert(k < 2*n);

        const Vars *t;
        unsigned j;
        if (k < n) {
            t = &global;
            j = k;
        } else {
            t = &local;
            j = k - n;
        }
        memcpy(payload + decls.offs[k], t->v[j], decls.sizes[k]);


    }
    for (unsigned i = 0; i < sizeof actions / sizeof actions[0]; ++i) {
        if (!actions[i][0])
            break;

        Operator a = Operator(actions[i][0] - 1);
        unsigned k = actions[i][1] - 1;
        unsigned n = sizeof global.v / sizeof global.v[0];
        assert(k < 2*n);

        Vars *t;
        unsigned j;
        if (k < n) {
            throw std::runtime_error("cannot modify globals");
        } else {
            t = &local;
            j = k - n;
        }

        switch (a) {
            case Operator::INCREMENT:
                increment_uint(t->v[j], decls.sizes[k]);
                break;
            default:
                throw std::runtime_error("unknown operator");
        }
    }
}




static long next_minute_epoche()
{
    struct timespec ts = {0};
    ixxx::posix::clock_gettime(CLOCK_REALTIME, &ts);
    long x = ts.tv_sec + 62;
    x = x / 60 * 60;
    return x;

}

static void login(int fd, std::vector<Packet> &flow, const Var_Decls &var_decls,
        const Vars &globals, Vars &locals, const Receiver_Config &cfg)
{
    unsigned char buf[64*1024];
    for (auto &packet : flow) {
        packet.apply_variables(var_decls, globals, locals);
        ixxx::util::write_all(fd, packet.payload, packet.payload_size);

        cfg.receive_next(fd, buf, sizeof buf);
        unsigned t = cfg.tag.read_uint(buf, sizeof buf);
        if (t != packet.answer_tag) {
            std::ostringstream o;
            o << "Unexpected answer tag: " << t << " (expected: " << packet.answer_tag << ')';
            throw std::runtime_error(o.str());
        }
    }
}


void *Sender::main()
{
    ixxx::util::FD efd ( ixxx::linux::epoll_create1(0) );
    {
        struct epoll_event ev = { .events = EPOLLERR,
            .data = { .ptr = 0 } };
        ixxx::linux::epoll_ctl(efd, EPOLL_CTL_ADD, cfg.receiver_pipe_in_fd, &ev);
    }
    std::vector<ixxx::util::FD> tfds; // i.e. for auto-closing
    tfds.reserve(sessions.size());
    for (auto &session : sessions) {
        session.fd = ixxx::util::connect(host, port);
        if (session.fd == -1) {
            std::ostringstream o;
            o << "Couldn't connect to " << host << ':' << port << " (" << errno << ')';
            throw std::runtime_error(o.str());
        }

        login(session.fd, prelude_flow, cfg.var_decls, cfg.vars, session.vars, receiver_cfg);

        ixxx::posix::write(cfg.receiver_pipe_in_fd, &session.fd, sizeof session.fd);

        session.tfd = ixxx::linux::timerfd_create(CLOCK_REALTIME, 0);
        tfds.emplace_back(session.tfd);

        struct itimerspec spec = {
            .it_interval = { .tv_nsec = long(session.interval_ns) },
            .it_value = {
                .tv_sec     = next_minute_epoche(),
                .tv_nsec    = long(session.start_off_ns)
            }
        };
        ixxx::linux::timerfd_settime(session.tfd, TFD_TIMER_ABSTIME, &spec,  0);

        struct epoll_event ev = { .events = EPOLLIN,
            .data = { .ptr = static_cast<Session*>(&session) } };
        ixxx::linux::epoll_ctl(efd, EPOLL_CTL_ADD, session.tfd, &ev);
    }
    struct epoll_event evs[16];
    bool loop_on = true;
    while (loop_on) {
        int k = epoll_wait(efd, evs, sizeof evs / sizeof evs[0], -1);
        for (int i = 0; i < k; ++i) {

            if (!evs[i].data.ptr) {
                // this will also close the write-side of the pipe
                // which is detected by the receiver-thread which then terminates, as well
                throw std::runtime_error("receiver terminated early");
            }

            Session &session = *static_cast<Session*>(evs[i].data.ptr);

            uint64_t n = 0;
            auto l = ixxx::posix::read(session.tfd, &n, sizeof n);
            assert(l == sizeof n);
            if (n != 1) {
                std::cerr << "Timer expired more than once on core " << core << ": " << l << '\n';
                ++timer_was_late;
            }

            if (send_count >= no_of_sends) {
                for (auto &session : sessions) {
                    auto c = session.fd;
                    std::cout << "Shutting down fd: " << c << '\n';
                    ixxx::posix::shutdown(c, SHUT_RDWR);
                    // we are closing it in the receiver!
                    // (closing it here would remove it from the receiver's epoll set
                    //  without a wake-up ...)
                }
                loop_on = false;
                break;
            }

            Packet &packet = main_flow[session.flow_pos++ % main_flow.size()];
            packet.apply_variables(cfg.var_decls, cfg.vars, session.vars);
            ixxx::util::write_all(session.fd, packet.payload, packet.payload_size);

            ++send_count;

        }
    }
    return 0;
}

static void *sender_main(void *x)
{
    Sender *s = static_cast<Sender*>(x);
    try {
        void *v = s->main();
        return v;
    } catch (std::exception &e) {
        close(s->cfg.receiver_pipe_in_fd);
        std::cerr << "Sender failed: " << e.what() << '\n';
        return (void*)-1;
    }
}

void Sender::spawn(bool realtime, bool affinity)
{
    ixxx::util::Pthread_Attr attr;

    if (affinity) {
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        CPU_SET(core, &cpus);
        ixxx::posix::pthread_attr_setaffinity_np(attr.ibute(), sizeof cpus, &cpus);
    }

    if (realtime) {
        ixxx::posix::pthread_attr_setschedpolicy(attr.ibute(), SCHED_FIFO);
        struct sched_param p = { .sched_priority = 1 };
        ixxx::posix::pthread_attr_setschedparam(attr.ibute(), &p);
        ixxx::posix::pthread_attr_setinheritsched(attr.ibute(), PTHREAD_EXPLICIT_SCHED);
    }

    ixxx::posix::pthread_create(&thread_id, attr.ibute(), sender_main,
            static_cast<void*>(this));
}


