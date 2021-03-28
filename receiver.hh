
// SPDX-FileCopyrightText: © 2021 Georg Sauthoff <mail@gms.tf>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RECEIVER_HH
#define RECEIVER_HH

#include <unordered_set>
#include <stdint.h>
#include <pthread.h>

struct Field {
    unsigned off {0};
    unsigned size {0};

    uint64_t read_uint(const unsigned char *b, size_t l) const;
};


struct Receiver_Config {
    Field len;
    Field tag;

    unsigned error_tag {0};

    Field error_msg_len;
    unsigned error_msg_off {0};

    unsigned receive_next(int fd, unsigned char *buf, size_t buf_size) const;
};

struct Receiver {

    Receiver(const Receiver_Config &cfg)
        : cfg(cfg) {}

    const Receiver_Config &cfg;

    pthread_t thread_id {0};

    unsigned core {0};

    int pipe_out_fd {0};
    std::unordered_set<int> conn_fds;

    unsigned receive_count {0};

    void *main();

    void spawn(bool affinity);

};


#endif
