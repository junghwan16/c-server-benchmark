# C Server Benchmark

High-performance HTTP server implementations showcasing different I/O models and their trade-offs.

## 📋 Table of Contents
- [Overview](#overview)
- [Server Implementations](#server-implementations)
- [Performance Comparison](#performance-comparison)
- [Building & Running](#building--running)
- [Testing](#testing)
- [Benchmarks](#benchmarks)

## Overview

This project demonstrates three different approaches to building HTTP servers in C, each optimized for different scenarios:

1. **Thread-based** - Simple, multi-core friendly
2. **Select-based** - Classic async I/O 
3. **Kqueue-based** - Modern high-performance async I/O

## Server Implementations

### 🧵 Thread Server (`thread_http`)

**Model:** One thread per connection

**How it works:**
- Main thread accepts connections
- Spawns a new thread for each client
- Each thread handles one request then exits

**Advantages:**
- ✅ Simple and straightforward code
- ✅ Utilizes all CPU cores automatically
- ✅ Good for CPU-intensive operations
- ✅ Blocking I/O is natural

**Disadvantages:**
- ❌ High memory usage (256KB stack per thread)
- ❌ Thread creation overhead
- ❌ Poor scalability beyond ~1000 connections
- ❌ Context switching overhead

**Best for:** Low to medium traffic, CPU-bound operations

---

### 🔄 Select-based AIO Server (`aio_http`)

**Model:** Single-threaded event loop with `select()`

**How it works:**
- Single thread handles all connections
- Uses `select()` to monitor multiple file descriptors
- Non-blocking I/O for all operations
- State machine for each connection

**Advantages:**
- ✅ Low memory footprint
- ✅ No thread overhead
- ✅ Predictable performance
- ✅ Good for I/O-bound operations

**Disadvantages:**
- ❌ **Hard limit of ~1024 connections** (FD_SETSIZE)
- ❌ Single CPU core only
- ❌ O(n) descriptor scanning
- ❌ Complex state management

**Best for:** Light I/O-bound workloads under 1000 connections

---

### ⚡ Kqueue Server (`kqueue_http`)

**Model:** Single-threaded event loop with `kqueue` (BSD/macOS)

**How it works:**
- Single thread with kernel event notification
- Kqueue provides edge-triggered events
- Connection pooling for memory efficiency
- Zero-copy capable architecture

**Advantages:**
- ✅ **Handles 10K+ connections** easily
- ✅ O(1) event delivery (vs select's O(n))
- ✅ Lower latency than select
- ✅ Kernel does the heavy lifting
- ✅ No FD_SETSIZE limitation

**Disadvantages:**
- ❌ BSD/macOS only (not portable)
- ❌ Single CPU core only
- ❌ More complex than select
- ❌ Requires understanding of kernel events

**Best for:** High-concurrency scenarios (C10K+)

## Performance Comparison

### Key Differences: select() vs kqueue

| Feature | select() | kqueue |
|---------|----------|---------|
| **Max connections** | ~1024 (FD_SETSIZE) | System limit (65K+) |
| **Performance** | O(n) scanning | O(1) event delivery |
| **CPU usage** | High (polling all fds) | Low (kernel notifications) |
| **Latency** | Higher | Lower |
| **Memory** | Bitmap of all fds | Only active events |
| **Portability** | POSIX standard | BSD/macOS only |

### Why kqueue is better than select:

1. **No artificial limits** - select is limited by FD_SETSIZE (usually 1024)
2. **Efficient notifications** - Kernel tells you exactly which fds are ready
3. **Better scaling** - O(1) vs O(n) complexity
4. **Edge triggering** - Can detect state changes, not just states
5. **More event types** - File changes, signals, timers, user events

## Building & Running

### Prerequisites
```bash
# macOS
brew install make clang

# Linux  
sudo apt-get install build-essential
```

### Build
```bash
make clean
make all
```

### Run Servers
```bash
# Thread server (port 8080)
./build/thread_http

# Select-based AIO server (port 8080)
./build/aio_http

# Kqueue server (port 8080, macOS/BSD only)
./build/kqueue_http
```

## Testing

### Quick Test
```bash
curl http://localhost:8080/
```

### Load Testing with wrk

#### Install wrk
```bash
# macOS
brew install wrk

# Linux
git clone https://github.com/wg/wrk.git
cd wrk && make
sudo cp wrk /usr/local/bin
```

#### Basic Benchmark
```bash
# 100 concurrent connections for 30 seconds
wrk -t4 -c100 -d30s --latency http://localhost:8080/index.html
```

#### C10K Test
```bash
# First, increase system limits (macOS)
sudo sysctl kern.maxfiles=65536
sudo sysctl kern.maxfilesperproc=65536
ulimit -n 65536

# Test with 10,000 connections
# Note: You may need multiple client machines for true C10K testing
wrk -t12 -c10000 -d30s --latency http://localhost:8080/index.html

# Progressive load test
for connections in 100 500 1000 2000 5000 10000; do
    echo "Testing with $connections connections..."
    wrk -t12 -c$connections -d10s http://localhost:8080/index.html
    sleep 5
done
```

## Benchmarks

### Automated C10K Test
```bash
# Run comprehensive test for all servers
./run-wrk-c10k-test.sh
```

This script will:
1. Test each server with 100, 500, 1000, 2000, 5000, and 10000 connections
2. Measure latency, throughput, and memory usage
3. Generate comparison report

### Expected Results (C10K Test)

| Server | Max Connections | Typical Throughput | Typical P99 Latency | Memory Usage |
|--------|----------------|-------------------|-------------------|--------------|
| **Thread** | ~2,000 | 50K req/s | 100ms | 500MB+ |
| **Select** | ~1,000 | 30K req/s | 50ms | 50MB |
| **Kqueue** | 10,000+ | 100K+ req/s | 20ms | 200MB |

### Real-World Recommendations

| Scenario | Recommended Server | Why |
|----------|-------------------|-----|
| < 100 concurrent users | Thread | Simple, good latency |
| 100-1000 concurrent | Thread or Kqueue | Depends on complexity |
| 1000+ concurrent (C10K) | Kqueue | Only one that scales |
| CPU-intensive work | Thread | Multi-core utilization |
| I/O-intensive work | Kqueue | Efficient event handling |
| Quick prototype | Thread | Simplest to understand |

## Architecture Details

### Memory Model
- **Thread**: 256KB stack × num_connections
- **Select**: Fixed ~100KB + buffers
- **Kqueue**: Connection pool (configurable)

### CPU Usage
- **Thread**: All cores (OS scheduled)
- **Select**: Single core (event loop)
- **Kqueue**: Single core (event loop)

### Connection Handling
- **Thread**: Accept → Fork thread → Handle → Exit
- **Select**: Accept → Add to fd_set → Poll → Handle
- **Kqueue**: Accept → Register event → Wait → Handle

## Limitations

- **Thread server**: System thread limit (~2000 threads)
- **Select server**: FD_SETSIZE limit (~1024 connections)
- **Kqueue server**: macOS/BSD only (use epoll on Linux)

## Future Improvements

1. **io_uring** implementation for Linux
2. **Thread pool** for thread server
3. **Multi-process** kqueue with SO_REUSEPORT
4. **HTTP/2** support
5. **TLS/SSL** support

## Contributing

Feel free to submit issues and enhancement requests!

## License

MIT