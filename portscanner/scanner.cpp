#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
using namespace std;
using Clock = chrono::steady_clock;

struct Result {
    vector<int> open;
    int closed = 0, filtered = 0, total = 0;
    double seconds = 0;
    bool ok = true;
};

bool resolve(const string& host, sockaddr_in& out) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out = *(sockaddr_in*)res->ai_addr;
    freeaddrinfo(res);
    return true;
}
Result scan(sockaddr_in target, int start, int end, int batch, int timeout_ms) {
    Result r;
    r.total = end - start + 1;
    auto t0 = Clock::now();
    int next = start;

    while (next <= end) {
        vector<pollfd> fds;
        vector<int> ports;

        // Step 1: start many connections at once (non-blocking)
        while (next <= end && (int)fds.size() < batch) {
            int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
            if (fd < 0) break;
            target.sin_port = htons(next);
            connect(fd, (sockaddr*)&target, sizeof(target));
            fds.push_back({fd, POLLOUT, 0});
            ports.push_back(next++);
        }
        if (fds.empty()) { r.ok = false; return r; }

        // Step 2: wait for results until the timeout expires
        auto deadline = Clock::now() + chrono::milliseconds(timeout_ms);
        int left = fds.size();
        while (left > 0) {
            int ms = chrono::duration_cast<chrono::milliseconds>(deadline - Clock::now()).count();
            if (ms <= 0) break;
            poll(fds.data(), fds.size(), ms);
            for (size_t i = 0; i < fds.size(); i++) {
                if (fds[i].fd < 0 || !fds[i].revents) continue;
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(fds[i].fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0) r.open.push_back(ports[i]);   // handshake completed
                else          r.closed++;                    // refused
                close(fds[i].fd);
                fds[i].fd = -1;
                left--;
            }
        }

        // Step 3: anything still waiting got no reply -> filtered
        for (auto& f : fds)
            if (f.fd >= 0) { close(f.fd); r.filtered++; }
    }

    sort(r.open.begin(), r.open.end());
    r.seconds = chrono::duration<double>(Clock::now() - t0).count();
    return r;
}
void print_result(const Result& r) {
    cout << "\nOpen ports:\n";
    if (r.open.empty()) cout << "  (none)\n";
    for (int p : r.open) {
        servent* s = getservbyport(htons(p), "tcp");
        cout << "  " << p << "/tcp  " << (s ? s->s_name : "unknown") << "\n";
    }
    int rate = r.seconds > 0 ? (int)(r.total / r.seconds) : 0;
    cout << "\nDone: " << r.open.size() << " open, " << r.closed << " closed, "
         << r.filtered << " filtered | " << r.total << " scanned in "
         << fixed << setprecision(3) << r.seconds << "s (" << rate << " ports/sec)\n";
}
int run_demo() {
    vector<int> wanted = {3000, 5000, 7000, 8080, 9090};
    vector<int> listeners, created;

    cout << "=== DEMO MODE ===\n\nStep 1: creating test servers on localhost\n";
    for (int p : wanted) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(p);
        if (bind(fd, (sockaddr*)&a, sizeof(a)) == 0 && listen(fd, 128) == 0) {
            listeners.push_back(fd);
            created.push_back(p);
            cout << "  listening on port " << p << "\n";
        } else {
            cout << "  port " << p << " already in use, skipped\n";
            close(fd);
        }
    }
    if (created.empty()) { cout << "Could not create any test port.\n"; return 1; }

    cout << "\nStep 2: scanning 127.0.0.1 ports 1-10000\n";
    sockaddr_in target{};
    resolve("127.0.0.1", target);
    Result r = scan(target, 1, 10000, 500, 500);
    print_result(r);

    cout << "\nStep 3: verification\n";
    int found = 0;
    for (int p : created)
        if (find(r.open.begin(), r.open.end(), p) != r.open.end()) found++;
    cout << "  test ports created : " << created.size() << "\n"
         << "  test ports detected: " << found << "\n";
    for (int p : r.open)
        if (find(created.begin(), created.end(), p) == created.end())
            cout << "  (port " << p << " is another service already running on this machine)\n";
    bool pass = r.ok && found == (int)created.size();
    cout << "  RESULT: " << (pass ? "PASS - scanner detected every open port" : "FAIL") << "\n";

    for (int fd : listeners) close(fd);
    return pass ? 0 : 1;
}
void usage() {
    cout << "Usage:\n"
         << "  ./scanner <host> [-p start-end] [-c concurrency] [-t timeout_ms]\n"
         << "  ./scanner --demo\n"
         << "Example: ./scanner 127.0.0.1 -p 1-10000\n";
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }
    if (string(argv[1]) == "--demo") return run_demo();

    string host = argv[1];
    int start = 1, end = 1024, conc = 200, timeout_ms = 800;
    for (int i = 2; i < argc; i++) {
        string a = argv[i];
        if (i + 1 >= argc) { usage(); return 1; }
        if (a == "-p") {
            if (sscanf(argv[++i], "%d-%d", &start, &end) != 2) start = end = atoi(argv[i]);
        } else if (a == "-c") conc = atoi(argv[++i]);
        else if (a == "-t") timeout_ms = atoi(argv[++i]);
        else { usage(); return 1; }
    }
    if (start < 1 || end > 65535 || start > end || conc < 1 || conc > 900 || timeout_ms < 1) {
        cout << "Invalid input (ports 1-65535, concurrency 1-900)\n";
        return 1;
    }

    sockaddr_in target{};
    if (!resolve(host, target)) { cout << "Cannot resolve host: " << host << "\n"; return 1; }
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &target.sin_addr, ip, sizeof(ip));
    cout << "Scanning " << host << " (" << ip << ") ports " << start << "-" << end << "\n";

    Result r = scan(target, start, end, conc, timeout_ms);
    if (!r.ok) { cout << "Scan failed (out of file descriptors, try a smaller -c)\n"; return 1; }
    print_result(r);
    return 0;
}
