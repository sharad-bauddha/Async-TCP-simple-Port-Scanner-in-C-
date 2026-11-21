#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <iostream>
#include <vector>
using namespace std;

int main(int argc, char** argv) {
    if (argc < 4) { cout << "Usage: ./scanner <host> <start> <end>\n"; return 1; }
    int start = atoi(argv[2]), end = atoi(argv[3]);
    const int BATCH = 200, TIMEOUT_MS = 1000;

    // 1. Host ka IP address nikalo
    addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(argv[1], nullptr, &hints, &res) != 0) { cout << "Host not found\n"; return 1; }
    sockaddr_in target = *(sockaddr_in*)res->ai_addr;
    freeaddrinfo(res);

    cout << "Scanning " << argv[1] << " ports " << start << "-" << end << "\n";

    // 2. Ports ko batches me scan karo
    for (int base = start; base <= end; base += BATCH) {
        vector<pollfd> fds;
        vector<int> ports;

        // 3. Ek saath kai non-blocking connections shuru karo
        for (int p = base; p < base + BATCH && p <= end; p++) {
            int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
            target.sin_port = htons(p);
            connect(fd, (sockaddr*)&target, sizeof(target));   // turant return hota hai
            fds.push_back({fd, POLLOUT, 0});
            ports.push_back(p);
        }

        // 4. poll() se wait karo, jo connection complete ho use check karo
        auto deadline = chrono::steady_clock::now() + chrono::milliseconds(TIMEOUT_MS);
        int left = fds.size();
        while (left > 0) {
            int ms = chrono::duration_cast<chrono::milliseconds>(deadline - chrono::steady_clock::now()).count();
            if (ms <= 0) break;                                 // timeout: baaki ports filtered
            poll(fds.data(), fds.size(), ms);
            for (size_t i = 0; i < fds.size(); i++) {
                if (fds[i].fd < 0 || !fds[i].revents) continue;
                int err = 0; socklen_t len = sizeof(err);
                getsockopt(fds[i].fd, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0) cout << "Port " << ports[i] << " is OPEN\n";   // handshake successful
                close(fds[i].fd);
                fds[i].fd = -1;                                 // poll ab ise ignore karega
                left--;
            }
        }
        for (auto& f : fds) if (f.fd >= 0) close(f.fd);         // timeout wale band karo
    }
    cout << "Scan complete.\n";
}
