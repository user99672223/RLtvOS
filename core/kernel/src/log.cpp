// log.cpp — kernel log sink.
#include "log.h"

#include <sys/time.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef __APPLE__
#include <stdlib.h>
#else
#include <sys/random.h>
#endif

#include "linux_abi.h"

namespace rlk {

double NowMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

std::string NormalizePath(std::string_view path) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') i++;
        size_t j = i;
        while (j < path.size() && path[j] != '/') j++;
        std::string_view c = path.substr(i, j - i);
        if (c.empty() || c == ".") {
        } else if (c == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.emplace_back(c);
        }
        i = j;
    }
    std::string out;
    for (auto& p : parts) {
        out += '/';
        out += p;
    }
    return out.empty() ? "/" : out;
}

void FillRandom(void* buf, size_t len) {
#ifdef __APPLE__
    arc4random_buf(buf, len);
#else
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done > 256 ? 256 : len - done;
        if (getentropy(p + done, chunk) != 0) {
            memset(p + done, 0x5a, len - done);  // never happens on Linux; keep going
            return;
        }
        done += chunk;
    }
#endif
}

static LogFn g_sink = nullptr;
static std::mutex g_mu;

void SetLogSink(LogFn fn) {
    g_sink = fn;
}

void LogV(const char* fmt, va_list ap) {
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_sink) {
        g_sink(buf);
    } else {
        fprintf(stderr, "%s\n", buf);
    }
}

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

std::string JsonEscape(std::string_view s) {
    static const char hex[] = "0123456789abcdef";
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    o += "\\u00";
                    o += hex[c >> 4];
                    o += hex[c & 15];
                } else {
                    o += (char)c;
                }
        }
    }
    return o;
}

std::string StraceStr(const void* data, size_t len, size_t max) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    std::string o;
    const size_t n = len < max ? len : max;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = p[i];
        switch (c) {
            case '\n': o += "\\n"; break;
            case '\t': o += "\\t"; break;
            case '\r': o += "\\r"; break;
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            default:
                if (c < 0x20 || c >= 0x7f) {
                    char b[6];
                    snprintf(b, sizeof b, "\\%o", c);
                    o += b;
                } else {
                    o += (char)c;
                }
        }
    }
    if (len > max) o += "\"...";
    return o;
}

const char* ErrnoName(int err) {
    switch (err) {
        case lx::eperm: return "EPERM";
        case lx::enoent: return "ENOENT";
        case lx::esrch: return "ESRCH";
        case lx::eintr: return "EINTR";
        case lx::eio: return "EIO";
        case lx::enxio: return "ENXIO";
        case lx::e2big: return "E2BIG";
        case lx::enoexec: return "ENOEXEC";
        case lx::ebadf: return "EBADF";
        case lx::echild: return "ECHILD";
        case lx::eagain: return "EAGAIN";
        case lx::enomem: return "ENOMEM";
        case lx::eacces: return "EACCES";
        case lx::efault: return "EFAULT";
        case lx::ebusy: return "EBUSY";
        case lx::eexist: return "EEXIST";
        case lx::exdev: return "EXDEV";
        case lx::enodev: return "ENODEV";
        case lx::enotdir: return "ENOTDIR";
        case lx::eisdir: return "EISDIR";
        case lx::einval: return "EINVAL";
        case lx::enfile: return "ENFILE";
        case lx::emfile: return "EMFILE";
        case lx::enotty: return "ENOTTY";
        case lx::efbig: return "EFBIG";
        case lx::enospc: return "ENOSPC";
        case lx::espipe: return "ESPIPE";
        case lx::erofs: return "EROFS";
        case lx::emlink: return "EMLINK";
        case lx::epipe: return "EPIPE";
        case lx::erange: return "ERANGE";
        case lx::edeadlk: return "EDEADLK";
        case lx::enametoolong: return "ENAMETOOLONG";
        case lx::enosys: return "ENOSYS";
        case lx::enotempty: return "ENOTEMPTY";
        case lx::eloop: return "ELOOP";
        case lx::eopnotsupp: return "EOPNOTSUPP";
        case lx::eafnosupport: return "EAFNOSUPPORT";
        case lx::etimedout: return "ETIMEDOUT";
        case lx::enotsock: return "ENOTSOCK";
        case lx::econnrefused: return "ECONNREFUSED";
        default: {
            static thread_local char buf[16];
            snprintf(buf, sizeof buf, "E%d", err);
            return buf;
        }
    }
}

}  // namespace rlk
