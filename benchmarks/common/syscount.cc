//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// Counts the system calls a benchmark makes, at the libc boundary: every
// socket, wait and sleep call the process issues goes through one of the
// wrappers below and bumps one counter. run.sh preloads this library, and
// the harness reads the counter around each measured case, so a row can say
// what a message costs in kernel entries for every library alike.
//
// Only calls that reach libc's wrappers are seen; what libc issues on its
// own (futex under a mutex, for one) is not.
//

#include <poll.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <dlfcn.h>

namespace {

std::atomic<unsigned long long> g_syscalls{0};

}  // namespace

extern "C" unsigned long long znet_bench_syscalls() {
  return g_syscalls.load(std::memory_order_relaxed);
}

// one wrapper per call: count, then hand over to the real one
#define ZNET_COUNTED(ret, name, params, args)                           \
  extern "C" ret name params {                                          \
    static const auto real =                                            \
        reinterpret_cast<ret(*) params>(dlsym(RTLD_NEXT, #name));       \
    g_syscalls.fetch_add(1, std::memory_order_relaxed);                 \
    return real args;                                                   \
  }

ZNET_COUNTED(ssize_t, send, (int fd, const void* buf, size_t len, int flags),
             (fd, buf, len, flags))
ZNET_COUNTED(ssize_t, sendto,
             (int fd, const void* buf, size_t len, int flags,
              const struct sockaddr* addr, socklen_t addr_len),
             (fd, buf, len, flags, addr, addr_len))
ZNET_COUNTED(ssize_t, sendmsg, (int fd, const struct msghdr* msg, int flags),
             (fd, msg, flags))
ZNET_COUNTED(ssize_t, recv, (int fd, void* buf, size_t len, int flags),
             (fd, buf, len, flags))
ZNET_COUNTED(ssize_t, recvfrom,
             (int fd, void* buf, size_t len, int flags, struct sockaddr* addr,
              socklen_t* addr_len),
             (fd, buf, len, flags, addr, addr_len))
ZNET_COUNTED(ssize_t, recvmsg, (int fd, struct msghdr* msg, int flags),
             (fd, msg, flags))
ZNET_COUNTED(ssize_t, read, (int fd, void* buf, size_t len), (fd, buf, len))
ZNET_COUNTED(ssize_t, write, (int fd, const void* buf, size_t len),
             (fd, buf, len))
ZNET_COUNTED(ssize_t, readv, (int fd, const struct iovec* iov, int count),
             (fd, iov, count))
ZNET_COUNTED(ssize_t, writev, (int fd, const struct iovec* iov, int count),
             (fd, iov, count))
ZNET_COUNTED(int, poll, (struct pollfd* fds, nfds_t count, int timeout),
             (fds, count, timeout))
ZNET_COUNTED(int, epoll_wait,
             (int epfd, struct epoll_event* events, int max, int timeout),
             (epfd, events, max, timeout))
ZNET_COUNTED(int, select,
             (int n, fd_set* r, fd_set* w, fd_set* e, struct timeval* timeout),
             (n, r, w, e, timeout))
ZNET_COUNTED(int, nanosleep,
             (const struct timespec* req, struct timespec* rem), (req, rem))
ZNET_COUNTED(int, clock_nanosleep,
             (clockid_t clock, int flags, const struct timespec* req,
              struct timespec* rem),
             (clock, flags, req, rem))
ZNET_COUNTED(int, sched_yield, (void), ())
