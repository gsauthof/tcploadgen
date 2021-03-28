
// SPDX-FileCopyrightText: © 2021 Georg Sauthoff <mail@gms.tf>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CLIENT_HH
#define CLIENT_HH

#include "receiver.hh"

#include <vector>
#include <string_view>
#include <stdint.h>

struct Var_Decls {
    unsigned char sizes[16] {0};
    unsigned offs[16] {0};
};

struct Vars {
    unsigned char v[8][32] {0};

};


enum class Operator {
    INCREMENT
};

Operator str2operator(const std::string_view &s);


struct Packet {
    unsigned char payload[1024];
    unsigned payload_size;
    unsigned answer_tag;
    unsigned char vars[8];
    // array of { operator, variable } pairs
    unsigned char actions[8][2];

    void apply_variables(const Var_Decls &decls, const Vars &global_vars, Vars &vars);
};

struct Session {
    uint64_t start_off_ns {0};
    uint64_t interval_ns {0};

    Vars vars;

    int fd {0};
    int tfd {0};

    unsigned flow_pos {0};

    unsigned packet_counter {0};
};

struct Sender_Config {
    Vars vars;
    Var_Decls var_decls;


    int receiver_pipe_in_fd {0};
};

struct Sender {

    Sender(const Sender_Config &cfg, const Receiver_Config &rcfg)
        : cfg(cfg), receiver_cfg(rcfg) {}

    const Sender_Config &cfg;
    const Receiver_Config &receiver_cfg;

    // each sender gets a copy of the flow since packets
    // are modified via variables in each sender
    std::vector<Packet> prelude_flow;
    std::vector<Packet> main_flow;

    std::vector<Session> sessions;

    const char *host {nullptr};
    const char *port {nullptr};

    pthread_t thread_id {0};

    unsigned core {0};
    unsigned priority {0};

    size_t no_of_sends {0};
    size_t send_count {0};

    unsigned timer_was_late {0};

    unsigned main_flow_count {0};


    void *main();

    void spawn(bool realtime, bool affinity);

};


struct Client {
    Sender_Config sender_cfg;
    Receiver_Config receiver_cfg;

    std::vector<Sender> senders;

    Receiver receiver {receiver_cfg};


    void parse_config(const char *filename);
};

#endif
