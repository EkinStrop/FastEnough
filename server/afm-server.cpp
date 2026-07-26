// AFM Server - pushed to /data/local/tmp and executed via adb shell
// Provides high-speed file transfer over TCP, bypassing ADB sync protocol.
// Now with MCRAW container support for virtual directory browsing.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <pthread.h>
#include <sys/sendfile.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#ifdef __aarch64__
#include <arm_acle.h>  // ARMv8 CRC32 intrinsics
#endif

// C++ includes for MCRAW support
#include <string>
#include <vector>
#include <sstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <deque>
#include <thread>
#include <atomic>
#include <mutex>
#include <fstream>

#include <motioncam/Decoder.hpp>
#include <nlohmann/json.hpp>

#define TINY_DNG_WRITER_IMPLEMENTATION
#include <tinydng/tiny_dng_writer.h>
#undef TINY_DNG_WRITER_IMPLEMENTATION

#include "protocol.h"

#define BUF_SIZE AFM_CHUNK_SIZE
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

static int g_running = 1;

// --- Helpers ---

static void tune_socket(int fd) {
    int bufsize = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
}

static void close_socket_on_exec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int send_all(int fd, const void* buf, size_t len) {
    const char* p = (const char*)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static int recv_all(int fd, void* buf, size_t len) {
    char* p = (char*)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static int send_msg(int fd, uint32_t cmd, const void* payload, uint32_t len) {
    MsgHeader hdr = { cmd, len };
    if (send_all(fd, &hdr, sizeof(hdr)) < 0) return -1;
    if (len > 0 && send_all(fd, payload, len) < 0) return -1;
    return 0;
}

static int send_error(int fd, const char* msg) {
    return send_msg(fd, RSP_ERROR, msg, strlen(msg));
}

static int send_ok(int fd, const void* data, uint32_t len) {
    return send_msg(fd, RSP_OK, data, len);
}

// Stream a memory buffer using the RSP_DATA/RSP_DONE chunked protocol (same as handle_pull)
static int stream_buffer(int fd, const char* data, uint64_t total_size) {
    // Send OK with file size (PullHeader)
    PullHeader ph = { total_size };
    if (send_ok(fd, &ph, sizeof(ph)) < 0) return -1;

    uint64_t remaining = total_size;
    uint64_t offset = 0;

    while (remaining > 0) {
        size_t chunk = remaining > BUF_SIZE ? BUF_SIZE : (size_t)remaining;
        MsgHeader hdr;
        hdr.cmd = RSP_DATA;
        hdr.length = (uint32_t)chunk;
        if (send_all(fd, &hdr, sizeof(hdr)) < 0) return -1;
        if (send_all(fd, data + offset, chunk) < 0) return -1;
        offset += chunk;
        remaining -= chunk;
    }

    return send_msg(fd, RSP_DONE, NULL, 0);
}

// --- Command handlers ---

static void handle_ping(int fd) {
    struct { uint32_t magic; uint32_t version; uint32_t effective_uid; } resp = {
        AFM_MAGIC, AFM_VERSION, (uint32_t)geteuid()
    };
    send_ok(fd, &resp, sizeof(resp));
}

static void handle_storage(int fd) {
    const char* candidates[] = {
        "/storage/emulated/0",
        "/sdcard",
        "/storage/self/primary",
        "/data/media/0",
        NULL
    };

    const char* env = getenv("EXTERNAL_STORAGE");
    if (env && env[0] == '/') {
        char resolved[PATH_MAX];
        if (realpath(env, resolved)) {
            struct stat st;
            if (stat(resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
                send_ok(fd, resolved, strlen(resolved));
                return;
            }
        }
    }

    for (int i = 0; candidates[i]; i++) {
        char resolved[PATH_MAX];
        if (realpath(candidates[i], resolved)) {
            struct stat st;
            if (stat(resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
                send_ok(fd, resolved, strlen(resolved));
                return;
            }
        }
    }
    send_ok(fd, "/sdcard", 7);
}

static void handle_disk_space(int fd) {
    struct statvfs svfs;
    if (statvfs("/storage/emulated/0", &svfs) < 0) {
        send_error(fd, strerror(errno));
        return;
    }
    struct { uint64_t total; uint64_t free; } resp;
    resp.total = (uint64_t)svfs.f_blocks * svfs.f_frsize;
    resp.free = (uint64_t)svfs.f_bavail * svfs.f_frsize;
    send_ok(fd, &resp, sizeof(resp));
}

static void handle_list(int fd, const char* path, uint32_t path_len) {
    char pathbuf[PATH_MAX];
    if (path_len >= PATH_MAX) { send_error(fd, "Path too long"); return; }
    memcpy(pathbuf, path, path_len);
    pathbuf[path_len] = '\0';

    DIR* dir = opendir(pathbuf);
    if (!dir) {
        send_error(fd, strerror(errno));
        return;
    }

    char* buf = (char*)malloc(256 * 1024);
    size_t buf_size = 256 * 1024;
    size_t buf_used = 4;
    uint32_t count = 0;

    struct dirent* de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        char fullpath[PATH_MAX];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", pathbuf, de->d_name);

        struct stat st;
        uint8_t type = 0;
        uint64_t size = 0;
        int64_t mtime = 0;

        if (lstat(fullpath, &st) == 0) {
            if (S_ISDIR(st.st_mode)) type = 1;
            else if (S_ISLNK(st.st_mode)) type = 2;
            else type = 0;
            size = st.st_size;
            mtime = st.st_mtime;
        }

        uint32_t name_len = strlen(de->d_name);
        size_t entry_size = sizeof(FileEntryHeader) + name_len;

        while (buf_used + entry_size > buf_size) {
            buf_size *= 2;
            buf = (char*)realloc(buf, buf_size);
        }

        FileEntryHeader* eh = (FileEntryHeader*)(buf + buf_used);
        eh->type = type;
        eh->size = size;
        eh->mtime = mtime;
        eh->name_len = name_len;
        memcpy(buf + buf_used + sizeof(FileEntryHeader), de->d_name, name_len);
        buf_used += entry_size;
        count++;
    }
    closedir(dir);

    memcpy(buf, &count, 4);
    send_ok(fd, buf, buf_used);
    free(buf);
}

static void handle_stat(int fd, const char* path, uint32_t path_len) {
    char pathbuf[PATH_MAX];
    if (path_len >= PATH_MAX) { send_error(fd, "Path too long"); return; }
    memcpy(pathbuf, path, path_len);
    pathbuf[path_len] = '\0';

    struct stat st;
    StatResponse resp;
    if (lstat(pathbuf, &st) != 0) {
        resp.type = 255;
        resp.size = 0;
        resp.mtime = 0;
    } else {
        if (S_ISDIR(st.st_mode)) resp.type = 1;
        else if (S_ISLNK(st.st_mode)) resp.type = 2;
        else resp.type = 0;
        resp.size = st.st_size;
        resp.mtime = st.st_mtime;
    }
    send_ok(fd, &resp, sizeof(resp));
}

// Async send job for pull double-buffering
struct SendJob {
    int       fd;
    char*     buf;
    size_t    len;
    int       error;
    pthread_t thread;
    int       active;
};

static void* send_thread_func(void* arg) {
    SendJob* job = (SendJob*)arg;
    if (send_all(job->fd, job->buf, job->len) < 0) {
        job->error = 1;
    }
    return NULL;
}

// --- CRC32 implementation ---
// Uses ARM hardware CRC32 instructions on ARMv8 (with runtime detection),
// falls back to table lookup otherwise.

static uint32_t crc32_table[256];
static int crc32_table_init = 0;

static void init_crc32_table(void) {
    if (crc32_table_init) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_init = 1;
}

#ifdef __aarch64__
#include <sys/auxv.h>
#ifndef HWCAP_CRC32
#define HWCAP_CRC32 (1 << 7)
#endif
static int g_has_arm_crc = -1;
static int check_arm_crc(void) {
    if (g_has_arm_crc < 0) {
        unsigned long hwcap = getauxval(AT_HWCAP);
        g_has_arm_crc = (hwcap & HWCAP_CRC32) ? 1 : 0;
    }
    return g_has_arm_crc;
}
#endif

static void crc32_update_raw(uint32_t* crc, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
#ifdef __aarch64__
    if (check_arm_crc()) {
        while (len >= 8) {
            uint64_t val;
            memcpy(&val, p, 8);
            *crc = __crc32d(*crc, val);
            p += 8;
            len -= 8;
        }
        while (len-- > 0)
            *crc = __crc32b(*crc, *p++);
        return;
    }
#endif
    init_crc32_table();
    for (size_t i = 0; i < len; i++)
        *crc = crc32_table[(*crc ^ p[i]) & 0xFF] ^ (*crc >> 8);
}

static uint32_t crc32_update(uint32_t crc, const void* data, size_t len) {
    uint32_t c = ~crc;
    crc32_update_raw(&c, data, len);
    return ~c;
}

// --- SHA-256 implementation for strong file comparison ---
typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} SHA256_CTX_AFM;

static const uint32_t k_sha256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define SHA256_ROTR(a,b) (((a) >> (b)) | ((a) << (32 - (b))))
#define SHA256_CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_EP0(x) (SHA256_ROTR(x,2) ^ SHA256_ROTR(x,13) ^ SHA256_ROTR(x,22))
#define SHA256_EP1(x) (SHA256_ROTR(x,6) ^ SHA256_ROTR(x,11) ^ SHA256_ROTR(x,25))
#define SHA256_SIG0(x) (SHA256_ROTR(x,7) ^ SHA256_ROTR(x,18) ^ ((x) >> 3))
#define SHA256_SIG1(x) (SHA256_ROTR(x,17) ^ SHA256_ROTR(x,19) ^ ((x) >> 10))

static void sha256_transform(SHA256_CTX_AFM* ctx, const uint8_t data[]) {
    uint32_t m[64];
    for (int i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    for (int i = 16; i < 64; ++i)
        m[i] = SHA256_SIG1(m[i - 2]) + m[i - 7] + SHA256_SIG0(m[i - 15]) + m[i - 16];

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + SHA256_EP1(e) + SHA256_CH(e, f, g) + k_sha256[i] + m[i];
        uint32_t t2 = SHA256_EP0(a) + SHA256_MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(SHA256_CTX_AFM* ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85; ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c; ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(SHA256_CTX_AFM* ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(SHA256_CTX_AFM* ctx, uint8_t hash[32]) {
    uint32_t i = ctx->datalen;
    ctx->data[i++] = 0x80;
    if (i > 56) {
        while (i < 64) ctx->data[i++] = 0x00;
        sha256_transform(ctx, ctx->data);
        i = 0;
    }
    while (i < 56) ctx->data[i++] = 0x00;

    ctx->bitlen += ctx->datalen * 8ULL;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; ++i) {
        hash[i]      = (ctx->state[0] >> (24 - i * 8)) & 0xff;
        hash[i + 4]  = (ctx->state[1] >> (24 - i * 8)) & 0xff;
        hash[i + 8]  = (ctx->state[2] >> (24 - i * 8)) & 0xff;
        hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0xff;
        hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0xff;
        hash[i + 20] = (ctx->state[5] >> (24 - i * 8)) & 0xff;
        hash[i + 24] = (ctx->state[6] >> (24 - i * 8)) & 0xff;
        hash[i + 28] = (ctx->state[7] >> (24 - i * 8)) & 0xff;
    }
}

// CRC cache — shared by push and pull for instant CMD_CRC32 responses
static pthread_mutex_t g_crc_mutex = PTHREAD_MUTEX_INITIALIZER;
static char     g_cached_crc_path[PATH_MAX] = {};
static uint32_t g_cached_crc = 0;
static int      g_cached_crc_valid = 0;

static void handle_pull(int fd, const char* path, uint32_t path_len) {
    char pathbuf[PATH_MAX];
    if (path_len >= PATH_MAX) { send_error(fd, "Path too long"); return; }
    memcpy(pathbuf, path, path_len);
    pathbuf[path_len] = '\0';

    int file_fd = open(pathbuf, O_RDONLY);
    if (file_fd < 0) {
        int err = errno;
        fprintf(stderr, "[PULL] open failed: '%s' errno=%d (%s)\n", pathbuf, err, strerror(err));
        send_error(fd, strerror(err));
        return;
    }

    struct stat st;
    fstat(file_fd, &st);
    uint64_t file_size = st.st_size;

    PullHeader ph = { file_size };
    if (send_ok(fd, &ph, sizeof(ph)) < 0) { close(file_fd); return; }

    uint64_t remaining = file_size;
    off_t offset = 0;
    int error = 0;

    while (remaining > 0) {
        size_t chunk = remaining > BUF_SIZE ? BUF_SIZE : (size_t)remaining;

        MsgHeader hdr;
        hdr.cmd = RSP_DATA;
        hdr.length = (uint32_t)chunk;
        if (send_all(fd, &hdr, sizeof(hdr)) < 0) { error = 1; break; }

        size_t sent = 0;
        while (sent < chunk) {
            ssize_t n = sendfile(fd, file_fd, &offset, chunk - sent);
            if (n <= 0) { error = 1; break; }
            sent += n;
        }
        if (error) break;
        remaining -= chunk;
    }

    close(file_fd);
    if (!error) send_msg(fd, RSP_DONE, NULL, 0);
}

static void shell_quote(const char* in, char* out, size_t out_size) {
    size_t j = 0;
    if (j + 1 < out_size) out[j++] = '\'';
    for (size_t i = 0; in[i] && j + 5 < out_size; i++) {
        if (in[i] == '\'') {
            out[j++] = '\'';
            out[j++] = '\\';
            out[j++] = '\'';
            out[j++] = '\'';
        } else {
            out[j++] = in[i];
        }
    }
    if (j + 1 < out_size) out[j++] = '\'';
    out[j] = '\0';
}

static void handle_install_batch(int fd, const char* payload, uint32_t payload_len) {
    nlohmann::json request = nlohmann::json::parse(payload, payload + payload_len, nullptr, false);
    if (request.is_discarded() || !request.contains("root") || !request.contains("packages")) {
        send_error(fd, "Invalid install batch manifest");
        return;
    }

    std::string root = request.value("root", "");
    const std::string allowed_prefix = "/data/local/tmp/afm-install-";
    if (root.rfind(allowed_prefix, 0) != 0 || root.find("..") != std::string::npos) {
        send_error(fd, "Invalid install staging path");
        return;
    }

    struct InstallTask {
        std::vector<std::string> apks;
        bool success = false;
        std::string output;
    };
    std::vector<InstallTask> tasks;
    for (const auto& package : request["packages"]) {
        InstallTask task;
        if (!package.contains("apks") || !package["apks"].is_array()) {
            send_error(fd, "Invalid APK group");
            return;
        }
        for (const auto& value : package["apks"]) {
            if (!value.is_string()) {
                send_error(fd, "Invalid APK path");
                return;
            }
            std::string path = value.get<std::string>();
            if (path.rfind(root + "/", 0) != 0 || path.find("..") != std::string::npos) {
                send_error(fd, "APK path is outside staging directory");
                return;
            }
            struct stat st;
            if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                send_error(fd, "Staged APK is missing");
                return;
            }
            task.apks.push_back(std::move(path));
        }
        if (task.apks.empty()) {
            send_error(fd, "APK group is empty");
            return;
        }
        tasks.push_back(std::move(task));
    }

    bool reinstall = request.value("reinstall", true);
    bool grant = request.value("grant", false);
    bool downgrade = request.value("downgrade", false);
    unsigned int parallelism = std::max(1u, std::min(4u, request.value("parallelism", 3u)));
    parallelism = std::min(parallelism, (unsigned int)tasks.size());
    std::atomic<size_t> next{0};
    std::atomic<int> completed{0};
    std::mutex send_mutex;

    uint32_t task_count = (uint32_t)tasks.size();
    if (send_ok(fd, &task_count, sizeof(task_count)) < 0) return;
    auto send_event = [&](size_t index, const char* state) {
        nlohmann::json event = {
            {"index", index},
            {"state", state},
            {"success", tasks[index].success},
            {"output", tasks[index].output},
            {"completed", completed.load()}
        };
        std::string encoded = event.dump();
        std::lock_guard<std::mutex> lock(send_mutex);
        send_msg(fd, RSP_DATA, encoded.data(), encoded.size());
    };

    auto worker = [&]() {
        while (true) {
            size_t index = next.fetch_add(1);
            if (index >= tasks.size()) return;
            InstallTask& task = tasks[index];
            send_event(index, "installing");
            auto run_pm = [](const std::string& command, std::string& output) {
                FILE* pipe = popen((command + " 2>&1").c_str(), "r");
                if (!pipe) {
                    output = strerror(errno);
                    return false;
                }
                char line[1024];
                while (fgets(line, sizeof(line), pipe)) output += line;
                int status = pclose(pipe);
                return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                    output.find("Success") != std::string::npos && output.find("Failure") == std::string::npos;
            };

            if (task.apks.size() == 1) {
                std::vector<char> quoted(task.apks.front().size() * 4 + 3);
                shell_quote(task.apks.front().c_str(), quoted.data(), quoted.size());
                std::string command = "/system/bin/pm install";
                if (reinstall) command += " -r";
                if (grant) command += " -g";
                if (downgrade) command += " -d";
                command += " ";
                command += quoted.data();
                task.success = run_pm(command, task.output);
            } else {
                std::string create = "/system/bin/pm install-create";
                if (reinstall) create += " -r";
                if (grant) create += " -g";
                if (downgrade) create += " -d";
                std::string createOutput;
                bool created = run_pm(create, createOutput);
                size_t begin = createOutput.find_last_of('[');
                size_t end = createOutput.find_last_of(']');
                std::string sessionId = (begin != std::string::npos && end != std::string::npos && end > begin)
                    ? createOutput.substr(begin + 1, end - begin - 1) : "";
                task.output += createOutput;
                bool written = created && !sessionId.empty();
                for (size_t apkIndex = 0; written && apkIndex < task.apks.size(); ++apkIndex) {
                    std::vector<char> quoted(task.apks[apkIndex].size() * 4 + 3);
                    shell_quote(task.apks[apkIndex].c_str(), quoted.data(), quoted.size());
                    std::string write = "/system/bin/pm install-write " + sessionId + " split" +
                        std::to_string(apkIndex) + " " + quoted.data();
                    written = run_pm(write, task.output);
                }
                if (written) {
                    task.success = run_pm("/system/bin/pm install-commit " + sessionId, task.output);
                } else {
                    std::string ignored;
                    run_pm("/system/bin/pm install-abandon " + sessionId, ignored);
                    task.success = false;
                }
            }
            completed.fetch_add(1);
            send_event(index, "complete");
        }
    };

    std::vector<std::thread> workers;
    for (unsigned int i = 0; i < parallelism; ++i) workers.emplace_back(worker);
    for (auto& thread : workers) thread.join();

    char quoted_root[PATH_MAX * 2];
    shell_quote(root.c_str(), quoted_root, sizeof(quoted_root));
    std::string cleanup = "rm -rf ";
    cleanup += quoted_root;
    system(cleanup.c_str());

    send_msg(fd, RSP_DONE, NULL, 0);
}

static uint16_t zip_u16(const unsigned char* value) {
    return (uint16_t)value[0] | ((uint16_t)value[1] << 8);
}

static uint32_t zip_u32(const unsigned char* value) {
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
        ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

static uint64_t zip_u64(const unsigned char* value) {
    uint64_t result = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) result |= (uint64_t)(*value++) << shift;
    return result;
}

static bool safe_relative_path(const std::string& value) {
    if (value.empty() || value[0] == '/' || value[0] == '\\' || value.find('\\') != std::string::npos) return false;
    size_t begin = 0;
    while (begin <= value.size()) {
        size_t end = value.find('/', begin);
        std::string part = value.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (part == ".." || part == ".") return false;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}

static bool safe_package_name(const std::string& value) {
    if (value.empty() || value.size() > 255) return false;
    for (char c : value) {
        if (!(c == '.' || c == '_' || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    }
    return value.find("..") == std::string::npos;
}

static std::string quoted_shell(const std::string& value) {
    std::vector<char> quoted(value.size() * 4 + 3);
    shell_quote(value.c_str(), quoted.data(), quoted.size());
    return quoted.data();
}

static bool run_command_output(const std::string& command, std::string& output) {
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
    if (!pipe) {
        output = strerror(errno);
        return false;
    }
    char line[2048];
    while (fgets(line, sizeof(line), pipe)) output += line;
    int status = pclose(pipe);
    return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool make_parent_directories(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return false;
    std::string parent = path.substr(0, slash);
    std::string output;
    return run_command_output("mkdir -p " + quoted_shell(parent), output);
}

static bool parse_zip64_values(const std::vector<unsigned char>& extra,
                               bool needSize, bool needCompressed, bool needOffset,
                               uint64_t& size, uint64_t& compressed, uint64_t& offset) {
    size_t position = 0;
    while (position + 4 <= extra.size()) {
        uint16_t tag = zip_u16(extra.data() + position);
        uint16_t length = zip_u16(extra.data() + position + 2);
        position += 4;
        if (position + length > extra.size()) return false;
        if (tag == 1) {
            const unsigned char* value = extra.data() + position;
            size_t remaining = length;
            auto take = [&](uint64_t& target) {
                if (remaining < 8) return false;
                target = zip_u64(value);
                value += 8;
                remaining -= 8;
                return true;
            };
            if (needSize && !take(size)) return false;
            if (needCompressed && !take(compressed)) return false;
            if (needOffset && !take(offset)) return false;
            return true;
        }
        position += length;
    }
    return !(needSize || needCompressed || needOffset);
}

static bool extract_store_zip(const std::string& archivePath, const std::string& destination, std::string& error) {
    int archive = open(archivePath.c_str(), O_RDONLY);
    if (archive < 0) {
        error = strerror(errno);
        return false;
    }
    struct stat info{};
    if (fstat(archive, &info) != 0 || info.st_size < 22) {
        error = "Archive is too small";
        close(archive);
        return false;
    }
    uint64_t fileSize = (uint64_t)info.st_size;
    size_t tailSize = (size_t)std::min<uint64_t>(fileSize, 65557);
    std::vector<unsigned char> tail(tailSize);
    if (pread(archive, tail.data(), tail.size(), (off_t)(fileSize - tailSize)) != (ssize_t)tail.size()) {
        error = "Could not read archive index";
        close(archive);
        return false;
    }
    size_t endPosition = SIZE_MAX;
    for (size_t i = tail.size() - 22;; --i) {
        if (zip_u32(tail.data() + i) == 0x06054b50) {
            endPosition = i;
            break;
        }
        if (i == 0) break;
    }
    if (endPosition == SIZE_MAX) {
        error = "Archive index was not found";
        close(archive);
        return false;
    }
    uint64_t entryCount = zip_u16(tail.data() + endPosition + 10);
    uint64_t centralOffset = zip_u32(tail.data() + endPosition + 16);
    if (entryCount == 0xffff || centralOffset == 0xffffffffu) {
        uint64_t absoluteEnd = fileSize - tailSize + endPosition;
        unsigned char locator[20];
        if (absoluteEnd < sizeof(locator) ||
            pread(archive, locator, sizeof(locator), (off_t)(absoluteEnd - sizeof(locator))) != sizeof(locator) ||
            zip_u32(locator) != 0x07064b50) {
            error = "ZIP64 locator is invalid";
            close(archive);
            return false;
        }
        unsigned char zip64[56];
        uint64_t zip64Offset = zip_u64(locator + 8);
        if (pread(archive, zip64, sizeof(zip64), (off_t)zip64Offset) != sizeof(zip64) ||
            zip_u32(zip64) != 0x06064b50) {
            error = "ZIP64 index is invalid";
            close(archive);
            return false;
        }
        entryCount = zip_u64(zip64 + 32);
        centralOffset = zip_u64(zip64 + 48);
    }
    if (entryCount > 1000000 || centralOffset >= fileSize) {
        error = "Archive index is out of range";
        close(archive);
        return false;
    }

    std::string setupOutput;
    if (!run_command_output("rm -rf " + quoted_shell(destination) + " && mkdir -p " + quoted_shell(destination), setupOutput)) {
        error = setupOutput.empty() ? "Could not create restore staging directory" : setupOutput;
        close(archive);
        return false;
    }
    uint64_t position = centralOffset;
    std::vector<unsigned char> copyBuffer(1024 * 1024);
    for (uint64_t index = 0; index < entryCount; ++index) {
        unsigned char header[46];
        if (pread(archive, header, sizeof(header), (off_t)position) != sizeof(header) ||
            zip_u32(header) != 0x02014b50) {
            error = "Archive contains an invalid entry";
            close(archive);
            return false;
        }
        uint16_t flags = zip_u16(header + 8);
        uint16_t method = zip_u16(header + 10);
        uint32_t expectedCrc = zip_u32(header + 16);
        uint64_t compressed = zip_u32(header + 20);
        uint64_t size = zip_u32(header + 24);
        uint16_t nameLength = zip_u16(header + 28);
        uint16_t extraLength = zip_u16(header + 30);
        uint16_t commentLength = zip_u16(header + 32);
        uint64_t localOffset = zip_u32(header + 42);
        std::string name(nameLength, '\0');
        std::vector<unsigned char> extra(extraLength);
        if (pread(archive, name.data(), name.size(), (off_t)(position + 46)) != (ssize_t)name.size() ||
            pread(archive, extra.data(), extra.size(), (off_t)(position + 46 + nameLength)) != (ssize_t)extra.size()) {
            error = "Archive entry is truncated";
            close(archive);
            return false;
        }
        position += 46 + nameLength + extraLength + commentLength;
        bool needCompressed = compressed == 0xffffffffu;
        bool needSize = size == 0xffffffffu;
        bool needOffset = localOffset == 0xffffffffu;
        if ((flags & 1) || method != 0 || !safe_relative_path(name) ||
            ((needCompressed || needSize || needOffset) &&
             !parse_zip64_values(extra, needSize, needCompressed, needOffset, size, compressed, localOffset)) ||
            compressed != size || localOffset + 30 > fileSize) {
            error = "Archive entry is unsupported or unsafe";
            close(archive);
            return false;
        }
        unsigned char local[30];
        if (pread(archive, local, sizeof(local), (off_t)localOffset) != sizeof(local) ||
            zip_u32(local) != 0x04034b50) {
            error = "Archive local entry is invalid";
            close(archive);
            return false;
        }
        uint64_t dataOffset = localOffset + 30 + zip_u16(local + 26) + zip_u16(local + 28);
        if (dataOffset > fileSize || size > fileSize - dataOffset) {
            error = "Archive entry exceeds file size";
            close(archive);
            return false;
        }
        std::string outputPath = destination + "/" + name;
        if (!make_parent_directories(outputPath)) {
            error = "Could not create archive entry directory";
            close(archive);
            return false;
        }
        int output = open(outputPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (output < 0) {
            error = strerror(errno);
            close(archive);
            return false;
        }
        uint64_t copied = 0;
        uint32_t crc = 0xffffffffu;
        bool copyOk = true;
        while (copied < size) {
            size_t chunk = (size_t)std::min<uint64_t>(size - copied, copyBuffer.size());
            ssize_t readBytes = pread(archive, copyBuffer.data(), chunk, (off_t)(dataOffset + copied));
            if (readBytes != (ssize_t)chunk) {
                copyOk = false;
                break;
            }
            size_t written = 0;
            while (written < chunk) {
                ssize_t count = write(output, copyBuffer.data() + written, chunk - written);
                if (count <= 0) {
                    copyOk = false;
                    break;
                }
                written += (size_t)count;
            }
            if (!copyOk) break;
            crc32_update_raw(&crc, copyBuffer.data(), chunk);
            copied += chunk;
        }
        close(output);
        if (!copyOk || (crc ^ 0xffffffffu) != expectedCrc) {
            error = copyOk ? "Archive entry failed its integrity check" : "Could not extract archive entry";
            close(archive);
            return false;
        }
    }
    close(archive);
    return true;
}

static bool write_fd_all(int fd, const void* data, size_t length) {
    const unsigned char* value = (const unsigned char*)data;
    while (length > 0) {
        ssize_t count = write(fd, value, length);
        if (count <= 0) return false;
        value += count;
        length -= (size_t)count;
    }
    return true;
}

static bool zip_write16(int fd, uint16_t value) {
    unsigned char bytes[2] = {(unsigned char)(value & 0xff), (unsigned char)((value >> 8) & 0xff)};
    return write_fd_all(fd, bytes, sizeof(bytes));
}

static bool zip_write32(int fd, uint32_t value) {
    unsigned char bytes[4] = {
        (unsigned char)(value & 0xff), (unsigned char)((value >> 8) & 0xff),
        (unsigned char)((value >> 16) & 0xff), (unsigned char)((value >> 24) & 0xff)
    };
    return write_fd_all(fd, bytes, sizeof(bytes));
}

static bool zip_write64(int fd, uint64_t value) {
    unsigned char bytes[8];
    for (unsigned shift = 0; shift < 64; shift += 8) bytes[shift / 8] = (unsigned char)((value >> shift) & 0xff);
    return write_fd_all(fd, bytes, sizeof(bytes));
}

struct DeviceZipEntry {
    std::string diskPath;
    std::string name;
    uint64_t size = 0;
    uint64_t localOffset = 0;
    uint32_t crc = 0;
};

static bool collect_zip_files(const std::string& root, const std::string& relative,
                              std::vector<DeviceZipEntry>& files, std::string& error) {
    std::string directory = relative.empty() ? root : root + "/" + relative;
    DIR* dir = opendir(directory.c_str());
    if (!dir) {
        error = strerror(errno);
        return false;
    }
    std::vector<std::string> names;
    while (dirent* item = readdir(dir)) {
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        names.push_back(item->d_name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        std::string childRelative = relative.empty() ? name : relative + "/" + name;
        std::string diskPath = root + "/" + childRelative;
        struct stat info{};
        if (lstat(diskPath.c_str(), &info) != 0) {
            error = strerror(errno);
            return false;
        }
        if (S_ISDIR(info.st_mode)) {
            if (!collect_zip_files(root, childRelative, files, error)) return false;
        } else if (S_ISREG(info.st_mode)) {
            if (!safe_relative_path(childRelative) || childRelative.size() > 65535) {
                error = "Backup contains an invalid archive path";
                return false;
            }
            files.push_back({diskPath, childRelative, (uint64_t)info.st_size, 0, 0});
        }
    }
    return true;
}

static bool create_store_zip(const std::string& sourceRoot, const std::string& archivePath, std::string& error) {
    std::vector<DeviceZipEntry> files;
    if (!collect_zip_files(sourceRoot, "", files, error)) return false;
    int output = open(archivePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (output < 0) {
        error = strerror(errno);
        return false;
    }
    std::vector<unsigned char> buffer(1024 * 1024);
    bool ok = true;
    for (auto& entry : files) {
        entry.localOffset = (uint64_t)lseek64(output, 0, SEEK_CUR);
        ok = zip_write32(output, 0x04034b50) && zip_write16(output, 45) && zip_write16(output, 0x0800) &&
            zip_write16(output, 0) && zip_write16(output, 0) && zip_write16(output, 0) && zip_write32(output, 0) &&
            zip_write32(output, 0xffffffffu) && zip_write32(output, 0xffffffffu) &&
            zip_write16(output, (uint16_t)entry.name.size()) && zip_write16(output, 20) &&
            write_fd_all(output, entry.name.data(), entry.name.size()) && zip_write16(output, 1) &&
            zip_write16(output, 16) && zip_write64(output, entry.size) && zip_write64(output, entry.size);
        if (!ok) break;
        int input = open(entry.diskPath.c_str(), O_RDONLY);
        if (input < 0) {
            error = strerror(errno);
            ok = false;
            break;
        }
        uint32_t crc = 0xffffffffu;
        uint64_t copied = 0;
        while (copied < entry.size) {
            size_t chunk = (size_t)std::min<uint64_t>(entry.size - copied, buffer.size());
            ssize_t count = read(input, buffer.data(), chunk);
            if (count <= 0 || !write_fd_all(output, buffer.data(), (size_t)count)) {
                ok = false;
                error = strerror(errno);
                break;
            }
            crc32_update_raw(&crc, buffer.data(), (size_t)count);
            copied += (uint64_t)count;
        }
        close(input);
        if (!ok) break;
        entry.crc = crc ^ 0xffffffffu;
        off64_t end = lseek64(output, 0, SEEK_CUR);
        unsigned char crcBytes[4] = {
            (unsigned char)(entry.crc & 0xff), (unsigned char)((entry.crc >> 8) & 0xff),
            (unsigned char)((entry.crc >> 16) & 0xff), (unsigned char)((entry.crc >> 24) & 0xff)
        };
        if (pwrite64(output, crcBytes, sizeof(crcBytes), (off64_t)(entry.localOffset + 14)) != sizeof(crcBytes) ||
            lseek64(output, end, SEEK_SET) != end) {
            ok = false;
            error = strerror(errno);
            break;
        }
    }
    uint64_t centralOffset = (uint64_t)lseek64(output, 0, SEEK_CUR);
    if (ok) {
        for (const auto& entry : files) {
            ok = zip_write32(output, 0x02014b50) && zip_write16(output, 45) && zip_write16(output, 45) &&
                zip_write16(output, 0x0800) && zip_write16(output, 0) && zip_write16(output, 0) && zip_write16(output, 0) &&
                zip_write32(output, entry.crc) && zip_write32(output, 0xffffffffu) && zip_write32(output, 0xffffffffu) &&
                zip_write16(output, (uint16_t)entry.name.size()) && zip_write16(output, 28) && zip_write16(output, 0) &&
                zip_write16(output, 0) && zip_write16(output, 0) && zip_write32(output, 0) && zip_write32(output, 0xffffffffu) &&
                write_fd_all(output, entry.name.data(), entry.name.size()) && zip_write16(output, 1) &&
                zip_write16(output, 24) && zip_write64(output, entry.size) && zip_write64(output, entry.size) &&
                zip_write64(output, entry.localOffset);
            if (!ok) break;
        }
    }
    uint64_t centralSize = (uint64_t)lseek64(output, 0, SEEK_CUR) - centralOffset;
    uint64_t zip64Offset = (uint64_t)lseek64(output, 0, SEEK_CUR);
    if (ok) {
        ok = zip_write32(output, 0x06064b50) && zip_write64(output, 44) && zip_write16(output, 45) &&
            zip_write16(output, 45) && zip_write32(output, 0) && zip_write32(output, 0) &&
            zip_write64(output, files.size()) && zip_write64(output, files.size()) &&
            zip_write64(output, centralSize) && zip_write64(output, centralOffset) &&
            zip_write32(output, 0x07064b50) && zip_write32(output, 0) && zip_write64(output, zip64Offset) &&
            zip_write32(output, 1) && zip_write32(output, 0x06054b50) && zip_write16(output, 0) &&
            zip_write16(output, 0) && zip_write16(output, 0xffff) && zip_write16(output, 0xffff) &&
            zip_write32(output, 0xffffffffu) && zip_write32(output, 0xffffffffu) && zip_write16(output, 0);
    }
    if (fsync(output) != 0) ok = false;
    close(output);
    if (!ok) {
        if (error.empty()) error = strerror(errno);
        unlink(archivePath.c_str());
    }
    return ok;
}

static bool write_json_file(const std::string& path, const nlohmann::json& value, std::string& error) {
    if (!make_parent_directories(path)) {
        error = "Could not create backup metadata directory";
        return false;
    }
    std::string encoded = value.dump(2);
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || !write_fd_all(fd, encoded.data(), encoded.size())) {
        error = strerror(errno);
        if (fd >= 0) close(fd);
        return false;
    }
    close(fd);
    return true;
}

static void handle_create_backup_archive(int fd, const char* payload, uint32_t payloadLen) {
    nlohmann::json request = nlohmann::json::parse(payload, payload + payloadLen, nullptr, false);
    if (request.is_discarded() || !request.contains("apps") || !request["apps"].is_array()) {
        send_error(fd, "Invalid backup archive manifest");
        return;
    }
    std::string created = request.value("created", "");
    if (created.empty() || created.find_first_not_of("0123456789-") != std::string::npos) {
        send_error(fd, "Invalid backup timestamp");
        return;
    }
    std::string staging = "/data/local/tmp/afm-backup-staging-" + std::to_string(getpid()) + "-" + created;
    std::string archive = "/data/local/tmp/afm-backup-job-" + std::to_string(getpid()) + "-" + created + ".zip";
    std::string setupOutput;
    if (!run_command_output("rm -rf " + quoted_shell(staging) + " " + quoted_shell(archive) +
                            " && mkdir -p " + quoted_shell(staging + "/Apps"), setupOutput)) {
        send_error(fd, setupOutput.c_str());
        return;
    }
    uint32_t appCount = (uint32_t)request["apps"].size();
    if (send_ok(fd, &appCount, sizeof(appCount)) < 0) return;
    bool includeApk = request.value("includeApk", true);
    bool includeData = request.value("includeData", true);
    bool includeDe = request.value("includeDeviceProtectedData", true);
    bool includeExternal = request.value("includeExternalData", true);
    bool includeObb = request.value("includeObb", true);
    bool includeMedia = request.value("includeMedia", true);
    bool excludeCache = request.value("excludeCache", true);
    std::vector<std::string> successfulPackages;
    nlohmann::json results = nlohmann::json::array();
    std::string cleanupOutput;

    for (size_t index = 0; index < request["apps"].size(); ++index) {
        const auto& app = request["apps"][index];
        std::string packageName = app.value("packageName", "");
        std::string label = app.value("label", packageName);
        bool ok = safe_package_name(packageName);
        std::string appOutput;
        nlohmann::json metadata;
        metadata["format"] = "FastEnoughAppBackup";
        metadata["formatVersion"] = 2;
        metadata["packageName"] = packageName;
        metadata["label"] = label;
        metadata["serial"] = request.value("serial", "");
        metadata["created"] = created;
        metadata["apkFiles"] = nlohmann::json::array();
        metadata["permissions"] = nlohmann::json::array();
        metadata["paths"] = {
            {"data", "/data/data/" + packageName}, {"data_de", "/data/user_de/0/" + packageName},
            {"external", "/sdcard/Android/data/" + packageName}, {"obb", "/sdcard/Android/obb/" + packageName},
            {"media", "/sdcard/Android/media/" + packageName}
        };
        nlohmann::json has = {{"apk", false}, {"data", false}, {"data_de", false},
            {"external", false}, {"obb", false}, {"media", false}};
        std::string appRoot = staging + "/Apps/" + packageName + "/" + created;
        std::string ignored;
        if (ok) ok = run_command_output("mkdir -p " + quoted_shell(appRoot), ignored);

        nlohmann::json event = {{"index", index}, {"state", "preparing"}, {"stage", "Collecting APK and app data"},
            {"progress", 0.05f}, {"packageName", packageName}, {"completed", successfulPackages.size()}};
        std::string eventText = event.dump();
        send_msg(fd, RSP_DATA, eventText.data(), eventText.size());

        if (ok && includeApk) {
            std::string pathsOutput;
            if (!run_command_output("pm path " + quoted_shell(packageName), pathsOutput)) ok = false;
            std::istringstream lines(pathsOutput);
            std::string line;
            int apkIndex = 0;
            while (std::getline(lines, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("package:", 0) != 0) continue;
                std::string source = line.substr(8);
                size_t slash = source.find_last_of('/');
                std::string name = slash == std::string::npos ? "base.apk" : source.substr(slash + 1);
                if (!safe_relative_path(name) || name.find('/') != std::string::npos) name = "split" + std::to_string(apkIndex) + ".apk";
                std::string copyOutput;
                if (!run_command_output("cp -f " + quoted_shell(source) + " " + quoted_shell(appRoot + "/" + name), copyOutput)) {
                    ok = false;
                    appOutput += copyOutput;
                    break;
                }
                metadata["apkFiles"].push_back({{"name", name}, {"source", source}});
                apkIndex++;
            }
            has["apk"] = !metadata["apkFiles"].empty();
            if (!has["apk"].get<bool>()) ok = false;
        }

        std::string dumpsys;
        run_command_output("dumpsys package " + quoted_shell(packageName), dumpsys);
        std::istringstream permissionLines(dumpsys);
        std::string permissionLine;
        while (std::getline(permissionLines, permissionLine)) {
            size_t permissionStart = permissionLine.find("android.permission.");
            if (permissionStart == std::string::npos || permissionLine.find("granted=true") == std::string::npos) continue;
            std::string permission = permissionLine.substr(permissionStart);
            size_t end = permission.find_first_of(": ");
            if (end != std::string::npos) permission.resize(end);
            if (!permission.empty()) metadata["permissions"].push_back(permission);
        }

        auto addTar = [&](const char* key, const std::string& source, const char* filename, bool enabled) {
            if (!ok || !enabled) return;
            struct stat sourceInfo{};
            if (stat(source.c_str(), &sourceInfo) != 0 || !S_ISDIR(sourceInfo.st_mode)) return;
            std::string command = "cd " + quoted_shell(source) + " && tar -cpf " + quoted_shell(appRoot + "/" + filename);
            if (excludeCache) command += " --exclude=cache --exclude=code_cache --exclude=no_backup";
            command += " .";
            std::string tarOutput;
            if (!run_command_output(command, tarOutput)) {
                ok = false;
                appOutput += tarOutput;
                return;
            }
            has[key] = true;
        };
        addTar("data", "/data/data/" + packageName, "data.tar", includeData);
        addTar("data_de", "/data/user_de/0/" + packageName, "data_de.tar", includeDe);
        addTar("external", "/sdcard/Android/data/" + packageName, "external.tar", includeExternal);
        addTar("obb", "/sdcard/Android/obb/" + packageName, "obb.tar", includeObb);
        addTar("media", "/sdcard/Android/media/" + packageName, "media.tar", includeMedia);
        metadata["has"] = has;
        std::string metadataError;
        if (ok && !write_json_file(appRoot + "/backup.json", metadata, metadataError)) {
            ok = false;
            appOutput += metadataError;
        }
        if (ok) successfulPackages.push_back(packageName);
        else run_command_output("rm -rf " + quoted_shell(appRoot), ignored);
        results.push_back(ok);
        event = {{"index", index}, {"state", "complete"}, {"stage", ok ? "App prepared" : "Backup failed"},
            {"progress", 0.70f}, {"success", ok}, {"output", appOutput}, {"packageName", packageName},
            {"completed", successfulPackages.size()}};
        eventText = event.dump();
        send_msg(fd, RSP_DATA, eventText.data(), eventText.size());
    }

    nlohmann::json contents = nlohmann::json::array();
    if (includeApk) contents.push_back("APK");
    if (includeData) contents.push_back("Data");
    if (includeDe) contents.push_back("DE");
    if (includeExternal) contents.push_back("External");
    if (includeObb) contents.push_back("OBB");
    if (includeMedia) contents.push_back("Media");
    nlohmann::json job = {{"format", "FastEnoughBackupJob"}, {"formatVersion", 2}, {"archiveMode", "ZIP64 store"},
        {"name", "Backup job " + created}, {"created", created}, {"serial", request.value("serial", "")},
        {"appCount", successfulPackages.size()}, {"requestedAppCount", request["apps"].size()},
        {"cancelled", false}, {"packages", successfulPackages}, {"contents", contents}};
    std::string jobError;
    bool archiveOk = !successfulPackages.empty() && write_json_file(staging + "/job.json", job, jobError) &&
        create_store_zip(staging, archive, jobError);
    run_command_output("rm -rf " + quoted_shell(staging), cleanupOutput);
    if (!archiveOk) {
        send_error(fd, jobError.empty() ? "Could not create device backup archive" : jobError.c_str());
        return;
    }
    struct stat archiveInfo{};
    if (stat(archive.c_str(), &archiveInfo) != 0) {
        unlink(archive.c_str());
        send_error(fd, strerror(errno));
        return;
    }
    nlohmann::json done = {{"archive", archive}, {"size", (uint64_t)archiveInfo.st_size},
        {"results", results}, {"packages", successfulPackages}};
    std::string doneText = done.dump();
    send_msg(fd, RSP_DONE, doneText.data(), doneText.size());
}

static bool install_device_apks(const std::vector<std::string>& apks,
                                bool reinstall, bool grant, bool downgrade,
                                std::string& output) {
    if (apks.empty()) return true;
    if (apks.size() == 1) {
        std::string command = "/system/bin/pm install";
        if (reinstall) command += " -r";
        if (grant) command += " -g";
        if (downgrade) command += " -d";
        command += " " + quoted_shell(apks.front());
        return run_command_output(command, output) && output.find("Success") != std::string::npos;
    }
    std::string create = "/system/bin/pm install-create";
    if (reinstall) create += " -r";
    if (grant) create += " -g";
    if (downgrade) create += " -d";
    std::string createOutput;
    bool created = run_command_output(create, createOutput) && createOutput.find("Success") != std::string::npos;
    output += createOutput;
    size_t begin = createOutput.find_last_of('[');
    size_t end = createOutput.find_last_of(']');
    std::string session = begin != std::string::npos && end != std::string::npos && end > begin
        ? createOutput.substr(begin + 1, end - begin - 1) : "";
    if (!created || session.empty()) return false;
    for (size_t index = 0; index < apks.size(); ++index) {
        std::string writeOutput;
        std::string command = "/system/bin/pm install-write " + session + " split" +
            std::to_string(index) + " " + quoted_shell(apks[index]);
        if (!run_command_output(command, writeOutput) || writeOutput.find("Success") == std::string::npos) {
            output += writeOutput;
            std::string ignored;
            run_command_output("/system/bin/pm install-abandon " + session, ignored);
            return false;
        }
        output += writeOutput;
    }
    std::string commitOutput;
    bool committed = run_command_output("/system/bin/pm install-commit " + session, commitOutput) &&
        commitOutput.find("Success") != std::string::npos;
    output += commitOutput;
    return committed;
}

static void handle_restore_backup_archive(int fd, const char* payload, uint32_t payload_len) {
    nlohmann::json request = nlohmann::json::parse(payload, payload + payload_len, nullptr, false);
    if (request.is_discarded() || !request.contains("archive") || !request.contains("apps") || !request["apps"].is_array()) {
        send_error(fd, "Invalid backup archive restore manifest");
        return;
    }
    std::string archive = request.value("archive", "");
    if (archive.rfind("/data/local/tmp/afm-restore-job-", 0) != 0 || archive.find("..") != std::string::npos) {
        send_error(fd, "Invalid backup archive path");
        return;
    }
    std::string staging = "/data/local/tmp/afm-restore-extracted-" + std::to_string(getpid()) + "-" +
        std::to_string((unsigned long long)time(nullptr));
    std::string extractError;
    if (!extract_store_zip(archive, staging, extractError)) {
        unlink(archive.c_str());
        send_error(fd, extractError.c_str());
        return;
    }
    uint32_t appCount = (uint32_t)request["apps"].size();
    if (send_ok(fd, &appCount, sizeof(appCount)) < 0) return;

    struct RestoreTask {
        std::string prefix;
        nlohmann::json metadata;
        bool success = false;
        std::string output;
    };
    std::vector<RestoreTask> tasks;
    for (const auto& app : request["apps"]) {
        RestoreTask task;
        task.prefix = app.value("prefix", "");
        task.metadata = app.value("metadata", nlohmann::json::object());
        if (!safe_relative_path(task.prefix) || !safe_package_name(task.metadata.value("packageName", ""))) {
            send_error(fd, "Invalid app entry in restore archive");
            return;
        }
        tasks.push_back(std::move(task));
    }

    bool includeApk = request.value("includeApk", true);
    bool includeData = request.value("includeData", true);
    bool includeDe = request.value("includeDeviceProtectedData", true);
    bool includeExternal = request.value("includeExternalData", true);
    bool includeObb = request.value("includeObb", true);
    bool includeMedia = request.value("includeMedia", true);
    bool grantPermissions = request.value("grantRuntimePermissions", true);
    bool reinstall = request.value("reinstall", true);
    bool downgrade = request.value("allowDowngrade", false);
    unsigned parallelism = std::max(1u, std::min(4u, request.value("parallelism", 4u)));
    parallelism = std::min<unsigned>(parallelism, tasks.size());
    std::atomic<size_t> next{0};
    std::atomic<int> completed{0};
    std::mutex sendMutex;
    auto sendEvent = [&](size_t index, const char* state, const char* stage, float progress) {
        nlohmann::json event = {
            {"index", index}, {"state", state}, {"stage", stage},
            {"progress", progress}, {"success", tasks[index].success},
            {"output", tasks[index].output}, {"completed", completed.load()},
            {"packageName", tasks[index].metadata.value("packageName", "")}
        };
        std::string encoded = event.dump();
        std::lock_guard<std::mutex> lock(sendMutex);
        send_msg(fd, RSP_DATA, encoded.data(), encoded.size());
    };

    auto worker = [&]() {
        while (true) {
            size_t index = next.fetch_add(1);
            if (index >= tasks.size()) return;
            RestoreTask& task = tasks[index];
            const std::string packageName = task.metadata.value("packageName", "");
            const std::string appRoot = staging + "/" + task.prefix;
            const auto has = task.metadata.value("has", nlohmann::json::object());
            bool ok = true;
            sendEvent(index, "restoring", "Installing APK", 0.10f);
            if (includeApk && has.value("apk", false)) {
                std::vector<std::string> apks;
                for (const auto& apk : task.metadata.value("apkFiles", nlohmann::json::array())) {
                    std::string name = apk.value("name", "");
                    if (!safe_relative_path(name) || name.find('/') != std::string::npos) {
                        ok = false;
                        task.output += "Invalid APK entry\n";
                        break;
                    }
                    apks.push_back(appRoot + "/" + name);
                }
                std::string installOutput;
                if (ok && !install_device_apks(apks, reinstall, grantPermissions, downgrade, installOutput)) ok = false;
                task.output += installOutput;
            }
            if (ok) {
                std::string ignored;
                run_command_output("am force-stop --user 0 " + quoted_shell(packageName), ignored);
            }

            auto restoreTar = [&](const char* filename, const std::string& target, bool enabled, bool present) {
                if (!ok || !enabled || !present) return;
                std::string archiveFile = appRoot + "/" + filename;
                struct stat archiveInfo{};
                if (stat(archiveFile.c_str(), &archiveInfo) != 0 || !S_ISREG(archiveInfo.st_mode)) {
                    task.output += std::string(filename) + " is missing\n";
                    ok = false;
                    return;
                }
                std::string ownerOutput;
                bool hadOwner = run_command_output("stat -c '%u:%g' " + quoted_shell(target) + " 2>/dev/null", ownerOutput);
                ownerOutput.erase(std::remove(ownerOutput.begin(), ownerOutput.end(), '\n'), ownerOutput.end());
                ownerOutput.erase(std::remove(ownerOutput.begin(), ownerOutput.end(), '\r'), ownerOutput.end());
                if (ownerOutput.empty() || ownerOutput.find_first_not_of("0123456789:") != std::string::npos ||
                    ownerOutput.find(':') == std::string::npos) hadOwner = false;
                std::string command = "mkdir -p " + quoted_shell(target) +
                    " && rm -rf " + quoted_shell(target) + "/* " + quoted_shell(target) + "/.[!.]* " +
                    quoted_shell(target) + "/..?* 2>/dev/null || true; tar -xpf " + quoted_shell(archiveFile) +
                    " -C " + quoted_shell(target);
                std::string restoreOutput;
                if (!run_command_output(command, restoreOutput)) {
                    task.output += restoreOutput;
                    ok = false;
                    return;
                }
                if (hadOwner) {
                    std::string fixOutput;
                    run_command_output("chown -R " + ownerOutput + " " + quoted_shell(target) +
                        "; restorecon -RF " + quoted_shell(target) + " >/dev/null 2>&1 || true", fixOutput);
                }
            };

            sendEvent(index, "restoring", "Restoring app data", 0.45f);
            restoreTar("data.tar", "/data/data/" + packageName, includeData, has.value("data", false));
            restoreTar("data_de.tar", "/data/user_de/0/" + packageName, includeDe, has.value("data_de", false));
            restoreTar("external.tar", "/sdcard/Android/data/" + packageName, includeExternal, has.value("external", false));
            restoreTar("obb.tar", "/sdcard/Android/obb/" + packageName, includeObb, has.value("obb", false));
            restoreTar("media.tar", "/sdcard/Android/media/" + packageName, includeMedia, has.value("media", false));

            sendEvent(index, "restoring", "Restoring permissions", 0.90f);
            if (ok && grantPermissions && task.metadata.contains("permissions")) {
                std::string command;
                for (const auto& permission : task.metadata["permissions"]) {
                    if (!permission.is_string()) continue;
                    command += "pm grant --user 0 " + quoted_shell(packageName) + " " +
                        quoted_shell(permission.get<std::string>()) + " 2>/dev/null; ";
                }
                if (!command.empty()) {
                    std::string ignored;
                    run_command_output(command, ignored);
                }
            }
            task.success = ok;
            completed.fetch_add(1);
            sendEvent(index, "complete", ok ? "App restored" : "Restore failed", 1.0f);
        }
    };

    std::vector<std::thread> workers;
    for (unsigned i = 0; i < parallelism; ++i) workers.emplace_back(worker);
    for (auto& thread : workers) thread.join();
    std::string cleanupOutput;
    run_command_output("rm -rf " + quoted_shell(staging) + " " + quoted_shell(archive), cleanupOutput);
    send_msg(fd, RSP_DONE, NULL, 0);
}

static void stream_file_and_remove(int fd, const char* file_path) {
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        send_error(fd, strerror(errno));
        unlink(file_path);
        return;
    }
    struct stat st;
    if (fstat(file_fd, &st) != 0) {
        close(file_fd);
        unlink(file_path);
        send_error(fd, strerror(errno));
        return;
    }
    PullHeader ph = { (uint64_t)st.st_size };
    if (send_ok(fd, &ph, sizeof(ph)) < 0) {
        close(file_fd);
        unlink(file_path);
        return;
    }
    uint64_t remaining = st.st_size;
    off_t offset = 0;
    int error = 0;
    while (remaining > 0) {
        size_t chunk = remaining > BUF_SIZE ? BUF_SIZE : (size_t)remaining;
        MsgHeader hdr{ RSP_DATA, (uint32_t)chunk };
        if (send_all(fd, &hdr, sizeof(hdr)) < 0) { error = 1; break; }
        size_t sent = 0;
        while (sent < chunk) {
            ssize_t n = sendfile(fd, file_fd, &offset, chunk - sent);
            if (n <= 0) { error = 1; break; }
            sent += n;
        }
        if (error) break;
        remaining -= chunk;
    }
    close(file_fd);
    unlink(file_path);
    if (!error) send_msg(fd, RSP_DONE, NULL, 0);
}

static void handle_archive_path(int fd, const char* payload, uint32_t payload_len) {
    if (payload_len < 5) { send_error(fd, "Invalid archive payload"); return; }
    uint32_t exclude_cache = 0;
    memcpy(&exclude_cache, payload, 4);
    const char* path = payload + 4;
    uint32_t path_len = payload_len - 4;
    char pathbuf[PATH_MAX];
    if (path_len >= PATH_MAX) { send_error(fd, "Path too long"); return; }
    memcpy(pathbuf, path, path_len);
    pathbuf[path_len] = '\0';

    struct stat st;
    if (lstat(pathbuf, &st) != 0 || !S_ISDIR(st.st_mode)) {
        send_error(fd, "Source directory not found");
        return;
    }

    char tmpl[] = "/data/local/tmp/afm-backup-XXXXXX.tar";
    int tmp_fd = mkstemps(tmpl, 4);
    if (tmp_fd < 0) {
        send_error(fd, strerror(errno));
        return;
    }
    close(tmp_fd);

    char quoted_path[PATH_MAX * 2];
    char quoted_tmp[PATH_MAX * 2];
    shell_quote(pathbuf, quoted_path, sizeof(quoted_path));
    shell_quote(tmpl, quoted_tmp, sizeof(quoted_tmp));

    char cmd[PATH_MAX * 5];
    snprintf(cmd, sizeof(cmd), "cd %s && tar -cpf %s%s .", quoted_path, quoted_tmp,
             exclude_cache ? " --exclude=cache --exclude=code_cache --exclude=no_backup" : "");
    int rc = system(cmd);
    if (rc != 0) {
        unlink(tmpl);
        send_error(fd, "tar archive failed");
        return;
    }
    stream_file_and_remove(fd, tmpl);
}

static void handle_extract_archive(int fd, const void* payload, uint32_t payload_len) {
    if (payload_len < 8) { send_error(fd, "Invalid extract payload"); return; }
    uint32_t target_len = 0, archive_len = 0;
    memcpy(&target_len, payload, 4);
    memcpy(&archive_len, (const char*)payload + 4, 4);
    if (target_len == 0 || archive_len == 0 || 8ULL + target_len + archive_len != payload_len) {
        send_error(fd, "Invalid extract payload length");
        return;
    }
    if (target_len >= PATH_MAX || archive_len >= PATH_MAX) {
        send_error(fd, "Path too long");
        return;
    }
    char target[PATH_MAX], archive[PATH_MAX];
    memcpy(target, (const char*)payload + 8, target_len);
    target[target_len] = '\0';
    memcpy(archive, (const char*)payload + 8 + target_len, archive_len);
    archive[archive_len] = '\0';

    char quoted_target[PATH_MAX * 2];
    char quoted_archive[PATH_MAX * 2];
    shell_quote(target, quoted_target, sizeof(quoted_target));
    shell_quote(archive, quoted_archive, sizeof(quoted_archive));

    char error_tmpl[] = "/data/local/tmp/afm-extract-error-XXXXXX";
    int error_fd = mkstemp(error_tmpl);
    if (error_fd >= 0) close(error_fd);

    char quoted_error[PATH_MAX * 2];
    shell_quote(error_tmpl, quoted_error, sizeof(quoted_error));

    char cmd[PATH_MAX * 10];
    snprintf(cmd, sizeof(cmd),
             "mkdir -p %s && { rm -rf %s/* %s/.[!.]* %s/..?* 2>/dev/null || true; } && tar -xpf %s -C %s 2>%s",
             quoted_target, quoted_target, quoted_target, quoted_target,
             quoted_archive, quoted_target, quoted_error);
    int rc = system(cmd);
    if (rc != 0) {
        char detail[1024] = "archive extract failed";
        FILE* error_file = fopen(error_tmpl, "rb");
        if (error_file) {
            size_t read_len = fread(detail, 1, sizeof(detail) - 1, error_file);
            detail[read_len] = '\0';
            fclose(error_file);
            if (read_len == 0) snprintf(detail, sizeof(detail), "archive extract failed (tar exit %d)", rc);
        }
        unlink(error_tmpl);
        unlink(archive);
        send_error(fd, detail);
        return;
    }
    unlink(error_tmpl);
    unlink(archive);
    snprintf(cmd, sizeof(cmd), "restorecon -RF %s >/dev/null 2>&1 || true", quoted_target);
    system(cmd);
    send_ok(fd, NULL, 0);
}

// Double-buffered push
struct WriteJob {
    int       file_fd;
    char*     buf;
    uint32_t  len;
    uint64_t  offset;
    int       error;
    pthread_t thread;
    int       active;
};

struct QueuedWrite {
    char* buf;
    uint32_t len;
    uint64_t offset;
};

struct AsyncWriter {
    int file_fd;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t has_work;
    pthread_cond_t has_free;
    std::deque<QueuedWrite> queue;
    std::vector<char*> free_buffers;
    std::vector<char*> all_buffers;
    bool done;
    bool error;
};

static void* write_thread_func(void* arg) {
    WriteJob* job = (WriteJob*)arg;
    uint32_t written = 0;
    while (written < job->len) {
        ssize_t w = pwrite(job->file_fd, job->buf + written, job->len - written,
                           (off_t)(job->offset + written));
        if (w <= 0) { job->error = 1; break; }
        written += w;
    }
    return NULL;
}

static void* async_writer_thread_func(void* arg) {
    AsyncWriter* writer = (AsyncWriter*)arg;
    while (1) {
        QueuedWrite task{};
        pthread_mutex_lock(&writer->mutex);
        while (writer->queue.empty() && !writer->done && !writer->error)
            pthread_cond_wait(&writer->has_work, &writer->mutex);
        if ((writer->done && writer->queue.empty()) || writer->error) {
            pthread_mutex_unlock(&writer->mutex);
            break;
        }
        task = writer->queue.front();
        writer->queue.pop_front();
        pthread_mutex_unlock(&writer->mutex);

        uint32_t written = 0;
        while (written < task.len) {
            ssize_t w = pwrite(writer->file_fd, task.buf + written, task.len - written,
                               (off_t)(task.offset + written));
            if (w <= 0) {
                pthread_mutex_lock(&writer->mutex);
                writer->error = true;
                pthread_cond_broadcast(&writer->has_free);
                pthread_cond_broadcast(&writer->has_work);
                pthread_mutex_unlock(&writer->mutex);
                return NULL;
            }
            written += w;
        }

        pthread_mutex_lock(&writer->mutex);
        writer->free_buffers.push_back(task.buf);
        pthread_cond_signal(&writer->has_free);
        pthread_mutex_unlock(&writer->mutex);
    }
    return NULL;
}

static bool async_writer_start(AsyncWriter* writer, int file_fd, int depth) {
    writer->file_fd = file_fd;
    writer->done = false;
    writer->error = false;
    pthread_mutex_init(&writer->mutex, NULL);
    pthread_cond_init(&writer->has_work, NULL);
    pthread_cond_init(&writer->has_free, NULL);
    for (int i = 0; i < depth; i++) {
        char* buf = (char*)malloc(BUF_SIZE + 64);
        if (!buf) {
            writer->error = true;
            break;
        }
        writer->all_buffers.push_back(buf);
        writer->free_buffers.push_back(buf);
    }
    if (writer->error || writer->free_buffers.empty())
        return false;
    return pthread_create(&writer->thread, NULL, async_writer_thread_func, writer) == 0;
}

static char* async_writer_acquire(AsyncWriter* writer) {
    pthread_mutex_lock(&writer->mutex);
    while (writer->free_buffers.empty() && !writer->error)
        pthread_cond_wait(&writer->has_free, &writer->mutex);
    if (writer->error) {
        pthread_mutex_unlock(&writer->mutex);
        return NULL;
    }
    char* buf = writer->free_buffers.back();
    writer->free_buffers.pop_back();
    pthread_mutex_unlock(&writer->mutex);
    return buf;
}

static bool async_writer_submit(AsyncWriter* writer, char* buf, uint32_t len, uint64_t offset) {
    pthread_mutex_lock(&writer->mutex);
    if (writer->error) {
        writer->free_buffers.push_back(buf);
        pthread_cond_signal(&writer->has_free);
        pthread_mutex_unlock(&writer->mutex);
        return false;
    }
    writer->queue.push_back({ buf, len, offset });
    pthread_cond_signal(&writer->has_work);
    pthread_mutex_unlock(&writer->mutex);
    return true;
}

static bool async_writer_finish(AsyncWriter* writer) {
    pthread_mutex_lock(&writer->mutex);
    writer->done = true;
    pthread_cond_broadcast(&writer->has_work);
    pthread_mutex_unlock(&writer->mutex);
    pthread_join(writer->thread, NULL);
    return !writer->error;
}

static void async_writer_destroy(AsyncWriter* writer) {
    for (char* buf : writer->all_buffers)
        free(buf);
    writer->all_buffers.clear();
    writer->free_buffers.clear();
    writer->queue.clear();
    pthread_cond_destroy(&writer->has_work);
    pthread_cond_destroy(&writer->has_free);
    pthread_mutex_destroy(&writer->mutex);
}

static void handle_push(int fd, const void* payload, uint32_t payload_len) {
    if (payload_len < sizeof(PushHeader)) {
        send_error(fd, "Invalid push header");
        return;
    }

    PushHeader ph;
    memcpy(&ph, payload, sizeof(ph));
    uint32_t path_len = payload_len - sizeof(PushHeader);
    char pathbuf[PATH_MAX];
    if (path_len >= PATH_MAX) { send_error(fd, "Path too long"); return; }
    memcpy(pathbuf, (const char*)payload + sizeof(PushHeader), path_len);
    pathbuf[path_len] = '\0';

    int file_fd = open(pathbuf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file_fd < 0 && errno == ENOENT) {
        // Parent directory may not exist yet (race with parallel mkdir).
        // Extract parent path and create it, then retry.
        char parent[PATH_MAX];
        strncpy(parent, pathbuf, sizeof(parent));
        parent[sizeof(parent) - 1] = '\0';
        char* slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = '\0';
            char cmd[PATH_MAX + 16];
            snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", parent);
            system(cmd);
            file_fd = open(pathbuf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        }
    }
    if (file_fd < 0) {
        int err = errno;
        fprintf(stderr, "[PUSH] open failed: '%s' errno=%d (%s)\n", pathbuf, err, strerror(err));
        send_error(fd, strerror(err));
        return;
    }

    if (send_ok(fd, NULL, 0) < 0) { close(file_fd); return; }

    uint64_t received = 0;
    int error = 0;

    // Inline CRC: compute while receiving (free — data is already in memory)
    uint32_t push_crc = ~(uint32_t)0;

    AsyncWriter writer;
    if (!async_writer_start(&writer, file_fd, 6)) {
        async_writer_destroy(&writer);
        close(file_fd);
        send_error(fd, "Failed to start writer");
        return;
    }

    while (1) {
        MsgHeader hdr;
        if (recv_all(fd, &hdr, sizeof(hdr)) < 0) { error = 1; break; }
        if (hdr.cmd == RSP_DONE) break;
        if (hdr.cmd != RSP_DATA || hdr.length == 0) { error = 1; break; }
        if (hdr.length > BUF_SIZE) { error = 1; break; }

        char* recv_buf = async_writer_acquire(&writer);
        if (!recv_buf) { error = 1; break; }
        if (recv_all(fd, recv_buf, hdr.length) < 0) { error = 1; break; }

        // CRC the received chunk before handing to write thread
        crc32_update_raw(&push_crc, recv_buf, hdr.length);

        if (!async_writer_submit(&writer, recv_buf, hdr.length, received)) {
            error = 1;
            break;
        }
        received += hdr.length;
    }

    if (!async_writer_finish(&writer))
        error = 1;
    async_writer_destroy(&writer);

    fsync(file_fd);
    close(file_fd);

    if (error) {
        pthread_mutex_lock(&g_crc_mutex);
        g_cached_crc_valid = 0;
        pthread_mutex_unlock(&g_crc_mutex);
        send_error(fd, "Write error (partial file kept for resume)");
    } else {
        push_crc = ~push_crc;
        pthread_mutex_lock(&g_crc_mutex);
        strncpy(g_cached_crc_path, pathbuf, PATH_MAX - 1);
        g_cached_crc_path[PATH_MAX - 1] = '\0';
        g_cached_crc = push_crc;
        g_cached_crc_valid = 1;
        pthread_mutex_unlock(&g_crc_mutex);
        send_ok(fd, NULL, 0);
    }
}

static void handle_delete(int fd, const char* path, uint32_t path_len) {
    char pathbuf[PATH_MAX];
    if (path_len >= PATH_MAX) { send_error(fd, "Path too long"); return; }
    memcpy(pathbuf, path, path_len);
    pathbuf[path_len] = '\0';

    struct stat st;
    if (lstat(pathbuf, &st) != 0) {
        send_error(fd, strerror(errno));
        return;
    }

    int ret;
    if (S_ISDIR(st.st_mode)) {
        char cmd[PATH_MAX + 16];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", pathbuf);
        ret = system(cmd);
    } else {
        ret = unlink(pathbuf);
    }

    if (ret != 0) send_error(fd, strerror(errno));
    else send_ok(fd, NULL, 0);
}

static void handle_mkdir(int fd, const char* path, uint32_t path_len) {
    char pathbuf[PATH_MAX];
    if (path_len >= PATH_MAX) { send_error(fd, "Path too long"); return; }
    memcpy(pathbuf, path, path_len);
    pathbuf[path_len] = '\0';

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", pathbuf);
    if (system(cmd) != 0) send_error(fd, "mkdir failed");
    else send_ok(fd, NULL, 0);
}

static void handle_rename(int fd, const void* payload, uint32_t payload_len) {
    if (payload_len < 4) { send_error(fd, "Invalid rename"); return; }

    uint32_t old_len;
    memcpy(&old_len, payload, 4);
    if (4 + old_len > payload_len) { send_error(fd, "Invalid rename"); return; }

    uint32_t new_len = payload_len - 4 - old_len;
    char old_path[PATH_MAX], new_path[PATH_MAX];
    if (old_len >= PATH_MAX || new_len >= PATH_MAX) { send_error(fd, "Path too long"); return; }

    memcpy(old_path, (const char*)payload + 4, old_len);
    old_path[old_len] = '\0';
    memcpy(new_path, (const char*)payload + 4 + old_len, new_len);
    new_path[new_len] = '\0';

    if (strcmp(old_path, new_path) == 0) {
        send_ok(fd, NULL, 0);
        return;
    }

    int rename_result;
#if defined(SYS_renameat2)
    rename_result = (int)syscall(
        SYS_renameat2, AT_FDCWD, old_path, AT_FDCWD, new_path, RENAME_NOREPLACE);
    if (rename_result != 0 &&
        (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP)) {
#endif
        struct stat destination_stat;
        if (lstat(new_path, &destination_stat) == 0) {
            errno = EEXIST;
            rename_result = -1;
        } else if (errno != ENOENT) {
            rename_result = -1;
        } else {
            rename_result = rename(old_path, new_path);
        }
#if defined(SYS_renameat2)
    }
#endif

    if (rename_result != 0)
        send_error(fd, strerror(errno));
    else
        send_ok(fd, NULL, 0);
}

static void handle_crc32(int fd, const char* path, uint32_t path_len) {
    char pathbuf[PATH_MAX];
    if (path_len >= PATH_MAX) { send_error(fd, "Path too long"); return; }
    memcpy(pathbuf, path, path_len);
    pathbuf[path_len] = '\0';

    // Check push CRC cache — instant return if this file was just pushed
    pthread_mutex_lock(&g_crc_mutex);
    if (g_cached_crc_valid && strcmp(pathbuf, g_cached_crc_path) == 0) {
        uint32_t crc = g_cached_crc;
        g_cached_crc_valid = 0;
        pthread_mutex_unlock(&g_crc_mutex);
        send_ok(fd, &crc, sizeof(crc));
        return;
    }
    pthread_mutex_unlock(&g_crc_mutex);

    int file_fd = open(pathbuf, O_RDONLY);
    if (file_fd < 0) { send_error(fd, strerror(errno)); return; }

    // 4MB read buffer (matches transfer chunk size)
    const size_t kBufSize = 4 * 1024 * 1024;
    char* buf = (char*)malloc(kBufSize);
    if (!buf) { close(file_fd); send_error(fd, "Out of memory"); return; }

    uint32_t crc = ~(uint32_t)0;
    ssize_t n;
    while ((n = read(file_fd, buf, kBufSize)) > 0) {
        crc32_update_raw(&crc, buf, n);
    }
    crc = ~crc;
    free(buf);
    close(file_fd);

    send_ok(fd, &crc, sizeof(crc));
}

static void handle_sha256(int fd, const char* path, uint32_t path_len) {
    char pathbuf[PATH_MAX];
    if (path_len >= PATH_MAX) { send_error(fd, "Path too long"); return; }
    memcpy(pathbuf, path, path_len);
    pathbuf[path_len] = '\0';

    int file_fd = open(pathbuf, O_RDONLY);
    if (file_fd < 0) { send_error(fd, strerror(errno)); return; }

    const size_t kBufSize = 4 * 1024 * 1024;
    char* buf = (char*)malloc(kBufSize);
    if (!buf) { close(file_fd); send_error(fd, "Out of memory"); return; }

    SHA256_CTX_AFM ctx;
    uint8_t hash[32];
    sha256_init(&ctx);

    ssize_t n;
    while ((n = read(file_fd, buf, kBufSize)) > 0) {
        sha256_update(&ctx, (const uint8_t*)buf, (size_t)n);
    }
    if (n < 0) {
        free(buf);
        close(file_fd);
        send_error(fd, strerror(errno));
        return;
    }

    sha256_final(&ctx, hash);
    free(buf);
    close(file_fd);

    send_ok(fd, hash, sizeof(hash));
}

// --- Read range handler (random byte-level access) ---

static void handle_read_range(int fd, const void* payload, uint32_t payload_len) {
    if (payload_len < 16) { send_error(fd, "Invalid read range"); return; }

    uint64_t offset, length;
    memcpy(&offset, payload, 8);
    memcpy(&length, (const char*)payload + 8, 8);
    uint32_t path_len = payload_len - 16;
    char pathbuf[PATH_MAX];
    if (path_len >= PATH_MAX) { send_error(fd, "Path too long"); return; }
    memcpy(pathbuf, (const char*)payload + 16, path_len);
    pathbuf[path_len] = '\0';

    int file_fd = open(pathbuf, O_RDONLY);
    if (file_fd < 0) {
        int err = errno;
        fprintf(stderr, "[READ_RANGE] open failed: '%s' errno=%d (%s)\n", pathbuf, err, strerror(err));
        send_error(fd, strerror(err));
        return;
    }

    struct stat st;
    fstat(file_fd, &st);
    uint64_t file_size = st.st_size;

    // Clamp to file bounds
    if (offset >= file_size) {
        close(file_fd);
        // Return zero-length range
        uint64_t actual = 0;
        send_ok(fd, &actual, sizeof(actual));
        send_msg(fd, RSP_DONE, NULL, 0);
        return;
    }
    uint64_t available = file_size - offset;
    if (length > available) length = available;

    // Send actual length we'll return
    if (send_ok(fd, &length, sizeof(length)) < 0) { close(file_fd); return; }
    posix_fadvise(file_fd, (off_t)offset, (off_t)length, POSIX_FADV_SEQUENTIAL);

    // Stream the range using sendfile
    uint64_t remaining = length;
    off_t sf_offset = (off_t)offset;
    int error = 0;

    while (remaining > 0) {
        size_t chunk = remaining > BUF_SIZE ? BUF_SIZE : (size_t)remaining;

        MsgHeader hdr;
        hdr.cmd = RSP_DATA;
        hdr.length = (uint32_t)chunk;
        if (send_all(fd, &hdr, sizeof(hdr)) < 0) { error = 1; break; }

        size_t sent = 0;
        while (sent < chunk) {
            ssize_t n = sendfile(fd, file_fd, &sf_offset, chunk - sent);
            if (n <= 0) { error = 1; break; }
            sent += n;
        }
        if (error) break;
        remaining -= chunk;
    }

    close(file_fd);
    if (!error) send_msg(fd, RSP_DONE, NULL, 0);
}

// --- Write range handler (random byte-level write) ---

static void handle_write_range(int fd, const void* payload, uint32_t payload_len) {
    if (payload_len < 16) { send_error(fd, "Invalid write range"); return; }

    uint64_t offset, length;
    memcpy(&offset, payload, 8);
    memcpy(&length, (const char*)payload + 8, 8);
    uint32_t path_len = payload_len - 16;
    char pathbuf[PATH_MAX];
    if (path_len >= PATH_MAX) { send_error(fd, "Path too long"); return; }
    memcpy(pathbuf, (const char*)payload + 16, path_len);
    pathbuf[path_len] = '\0';

    // Open file for writing at offset (create if needed)
    int file_fd = open(pathbuf, O_WRONLY | O_CREAT, 0644);
    if (file_fd < 0 && errno == ENOENT) {
        char parent[PATH_MAX];
        strncpy(parent, pathbuf, sizeof(parent));
        parent[sizeof(parent) - 1] = '\0';
        char* slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = '\0';
            char cmd[PATH_MAX + 16];
            snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", parent);
            system(cmd);
            file_fd = open(pathbuf, O_WRONLY | O_CREAT, 0644);
        }
    }
    if (file_fd < 0) { send_error(fd, strerror(errno)); return; }

    // Seek to offset
    if (lseek(file_fd, offset, SEEK_SET) < 0) {
        send_error(fd, strerror(errno));
        close(file_fd);
        return;
    }
    posix_fadvise(file_fd, (off_t)offset, (off_t)length, POSIX_FADV_SEQUENTIAL);

    // Send OK to signal ready for data
    if (send_ok(fd, NULL, 0) < 0) { close(file_fd); return; }

    uint64_t written = 0;
    int error = 0;
    uint32_t range_crc = ~(uint32_t)0;

    AsyncWriter writer;
    if (!async_writer_start(&writer, file_fd, 6)) {
        async_writer_destroy(&writer);
        close(file_fd);
        send_error(fd, "Failed to start writer");
        return;
    }

    while (1) {
        MsgHeader hdr;
        if (recv_all(fd, &hdr, sizeof(hdr)) < 0) { error = 1; break; }
        if (hdr.cmd == RSP_DONE) break;
        if (hdr.cmd != RSP_DATA || hdr.length == 0) { error = 1; break; }
        if (hdr.length > BUF_SIZE) { error = 1; break; }

        char* recv_buf = async_writer_acquire(&writer);
        if (!recv_buf) { error = 1; break; }
        if (recv_all(fd, recv_buf, hdr.length) < 0) { error = 1; break; }
        crc32_update_raw(&range_crc, recv_buf, hdr.length);

        if (!async_writer_submit(&writer, recv_buf, hdr.length, offset + written)) {
            error = 1;
            break;
        }
        written += hdr.length;
    }

    if (!async_writer_finish(&writer))
        error = 1;
    async_writer_destroy(&writer);

    // No fsync per block — kernel writeback handles persistence.
    // File was pre-allocated by createFile; blocks are non-overlapping.
    close(file_fd);

    if (error) {
        send_error(fd, "Write failed");
    } else {
        range_crc = ~range_crc;
        struct {
            uint64_t written;
            uint32_t crc;
        } resp;
        resp.written = written;
        resp.crc = range_crc;
        send_ok(fd, &resp, sizeof(resp));
    }
}

static void handle_create_file(int fd, const void* payload, uint32_t payload_len) {
    if (payload_len < 8) { send_error(fd, "Invalid create file"); return; }

    uint64_t total_size;
    memcpy(&total_size, payload, 8);
    uint32_t path_len = payload_len - 8;
    char pathbuf[PATH_MAX];
    if (path_len >= PATH_MAX) { send_error(fd, "Path too long"); return; }
    memcpy(pathbuf, (const char*)payload + 8, path_len);
    pathbuf[path_len] = '\0';

    int file_fd = open(pathbuf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file_fd < 0) { send_error(fd, strerror(errno)); return; }

    // Pre-allocate if size known
    if (total_size > 0) {
        ftruncate(file_fd, total_size);
    }

    close(file_fd);
    send_ok(fd, NULL, 0);
}

// --- Resume handlers ---

static void handle_resume_pull(int fd, const void* payload, uint32_t payload_len) {
    if (payload_len < 8) { send_error(fd, "Invalid resume pull"); return; }

    uint64_t offset;
    memcpy(&offset, payload, 8);
    uint32_t path_len = payload_len - 8;
    char pathbuf[PATH_MAX];
    if (path_len >= PATH_MAX) { send_error(fd, "Path too long"); return; }
    memcpy(pathbuf, (const char*)payload + 8, path_len);
    pathbuf[path_len] = '\0';

    int file_fd = open(pathbuf, O_RDONLY);
    if (file_fd < 0) { send_error(fd, strerror(errno)); return; }

    struct stat st;
    fstat(file_fd, &st);
    uint64_t file_size = st.st_size;

    if (offset > file_size) offset = file_size;
    lseek(file_fd, offset, SEEK_SET);

    PullHeader ph = { file_size };
    if (send_ok(fd, &ph, sizeof(ph)) < 0) { close(file_fd); return; }

    uint64_t remaining = file_size - offset;
    off_t sf_offset = (off_t)offset;
    int error = 0;

    while (remaining > 0) {
        size_t chunk = remaining > BUF_SIZE ? BUF_SIZE : (size_t)remaining;

        MsgHeader hdr;
        hdr.cmd = RSP_DATA;
        hdr.length = (uint32_t)chunk;
        if (send_all(fd, &hdr, sizeof(hdr)) < 0) { error = 1; break; }

        size_t sent = 0;
        while (sent < chunk) {
            ssize_t n = sendfile(fd, file_fd, &sf_offset, chunk - sent);
            if (n <= 0) { error = 1; break; }
            sent += n;
        }
        if (error) break;
        remaining -= chunk;
    }

    close(file_fd);
    if (!error) send_msg(fd, RSP_DONE, NULL, 0);
}

static void handle_resume_push(int fd, const void* payload, uint32_t payload_len) {
    if (payload_len < 8) { send_error(fd, "Invalid resume push"); return; }

    uint64_t total_size;
    memcpy(&total_size, payload, 8);
    uint32_t path_len = payload_len - 8;
    char pathbuf[PATH_MAX];
    if (path_len >= PATH_MAX) { send_error(fd, "Path too long"); return; }
    memcpy(pathbuf, (const char*)payload + 8, path_len);
    pathbuf[path_len] = '\0';

    uint64_t existing_size = 0;
    struct stat st;
    if (stat(pathbuf, &st) == 0) {
        existing_size = st.st_size;
    }

    if (existing_size >= total_size) {
        existing_size = 0;
    }

    int file_fd;
    if (existing_size > 0) {
        file_fd = open(pathbuf, O_WRONLY | O_APPEND);
    } else {
        file_fd = open(pathbuf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        existing_size = 0;
    }

    if (file_fd < 0) { send_error(fd, strerror(errno)); return; }

    if (send_ok(fd, &existing_size, sizeof(existing_size)) < 0) { close(file_fd); return; }

    char* buf_a = (char*)malloc(BUF_SIZE + 64);
    char* buf_b = (char*)malloc(BUF_SIZE + 64);
    char* recv_buf = buf_a;
    uint64_t received = 0;
    int error = 0;

    WriteJob wjob;
    wjob.file_fd = file_fd;
    wjob.active = 0;
    wjob.error = 0;

    while (1) {
        MsgHeader hdr;
        if (recv_all(fd, &hdr, sizeof(hdr)) < 0) { error = 1; break; }
        if (hdr.cmd == RSP_DONE) break;
        if (hdr.cmd != RSP_DATA || hdr.length == 0) { error = 1; break; }
        if (recv_all(fd, recv_buf, hdr.length) < 0) { error = 1; break; }

        if (wjob.active) {
            pthread_join(wjob.thread, NULL);
            wjob.active = 0;
            if (wjob.error) { error = 1; break; }
        }

        wjob.buf = recv_buf;
        wjob.len = hdr.length;
        wjob.offset = existing_size + received;
        wjob.error = 0;
        wjob.active = 1;
        pthread_create(&wjob.thread, NULL, write_thread_func, &wjob);

        recv_buf = (recv_buf == buf_a) ? buf_b : buf_a;
        received += hdr.length;
    }

    if (wjob.active) {
        pthread_join(wjob.thread, NULL);
        if (wjob.error) error = 1;
    }

    free(buf_a);
    free(buf_b);
    fsync(file_fd);
    close(file_fd);

    if (error) {
        send_error(fd, "Write error (partial file kept for resume)");
    } else {
        send_ok(fd, NULL, 0);
    }
}

// Build a DNG file in memory from a decoded frame
static bool build_dng_memory(
    const std::vector<uint8_t>& pixelData,
    const nlohmann::json& frameMetadata,
    const nlohmann::json& containerMetadata,
    std::string& outBuffer)
{
    const unsigned int width = frameMetadata["width"];
    const unsigned int height = frameMetadata["height"];

    std::vector<float> asShotNeutral = frameMetadata["asShotNeutral"];
    std::string sensorArrangement = containerMetadata["sensorArrangment"]; // note: typo in upstream
    std::vector<float> colorMatrix1 = containerMetadata["colorMatrix1"];
    std::vector<float> colorMatrix2 = containerMetadata["colorMatrix2"];
    std::vector<float> forwardMatrix1 = containerMetadata["forwardMatrix1"];
    std::vector<float> forwardMatrix2 = containerMetadata["forwardMatrix2"];

    // Prefer per-frame dynamic levels, fall back to container-level static levels
    std::vector<uint16_t> blackLevel;
    double whiteLevel;
    if (frameMetadata.contains("dynamicBlackLevel") && frameMetadata["dynamicBlackLevel"].is_array()) {
        auto dbl = frameMetadata["dynamicBlackLevel"];
        for (size_t i = 0; i < 4 && i < dbl.size(); i++)
            blackLevel.push_back((uint16_t)dbl[i].get<float>());
    } else {
        blackLevel = containerMetadata["blackLevel"].get<std::vector<uint16_t>>();
    }
    while (blackLevel.size() < 4) blackLevel.push_back(0);

    if (frameMetadata.contains("dynamicWhiteLevel") && frameMetadata["dynamicWhiteLevel"].is_number()) {
        whiteLevel = frameMetadata["dynamicWhiteLevel"].get<float>();
    } else {
        whiteLevel = containerMetadata["whiteLevel"];
    }

    // Fix white level when metadata doesn't match actual pixel data range.
    // Black levels are left as-is since they may already be in the correct scale.
    {
        uint16_t wl = (uint16_t)whiteLevel;
        const uint16_t* pixels = reinterpret_cast<const uint16_t*>(pixelData.data());
        size_t pixelCount = pixelData.size() / 2;
        uint16_t maxVal = 0;
        size_t step = std::max<size_t>(1, pixelCount / 500000);
        for (size_t i = 0; i < pixelCount; i += step)
            if (pixels[i] > maxVal) maxVal = pixels[i];

        if (wl > 0 && maxVal > wl + wl / 2) {
            if (maxVal <= 4095)       whiteLevel = 4095.0;
            else if (maxVal <= 16383) whiteLevel = 16383.0;
            else                      whiteLevel = 65535.0;
        }
    }

    // Normalize forward matrices to D50 white point
    auto normalizeToD50 = [](std::vector<float>& m) {
        if (m.size() < 9) return;
        const float D50[3] = { 0.9642f, 1.0f, 0.8251f };
        bool allZero = true;
        for (int i = 0; i < 9; i++) if (m[i] != 0.0f) { allZero = false; break; }
        if (allZero) { m = {0.9642f,0,0, 0,1.0f,0, 0,0,0.8251f}; return; }
        for (int row = 0; row < 3; row++) {
            float sum = m[row*3+0] + m[row*3+1] + m[row*3+2];
            if (std::abs(sum) < 1e-9f) { m = {0.9642f,0,0, 0,1.0f,0, 0,0,0.8251f}; return; }
            float scale = D50[row] / sum;
            m[row*3+0] *= scale; m[row*3+1] *= scale; m[row*3+2] *= scale;
        }
    };
    normalizeToD50(forwardMatrix1);
    normalizeToD50(forwardMatrix2);

    tinydngwriter::DNGImage dng;
    dng.SetBigEndian(false);
    dng.SetDNGVersion(1, 4, 0, 0);
    dng.SetDNGBackwardVersion(1, 1, 0, 0);
    dng.SetImageData(reinterpret_cast<const unsigned char*>(pixelData.data()), pixelData.size());
    dng.SetImageWidth(width);
    dng.SetImageLength(height);
    dng.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG);
    dng.SetPhotometric(tinydngwriter::PHOTOMETRIC_CFA);
    dng.SetRowsPerStrip(height);
    dng.SetSamplesPerPixel(1);
    dng.SetCFARepeatPatternDim(2, 2);
    dng.SetBlackLevelRepeatDim(2, 2);
    dng.SetBlackLevel(4, blackLevel.data());
    dng.SetWhiteLevel(whiteLevel);
    dng.SetCompression(tinydngwriter::COMPRESSION_NONE);

    std::vector<uint8_t> cfa;
    if (sensorArrangement == "rggb")       cfa = { 0, 1, 1, 2 };
    else if (sensorArrangement == "bggr")  cfa = { 2, 1, 1, 0 };
    else if (sensorArrangement == "grbg")  cfa = { 1, 0, 2, 1 };
    else if (sensorArrangement == "gbrg")  cfa = { 1, 2, 0, 1 };
    else return false;

    dng.SetCFAPattern(4, cfa.data());
    dng.SetCFALayout(1);

    const uint16_t bps[1] = { 16 };
    dng.SetBitsPerSample(1, bps);
    dng.SetColorMatrix1(3, colorMatrix1.data());
    dng.SetColorMatrix2(3, colorMatrix2.data());

    // Camera calibration: identity matrices
    const float identity[9] = {1,0,0, 0,1,0, 0,0,1};
    dng.SetCameraCalibration1(3, identity);
    dng.SetCameraCalibration2(3, identity);

    dng.SetForwardMatrix1(3, forwardMatrix1.data());
    dng.SetForwardMatrix2(3, forwardMatrix2.data());
    dng.SetAsShotNeutral(3, asShotNeutral.data());
    dng.SetCalibrationIlluminant1(21);
    dng.SetCalibrationIlluminant2(17);
    dng.SetUniqueCameraModel("MotionCam");
    dng.SetSubfileType();

    // Lens shading map as OpcodeList2 GainMap (vignette correction)
    if (frameMetadata.contains("lensShadingMap") && frameMetadata["lensShadingMap"].is_array()) {
        int lsmW = frameMetadata.value("lensShadingMapWidth", 0);
        int lsmH = frameMetadata.value("lensShadingMapHeight", 0);
        if (lsmW > 0 && lsmH > 0) {
            std::vector<std::vector<float>> shadingMap;
            for (const auto& channel : frameMetadata["lensShadingMap"]) {
                if (!channel.is_array()) continue;
                std::vector<float> chanData;
                chanData.reserve(channel.size());
                for (const auto& v : channel) chanData.push_back(v.get<float>());
                shadingMap.emplace_back(std::move(chanData));
            }
            if (!shadingMap.empty()) {
                unsigned int planes = std::min<unsigned int>((unsigned int)shadingMap.size(), 4);
                unsigned int mapH = (unsigned int)lsmH, mapW = (unsigned int)lsmW;
                unsigned int rowPitch = (mapH > 1) ? std::max(1u, (height - 1) / (mapH - 1)) : height;
                unsigned int colPitch = (mapW > 1) ? std::max(1u, (width - 1) / (mapW - 1)) : width;

                tinydngwriter::GainMapParams gp;
                gp.top = 0; gp.left = 0; gp.bottom = height; gp.right = width;
                gp.plane = 0; gp.planes = planes;
                gp.row_pitch = rowPitch; gp.col_pitch = colPitch;
                gp.map_points_v = mapH; gp.map_points_h = mapW;
                gp.map_spacing_v = (height > 0) ? (double)rowPitch / (double)height : 0.0;
                gp.map_spacing_h = (width > 0) ? (double)colPitch / (double)width : 0.0;
                gp.map_origin_v = 0.0; gp.map_origin_h = 0.0;
                gp.map_planes = planes;

                for (unsigned int ch = 0; ch < planes; ch++) {
                    const auto& src = (ch < shadingMap.size()) ? shadingMap[ch] : shadingMap[0];
                    for (unsigned int v = 0; v < mapH; v++) {
                        for (unsigned int h = 0; h < mapW; h++) {
                            size_t idx = (size_t)v * mapW + h;
                            float gain = (idx < src.size()) ? src[idx] : 1.0f;
                            if (!std::isfinite(gain) || gain <= 0.0f) gain = 1.0f;
                            else if (gain > 16.0f) gain = 16.0f;
                            gp.gain_data.push_back(gain);
                        }
                    }
                }

                tinydngwriter::OpcodeList opcodeList;
                opcodeList.AddGainMap(gp);
                dng.SetOpcodeList2(opcodeList);
            }
        }
    }

    const uint32_t activeArea[4] = { 0, 0, height, width };
    dng.SetActiveArea(&activeArea[0]);

    std::string err;
    tinydngwriter::DNGWriter writer(false);
    writer.AddImage(&dng);

    std::ostringstream oss(std::ios::binary);
    if (!writer.WriteToFile(oss, &err)) return false;

    outBuffer = oss.str();
    return true;
}

// Build a WAV file in memory from audio chunks
static bool build_wav_memory(
    const std::vector<motioncam::AudioChunk>& chunks,
    int sampleRate, int numChannels,
    std::string& outBuffer)
{
    // Count total samples per channel
    size_t totalSamples = 0;
    for (auto& c : chunks) totalSamples += c.second.size();
    // totalSamples is interleaved count; actual per-channel = totalSamples / numChannels
    // But WAV stores interleaved, so total PCM bytes = totalSamples * sizeof(int16_t)

    uint32_t dataSize = (uint32_t)(totalSamples * sizeof(int16_t));
    uint32_t fileSize = 36 + dataSize;

    std::ostringstream oss(std::ios::binary);

    // RIFF header
    oss.write("RIFF", 4);
    oss.write((const char*)&fileSize, 4);
    oss.write("WAVE", 4);

    // fmt sub-chunk
    oss.write("fmt ", 4);
    uint32_t fmtSize = 16;
    oss.write((const char*)&fmtSize, 4);
    uint16_t audioFormat = 1; // PCM
    oss.write((const char*)&audioFormat, 2);
    uint16_t channels = (uint16_t)numChannels;
    oss.write((const char*)&channels, 2);
    uint32_t sr = (uint32_t)sampleRate;
    oss.write((const char*)&sr, 4);
    uint32_t byteRate = sr * channels * sizeof(int16_t);
    oss.write((const char*)&byteRate, 4);
    uint16_t blockAlign = channels * sizeof(int16_t);
    oss.write((const char*)&blockAlign, 2);
    uint16_t bitsPerSample = 16;
    oss.write((const char*)&bitsPerSample, 2);

    // data sub-chunk
    oss.write("data", 4);
    oss.write((const char*)&dataSize, 4);

    // Write interleaved PCM samples
    for (auto& c : chunks) {
        oss.write((const char*)c.second.data(), c.second.size() * sizeof(int16_t));
    }

    outBuffer = oss.str();
    return true;
}

// CMD_MCRAW_LIST: list virtual contents of an MCRAW container
// Payload: path to .mcraw file
// Response: same format as CMD_LIST (count + FileEntryHeaders)
static void handle_mcraw_list(int fd, const char* path, uint32_t path_len) {
    char pathbuf[PATH_MAX];
    if (path_len >= PATH_MAX) { send_error(fd, "Path too long"); return; }
    memcpy(pathbuf, path, path_len);
    pathbuf[path_len] = '\0';

    // Get mtime of the .mcraw file itself
    struct stat mcraw_st;
    int64_t mcraw_mtime = 0;
    if (stat(pathbuf, &mcraw_st) == 0) {
        mcraw_mtime = mcraw_st.st_mtime;
    }

    try {
        std::string pathStr(pathbuf);
        motioncam::Decoder decoder(pathStr);
        auto& frames = decoder.getFrames();
        auto& meta = decoder.getContainerMetadata();

        // Estimate frame DNG size from first frame metadata
        uint64_t dngEstSize = 0;
        if (!frames.empty()) {
            nlohmann::json frameMeta;
            decoder.loadFrameMetadata(frames[0], frameMeta);
            unsigned int w = frameMeta["width"];
            unsigned int h = frameMeta["height"];
            dngEstSize = (uint64_t)w * h * 2 + 8192; // pixel data + DNG header overhead
        }

        // Check if audio exists
        bool hasAudio = false;
        uint64_t audioEstSize = 0;
        try {
            if (meta.contains("extraData") &&
                meta["extraData"].contains("audioSampleRate") &&
                meta["extraData"].contains("audioChannels")) {
                int sr = meta["extraData"]["audioSampleRate"];
                int ch = meta["extraData"]["audioChannels"];
                if (sr > 0 && ch > 0) {
                    // Load audio to count samples for size estimate
                    std::vector<motioncam::AudioChunk> audioChunks;
                    decoder.loadAudio(audioChunks);
                    if (!audioChunks.empty()) {
                        hasAudio = true;
                        size_t totalSamples = 0;
                        for (auto& c : audioChunks) totalSamples += c.second.size();
                        audioEstSize = 44 + totalSamples * sizeof(int16_t); // WAV header + PCM
                    }
                }
            }
        } catch (...) {
            // No audio, that's fine
        }

        // Build metadata.json size
        std::string metaJson = meta.dump(2);
        uint64_t metaSize = metaJson.size();

        // Count entries: metadata.json + frames + optional audio.wav
        uint32_t count = 1 + (uint32_t)frames.size() + (hasAudio ? 1 : 0);

        // Allocate buffer for response
        size_t buf_size = 256 * 1024;
        char* buf = (char*)malloc(buf_size);
        size_t buf_used = 4; // reserve for count

        auto addEntry = [&](const char* name, uint64_t size) {
            uint32_t name_len = strlen(name);
            size_t entry_size = sizeof(FileEntryHeader) + name_len;
            while (buf_used + entry_size > buf_size) {
                buf_size *= 2;
                buf = (char*)realloc(buf, buf_size);
            }
            FileEntryHeader* eh = (FileEntryHeader*)(buf + buf_used);
            eh->type = 0; // regular file
            eh->size = size;
            eh->mtime = mcraw_mtime;
            eh->name_len = name_len;
            memcpy(buf + buf_used + sizeof(FileEntryHeader), name, name_len);
            buf_used += entry_size;
        };

        // metadata.json
        addEntry("metadata.json", metaSize);

        // Frames: frame_000001.dng through frame_NNNNNN.dng
        for (size_t i = 0; i < frames.size(); i++) {
            char fname[32];
            snprintf(fname, sizeof(fname), "frame_%06zu.dng", i + 1);
            addEntry(fname, dngEstSize);
        }

        // audio.wav
        if (hasAudio) {
            addEntry("audio.wav", audioEstSize);
        }

        memcpy(buf, &count, 4);
        send_ok(fd, buf, buf_used);
        free(buf);

    } catch (std::exception& e) {
        send_error(fd, e.what());
    }
}

// CMD_MCRAW_EXTRACT: extract a virtual item from an MCRAW container
// Payload: [4B mcraw_path_len][mcraw_path][virtual_name]
// Response: same as CMD_PULL (PullHeader + RSP_DATA chunks + RSP_DONE)
static void handle_mcraw_extract(int fd, const void* payload, uint32_t payload_len) {
    if (payload_len < 5) { send_error(fd, "Invalid mcraw extract payload"); return; }

    // Parse: [4B mcraw_path_len][mcraw_path bytes][virtual_name bytes]
    uint32_t mcraw_path_len;
    memcpy(&mcraw_path_len, payload, 4);

    if (4 + mcraw_path_len >= payload_len) {
        send_error(fd, "Invalid mcraw extract: path length exceeds payload");
        return;
    }

    char mcraw_path[PATH_MAX];
    if (mcraw_path_len >= PATH_MAX) { send_error(fd, "Path too long"); return; }
    memcpy(mcraw_path, (const char*)payload + 4, mcraw_path_len);
    mcraw_path[mcraw_path_len] = '\0';

    uint32_t vname_len = payload_len - 4 - mcraw_path_len;
    char vname[256];
    if (vname_len >= sizeof(vname)) { send_error(fd, "Virtual name too long"); return; }
    memcpy(vname, (const char*)payload + 4 + mcraw_path_len, vname_len);
    vname[vname_len] = '\0';

    try {
        std::string mcrawStr(mcraw_path);
        motioncam::Decoder decoder(mcrawStr);
        auto& frames = decoder.getFrames();
        auto& containerMeta = decoder.getContainerMetadata();

        std::string virtualName(vname);

        if (virtualName == "metadata.json") {
            // Stream container metadata as JSON
            std::string json = containerMeta.dump(2);
            stream_buffer(fd, json.data(), json.size());

        } else if (virtualName == "audio.wav") {
            // Extract all audio and stream as WAV
            std::vector<motioncam::AudioChunk> audioChunks;
            decoder.loadAudio(audioChunks);

            if (audioChunks.empty()) {
                send_error(fd, "No audio data in container");
                return;
            }

            int sampleRate = decoder.audioSampleRateHz();
            int numChannels = decoder.numAudioChannels();

            std::string wavData;
            if (!build_wav_memory(audioChunks, sampleRate, numChannels, wavData)) {
                send_error(fd, "Failed to build WAV data");
                return;
            }

            stream_buffer(fd, wavData.data(), wavData.size());

        } else if (virtualName.rfind("frame_", 0) == 0 && virtualName.size() > 10 &&
                   virtualName.substr(virtualName.size() - 4) == ".dng") {
            // Parse frame index from "frame_NNNNNN.dng"
            std::string indexStr = virtualName.substr(6, virtualName.size() - 10);
            int frameIdx = std::stoi(indexStr) - 1; // 1-based to 0-based

            if (frameIdx < 0 || frameIdx >= (int)frames.size()) {
                send_error(fd, "Frame index out of range");
                return;
            }

            // Load and decompress frame
            std::vector<uint8_t> pixelData;
            nlohmann::json frameMeta;
            decoder.loadFrame(frames[frameIdx], pixelData, frameMeta);

            // Build DNG in memory
            std::string dngData;
            if (!build_dng_memory(pixelData, frameMeta, containerMeta, dngData)) {
                send_error(fd, "Failed to build DNG");
                return;
            }

            stream_buffer(fd, dngData.data(), dngData.size());

        } else {
            send_error(fd, "Unknown virtual file");
        }

    } catch (std::exception& e) {
        send_error(fd, e.what());
    }
}

// --- Main ---

static void handle_client(int client_fd) {
    int bufsize = 16 * 1024 * 1024;
    setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    char* payload_buf = (char*)malloc(BUF_SIZE + PATH_MAX);

    while (g_running) {
        MsgHeader hdr;
        if (recv_all(client_fd, &hdr, sizeof(hdr)) < 0) break;

        if (hdr.length > BUF_SIZE + PATH_MAX) {
            send_error(client_fd, "Payload too large");
            break;
        }

        if (hdr.length > 0) {
            if (recv_all(client_fd, payload_buf, hdr.length) < 0) break;
        }

        switch (hdr.cmd) {
            case CMD_PING:    handle_ping(client_fd); break;
            case CMD_LIST:    handle_list(client_fd, payload_buf, hdr.length); break;
            case CMD_PULL:    handle_pull(client_fd, payload_buf, hdr.length); break;
            case CMD_PUSH:    handle_push(client_fd, payload_buf, hdr.length); break;
            case CMD_DELETE:  handle_delete(client_fd, payload_buf, hdr.length); break;
            case CMD_MKDIR:   handle_mkdir(client_fd, payload_buf, hdr.length); break;
            case CMD_RENAME:  handle_rename(client_fd, payload_buf, hdr.length); break;
            case CMD_STAT:    handle_stat(client_fd, payload_buf, hdr.length); break;
            case CMD_STORAGE:     handle_storage(client_fd); break;
            case CMD_QUIT:        g_running = 0; send_ok(client_fd, NULL, 0); break;
            case CMD_RESUME_PULL: handle_resume_pull(client_fd, payload_buf, hdr.length); break;
            case CMD_RESUME_PUSH: handle_resume_push(client_fd, payload_buf, hdr.length); break;
            case CMD_CRC32:       handle_crc32(client_fd, payload_buf, hdr.length); break;
            case CMD_SHA256:      handle_sha256(client_fd, payload_buf, hdr.length); break;
            case CMD_MCRAW_LIST:    handle_mcraw_list(client_fd, payload_buf, hdr.length); break;
            case CMD_MCRAW_EXTRACT: handle_mcraw_extract(client_fd, payload_buf, hdr.length); break;
            case CMD_READ_RANGE:    handle_read_range(client_fd, payload_buf, hdr.length); break;
            case CMD_DISK_SPACE:    handle_disk_space(client_fd); break;
            case CMD_WRITE_RANGE:   handle_write_range(client_fd, payload_buf, hdr.length); break;
            case CMD_CREATE_FILE:   handle_create_file(client_fd, payload_buf, hdr.length); break;
            case CMD_ARCHIVE_PATH:  handle_archive_path(client_fd, payload_buf, hdr.length); break;
            case CMD_EXTRACT_ARCHIVE: handle_extract_archive(client_fd, payload_buf, hdr.length); break;
            case CMD_INSTALL_BATCH: handle_install_batch(client_fd, payload_buf, hdr.length); break;
            case CMD_RESTORE_BACKUP_ARCHIVE: handle_restore_backup_archive(client_fd, payload_buf, hdr.length); break;
            case CMD_CREATE_BACKUP_ARCHIVE: handle_create_backup_archive(client_fd, payload_buf, hdr.length); break;
            default:              send_error(client_fd, "Unknown command"); break;
        }
    }

    free(payload_buf);
    close(client_fd);
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);

    int port = AFM_PORT;
    if (argc > 1) port = atoi(argv[1]);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }
    close_socket_on_exec(server_fd);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 32) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("AFM_READY %d\n", port);
    fflush(stdout);

    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        close_socket_on_exec(client_fd);
        tune_socket(client_fd);

        // Spawn a thread per client for parallel transfers
        pthread_t t;
        int* fd_ptr = (int*)malloc(sizeof(int));
        *fd_ptr = client_fd;
        pthread_create(&t, NULL, [](void* arg) -> void* {
            int fd = *(int*)arg;
            free(arg);
            handle_client(fd);
            return NULL;
        }, fd_ptr);
        pthread_detach(t);
    }

    close(server_fd);
    return 0;
}
