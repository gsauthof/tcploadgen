
// SPDX-FileCopyrightText: © 2021 Georg Sauthoff <mail@gms.tf>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "client.hh"

#include <fstream>
#include <toml++/toml.h>

#include <stdexcept>
#include <unordered_map>
#include <string>
#include <string_view>
#include <string.h>

#include <iostream>

static void parse_vars(const toml::table &tbl, Var_Decls &decls,
        std::unordered_map<std::string, unsigned> &var2id)
{
    auto vars = tbl["variables"];
    if (!vars)
        throw std::runtime_error("[variables] table is missing");

    auto globals = tbl["global"].as_table();

    unsigned global_reg = 0;
    unsigned local_reg = 8;

    for (auto &p : *vars.as_table()) {
        unsigned i = 0;
        if (globals && globals->contains(p.first)) {
            i = global_reg++;
            if (i > 7)
                throw std::runtime_error("too many global variables");
        } else {
            i = local_reg++;
            if (i > 15)
                throw std::runtime_error("too many local variables");
        }
        toml::node_view q{p.second};
        decls.sizes[i] = q["size"].value<unsigned>().value();
        decls.offs[i] = q["off"].value<unsigned>().value();

        var2id[p.first] = i;

    }
}

static void store_int(uint64_t i, unsigned size, unsigned char *s)
{
    switch (size) {
        case 1:
            {
                uint8_t x = i;
                *s = x;
            }
            break;
        case 2:
            {
                uint16_t x = i;
                memcpy(s, &x, sizeof x);
            }
            break;
        case 4:
            {
                uint32_t x = i;
                memcpy(s, &x, sizeof x);
            }
            break;
        case 8:
            {
                memcpy(s, &i, sizeof i);
            }
            break;
        default:
            throw std::runtime_error("Unimplemented integer size");
    }
}

static void store_str(const std::string_view &v, unsigned size, unsigned char *s)
{
    unsigned l = std::min(static_cast<unsigned>(v.size()), size);
    memcpy(s, v.data(), l);
}

static void parse_ass(const toml::node_view<const toml::node> &tbl, bool global, const Var_Decls &decls,
        std::unordered_map<std::string, unsigned> &var2id, Vars &vars)
{
    unsigned off = global ? 0 : 8;
    for (auto &p : *tbl.as_table()) {
        auto x = var2id.find(p.first);
        if (x == var2id.end())
            throw std::runtime_error("Couldn't find variable decl: " + std::string(p.first));

        if (!global && x->second < 8)
            throw std::runtime_error("accessing a global variable from a local context");
        unsigned i = x->second - off;
        if (i > 7)
            throw std::runtime_error("accessing a local variable from a global context");

        //std::cerr << "dbg: writing " << p.first << " to slot: " << i << '\n';

        switch (p.second.type()) {
            case toml::node_type::integer:
                store_int(p.second.value<uint64_t>().value(), decls.sizes[x->second],
                        vars.v[i]);
                break;
            case toml::node_type::string:
                store_str(p.second.value<std::string_view>().value(), decls.sizes[x->second],
                        vars.v[i]);
                break;
            default:
                throw std::runtime_error("Type not implemented for: " + std::string(p.first));
                break;
        }


    }
}

struct BCD_Table {
    unsigned char table[256] {0};

    constexpr BCD_Table()
    {
        for (char c = '0'; c <= '9'; ++c)
            table[static_cast<unsigned char>(c)] = c - '0';
        for (char c = 'a'; c <= 'f'; ++c)
            table[static_cast<unsigned char>(c)] = c - 'a' + 10;
        for (char c = 'A'; c <= 'F'; ++c)
            table[static_cast<unsigned char>(c)] = c - 'A' + 10;
    }

    unsigned char lookup(char high, char low) const
    {
        unsigned char h = table[static_cast<unsigned char>(high)];
        h = h << 4;
        unsigned char l = table[static_cast<unsigned char>(low)];
        unsigned r = h | l;
        return r;
    }
};

static constexpr BCD_Table bcd_table;

static void parse_packet(const std::string_view &s, Packet &p)
{
    if (s.size() % 2)
        throw std::runtime_error("packet string ends with a half byte");
    if (s.size() > sizeof p.payload)
        throw std::runtime_error("packet payload too large");
    unsigned k = 0;
    for (size_t i = 0; i < s.size(); i += 2) {
        p.payload[k++] = bcd_table.lookup(s[i], s[i+1]);
    }
    p.payload_size = k;
}



static void parse_flow(const toml::array &pkts,
        std::unordered_map<std::string, unsigned> var2id,
        std::vector<Packet> &flow)
{
    for (const toml::node &pkt : pkts) {
        flow.emplace_back();
        Packet &p = flow.back();

        const toml::table *tblP = pkt.as_table();
        if (!tblP)
            throw std::runtime_error("flow element is not a table");

        const toml::table &tbl = *tblP;

        if (auto v = tbl["pkt"]) {
            if (auto w = v.value<std::string_view>())
                parse_packet(w.value(), p);
            else
                throw std::runtime_error("pkt not a string type");
        } else {
            throw std::runtime_error("pkt key missing in flow packet");
        }

        if (auto vars = tbl["vars"].as_array()) {
            unsigned k = 0;
            for (const toml::node &var : *vars) {
                auto i = var2id.find(var.value<std::string>().value());
                if (i == var2id.end()) {
                    throw std::runtime_error("unknown variable: " + var.value<std::string>().value());
                }
                if (k >= sizeof p.vars / sizeof p.vars[0])
                    throw std::runtime_error("too many variables specified in packet");
                p.vars[k++] = 1 + i->second;
            }
        }

        if (auto actions = tbl["actions"].as_array()) {
            unsigned k = 0;
            for (const toml::node &a : *actions) {
                const toml::table *actP = a.as_table();
                if (!actP)
                    throw std::runtime_error("action is not a table");
                const toml::table &act = *actP;
                if (k >= sizeof p.actions / sizeof p.actions[0])
                    throw std::runtime_error("too many actions specified in packet");
                p.actions[k][0] = 1 + static_cast<unsigned>(str2operator(act["op"].value<std::string_view>().value()));
                unsigned id = var2id[act["name"].value<std::string>().value()];
                if (id < sizeof Vars::v / sizeof Vars::v[0])
                    throw std::runtime_error("can't modify global variable with action");
                p.actions[k][1] = 1 + id;
                ++k;
            }
        }

        if (auto answer_tag = tbl["answer_tag"].as_integer()) {
            p.answer_tag = **answer_tag;
        }


    }
}

template <typename T>
static void set_or_fail(T &x, const toml::node_view<const toml::node> &tbl, const char *key,
        const char *prefix)
{
    auto node = tbl[key];
    if (!node) {
        std::ostringstream o;
        o << "Key not found: " << prefix << key;
        throw std::runtime_error(o.str());
    }
    auto v = node.value<T>();
    if (!v) {
        std::ostringstream o;
        o << prefix << key << " has unexpected type";
        throw std::runtime_error(o.str());
    }
    x = v.value();
}

static void parse_field(const toml::node_view<const toml::node> &tbl, const char *key,
        Field &f, const char *prefixP)
{
    std::string t(prefixP);
    t += key;
    t += '.';
    const char *prefix = t.c_str();

    auto node = tbl[key];
    set_or_fail(f.off, node, "off", prefix);
    set_or_fail(f.size, node, "size", prefix);
}

static void parse_receiver(const toml::node_view<const toml::node> &tbl, Receiver &rec,
        Receiver_Config &cfg)
{
    const char *prefix = "receiver.";
    set_or_fail(rec.core, tbl, "core", prefix);
    set_or_fail(cfg.error_msg_off, tbl, "error_msg_off", prefix);
    set_or_fail(cfg.error_tag, tbl, "error_tag", prefix);

    parse_field(tbl, "len", cfg.len, prefix);
    parse_field(tbl, "tag", cfg.tag, prefix);
    parse_field(tbl, "error_msg_len", cfg.error_msg_len, prefix);
}

void Client::parse_config(const char *filename)
{
    try {

    toml::table tblP = toml::parse_file(filename);
    const toml::table &tbl = tblP;


    const toml::array *cores = tbl["sender"]["cores"].as_array();

    if (!cores)
        throw std::runtime_error("no sender.cores specified!");

   
    senders.reserve(cores->size());

    std::unordered_map<std::string, unsigned> var2id;
    parse_vars(tbl, sender_cfg.var_decls, var2id);

    parse_ass(tbl["global"], true, sender_cfg.var_decls, var2id, sender_cfg.vars);

    for (const toml::node &node : *cores) {
        senders.emplace_back(sender_cfg, receiver_cfg);
        Sender &sender = senders.back();

        sender.core = node.value<unsigned>().value();
        sender.priority = tbl["sender"]["priority"].value_or(0u);

        if (auto t = tbl["flow"]["prelude"].as_array())
            parse_flow(*t, var2id, sender.prelude_flow);
        else
            throw std::runtime_error("flow.prelude is missing");
        if (auto t = tbl["flow"]["main"].as_array())
            parse_flow(*t, var2id, sender.main_flow);
        else
            throw std::runtime_error("flow.main is missing");
    }

    const toml::array *sessions = tbl["sessions"].as_array();
    if (!sessions)
        throw std::runtime_error("no sessions defined!");

    uint64_t interval_ns = tbl["sender"]["session"]["interval_ns"].value<uint64_t>().value_or(0);
    if (!interval_ns)
        throw std::runtime_error("no sender.session.interval_ns specified");
    uint64_t start_off_inc_ns = tbl["sender"]["session"]["start_off_inc_ns"].value<uint64_t>().value_or(0);
    if (!start_off_inc_ns)
        throw std::runtime_error("no sender.session.start_off_inc_ns specified");
    uint64_t start_off_ns = tbl["sender"]["session"]["start_off_ns"].value<uint64_t>().value_or(0);

    unsigned session_limit = tbl["sender"]["sessions"].value<unsigned>().value_or(unsigned(-1));

    unsigned i = 0;
    unsigned k = 0;
    for (const toml::node &node : *sessions) {
        if (k >= session_limit)
            break;
        senders[i].sessions.emplace_back();
        senders[i].sessions.back().start_off_ns = start_off_ns;
        senders[i].sessions.back().interval_ns = interval_ns;
        parse_ass(toml::node_view{node}, false, sender_cfg.var_decls, var2id,
                senders[i].sessions.back().vars);

        start_off_ns += start_off_inc_ns;
        i = (i + 1) % senders.size();
        ++k;
    }


    parse_receiver(tbl["receiver"], receiver, receiver_cfg);

    } catch (const toml::parse_error &e) {
        std::ostringstream o;
        o << "Parse Error: " << e;
        throw std::runtime_error(o.str());
    }
}
