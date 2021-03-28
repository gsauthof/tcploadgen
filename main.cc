
// SPDX-FileCopyrightText: © 2021 Georg Sauthoff <mail@gms.tf>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "client.hh"

#include <exception>
#include <iostream>
#include <string>
#include <sstream>

#include <ixxx/posix.hh>
#include <ixxx/linux.hh>  // prctl
#include <ixxx/pthread.hh>

#include <unistd.h>      // getopt
#include <sys/prctl.h>   // PR_SET_TIMERSLACK


struct Args {
    std::string host;
    std::string port;

    std::string filename;

    size_t no_senders {0};
    size_t no_pkts {0};
    bool timerslack {false};
    bool set_affinity {true};

    void parse(int argc, char **argv);
    void help(std::ostream &o, const char *argv0);

};

void Args::help(std::ostream &o, const char *argv0)
{
    o << argv0 << " - tcp load generator\n"
        << "Usage: " << argv0 << " -c FILENAME HOST PORT\n"
        << "\n"
        << "Options:\n"
        << "  -A             do NOT set thread CPU affinities\n"
        << "  -c FILENAME    TOML configuration\n"
        << "  -j #SENDERS    number of sender threads\n"
        << "  -h             display this help\n"
        << "  -n #PKTS       packets to send for each sender\n"
        << "  -s             use 1 ns timerslack instead of realtime sched policy\n"
        << "\n"
        << "2021, Georg Sauthoff <mail@gms.tf>, GPLv3+\n";
}

void Args::parse(int argc, char **argv)
{
    char c = 0;
    // '-' prefix: no reordering of arguments, non-option arguments are
    // returned as argument to the 1 option
    // ':': preceding option takes a mandatory argument
    while ((c = getopt(argc, argv, "-c:j:hn:s")) != -1) {
        switch (c) {
            case '?':
                {
                    std::ostringstream o;
                    o << "unexpected option : -" << char(optopt) << '\n';
                    throw std::runtime_error(o.str());
                }
                break;
            case 'A':
                set_affinity = false;
                break;
            case 'c':
                filename = optarg;
                break;
            case 'j':
                no_senders = atol(optarg);
                break;
            case 'h':
                help(std::cerr, argv[0]);
                exit(0);
                break;
            case 'n':
                no_pkts = atol(optarg);
                break;
            case 's':
                timerslack = true;
                ixxx::linux::prctl(PR_SET_TIMERSLACK, 1);
                break;
            case 1:
                if (host.empty())
                    host = optarg;
                else if (port.empty())
                    port = optarg;
                else
                    throw std::runtime_error("too many positional arguments");
                break;
        }
    }
    if (filename.empty())
        throw std::runtime_error("No configuration file specified (cf. -c FILENAME)");
    if (host.empty())
        throw std::runtime_error("No host specified (positional argument)");
    if (port.empty())
        throw std::runtime_error("No port specified (positional argument)");

}


int main(int argc, char **argv)
{
    try {
        Args args;
        args.parse(argc, argv);

        Client client;

        client.parse_config(args.filename.c_str());

        if (args.no_senders)
            while (args.no_senders < client.senders.size())
                client.senders.pop_back();

        int rw_pipe[2] = {0};
        ixxx::posix::pipe(rw_pipe);

        client.sender_cfg.receiver_pipe_in_fd = rw_pipe[1];
        client.receiver.pipe_out_fd = rw_pipe[0];

        for (auto &s : client.senders) {
            s.host = args.host.c_str();
            s.port = args.port.c_str();
            s.no_of_sends = args.no_pkts;
        }

        client.receiver.spawn(args.set_affinity);

        for (auto &sender : client.senders) {
            sender.spawn(!args.timerslack, args.set_affinity);
        }

        void *v = nullptr;
        bool success = true;
        ixxx::posix::pthread_join(client.receiver.thread_id, &v);
        success = success && !v;

        for (auto &sender : client.senders) {
            ixxx::posix::pthread_join(sender.thread_id, &v);
            success = success && !v;
        }

        std::cout << "Received messages: " << client.receiver.receive_count << '\n';
        for (auto &sender : client.senders) {
            std::cout << "Sent messages on core " << sender.core << ": "
                << sender.send_count << '\n'
                << "Missed timer events on core " << sender.core << ": "
                << sender.timer_was_late << '\n';
        }

        return !success;

    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}


