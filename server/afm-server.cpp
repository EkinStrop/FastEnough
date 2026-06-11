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

#include <motioncam/Decoder.hpp>
#include <nlohmann/json.hpp>

#define TINY_DNG_WRITER_IMPLEMENTATION
#include <tinydng/tiny_dng_writer.h>
#undef TINY_DNG_WRITER_IMPLEMENTATION

#include "protocol.h"

#define BUF_SIZE AFM_CHUNK_SIZE

static int g_running = 1;

// --- Helpers ---

static void tune_socket(int fd) {
    int bufsize = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
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
    struct { uint32_t magic; uint32_t version; } resp = { AFM_MAGIC, AFM_VERSION };
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

    if (rename(old_path, new_path) != 0)
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
