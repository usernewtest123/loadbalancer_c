# ⚡ cppbalancer

A high-performance, non-blocking TCP/HTTP load balancer written in modern C++17. Built from scratch using Linux `epoll`, edge-triggered I/O, and a multi-threaded `SO_REUSEPORT` worker pool — no frameworks, no dependencies.

---

## What it does

`cppbalancer` sits in front of a pool of backend servers and distributes incoming TCP connections across them. It proxies raw bytes bidirectionally with zero-copy-friendly ring buffers, handles partial writes, graceful half-closes, and backend failover — all without blocking a thread.

```
Client ──► [cppbalancer :8181] ──► Backend A :9007
                                ──► Backend B :9003
                                ──► Backend C :6789
```

---

## Architecture

### Worker pool (`SO_REUSEPORT`)
The main function spawns one thread per CPU core. Each thread binds its own `listen_fd` to port `8181` using `SO_REUSEPORT`, meaning the OS kernel load-balances `accept()` calls across workers with no userspace coordination. No mutex. No shared accept queue. Pure parallelism.

### Epoll event loop (edge-triggered)
Each worker runs its own `epoll` instance. All sockets are `O_NONBLOCK`. Client and backend fds are both watched with `EPOLLET` (edge-triggered) for efficiency — the loop only wakes up when new data actually arrives.

> **Why not level-triggered everywhere?**  
> The `listen_fd` uses level-triggered (`EPOLLIN` without `EPOLLET`) intentionally. Edge-triggered on a listen socket can silently drop connections under burst load when a wakeup is missed.

### Connection state machine

```
CONNECTING  →  ACTIVE  →  HALF_CLOSED  →  CLOSING
```

| State | Description |
|---|---|
| `CONNECTING` | Backend TCP handshake in progress; client data is buffered in `c2b_buf` |
| `ACTIVE` | Both sides open; full bidirectional proxy |
| `HALF_CLOSED` | One peer sent FIN; draining the other direction before teardown |
| `CLOSING` | Deferred cleanup at end of event loop iteration |

### Bidirectional buffering

Each `Connection` owns two fixed-size buffers:

- `c2b_buf` — data from **client → backend** (65 KB)
- `b2c_buf` — data from **backend → client** (65 KB)

`append_to_buffer(fd, ...)` and `try_write(fd)` are intentionally symmetric — the same logic handles both directions. `EPOLLOUT` is only armed when there's buffered data to flush, and de-armed immediately after, keeping the epoll set clean.

### TCP_NODELAY
Both client and backend sockets have Nagle's algorithm disabled. Without this, the kernel holds small writes (e.g. a lone HTTP status line) until an ACK arrives — adding ~40 ms latency even on loopback and making browsers time out waiting for headers.

### Health checker
A background thread probes every backend with a real TCP `connect()` every 5 seconds. Unhealthy backends are atomically marked and skipped during routing. Round-robin selection walks the list and finds the next healthy backend.

---

## Build & run

### Requirements
- Linux kernel ≥ 3.9 (for `SO_REUSEPORT`)
- GCC ≥ 9 or Clang ≥ 10
- C++17

### Compile
```bash
g++ -O2 -std=c++17 -pthread -o cppbalancer main.cpp
```

### Run
```bash
./cppbalancer
# Workers start on port 8181, one per CPU core
```

### Configure backends
Edit `init_backends()` in `main.cpp`:
```cpp
void init_backends() {
    backends.emplace_back(std::make_unique<Backend>("127.0.0.1", 9007));
    backends.emplace_back(std::make_unique<Backend>("127.0.0.1", 9003));
    backends.emplace_back(std::make_unique<Backend>("127.0.0.1", 6789));
}
```

---

## Key design decisions & tradeoffs

| Decision | Reasoning |
|---|---|
| `epoll` over `select`/`poll` | O(1) event delivery regardless of fd count; essential for high connection counts |
| Edge-triggered I/O | Fewer wakeups under load; requires draining loops but reduces syscall overhead |
| `SO_REUSEPORT` multi-thread | Eliminates the thundering-herd problem of a shared accept queue; each core is independent |
| Fixed-size stack buffers | Avoids heap allocation per connection; predictable memory layout |
| `TCP_NODELAY` on both sides | Eliminates Nagle-induced latency for HTTP and other request-response protocols |
| Deferred `close_connection()` | Avoids freeing `conn` mid-event-loop iteration and using a dangling pointer |
| Level-triggered listen fd | Safer under accept bursts; edge-triggered listen can miss connections |

---

## Limitations & future work

- **Round-robin only** — no weighted, least-connections, or consistent-hash routing
- **No TLS** — termination would require OpenSSL/BoringSSL integration
- **No config file** — backends are hardcoded; could add TOML/YAML or hot-reload via signal
- **Buffer overflow drop** — if a backend is slower than the client sends, bytes are silently dropped at the buffer boundary; a proper implementation would apply backpressure
- **No observability** — no metrics endpoint, no structured logging

---

## File structure

```
.
└── cppbalancer.cpp          # Everything: backends, connection state, worker loop, health checker
```

---

## License

MIT
