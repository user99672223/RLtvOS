// socket.h — AF_UNIX sockets: stream, dgram and seqpacket, socketpair(2),
// names in the filesystem (an overlay Sock node) and in the abstract
// namespace, fd passing (SCM_RIGHTS) and credentials (SCM_CREDENTIALS,
// SO_PEERCRED). Everything is in-process: a connection is two UnixSocket
// objects pointing at each other, each with a receive queue of messages that
// carry bytes, fds and the sender's credentials. Blocking follows waitq.h
// (interruptible by signals). Errors are -errno (Linux numbering) like the
// rest of the fd layer.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fds.h"
#include "waitq.h"

namespace rlk {

struct PollTable;
struct TmpNode;

struct SockCreds {
    int32_t pid = 0, uid = 1000, gid = 1000;
};

// A bound name: "" = unnamed, "@..." = abstract (the '@' stands for the
// leading NUL), anything else = a filesystem path.
struct SockMsg {
    std::vector<uint8_t> data;
    size_t off = 0;                              // stream: bytes already consumed
    std::vector<std::shared_ptr<OpenFile>> fds;  // SCM_RIGHTS
    SockCreds creds;                             // the sender
    std::string from;                            // dgram: the sender's name
};

// What a recv returned besides the bytes.
struct RecvInfo {
    std::vector<std::shared_ptr<OpenFile>> fds;
    SockCreds creds;
    std::string from;
    bool has_creds = false;
    bool truncated = false;   // dgram/seqpacket: the message was longer than the buffer
};

class UnixSocket : public std::enable_shared_from_this<UnixSocket> {
public:
    static constexpr size_t kDefaultBuf = 212992;  // net.core.[rw]mem_default

    UnixSocket(int type, SockCreds owner);
    ~UnixSocket();

    // ---- names ----
    // abstract: name without the leading NUL. fs: node is the Sock node the
    // caller created at `name`. -EADDRINUSE / -EINVAL.
    int bind_abstract(const std::string& name);
    int bind_path(const std::string& path, const std::shared_ptr<TmpNode>& node);
    static std::shared_ptr<UnixSocket> find_abstract(const std::string& name);
    std::string name();        // getsockname
    std::string peer_name();   // getpeername ("" when unconnected → caller returns ENOTCONN)

    // ---- connections ----
    int listen(int backlog);
    // Stream/seqpacket: connects to a listener (queues the server side for
    // accept). Dgram: sets the default destination. nonblock: -EAGAIN instead
    // of waiting for backlog room.
    int connect(const std::shared_ptr<UnixSocket>& target, bool nonblock);
    std::shared_ptr<UnixSocket> accept(bool nonblock, int& err);
    static void pair(int type, SockCreds owner, std::shared_ptr<UnixSocket>& a, std::shared_ptr<UnixSocket>& b);
    int shutdown(int how);  // lx::shut_*
    void close();           // the last open file went away

    // ---- data ----
    // to: dgram destination (nullptr = the connected peer). Blocking sends
    // send everything; nonblock returns what fitted or -EAGAIN. -EPIPE when
    // the peer is gone (the caller raises SIGPIPE unless MSG_NOSIGNAL).
    int64_t send(const uint8_t* data, size_t len, std::vector<std::shared_ptr<OpenFile>> fds, int flags,
                 const std::shared_ptr<UnixSocket>& to, bool nonblock);
    int64_t recv(uint8_t* buf, size_t len, int flags, bool nonblock, RecvInfo* info);

    // ---- state ----
    unsigned poll(PollTable* pt);
    int type() const { return type_; }
    bool is_listening();
    bool is_connected();
    SockCreds owner() const { return owner_; }
    SockCreds peer_creds();  // SO_PEERCRED (-ENOTCONN when never connected: has_peer_creds())
    bool has_peer_creds();
    int64_t queued_bytes();  // FIONREAD / SIOCINQ
    int take_error();        // SO_ERROR
    void set_error(int err);

    // options (guarded by the caller's single-threaded use or harmless races)
    bool passcred = false;
    size_t sndbuf = kDefaultBuf, rcvbuf = kDefaultBuf;
    uint64_t ino;

private:
    friend class SocketFile;
    void disconnect_locked_pair(const std::shared_ptr<UnixSocket>& peer);  // both mutexes held
    unsigned poll_locked();

    const int type_;
    const SockCreds owner_;
    std::mutex mu_;
    std::string name_;                      // bound name
    std::weak_ptr<TmpNode> node_;           // fs binding
    bool listening_ = false;
    int backlog_max_ = 0;
    std::deque<std::shared_ptr<UnixSocket>> backlog_;
    std::shared_ptr<UnixSocket> peer_;
    std::string peer_name_;
    SockCreds peer_creds_;
    bool had_peer_ = false;                 // was ever connected (peer_creds_ valid)
    bool peer_gone_ = false;                // the peer closed
    bool rd_shut_ = false, wr_shut_ = false;
    bool rx_eof_ = false;                   // the peer shut its write side or closed
    bool closed_ = false;
    int so_error_ = 0;
    std::deque<SockMsg> rx_;
    size_t rx_bytes_ = 0;
    WaitQueue rq_;   // readers, acceptors, pollers
    WaitQueue wq_;   // writers waiting for room in the peer's queue; connectors waiting for backlog room
};

// The fd-table view of a socket.
class SocketFile final : public OpenFile {
public:
    SocketFile(std::shared_ptr<UnixSocket> s, int flags);  // lx::sock_nonblock honoured
    ~SocketFile() override;
    int64_t read(void* buf, size_t len) override;
    int64_t write(const void* buf, size_t len) override;
    int fstat(lx::stat& st) override;
    int64_t ioctl(unsigned req, uint64_t arg) override;
    unsigned poll(PollTable* pt) override;
    std::shared_ptr<UnixSocket> sock() { return sock_; }
    bool nonblock() const { return (oflags & lx::o_nonblock) != 0; }

private:
    std::shared_ptr<UnixSocket> sock_;
};

}  // namespace rlk
