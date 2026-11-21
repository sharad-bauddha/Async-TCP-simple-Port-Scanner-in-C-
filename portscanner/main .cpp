#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <vector>
#include <unordered_map>

using Clock = std::chrono::steady_clock;

struct Options {
    std::string host;
    int start = 1, end = 1024;
    int concurrency = 500;
    int timeout_ms = 800;
};

struct Pending {
    int port;
    Clock::time_point deadline;
};

static void usage(const char* p) {
    std::cerr << "Usage: " << p << " <host> [-p start-end] [-c concurrency] [-t timeout_ms]\n"
              << "Example: " << p << " 127.0.0.1 -p 1-65535 -c 1000 -t 500\n";
}

static bool parse_args(int argc, char** argv, Options& o) {
    if (argc < 2) return false;
    o.host = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (i + 1 >= argc) return false;
        if (a == "-p") {
            if (sscanf(argv[++i], "%d-%d", &o.start, &o.end) != 2) {
                o.start = o.end = std::atoi(argv[i]);
            }
        } else if (a == "-c") o.concurrency = std::atoi(argv[++i]);
        else if (a == "-t") o.timeout_ms = std::atoi(argv[++i]);
        else return false;
    }
    return o.start >= 1 && o.end <= 65535 && o.start <= o.end &&
           o.concurrency > 0 && o.timeout_ms > 0;
}

static bool resolve(const std::string& host, sockaddr_in& out) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    freeaddrinfo(res);
    return true;
}

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) { usage(argv[0]); return 1; }

    sockaddr_in target{};
    if (!resolve(opt.host, target)) {
        std::cerr << "Cannot resolve host: " << opt.host << "\n";
        return 1;
    }
    char ipstr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &target.sin_addr, ipstr, sizeof ipstr);
    std::cout << "Scanning " << opt.host << " (" << ipstr << ") ports "
              << opt.start << "-" << opt.end << "\n";

    int ep = epoll_create1(0);
    if (ep < 0) { perror("epoll_create1"); return 1; }

    std::unordered_map<int, Pending> pending;   // fd -> state
    std::set<int> open_ports;
    int next_port = opt.start;
    auto t0 = Clock::now();

    auto finish = [&](int fd, bool is_open) {
        if (is_open) open_ports.insert(pending[fd].port);
        epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        pending.erase(fd);
    };

    epoll_event events[256];
    while (next_port <= opt.end || !pending.empty()) {
        // 1) start new connections up to the concurrency limit
        while (next_port <= opt.end && (int)pending.size() < opt.concurrency) {
            int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
            if (fd < 0) {
                if (errno == EMFILE || errno == ENFILE) break;  // fd limit: wait for some to finish
                perror("socket"); return 1;
            }
            sockaddr_in addr = target;
            addr.sin_port = htons(next_port);
            int port = next_port++;
            int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
            if (rc == 0) {                       // connected immediately (common on localhost)
                open_ports.insert(port);
                close(fd);
            } else if (errno == EINPROGRESS) {
                epoll_event ev{};
                ev.events = EPOLLOUT;
                ev.data.fd = fd;
                epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
                pending[fd] = {port, Clock::now() + std::chrono::milliseconds(opt.timeout_ms)};
            } else {
                close(fd);                       // immediate failure
            }
        }
        if (pending.empty()) continue;

        // 2) wait for events (short tick so timeouts are checked regularly)
        int n = epoll_wait(ep, events, 256, 50);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (!pending.count(fd)) continue;
            int err = 0;
            socklen_t len = sizeof err;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            finish(fd, err == 0);               // err==0 -> handshake completed -> open
        }

        // 3) expire timed-out connections (filtered / dropped packets)
        auto now = Clock::now();
        std::vector<int> expired;
        for (auto& [fd, p] : pending)
            if (now >= p.deadline) expired.push_back(fd);
        for (int fd : expired) finish(fd, false);
    }
    close(ep);

    double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    std::cout << "\nOpen ports:\n";
    for (int p : open_ports) {
        servent* s = getservbyport(htons(p), "tcp");
        std::cout << "  " << p << "/tcp  " << (s ? s->s_name : "unknown") << "\n";
    }
    std::cout << "\nDone: " << open_ports.size() << " open, "
              << (opt.end - opt.start + 1) << " scanned in " << secs << "s\n";
    return 0;
}
