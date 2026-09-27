// pipe.h — pipe2(): a 64 KB ring shared by a read end and a write end.
// Reads block until data or EOF (no writers left); writes block until space
// or fail with EPIPE when no reader is left (the caller raises SIGPIPE).
// O_NONBLOCK on either end gives EAGAIN instead of blocking.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "fds.h"
#include "waitq.h"

namespace rlk {

class Pipe {
public:
    static constexpr size_t kCapacity = 65536;
    static constexpr size_t kPipeBuf = 4096;  // writes up to this size are atomic

    std::mutex mu;
    std::vector<uint8_t> ring = std::vector<uint8_t>(kCapacity);
    size_t head = 0, count = 0;
    int readers = 0, writers = 0;
    WaitQueue rq, wq;   // blocked readers / writers
    uint64_t ino;

    Pipe();
    size_t space() const { return kCapacity - count; }
    void push(const uint8_t* p, size_t n);
    size_t pop(uint8_t* p, size_t n);
};

class PipeFile final : public OpenFile {
public:
    PipeFile(std::shared_ptr<Pipe> pipe, bool write_end);
    ~PipeFile() override;
    int64_t read(void* buf, size_t len) override;
    int64_t write(const void* buf, size_t len) override;
    int fstat(lx::stat& st) override;
    int64_t ioctl(unsigned req, uint64_t arg) override;
    // poll(): readable = data or EOF; writable = space or no reader.
    bool readable();
    bool writable();
    std::shared_ptr<Pipe> pipe() { return pipe_; }
    bool is_write_end() const { return write_end_; }

private:
    std::shared_ptr<Pipe> pipe_;
    bool write_end_;
};

// Both ends; flags: lx::o_nonblock / lx::o_cloexec are applied by the caller.
void MakePipe(std::shared_ptr<PipeFile>& rd, std::shared_ptr<PipeFile>& wr);

}  // namespace rlk
