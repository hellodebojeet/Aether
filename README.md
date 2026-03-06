# High-Performance Multithreaded HTTP/HTTPS Proxy Server in C

[![Language](https://img.shields.io/badge/language-C%20(C11)-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)](https://www.kernel.org/)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

A production-grade HTTP/HTTPS forward proxy written in pure C using POSIX sockets, POSIX threads, and a custom LRU cache — capable of tunneling real-world HTTPS traffic to sites like Google, YouTube, and GitHub.

---

## Project Overview

This project is a fully functional forward proxy server built from scratch in C. It intercepts HTTP and HTTPS requests from clients, forwards them to origin servers, and returns the responses — optionally caching them for future reuse.

**Why this exists:**  
Most proxy implementations rely on high-level frameworks. This project implements the full proxy protocol stack at the socket level, including HTTP/1.1 message parsing, HTTPS CONNECT tunneling, HTTP-compliant caching with revalidation, concurrent threading, and structured logging — with no external dependencies beyond the POSIX standard library.

**What it solves:**
- Full HTTP and HTTPS proxying with real-world websites
- Efficient concurrent request handling via a thread-per-client model capped by semaphores
- HTTP-compliant LRU caching to reduce origin load and latency
- Access control, authentication, and rate limiting for secure deployment

---

## Features

### Core Proxy
| Feature | Description |
|---|---|
| HTTP forwarding | Parses and forwards GET requests to any HTTP origin |
| HTTPS CONNECT tunneling | Handles `CONNECT host:443` to create a transparent TCP tunnel for TLS traffic |
| HTTP/1.1 response handling | Correctly reads `Content-Length` and `Transfer-Encoding: chunked` responses |
| Streaming | Forwards each buffer to the client as it arrives, without buffering the full body |

### Caching
| Feature | Description |
|---|---|
| LRU in-memory cache | O(1) cache lookup via linked list with mutex-protected LRU eviction |
| Cache-Control support | Respects `no-store`, `private`, and `max-age` directives |
| ETag validation | Stores `ETag` headers; sends `If-None-Match` on stale cache hits |
| Last-Modified validation | Stores `Last-Modified`; sends `If-Modified-Since` on stale cache hits |
| 304 Not Modified | On server `304`, serves cached content without downloading full body |
| Expiry tracking | Computes `expiry_time` from `max-age` or `Expires` header |

### Security
| Feature | Description |
|---|---|
| IP access control | Allow/deny list — permits `127.0.0.1` and `192.168.1.0/24`; returns `403 Forbidden` otherwise |
| Proxy authentication | Validates `Proxy-Authorization: Basic` header; returns `407` if missing or wrong |
| Per-IP rate limiting | Sliding-window limiter: 100 requests/minute per client IP; returns `429 Too Many Requests` |

### Observability
| Feature | Description |
|---|---|
| Structured access log | Timestamped log line per request: IP, method, URL, status, bytes, latency, cache status |
| Error log | Logs upstream failures, socket errors, and parse failures |
| Metrics endpoint | `GET /proxy-status` returns JSON with request count, active connections, cache ratios |
| Latency tracking | Per-request wall-clock latency measured with `clock_gettime(CLOCK_MONOTONIC)` |

---

## Architecture

```
┌──────────────────────────────────────────┐
│              Client (Browser)            │
└────────────────────┬─────────────────────┘
                     │ HTTP or HTTPS CONNECT
                     ▼
┌──────────────────────────────────────────┐
│           Proxy Server (port 8080)       │
│                                          │
│  main()                                  │
│  ├── bind + listen on port               │
│  ├── accept() in infinite loop           │
│  └── pthread_create(thread_fn) per conn  │
│                                          │
│  thread_fn()                             │
│  ├── IP access check (is_allowed_ip)     │
│  ├── Rate limit check (check_rate_limit) │
│  ├── Receive HTTP request                │
│  ├── ParsedRequest_parse()               │
│  │                                       │
│  ├── GET /proxy-status → JSON metrics    │
│  ├── Proxy-Authorization check           │
│  │                                       │
│  ├── CONNECT → tunnel_data(poll loop)    │
│  │     └── transparent TCP tunnel       │
│  │                                       │
│  └── GET → cache lookup → handle_request │
│        ├── Cache HIT → serve from cache  │
│        └── Cache MISS                    │
│              ├── inject If-None-Match    │
│              ├── connect to origin       │
│              ├── stream response client │
│              └── add_cache_element       │
│                                          │
│  LRU Cache (mutex-protected linked list) │
│  Semaphore (MAX_CLIENTS=400 concurrent)  │
└────────────────────┬─────────────────────┘
                     │ TCP (port 80/443)
                     ▼
┌──────────────────────────────────────────┐
│            Origin Server                 │
└──────────────────────────────────────────┘
```

**Key design decisions:**
- **Thread-per-connection**: Simple to reason about; semaphore prevents unbounded thread creation.
- **LRU eviction**: When the cache is full, the least-recently-used entry is evicted before inserting a new one.
- **poll() for tunneling**: Non-blocking bidirectional byte relay avoids deadlock on blocking reads.
- **Streaming first**: Responses are forwarded to the client as they arrive; buffering only happens for cacheable responses.

---

## Project Structure

```
Multithreaded-Proxy-Web-Server/
│
├── proxy_server_with_cache.c   # Main proxy: sockets, threads, cache, tunnel, auth
├── proxy_parse.c               # HTTP/1.1 request parser (GET + CONNECT)
├── proxy_parse.h               # Parser structs and function declarations
│
├── CMakeLists.txt              # CMake build configuration
├── Makefile                    # Legacy Makefile (gcc)
│
├── build/                      # CMake build output
│   └── proxy                   # Compiled binary
│
├── access.log                  # Per-request access log (created at runtime)
└── error.log                   # Upstream/socket error log (created at runtime)
```

---

##  Build Instructions

### Requirements
- Linux (any modern kernel)
- GCC ≥ 9 or Clang ≥ 10
- CMake ≥ 3.10 (recommended) **or** plain GCC

### CMake (recommended)

```bash
git clone https://github.com/yourusername/Multithreaded-Proxy-Web-Server.git
cd Multithreaded-Proxy-Web-Server
mkdir -p build && cd build
cmake ..
make
```

### GCC (direct)

```bash
gcc -D_GNU_SOURCE -std=c11 -g -Wall -o proxy proxy_server_with_cache.c proxy_parse.c -lpthread
```

---

##  Running the Proxy

```bash
./build/proxy <port>

# Example
./build/proxy 8080
```

**Browser config:**  
Set your browser or system proxy to:
```
HTTP Proxy:  localhost
Port:        8080
```

All HTTP and HTTPS traffic will be forwarded through the proxy.

> **Default credentials:** `user:pass`  
> Send as `Proxy-Authorization: Basic dXNlcjpwYXNz`

---

##  Testing

### HTTP forwarding
```bash
curl -x http://localhost:8080 -U "user:pass" http://example.com
```

### HTTPS tunneling
```bash
curl -x http://localhost:8080 -U "user:pass" https://www.google.com
curl -x http://localhost:8080 -U "user:pass" https://github.com
curl -x http://localhost:8080 -U "user:pass" https://www.youtube.com
```

### Rate limit test (trips 100 req/min limit)
```bash
for i in $(seq 1 105); do
  curl -s -o /dev/null -x http://localhost:8080 -U "user:pass" http://example.com
done
# Requests 101+ return HTTP 429
```

### Access denied test
```bash
# From a non-allowlisted IP, the proxy returns 403
curl -x http://localhost:8080 http://example.com
```

---

##  Metrics Endpoint

```bash
curl -x http://localhost:8080 -U "user:pass" http://localhost:8080/proxy-status
```

**Response:**
```json
{
  "requests": 12450,
  "connections": 8,
  "cache_hits": 5230,
  "cache_misses": 7220,
  "cache_hit_ratio": 0.42,
  "cache_size": 52428800
}
```

Also accessible at: `http://localhost:8080/status`

---

##  Logging

### access.log
One line per completed request:
```
2026-03-06T14:10:22Z 127.0.0.1 GET /index.html 200 15432 120ms MISS
2026-03-06T14:10:25Z 127.0.0.1 GET /index.html 200 15432 2ms HIT
2026-03-06T14:11:01Z 127.0.0.1 CONNECT www.google.com 200 0 890ms MISS
```

Fields: `timestamp client_ip method url status_code bytes latency cache_status`

### error.log
```
2026-03-06T14:10:23Z No such host exists. api.example.com
2026-03-06T14:10:44Z Error in connecting ! origin.example.com
```

---

##  Performance Notes

- **Concurrency**: Up to 400 simultaneous client connections via semaphore (`MAX_CLIENTS = 400`)
- **Cache capacity**: 200 MB total (`MAX_SIZE = 200 MB`); max single entry 10 MB (`MAX_ELEMENT_SIZE`)
- **LRU eviction**: O(n) scan on eviction (n = cached entries); suitable for hundreds of entries
- **poll()-based tunneling**: Single syscall per iteration, handles bidirectional HTTPS byte relay efficiently
- **Streaming**: Response bytes are sent to the client on each `recv()` call rather than buffered to completion

---

##  Limitations

| Limitation | Notes |
|---|---|
| HTTP/2 not supported | Only HTTP/1.1 is parsed; HTTP/2 responses pass through CONNECT tunnels opaquely |
| TLS inspection not supported | CONNECT tunnels are fully transparent — no MITM/SSL inspection |
| `epoll` not implemented | Uses one thread per connection rather than an event-driven model |
| Memory-only cache | Cache is lost on restart; no disk persistence |
| LRU eviction is O(n) | Fine for development; for production at scale, use a hash map + doubly-linked list |
| Hardcoded credentials | Authentication config is compiled in; no config file support yet |

---

##  Future Improvements

- [ ] `epoll`-based event loop for C10K-scale concurrency
- [ ] Disk-backed persistent cache (SQLite or `mmap`)
- [ ] HTTP/2 protocol support via `nghttp2`
- [ ] Config file for credentials, allowlist, and rate limits
- [ ] Connection pooling to origin servers (keep-alive upstream)
- [ ] Prometheus-compatible metrics export
- [ ] TLS MITM proxy with dynamic certificate generation (for inspection use cases)
- [ ] Load balancing across multiple upstream servers

---

##  Learning Outcomes

This project demonstrates proficiency in:

| Area | What's shown |
|---|---|
| Low-level networking | Raw POSIX socket creation, binding, listening, `accept()`, `recv()`, `send()` |
| HTTP protocol | Manual HTTP/1.1 request/response parsing, header extraction, chunked encoding |
| Multithreading in C | `pthread_create`, `pthread_mutex`, `sem_t` semaphore synchronization |
| Systems programming | Memory management, safe string handling, file I/O, `clock_gettime` |
| Caching systems | LRU eviction, HTTP Cache-Control compliance, ETag/Last-Modified revalidation |
| Security | IP filtering, Basic Auth validation, per-client sliding-window rate limiting |
| Proxy architecture | CONNECT tunneling, bidirectional `poll()` relay, transparent TLS forwarding |

---

## 📄 License

```
MIT License

Copyright (c) 2026

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
