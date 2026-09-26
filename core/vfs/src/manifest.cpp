// manifest.cpp — see manifest.h
#include "rlvfs/manifest.h"

#include <zlib.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace rlvfs {

const Entry* Entry::child(std::string_view n) const {
    auto it = std::lower_bound(children.begin(), children.end(), n,
                               [](const Entry* e, std::string_view v) { return e->name < v; });
    if (it != children.end() && (*it)->name == n) return *it;
    return nullptr;
}

bool gunzip(const uint8_t* data, size_t n, std::string& out, std::string& err) {
    z_stream strm;
    memset(&strm, 0, sizeof strm);
    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) { err = "inflateInit2 failed"; return false; }
    strm.next_in = const_cast<Bytef*>(data);
    strm.avail_in = (uInt)n;
    out.clear();
    out.reserve(n * 6);
    char buf[1 << 16];
    int rc;
    do {
        strm.next_out = (Bytef*)buf;
        strm.avail_out = sizeof buf;
        rc = inflate(&strm, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            err = std::string("inflate: ") + (strm.msg ? strm.msg : "error") + " rc=" + std::to_string(rc);
            inflateEnd(&strm);
            return false;
        }
        out.append(buf, sizeof buf - strm.avail_out);
    } while (rc != Z_STREAM_END);
    inflateEnd(&strm);
    return true;
}

namespace {

// Parses a JSON string starting at s[i] == '"'; appends decoded UTF-8 to out;
// returns index after the closing quote or npos on error.
size_t parse_json_string(std::string_view s, size_t i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return std::string_view::npos;
    i++;
    out.clear();
    while (i < s.size()) {
        char c = s[i];
        if (c == '"') return i + 1;
        if (c != '\\') { out.push_back(c); i++; continue; }
        if (i + 1 >= s.size()) return std::string_view::npos;
        char e = s[i + 1];
        i += 2;
        switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (i + 4 > s.size()) return std::string_view::npos;
                unsigned cp = (unsigned)strtoul(std::string(s.substr(i, 4)).c_str(), nullptr, 16);
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                    unsigned lo = (unsigned)strtoul(std::string(s.substr(i + 2, 4)).c_str(), nullptr, 16);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                if (cp < 0x80) out.push_back((char)cp);
                else if (cp < 0x800) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
                else if (cp < 0x10000) {
                    out.push_back((char)(0xE0 | (cp >> 12)));
                    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back((char)(0xF0 | (cp >> 18)));
                    out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back((char)(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default: return std::string_view::npos;
        }
    }
    return std::string_view::npos;
}

size_t skip_ws(std::string_view s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) i++;
    return i;
}

}  // namespace

Entry* Manifest::add(std::string path, EntryType type) {
    entries_.emplace_back();
    Entry* e = &entries_.back();
    e->path = std::move(path);
    e->type = type;
    size_t slash = e->path.rfind('/');
    e->name = slash == std::string::npos ? std::string_view(e->path) : std::string_view(e->path).substr(slash + 1);
    by_path_[std::string_view(e->path)] = e;
    if (slash == std::string::npos) {
        roots_.push_back(e->path);
    } else {
        Entry* parent = ensure_dir(std::string_view(e->path).substr(0, slash));
        e->parent = parent;
        // keep children sorted (manifest is sorted per directory; insertion
        // at the end is the common case)
        auto& ch = parent->children;
        if (ch.empty() || ch.back()->name < e->name) ch.push_back(e);
        else {
            auto it = std::lower_bound(ch.begin(), ch.end(), e->name,
                                       [](const Entry* x, std::string_view v) { return x->name < v; });
            ch.insert(it, e);
        }
    }
    return e;
}

Entry* Manifest::ensure_dir(std::string_view path) {
    auto it = by_path_.find(path);
    if (it != by_path_.end()) return it->second;
    Entry* e = add(std::string(path), EntryType::Dir);
    e->mode = 0755;
    return e;
}

bool Manifest::parse_line(std::string_view line, std::string& err) {
    size_t i = skip_ws(line, 0);
    if (i >= line.size()) return true;
    if (line[i] != '{') { err = "line " + std::to_string(line_no_) + ": expected {"; return false; }
    i++;
    std::string p, t, h, l, key, sval;
    uint64_t size = 0, rdev = 0;
    uint32_t mode = 0;
    int64_t mtime = 0;
    for (;;) {
        i = skip_ws(line, i);
        if (i < line.size() && line[i] == '}') break;
        if (i < line.size() && line[i] == ',') { i++; continue; }
        size_t j = parse_json_string(line, i, key);
        if (j == std::string_view::npos) { err = "line " + std::to_string(line_no_) + ": bad key"; return false; }
        i = skip_ws(line, j);
        if (i >= line.size() || line[i] != ':') { err = "line " + std::to_string(line_no_) + ": expected :"; return false; }
        i = skip_ws(line, i + 1);
        if (i < line.size() && line[i] == '"') {
            j = parse_json_string(line, i, sval);
            if (j == std::string_view::npos) { err = "line " + std::to_string(line_no_) + ": bad string"; return false; }
            i = j;
            if (key == "p") p = sval;
            else if (key == "t") t = sval;
            else if (key == "h") h = sval;
            else if (key == "l") l = sval;
        } else {
            size_t start = i;
            while (i < line.size() && (isdigit((unsigned char)line[i]) || line[i] == '-' || line[i] == '.' ||
                                       line[i] == 'e' || line[i] == 'E' || line[i] == '+' || isalpha((unsigned char)line[i])))
                i++;
            std::string num(line.substr(start, i - start));
            if (key == "s") size = strtoull(num.c_str(), nullptr, 10);
            else if (key == "m") mode = (uint32_t)strtoul(num.c_str(), nullptr, 10);
            else if (key == "mt") mtime = strtoll(num.c_str(), nullptr, 10);
            else if (key == "rdev") rdev = strtoull(num.c_str(), nullptr, 10);
        }
    }
    if (p.empty()) { err = "line " + std::to_string(line_no_) + ": missing p"; return false; }
    EntryType type;
    switch (t.empty() ? 'f' : t[0]) {
        case 'f': type = EntryType::File; break;
        case 'd': type = EntryType::Dir; break;
        case 'l': type = EntryType::Symlink; break;
        case 'c': type = EntryType::CharDev; break;
        case 'b': type = EntryType::BlockDev; break;
        case 'p': type = EntryType::Fifo; break;
        case 's': type = EntryType::Socket; break;
        default: type = EntryType::File; break;
    }
    Entry* e;
    auto it = by_path_.find(std::string_view(p));
    if (it != by_path_.end()) {
        e = it->second;  // synthesised dir now described properly
        e->type = type;
    } else {
        e = add(std::move(p), type);
    }
    e->mode = mode & 07777;
    e->size = type == EntryType::File ? size : 0;
    e->mtime = mtime;
    e->rdev = rdev;
    e->hash = std::move(h);
    e->link = std::move(l);
    if (type == EntryType::File) { total_bytes_ += e->size; file_count_++; }
    return true;
}

bool Manifest::load_text(std::string_view text, std::string& err) {
    size_t pos = 0;
    line_no_ = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        line_no_++;
        if (!parse_line(line, err)) return false;
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return true;
}

bool Manifest::load_gzip(const uint8_t* data, size_t n, std::string& err) {
    std::string text;
    if (!gunzip(data, n, text, err)) return false;
    return load_text(text, err);
}

const Entry* Manifest::find(std::string_view path) const {
    auto it = by_path_.find(path);
    return it == by_path_.end() ? nullptr : it->second;
}

}  // namespace rlvfs
