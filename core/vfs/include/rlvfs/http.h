// http.h — minimal blocking HTTP/1.1 client (POSIX sockets, keep-alive,
// Range requests) for the laptop assets server. No TLS, LAN only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace rlvfs {

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;  // lower-case keys
    std::vector<uint8_t> body;
    std::string error;                            // non-empty on transport failure
};

class HttpClient {
public:
    HttpClient(std::string host, uint16_t port, int timeout_ms = 15000);
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // GET path; if range_len > 0 sends "Range: bytes=off-(off+len-1)".
    // Retries once on a stale keep-alive connection. Returns false on
    // transport failure (resp.error set); HTTP errors are in resp.status.
    bool get(const std::string& path, HttpResponse& resp, uint64_t range_off = 0, uint64_t range_len = 0);
    bool head(const std::string& path, HttpResponse& resp);

    const std::string& host() const { return host_; }
    uint16_t port() const { return port_; }
    uint64_t bytes_received() const { return bytes_received_; }
    uint64_t requests() const { return requests_; }

private:
    bool connect();
    void disconnect();
    bool send_all(const std::string& data);
    bool request(const std::string& method, const std::string& path, const std::string& extra_headers,
                 HttpResponse& resp);
    bool read_response(HttpResponse& resp, bool want_body);

    std::string host_;
    uint16_t port_;
    int timeout_ms_;
    int fd_ = -1;
    std::vector<uint8_t> rbuf_;
    uint64_t bytes_received_ = 0;
    uint64_t requests_ = 0;
};

// Percent-encode a path for the request line (keeps '/', encodes spaces etc.).
std::string url_encode_path(const std::string& path);

}  // namespace rlvfs
