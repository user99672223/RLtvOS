// vfs_test.cpp — end-to-end test of rlvfs against laptop/assets_server.py.
// Builds and runs on the Linux REPO box (no TV needed):
//   cmake -S core -B build/host -DRL_BUILD_TESTS=ON && cmake --build build/host && build/host/vfs_test
// It creates a temp tree (rootfs/prefix/game/home roots with files, symlinks,
// a 3 MB random file), starts the Python server on a free port with
// --no-hash, then checks manifest load, stat, symlink resolution, mount
// shadowing, readdir, reads across block boundaries and cache hits.
#include <dirent.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "rlvfs/blockcache.h"
#include "rlvfs/http.h"
#include "rlvfs/manifest.h"
#include "rlvfs/vfs.h"

using namespace rlvfs;

static int g_failures = 0;
#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                    \
            g_failures++;                                                                      \
        }                                                                                      \
    } while (0)

static void write_file(const std::string& p, const std::string& data, mode_t mode = 0644) {
    std::ofstream f(p, std::ios::binary);
    f.write(data.data(), (std::streamsize)data.size());
    f.close();
    chmod(p.c_str(), mode);
}

static int free_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(fd, (sockaddr*)&a, sizeof a);
    socklen_t l = sizeof a;
    getsockname(fd, (sockaddr*)&a, &l);
    int port = ntohs(a.sin_port);
    close(fd);
    return port;
}

int main(int argc, char** argv) {
    const char* repo = argc > 1 ? argv[1] : nullptr;
    if (!repo) { fprintf(stderr, "usage: vfs_test <repo_root>\n"); return 2; }
    char tmpl[] = "/tmp/rlvfs-test-XXXXXX";
    std::string tmp = mkdtemp(tmpl);
    std::string rootfs = tmp + "/rootfs", prefix = tmp + "/prefix", game = tmp + "/game", home = tmp + "/home";
    for (auto& d : {rootfs, rootfs + "/bin", rootfs + "/usr", rootfs + "/usr/bin", rootfs + "/etc", rootfs + "/prefix",
                    rootfs + "/home", rootfs + "/home/user", prefix, prefix + "/drive_c", game, home})
        mkdir(d.c_str(), 0755);
    write_file(rootfs + "/usr/bin/ls", "ELF-ls", 0755);
    write_file(rootfs + "/etc/hostname", "rltvos\n");
    symlink("usr/bin", (rootfs + "/bin").c_str());  // will fail: dir exists → use lib instead
    rmdir((rootfs + "/bin").c_str());
    symlink("usr/bin", (rootfs + "/bin").c_str());
    symlink("/etc/hostname", (rootfs + "/usr/hn").c_str());
    symlink("hn", (rootfs + "/usr/hn2").c_str());
    symlink("loop2", (rootfs + "/usr/loop1").c_str());
    symlink("loop1", (rootfs + "/usr/loop2").c_str());
    write_file(rootfs + "/prefix/shadowed", "should not be visible");
    write_file(prefix + "/drive_c/reg.txt", "WINE REGISTRY");
    write_file(home + "/.profile", "export X=1\n");
    // 3 MB + 12345 bytes of random data
    std::string big;
    big.resize(3 * 1024 * 1024 + 12345);
    std::mt19937_64 rng(42);
    for (size_t i = 0; i < big.size(); i += 8) {
        uint64_t v = rng();
        memcpy(&big[i], &v, std::min<size_t>(8, big.size() - i));
    }
    write_file(game + "/big.pak", big);
    std::string empty;
    write_file(game + "/empty", empty);

    int port = free_port();
    std::string server = std::string(repo) + "/laptop/assets_server.py";
    pid_t pid = fork();
    if (pid == 0) {
        setenv("ASSETS_DIR", tmp.c_str(), 1);
        setenv("ROOTFS_DIR", rootfs.c_str(), 1);
        setenv("WINEPREFIX_DIR", prefix.c_str(), 1);
        setenv("GAME_DIR", game.c_str(), 1);
        setenv("HOME_DIR", home.c_str(), 1);
        std::string p = std::to_string(port);
        execlp("python3", "python3", server.c_str(), "--port", p.c_str(), "--bind", "127.0.0.1", "--no-hash",
               "--rebuild", (char*)nullptr);
        _exit(127);
    }
    HttpClient http("127.0.0.1", (uint16_t)port, 5000);
    HttpResponse resp;
    bool up = false;
    for (int i = 0; i < 100 && !up; i++) {
        if (http.get("/health", resp) && resp.status == 200) up = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(up);
    if (!up) { kill(pid, SIGTERM); return 1; }

    // Manifest
    CHECK(http.get("/manifest.jsonl.gz", resp) && resp.status == 200);
    Manifest m;
    std::string err;
    CHECK(m.load_gzip(resp.body.data(), resp.body.size(), err));
    if (!err.empty()) fprintf(stderr, "manifest error: %s\n", err.c_str());
    printf("manifest: %zu entries, %llu files, %llu bytes\n", m.size(), (unsigned long long)m.file_count(),
           (unsigned long long)m.total_bytes());
    CHECK(m.find("rootfs/usr/bin/ls") != nullptr);
    CHECK(m.find("rootfs/bin") && m.find("rootfs/bin")->is_symlink() && m.find("rootfs/bin")->link == "usr/bin");
    CHECK(m.find("game/big.pak") && m.find("game/big.pak")->size == big.size());
    CHECK(m.find("rootfs")->is_dir());

    std::string cache_dir = tmp + "/cache";
    BlockCache cache(cache_dir, http, 1 << 20, 2);
    Vfs vfs(m, cache);
    vfs.add_mount("/", "rootfs");
    vfs.add_mount("/prefix", "prefix");
    vfs.add_mount("/game", "game");
    vfs.add_mount("/home/user", "home");

    GuestStat st;
    CHECK(vfs.stat("/usr/bin/ls", true, st) == 0 && S_ISREG(st.mode) && st.size == 6 && (st.mode & 0777) == 0755);
    CHECK(vfs.stat("/bin/ls", true, st) == 0 && S_ISREG(st.mode));            // through symlinked dir
    CHECK(vfs.stat("/bin", false, st) == 0 && S_ISLNK(st.mode));
    CHECK(vfs.stat("/bin", true, st) == 0 && S_ISDIR(st.mode));
    CHECK(vfs.stat("/usr/hn2", true, st) == 0 && S_ISREG(st.mode) && st.size == 7);  // hn2 → hn → /etc/hostname
    CHECK(vfs.stat("/usr/loop1", true, st) == -ELOOP);
    CHECK(vfs.stat("/nope", true, st) == -ENOENT);
    CHECK(vfs.stat("/etc/hostname/x", true, st) == -ENOTDIR);
    CHECK(vfs.stat("/prefix/drive_c/reg.txt", true, st) == 0 && st.size == 13);
    CHECK(vfs.stat("/prefix/shadowed", true, st) == -ENOENT);                 // mount shadows rootfs/prefix
    CHECK(vfs.stat("/home/user/.profile", true, st) == 0 && st.size == 11);
    CHECK(vfs.stat("/game/empty", true, st) == 0 && st.size == 0);
    CHECK(vfs.stat("/", true, st) == 0 && S_ISDIR(st.mode));
    CHECK(vfs.stat("/usr/../etc/./hostname", true, st) == 0);
    std::string target;
    CHECK(vfs.readlink("/usr/hn", target) == 0 && target == "/etc/hostname");
    CHECK(vfs.readlink("/etc/hostname", target) == -EINVAL);

    std::vector<DirEnt> ents;
    CHECK(vfs.readdir("/", ents) == 0);
    bool saw_bin = false, saw_prefix = false, saw_home = false;
    for (auto& d : ents) {
        if (d.name == "bin") saw_bin = d.dtype == DT_LNK;
        if (d.name == "prefix") saw_prefix = d.dtype == DT_DIR;
        if (d.name == "home") saw_home = true;
    }
    CHECK(saw_bin && saw_prefix && saw_home);
    CHECK(vfs.readdir("/prefix", ents) == 0 && ents.size() == 3 && ents[2].name == "drive_c");
    CHECK(vfs.readdir("/etc/hostname", ents) == -ENOTDIR);

    // Reads
    const Entry* bigE = vfs.resolve("/game/big.pak", true, nullptr);
    CHECK(bigE != nullptr);
    std::vector<char> buf(big.size());
    // partial read crossing the first block boundary
    int64_t n = vfs.pread(*bigE, buf.data(), 4096, (1 << 20) - 2048, err);
    CHECK(n == 4096);
    CHECK(memcmp(buf.data(), big.data() + (1 << 20) - 2048, 4096) == 0);
    auto s1 = cache.stats();
    printf("after first read: fetch_requests=%llu fetched=%llu miss=%llu hit=%llu\n",
           (unsigned long long)s1.fetch_requests, (unsigned long long)s1.fetched_bytes,
           (unsigned long long)s1.miss_blocks, (unsigned long long)s1.hit_blocks);
    CHECK(s1.fetch_requests == 1);      // one Range request with read-ahead
    CHECK(s1.miss_blocks == 2);         // blocks 0..1 (2 blocks read-ahead window)
    // same range again: pure cache hit
    n = vfs.pread(*bigE, buf.data(), 4096, (1 << 20) - 2048, err);
    CHECK(n == 4096);
    auto s2 = cache.stats();
    CHECK(s2.fetch_requests == s1.fetch_requests);
    // whole file
    uint64_t off = 0;
    while (off < big.size()) {
        n = vfs.pread(*bigE, buf.data() + off, 300000, off, err);
        CHECK(n > 0);
        if (n <= 0) { fprintf(stderr, "pread error: %s\n", err.c_str()); break; }
        off += (uint64_t)n;
    }
    CHECK(off == big.size());
    CHECK(memcmp(buf.data(), big.data(), big.size()) == 0);
    CHECK(vfs.pread(*bigE, buf.data(), 100, big.size(), err) == 0);  // EOF
    // tail read of the last partial block
    n = vfs.pread(*bigE, buf.data(), 100000, big.size() - 5000, err);
    CHECK(n == 5000 && memcmp(buf.data(), big.data() + big.size() - 5000, 5000) == 0);
    const Entry* ls = vfs.resolve("/bin/ls", true, nullptr);
    n = vfs.pread(*ls, buf.data(), 100, 0, err);
    CHECK(n == 6 && memcmp(buf.data(), "ELF-ls", 6) == 0);
    const Entry* emptyE = vfs.resolve("/game/empty", true, nullptr);
    CHECK(vfs.pread(*emptyE, buf.data(), 10, 0, err) == 0);

    // Persistence: a new cache object over the same dir must not refetch.
    {
        BlockCache cache2(cache_dir, http, 1 << 20, 2);
        n = cache2.pread(*bigE, buf.data(), 4096, 2 * (1 << 20) + 5, err);
        CHECK(n == 4096 && memcmp(buf.data(), big.data() + 2 * (1 << 20) + 5, 4096) == 0);
        CHECK(cache2.stats().fetch_requests == 0);
    }
    // Purged data file → transparent refetch.
    {
        std::string data_path = cache_dir + "/" + stable_hash_hex("game/big.pak") + ".data";
        unlink(data_path.c_str());
        BlockCache cache3(cache_dir, http, 1 << 20, 1);
        n = cache3.pread(*bigE, buf.data(), 16, 0, err);
        CHECK(n == 16 && memcmp(buf.data(), big.data(), 16) == 0);
        CHECK(cache3.stats().fetch_requests == 1);
    }

    printf("http: %llu requests, %llu bytes\n", (unsigned long long)http.requests(),
           (unsigned long long)http.bytes_received());
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
    std::string rm = "rm -rf " + tmp;
    if (g_failures == 0) system(rm.c_str());
    else fprintf(stderr, "kept %s for inspection\n", tmp.c_str());
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
