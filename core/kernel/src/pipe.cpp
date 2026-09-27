// pipe.cpp — see pipe.h.
#include "pipe.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "linux_abi.h"
#include "poll.h"

namespace rlk {

namespace {
std::atomic<uint64_t> g_pipe_ino {0x50000};

// Blocks the calling guest thread (or a private waiter on app threads).
Waiter::Result block(Waiter*& w, Waiter& local, uint64_t gen) {
    (void)local;
    return w->wait(gen, 0);
}
}  // namespace

Pipe::Pipe() : ino(g_pipe_ino.fetch_add(1)) {}

void Pipe::push(const uint8_t* p, size_t n) {
    size_t tail = (head + count) % kCapacity;
    size_t first = std::min(n, kCapacity - tail);
    memcpy(ring.data() + tail, p, first);
    if (n > first) memcpy(ring.data(), p + first, n - first);
    count += n;
}

size_t Pipe::pop(uint8_t* p, size_t n) {
    n = std::min(n, count);
    size_t first = std::min(n, kCapacity - head);
    memcpy(p, ring.data() + head, first);
    if (n > first) memcpy(p + first, ring.data(), n - first);
    head = (head + n) % kCapacity;
    count -= n;
    return n;
}

PipeFile::PipeFile(std::shared_ptr<Pipe> pipe, bool write_end) : pipe_(std::move(pipe)), write_end_(write_end) {
    std::lock_guard<std::mutex> lk(pipe_->mu);
    if (write_end_) {
        pipe_->writers++;
    } else {
        pipe_->readers++;
    }
    path = "pipe:[" + std::to_string(pipe_->ino) + "]";
    oflags = write_end_ ? lx::o_wronly : lx::o_rdonly;
}

PipeFile::~PipeFile() {
    {
        std::lock_guard<std::mutex> lk(pipe_->mu);
        if (write_end_) {
            pipe_->writers--;
        } else {
            pipe_->readers--;
        }
    }
    // The other side may be waiting for exactly this: EOF or EPIPE.
    pipe_->rq.wake();
    pipe_->wq.wake();
}

int64_t PipeFile::read(void* buf, size_t len) {
    if (write_end_) return -lx::ebadf;
    if (len == 0) return 0;
    Waiter local;
    Waiter* w = CurrentWaiter() ? CurrentWaiter() : &local;
    for (;;) {
        pipe_->rq.add(w);
        const uint64_t gen = w->prepare();
        {
            std::lock_guard<std::mutex> lk(pipe_->mu);
            if (pipe_->count > 0) {
                size_t n = pipe_->pop(static_cast<uint8_t*>(buf), len);
                pipe_->rq.remove(w);
                pipe_->wq.wake();
                return (int64_t)n;
            }
            if (pipe_->writers == 0) {
                pipe_->rq.remove(w);
                return 0;  // EOF
            }
            if (oflags & lx::o_nonblock) {
                pipe_->rq.remove(w);
                return -lx::eagain;
            }
        }
        auto r = block(w, local, gen);
        pipe_->rq.remove(w);
        if (r == Waiter::Result::Interrupted) return -lx::eintr;
    }
}

int64_t PipeFile::write(const void* buf, size_t len) {
    if (!write_end_) return -lx::ebadf;
    if (len == 0) return 0;
    Waiter local;
    Waiter* w = CurrentWaiter() ? CurrentWaiter() : &local;
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t done = 0;
    for (;;) {
        pipe_->wq.add(w);
        const uint64_t gen = w->prepare();
        {
            std::lock_guard<std::mutex> lk(pipe_->mu);
            if (pipe_->readers == 0) {
                pipe_->wq.remove(w);
                return done ? (int64_t)done : -lx::epipe;
            }
            const size_t want = len - done;
            // POSIX: a write of at most PIPE_BUF bytes is not interleaved
            // with others — wait for room for all of it; larger writes go in
            // chunks as space appears.
            const bool atomic = want <= Pipe::kPipeBuf;
            size_t room = pipe_->space();
            if (room > 0 && (!atomic || room >= want)) {
                size_t n = std::min(room, want);
                pipe_->push(p + done, n);
                done += n;
                pipe_->wq.remove(w);
                pipe_->rq.wake();
                if (done == len) return (int64_t)done;
                continue;
            }
            if (oflags & lx::o_nonblock) {
                pipe_->wq.remove(w);
                return done ? (int64_t)done : -lx::eagain;
            }
        }
        auto r = block(w, local, gen);
        pipe_->wq.remove(w);
        if (r == Waiter::Result::Interrupted) return done ? (int64_t)done : -lx::eintr;
    }
}

int PipeFile::fstat(lx::stat& st) {
    FillStat(st, lx::s_ififo | 0600, 0, 0, pipe_->ino);
    return 0;
}

int64_t PipeFile::ioctl(unsigned req, uint64_t arg) {
    if (req == lx::fionread && arg) {
        std::lock_guard<std::mutex> lk(pipe_->mu);
        *reinterpret_cast<int32_t*>(arg) = (int32_t)pipe_->count;
        return 0;
    }
    if (req == lx::fionbio && arg) {
        if (*reinterpret_cast<const int32_t*>(arg)) {
            oflags |= lx::o_nonblock;
        } else {
            oflags &= ~lx::o_nonblock;
        }
        return 0;
    }
    return -lx::enotty;
}

unsigned PipeFile::poll(PollTable* pt) {
    if (pt) pt->add(write_end_ ? pipe_->wq : pipe_->rq);
    std::lock_guard<std::mutex> lk(pipe_->mu);
    unsigned m = 0;
    if (write_end_) {
        if (pipe_->readers == 0) m |= lx::pollerr;
        if (pipe_->space() > 0) m |= lx::pollout;
    } else {
        if (pipe_->count > 0) m |= lx::pollin;
        if (pipe_->writers == 0) m |= lx::pollhup | (pipe_->count > 0 ? 0u : lx::pollin);
    }
    return m;
}

bool PipeFile::readable() {
    std::lock_guard<std::mutex> lk(pipe_->mu);
    return pipe_->count > 0 || pipe_->writers == 0;
}

bool PipeFile::writable() {
    std::lock_guard<std::mutex> lk(pipe_->mu);
    return pipe_->space() > 0 || pipe_->readers == 0;
}

void MakePipe(std::shared_ptr<PipeFile>& rd, std::shared_ptr<PipeFile>& wr) {
    auto p = std::make_shared<Pipe>();
    rd = std::make_shared<PipeFile>(p, false);
    wr = std::make_shared<PipeFile>(p, true);
}

// ---- current waiter (thread-local) ---------------------------------------------

namespace {
thread_local Waiter* t_waiter = nullptr;
}

Waiter* CurrentWaiter() {
    return t_waiter;
}

void SetCurrentWaiter(Waiter* w) {
    t_waiter = w;
}

}  // namespace rlk
