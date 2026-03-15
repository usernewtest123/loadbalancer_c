#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <memory>

#define MAX_EVENTS   1024
#define BUFFER_SIZE  65536   // Increased: HTTP responses easily exceed 4 KB
#define LISTEN_PORT  8181

// ---------------------------------------------------------------------------
// Backend
// ---------------------------------------------------------------------------
struct Backend {
    std::string ip;
    int port;
    std::atomic<bool> healthy{true};
    std::atomic<int>  active_connections{0};
    Backend(std::string ip_, int port_) : ip(ip_), port(port_) {}
};

std::vector<std::unique_ptr<Backend>> backends;
std::atomic<size_t> rr_index{0};

void init_backends() {
    backends.emplace_back(std::make_unique<Backend>("127.0.0.1", 9007));
    backends.emplace_back(std::make_unique<Backend>("127.0.0.1", 9003));
    backends.emplace_back(std::make_unique<Backend>("127.0.0.1", 6789));
}

Backend* select_backend() {
    size_t n = backends.size();
    for (size_t i = 0; i < n; ++i) {
        size_t idx = rr_index.fetch_add(1, std::memory_order_relaxed) % n;
        if (backends[idx]->healthy) return backends[idx].get();
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------
void set_nonblocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

// Disable Nagle — critical for HTTP: without this the kernel holds small
// writes (e.g. a lone status line) until the ACK arrives, adding ~40 ms
// latency on loopback and making browsers think the server is unresponsive.
void set_nodelay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// ---------------------------------------------------------------------------
// Connection state machine
//
//  CONNECTING  – backend TCP handshake in progress; client data is buffered
//  ACTIVE      – both sides open; normal bidirectional proxy
//  HALF_CLOSED – one side sent FIN; keep draining the other direction
//  CLOSING     – tear everything down
// ---------------------------------------------------------------------------
enum class ConnState { CONNECTING, ACTIVE, HALF_CLOSED, CLOSING };

struct Connection {
    int        client_fd;
    int        backend_fd;
    ConnState  state;
    int        epfd;
    Backend*   backend;

    // c2b: bytes read from client,  waiting to be written to backend
    // b2c: bytes read from backend, waiting to be written to client
    char   c2b_buf[BUFFER_SIZE];
    size_t c2b_start = 0, c2b_end = 0;
    char   b2c_buf[BUFFER_SIZE];
    size_t b2c_start = 0, b2c_end = 0;

    // Half-close tracking: which side has sent FIN already
    bool client_eof  = false;
    bool backend_eof = false;

    Connection(int cfd, int bfd, int ep, Backend* b)
        : client_fd(cfd), backend_fd(bfd), epfd(ep), backend(b),
          state(ConnState::CONNECTING) {}

    ~Connection() {
        epoll_ctl(epfd, EPOLL_CTL_DEL, client_fd,  nullptr);
        epoll_ctl(epfd, EPOLL_CTL_DEL, backend_fd, nullptr);
        close(client_fd);
        close(backend_fd);
        if (backend) backend->active_connections--;
        std::cout << "Connection closed: client=" << client_fd
                  << " backend=" << backend_fd << "\n";
    }

    void close_connection() { state = ConnState::CLOSING; }

    // -----------------------------------------------------------------------
    // try_write(fd)
    //   Writing TO fd -> drain the buffer destined for fd:
    //     backend_fd  <- c2b_buf   (data from client going to backend)
    //     client_fd   <- b2c_buf   (data from backend going to client)
    // -----------------------------------------------------------------------
    void try_write(int fd) {
        char*   buf;
        size_t* start;
        size_t* end;
        if (fd == backend_fd) { buf = c2b_buf; start = &c2b_start; end = &c2b_end; }
        else                  { buf = b2c_buf; start = &b2c_start; end = &b2c_end; }

        while (*start < *end) {
            ssize_t n = write(fd, buf + *start, *end - *start);
            if (n > 0) {
                *start += n;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                close_connection();
                return;
            }
        }
        if (*start >= *end) {
            *start = *end = 0;
            // Buffer fully drained: propagate any pending half-close.
            if (fd == backend_fd && client_eof) {
                shutdown(backend_fd, SHUT_WR);
            } else if (fd == client_fd && backend_eof) {
                shutdown(client_fd, SHUT_WR);
                close_connection();
            }
        }
    }

    // -----------------------------------------------------------------------
    // append_to_buffer(fd, data, n)
    //   Reading FROM fd -> store into the buffer that carries data away from fd:
    //     client_fd  -> c2b_buf   (will be forwarded to backend)
    //     backend_fd -> b2c_buf   (will be forwarded to client)
    // -----------------------------------------------------------------------
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

    // -----------------------------------------------------------------------
    // has_data(fd) -- is there data buffered TO WRITE to fd?
    //   backend_fd is fed from c2b_buf
    //   client_fd  is fed from b2c_buf
    // -----------------------------------------------------------------------
    bool has_data(int fd) const {
        if (fd == backend_fd) return c2b_end > c2b_start;
        else                  return b2c_end > b2c_start;
    }

    // Re-arm the peer of fd for EPOLLOUT so buffered bytes get flushed.
    void arm_peer_write(int fd) {
        int peer = (fd == client_fd) ? backend_fd : client_fd;
        if (has_data(peer)) {
            epoll_event e{};
            e.events  = EPOLLIN | EPOLLOUT | EPOLLET;
            e.data.fd = peer;
            epoll_ctl(epfd, EPOLL_CTL_MOD, peer, &e);
        }
    }
};

void remove_connection(Connection* conn, std::map<int, Connection*>& m) {
    if (!conn) return;
    m.erase(conn->client_fd);
    m.erase(conn->backend_fd);
    delete conn;
}

// ---------------------------------------------------------------------------
// Health checker
// ---------------------------------------------------------------------------
void backend_health_checker() {
    while (true) {
        for (auto& b : backends) {
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            struct timeval tv{ .tv_sec = 2, .tv_usec = 0 };
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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

// ---------------------------------------------------------------------------
// Worker thread  (one per CPU core, all sharing SO_REUSEPORT)
// ---------------------------------------------------------------------------
void worker_thread() {
    thread_local std::map<int, Connection*> fd_to_conn;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(LISTEN_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd,  (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind");   return; }
    if (listen(listen_fd, 1024) < 0)                          { perror("listen"); return; }

    set_nonblocking(listen_fd);
    int epfd = epoll_create1(0);

    // Level-triggered on listen_fd: EPOLLET here can silently drop new
    // connections under burst load when a wakeup is missed.
    epoll_event ev{};
    ev.events  = EPOLLIN;   // NOT EPOLLET
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    epoll_event events[MAX_EVENTS];
    char        buffer[BUFFER_SIZE];

    std::cout << "Worker started, port " << LISTEN_PORT << "\n";

    while (true) {
        int n_ready = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n_ready < 0) { if (errno == EINTR) continue; perror("epoll_wait"); break; }

        for (int i = 0; i < n_ready; ++i) {
            int      fd       = events[i].data.fd;
            uint32_t ev_flags = events[i].events;

            // ----------------------------------------------------------------
            // New incoming client
            // ----------------------------------------------------------------
            if (fd == listen_fd) {
                while (true) {
                    int cfd = accept(listen_fd, nullptr, nullptr);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept"); break;
                    }

                    set_nonblocking(cfd);
                    set_nodelay(cfd);

                    Backend* backend = select_backend();
                    if (!backend) {
                        std::cerr << "No healthy backend\n";
                        close(cfd);
                        continue;
                    }

                    int bfd = socket(AF_INET, SOCK_STREAM, 0);
                    set_nonblocking(bfd);
                    set_nodelay(bfd);

                    sockaddr_in baddr{};
                    baddr.sin_family = AF_INET;
                    baddr.sin_port   = htons(backend->port);
                    inet_pton(AF_INET, backend->ip.c_str(), &baddr.sin_addr);

                    int ret = connect(bfd, (sockaddr*)&baddr, sizeof(baddr));
                    if (ret < 0 && errno != EINPROGRESS) {
                        perror("backend connect");
                        close(cfd); close(bfd);
                        continue;
                    }

                    backend->active_connections++;

                    Connection* conn = new Connection(cfd, bfd, epfd, backend);
                    fd_to_conn[cfd] = conn;
                    fd_to_conn[bfd] = conn;

                    // client_fd: watch for reads immediately so the HTTP request
                    // is buffered in c2b_buf while the backend handshake is pending.
                    epoll_event e1{};
                    e1.events  = EPOLLIN | EPOLLET;
                    e1.data.fd = cfd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &e1);

                    // backend_fd: EPOLLOUT fires when the handshake completes
                    // (or immediately for a synchronous connect on loopback).
                    epoll_event e2{};
                    e2.events  = EPOLLOUT | EPOLLET;
                    e2.data.fd = bfd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, bfd, &e2);

                    // connect() can succeed synchronously on loopback.
                    // Transition to ACTIVE immediately so the pending client
                    // data isn't gated on a CONNECTING check that never clears.
                    if (ret == 0) conn->state = ConnState::ACTIVE;

                    std::cout << "New client=" << cfd << " backend=" << bfd
                              << (ret == 0 ? " (sync)" : " (async)") << "\n";
                }
                continue;
            }

            // ----------------------------------------------------------------
            // Existing connection event
            // ----------------------------------------------------------------
            auto it = fd_to_conn.find(fd);
            if (it == fd_to_conn.end()) continue;
            Connection* conn = it->second;

            // ----------------------------------------------------------------
            // Backend handshake complete
            // ----------------------------------------------------------------
            if (conn->state == ConnState::CONNECTING &&
                fd == conn->backend_fd &&
                (ev_flags & EPOLLOUT))
            {
                int err = 0; socklen_t len = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                    std::cerr << "Backend connect failed: " << strerror(err) << "\n";
                    remove_connection(conn, fd_to_conn);
                    continue;
                }

                conn->state = ConnState::ACTIVE;

                // Switch to EPOLLIN. If the client already sent data during the
                // handshake it's sitting in c2b_buf — add EPOLLOUT to flush it.
                uint32_t flags = EPOLLIN | EPOLLET;
                if (conn->has_data(fd)) flags |= EPOLLOUT;

                epoll_event e{};
                e.events  = flags;
                e.data.fd = fd;
                epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &e);
                continue;
            }

            bool removed = false;

            // ----------------------------------------------------------------
            // Readable
            // ----------------------------------------------------------------
            if (!removed && (ev_flags & EPOLLIN)) {
                bool active = (conn->state == ConnState::ACTIVE ||
                               conn->state == ConnState::HALF_CLOSED);
                while (true) {
                    ssize_t n = read(fd, buffer, sizeof(buffer));
                    if (n > 0) {
                        conn->append_to_buffer(fd, buffer, (size_t)n);
                        if (active) conn->arm_peer_write(fd);

                        bool full = (fd == conn->client_fd  && conn->c2b_end >= BUFFER_SIZE) ||
                                    (fd == conn->backend_fd && conn->b2c_end >= BUFFER_SIZE);
                        if (full) break;

                    } else if (n == 0) {
                        // Peer closed write side (FIN). Don't hard-close yet:
                        // the response body may still be in flight.
                        if (fd == conn->client_fd) {
                            conn->client_eof = true;
                            if (conn->c2b_end == conn->c2b_start)
                                shutdown(conn->backend_fd, SHUT_WR);
                        } else {
                            conn->backend_eof = true;
                            if (conn->b2c_end == conn->b2c_start) {
                                shutdown(conn->client_fd, SHUT_WR);
                                conn->close_connection();
                            }
                        }
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        remove_connection(conn, fd_to_conn);
                        removed = true;
                        break;
                    }
                }
            }

            // ----------------------------------------------------------------
            // Writable
            // ----------------------------------------------------------------
            if (!removed && (ev_flags & EPOLLOUT)) {
                conn->try_write(fd);
                if (!conn->has_data(fd)) {
                    epoll_event e{};
                    e.events  = EPOLLIN | EPOLLET;
                    e.data.fd = fd;
                    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &e);
                }
            }

            // ----------------------------------------------------------------
            // Deferred cleanup
            // ----------------------------------------------------------------
            if (!removed && conn->state == ConnState::CLOSING) {
                remove_connection(conn, fd_to_conn);
            }
        }
    }

    close(listen_fd);
    close(epfd);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    init_backends();
    std::thread(backend_health_checker).detach();

    unsigned int workers = std::thread::hardware_concurrency();
    if (workers == 0) workers = 2;

    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (unsigned int i = 0; i < workers; ++i)
        pool.emplace_back(worker_thread);

    for (auto& t : pool) t.join();
    return 0;
}
