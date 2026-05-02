// AFM Protocol - Windows client version
#pragma once
#include <cstdint>

#define AFM_PORT 5740
#define AFM_MAGIC 0x41464D53
#define AFM_VERSION 1
#define AFM_CHUNK_SIZE (4 * 1024 * 1024)

enum {
    CMD_PING    = 1,
    CMD_LIST    = 2,
    CMD_PULL    = 3,
    CMD_PUSH    = 4,
    CMD_DELETE  = 5,
    CMD_MKDIR   = 6,
    CMD_RENAME  = 7,
    CMD_STAT    = 8,
    CMD_STORAGE     = 9,
    CMD_QUIT        = 10,
    CMD_RESUME_PULL = 11,
    CMD_RESUME_PUSH = 12,
    CMD_CRC32       = 13,  // payload: path string - returns 4B CRC32
    CMD_MCRAW_LIST  = 14,  // payload: path to .mcraw file - returns virtual dir listing
    CMD_MCRAW_EXTRACT = 15, // payload: [4B mcraw_path_len][mcraw_path][virtual_name] - streams DNG/WAV/JSON
    CMD_READ_RANGE = 16, // payload: [8B offset][8B length][path string] - read byte range
    CMD_DISK_SPACE = 17, // payload: none - returns [8B total][8B free] bytes
    CMD_WRITE_RANGE = 18, // payload: [8B offset][8B length][path string] then RSP_DATA chunks, RSP_DONE
    CMD_CREATE_FILE = 19, // payload: [8B total_size][path string] - create/truncate file
};

enum {
    RSP_OK    = 100,
    RSP_ERROR = 101,
    RSP_DATA  = 102,
    RSP_DONE  = 103,
};

#pragma pack(push, 1)

struct MsgHeader {
    uint32_t cmd;
    uint32_t length;
};

struct FileEntryHeader {
    uint8_t  type;      // 0=file, 1=dir, 2=symlink
    uint64_t size;
    int64_t  mtime;
    uint32_t name_len;
};

struct PullHeader {
    uint64_t file_size;
};

struct PushHeader {
    uint64_t file_size;
};

struct StatResponse {
    uint8_t  type;      // 0=file, 1=dir, 2=symlink, 255=not found
    uint64_t size;
    int64_t  mtime;
};

#pragma pack(pop)
