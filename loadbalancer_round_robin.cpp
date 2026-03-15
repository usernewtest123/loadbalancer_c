#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>

#define MAX_EVENTS 1024
#define BUFFER_SIZE 4096
#define LISTEN_PORT 8181

// --- Backend structure ---
struct Backend {
    std::string ip;
    int port;
    std::atomic<bool> healthy{true};
    std::atomic<int> active_connections{0};

    Backend(std::string ip_, int port_) : ip(ip_), port(port_) {}
};

// --- Backend Pool ---
// std::atomic is non-copyable/non-movable, so backends cannot live in a
// plain vector with an initializer list.  Use unique_ptr to heap-allocate
// each Backend so the vector only stores pointers (which are movable).
std::vector<std::unique_ptr<Backend>> backends;
std::atomic<size_t> rr_index{0};

void init_backends() {
    backends.emplace_back(std::make_unique<Backend>("127.0.0.1", 9007));
    backends.emplace_back(std::make_unique<Backend>("127.0.0.1", 9003));
    backends.emplace_back(std::make_unique<Backend>("127.0.0.1", 6789));
}

// Select a healthy backend (round-robin)
Backend* select_backend() {
    size_t n = backends.size();
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (rr_index++) % n;
        if (backends[idx]->healthy) return backends[idx].get();
    }
    return nullptr;
}

// --- Utility ---
void set_nonblocking(int sock) {
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
}

// --- Connection ---
enum class ConnState { CONNECTING, ACTIVE, CLOSING };

struct Connection {
    int client_fd;
    int backend_fd;
    ConnState state;
    int epfd;
    Backend* backend;

    // Buffers
    char c2b_buf[BUFFER_SIZE];
    size_t c2b_start = 0, c2b_end = 0;
    char b2c_buf[BUFFER_SIZE];
    size_t b2c_start = 0, b2c_end = 0;

    Connection(int cfd, int bfd, int ep, Backend* b)
        : client_fd(cfd), backend_fd(bfd), epfd(ep), backend(b), state(ConnState::CONNECTING) {}

    ~Connection() {
        epoll_ctl(epfd, EPOLL_CTL_DEL, client_fd, nullptr);
        epoll_ctl(epfd, EPOLL_CTL_DEL, backend_fd, nullptr);
        close(client_fd);
        close(backend_fd);
        if (backend) backend->active_connections--;
        std::cout << "Connection closed: client=" << client_fd << " backend=" << backend_fd << "\n";
    }

    void close_connection() { state = ConnState::CLOSING; }

    void try_write(int fd) {
        char* buf; size_t* start; size_t* end;
        if (fd == backend_fd) { buf = c2b_buf; start = &c2b_start; end = &c2b_end; }
        else                  { buf = b2c_buf; start = &b2c_start; end = &b2c_end; }

        while (*start < *end) {
            ssize_t n = write(fd, buf + *start, *end - *start);
            if (n > 0) *start += n;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else { close_connection(); break; }
        }
        if (*start == *end) *start = *end = 0;
    }

    void append_to_buffer(int fd, const char* data, size_t n) {
        if (fd == client_fd) {
            size_t space = BUFFER_SIZE - c2b_end;
            if (n > space) n = space;
            memcpy(c2b_buf + c2b_end, data, n);
            c2b_end += n;
        } else {
            size_t space = BUFFER_SIZE - b2c_end;
            if (n > space) n = space;
            memcpy(b2c_buf + b2c_end, data, n);
            b2c_end += n;
        }
    }

    // Returns true if there is buffered data waiting to be written to fd
    bool has_data(int fd) const {
        if (fd == backend_fd) return c2b_end > c2b_start;
        else                  return b2c_end > b2c_start;
    }
};


std::map<int, Connection*> fd_to_connection;

void remove_connection(Connection* conn) {
    if (!conn) return;
    fd_to_connection.erase(conn->client_fd);
    fd_to_connection.erase(conn->backend_fd);
    delete conn;
}


void backend_health_checker() {
    while (true) {
        for (auto& b : backends) { 
            int sock = socket(AF_INET, SOCK_STREAM, 0);

            
            struct timeval tv{ .tv_sec = 2, .tv_usec = 0 };
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(b->port);
            inet_pton(AF_INET, b->ip.c_str(), &addr.sin_addr);

            int ret = connect(sock, (sockaddr*)&addr, sizeof(addr));
            b->healthy = (ret == 0);
            close(sock);
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

int main() {
    init_backends();
    std::thread(backend_health_checker).detach();

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);


    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(LISTEN_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(listen_fd, 128) < 0) { perror("listen"); return 1; }

    set_nonblocking(listen_fd);
    int epfd = epoll_create1(0);

    epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    epoll_event events[MAX_EVENTS];
    char buffer[BUFFER_SIZE];

    std::cout << "Load Balancer listening on port " << LISTEN_PORT << "\n";

    while (true) {
        int n_ready = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n_ready < 0) { if (errno == EINTR) continue; perror("epoll_wait"); break; }

        for (int i = 0; i < n_ready; ++i) {
            int      fd       = events[i].data.fd;
            uint32_t ev_flags = events[i].events;

            // --- New Client ---
            if (fd == listen_fd) {
                while (true) {
                    int client_fd = accept(listen_fd, nullptr, nullptr);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept"); break;
                    }
                    set_nonblocking(client_fd);

                    Backend* backend = select_backend();
                    if (!backend) { std::cerr << "No healthy backend!\n"; close(client_fd); continue; }

                    int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
                    set_nonblocking(backend_fd);

                    sockaddr_in baddr{};
                    baddr.sin_family = AF_INET;
                    baddr.sin_port   = htons(backend->port);
                    inet_pton(AF_INET, backend->ip.c_str(), &baddr.sin_addr);

                    int ret = connect(backend_fd, (sockaddr*)&baddr, sizeof(baddr));
                    if (ret < 0 && errno != EINPROGRESS) {
                        perror("backend connect");
                        close(client_fd); close(backend_fd); continue;
                    }

                    backend->active_connections++;

                    Connection* conn = new Connection(client_fd, backend_fd, epfd, backend);
                    fd_to_connection[client_fd]  = conn;
                    fd_to_connection[backend_fd] = conn;

                    epoll_event e1{}; e1.events = EPOLLIN | EPOLLET; e1.data.fd = client_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &e1);

                    
                    epoll_event e2{};
                    e2.events  = EPOLLOUT | EPOLLET;
                    e2.data.fd = backend_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, backend_fd, &e2);

                    
                    conn->state = ConnState::CONNECTING;

                    std::cout << "New client=" << client_fd << " backend=" << backend_fd
                              << ((ret == 0) ? " ready" : " connecting") << "\n";
                }
                continue; // done handling listen_fd
            }

     
            auto it = fd_to_connection.find(fd);
            if (it == fd_to_connection.end()) continue;
            Connection* conn = it->second;

            // Backend connect completed
            if (conn->state == ConnState::CONNECTING && fd == conn->backend_fd && (ev_flags & EPOLLOUT)) {
                int err = 0; socklen_t len = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                    remove_connection(conn); continue;
                }
                conn->state = ConnState::ACTIVE;
                epoll_event e{}; e.events = EPOLLIN | EPOLLET; e.data.fd = fd;
                epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &e);
                continue;
            }

          
            bool removed = false;

            // --- Read ---
            if (!removed && (ev_flags & EPOLLIN)) {
                while (true) {
                    ssize_t n = read(fd, buffer, sizeof(buffer));
                    if (n > 0) {
                        conn->append_to_buffer(fd, buffer, n);
                        int peer_fd = (fd == conn->client_fd) ? conn->backend_fd : conn->client_fd;
                        if (conn->has_data(peer_fd)) {
                            epoll_event e{}; e.events = EPOLLIN | EPOLLOUT | EPOLLET; e.data.fd = peer_fd;
                            epoll_ctl(epfd, EPOLL_CTL_MOD, peer_fd, &e);
                        }
                        // Stop reading if the destination buffer is full.
                        if ((fd == conn->client_fd  && conn->c2b_end == BUFFER_SIZE) ||
                            (fd == conn->backend_fd && conn->b2c_end == BUFFER_SIZE)) break;
                    } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                        remove_connection(conn);
                        removed = true;
                        break;
                    } else {
                        break; // EAGAIN — no more data right now
                    }
                }
            }

            // --- Write ---
            if (!removed && (ev_flags & EPOLLOUT)) {
                conn->try_write(fd);
                if (!conn->has_data(fd)) {
                    epoll_event e{}; e.events = EPOLLIN | EPOLLET; e.data.fd = fd;
                    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &e);
                }
            }

            if (!removed && conn->state == ConnState::CLOSING) {
                remove_connection(conn);
            }
        }
    }

    close(listen_fd);
    return 0;
}
