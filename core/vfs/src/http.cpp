// http.cpp — see http.h
#include "rlvfs/http.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>

namespace rlvfs {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

}  // namespace

std::string url_encode_path(const std::string& path) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size() + 8);
    for (unsigned char c : path) {
        if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == '+' || c == ':' ||
            c == '@' || c == ',' || c == '=' || c == '(' || c == ')' || c == '!' || c == '\'' || c == '*') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

HttpClient::HttpClient(std::string host, uint16_t port, int timeout_ms)
    : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms) {}

HttpClient::~HttpClient() { disconnect(); }

void HttpClient::disconnect() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    rbuf_.clear();
}

bool HttpClient::connect() {
    if (fd_ >= 0) return true;
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port_);
    if (getaddrinfo(host_.c_str(), portstr, &hints, &res) != 0 || !res) return false;
    int fd = -1;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv;
        tv.tv_sec = timeout_ms_ / 1000;
        tv.tv_usec = (timeout_ms_ % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return false;
    fd_ = fd;
    return true;
}

bool HttpClient::send_all(const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
#ifdef MSG_NOSIGNAL
        ssize_t n = ::send(fd_, data.data() + off, data.size() - off, MSG_NOSIGNAL);
#else
        ssize_t n = ::send(fd_, data.data() + off, data.size() - off, 0);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        off += (size_t)n;
    }
    return true;
}

bool HttpClient::read_response(HttpResponse& resp, bool want_body) {
    // Read headers.
    size_t hdr_end = std::string::npos;
    for (;;) {
        std::string_view sv((const char*)rbuf_.data(), rbuf_.size());
        hdr_end = sv.find("\r\n\r\n");
        if (hdr_end != std::string::npos) break;
        if (rbuf_.size() > 64 * 1024) { resp.error = "headers too large"; return false; }
        uint8_t tmp[16384];
        ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            resp.error = std::string("recv: ") + strerror(errno);
            return false;
        }
        if (n == 0) { resp.error = "connection closed"; return false; }
        bytes_received_ += (uint64_t)n;
        rbuf_.insert(rbuf_.end(), tmp, tmp + n);
    }
    std::string head((const char*)rbuf_.data(), hdr_end);
    rbuf_.erase(rbuf_.begin(), rbuf_.begin() + (long)hdr_end + 4);

    // Status line.
    size_t sp = head.find(' ');
    if (sp == std::string::npos) { resp.error = "bad status line"; return false; }
    resp.status = atoi(head.c_str() + sp + 1);
    size_t pos = head.find("\r\n");
    while (pos != std::string::npos) {
        size_t next = head.find("\r\n", pos + 2);
        std::string line = head.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = lower(line.substr(0, colon));
            size_t v = colon + 1;
            while (v < line.size() && line[v] == ' ') v++;
            resp.headers[k] = line.substr(v);
        }
        pos = next;
    }
    bool close_after = lower(resp.headers["connection"]) == "close";
    uint64_t clen = 0;
    bool has_len = false;
    auto it = resp.headers.find("content-length");
    if (it != resp.headers.end()) { clen = strtoull(it->second.c_str(), nullptr, 10); has_len = true; }
    if (resp.headers.count("transfer-encoding")) { resp.error = "chunked encoding not supported"; disconnect(); return false; }
    if (!want_body || resp.status == 204 || resp.status == 304) {
        if (close_after) disconnect();
        return true;
    }
    if (!has_len) {
        // Read to EOF.
        for (;;) {
            uint8_t tmp[16384];
            ssize_t n = ::recv(fd_, tmp, sizeof tmp, 0);
            if (n < 0) { if (errno == EINTR) continue; break; }
            if (n == 0) break;
            bytes_received_ += (uint64_t)n;
            rbuf_.insert(rbuf_.end(), tmp, tmp + n);
        }
        resp.body.swap(rbuf_);
        rbuf_.clear();
        disconnect();
        return true;
    }
    resp.body.resize(clen);
    size_t have = std::min<size_t>(rbuf_.size(), clen);
    if (have) memcpy(resp.body.data(), rbuf_.data(), have);
    rbuf_.erase(rbuf_.begin(), rbuf_.begin() + (long)have);
    while (have < clen) {
        ssize_t n = ::recv(fd_, resp.body.data() + have, clen - have, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            resp.error = std::string("recv body: ") + strerror(errno);
            disconnect();
            return false;
        }
        if (n == 0) { resp.error = "connection closed in body"; disconnect(); return false; }
        bytes_received_ += (uint64_t)n;
        have += (size_t)n;
    }
    if (close_after) disconnect();
    return true;
}

bool HttpClient::request(const std::string& method, const std::string& path, const std::string& extra_headers,
                         HttpResponse& resp) {
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host_ + "\r\nUser-Agent: rlvfs/1\r\n" +
                      "Connection: keep-alive\r\n" + extra_headers + "\r\n";
    for (int attempt = 0; attempt < 2; attempt++) {
        resp = HttpResponse{};
        if (!connect()) { resp.error = "connect failed"; return false; }
        requests_++;
        if (!send_all(req)) { disconnect(); continue; }
        if (read_response(resp, method != "HEAD")) return true;
        disconnect();
        // retry once (stale keep-alive)
    }
    if (resp.error.empty()) resp.error = "request failed";
    return false;
}

bool HttpClient::get(const std::string& path, HttpResponse& resp, uint64_t range_off, uint64_t range_len) {
    std::string extra;
    if (range_len > 0) {
        char buf[96];
        snprintf(buf, sizeof buf, "Range: bytes=%llu-%llu\r\n", (unsigned long long)range_off,
                 (unsigned long long)(range_off + range_len - 1));
        extra = buf;
    }
    return request("GET", path, extra, resp);
}

bool HttpClient::head(const std::string& path, HttpResponse& resp) { return request("HEAD", path, "", resp); }

}  // namespace rlvfs
