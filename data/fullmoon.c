/*
 * fullmoon.c - Unified Moonlight Companion Daemon:
 *              - Zero-idle two-way clipboard synchronization (Wayland data-control)
 *              - On-demand webcam & microphone forwarding with automatic host demand detection
 *              - Zero-CPU V4L2 virtual camera standby handle (permanent Chrome visibility)
 *              - Native Wayland Layer-Shell device selector overlay
 *
 * Architecture Highlights:
 * - Client: Transparent supervisor (--wrap <moonlight>) monitoring Moonlight process group.
 *   - Automatically connects to Sunshine host on TCP port 46003.
 *   - Live two-way authenticated encrypted clipboard sync.
 *   - Dormant camera & mic pipelines (0% CPU, 0 RAM) until host requests streaming.
 * - Host: Socket-activated daemon (--serve) on TCP port 46003 and UDP port 46002.
 *   - Holds zero-CPU standby writer handle on /dev/video10 so Chrome always sees camera.
 *   - Monitors /dev/video10 reader activity and PipeWire microphone recording activity.
 *   - Sends PKT_MEDIA_REQ_START / STOP over persistent encrypted TCP channel.
 *   - Forwards decrypted Opus to 127.0.0.1:46000 and H.264 video to /dev/video10.
 * - End-to-end authenticated encryption: RFC 8439 ChaCha20 + HMAC-SHA256.
 * - Static binary: compiles with -static -s, zero external runtime library dependencies.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <linux/videodev2.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

/* Stub dlopen symbols so static libfreetype never triggers glibc dynamic loader warnings */
void *dlopen(const char *filename, int flags) { (void)filename; (void)flags; return NULL; }
void *dlsym(void *handle, const char *symbol) { (void)handle; (void)symbol; return NULL; }
int dlclose(void *handle) { (void)handle; return 0; }
char *dlerror(void) { return NULL; }

/* ============================================================================
 * Protocol Constants & Definitions
 * ============================================================================ */
#define FULLMOON_MAGIC          0x50494C43U /* "CLIP" in LE */
#define FULLMOON_VERSION        1
#define FULLMOON_TCP_PORT       46003
#define FULLMOON_UDP_PORT       46002
#define FULLMOON_LOCAL_AUDIO    46000
#define MAX_PAYLOAD_BYTES       (16 * 1024 * 1024)
#define CHUNK_SIZE_BYTES        (64 * 1024)
#define RING_DIGESTS_COUNT      16
#define KEY_LEN                 32
#define SALT_LEN                16
#define MAX_KEYS                16
#define MAX_DEVICES             32
#define MAX_PREFS               16
#define TRANSFER_TIMEOUT_MSEC   3000
#define HANDSHAKE_TIMEOUT_MSEC  3000

/* MIME codes for clipboard */
#define MIME_CODE_TEXT_UTF8     1
#define MIME_CODE_PNG           2
#define MIME_CODE_TEXT_PLAIN    3

/* TCP Packet types (Control & Clipboard channel) */
#define PKT_HANDSHAKE_REQ       0x01
#define PKT_HANDSHAKE_RESP      0x02
#define PKT_CLIP_START          0x10
#define PKT_CLIP_CHUNK          0x11
#define PKT_CLIP_END            0x12
#define PKT_PING                0x20
#define PKT_PONG                0x21
#define PKT_MEDIA_REQ_START     0x30
#define PKT_MEDIA_REQ_STOP      0x31
#define PKT_MEDIA_CAPS          0x32

#define MEDIA_FLAG_CAM          0x01
#define MEDIA_FLAG_MIC          0x02

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t fps_num;
    uint16_t fps_denom;
    uint32_t flags;
} MediaVideoCaps;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t fps_num;
    uint16_t fps_denom;
    bool is_mjpeg;
} CamCaps;

/* UDP Packet types (Media streaming channel) */
#define MSTR_MAGIC_0 'M'
#define MSTR_MAGIC_1 'S'
#define MSTR_MAGIC_2 'T'
#define MSTR_MAGIC_3 'R'
#define MSTR_VERSION 1
#define MSTR_HDR_LEN 40
#define MSTR_MAX_PAYLOAD 65000

typedef enum {
    UDP_PKT_AUDIO       = 1,
    UDP_PKT_VIDEO       = 2,
    UDP_PKT_HEARTBEAT   = 3,
    UDP_PKT_RESET_VIDEO = 4,
    UDP_PKT_STOP        = 5
} UdpPacketType;

#define PKT_AUDIO       UDP_PKT_AUDIO
#define PKT_VIDEO       UDP_PKT_VIDEO
#define PKT_HEARTBEAT   UDP_PKT_HEARTBEAT
#define PKT_RESET_VIDEO UDP_PKT_RESET_VIDEO
#define PKT_STOP        UDP_PKT_STOP

/* ============================================================================
 * Safe Memory Utilities & Hardening
 * ============================================================================ */
static void safe_memzero(void *ptr, size_t len) {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
    explicit_bzero(ptr, len);
#else
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len--) *p++ = 0;
#endif
}

static inline int crypto_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    uint8_t res = 0;
    for (size_t i = 0; i < n; i++) res |= (pa[i] ^ pb[i]);
    return res;
}

static uint64_t now_msec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static bool fill_random_bytes(uint8_t *buf, size_t len) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, buf + off, len - off);
        if (n <= 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        off += (size_t)n;
    }
    close(fd);
    return true;
}

static bool net_write_all(int fd, const void *buf, size_t total, int timeout_ms) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t written = 0;
    uint64_t start = now_msec();

    while (written < total) {
        ssize_t n = write(fd, p + written, total - written);
        if (n > 0) {
            written += (size_t)n;
            continue;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                int remain = timeout_ms - (int)(now_msec() - start);
                if (remain <= 0) return false;
                struct pollfd pfd = {.fd = fd, .events = POLLOUT};
                if (poll(&pfd, 1, remain) > 0) continue;
                return false;
            }
            return false;
        }
        return false;
    }
    return true;
}

/* ============================================================================
 * Cryptographic Primitives: SHA-256, HMAC-SHA256, ChaCha20 (RFC 8439)
 * ============================================================================ */
typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t buffer[64];
} Sha256Ctx;

static inline uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static inline uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

static inline uint32_t load_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | ((uint32_t)p[3]);
}

static inline void store_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static inline uint32_t load_le32(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void store_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint64_t load_be64(const uint8_t *p) {
    return ((uint64_t)load_be32(p) << 32) | (uint64_t)load_be32(p + 4);
}

static inline void store_be64(uint8_t *p, uint64_t v) {
    store_be32(p, (uint32_t)(v >> 32));
    store_be32(p + 4, (uint32_t)(v & 0xFFFFFFFFU));
}

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_transform(uint32_t state[8], const uint8_t data[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    uint32_t w[64];

    for (int i = 0; i < 16; i++) {
        w[i] = load_be32(data + i * 4);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha256_init(Sha256Ctx *ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->bitcount = 0;
}

static void sha256_update(Sha256Ctx *ctx, const void *data_in, size_t len) {
    const uint8_t *data = (const uint8_t *)data_in;
    size_t buf_idx = (size_t)((ctx->bitcount >> 3) & 0x3F);
    ctx->bitcount += (uint64_t)len << 3;

    size_t space = 64 - buf_idx;
    if (len >= space) {
        memcpy(ctx->buffer + buf_idx, data, space);
        sha256_transform(ctx->state, ctx->buffer);
        data += space;
        len -= space;
        while (len >= 64) {
            sha256_transform(ctx->state, data);
            data += 64;
            len -= 64;
        }
        buf_idx = 0;
    }
    if (len > 0) {
        memcpy(ctx->buffer + buf_idx, data, len);
    }
}

static void sha256_final(Sha256Ctx *ctx, uint8_t out[32]) {
    size_t buf_idx = (size_t)((ctx->bitcount >> 3) & 0x3F);
    ctx->buffer[buf_idx++] = 0x80;
    if (buf_idx > 56) {
        memset(ctx->buffer + buf_idx, 0, 64 - buf_idx);
        sha256_transform(ctx->state, ctx->buffer);
        buf_idx = 0;
    }
    memset(ctx->buffer + buf_idx, 0, 56 - buf_idx);
    store_be32(ctx->buffer + 56, (uint32_t)(ctx->bitcount >> 32));
    store_be32(ctx->buffer + 60, (uint32_t)ctx->bitcount);
    sha256_transform(ctx->state, ctx->buffer);
    for (int i = 0; i < 8; i++) store_be32(out + i * 4, ctx->state[i]);
    safe_memzero(ctx, sizeof(*ctx));
}

static void sha256(const void *data, size_t len, uint8_t out[32]) {
    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const void *msg1, size_t len1,
                        const void *msg2, size_t len2,
                        uint8_t out[32]) {
    uint8_t k_pad[64];
    memset(k_pad, 0, sizeof(k_pad));
    if (key_len > 64) {
        sha256(key, key_len, k_pad);
    } else {
        memcpy(k_pad, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k_pad[i] ^ 0x36;
        opad[i] = k_pad[i] ^ 0x5c;
    }
    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, 64);
    if (msg1 && len1) sha256_update(&ctx, msg1, len1);
    if (msg2 && len2) sha256_update(&ctx, msg2, len2);
    uint8_t inner[32];
    sha256_final(&ctx, inner);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, 64);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
    safe_memzero(inner, sizeof(inner));
    safe_memzero(k_pad, sizeof(k_pad));
}

#define CHACHA_QUARTERROUND(a, b, c, d) \
    do { \
        a += b; d ^= a; d = (d << 16) | (d >> 16); \
        c += d; b ^= c; b = (b << 12) | (b >> 20); \
        a += b; d ^= a; d = (d <<  8) | (d >> 24); \
        c += d; b ^= c; b = (b <<  7) | (b >> 25); \
    } while (0)

static void chacha20_block(const uint32_t in[16], uint8_t out[64]) {
    uint32_t x[16];
    memcpy(x, in, sizeof(x));
    for (int i = 0; i < 10; i++) {
        CHACHA_QUARTERROUND(x[0], x[4], x[8],  x[12]);
        CHACHA_QUARTERROUND(x[1], x[5], x[9],  x[13]);
        CHACHA_QUARTERROUND(x[2], x[6], x[10], x[14]);
        CHACHA_QUARTERROUND(x[3], x[7], x[11], x[15]);
        CHACHA_QUARTERROUND(x[0], x[5], x[10], x[15]);
        CHACHA_QUARTERROUND(x[1], x[6], x[11], x[12]);
        CHACHA_QUARTERROUND(x[2], x[7], x[8],  x[13]);
        CHACHA_QUARTERROUND(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) {
        store_le32(out + i * 4, x[i] + in[i]);
    }
}

static void chacha20_crypt(const uint8_t key[32], uint32_t counter,
                           const uint8_t nonce[12],
                           const uint8_t *in, uint8_t *out, size_t len) {
    uint32_t state[16] = {
        0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
        load_le32(key + 0),  load_le32(key + 4),  load_le32(key + 8),  load_le32(key + 12),
        load_le32(key + 16), load_le32(key + 20), load_le32(key + 24), load_le32(key + 28),
        counter,
        load_le32(nonce + 0), load_le32(nonce + 4), load_le32(nonce + 8)
    };
    uint8_t block[64];
    while (len > 0) {
        state[12] = counter;
        chacha20_block(state, block);
        size_t chunk = (len < 64) ? len : 64;
        for (size_t i = 0; i < chunk; i++) {
            out[i] = in[i] ^ block[i];
        }
        len -= chunk;
        in += chunk;
        out += chunk;
        counter++;
    }
    safe_memzero(block, sizeof(block));
    safe_memzero(state, sizeof(state));
}

/* ============================================================================
 * Configuration & Key Management
 * ============================================================================ */
typedef struct {
    char target_host[128];
    char lan_host[128];
    char wan_host[128];
    char lan_ssid[128];
    int tcp_port;
    int udp_port;
    int local_audio_port;
    char secret_key_file[512];
    char secret_key_hex[128];
    char ssh_host[128];
    char loopback_device[128];
    char video_encoder[64];
    int video_bitrate;
    int audio_bitrate;
    int idle_timeout_sec;

    uint32_t col_bg;
    uint32_t col_card_bg;
    uint32_t col_card_border;
    uint32_t col_text;
    uint32_t col_muted;
    uint32_t col_accent;
    uint32_t col_active;
    uint32_t col_danger;
} FullmoonConfig;

typedef struct {
    char cam_prefs[MAX_PREFS][256];
    int cam_pref_count;
    bool cam_disabled;

    char mic_prefs[MAX_PREFS][256];
    int mic_pref_count;
    bool mic_disabled;
} PersistentState;

typedef struct {
    char label[128];
    char target[256];
    char devnode[64];
    bool is_disabled_entry;
} DeviceItem;

static FullmoonConfig g_cfg;
static PersistentState g_pref;
static uint8_t g_keys[MAX_KEYS][KEY_LEN];
static int g_key_count = 0;

static char g_home_dir[256] = "";
static char g_runtime_dir[256] = "";
static char g_config_dir[256] = "";
static char g_state_dir[256] = "";
static char g_pid_file[512] = "";
static char g_status_file[512] = "";
static char g_pref_file[512] = "";
static char g_legacy_cam_file[512] = "";
static char g_legacy_mic_file[512] = "";

static void trim_and_unquote(char *s) {
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    if (len >= 2 && ((s[0] == '"' && s[len - 1] == '"') || (s[0] == '\'' && s[len - 1] == '\''))) {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
    }
}

static void expand_tilde_path(const char *in, char *out, size_t out_sz) {
    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0')) {
        snprintf(out, out_sz, "%s%s", g_home_dir, in + 1);
    } else {
        snprintf(out, out_sz, "%s", in);
    }
}

static void mkdir_p(const char *dir_path, mode_t mode) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir_path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    mkdir(tmp, mode);
}

static bool parse_hex_key(const char *hex_str, uint8_t out[KEY_LEN]) {
    while (*hex_str && isspace((unsigned char)*hex_str)) hex_str++;
    for (int i = 0; i < KEY_LEN; i++) {
        char c1 = hex_str[i * 2];
        char c2 = hex_str[i * 2 + 1];
        if (!c1 || !c2) return false;
        int hi = (c1 >= '0' && c1 <= '9') ? (c1 - '0') :
                 (c1 >= 'a' && c1 <= 'f') ? (c1 - 'a' + 10) :
                 (c1 >= 'A' && c1 <= 'F') ? (c1 - 'A' + 10) : -1;
        int lo = (c2 >= '0' && c2 <= '9') ? (c2 - '0') :
                 (c2 >= 'a' && c2 <= 'f') ? (c2 - 'a' + 10) :
                 (c2 >= 'A' && c2 <= 'F') ? (c2 - 'A' + 10) : -1;
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool ensure_keys(void) {
    if (g_key_count > 0) return true;

    if (g_cfg.secret_key_hex[0] && parse_hex_key(g_cfg.secret_key_hex, g_keys[0])) {
        g_key_count = 1;
    }

    char path[512];
    expand_tilde_path(g_cfg.secret_key_file, path, sizeof(path));
    FILE *fp = fopen(path, "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            char *s = line;
            while (*s && isspace((unsigned char)*s)) s++;
            if (*s == '#' || *s == '\0') continue;
            if (g_key_count < MAX_KEYS) {
                uint8_t k[KEY_LEN];
                if (parse_hex_key(s, k)) {
                    bool dup = false;
                    for (int i = 0; i < g_key_count; i++) {
                        if (memcmp(g_keys[i], k, KEY_LEN) == 0) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup) {
                        memcpy(g_keys[g_key_count], k, KEY_LEN);
                        g_key_count++;
                    }
                }
            }
        }
        fclose(fp);
        chmod(path, 0600);
    }

    if (g_key_count > 0) return true;

    /* Generate new secret key if none found */
    if (!fill_random_bytes(g_keys[0], KEY_LEN)) return false;
    g_key_count = 1;

    char dir_buf[512];
    snprintf(dir_buf, sizeof(dir_buf), "%s", path);
    char *slash = strrchr(dir_buf, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(dir_buf, 0700);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        char hex[KEY_LEN * 2 + 2];
        static const char hexchars[] = "0123456789abcdef";
        for (int i = 0; i < KEY_LEN; i++) {
            hex[i * 2]     = hexchars[(g_keys[0][i] >> 4) & 0xF];
            hex[i * 2 + 1] = hexchars[g_keys[0][i] & 0xF];
        }
        hex[KEY_LEN * 2] = '\n';
        hex[KEY_LEN * 2 + 1] = '\0';
        ssize_t w = write(fd, hex, KEY_LEN * 2 + 1);
        (void)w;
        close(fd);
    }
    return true;
}

static uint32_t parse_color_spec(const char *spec, uint32_t fallback) {
    if (!spec) return fallback;
    while (*spec && isspace((unsigned char)*spec)) spec++;
    if (!*spec) return fallback;

    if (strncasecmp(spec, "rgba", 4) == 0 || strncasecmp(spec, "rgb", 3) == 0) {
        const char *p = strchr(spec, '(');
        if (p) {
            p++;
            int r = 0, g = 0, b = 0;
            double a = 1.0;
            if (sscanf(p, "%d , %d , %d , %lf", &r, &g, &b, &a) >= 3 ||
                sscanf(p, "%d %d %d %lf", &r, &g, &b, &a) >= 3 ||
                sscanf(p, "%d,%d,%d,%lf", &r, &g, &b, &a) >= 3) {
                if (r < 0) r = 0; else if (r > 255) r = 255;
                if (g < 0) g = 0; else if (g > 255) g = 255;
                if (b < 0) b = 0; else if (b > 255) b = 255;
                uint32_t u_a = (a <= 1.0 && a >= 0.0) ? (uint32_t)(a * 255.0 + 0.5)
                             : ((a > 1.0 && a <= 255.0) ? (uint32_t)(a + 0.5) : 255);
                return (u_a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }

    const char *hex = (*spec == '#') ? spec + 1 : spec;
    size_t len = 0;
    while (isxdigit((unsigned char)hex[len])) len++;
    if (len == 6) {
        uint32_t rgb = (uint32_t)strtoul(hex, NULL, 16);
        return 0xFF000000U | (rgb & 0xFFFFFFU);
    } else if (len == 8) {
        char buf[9];
        memcpy(buf, hex, 8);
        buf[8] = '\0';
        uint32_t val = (uint32_t)strtoul(buf, NULL, 16);
        return ((val & 0xFFU) << 24) | ((val >> 8) & 0xFFFFFFU);
    }
    return fallback;
}

static void load_config(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    snprintf(g_cfg.target_host, sizeof(g_cfg.target_host), "auto");
    snprintf(g_cfg.lan_host, sizeof(g_cfg.lan_host), "192.168.1.12");
    snprintf(g_cfg.wan_host, sizeof(g_cfg.wan_host), "sncr.duckdns.org");
    snprintf(g_cfg.lan_ssid, sizeof(g_cfg.lan_ssid), "ev");
    g_cfg.tcp_port = FULLMOON_TCP_PORT;
    g_cfg.udp_port = FULLMOON_UDP_PORT;
    g_cfg.local_audio_port = FULLMOON_LOCAL_AUDIO;
    snprintf(g_cfg.secret_key_file, sizeof(g_cfg.secret_key_file), "%s/.dotfiles/tmp/meet-stream.key", g_home_dir);
    snprintf(g_cfg.ssh_host, sizeof(g_cfg.ssh_host), "ev");
    snprintf(g_cfg.loopback_device, sizeof(g_cfg.loopback_device), "/dev/video10");
    snprintf(g_cfg.video_encoder, sizeof(g_cfg.video_encoder), "auto");
    g_cfg.video_bitrate = 2000;
    g_cfg.audio_bitrate = 64000;
    g_cfg.idle_timeout_sec = 5;

    g_cfg.col_bg          = 0xD90E1016U;
    g_cfg.col_card_bg     = 0xF5181A20U;
    g_cfg.col_card_border = 0xFF3EAEFFU;
    g_cfg.col_text        = 0xFFFFFFFFU;
    g_cfg.col_muted       = 0xFF888888U;
    g_cfg.col_accent      = 0xFF3EAEFFU;
    g_cfg.col_active      = 0xFF2ECC71U;
    g_cfg.col_danger      = 0xFFCC6666U;

    char cpaths[3][512];
    snprintf(cpaths[0], sizeof(cpaths[0]), "%s/meet-stream.conf", g_config_dir);
    snprintf(cpaths[1], sizeof(cpaths[1]), "%s/sway/meet-stream.conf", g_config_dir);
    snprintf(cpaths[2], sizeof(cpaths[2]), "%s/.dotfiles/meet-stream.conf", g_home_dir);

    for (int ci = 0; ci < 3; ci++) {
        FILE *fp = fopen(cpaths[ci], "r");
        if (!fp) continue;
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            char *p = line;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '#' || *p == '\0') continue;
            char *eq = strchr(p, '=');
            if (!eq) continue;
            *eq = '\0';
            char *key = p;
            char *val = eq + 1;
            trim_and_unquote(key);
            trim_and_unquote(val);

            if (strcasecmp(key, "target_host") == 0) snprintf(g_cfg.target_host, sizeof(g_cfg.target_host), "%s", val);
            else if (strcasecmp(key, "lan_host") == 0) snprintf(g_cfg.lan_host, sizeof(g_cfg.lan_host), "%s", val);
            else if (strcasecmp(key, "wan_host") == 0) snprintf(g_cfg.wan_host, sizeof(g_cfg.wan_host), "%s", val);
            else if (strcasecmp(key, "lan_ssid") == 0) snprintf(g_cfg.lan_ssid, sizeof(g_cfg.lan_ssid), "%s", val);
            else if (strcasecmp(key, "port") == 0) g_cfg.udp_port = atoi(val);
            else if (strcasecmp(key, "clip_port") == 0) g_cfg.tcp_port = atoi(val);
            else if (strcasecmp(key, "local_audio_port") == 0) g_cfg.local_audio_port = atoi(val);
            else if (strcasecmp(key, "secret_key_file") == 0) expand_tilde_path(val, g_cfg.secret_key_file, sizeof(g_cfg.secret_key_file));
            else if (strcasecmp(key, "secret_key") == 0) snprintf(g_cfg.secret_key_hex, sizeof(g_cfg.secret_key_hex), "%s", val);
            else if (strcasecmp(key, "ssh_host") == 0) snprintf(g_cfg.ssh_host, sizeof(g_cfg.ssh_host), "%s", val);
            else if (strcasecmp(key, "loopback_device") == 0) snprintf(g_cfg.loopback_device, sizeof(g_cfg.loopback_device), "%s", val);
            else if (strcasecmp(key, "video_encoder") == 0) snprintf(g_cfg.video_encoder, sizeof(g_cfg.video_encoder), "%s", val);
            else if (strcasecmp(key, "video_bitrate") == 0) g_cfg.video_bitrate = atoi(val);
            else if (strcasecmp(key, "audio_bitrate") == 0) g_cfg.audio_bitrate = atoi(val);
            else if (strcasecmp(key, "idle_timeout_sec") == 0) g_cfg.idle_timeout_sec = atoi(val);
            else if (strcasecmp(key, "bg_color") == 0) g_cfg.col_bg = parse_color_spec(val, g_cfg.col_bg);
            else if (strcasecmp(key, "card_bg") == 0) g_cfg.col_card_bg = parse_color_spec(val, g_cfg.col_card_bg);
            else if (strcasecmp(key, "card_border") == 0) g_cfg.col_card_border = parse_color_spec(val, g_cfg.col_card_border);
            else if (strcasecmp(key, "text_color") == 0) g_cfg.col_text = parse_color_spec(val, g_cfg.col_text);
            else if (strcasecmp(key, "muted_color") == 0) g_cfg.col_muted = parse_color_spec(val, g_cfg.col_muted);
            else if (strcasecmp(key, "accent_color") == 0) g_cfg.col_accent = parse_color_spec(val, g_cfg.col_accent);
            else if (strcasecmp(key, "active_color") == 0) g_cfg.col_active = parse_color_spec(val, g_cfg.col_active);
            else if (strcasecmp(key, "danger_color") == 0) g_cfg.col_danger = parse_color_spec(val, g_cfg.col_danger);
        }
        fclose(fp);
        break;
    }
}

/* ============================================================================
 * TCP Control & Clipboard Framing
 * ============================================================================ */
typedef struct {
    uint8_t key_send[32];
    uint8_t key_recv[32];
    uint8_t salt[SALT_LEN];
    uint32_t seq_send;
    uint32_t seq_recv;
    bool established;
} SessionCrypto;

static void derive_session_keys(const uint8_t master_key[32],
                                const uint8_t salt[SALT_LEN],
                                bool is_client,
                                SessionCrypto *sc) {
    uint8_t k_c2h[32], k_h2c[32];
    hmac_sha256(master_key, 32, "CLIP_C2H", 8, salt, SALT_LEN, k_c2h);
    hmac_sha256(master_key, 32, "CLIP_H2C", 8, salt, SALT_LEN, k_h2c);

    if (is_client) {
        memcpy(sc->key_send, k_c2h, 32);
        memcpy(sc->key_recv, k_h2c, 32);
    } else {
        memcpy(sc->key_send, k_h2c, 32);
        memcpy(sc->key_recv, k_c2h, 32);
    }
    memcpy(sc->salt, salt, SALT_LEN);
    sc->seq_send = 1;
    sc->seq_recv = 1;
    sc->established = true;
    safe_memzero(k_c2h, sizeof(k_c2h));
    safe_memzero(k_h2c, sizeof(k_h2c));
}

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;      /* FULLMOON_MAGIC */
    uint8_t  version;    /* FULLMOON_VERSION */
    uint8_t  type;       /* PKT_* */
    uint8_t  salt[SALT_LEN];
    uint8_t  hmac[16];   /* Truncated HMAC-SHA256 */
} HandshakePacket;

typedef struct {
    uint32_t magic;      /* FULLMOON_MAGIC */
    uint8_t  version;    /* FULLMOON_VERSION */
    uint8_t  type;       /* PKT_* */
    uint16_t mime_code;  /* MIME_CODE_* */
    uint32_t total_len;  /* Total payload bytes */
    uint16_t chunk_seq;  /* 0, 1, ... */
    uint16_t chunk_len;  /* Ciphertext bytes in this frame */
    uint32_t seq;        /* Sequence number */
    uint8_t  nonce[12];  /* Nonce for ChaCha20 */
    uint8_t  hmac[16];   /* Truncated HMAC-SHA256 */
} FrameHeader;
#pragma pack(pop)

static void make_nonce(const uint8_t salt[SALT_LEN], uint32_t seq, uint8_t nonce[12]) {
    memcpy(nonce, salt, 8);
    store_be32(nonce + 8, seq);
}

static bool seal_frame(SessionCrypto *sc, uint8_t type, uint16_t mime_code,
                       uint32_t total_len, uint16_t chunk_seq,
                       const uint8_t *plain, uint16_t chunk_len,
                       uint8_t *wire_buf, size_t *wire_len) {
    FrameHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = htonl(FULLMOON_MAGIC);
    hdr.version = FULLMOON_VERSION;
    hdr.type = type;
    hdr.mime_code = htons(mime_code);
    hdr.total_len = htonl(total_len);
    hdr.chunk_seq = htons(chunk_seq);
    hdr.chunk_len = htons(chunk_len);
    hdr.seq = htonl(sc->seq_send);

    make_nonce(sc->salt, sc->seq_send, hdr.nonce);
    sc->seq_send++;

    uint8_t *cipher = wire_buf + sizeof(FrameHeader);
    if (chunk_len > 0 && plain) {
        chacha20_crypt(sc->key_send, 1, hdr.nonce, plain, cipher, chunk_len);
    }

    uint8_t full_hmac[32];
    size_t hdr_auth_len = sizeof(FrameHeader) - 16;
    hmac_sha256(sc->key_send, 32, &hdr, hdr_auth_len, cipher, chunk_len, full_hmac);
    memcpy(hdr.hmac, full_hmac, 16);
    safe_memzero(full_hmac, sizeof(full_hmac));

    memcpy(wire_buf, &hdr, sizeof(FrameHeader));
    *wire_len = sizeof(FrameHeader) + chunk_len;
    return true;
}

static bool open_frame(SessionCrypto *sc, const uint8_t *wire_buf, size_t wire_len,
                       FrameHeader *out_hdr, uint8_t *plain_out) {
    if (wire_len < sizeof(FrameHeader)) return false;
    memcpy(out_hdr, wire_buf, sizeof(FrameHeader));

    if (ntohl(out_hdr->magic) != FULLMOON_MAGIC || out_hdr->version != FULLMOON_VERSION) {
        return false;
    }
    uint16_t chunk_len = ntohs(out_hdr->chunk_len);
    if (wire_len != sizeof(FrameHeader) + chunk_len) return false;

    uint8_t full_hmac[32];
    size_t hdr_auth_len = sizeof(FrameHeader) - 16;
    const uint8_t *cipher = wire_buf + sizeof(FrameHeader);
    hmac_sha256(sc->key_recv, 32, out_hdr, hdr_auth_len, cipher, chunk_len, full_hmac);

    if (crypto_memcmp(full_hmac, out_hdr->hmac, 16) != 0) {
        safe_memzero(full_hmac, sizeof(full_hmac));
        return false;
    }
    safe_memzero(full_hmac, sizeof(full_hmac));

    if (chunk_len > 0 && plain_out) {
        chacha20_crypt(sc->key_recv, 1, out_hdr->nonce, cipher, plain_out, chunk_len);
    }
    return true;
}

/* ============================================================================
 * UDP Media Framing & Replay Protection
 * ============================================================================ */
static size_t mstr_seal_packet(const uint8_t key[32], uint8_t pkt_type,
                               uint64_t session_id, uint64_t seq,
                               const uint8_t *payload, uint16_t payload_len,
                               uint8_t *out_pkt) {
    out_pkt[0] = MSTR_MAGIC_0;
    out_pkt[1] = MSTR_MAGIC_1;
    out_pkt[2] = MSTR_MAGIC_2;
    out_pkt[3] = MSTR_MAGIC_3;
    out_pkt[4] = MSTR_VERSION;
    out_pkt[5] = pkt_type;
    out_pkt[6] = (uint8_t)(payload_len >> 8);
    out_pkt[7] = (uint8_t)(payload_len & 0xFF);
    store_be64(out_pkt + 8, session_id);
    store_be64(out_pkt + 16, seq);

    uint8_t nonce[12];
    store_be32(nonce, (uint32_t)(session_id >> 32) ^ (uint32_t)session_id);
    store_be64(nonce + 4, seq);

    if (payload_len > 0 && payload) {
        chacha20_crypt(key, 1, nonce, payload, out_pkt + MSTR_HDR_LEN, payload_len);
    }

    uint8_t full_mac[32];
    hmac_sha256(key, 32, out_pkt, 24, out_pkt + MSTR_HDR_LEN, payload_len, full_mac);
    memcpy(out_pkt + 24, full_mac, 16);

    return (size_t)MSTR_HDR_LEN + payload_len;
}

static bool mstr_open_packet(const uint8_t key[32], const uint8_t *in_pkt, size_t in_len,
                             uint8_t *out_type, uint64_t *out_session, uint64_t *out_seq,
                             uint8_t *out_payload, uint16_t *out_payload_len) {
    if (in_len < MSTR_HDR_LEN) return false;
    if (in_pkt[0] != MSTR_MAGIC_0 || in_pkt[1] != MSTR_MAGIC_1 ||
        in_pkt[2] != MSTR_MAGIC_2 || in_pkt[3] != MSTR_MAGIC_3) {
        return false;
    }
    if (in_pkt[4] != MSTR_VERSION) return false;

    uint8_t pkt_type = in_pkt[5];
    uint16_t plen = (uint16_t)((in_pkt[6] << 8) | in_pkt[7]);
    if ((size_t)MSTR_HDR_LEN + plen != in_len) return false;

    uint8_t full_mac[32];
    hmac_sha256(key, 32, in_pkt, 24, in_pkt + MSTR_HDR_LEN, plen, full_mac);
    if (crypto_memcmp(in_pkt + 24, full_mac, 16) != 0) {
        return false;
    }

    uint64_t session_id = load_be64(in_pkt + 8);
    uint64_t seq = load_be64(in_pkt + 16);

    if (plen > 0 && out_payload) {
        uint8_t nonce[12];
        store_be32(nonce, (uint32_t)(session_id >> 32) ^ (uint32_t)session_id);
        store_be64(nonce + 4, seq);
        chacha20_crypt(key, 1, nonce, in_pkt + MSTR_HDR_LEN, out_payload, plen);
    }

    if (out_type) *out_type = pkt_type;
    if (out_session) *out_session = session_id;
    if (out_seq) *out_seq = seq;
    if (out_payload_len) *out_payload_len = plen;
    return true;
}

typedef struct {
    uint64_t session_id;
    uint64_t max_seq;
    uint64_t window_bitmap;
    bool initialized;
} ReplayWindow;

static bool replay_check_and_update(ReplayWindow *rw, uint64_t session_id, uint64_t seq) {
    if (!rw->initialized || rw->session_id != session_id) {
        rw->session_id = session_id;
        rw->max_seq = seq;
        rw->window_bitmap = 1ULL;
        rw->initialized = true;
        return true;
    }
    if (seq > rw->max_seq) {
        uint64_t shift = seq - rw->max_seq;
        if (shift >= 64) rw->window_bitmap = 1ULL;
        else rw->window_bitmap = (rw->window_bitmap << shift) | 1ULL;
        rw->max_seq = seq;
        return true;
    }
    uint64_t diff = rw->max_seq - seq;
    if (diff >= 64) return false;
    uint64_t mask = 1ULL << diff;
    if (rw->window_bitmap & mask) return false;
    rw->window_bitmap |= mask;
    return true;
}

/* ============================================================================
 * Echo Suppression Ring Buffer
 * ============================================================================ */
typedef struct {
    uint8_t digests[RING_DIGESTS_COUNT][32];
    int head;
} EchoRing;

static EchoRing g_echo_ring;

static void echo_ring_add(const uint8_t digest[32]) {
    memcpy(g_echo_ring.digests[g_echo_ring.head], digest, 32);
    g_echo_ring.head = (g_echo_ring.head + 1) % RING_DIGESTS_COUNT;
}

static bool echo_ring_contains(const uint8_t digest[32]) {
    for (int i = 0; i < RING_DIGESTS_COUNT; i++) {
        if (crypto_memcmp(g_echo_ring.digests[i], digest, 32) == 0) {
            return true;
        }
    }
    return false;
}

/* ============================================================================
 * Raw Wayland Wire Engine (zwlr_data_control_v1 & ext_data_control_v1)
 * ============================================================================ */
static int g_wl_fd = -1;
static uint32_t g_wl_next_id = 2;
static uint32_t g_id_registry = 0;
static uint32_t g_id_seat = 0;
static uint32_t g_id_manager = 0;
static uint32_t g_id_device = 0;
static uint32_t g_id_active_source = 0;
static bool g_we_own_selection = false;

static void wl_send_msg(uint32_t obj_id, uint16_t opcode, const void *payload, size_t payload_len, int fd_to_pass);

typedef struct {
    uint32_t id;
    char mime[128];
} WaylandOffer;

static WaylandOffer g_offers[16];
static int g_offer_head = 0;

static void offer_register(uint32_t offer_id) {
    for (int i = 0; i < 16; i++) {
        if (g_offers[i].id == offer_id) {
            g_offers[i].mime[0] = '\0';
            return;
        }
    }
    int idx = g_offer_head;
    g_offer_head = (g_offer_head + 1) % 16;
    if (g_offers[idx].id != 0) {
        wl_send_msg(g_offers[idx].id, 1, NULL, 0, -1);
    }
    g_offers[idx].id = offer_id;
    g_offers[idx].mime[0] = '\0';
}

static WaylandOffer *offer_find(uint32_t offer_id) {
    for (int i = 0; i < 16; i++) {
        if (g_offers[i].id == offer_id) return &g_offers[i];
    }
    return NULL;
}

static uint8_t *g_current_clip_data = NULL;
static size_t g_current_clip_len = 0;
static uint16_t g_current_clip_mime = MIME_CODE_TEXT_UTF8;

#define MAX_QUEUED_FDS 8
static int g_fd_queue[MAX_QUEUED_FDS];
static int g_fd_queue_count = 0;

static void fd_queue_push(int fd) {
    if (g_fd_queue_count < MAX_QUEUED_FDS) {
        g_fd_queue[g_fd_queue_count++] = fd;
    } else {
        close(fd);
    }
}

static int fd_queue_pop(void) {
    if (g_fd_queue_count <= 0) return -1;
    int fd = g_fd_queue[0];
    for (int i = 1; i < g_fd_queue_count; i++) {
        g_fd_queue[i - 1] = g_fd_queue[i];
    }
    g_fd_queue_count--;
    return fd;
}

static void wl_send_msg(uint32_t obj_id, uint16_t opcode, const void *payload, size_t payload_len, int fd_to_pass) {
    if (g_wl_fd < 0) return;
    uint32_t hdr[2];
    hdr[0] = obj_id;
    hdr[1] = ((uint32_t)(8 + payload_len) << 16) | opcode;

    struct iovec iov[2];
    iov[0].iov_base = hdr;
    iov[0].iov_len = 8;
    iov[1].iov_base = (void *)payload;
    iov[1].iov_len = payload_len;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = payload_len > 0 ? 2 : 1;

    union {
        struct cmsghdr cm;
        char control[CMSG_SPACE(sizeof(int))];
    } cmsg_un;

    if (fd_to_pass >= 0) {
        msg.msg_control = cmsg_un.control;
        msg.msg_controllen = sizeof(cmsg_un.control);
        struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd_to_pass, sizeof(int));
    }

    sendmsg(g_wl_fd, &msg, MSG_NOSIGNAL);
}

static void wl_send_u32_1(uint32_t obj, uint16_t op, uint32_t a) {
    wl_send_msg(obj, op, &a, 4, -1);
}

static void wl_send_u32_2(uint32_t obj, uint16_t op, uint32_t a, uint32_t b) {
    uint32_t p[2] = {a, b};
    wl_send_msg(obj, op, p, 8, -1);
}

static void wl_send_string(uint32_t obj, uint16_t op, const char *str) {
    uint32_t slen = (uint32_t)strlen(str) + 1;
    uint32_t pad = (4 - (slen % 4)) % 4;
    size_t payload_len = 4 + slen + pad;
    uint8_t *buf = (uint8_t *)calloc(1, payload_len);
    if (!buf) return;
    store_le32(buf, slen);
    memcpy(buf + 4, str, slen);
    wl_send_msg(obj, op, buf, payload_len, -1);
    free(buf);
}

static void wl_registry_bind(uint32_t name, const char *iface, uint32_t version, uint32_t new_id) {
    uint32_t buf[64];
    buf[0] = name;
    uint32_t slen = (uint32_t)strlen(iface) + 1;
    buf[1] = slen;
    memset(buf + 2, 0, ((slen + 3) & ~3));
    memcpy(buf + 2, iface, slen);
    int idx = 2 + (int)((slen + 3) / 4);
    buf[idx++] = version;
    buf[idx++] = new_id;
    wl_send_msg(g_id_registry, 0, buf, (size_t)idx * 4, -1);
}

static void broadcast_local_clipboard(const uint8_t *data, size_t len, uint16_t mime_code);
static void handle_remote_clipboard(const uint8_t *data, size_t len, uint16_t mime_code);

static uint8_t *read_pipe_all(int fd, size_t *out_len, uint64_t timeout_msec) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    size_t cap = 16384;
    size_t sz = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) { close(fd); return NULL; }

    uint64_t start = now_msec();
    struct pollfd pfd = {.fd = fd, .events = POLLIN};

    while (sz < MAX_PAYLOAD_BYTES) {
        int remain = (int)(timeout_msec - (now_msec() - start));
        if (remain <= 0) break;
        int ret = poll(&pfd, 1, remain);
        if (ret <= 0) break;

        if (pfd.revents & (POLLIN | POLLHUP)) {
            if (sz + 4096 > cap) {
                cap *= 2;
                if (cap > MAX_PAYLOAD_BYTES) cap = MAX_PAYLOAD_BYTES + 1;
                uint8_t *nb = (uint8_t *)realloc(buf, cap);
                if (!nb) break;
                buf = nb;
            }
            ssize_t n = read(fd, buf + sz, cap - sz);
            if (n == 0) break;
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                break;
            }
            sz += (size_t)n;
        }
    }
    close(fd);
    *out_len = sz;
    return buf;
}

static void wayland_cleanup(void) {
    if (g_wl_fd >= 0) {
        close(g_wl_fd);
        g_wl_fd = -1;
    }
    g_id_registry = 0;
    g_id_seat = 0;
    g_id_manager = 0;
    g_id_device = 0;
    g_id_active_source = 0;
    g_we_own_selection = false;
}

static void process_wayland_events(void) {
    if (g_wl_fd < 0) return;
    static uint8_t rbuf[65536];
    static size_t rfilled = 0;

    while (rfilled < sizeof(rbuf)) {
        struct iovec iov = {.iov_base = rbuf + rfilled, .iov_len = sizeof(rbuf) - rfilled};
        char cmsg_buf[CMSG_SPACE(sizeof(int) * MAX_QUEUED_FDS)];
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);

        ssize_t n = recvmsg(g_wl_fd, &msg, MSG_DONTWAIT);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            wayland_cleanup();
            return;
        }

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        while (cmsg) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                int count = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                int *fds = (int *)CMSG_DATA(cmsg);
                for (int i = 0; i < count; i++) {
                    fd_queue_push(fds[i]);
                }
            }
            cmsg = CMSG_NXTHDR(&msg, cmsg);
        }

        rfilled += (size_t)n;
    }

    size_t off = 0;
    while (rfilled - off >= 8) {
        uint32_t obj_id = load_le32(rbuf + off);
        uint32_t val1 = load_le32(rbuf + off + 4);
        uint16_t msg_len = (uint16_t)(val1 >> 16);
        uint16_t opcode = (uint16_t)(val1 & 0xFFFF);

        if (msg_len < 8 || (off + msg_len) > rfilled) break;

        const uint8_t *payload = rbuf + off + 8;
        size_t plen = msg_len - 8;

        if (obj_id == g_id_registry && opcode == 0) {
            if (plen >= 12) {
                uint32_t name = load_le32(payload);
                uint32_t slen = load_le32(payload + 4);
                if (8 + slen <= plen) {
                    const char *iface = (const char *)(payload + 8);
                    size_t pad = (4 - (slen % 4)) % 4;
                    size_t voff = 8 + slen + pad;
                    if (voff + 4 <= plen) {
                        uint32_t version = load_le32(payload + voff);
                        if (strcmp(iface, "wl_seat") == 0 && g_id_seat == 0) {
                            g_id_seat = g_wl_next_id++;
                            wl_registry_bind(name, "wl_seat", version < 7 ? version : 7, g_id_seat);
                        } else if ((strcmp(iface, "zwlr_data_control_manager_v1") == 0 ||
                                    strcmp(iface, "ext_data_control_manager_v1") == 0) && g_id_manager == 0) {
                            g_id_manager = g_wl_next_id++;
                            wl_registry_bind(name, iface, version < 2 ? version : 2, g_id_manager);
                        }
                    }
                }
            }
        } else if (obj_id == g_id_device) {
            if (opcode == 0 /* data_offer(id) */) {
                if (plen >= 4) {
                    uint32_t offer_id = load_le32(payload);
                    offer_register(offer_id);
                }
            } else if (opcode == 1 /* selection(id) */) {
                if (plen >= 4) {
                    uint32_t offer_id = load_le32(payload);
                    if (offer_id == 0) {
                        /* Selection cleared */
                    } else if (g_we_own_selection) {
                        g_we_own_selection = false;
                    } else {
                        WaylandOffer *offr = offer_find(offer_id);
                        if (offr && offr->mime[0]) {
                            int pipefds[2];
                            if (pipe2(pipefds, O_CLOEXEC) == 0) {
                                uint32_t slen = (uint32_t)strlen(offr->mime) + 1;
                                uint32_t pad = (4 - (slen % 4)) % 4;
                                size_t plen_send = 4 + slen + pad;
                                uint8_t *mbuf = (uint8_t *)calloc(1, plen_send);
                                if (mbuf) {
                                    store_le32(mbuf, slen);
                                    memcpy(mbuf + 4, offr->mime, slen);
                                    wl_send_msg(offer_id, 0 /* receive */, mbuf, plen_send, pipefds[1]);
                                    free(mbuf);
                                }
                                close(pipefds[1]);

                                size_t clen = 0;
                                uint8_t *cdata = read_pipe_all(pipefds[0], &clen, TRANSFER_TIMEOUT_MSEC);
                                if (cdata && clen > 0) {
                                    uint8_t dig[32];
                                    sha256(cdata, clen, dig);
                                    if (!echo_ring_contains(dig)) {
                                        echo_ring_add(dig);
                                        uint16_t mcode = (strcmp(offr->mime, "image/png") == 0) ? MIME_CODE_PNG : MIME_CODE_TEXT_UTF8;
                                        broadcast_local_clipboard(cdata, clen, mcode);
                                    }
                                }
                                free(cdata);
                            }
                        }
                    }
                }
            }
        } else if (obj_id == g_id_active_source) {
            if (opcode == 0 /* send(mime, fd) */) {
                int passed_fd = fd_queue_pop();
                if (passed_fd >= 0) {
                    if (g_current_clip_data && g_current_clip_len > 0) {
                        int fl = fcntl(passed_fd, F_GETFL, 0);
                        fcntl(passed_fd, F_SETFL, fl | O_NONBLOCK);
                        size_t written = 0;
                        while (written < g_current_clip_len) {
                            ssize_t nw = write(passed_fd, g_current_clip_data + written, g_current_clip_len - written);
                            if (nw <= 0) {
                                if (nw < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                                    struct pollfd wfd = {.fd = passed_fd, .events = POLLOUT};
                                    if (poll(&wfd, 1, 500) > 0) continue;
                                }
                                break;
                            }
                            written += (size_t)nw;
                        }
                    }
                    close(passed_fd);
                }
            } else if (opcode == 1 /* cancelled */) {
                g_we_own_selection = false;
                wl_send_msg(g_id_active_source, 1 /* destroy */, NULL, 0, -1);
                g_id_active_source = 0;
            }
        } else {
            WaylandOffer *offr = offer_find(obj_id);
            if (offr && opcode == 0 /* offer(mime) */ && plen >= 4) {
                uint32_t slen = load_le32(payload);
                if (4 + slen <= plen) {
                    const char *m = (const char *)(payload + 4);
                    if (strcmp(m, "text/plain;charset=utf-8") == 0 ||
                        strcmp(m, "UTF8_STRING") == 0 ||
                        strcmp(m, "text/plain") == 0) {
                        if (strcmp(offr->mime, "image/png") != 0) {
                            snprintf(offr->mime, sizeof(offr->mime), "%s", m);
                        }
                    } else if (strcmp(m, "image/png") == 0) {
                        snprintf(offr->mime, sizeof(offr->mime), "image/png");
                    }
                }
            }
        }

        off += msg_len;
    }

    if (off > 0) {
        if (off < rfilled) memmove(rbuf, rbuf + off, rfilled - off);
        rfilled -= off;
    }

    if (g_id_seat != 0 && g_id_manager != 0 && g_id_device == 0) {
        g_id_device = g_wl_next_id++;
        wl_send_u32_2(g_id_manager, 1 /* get_data_device */, g_id_device, g_id_seat);
    }
}

static bool wayland_init(void) {
    if (g_wl_fd >= 0) return true;

    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir || !*runtime_dir) runtime_dir = "/run/user/1000";

    char wl_path[512] = "";
    const char *wd = getenv("WAYLAND_DISPLAY");
    const char *sway_sock = getenv("SWAYSOCK");

    if (sway_sock && strstr(sway_sock, "sway-sunshine.sock")) {
        snprintf(wl_path, sizeof(wl_path), "%s/wayland-1", runtime_dir);
    } else if (wd && wd[0] != '\0') {
        if (wd[0] == '/') snprintf(wl_path, sizeof(wl_path), "%s", wd);
        else snprintf(wl_path, sizeof(wl_path), "%s/%s", runtime_dir, wd);
    } else {
        snprintf(wl_path, sizeof(wl_path), "%s/wayland-0", runtime_dir);
    }

    g_wl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (g_wl_fd < 0) return false;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, wl_path, sizeof(sa.sun_path) - 1);

    if (connect(g_wl_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(g_wl_fd);
        g_wl_fd = -1;
        return false;
    }

    fcntl(g_wl_fd, F_SETFL, fcntl(g_wl_fd, F_GETFL, 0) | O_NONBLOCK);

    g_id_registry = g_wl_next_id++;
    wl_send_u32_1(1 /* wl_display */, 1 /* get_registry */, g_id_registry);
    uint32_t sync_cb = g_wl_next_id++;
    wl_send_u32_1(1 /* wl_display */, 0 /* sync */, sync_cb);

    uint64_t start = now_msec();
    while ((g_id_seat == 0 || g_id_manager == 0) && (now_msec() - start < 1500)) {
        struct pollfd pfd = {.fd = g_wl_fd, .events = POLLIN};
        if (poll(&pfd, 1, 200) > 0) {
            process_wayland_events();
        }
    }

    if (g_id_seat == 0 || g_id_manager == 0) {
        wayland_cleanup();
        return false;
    }

    if (g_id_device == 0) {
        g_id_device = g_wl_next_id++;
        wl_send_u32_2(g_id_manager, 1 /* get_data_device */, g_id_device, g_id_seat);
    }
    return true;
}

static void handle_remote_clipboard(const uint8_t *data, size_t len, uint16_t mime_code) {
    if (!data || len == 0 || g_wl_fd < 0 || g_id_manager == 0 || g_id_device == 0) return;

    uint8_t dig[32];
    sha256(data, len, dig);
    echo_ring_add(dig);
    g_we_own_selection = true;

    if (g_id_active_source != 0) {
        wl_send_msg(g_id_active_source, 1 /* destroy */, NULL, 0, -1);
        g_id_active_source = 0;
    }

    if (g_current_clip_data) free(g_current_clip_data);
    g_current_clip_data = (uint8_t *)malloc(len);
    if (!g_current_clip_data) return;
    memcpy(g_current_clip_data, data, len);
    g_current_clip_len = len;
    g_current_clip_mime = mime_code;

    /* Create new data control source (opcode 0) */
    g_id_active_source = g_wl_next_id++;
    wl_send_u32_1(g_id_manager, 0 /* create_data_source */, g_id_active_source);

    /* Advertise MIME types (opcode 0 on source) */
    if (mime_code == MIME_CODE_PNG) {
        wl_send_string(g_id_active_source, 0 /* offer */, "image/png");
    } else {
        wl_send_string(g_id_active_source, 0 /* offer */, "text/plain;charset=utf-8");
        wl_send_string(g_id_active_source, 0 /* offer */, "text/plain");
        wl_send_string(g_id_active_source, 0 /* offer */, "UTF8_STRING");
    }

    /* Set selection on data device (opcode 0 on device) */
    wl_send_u32_1(g_id_device, 0 /* set_selection */, g_id_active_source);
}

static int g_peer_tcp_fd = -1;
static SessionCrypto g_server_session_crypto;

/* Incoming clipboard chunk assembly */
static uint8_t *g_in_clip_buf = NULL;
static size_t g_in_clip_len = 0;
static size_t g_in_clip_received = 0;
static uint16_t g_in_clip_mime = MIME_CODE_TEXT_UTF8;

static void handle_net_frame(const FrameHeader *hdr, const uint8_t *plain, size_t chunk_len) {
    if (hdr->type == PKT_CLIP_CHUNK || hdr->type == PKT_CLIP_START || hdr->type == PKT_CLIP_END) {
        uint32_t total = ntohl(hdr->total_len);
        uint16_t chunk_seq = ntohs(hdr->chunk_seq);
        uint16_t mcode = ntohs(hdr->mime_code);

        if (total == 0 || total > MAX_PAYLOAD_BYTES) return;

        if (chunk_seq == 0) {
            if (g_in_clip_buf) free(g_in_clip_buf);
            g_in_clip_buf = (uint8_t *)malloc(total);
            if (!g_in_clip_buf) return;
            g_in_clip_len = total;
            g_in_clip_received = 0;
            g_in_clip_mime = mcode;
        }

        if (g_in_clip_buf && g_in_clip_received + chunk_len <= g_in_clip_len) {
            memcpy(g_in_clip_buf + g_in_clip_received, plain, chunk_len);
            g_in_clip_received += chunk_len;

            if (g_in_clip_received >= g_in_clip_len) {
                handle_remote_clipboard(g_in_clip_buf, g_in_clip_len, g_in_clip_mime);
                free(g_in_clip_buf);
                g_in_clip_buf = NULL;
                g_in_clip_len = 0;
                g_in_clip_received = 0;
            }
        }
    }
}

static void broadcast_local_clipboard(const uint8_t *data, size_t len, uint16_t mime_code) {
    if (g_peer_tcp_fd < 0 || !g_server_session_crypto.established || len == 0 || !data || len > MAX_PAYLOAD_BYTES) return;

    size_t offset = 0;
    uint16_t chunk_seq = 0;
    uint8_t wire_buf[sizeof(FrameHeader) + CHUNK_SIZE_BYTES];

    while (offset < len) {
        size_t chunk_sz = len - offset;
        if (chunk_sz > CHUNK_SIZE_BYTES) chunk_sz = CHUNK_SIZE_BYTES;
        size_t wire_len = 0;

        seal_frame(&g_server_session_crypto, PKT_CLIP_CHUNK, mime_code, (uint32_t)len, chunk_seq,
                   data + offset, (uint16_t)chunk_sz,
                   wire_buf, &wire_len);

        if (!net_write_all(g_peer_tcp_fd, wire_buf, wire_len, 3000)) {
            close(g_peer_tcp_fd);
            g_peer_tcp_fd = -1;
            g_server_session_crypto.established = false;
            return;
        }
        offset += chunk_sz;
        chunk_seq++;
    }
}


#define StreamConfig FullmoonConfig
#define g_master_key (g_keys[0])
#define ensure_master_key ensure_keys

/* ============================================================================
 * Persistent MRU Device State & Docked / Undocked Fallback
 * ============================================================================ */
static void mru_promote(char list[MAX_PREFS][256], int *count, const char *target) {
    if (!target || !target[0] || strcmp(target, "none") == 0) return;
    int existing = -1;
    for (int i = 0; i < *count; i++) {
        if (strcmp(list[i], target) == 0) {
            existing = i;
            break;
        }
    }
    if (existing == 0) return;
    if (existing > 0) {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%s", list[existing]);
        for (int i = existing; i > 0; i--) {
            snprintf(list[i], 256, "%s", list[i - 1]);
        }
        snprintf(list[0], 256, "%s", tmp);
        return;
    }
    int limit = (*count < MAX_PREFS) ? (*count) : (MAX_PREFS - 1);
    for (int i = limit; i > 0; i--) {
        snprintf(list[i], 256, "%s", list[i - 1]);
    }
    snprintf(list[0], 256, "%s", target);
    if (*count < MAX_PREFS) (*count)++;
}

static void mru_append_unique(char list[MAX_PREFS][256], int *count, const char *target) {
    if (!target || !target[0] || strcmp(target, "none") == 0 || *count >= MAX_PREFS) return;
    for (int i = 0; i < *count; i++) {
        if (strcmp(list[i], target) == 0) return;
    }
    snprintf(list[(*count)++], 256, "%s", target);
}

static void read_first_line_file(const char *path, char *out, size_t out_sz) {
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, (int)out_sz, f)) {
        trim_and_unquote(out);
    }
    fclose(f);
}

static void load_persistent_state(void) {
    memset(&g_pref, 0, sizeof(g_pref));

    FILE *f = fopen(g_pref_file, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            char *k = line;
            char *v = eq + 1;
            trim_and_unquote(k);
            trim_and_unquote(v);
            if (strcmp(k, "cam_disabled") == 0) {
                g_pref.cam_disabled = (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0);
            } else if (strcmp(k, "mic_disabled") == 0) {
                g_pref.mic_disabled = (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0);
            } else if (strcmp(k, "cam_pref") == 0) {
                mru_append_unique(g_pref.cam_prefs, &g_pref.cam_pref_count, v);
            } else if (strcmp(k, "mic_pref") == 0) {
                mru_append_unique(g_pref.mic_prefs, &g_pref.mic_pref_count, v);
            }
        }
        fclose(f);
    }

    /* Migrate or incorporate legacy ~/.config/meet-stream-cam & meet-stream-mic */
    char leg_cam[256] = "", leg_mic[256] = "";
    read_first_line_file(g_legacy_cam_file, leg_cam, sizeof(leg_cam));
    read_first_line_file(g_legacy_mic_file, leg_mic, sizeof(leg_mic));
    if (leg_cam[0] && g_pref.cam_pref_count == 0) {
        if (strcmp(leg_cam, "none") == 0) g_pref.cam_disabled = true;
        else mru_promote(g_pref.cam_prefs, &g_pref.cam_pref_count, leg_cam);
    }
    if (leg_mic[0] && g_pref.mic_pref_count == 0) {
        if (strcmp(leg_mic, "none") == 0) g_pref.mic_disabled = true;
        else mru_promote(g_pref.mic_prefs, &g_pref.mic_pref_count, leg_mic);
    }
}

static void save_persistent_state(void) {
    mkdir_p(g_config_dir, 0700);
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", g_pref_file, (int)getpid());
    FILE *f = fopen(tmp_path, "w");
    if (f) {
        fprintf(f, "cam_disabled = %d\n", g_pref.cam_disabled ? 1 : 0);
        fprintf(f, "mic_disabled = %d\n", g_pref.mic_disabled ? 1 : 0);
        for (int i = 0; i < g_pref.cam_pref_count; i++) {
            fprintf(f, "cam_pref = %s\n", g_pref.cam_prefs[i]);
        }
        for (int i = 0; i < g_pref.mic_pref_count; i++) {
            fprintf(f, "mic_pref = %s\n", g_pref.mic_prefs[i]);
        }
        fclose(f);
        rename(tmp_path, g_pref_file);
    }

    /* Keep legacy single-line files updated for external scripts */
    FILE *fc = fopen(g_legacy_cam_file, "w");
    if (fc) {
        if (g_pref.cam_disabled) fprintf(fc, "none\n");
        else if (g_pref.cam_pref_count > 0) fprintf(fc, "%s\n", g_pref.cam_prefs[0]);
        fclose(fc);
    }
    FILE *fm = fopen(g_legacy_mic_file, "w");
    if (fm) {
        if (g_pref.mic_disabled) fprintf(fm, "none\n");
        else if (g_pref.mic_pref_count > 0) fprintf(fm, "%s\n", g_pref.mic_prefs[0]);
        else fprintf(fm, "default\n");
        fclose(fm);
    }
}

static void select_and_record_camera(const char *target) {
    if (!target || !target[0] || strcmp(target, "none") == 0) {
        g_pref.cam_disabled = true;
    } else {
        g_pref.cam_disabled = false;
        mru_promote(g_pref.cam_prefs, &g_pref.cam_pref_count, target);
    }
    save_persistent_state();
}

static void select_and_record_microphone(const char *target) {
    if (!target || !target[0] || strcmp(target, "none") == 0) {
        g_pref.mic_disabled = true;
    } else {
        g_pref.mic_disabled = false;
        mru_promote(g_pref.mic_prefs, &g_pref.mic_pref_count, target);
    }
    save_persistent_state();
}

/* ============================================================================
 * Hardware Enumeration: V4L2 Cameras & PipeWire / PulseAudio Microphones
 * ============================================================================ */
static bool is_valid_v4l2_capture_node(const char *devpath) {
    int fd = open(devpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    bool ok = false;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        if ((caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE)) &&
            !(caps & V4L2_CAP_META_CAPTURE)) {
            ok = true;
        }
    }
    close(fd);
    return ok;
}

static int cmp_device_item(const void *a, const void *b) {
    const DeviceItem *da = (const DeviceItem *)a;
    const DeviceItem *db = (const DeviceItem *)b;
    if (da->is_disabled_entry != db->is_disabled_entry) {
        return da->is_disabled_entry ? 1 : -1;
    }
    return strcmp(da->devnode, db->devnode);
}

static int discover_cameras(DeviceItem out[MAX_DEVICES]) {
    int count = 0;
    DIR *d = opendir("/sys/class/video4linux");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL && count < MAX_DEVICES - 1) {
            if (strncmp(de->d_name, "video", 5) != 0) continue;

            char idx_path[512], name_path[512], devpath[128];
            snprintf(idx_path, sizeof(idx_path), "/sys/class/video4linux/%s/index", de->d_name);
            snprintf(name_path, sizeof(name_path), "/sys/class/video4linux/%s/name", de->d_name);
            snprintf(devpath, sizeof(devpath), "/dev/%s", de->d_name);

            /* Ignore secondary metadata nodes (index != 0) */
            char idx_buf[32] = "0";
            read_first_line_file(idx_path, idx_buf, sizeof(idx_buf));
            if (strcmp(idx_buf, "0") != 0) continue;

            /* Skip loopback target device */
            if (strcmp(devpath, g_cfg.loopback_device) == 0) continue;

            char card_name[96] = "Webcam";
            read_first_line_file(name_path, card_name, sizeof(card_name));
            if (strcasestr(card_name, "Moonlight Virtual Camera") ||
                strcasestr(card_name, "Dummy video device") ||
                strcasestr(card_name, "v4l2loopback")) {
                continue;
            }

            if (access(devpath, R_OK) != 0) continue;
            if (!is_valid_v4l2_capture_node(devpath)) continue;

            /* Find persistent /dev/v4l/by-id/...-video-index0 link */
            char by_id[256] = "";
            DIR *bd = opendir("/dev/v4l/by-id");
            if (bd) {
                struct dirent *bde;
                while ((bde = readdir(bd)) != NULL) {
                    if (bde->d_name[0] == '.') continue;
                    if (!strstr(bde->d_name, "-video-index0")) continue;
                    char lpath[320];
                    snprintf(lpath, sizeof(lpath), "/dev/v4l/by-id/%s", bde->d_name);
                    char *rp = realpath(lpath, NULL);
                    if (rp) {
                        if (strcmp(rp, devpath) == 0) {
                            snprintf(by_id, sizeof(by_id), "%s", lpath);
                            free(rp);
                            break;
                        }
                        free(rp);
                    }
                }
                closedir(bd);
            }

            DeviceItem *it = &out[count++];
            memset(it, 0, sizeof(*it));
            snprintf(it->devnode, sizeof(it->devnode), "%s", devpath);
            snprintf(it->target, sizeof(it->target), "%s", by_id[0] ? by_id : devpath);
            snprintf(it->label, sizeof(it->label), "%s (%s)", card_name, devpath);
            it->is_disabled_entry = false;
        }
        closedir(d);
    }

    if (count > 1) {
        qsort(out, (size_t)count, sizeof(DeviceItem), cmp_device_item);
    }

    DeviceItem *dis = &out[count++];
    memset(dis, 0, sizeof(*dis));
    snprintf(dis->label, sizeof(dis->label), "Disabled (Audio Only)");
    snprintf(dis->target, sizeof(dis->target), "none");
    snprintf(dis->devnode, sizeof(dis->devnode), "none");
    dis->is_disabled_entry = true;
    return count;
}

static bool probe_camera_best_caps(const char *devnode, CamCaps *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
        out->width = 1280;
        out->height = 720;
        out->fps_num = 30;
        out->fps_denom = 1;
        out->is_mjpeg = false;
    }
    if (!devnode || !devnode[0] || strcmp(devnode, "none") == 0 || access(devnode, R_OK) != 0) {
        return false;
    }
    int fd = open(devnode, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;

    struct v4l2_fmtdesc fdesc;
    memset(&fdesc, 0, sizeof(fdesc));
    fdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    uint32_t best_score = 0;

    while (ioctl(fd, VIDIOC_ENUM_FMT, &fdesc) == 0) {
        bool mjpeg = (fdesc.pixelformat == V4L2_PIX_FMT_MJPEG);
        bool raw = (fdesc.pixelformat == V4L2_PIX_FMT_YUYV || 
                    fdesc.pixelformat == V4L2_PIX_FMT_UYVY || 
                    fdesc.pixelformat == V4L2_PIX_FMT_NV12);
        if (mjpeg || raw) {
            struct v4l2_frmsizeenum fsize;
            memset(&fsize, 0, sizeof(fsize));
            fsize.pixel_format = fdesc.pixelformat;

            while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fsize) == 0) {
                if (fsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                    uint32_t w = fsize.discrete.width;
                    uint32_t h = fsize.discrete.height;

                    struct v4l2_frmivalenum fival;
                    memset(&fival, 0, sizeof(fival));
                    fival.pixel_format = fdesc.pixelformat;
                    fival.width = w;
                    fival.height = h;

                    bool can_30fps = false;
                    while (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fival) == 0) {
                        if (fival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
                            if (fival.discrete.numerator > 0 && 
                                (fival.discrete.denominator / fival.discrete.numerator) >= 25) {
                                can_30fps = true;
                                break;
                            }
                        } else if (fival.type == V4L2_FRMIVAL_TYPE_STEPWISE ||
                                   fival.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
                            can_30fps = true;
                            break;
                        }
                        fival.index++;
                    }

                    if (can_30fps && w <= 1920 && h <= 1080) {
                        uint32_t score = 0;
                        if (w == 1920 && h == 1080) score = 10000;
                        else if (w == 1280 && h == 720) score = 8000;
                        else if (w == 1024 && h == 576) score = 6000;
                        else if (w == 960 && h == 540) score = 5000;
                        else if (w == 640 && h == 480) score = 4000;
                        else if (w == 640 && h == 360) score = 3500;
                        else score = (w * h) / 1000;

                        if (raw) score += 10;

                        if (score > best_score && out) {
                            best_score = score;
                            out->width = (uint16_t)w;
                            out->height = (uint16_t)h;
                            out->is_mjpeg = mjpeg;
                        }
                    }
                }
                fsize.index++;
            }
        }
        fdesc.index++;
    }
    close(fd);
    return (best_score > 0);
}

static bool camera_matches_target(const DeviceItem *cam, const char *pref_target) {
    if (!cam || !pref_target || !pref_target[0]) return false;
    if (strcmp(cam->target, pref_target) == 0 || strcmp(cam->devnode, pref_target) == 0) {
        return true;
    }
    char *rp1 = realpath(cam->target, NULL);
    char *rp2 = realpath(pref_target, NULL);
    bool match = (rp1 && rp2 && strcmp(rp1, rp2) == 0);
    if (rp1) free(rp1);
    if (rp2) free(rp2);
    return match;
}

/* Resolves best available camera from MRU preferences (with docked/undocked fallback) */
static int resolve_active_camera(const DeviceItem cams[], int cam_count, bool *out_is_fallback) {
    if (out_is_fallback) *out_is_fallback = false;
    if (g_pref.cam_disabled) {
        for (int i = 0; i < cam_count; i++) {
            if (cams[i].is_disabled_entry) return i;
        }
    }

    /* 1. Try saved MRU preferences in order */
    for (int p = 0; p < g_pref.cam_pref_count; p++) {
        for (int i = 0; i < cam_count; i++) {
            if (cams[i].is_disabled_entry) continue;
            if (camera_matches_target(&cams[i], g_pref.cam_prefs[p])) {
                if (out_is_fallback && p > 0) *out_is_fallback = true;
                return i;
            }
        }
    }

    /* 2. Fallback to first available physical camera without clobbering saved preference */
    for (int i = 0; i < cam_count; i++) {
        if (!cams[i].is_disabled_entry) {
            if (out_is_fallback && g_pref.cam_pref_count > 0) *out_is_fallback = true;
            return i;
        }
    }

    /* 3. No physical cameras connected -> Disabled (Audio Only) */
    return cam_count - 1;
}

static int discover_microphones(DeviceItem out[MAX_DEVICES]) {
    int count = 0;

    DeviceItem *def = &out[count++];
    memset(def, 0, sizeof(*def));
    snprintf(def->label, sizeof(def->label), "Default Microphone (System Default)");
    snprintf(def->target, sizeof(def->target), "default");
    snprintf(def->devnode, sizeof(def->devnode), "default");
    def->is_disabled_entry = false;

    FILE *fp = popen("pactl list sources 2>/dev/null", "r");
    if (fp) {
        char line[512];
        char cur_name[256] = "";
        char cur_desc[128] = "";
        bool is_monitor = false;

        while (fgets(line, sizeof(line), fp) && count < MAX_DEVICES - 1) {
            if (strncmp(line, "Source #", 8) == 0) {
                if (cur_name[0] && cur_desc[0] && !is_monitor &&
                    strcmp(cur_name, "source-moonlight-mic") != 0) {
                    DeviceItem *it = &out[count++];
                    memset(it, 0, sizeof(*it));
                    snprintf(it->label, sizeof(it->label), "%s", cur_desc);
                    snprintf(it->target, sizeof(it->target), "%s", cur_name);
                    snprintf(it->devnode, sizeof(it->devnode), "%s", cur_name);
                }
                cur_name[0] = '\0';
                cur_desc[0] = '\0';
                is_monitor = false;
                continue;
            }

            char *p = line;
            while (*p && isspace((unsigned char)*p)) p++;
            if (strncmp(p, "Name:", 5) == 0) {
                snprintf(cur_name, sizeof(cur_name), "%s", p + 5);
                trim_and_unquote(cur_name);
                size_t nlen = strlen(cur_name);
                if (nlen > 8 && strcmp(cur_name + nlen - 8, ".monitor") == 0) {
                    is_monitor = true;
                }
            } else if (strncmp(p, "Description:", 12) == 0) {
                snprintf(cur_desc, sizeof(cur_desc), "%s", p + 12);
                trim_and_unquote(cur_desc);
            } else if (strncmp(p, "device.class", 12) == 0 && strstr(p, "\"monitor\"")) {
                is_monitor = true;
            }
        }
        if (cur_name[0] && cur_desc[0] && !is_monitor &&
            strcmp(cur_name, "source-moonlight-mic") != 0 && count < MAX_DEVICES - 1) {
            DeviceItem *it = &out[count++];
            memset(it, 0, sizeof(*it));
            snprintf(it->label, sizeof(it->label), "%s", cur_desc);
            snprintf(it->target, sizeof(it->target), "%s", cur_name);
            snprintf(it->devnode, sizeof(it->devnode), "%s", cur_name);
        }
        pclose(fp);
    }

    DeviceItem *dis = &out[count++];
    memset(dis, 0, sizeof(*dis));
    snprintf(dis->label, sizeof(dis->label), "Disabled (Video Only)");
    snprintf(dis->target, sizeof(dis->target), "none");
    snprintf(dis->devnode, sizeof(dis->devnode), "none");
    dis->is_disabled_entry = true;
    return count;
}

static int resolve_active_microphone(const DeviceItem mics[], int mic_count, bool *out_is_fallback) {
    if (out_is_fallback) *out_is_fallback = false;
    if (g_pref.mic_disabled) {
        for (int i = 0; i < mic_count; i++) {
            if (mics[i].is_disabled_entry) return i;
        }
    }

    /* 1. Try saved MRU preferences in order */
    for (int p = 0; p < g_pref.mic_pref_count; p++) {
        for (int i = 0; i < mic_count; i++) {
            if (mics[i].is_disabled_entry) continue;
            if (strcmp(mics[i].target, g_pref.mic_prefs[p]) == 0) {
                if (out_is_fallback && p > 0) *out_is_fallback = true;
                return i;
            }
        }
    }

    /* 2. Fallback to Default Microphone (index 0) without clobbering saved preference */
    if (out_is_fallback && g_pref.mic_pref_count > 0 && strcmp(g_pref.mic_prefs[0], "default") != 0) {
        *out_is_fallback = true;
    }
    return 0;
}


/* ============================================================================
 * Network Target Resolution & Desktop Notifications
 * ============================================================================ */
static bool check_ssid_match(const char *wanted_ssid) {
    if (!wanted_ssid || !wanted_ssid[0]) return false;

    /* 1. Fast D-Bus lookup of active connections (0.005s, zero radio scanning) */
    FILE *fp = popen("nmcli -t -f TYPE,NAME connection show --active 2>/dev/null", "r");
    if (fp) {
        char line[256];
        bool matched = false;
        while (fgets(line, sizeof(line), fp)) {
            trim_and_unquote(line);
            if (strncmp(line, "802-11-wireless:", 16) == 0 &&
                strcasecmp(line + 16, wanted_ssid) == 0) {
                matched = true;
                break;
            }
        }
        pclose(fp);
        if (matched) return true;
    }

    /* 2. Fast cached Wi-Fi list without triggering channel scan (--rescan no) */
    fp = popen("nmcli -t -f active,ssid dev wifi --rescan no 2>/dev/null", "r");
    if (fp) {
        char line[256];
        bool matched = false;
        while (fgets(line, sizeof(line), fp)) {
            trim_and_unquote(line);
            if (strncmp(line, "yes:", 4) == 0 && strcasecmp(line + 4, wanted_ssid) == 0) {
                matched = true;
                break;
            }
        }
        pclose(fp);
        if (matched) return true;
    }

    return false;
}

static bool resolve_ipv4_sockaddr(const char *host, int port, struct sockaddr_in *out_sin) {
    if (!host || !host[0] || !out_sin) return false;
    memset(out_sin, 0, sizeof(*out_sin));
    out_sin->sin_family = AF_INET;
    out_sin->sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &out_sin->sin_addr) == 1) {
        return true;
    }

    int pfd[2];
    if (pipe(pfd) < 0) return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return false;
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        close(pfd[1]);
        execlp("getent", "getent", "ahostsv4", host, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    char buf[256];
    ssize_t n = read(pfd[0], buf, sizeof(buf) - 1);
    close(pfd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (n <= 0) return false;
    buf[n] = '\0';
    char ip_str[64] = "";
    if (sscanf(buf, "%63s", ip_str) == 1 && inet_pton(AF_INET, ip_str, &out_sin->sin_addr) == 1) {
        return true;
    }
    return false;
}

static bool probe_tcp_host_fast(const char *host, int port, int timeout_ms) {
    struct sockaddr_in sin;
    if (!resolve_ipv4_sockaddr(host, port, &sin)) return false;

    int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s < 0) return false;
    int r = connect(s, (struct sockaddr *)&sin, sizeof(sin));
    bool ok = false;
    if (r == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        struct pollfd pfd = {.fd = s, .events = POLLOUT};
        if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT)) {
            int err = 0;
            socklen_t elen = sizeof(err);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0) {
                ok = true;
            }
        }
    }
    close(s);
    return ok;
}

static void resolve_target_host(char *out, size_t out_sz) {
    if (g_cfg.target_host[0] && strcasecmp(g_cfg.target_host, "auto") != 0) {
        snprintf(out, out_sz, "%s", g_cfg.target_host);
        return;
    }
    if (check_ssid_match(g_cfg.lan_ssid) || probe_tcp_host_fast(g_cfg.lan_host, 22, 180)) {
        snprintf(out, out_sz, "%s", g_cfg.lan_host);
    } else {
        snprintf(out, out_sz, "%s", g_cfg.wan_host);
    }
}

static void send_notification(const char *urgency, const char *summary, const char *body) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        pid_t pid2 = fork();
        if (pid2 < 0) _exit(127);
        if (pid2 > 0) _exit(0);
        setsid();
        unsetenv("WAYLAND_SOCKET");
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        if (g_runtime_dir[0] && !getenv("DBUS_SESSION_BUS_ADDRESS")) {
            char bus[512];
            snprintf(bus, sizeof(bus), "unix:path=%s/bus", g_runtime_dir);
            setenv("DBUS_SESSION_BUS_ADDRESS", bus, 0);
        }
        execlp("notify-send", "notify-send",
               "-u", urgency ? urgency : "normal",
               summary ? summary : "Meet Stream",
               body ? body : "",
               (char *)NULL);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
}


/* ============================================================================
 * Process & Pipeline Helpers (Safe fork+execvp, zero shell interpolation)
 * ============================================================================ */
static bool is_our_daemon_pid(pid_t pid) {
    if (pid <= 1) return false;
    if (kill(pid, 0) != 0) return false;
    char cpath[128];
    snprintf(cpath, sizeof(cpath), "/proc/%d/comm", (int)pid);
    char comm[64] = "";
    read_first_line_file(cpath, comm, sizeof(comm));
    return strcmp(comm, "meet-stream") == 0;
}

static pid_t get_running_daemon_pid(void) {
    char buf[64] = "";
    read_first_line_file(g_pid_file, buf, sizeof(buf));
    if (!buf[0]) return 0;
    pid_t pid = (pid_t)atoi(buf);
    if (is_our_daemon_pid(pid)) return pid;
    return 0;
}

static void terminate_child_pid(pid_t *pid_ptr) {
    pid_t p = *pid_ptr;
    if (p <= 1) return;
    kill(p, SIGTERM);
    kill(-p, SIGTERM);
    for (int i = 0; i < 15; i++) {
        int st = 0;
        pid_t r = waitpid(p, &st, WNOHANG);
        if (r == p || (r < 0 && errno == ECHILD)) {
            *pid_ptr = 0;
            return;
        }
        usleep(10000);
    }
    kill(p, SIGKILL);
    kill(-p, SIGKILL);
    int st = 0;
    waitpid(p, &st, 0);
    *pid_ptr = 0;
}

static void kill_stray_camera_gst(void) {
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        pid_t pid = (pid_t)atoi(de->d_name);
        if (pid <= 1 || pid == getpid()) continue;
        char cpath[128];
        snprintf(cpath, sizeof(cpath), "/proc/%d/cmdline", (int)pid);
        int fd = open(cpath, O_RDONLY);
        if (fd < 0) continue;
        char cmd[512];
        ssize_t n = read(fd, cmd, sizeof(cmd) - 1);
        close(fd);
        if (n <= 0) continue;
        cmd[n] = '\0';
        for (ssize_t i = 0; i < n; i++) {
            if (cmd[i] == '\0') cmd[i] = ' ';
        }
        if (strstr(cmd, "gst-launch-1.0") &&
            (strstr(cmd, "v4l2src") || strstr(cmd, "pulsesrc") || strstr(cmd, "46002"))) {
            kill(pid, SIGTERM);
            kill(-pid, SIGTERM);
            usleep(5000);
            kill(pid, SIGKILL);
            kill(-pid, SIGKILL);
        }
    }
    closedir(d);
}

static bool gst_has_element(const char *elem_name) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        execlp("gst-inspect-1.0", "gst-inspect-1.0", elem_name, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return false;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int bind_local_ephemeral_udp(uint16_t *out_port) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        close(fd);
        return -1;
    }
    socklen_t slen = sizeof(sin);
    if (getsockname(fd, (struct sockaddr *)&sin, &slen) < 0) {
        close(fd);
        return -1;
    }
    *out_port = ntohs(sin.sin_port);
    return fd;
}

static pid_t spawn_client_audio_gst(const char *mic_target, uint16_t local_port) {
    if (!mic_target || strcmp(mic_target, "none") == 0) return 0;

    char dev_arg[300];
    snprintf(dev_arg, sizeof(dev_arg), "device=%s", mic_target);
    char br_arg[64];
    snprintf(br_arg, sizeof(br_arg), "bitrate=%d", g_cfg.audio_bitrate > 0 ? g_cfg.audio_bitrate : 64000);
    char port_arg[32];
    snprintf(port_arg, sizeof(port_arg), "port=%u", (unsigned)local_port);

    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/audio.log", g_state_dir);

    pid_t parent_pid = getpid();
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent_pid) _exit(0);
        setsid();
        int logfd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); if (devnull > 2) close(devnull); }
        if (logfd >= 0) {
            dup2(logfd, STDOUT_FILENO);
            dup2(logfd, STDERR_FILENO);
            if (logfd > 2) close(logfd);
        }

        const char *argv[32];
        int a = 0;
        argv[a++] = "gst-launch-1.0";
        argv[a++] = "-q";
        argv[a++] = "pulsesrc";
        if (strcmp(mic_target, "default") != 0) {
            argv[a++] = dev_arg;
        }
        argv[a++] = "!";
        argv[a++] = "audioconvert";
        argv[a++] = "!";
        argv[a++] = "audioresample";
        argv[a++] = "!";
        argv[a++] = "audio/x-raw,rate=48000,channels=1";
        argv[a++] = "!";
        argv[a++] = "opusenc";
        argv[a++] = br_arg;
        argv[a++] = "frame-size=20";
        argv[a++] = "audio-type=voice";
        argv[a++] = "!";
        argv[a++] = "rtpopuspay";
        argv[a++] = "pt=127";
        argv[a++] = "!";
        argv[a++] = "udpsink";
        argv[a++] = "host=127.0.0.1";
        argv[a++] = port_arg;
        argv[a++] = "sync=false";
        argv[a] = NULL;

        execvp("gst-launch-1.0", (char *const *)argv);
        _exit(127);
    }
    return pid;
}

static pid_t spawn_client_video_gst(const char *cam_target, uint16_t local_port,
                                    uint16_t width, uint16_t height, bool is_mjpeg) {
    if (!cam_target || !cam_target[0] || strcmp(cam_target, "none") == 0) return 0;
    if (access(cam_target, R_OK) != 0) return 0;
    if (width == 0 || height == 0) { width = 1280; height = 720; }

    /* Disable auto-exposure throttling so webcam maintains full 30fps in indoor lighting */
    int cfd = open(cam_target, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (cfd >= 0) {
        struct v4l2_control ctrl;
        memset(&ctrl, 0, sizeof(ctrl));
        ctrl.id = V4L2_CID_EXPOSURE_AUTO_PRIORITY;
        ctrl.value = 0;
        ioctl(cfd, VIDIOC_S_CTRL, &ctrl);
        close(cfd);
    }

    bool use_vah264 = false;
    bool use_vaapi = false;
    if (strcasecmp(g_cfg.video_encoder, "vah264enc") == 0) {
        use_vah264 = true;
    } else if (strcasecmp(g_cfg.video_encoder, "vaapih264enc") == 0) {
        use_vaapi = true;
    } else if (strcasecmp(g_cfg.video_encoder, "x264enc") != 0) {
        if (gst_has_element("vah264enc")) use_vah264 = true;
        else if (gst_has_element("vaapih264enc")) use_vaapi = true;
    }

    char dev_arg[300];
    snprintf(dev_arg, sizeof(dev_arg), "device=%s", cam_target);
    char br_arg[64];
    snprintf(br_arg, sizeof(br_arg), "bitrate=%d", g_cfg.video_bitrate > 0 ? g_cfg.video_bitrate : 4000);
    char port_arg[32];
    snprintf(port_arg, sizeof(port_arg), "port=%u", (unsigned)local_port);

    char caps_str[128];
    if (is_mjpeg) {
        snprintf(caps_str, sizeof(caps_str), "image/jpeg,width=%u,height=%u,framerate=30/1",
                 (unsigned)width, (unsigned)height);
    } else {
        snprintf(caps_str, sizeof(caps_str), "video/x-raw,width=%u,height=%u,framerate=30/1",
                 (unsigned)width, (unsigned)height);
    }

    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/video.log", g_state_dir);

    pid_t parent_pid = getpid();
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent_pid) _exit(0);
        setsid();
        int logfd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); if (devnull > 2) close(devnull); }
        if (logfd >= 0) {
            dup2(logfd, STDOUT_FILENO);
            dup2(logfd, STDERR_FILENO);
            if (logfd > 2) close(logfd);
        }

        const char *argv[48];
        int a = 0;
        argv[a++] = "gst-launch-1.0";
        argv[a++] = "-q";
        argv[a++] = "v4l2src";
        argv[a++] = "do-timestamp=true";
        argv[a++] = dev_arg;
        argv[a++] = "!";
        argv[a++] = caps_str;
        argv[a++] = "!";
        if (is_mjpeg) {
            argv[a++] = "jpegdec";
            argv[a++] = "!";
        }
        argv[a++] = "videoconvert";
        argv[a++] = "!";
        if (use_vah264) {
            argv[a++] = "vah264enc";
            argv[a++] = "rate-control=cbr";
            argv[a++] = "key-int-max=30";
            argv[a++] = br_arg;
        } else if (use_vaapi) {
            argv[a++] = "vaapih264enc";
            argv[a++] = "rate-control=cbr";
            argv[a++] = "key-int-max=30";
            argv[a++] = "tune=low-power";
        } else {
            argv[a++] = "x264enc";
            argv[a++] = "tune=zerolatency";
            argv[a++] = "speed-preset=ultrafast";
            argv[a++] = "key-int-max=30";
            argv[a++] = "bframes=0";
            argv[a++] = "byte-stream=true";
            argv[a++] = br_arg;
        }
        argv[a++] = "!";
        argv[a++] = "h264parse";
        argv[a++] = "!";
        argv[a++] = "rtph264pay";
        argv[a++] = "config-interval=1";
        argv[a++] = "pt=96";
        argv[a++] = "aggregate-mode=zero-latency";
        argv[a++] = "!";
        argv[a++] = "udpsink";
        argv[a++] = "host=127.0.0.1";
        argv[a++] = port_arg;
        argv[a++] = "sync=false";
        argv[a] = NULL;

        execvp("gst-launch-1.0", (char *const *)argv);
        _exit(127);
    }
    return pid;
}

static pid_t spawn_server_video_gst(uint16_t local_port, int v4l2_fd, uint16_t width, uint16_t height) {
    if (v4l2_fd < 0 || access(g_cfg.loopback_device, W_OK) != 0) return 0;
    (void)width;
    (void)height;

    char port_arg[32];
    snprintf(port_arg, sizeof(port_arg), "port=%u", (unsigned)local_port);
    char fd_arg[32];
    snprintf(fd_arg, sizeof(fd_arg), "fd=%d", v4l2_fd);

    pid_t parent_pid = getpid();
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent_pid) _exit(0);
        setsid();
        int logfd = open("/tmp/fullmoon_server_video.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); if (devnull > 2) close(devnull); }
        if (logfd >= 0) {
            dup2(logfd, STDOUT_FILENO);
            dup2(logfd, STDERR_FILENO);
            if (logfd > 2) close(logfd);
        }

        /* Retain inherited loopback writer fd in child process across exec */
        fcntl(v4l2_fd, F_SETFD, 0);

        const char *argv[] = {
            "gst-launch-1.0", "-v",
            "udpsrc", "address=127.0.0.1", port_arg, "buffer-size=2097152",
            "caps=application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96",
            "!", "rtph264depay",
            "!", "h264parse",
            "!", "avdec_h264",
            "!", "videoconvert",
            "!", "video/x-raw,format=YUY2",
            "!", "fdsink", fd_arg, "sync=false",
            NULL
        };
        execvp("gst-launch-1.0", (char *const *)argv);
        _exit(127);
    }
    return pid;
}

/* ============================================================================
 * Client Streaming Supervisor Daemon & Remote On-Demand Receiver (--serve)
 * ============================================================================ */
static volatile sig_atomic_t g_daemon_stop = 0;
static volatile sig_atomic_t g_daemon_reload = 0;

static void daemon_sigterm(int sig) {
    (void)sig;
    g_daemon_stop = 1;
}

static void daemon_sigusr1(int sig) {
    (void)sig;
    g_daemon_reload = 1;
}

static void write_status_file(const char *target_host, const char *cam_label,
                              const char *cam_target, const char *mic_label,
                              const char *mic_target) {
    mkdir_p(g_state_dir, 0700);
    FILE *f = fopen(g_status_file, "w");
    if (!f) return;
    fprintf(f, "target_host=%s\n", target_host ? target_host : "");
    fprintf(f, "cam_label=%s\n", cam_label ? cam_label : "");
    fprintf(f, "cam_target=%s\n", cam_target ? cam_target : "");
    fprintf(f, "mic_label=%s\n", mic_label ? mic_label : "");
    fprintf(f, "mic_target=%s\n", mic_target ? mic_target : "");
    fclose(f);
}

static int run_client_daemon(bool foreground) {
    if (!ensure_master_key()) {
        fprintf(stderr, "meet-stream: failed to load or generate secret key\n");
        return 1;
    }

    mkdir_p(g_state_dir, 0700);
    load_persistent_state();

    DeviceItem cams[MAX_DEVICES], mics[MAX_DEVICES];
    int cam_count = discover_cameras(cams);
    int mic_count = discover_microphones(mics);

    bool cam_fb = false, mic_fb = false;
    int cam_idx = resolve_active_camera(cams, cam_count, &cam_fb);
    int mic_idx = resolve_active_microphone(mics, mic_count, &mic_fb);

    char cur_cam_target[256], cur_cam_label[128];
    char cur_mic_target[256], cur_mic_label[128];
    snprintf(cur_cam_target, sizeof(cur_cam_target), "%s", cams[cam_idx].target);
    snprintf(cur_cam_label, sizeof(cur_cam_label), "%s", cams[cam_idx].label);
    snprintf(cur_mic_target, sizeof(cur_mic_target), "%s", mics[mic_idx].target);
    snprintf(cur_mic_label, sizeof(cur_mic_label), "%s", mics[mic_idx].label);

    if (strcmp(cur_cam_target, "none") == 0 && strcmp(cur_mic_target, "none") == 0) {
        send_notification("critical", "Streaming Not Started", "Both webcam and microphone are disabled.");
        return 1;
    }

    char target_host[128];
    resolve_target_host(target_host, sizeof(target_host));

    /* Resolve remote UDP destination */
    struct sockaddr_in dst_sin;
    if (!resolve_ipv4_sockaddr(target_host, g_cfg.udp_port, &dst_sin)) {
        send_notification("critical", "Meet Stream Error", "Failed to resolve target host.");
        return 1;
    }

    int net_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (net_fd < 0) {
        return 1;
    }
    if (connect(net_fd, (struct sockaddr *)&dst_sin, sizeof(dst_sin)) < 0) {
        close(net_fd);
        return 1;
    }

    uint16_t audio_port = 0, video_port = 0;
    int audio_fd = bind_local_ephemeral_udp(&audio_port);
    int video_fd = bind_local_ephemeral_udp(&video_port);
    if (audio_fd < 0 || video_fd < 0) {
        if (audio_fd >= 0) close(audio_fd);
        if (video_fd >= 0) close(video_fd);
        close(net_fd);
        return 1;
    }

    if (!foreground) {
        pid_t pid = fork();
        if (pid < 0) return 1;
        if (pid > 0) {
            /* Parent: notify user and exit */
            char body[384];
            if (strcmp(cur_cam_target, "none") != 0 && strcmp(cur_mic_target, "none") != 0) {
                snprintf(body, sizeof(body), "Mic (%s) & Cam (%s)%s -> %s",
                         cur_mic_label, cur_cam_label,
                         (cam_fb || mic_fb) ? " [auto-fallback]" : "",
                         target_host);
                send_notification("normal", "Streaming Active", body);
            } else if (strcmp(cur_mic_target, "none") != 0) {
                snprintf(body, sizeof(body), "Audio Only (%s) -> %s", cur_mic_label, target_host);
                send_notification("normal", "Audio Only Active", body);
            } else {
                snprintf(body, sizeof(body), "Video Only (%s) -> %s", cur_cam_label, target_host);
                send_notification("normal", "Video Only Active", body);
            }
            return 0;
        }
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
    }

    FILE *pf = fopen(g_pid_file, "w");
    if (pf) {
        fprintf(pf, "%d\n", (int)getpid());
        fclose(pf);
    }
    write_status_file(target_host, cur_cam_label, cur_cam_target, cur_mic_label, cur_mic_target);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = daemon_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = daemon_sigusr1;
    sigaction(SIGUSR1, &sa, NULL);

    uint64_t session_id = 0;
    fill_random_bytes((uint8_t *)&session_id, sizeof(session_id));
    if (session_id == 0) session_id = (uint64_t)time(NULL) ^ (uint64_t)getpid();
    uint64_t seq = 1;

    kill_stray_camera_gst();
    pid_t audio_pid = spawn_client_audio_gst(cur_mic_target, audio_port);
    CamCaps ccaps;
    probe_camera_best_caps(cur_cam_target, &ccaps);
    pid_t video_pid = spawn_client_video_gst(cur_cam_target, video_port, ccaps.width, ccaps.height, ccaps.is_mjpeg);

    uint8_t rtp_buf[MSTR_MAX_PAYLOAD];
    uint8_t wire_buf[MSTR_HDR_LEN + MSTR_MAX_PAYLOAD];

    /* Send initial heartbeat to wake socket-activated remote receiver immediately */
    size_t hb_len = mstr_seal_packet(g_master_key, PKT_HEARTBEAT, session_id, seq++, NULL, 0, wire_buf);
    send(net_fd, wire_buf, hb_len, MSG_DONTWAIT);

    time_t last_hb = time(NULL);

    /* Watchdog: attach to Sway/Wayland socket so daemon dies automatically if Sway exits */
    int wl_watch_fd = -1;
    const char *sway_sock = getenv("SWAYSOCK");
    const char *wd = getenv("WAYLAND_DISPLAY");
    char wl_path[512] = "";
    if (sway_sock && *sway_sock) {
        snprintf(wl_path, sizeof(wl_path), "%s", sway_sock);
    } else if (wd && *wd) {
        snprintf(wl_path, sizeof(wl_path), "%s/%s", g_runtime_dir, wd);
    } else {
        snprintf(wl_path, sizeof(wl_path), "%s/wayland-1", g_runtime_dir);
    }
    if (wl_path[0]) {
        wl_watch_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (wl_watch_fd >= 0) {
            struct sockaddr_un waddr;
            memset(&waddr, 0, sizeof(waddr));
            waddr.sun_family = AF_UNIX;
            snprintf(waddr.sun_path, sizeof(waddr.sun_path), "%s", wl_path);
            if (connect(wl_watch_fd, (struct sockaddr *)&waddr, sizeof(waddr)) < 0) {
                close(wl_watch_fd);
                wl_watch_fd = -1;
            }
        }
    }

    struct pollfd pfds[3];
    pfds[0].fd = audio_fd;
    pfds[0].events = POLLIN;
    pfds[1].fd = video_fd;
    pfds[1].events = POLLIN;
    int pfd_cnt = 2;
    if (wl_watch_fd >= 0) {
        pfds[2].fd = wl_watch_fd;
        pfds[2].events = POLLHUP | POLLERR;
        pfd_cnt = 3;
    }

    while (!g_daemon_stop) {
        int r = poll(pfds, pfd_cnt, 1000);
        if (r > 0) {
            if (wl_watch_fd >= 0 && (pfds[2].revents & (POLLHUP | POLLERR))) {
                /* Sway / Wayland session exited: shutdown streaming immediately */
                g_daemon_stop = 1;
                break;
            }
            if (pfds[0].revents & POLLIN) {
                for (int k = 0; k < 32; k++) {
                    ssize_t n = recv(audio_fd, rtp_buf, sizeof(rtp_buf), MSG_DONTWAIT);
                    if (n <= 0) break;
                    size_t wlen = mstr_seal_packet(g_master_key, PKT_AUDIO, session_id, seq++,
                                                   rtp_buf, (uint16_t)n, wire_buf);
                    send(net_fd, wire_buf, wlen, MSG_DONTWAIT);
                }
            }
            if (pfds[1].revents & POLLIN) {
                for (int k = 0; k < 64; k++) {
                    ssize_t n = recv(video_fd, rtp_buf, sizeof(rtp_buf), MSG_DONTWAIT);
                    if (n <= 0) break;
                    size_t wlen = mstr_seal_packet(g_master_key, PKT_VIDEO, session_id, seq++,
                                                   rtp_buf, (uint16_t)n, wire_buf);
                    send(net_fd, wire_buf, wlen, MSG_DONTWAIT);
                }
            }
        }

        time_t now = time(NULL);
        if (now - last_hb >= 1) {
            last_hb = now;
            size_t wlen = mstr_seal_packet(g_master_key, PKT_HEARTBEAT, session_id, seq++,
                                           NULL, 0, wire_buf);
            send(net_fd, wire_buf, wlen, MSG_DONTWAIT);
        }

        /* Reap dead children if any */
        int st = 0;
        if (audio_pid > 0 && waitpid(audio_pid, &st, WNOHANG) == audio_pid) audio_pid = 0;
        if (video_pid > 0 && waitpid(video_pid, &st, WNOHANG) == video_pid) video_pid = 0;

        /* Handle live hot-swap request from Wayland selector UI (SIGUSR1) */
        if (g_daemon_reload) {
            g_daemon_reload = 0;
            load_persistent_state();
            cam_count = discover_cameras(cams);
            mic_count = discover_microphones(mics);
            int n_cam_idx = resolve_active_camera(cams, cam_count, NULL);
            int n_mic_idx = resolve_active_microphone(mics, mic_count, NULL);

            if (strcmp(cams[n_cam_idx].target, cur_cam_target) != 0) {
                snprintf(cur_cam_target, sizeof(cur_cam_target), "%s", cams[n_cam_idx].target);
                snprintf(cur_cam_label, sizeof(cur_cam_label), "%s", cams[n_cam_idx].label);
                terminate_child_pid(&video_pid);
                size_t rlen = mstr_seal_packet(g_master_key, PKT_RESET_VIDEO, session_id, seq++,
                                               NULL, 0, wire_buf);
                send(net_fd, wire_buf, rlen, MSG_DONTWAIT);
                CamCaps n_ccaps;
                probe_camera_best_caps(cur_cam_target, &n_ccaps);
                video_pid = spawn_client_video_gst(cur_cam_target, video_port, n_ccaps.width, n_ccaps.height, n_ccaps.is_mjpeg);
            }
            if (strcmp(mics[n_mic_idx].target, cur_mic_target) != 0) {
                snprintf(cur_mic_target, sizeof(cur_mic_target), "%s", mics[n_mic_idx].target);
                snprintf(cur_mic_label, sizeof(cur_mic_label), "%s", mics[n_mic_idx].label);
                terminate_child_pid(&audio_pid);
                audio_pid = spawn_client_audio_gst(cur_mic_target, audio_port);
            }
            write_status_file(target_host, cur_cam_label, cur_cam_target, cur_mic_label, cur_mic_target);
        }
    }

    /* Send 3x authenticated STOP packets so remote receiver exits immediately */
    for (int i = 0; i < 3; i++) {
        size_t slen = mstr_seal_packet(g_master_key, PKT_STOP, session_id, seq++,
                                       NULL, 0, wire_buf);
        send(net_fd, wire_buf, slen, MSG_DONTWAIT);
        if (i < 2) usleep(5000);
    }

    terminate_child_pid(&audio_pid);
    terminate_child_pid(&video_pid);
    kill_stray_camera_gst();

    close(audio_fd);
    close(video_fd);
    close(net_fd);
    if (wl_watch_fd >= 0) close(wl_watch_fd);
    unlink(g_pid_file);
    unlink(g_status_file);
    return 0;
}

static bool stop_client_stream(bool notify) {
    pid_t pid = get_running_daemon_pid();
    if (pid > 0) {
        kill(pid, SIGTERM);
        kill(-pid, SIGTERM);
        for (int i = 0; i < 40; i++) {
            if (kill(pid, 0) != 0) break;
            usleep(10000);
        }
        if (kill(pid, 0) == 0) {
            kill(pid, SIGKILL);
            kill(-pid, SIGKILL);
        }
    }
    kill_stray_camera_gst();
    unlink(g_pid_file);
    unlink(g_status_file);
    if (notify) {
        send_notification("low", "Streaming Stopped", "Webcam and microphone forwarding disabled.");
    }
    return true;
}


/* ============================================================================
 * FreeType Font + Color Emoji + Rounded UI Renderer
 * ============================================================================ */
static FT_Library g_ft = NULL;
static FT_Face g_face_reg = NULL;
static FT_Face g_face_bold = NULL;
static FT_Face g_face_sym = NULL;
static FT_Face g_face_symbola = NULL;
static FT_Face g_face_emoji = NULL;

typedef struct {
    uint32_t *px;
    int w, h;
} SurfaceBuf;

static void init_fonts(void) {
    if (g_ft) return;
    FT_Init_FreeType(&g_ft);

    char path_reg[512], path_bold[512];
    snprintf(path_reg, sizeof(path_reg), "%s/.fonts/fira/FiraMono-Regular.ttf", g_home_dir);
    snprintf(path_bold, sizeof(path_bold), "%s/.fonts/fira/FiraMono-Bold.ttf", g_home_dir);

    if (FT_New_Face(g_ft, path_reg, 0, &g_face_reg) != 0) {
        FT_New_Face(g_ft, "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 0, &g_face_reg);
    }
    if (FT_New_Face(g_ft, path_bold, 0, &g_face_bold) != 0) {
        FT_New_Face(g_ft, "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", 0, &g_face_bold);
    }
    FT_New_Face(g_ft, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 0, &g_face_sym);

    const char *symb_paths[] = {
        "/usr/share/fonts/truetype/ancient-scripts/Symbola_hint.ttf",
        "/usr/share/fonts/truetype/ancient-scripts/Symbola.ttf",
        "/usr/share/fonts/symbola/Symbola.ttf",
        "/usr/share/fonts/TTF/Symbola.ttf",
        NULL
    };
    for (int i = 0; symb_paths[i]; i++) {
        if (FT_New_Face(g_ft, symb_paths[i], 0, &g_face_symbola) == 0) break;
    }

    const char *noto_paths[] = {
        "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/google-noto-color-emoji-fonts/NotoColorEmoji.ttf",
        "/usr/share/fonts/noto-color-emoji/NotoColorEmoji.ttf",
        "/usr/share/fonts/TTF/NotoColorEmoji.ttf",
        NULL
    };
    for (int i = 0; noto_paths[i]; i++) {
        if (FT_New_Face(g_ft, noto_paths[i], 0, &g_face_emoji) == 0) break;
    }
    if (g_face_emoji && g_face_emoji->num_fixed_sizes > 0) {
        FT_Select_Size(g_face_emoji, 0);
    }
}

static inline uint32_t premul_argb(uint32_t argb) {
    uint32_t a = (argb >> 24) & 0xFF;
    if (a == 255) return argb;
    if (a == 0) return 0;
    uint32_t r = (((argb >> 16) & 0xFF) * a + 127) / 255;
    uint32_t g = (((argb >> 8) & 0xFF) * a + 127) / 255;
    uint32_t b = ((argb & 0xFF) * a + 127) / 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static inline void blend_premul(SurfaceBuf *s, int x, int y, uint32_t src_premul) {
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) return;
    uint32_t sa = (src_premul >> 24) & 0xFF;
    if (sa == 0) return;
    if (sa == 255) {
        s->px[y * s->w + x] = src_premul;
        return;
    }
    uint32_t dst = s->px[y * s->w + x];
    uint32_t da = (dst >> 24) & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >> 8) & 0xFF;
    uint32_t db = dst & 0xFF;

    uint32_t sr = (src_premul >> 16) & 0xFF;
    uint32_t sg = (src_premul >> 8) & 0xFF;
    uint32_t sb = src_premul & 0xFF;

    uint32_t inv = 255 - sa;
    uint32_t oa = sa + (da * inv + 127) / 255;
    uint32_t or_ = sr + (dr * inv + 127) / 255;
    uint32_t og = sg + (dg * inv + 127) / 255;
    uint32_t ob = sb + (db * inv + 127) / 255;
    s->px[y * s->w + x] = (oa << 24) | (or_ << 16) | (og << 8) | ob;
}

static inline void blend_pixel(SurfaceBuf *s, int x, int y, uint32_t src_argb) {
    blend_premul(s, x, y, premul_argb(src_argb));
}

static void fill_rect(SurfaceBuf *s, int x, int y, int w, int h, uint32_t argb) {
    uint32_t pm = premul_argb(argb);
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= s->h) continue;
        for (int i = x; i < x + w; i++) {
            if (i < 0 || i >= s->w) continue;
            if ((argb >> 24) == 255) s->px[j * s->w + i] = pm;
            else blend_premul(s, i, j, pm);
        }
    }
}

static void draw_rounded_box(SurfaceBuf *s, int x, int y, int w, int h, int r,
                             uint32_t bg_argb, uint32_t bdr_argb, int bdr_w) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int dx = 0, dy = 0;
            if (i < r && j < r) { dx = r - i; dy = r - j; }
            else if (i >= w - r && j < r) { dx = i - (w - r - 1); dy = r - j; }
            else if (i < r && j >= h - r) { dx = r - i; dy = j - (h - r - 1); }
            else if (i >= w - r && j >= h - r) { dx = i - (w - r - 1); dy = j - (h - r - 1); }

            int dist2 = dx * dx + dy * dy;
            if (dist2 > r * r + 1) continue;

            bool is_border = false;
            if (bdr_w > 0 && bdr_argb) {
                if (i < bdr_w || i >= w - bdr_w || j < bdr_w || j >= h - bdr_w) is_border = true;
                else if (dx > 0 && dy > 0 && dist2 >= (r - bdr_w) * (r - bdr_w)) is_border = true;
            }
            blend_pixel(s, x + i, y + j, is_border ? bdr_argb : bg_argb);
        }
    }
}

static uint32_t utf8_next(const char **p) {
    const uint8_t *s = (const uint8_t *)*p;
    if (!*s) return 0;
    uint32_t cp = 0;
    if (s[0] < 0x80) {
        cp = s[0]; *p += 1;
    } else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); *p += 2;
    } else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); *p += 3;
    } else if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); *p += 4;
    } else {
        cp = s[0]; *p += 1;
    }
    return cp;
}

static int render_text_line(SurfaceBuf *s, const char *text, int start_x, int baseline_y,
                            int px_size, uint32_t fg, bool bold, bool draw, int max_width) {
    if (!text) return 0;
    if (g_face_reg) FT_Set_Pixel_Sizes(g_face_reg, 0, px_size);
    if (g_face_bold) FT_Set_Pixel_Sizes(g_face_bold, 0, px_size);
    if (g_face_sym) FT_Set_Pixel_Sizes(g_face_sym, 0, px_size);
    if (g_face_symbola) FT_Set_Pixel_Sizes(g_face_symbola, 0, px_size + 2);

    int pen_x = start_x;
    const char *p = text;

    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (cp == 0) break;
        if (cp == 0xFE0F || cp == 0xFE0E) continue;

        if (max_width > 0 && (pen_x - start_x) + px_size > max_width) {
            break;
        }

        /* Color emoji support for 📷 (0x1F4F7) & 🎤 (0x1F3A4) */
        if ((cp == 0x1F4F7 || cp == 0x1F3A4) && g_face_emoji) {
            FT_UInt eg = FT_Get_Char_Index(g_face_emoji, cp);
            if (eg != 0 && FT_Load_Glyph(g_face_emoji, eg, FT_LOAD_COLOR) == 0 &&
                FT_Render_Glyph(g_face_emoji->glyph, FT_RENDER_MODE_NORMAL) == 0) {
                FT_Bitmap *bm = &g_face_emoji->glyph->bitmap;
                int target_h = px_size + 2;
                int target_w = (bm->rows > 0) ? (int)((bm->width * target_h + bm->rows / 2) / bm->rows) : target_h;
                if (draw && s && bm->pixel_mode == FT_PIXEL_MODE_BGRA && bm->buffer) {
                    int dst_y0 = baseline_y - target_h + 3;
                    for (int ty = 0; ty < target_h; ty++) {
                        int sy0 = (ty * (int)bm->rows) / target_h;
                        int sy1 = ((ty + 1) * (int)bm->rows) / target_h;
                        if (sy1 <= sy0) sy1 = sy0 + 1;
                        for (int tx = 0; tx < target_w; tx++) {
                            int sx0 = (tx * (int)bm->width) / target_w;
                            int sx1 = ((tx + 1) * (int)bm->width) / target_w;
                            if (sx1 <= sx0) sx1 = sx0 + 1;
                            uint32_t sb_ = 0, sg_ = 0, sr_ = 0, sa_ = 0, cnt = 0;
                            for (int sy = sy0; sy < sy1 && sy < (int)bm->rows; sy++) {
                                const uint8_t *row = bm->buffer + sy * bm->pitch;
                                for (int sx = sx0; sx < sx1 && sx < (int)bm->width; sx++) {
                                    const uint8_t *px_p = row + sx * 4;
                                    sb_ += px_p[0]; sg_ += px_p[1]; sr_ += px_p[2]; sa_ += px_p[3];
                                    cnt++;
                                }
                            }
                            if (cnt > 0 && sa_ > 0) {
                                uint32_t pre = ((sa_ / cnt) << 24) | ((sr_ / cnt) << 16) |
                                               ((sg_ / cnt) << 8) | (sb_ / cnt);
                                blend_premul(s, pen_x + tx, dst_y0 + ty, pre);
                            }
                        }
                    }
                }
                pen_x += target_w + 4;
                continue;
            }
        }

        FT_Face face = (bold && g_face_bold) ? g_face_bold : g_face_reg;
        if (!face) break;
        FT_UInt gidx = FT_Get_Char_Index(face, cp);
        if (gidx == 0 && g_face_sym) {
            gidx = FT_Get_Char_Index(g_face_sym, cp);
            if (gidx != 0) face = g_face_sym;
        }
        if (gidx == 0 && g_face_symbola) {
            gidx = FT_Get_Char_Index(g_face_symbola, cp);
            if (gidx != 0) face = g_face_symbola;
        }

        if (FT_Load_Glyph(face, gidx, FT_LOAD_RENDER) == 0) {
            FT_GlyphSlot slot = face->glyph;
            int adv = (int)(slot->advance.x >> 6);
            if (adv <= 0) adv = px_size / 2;

            if (draw && s) {
                uint32_t fg_r = (fg >> 16) & 0xFF;
                uint32_t fg_g = (fg >> 8) & 0xFF;
                uint32_t fg_b = fg & 0xFF;
                uint32_t fg_a = (fg >> 24) & 0xFF;

                for (int r = 0; r < (int)slot->bitmap.rows; r++) {
                    const uint8_t *row = slot->bitmap.buffer + r * slot->bitmap.pitch;
                    for (int c = 0; c < (int)slot->bitmap.width; c++) {
                        uint32_t a = (row[c] * fg_a) / 255;
                        if (a > 0) {
                            uint32_t argb = (a << 24) | (fg_r << 16) | (fg_g << 8) | fg_b;
                            blend_pixel(s, pen_x + slot->bitmap_left + c,
                                        baseline_y - slot->bitmap_top + r, argb);
                        }
                    }
                }
            }
            pen_x += adv;
        }
    }
    return pen_x - start_x;
}

static int measure_text(const char *text, int px_size, bool bold) {
    return render_text_line(NULL, text, 0, 0, px_size, 0xFFFFFFFFU, bold, false, 0);
}

/* ============================================================================
 * Raw Wayland Wire Protocol & Layer-Shell Selector Overlay UI
 * ============================================================================ */
typedef enum {
    UI_MODE_ALL = 0,
    UI_MODE_CAM = 1,
    UI_MODE_MIC = 2
} UiPickerMode;

typedef struct {
    int x, y, w, h;
    bool is_cam;
    bool is_mic;
    bool is_toggle_btn;
    int dev_idx;
} UiHitRow;

typedef struct {
    UiPickerMode mode;
    DeviceItem cams[MAX_DEVICES];
    int cam_count;
    int active_cam_idx;
    bool cam_is_fallback;

    DeviceItem mics[MAX_DEVICES];
    int mic_count;
    int active_mic_idx;
    bool mic_is_fallback;

    bool is_streaming;
    char target_host[128];

    UiHitRow rows[MAX_DEVICES * 2 + 2];
    int row_count;
    int selected_row;

    int card_x, card_y, card_w, card_h;
    int pill_x, pill_y, pill_w, pill_h;
} UiState;

static UiState g_ui;
static int g_ui_wl_fd = -1;
static uint32_t g_ui_wl_next_id = 2;
static uint32_t g_ui_id_registry = 0;
static uint32_t g_id_compositor = 0;
static uint32_t g_id_shm = 0;
static uint32_t g_ui_id_seat = 0;
static uint32_t g_id_pointer = 0;
static uint32_t g_id_keyboard = 0;
static uint32_t g_id_layer_shell = 0;
static uint32_t g_id_surface = 0;
static uint32_t g_id_layer = 0;

static int g_surf_w = 1920;
static int g_surf_h = 1080;
static bool g_surf_configured = false;
static bool g_ui_running = true;
static int g_ptr_x = -1;
static int g_ptr_y = -1;

static void ui_refresh_devices(void) {
    load_persistent_state();
    g_ui.cam_count = discover_cameras(g_ui.cams);
    g_ui.mic_count = discover_microphones(g_ui.mics);
    g_ui.active_cam_idx = resolve_active_camera(g_ui.cams, g_ui.cam_count, &g_ui.cam_is_fallback);
    g_ui.active_mic_idx = resolve_active_microphone(g_ui.mics, g_ui.mic_count, &g_ui.mic_is_fallback);
    g_ui.is_streaming = (get_running_daemon_pid() > 0);
    resolve_target_host(g_ui.target_host, sizeof(g_ui.target_host));

    g_ui.row_count = 0;
    if (g_ui.mode == UI_MODE_ALL || g_ui.mode == UI_MODE_CAM) {
        for (int i = 0; i < g_ui.cam_count; i++) {
            UiHitRow *r = &g_ui.rows[g_ui.row_count++];
            memset(r, 0, sizeof(*r));
            r->is_cam = true;
            r->dev_idx = i;
        }
    }
    if (g_ui.mode == UI_MODE_ALL || g_ui.mode == UI_MODE_MIC) {
        for (int i = 0; i < g_ui.mic_count; i++) {
            UiHitRow *r = &g_ui.rows[g_ui.row_count++];
            memset(r, 0, sizeof(*r));
            r->is_mic = true;
            r->dev_idx = i;
        }
    }
    UiHitRow *btn = &g_ui.rows[g_ui.row_count++];
    memset(btn, 0, sizeof(*btn));
    btn->is_toggle_btn = true;
    btn->dev_idx = -1;

    if (g_ui.selected_row < 0) g_ui.selected_row = 0;
    if (g_ui.selected_row >= g_ui.row_count) g_ui.selected_row = g_ui.row_count - 1;
}

static void ui_render_surface(SurfaceBuf *s) {
    /* 1. Translucent dark backdrop */
    fill_rect(s, 0, 0, s->w, s->h, g_cfg.col_bg);

    int card_w = 720;
    if (card_w > s->w - 48) card_w = s->w - 48;

    int row_h = 40;
    int row_gap = 6;
    int sec_hdr_h = 34;

    int content_h = 74; /* Header + divider */
    if (g_ui.mode == UI_MODE_ALL || g_ui.mode == UI_MODE_CAM) {
        content_h += sec_hdr_h + g_ui.cam_count * (row_h + row_gap) + 14;
    }
    if (g_ui.mode == UI_MODE_ALL || g_ui.mode == UI_MODE_MIC) {
        content_h += sec_hdr_h + g_ui.mic_count * (row_h + row_gap) + 14;
    }
    content_h += 54 + 34; /* Action button + footer hints */

    int card_h = content_h;
    int card_x = (s->w - card_w) / 2;
    int card_y = (s->h - card_h) / 2;
    if (card_y < 24) card_y = 24;

    g_ui.card_x = card_x;
    g_ui.card_y = card_y;
    g_ui.card_w = card_w;
    g_ui.card_h = card_h;

    draw_rounded_box(s, card_x, card_y, card_w, card_h, 10,
                     g_cfg.col_card_bg, g_cfg.col_card_border, 2);

    int pad_x = 24;
    int cur_y = card_y + 22;

    /* Header Title */
    render_text_line(s, "Meet Stream", card_x + pad_x, cur_y + 20, 20, g_cfg.col_text, true, true, 0);

    /* Status Pill on Top Right */
    char status_txt[160];
    uint32_t pill_bg, pill_bdr, pill_fg;
    if (g_ui.is_streaming) {
        snprintf(status_txt, sizeof(status_txt), "● STREAMING -> %s", g_ui.target_host);
        pill_bg = 0x332ECC71U;
        pill_bdr = g_cfg.col_active;
        pill_fg = g_cfg.col_active;
    } else {
        snprintf(status_txt, sizeof(status_txt), "○ IDLE (%s)", g_ui.target_host);
        pill_bg = 0xFF252834U;
        pill_bdr = 0xFF44485AU;
        pill_fg = g_cfg.col_muted;
    }
    int st_w = measure_text(status_txt, 14, true);
    int pill_w = st_w + 24;
    int pill_h = 30;
    int pill_x = card_x + card_w - pad_x - pill_w;
    int pill_y = cur_y - 2;
    g_ui.pill_x = pill_x; g_ui.pill_y = pill_y; g_ui.pill_w = pill_w; g_ui.pill_h = pill_h;
    draw_rounded_box(s, pill_x, pill_y, pill_w, pill_h, 6, pill_bg, pill_bdr, 1);
    render_text_line(s, status_txt, pill_x + 12, pill_y + 20, 14, pill_fg, true, true, 0);

    cur_y += 40;
    fill_rect(s, card_x + pad_x, cur_y, card_w - 2 * pad_x, 1, 0xFF2E3240U);
    cur_y += 14;

    int r_idx = 0;

    /* Section 1: Webcams */
    if (g_ui.mode == UI_MODE_ALL || g_ui.mode == UI_MODE_CAM) {
        char sec_cam[160];
        if (g_ui.cam_is_fallback && g_pref.cam_pref_count > 0) {
            snprintf(sec_cam, sizeof(sec_cam), "📷 Webcam  [docked preference saved; using available fallback]");
        } else {
            snprintf(sec_cam, sizeof(sec_cam), "📷 Webcam");
        }
        render_text_line(s, sec_cam, card_x + pad_x, cur_y + 18, 15, g_cfg.col_accent, true, true, card_w - 2 * pad_x);
        cur_y += sec_hdr_h;

        for (int i = 0; i < g_ui.cam_count; i++) {
            UiHitRow *hr = &g_ui.rows[r_idx];
            hr->x = card_x + pad_x;
            hr->y = cur_y;
            hr->w = card_w - 2 * pad_x;
            hr->h = row_h;

            bool is_sel = (r_idx == g_ui.selected_row);
            bool is_act = (i == g_ui.active_cam_idx);

            uint32_t r_bg = is_sel ? 0xFF2B3245U : (is_act ? 0xFF202634U : 0xFF1E2028U);
            uint32_t r_bdr = is_sel ? g_cfg.col_accent : (is_act ? 0xFF3A506BU : 0xFF2A2D38U);
            draw_rounded_box(s, hr->x, hr->y, hr->w, hr->h, 6, r_bg, r_bdr, is_sel ? 2 : 1);

            const char *mark = is_act ? "✓" : " ";
            uint32_t mark_col = is_act ? g_cfg.col_active : g_cfg.col_muted;
            render_text_line(s, mark, hr->x + 14, hr->y + 26, 16, mark_col, true, true, 0);

            uint32_t lbl_col = g_ui.cams[i].is_disabled_entry ? g_cfg.col_muted : g_cfg.col_text;
            render_text_line(s, g_ui.cams[i].label, hr->x + 40, hr->y + 26, 15, lbl_col, is_act, true, hr->w - 52);

            cur_y += row_h + row_gap;
            r_idx++;
        }
        cur_y += 8;
    }

    /* Section 2: Microphones */
    if (g_ui.mode == UI_MODE_ALL || g_ui.mode == UI_MODE_MIC) {
        char sec_mic[160];
        if (g_ui.mic_is_fallback && g_pref.mic_pref_count > 0) {
            snprintf(sec_mic, sizeof(sec_mic), "🎤 Microphone  [docked preference saved; using available fallback]");
        } else {
            snprintf(sec_mic, sizeof(sec_mic), "🎤 Microphone");
        }
        render_text_line(s, sec_mic, card_x + pad_x, cur_y + 18, 15, g_cfg.col_accent, true, true, card_w - 2 * pad_x);
        cur_y += sec_hdr_h;

        for (int i = 0; i < g_ui.mic_count; i++) {
            UiHitRow *hr = &g_ui.rows[r_idx];
            hr->x = card_x + pad_x;
            hr->y = cur_y;
            hr->w = card_w - 2 * pad_x;
            hr->h = row_h;

            bool is_sel = (r_idx == g_ui.selected_row);
            bool is_act = (i == g_ui.active_mic_idx);

            uint32_t r_bg = is_sel ? 0xFF2B3245U : (is_act ? 0xFF202634U : 0xFF1E2028U);
            uint32_t r_bdr = is_sel ? g_cfg.col_accent : (is_act ? 0xFF3A506BU : 0xFF2A2D38U);
            draw_rounded_box(s, hr->x, hr->y, hr->w, hr->h, 6, r_bg, r_bdr, is_sel ? 2 : 1);

            const char *mark = is_act ? "✓" : " ";
            uint32_t mark_col = is_act ? g_cfg.col_active : g_cfg.col_muted;
            render_text_line(s, mark, hr->x + 14, hr->y + 26, 16, mark_col, true, true, 0);

            uint32_t lbl_col = g_ui.mics[i].is_disabled_entry ? g_cfg.col_muted : g_cfg.col_text;
            render_text_line(s, g_ui.mics[i].label, hr->x + 40, hr->y + 26, 15, lbl_col, is_act, true, hr->w - 52);

            cur_y += row_h + row_gap;
            r_idx++;
        }
        cur_y += 8;
    }

    /* Action Button: Toggle Stream */
    UiHitRow *btn = &g_ui.rows[r_idx];
    btn->x = card_x + pad_x;
    btn->y = cur_y;
    btn->w = card_w - 2 * pad_x;
    btn->h = 42;
    bool btn_sel = (r_idx == g_ui.selected_row);

    const char *btn_txt = g_ui.is_streaming ? "■ Stop Streaming (s)" : "▶ Start Streaming (s)";
    uint32_t btn_bg = g_ui.is_streaming ? (btn_sel ? 0xFF993333U : 0xFF552222U)
                                        : (btn_sel ? 0xFF2563EB : 0xFF1E3A5F);
    uint32_t btn_bdr = btn_sel ? 0xFFFFFFFFU : (g_ui.is_streaming ? g_cfg.col_danger : g_cfg.col_accent);
    draw_rounded_box(s, btn->x, btn->y, btn->w, btn->h, 6, btn_bg, btn_bdr, btn_sel ? 2 : 1);
    int bw = measure_text(btn_txt, 15, true);
    render_text_line(s, btn_txt, btn->x + (btn->w - bw) / 2, btn->y + 27, 15, g_cfg.col_text, true, true, 0);

    cur_y += 52;

    /* Footer keybinding hint */
    const char *hint = "j/k: Move   Tab: Section   Space: Apply   Enter: Apply & Close   s: Stream   Esc/q: Close";
    int hw = measure_text(hint, 12, false);
    int hx = card_x + (card_w - hw) / 2;
    if (hx < card_x + 12) hx = card_x + 12;
    render_text_line(s, hint, hx, cur_y + 12, 12, g_cfg.col_muted, false, true, card_w - 24);
}

static void ui_apply_row(int row_idx, bool close_after) {
    if (row_idx < 0 || row_idx >= g_ui.row_count) return;
    UiHitRow *r = &g_ui.rows[row_idx];

    if (r->is_toggle_btn) {
        if (g_ui.is_streaming) {
            stop_client_stream(true);
        } else {
            run_client_daemon(false);
        }
        ui_refresh_devices();
        if (close_after) g_ui_running = false;
        return;
    }

    if (r->is_cam && r->dev_idx >= 0 && r->dev_idx < g_ui.cam_count) {
        const DeviceItem *c = &g_ui.cams[r->dev_idx];
        select_and_record_camera(c->target);
        pid_t dpid = get_running_daemon_pid();
        if (dpid > 0) {
            kill(dpid, SIGUSR1);
            send_notification("normal", "Webcam Switched", c->label);
        } else if (close_after) {
            send_notification("normal", "Webcam Selected", c->label);
        }
    } else if (r->is_mic && r->dev_idx >= 0 && r->dev_idx < g_ui.mic_count) {
        const DeviceItem *m = &g_ui.mics[r->dev_idx];
        select_and_record_microphone(m->target);
        pid_t dpid = get_running_daemon_pid();
        if (dpid > 0) {
            kill(dpid, SIGUSR1);
            send_notification("normal", "Microphone Switched", m->label);
        } else if (close_after) {
            send_notification("normal", "Microphone Selected", m->label);
        }
    }

    ui_refresh_devices();
    if (close_after) {
        g_ui_running = false;
    }
}

/* Raw Wayland Wire Helpers */
static void ui_wl_send_msg(uint32_t obj_id, uint16_t opcode, const void *payload, size_t payload_len, int fd_to_pass) {
    if (g_ui_wl_fd < 0) return;
    uint32_t hdr[2];
    hdr[0] = obj_id;
    hdr[1] = ((uint32_t)(8 + payload_len) << 16) | opcode;

    struct iovec iov[2];
    iov[0].iov_base = hdr;
    iov[0].iov_len = 8;
    iov[1].iov_base = (void *)payload;
    iov[1].iov_len = payload_len;

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = payload_len > 0 ? 2 : 1;

    union {
        struct cmsghdr cm;
        char control[CMSG_SPACE(sizeof(int))];
    } cmsg_un;

    if (fd_to_pass >= 0) {
        msg.msg_control = cmsg_un.control;
        msg.msg_controllen = sizeof(cmsg_un.control);
        struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd_to_pass, sizeof(int));
    }

    sendmsg(g_ui_wl_fd, &msg, MSG_NOSIGNAL);
}

static void ui_wl_send_u32_1(uint32_t obj, uint16_t op, uint32_t a) {
    ui_wl_send_msg(obj, op, &a, 4, -1);
}
static void ui_wl_send_u32_2(uint32_t obj, uint16_t op, uint32_t a, uint32_t b) {
    uint32_t p[2] = {a, b};
    ui_wl_send_msg(obj, op, p, 8, -1);
}
static void ui_wl_send_u32_3(uint32_t obj, uint16_t op, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t p[3] = {a, b, c};
    ui_wl_send_msg(obj, op, p, 12, -1);
}
static void ui_wl_send_u32_4(uint32_t obj, uint16_t op, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t p[4] = {a, b, c, d};
    ui_wl_send_msg(obj, op, p, 16, -1);
}

static void ui_wl_registry_bind(uint32_t name, const char *iface, uint32_t version, uint32_t new_id) {
    uint32_t buf[64];
    buf[0] = name;
    uint32_t slen = (uint32_t)strlen(iface) + 1;
    buf[1] = slen;
    memset(buf + 2, 0, ((slen + 3) & ~3));
    memcpy(buf + 2, iface, slen);
    int idx = 2 + (int)((slen + 3) / 4);
    buf[idx++] = version;
    buf[idx++] = new_id;
    ui_wl_send_msg(g_ui_id_registry, 0, buf, (size_t)idx * 4, -1);
}

static void wl_get_layer_surface(uint32_t new_id, uint32_t surface, uint32_t output, uint32_t layer, const char *ns) {
    uint32_t buf[64];
    buf[0] = new_id;
    buf[1] = surface;
    buf[2] = output;
    buf[3] = layer;
    uint32_t slen = (uint32_t)strlen(ns) + 1;
    buf[4] = slen;
    memset(buf + 5, 0, ((slen + 3) & ~3));
    memcpy(buf + 5, ns, slen);
    int idx = 5 + (int)((slen + 3) / 4);
    ui_wl_send_msg(g_id_layer_shell, 0, buf, (size_t)idx * 4, -1);
}

static void wl_commit_rendered_frame(void) {
    if (!g_surf_configured || g_surf_w <= 0 || g_surf_h <= 0) return;
    size_t sz = (size_t)g_surf_w * (size_t)g_surf_h * 4;
    int mfd = memfd_create("meet-stream-shm", MFD_CLOEXEC);
    if (mfd < 0) return;
    if (ftruncate(mfd, (off_t)sz) < 0) {
        close(mfd);
        return;
    }
    uint32_t *px = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (px == MAP_FAILED) {
        close(mfd);
        return;
    }

    SurfaceBuf sb = {.px = px, .w = g_surf_w, .h = g_surf_h};
    ui_render_surface(&sb);
    munmap(px, sz);

    uint32_t pool_id = g_ui_wl_next_id++;
    uint32_t buf_id = g_ui_wl_next_id++;
    uint32_t cp_args[2] = {pool_id, (uint32_t)sz};
    ui_wl_send_msg(g_id_shm, 0, cp_args, 8, mfd);
    close(mfd);

    uint32_t cb_args[6] = {buf_id, 0, (uint32_t)g_surf_w, (uint32_t)g_surf_h, (uint32_t)(g_surf_w * 4), 0};
    ui_wl_send_msg(pool_id, 0, cb_args, 24, -1);

    ui_wl_send_u32_3(g_id_surface, 1 /* attach */, buf_id, 0, 0);
    ui_wl_send_u32_4(g_id_surface, 2 /* damage */, 0, 0, (uint32_t)g_surf_w, (uint32_t)g_surf_h);
    ui_wl_send_msg(g_id_surface, 6 /* commit */, NULL, 0, -1);

    ui_wl_send_msg(buf_id, 0 /* destroy buffer */, NULL, 0, -1);
    ui_wl_send_msg(pool_id, 1 /* destroy pool */, NULL, 0, -1);
}

static void ui_handle_key(uint32_t evdev_key) {
    if (evdev_key == KEY_ESC || evdev_key == KEY_Q) {
        g_ui_running = false;
        return;
    }
    if (evdev_key == KEY_J || evdev_key == KEY_DOWN) {
        if (g_ui.row_count > 0) {
            g_ui.selected_row = (g_ui.selected_row + 1) % g_ui.row_count;
            wl_commit_rendered_frame();
        }
    } else if (evdev_key == KEY_K || evdev_key == KEY_UP) {
        if (g_ui.row_count > 0) {
            g_ui.selected_row = (g_ui.selected_row - 1 + g_ui.row_count) % g_ui.row_count;
            wl_commit_rendered_frame();
        }
    } else if (evdev_key == KEY_TAB || evdev_key == KEY_L || evdev_key == KEY_RIGHT ||
               evdev_key == KEY_H || evdev_key == KEY_LEFT) {
        /* Jump between Camera section, Microphone section, and Toggle Button */
        if (g_ui.row_count > 0) {
            UiHitRow *cur = &g_ui.rows[g_ui.selected_row];
            for (int step = 1; step < g_ui.row_count; step++) {
                int cand = (g_ui.selected_row + step) % g_ui.row_count;
                if (g_ui.rows[cand].is_cam != cur->is_cam ||
                    g_ui.rows[cand].is_mic != cur->is_mic ||
                    g_ui.rows[cand].is_toggle_btn != cur->is_toggle_btn) {
                    g_ui.selected_row = cand;
                    break;
                }
            }
            wl_commit_rendered_frame();
        }
    } else if (evdev_key == KEY_SPACE) {
        ui_apply_row(g_ui.selected_row, false);
        wl_commit_rendered_frame();
    } else if (evdev_key == KEY_ENTER || evdev_key == KEY_KPENTER) {
        ui_apply_row(g_ui.selected_row, true);
        if (g_ui_running) wl_commit_rendered_frame();
    } else if (evdev_key == KEY_S) {
        if (g_ui.is_streaming) stop_client_stream(true);
        else run_client_daemon(false);
        ui_refresh_devices();
        wl_commit_rendered_frame();
    } else if (evdev_key >= KEY_1 && evdev_key <= KEY_9) {
        int idx = (int)(evdev_key - KEY_1);
        if (idx < g_ui.row_count) {
            g_ui.selected_row = idx;
            ui_apply_row(idx, false);
            wl_commit_rendered_frame();
        }
    }
}

static void ui_handle_pointer_motion(int x, int y) {
    g_ptr_x = x;
    g_ptr_y = y;
    for (int i = 0; i < g_ui.row_count; i++) {
        UiHitRow *r = &g_ui.rows[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) {
            if (g_ui.selected_row != i) {
                g_ui.selected_row = i;
                wl_commit_rendered_frame();
            }
            break;
        }
    }
}

static void ui_handle_pointer_button(uint32_t button) {
    if (button == BTN_RIGHT) {
        g_ui_running = false;
        return;
    }
    if (button != BTN_LEFT) return;

    /* Check status pill click -> toggles streaming */
    if (g_ptr_x >= g_ui.pill_x && g_ptr_x < g_ui.pill_x + g_ui.pill_w &&
        g_ptr_y >= g_ui.pill_y && g_ptr_y < g_ui.pill_y + g_ui.pill_h) {
        if (g_ui.is_streaming) stop_client_stream(true);
        else run_client_daemon(false);
        ui_refresh_devices();
        wl_commit_rendered_frame();
        return;
    }

    /* Check row clicks */
    for (int i = 0; i < g_ui.row_count; i++) {
        UiHitRow *r = &g_ui.rows[i];
        if (g_ptr_x >= r->x && g_ptr_x < r->x + r->w &&
            g_ptr_y >= r->y && g_ptr_y < r->y + r->h) {
            g_ui.selected_row = i;
            ui_apply_row(i, false);
            wl_commit_rendered_frame();
            return;
        }
    }

    /* Click outside modal card -> dismiss overlay */
    if (g_ptr_x < g_ui.card_x || g_ptr_x >= g_ui.card_x + g_ui.card_w ||
        g_ptr_y < g_ui.card_y || g_ptr_y >= g_ui.card_y + g_ui.card_h) {
        g_ui_running = false;
    }
}

static void ui_process_wayland_events(void) {
    static uint8_t rbuf[65536];
    static size_t rfilled = 0;

    while (rfilled < sizeof(rbuf)) {
        struct iovec iov = {.iov_base = rbuf + rfilled, .iov_len = sizeof(rbuf) - rfilled};
        char cmsg_buf[128];
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);

        ssize_t n = recvmsg(g_ui_wl_fd, &msg, MSG_DONTWAIT);
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                size_t nfds = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                const int *fds = (const int *)CMSG_DATA(cm);
                for (size_t i = 0; i < nfds; i++) {
                    if (fds[i] >= 0) close(fds[i]);
                }
            }
        }
        if (n == 0) { g_ui_running = false; return; }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            g_ui_running = false;
            return;
        }
        rfilled += (size_t)n;
    }

    size_t pos = 0;
    while (pos + 8 <= rfilled) {
        uint32_t obj_id = *(uint32_t *)(rbuf + pos);
        uint32_t sz_op = *(uint32_t *)(rbuf + pos + 4);
        uint16_t len = (uint16_t)(sz_op >> 16);
        uint16_t opcode = (uint16_t)(sz_op & 0xFFFF);
        if (len < 8 || pos + len > rfilled) break;

        const uint8_t *p = rbuf + pos + 8;

        if (obj_id == g_ui_id_registry && opcode == 0 /* global */) {
            uint32_t name = *(const uint32_t *)p;
            uint32_t slen = *(const uint32_t *)(p + 4);
            const char *iface = (const char *)(p + 8);
            uint32_t ver_off = 8 + ((slen + 3) & ~3);
            uint32_t ver = *(const uint32_t *)(p + ver_off);

            if (strcmp(iface, "wl_compositor") == 0) {
                g_id_compositor = g_ui_wl_next_id++;
                ui_wl_registry_bind(name, iface, ver < 4 ? ver : 4, g_id_compositor);
            } else if (strcmp(iface, "wl_shm") == 0) {
                g_id_shm = g_ui_wl_next_id++;
                ui_wl_registry_bind(name, iface, 1, g_id_shm);
            } else if (strcmp(iface, "wl_seat") == 0) {
                g_ui_id_seat = g_ui_wl_next_id++;
                ui_wl_registry_bind(name, iface, ver < 5 ? ver : 5, g_ui_id_seat);
            } else if (strcmp(iface, "zwlr_layer_shell_v1") == 0) {
                g_id_layer_shell = g_ui_wl_next_id++;
                ui_wl_registry_bind(name, iface, ver < 4 ? ver : 4, g_id_layer_shell);
            }
        } else if (obj_id == g_ui_id_seat && opcode == 0 /* capabilities */) {
            uint32_t caps = *(const uint32_t *)p;
            if ((caps & 1 /* POINTER */) && !g_id_pointer) {
                g_id_pointer = g_ui_wl_next_id++;
                ui_wl_send_u32_1(g_ui_id_seat, 0 /* get_pointer */, g_id_pointer);
            }
            if ((caps & 2 /* KEYBOARD */) && !g_id_keyboard) {
                g_id_keyboard = g_ui_wl_next_id++;
                ui_wl_send_u32_1(g_ui_id_seat, 1 /* get_keyboard */, g_id_keyboard);
            }
        } else if (obj_id == g_id_layer) {
            if (opcode == 0 /* configure */) {
                uint32_t serial = *(const uint32_t *)p;
                uint32_t w = *(const uint32_t *)(p + 4);
                uint32_t h = *(const uint32_t *)(p + 8);
                if (w > 0) g_surf_w = (int)w;
                if (h > 0) g_surf_h = (int)h;
                ui_wl_send_u32_1(g_id_layer, 6 /* ack_configure */, serial);
                g_surf_configured = true;
                wl_commit_rendered_frame();
            } else if (opcode == 1 /* closed */) {
                g_ui_running = false;
            }
        } else if (obj_id == g_id_keyboard && opcode == 3 /* key */) {
            uint32_t key = *(const uint32_t *)(p + 8);
            uint32_t state = *(const uint32_t *)(p + 12);
            if (state == 1 /* PRESSED */) {
                ui_handle_key(key);
            }
        } else if (obj_id == g_id_pointer) {
            if (opcode == 0 /* enter */) {
                int32_t sx = *(const int32_t *)(p + 8) / 256;
                int32_t sy = *(const int32_t *)(p + 12) / 256;
                ui_handle_pointer_motion(sx, sy);
            } else if (opcode == 2 /* motion */) {
                int32_t sx = *(const int32_t *)(p + 4) / 256;
                int32_t sy = *(const int32_t *)(p + 8) / 256;
                ui_handle_pointer_motion(sx, sy);
            } else if (opcode == 3 /* button */) {
                uint32_t button = *(const uint32_t *)(p + 8);
                uint32_t state = *(const uint32_t *)(p + 12);
                if (state == 1) ui_handle_pointer_button(button);
            } else if (opcode == 4 /* axis */) {
                uint32_t axis = *(const uint32_t *)(p + 4);
                int32_t val = *(const int32_t *)(p + 8);
                if (axis == 0 && g_ui.row_count > 0) {
                    if (val > 0) g_ui.selected_row = (g_ui.selected_row + 1) % g_ui.row_count;
                    else if (val < 0) g_ui.selected_row = (g_ui.selected_row - 1 + g_ui.row_count) % g_ui.row_count;
                    wl_commit_rendered_frame();
                }
            }
        }
        pos += len;
    }

    if (pos > 0) {
        memmove(rbuf, rbuf + pos, rfilled - pos);
        rfilled -= pos;
    }
}

static void ui_sigterm(int sig) {
    (void)sig;
    g_ui_running = false;
}

static int run_wayland_selector(UiPickerMode mode) {
    /* Single-instance lock so pressing $mod+Shift+s again closes the open overlay */
    char lock_path[512];
    snprintf(lock_path, sizeof(lock_path), "%s/meet-stream-ui.lock", g_runtime_dir);
    int lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd >= 0) {
        if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
            char buf[32] = "";
            ssize_t n = pread(lock_fd, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                pid_t other = (pid_t)atoi(buf);
                if (other > 1) kill(other, SIGTERM);
            }
            close(lock_fd);
            return 0;
        }
        if (ftruncate(lock_fd, 0) == 0) {
            char pbuf[32];
            int plen = snprintf(pbuf, sizeof(pbuf), "%d\n", (int)getpid());
            if (plen > 0) {
                ssize_t w = pwrite(lock_fd, pbuf, (size_t)plen, 0);
                (void)w;
            }
        }
    }

    memset(&g_ui, 0, sizeof(g_ui));
    g_ui.mode = mode;
    init_fonts();
    ui_refresh_devices();

    const char *w_sock = getenv("WAYLAND_SOCKET");
    if (w_sock && *w_sock) {
        g_ui_wl_fd = atoi(w_sock);
        fcntl(g_ui_wl_fd, F_SETFD, FD_CLOEXEC);
        unsetenv("WAYLAND_SOCKET");
    } else {
        const char *wd = getenv("WAYLAND_DISPLAY");
        char wl_path[512];
        if (wd && *wd) snprintf(wl_path, sizeof(wl_path), "%s/%s", g_runtime_dir, wd);
        else snprintf(wl_path, sizeof(wl_path), "%s/wayland-1", g_runtime_dir);

        g_ui_wl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_un waddr;
        memset(&waddr, 0, sizeof(waddr));
        waddr.sun_family = AF_UNIX;
        snprintf(waddr.sun_path, sizeof(waddr.sun_path), "%s", wl_path);
        if (connect(g_ui_wl_fd, (struct sockaddr *)&waddr, sizeof(waddr)) < 0) {
            fprintf(stderr, "meet-stream: failed to connect to Wayland display at %s\n", wl_path);
            if (lock_fd >= 0) { close(lock_fd); unlink(lock_path); }
            return 1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = ui_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    g_ui_id_registry = g_ui_wl_next_id++;
    ui_wl_send_u32_1(1 /* wl_display */, 1 /* get_registry */, g_ui_id_registry);
    uint32_t sync_cb = g_ui_wl_next_id++;
    ui_wl_send_u32_1(1 /* wl_display */, 0 /* sync */, sync_cb);

    struct pollfd pfd = {.fd = g_ui_wl_fd, .events = POLLIN};
    for (int i = 0; i < 15 && (!g_id_compositor || !g_id_shm || !g_id_layer_shell); i++) {
        if (poll(&pfd, 1, 100) > 0) ui_process_wayland_events();
    }

    if (!g_id_compositor || !g_id_shm || !g_id_layer_shell) {
        fprintf(stderr, "meet-stream: compositor missing required Wayland globals\n");
        close(g_ui_wl_fd);
        if (lock_fd >= 0) { close(lock_fd); unlink(lock_path); }
        return 1;
    }

    g_id_surface = g_ui_wl_next_id++;
    ui_wl_send_u32_1(g_id_compositor, 0 /* create_surface */, g_id_surface);

    g_id_layer = g_ui_wl_next_id++;
    wl_get_layer_surface(g_id_layer, g_id_surface, 0, 3 /* OVERLAY */, "meet-stream");
    ui_wl_send_u32_2(g_id_layer, 0 /* set_size */, 0, 0);
    ui_wl_send_u32_1(g_id_layer, 1 /* set_anchor */, 1 | 2 | 4 | 8 /* TOP|BOTTOM|LEFT|RIGHT */);
    ui_wl_send_u32_1(g_id_layer, 2 /* set_exclusive_zone */, (uint32_t)-1);
    ui_wl_send_u32_1(g_id_layer, 4 /* set_keyboard_interactivity */, 1 /* EXCLUSIVE */);
    ui_wl_send_msg(g_id_surface, 6 /* commit */, NULL, 0, -1);

    while (g_ui_running) {
        int r = poll(&pfd, 1, 1000);
        if (r > 0) {
            if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
            if (pfd.revents & POLLIN) ui_process_wayland_events();
        }
    }

    close(g_ui_wl_fd);
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        unlink(lock_path);
    }
    return 0;
}


static int run_sync_key(const char *target_ssh_host) {
    if (!ensure_master_key()) {
        fprintf(stderr, "meet-stream: failed to create or load secret key at %s\n", g_cfg.secret_key_file);
        return 1;
    }
    const char *remote = (target_ssh_host && target_ssh_host[0]) ? target_ssh_host : g_cfg.ssh_host;
    if (!remote || !remote[0]) {
        fprintf(stderr, "meet-stream: no ssh_host configured in meet-stream.conf (e.g. ssh_host = ev)\n");
        return 1;
    }

    char hex[KEY_LEN * 2 + 1];
    static const char hexchars[] = "0123456789abcdef";
    for (int i = 0; i < KEY_LEN; i++) {
        hex[i * 2]     = hexchars[(g_master_key[i] >> 4) & 0xF];
        hex[i * 2 + 1] = hexchars[g_master_key[i] & 0xF];
    }
    hex[KEY_LEN * 2] = '\0';

    char host_comment[64] = "";
    char my_host[64] = "";
    if (gethostname(my_host, sizeof(my_host)) == 0 && my_host[0]) {
        snprintf(host_comment, sizeof(host_comment), " # %s", my_host);
    }

    char remote_path[512];
    size_t hlen = strlen(g_home_dir);
    if (strncmp(g_cfg.secret_key_file, g_home_dir, hlen) == 0 && g_cfg.secret_key_file[hlen] == '/') {
        snprintf(remote_path, sizeof(remote_path), "~%s", g_cfg.secret_key_file + hlen);
    } else {
        snprintf(remote_path, sizeof(remote_path), "%s", g_cfg.secret_key_file);
    }

    printf("Authorizing key for %s on %s:%s ...\n", my_host[0] ? my_host : "client", remote, remote_path);

    char remote_cmd[1024];
    snprintf(remote_cmd, sizeof(remote_cmd),
             "KEYFILE=%s; dir=$(dirname \"$KEYFILE\"); mkdir -p \"$dir\" 2>/dev/null; "
             "touch \"$KEYFILE\" && chmod 0600 \"$KEYFILE\" && "
             "(grep -qs \"^%s\" \"$KEYFILE\" || printf '%%s%%s\\n' '%s' '%s' >> \"$KEYFILE\")",
             remote_path, hex, hex, host_comment);

    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        execlp("ssh", "ssh", remote, remote_cmd, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
        printf("Successfully authorized secret key on %s:%s\n", remote, remote_path);
        return 0;
    } else {
        fprintf(stderr, "meet-stream: ssh key sync failed with exit code %d\n", WEXITSTATUS(st));
        return 1;
    }
}


/* ============================================================================
 * Host Server Mode: Demand Detection, Standby Handle & Full Receiver
 * ============================================================================ */
static int g_v4l2_standby_fd = -1;
static uint16_t g_negotiated_width = 1920;
static uint16_t g_negotiated_height = 1080;
static uint16_t g_negotiated_fps_num = 30;
static uint16_t g_negotiated_fps_denom = 1;

static int count_v4l2_external_openers(const char *devnode, pid_t child_pid1, pid_t child_pid2);

static void v4l2_write_blank_frame(int fd, uint16_t width, uint16_t height) {
    if (fd < 0) return;
    if (width == 0 || height == 0) {
        width = g_negotiated_width > 0 ? g_negotiated_width : 1920;
        height = g_negotiated_height > 0 ? g_negotiated_height : 1080;
    }
    size_t frame_size = (size_t)width * (size_t)height * 2;
    unsigned char *black = (unsigned char *)mmap(NULL, frame_size, PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (black == MAP_FAILED) return;
    for (size_t i = 0; i < frame_size; i += 4) {
        black[i]   = 0;   /* Y0 */
        black[i+1] = 128; /* U  */
        black[i+2] = 0;   /* Y1 */
        black[i+3] = 128; /* V  */
    }
    ssize_t nw = write(fd, black, frame_size);
    (void)nw;
    munmap(black, frame_size);
}

static void v4l2_standby_close(void) {
    if (g_v4l2_standby_fd >= 0) {
        close(g_v4l2_standby_fd);
        g_v4l2_standby_fd = -1;
    }
}

static void v4l2_ensure_standard_fps(void) {
    if (access(g_cfg.loopback_device, F_OK) != 0) return;
    char sys_fmt_path[128];
    const char *dev_name = strrchr(g_cfg.loopback_device, '/');
    dev_name = dev_name ? dev_name + 1 : "video10";
    snprintf(sys_fmt_path, sizeof(sys_fmt_path), "/sys/devices/virtual/video4linux/%s/format", dev_name);

    char buf[64];
    int rfd = open(sys_fmt_path, O_RDONLY);
    if (rfd >= 0) {
        ssize_t n = read(rfd, buf, sizeof(buf) - 1);
        close(rfd);
        if (n > 0) {
            buf[n] = '\0';
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) {
                buf[--n] = '\0';
            }
            if (n >= 3 && strcmp(buf + n - 3, "@30") == 0) {
                return;
            }
        }
    }

    int sfd = open(sys_fmt_path, O_WRONLY);
    if (sfd >= 0) {
        char fps_buf[32];
        snprintf(fps_buf, sizeof(fps_buf), "@%u/%u\n",
                 (unsigned)(g_negotiated_fps_num > 0 ? g_negotiated_fps_num : 30),
                 (unsigned)(g_negotiated_fps_denom > 0 ? g_negotiated_fps_denom : 1));
        ssize_t nw = write(sfd, fps_buf, strlen(fps_buf));
        (void)nw;
        close(sfd);
    }
}

static void v4l2_apply_negotiated_format(uint16_t w, uint16_t h, uint16_t fps_num, uint16_t fps_denom) {
    if (w < 320 || w > 1920 || h < 240 || h > 1080) return;
    if (fps_num == 0 || fps_denom == 0) { fps_num = 30; fps_denom = 1; }

    v4l2_ensure_standard_fps();

    if (g_negotiated_width == w && g_negotiated_height == h &&
        g_negotiated_fps_num == fps_num && g_negotiated_fps_denom == fps_denom &&
        g_v4l2_standby_fd >= 0) {
        return;
    }

    g_negotiated_width = w;
    g_negotiated_height = h;
    g_negotiated_fps_num = fps_num;
    g_negotiated_fps_denom = fps_denom;

    int openers = count_v4l2_external_openers(g_cfg.loopback_device, 0, 0);
    if (openers > 0) {
        /* A consumer is currently reading: leave loopback alone */
        return;
    }

    v4l2_standby_close();

    /* Set device-level FPS in sysfs */
    char sys_fmt_path[128];
    const char *dev_name = strrchr(g_cfg.loopback_device, '/');
    dev_name = dev_name ? dev_name + 1 : "video10";
    snprintf(sys_fmt_path, sizeof(sys_fmt_path), "/sys/devices/virtual/video4linux/%s/format", dev_name);
    int sfd = open(sys_fmt_path, O_WRONLY);
    if (sfd >= 0) {
        char fps_buf[32];
        snprintf(fps_buf, sizeof(fps_buf), "@%u/%u\n", (unsigned)fps_num, (unsigned)fps_denom);
        ssize_t nw = write(sfd, fps_buf, strlen(fps_buf));
        (void)nw;
        close(sfd);
    }

    int fd = open(g_cfg.loopback_device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = w;
    fmt.fmt.pix.height = h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        close(fd);
        return;
    }

    /* Ensure sysfs format reflects clean @30/1 after VIDIOC_S_FMT */
    sfd = open(sys_fmt_path, O_WRONLY);
    if (sfd >= 0) {
        char fps_buf[32];
        snprintf(fps_buf, sizeof(fps_buf), "@%u/%u\n", (unsigned)fps_num, (unsigned)fps_denom);
        ssize_t nw = write(sfd, fps_buf, strlen(fps_buf));
        (void)nw;
        close(sfd);
    }

    v4l2_write_blank_frame(fd, w, h);
    g_v4l2_standby_fd = fd;

    /* Restart WirePlumber asynchronously so PipeWire immediately re-enumerates the updated resolution & 30/1 framerate */
    int sys_rc = system("systemctl --user restart wireplumber >/dev/null 2>&1 &");
    (void)sys_rc;
}

static void v4l2_standby_open(void) {
    if (g_v4l2_standby_fd >= 0) {
        v4l2_ensure_standard_fps();
        return;
    }

    v4l2_apply_negotiated_format(g_negotiated_width, g_negotiated_height,
                                 g_negotiated_fps_num, g_negotiated_fps_denom);
}

static int count_v4l2_external_openers(const char *devnode, pid_t child_pid1, pid_t child_pid2) {
    DIR *proc = opendir("/proc");
    if (!proc) return -1;
    struct dirent *de;
    int openers = 0;
    char target[PATH_MAX];
    char linkpath[64];
    pid_t self_pid = getpid();

    while ((de = readdir(proc))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        pid_t pid = (pid_t)atoi(de->d_name);
        if (pid == self_pid || (child_pid1 > 0 && pid == child_pid1) || (child_pid2 > 0 && pid == child_pid2)) continue;

        char fd_dir[64];
        snprintf(fd_dir, sizeof(fd_dir), "/proc/%s/fd", de->d_name);
        DIR *fdd = opendir(fd_dir);
        if (!fdd) continue;

        struct dirent *fde;
        bool has_opened = false;
        while ((fde = readdir(fdd))) {
            if (!isdigit((unsigned char)fde->d_name[0])) continue;
            snprintf(linkpath, sizeof(linkpath), "/proc/%s/fd/%s", de->d_name, fde->d_name);
            ssize_t len = readlink(linkpath, target, sizeof(target) - 1);
            if (len > 0) {
                target[len] = '\0';
                if (strcmp(target, devnode) == 0) {
                    has_opened = true;
                    break;
                }
            }
        }
        closedir(fdd);
        if (has_opened) openers++;
    }
    closedir(proc);
    return openers;
}

static int spawn_mic_subscriber(pid_t *out_pid) {
    int pfd[2];
    if (pipe2(pfd, O_NONBLOCK | O_CLOEXEC) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); dup2(devnull, STDERR_FILENO); if (devnull > 2) close(devnull); }
        close(pfd[1]);
        execlp("pactl", "pactl", "subscribe", (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    *out_pid = pid;
    return pfd[0];
}

static bool check_mic_recording_active(void) {
    FILE *fp = popen("pactl list short sources 2>/dev/null", "r");
    if (!fp) return false;
    char line[256];
    unsigned int mic_src_id = 0;
    while (fgets(line, sizeof(line), fp)) {
        unsigned int id = 0;
        char name[128] = "";
        if (sscanf(line, "%u %127s", &id, name) == 2) {
            if (strcmp(name, "source-moonlight-mic") == 0) {
                mic_src_id = id;
                break;
            }
        }
    }
    pclose(fp);
    if (mic_src_id == 0) return false;

    fp = popen("pactl list short source-outputs 2>/dev/null", "r");
    if (!fp) return false;
    bool active = false;
    while (fgets(line, sizeof(line), fp)) {
        unsigned int so_id = 0, src_id = 0;
        if (sscanf(line, "%u %u", &so_id, &src_id) == 2) {
            if (src_id == mic_src_id) {
                active = true;
                break;
            }
        }
    }
    pclose(fp);
    return active;
}

static void send_tcp_media_command(uint8_t cmd, uint16_t flags) {
    if (g_peer_tcp_fd < 0 || !g_server_session_crypto.established) return;
    uint8_t wire[sizeof(FrameHeader) + 16];
    memset(wire, 0, sizeof(wire));
    size_t wlen = 0;
    seal_frame(&g_server_session_crypto, cmd, flags, 0, 0, NULL, 0, wire, &wlen);
    net_write_all(g_peer_tcp_fd, wire, wlen, 2000);
}

static void send_tcp_media_caps(uint16_t width, uint16_t height, uint16_t fps_num, uint16_t fps_denom) {
    if (g_peer_tcp_fd < 0 || !g_server_session_crypto.established) return;
    MediaVideoCaps caps;
    caps.width = htons(width);
    caps.height = htons(height);
    caps.fps_num = htons(fps_num);
    caps.fps_denom = htons(fps_denom);
    caps.flags = 0;

    uint8_t wire[sizeof(FrameHeader) + sizeof(MediaVideoCaps) + 16];
    size_t wlen = 0;
    if (seal_frame(&g_server_session_crypto, PKT_MEDIA_CAPS, 0, sizeof(caps), 0,
                   (const uint8_t *)&caps, sizeof(caps), wire, &wlen)) {
        net_write_all(g_peer_tcp_fd, wire, wlen, 2000);
    }
}

static void run_server(void) {
    if (!ensure_keys()) {
        fprintf(stderr, "fullmoon --serve: failed to load or create secret key\n");
        exit(1);
    }

    int listen_tcp_fd = -1;
    const char *listen_fds = getenv("LISTEN_FDS");
    if (listen_fds && atoi(listen_fds) >= 1) {
        listen_tcp_fd = 3; /* SD_LISTEN_FDS_START */
        fcntl(listen_tcp_fd, F_SETFL, fcntl(listen_tcp_fd, F_GETFL, 0) | O_NONBLOCK);
    } else {
        listen_tcp_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        int reuse = 1;
        setsockopt(listen_tcp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
        sin.sin_port = htons((uint16_t)g_cfg.tcp_port);
        if (bind(listen_tcp_fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
            perror("bind tcp");
            exit(1);
        }
        listen(listen_tcp_fd, 4);
    }

    int udp_rx_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (udp_rx_fd >= 0) {
        int reuse = 1;
        setsockopt(udp_rx_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        struct sockaddr_in usin;
        memset(&usin, 0, sizeof(usin));
        usin.sin_family = AF_INET;
        usin.sin_addr.s_addr = htonl(INADDR_ANY);
        usin.sin_port = htons((uint16_t)g_cfg.udp_port);
        bind(udp_rx_fd, (struct sockaddr *)&usin, sizeof(usin));
    }

    int lo_audio_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_in audio_dst;
    memset(&audio_dst, 0, sizeof(audio_dst));
    audio_dst.sin_family = AF_INET;
    audio_dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    audio_dst.sin_port = htons((uint16_t)g_cfg.local_audio_port);

    uint16_t local_video_port = 0;
    int probe_fd = bind_local_ephemeral_udp(&local_video_port);
    if (probe_fd >= 0) close(probe_fd);
    if (local_video_port == 0) local_video_port = 46004;

    struct sockaddr_in video_dst;
    memset(&video_dst, 0, sizeof(video_dst));
    video_dst.sin_family = AF_INET;
    video_dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    video_dst.sin_port = htons(local_video_port);

    uint64_t wl_start = now_msec();
    while (!wayland_init() && (now_msec() - wl_start < 2500)) {
        usleep(150000);
    }

    /* Establish zero-CPU standby on /dev/video10 */
    v4l2_standby_open();

    int inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd >= 0) {
        inotify_add_watch(inotify_fd, g_cfg.loopback_device, IN_OPEN | IN_CLOSE_NOWRITE | IN_CLOSE_WRITE);
    }

    pid_t mic_sub_pid = 0;
    int mic_sub_fd = spawn_mic_subscriber(&mic_sub_pid);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev;

    ev.events = EPOLLIN;
    ev.data.fd = listen_tcp_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_tcp_fd, &ev);

    if (udp_rx_fd >= 0) {
        ev.events = EPOLLIN;
        ev.data.fd = udp_rx_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, udp_rx_fd, &ev);
    }
    if (g_wl_fd >= 0) {
        ev.events = EPOLLIN;
        ev.data.fd = g_wl_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, g_wl_fd, &ev);
    }
    if (inotify_fd >= 0) {
        ev.events = EPOLLIN;
        ev.data.fd = inotify_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, inotify_fd, &ev);
    }
    if (mic_sub_fd >= 0) {
        ev.events = EPOLLIN;
        ev.data.fd = mic_sub_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, mic_sub_fd, &ev);
    }

    bool cam_wanted = false;
    bool mic_wanted = false;
    bool cam_active = false;
    bool mic_active = false;
    uint64_t cam_idle_since = 0;
    uint64_t mic_idle_since = 0;
    uint64_t next_wl_attempt = 0;

    pid_t server_video_gst_pid = 0;
    ReplayWindow udp_rw;
    memset(&udp_rw, 0, sizeof(udp_rw));

    uint8_t net_rbuf[sizeof(FrameHeader) + CHUNK_SIZE_BYTES];
    size_t net_rfilled = 0;
    uint8_t udp_wire[MSTR_HDR_LEN + MSTR_MAX_PAYLOAD];
    uint8_t udp_plain[MSTR_MAX_PAYLOAD];

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = daemon_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    int init_openers = count_v4l2_external_openers(g_cfg.loopback_device, 0, mic_sub_pid);
    if (init_openers > 0) cam_wanted = true;
    mic_wanted = check_mic_recording_active();

    while (!g_daemon_stop) {
        int gst_st = 0;
        if (server_video_gst_pid > 0 && waitpid(server_video_gst_pid, &gst_st, WNOHANG) == server_video_gst_pid) {
            server_video_gst_pid = 0;
        }

        struct epoll_event events[16];
        int nfds = epoll_wait(epfd, events, 16, 250);

        uint64_t now = now_msec();

        if (g_wl_fd < 0 && now >= next_wl_attempt) {
            if (wayland_init()) {
                ev.events = EPOLLIN;
                ev.data.fd = g_wl_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, g_wl_fd, &ev);
            }
            next_wl_attempt = now + 1500;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_tcp_fd) {
                int client_s = accept4(listen_tcp_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (client_s >= 0) {
                    if (g_peer_tcp_fd >= 0) {
                        epoll_ctl(epfd, EPOLL_CTL_DEL, g_peer_tcp_fd, NULL);
                        close(g_peer_tcp_fd);
                    }
                    g_peer_tcp_fd = client_s;
                    int one = 1;
                    setsockopt(g_peer_tcp_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    ev.events = EPOLLIN | EPOLLRDHUP;
                    ev.data.fd = g_peer_tcp_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, g_peer_tcp_fd, &ev);
                    g_server_session_crypto.established = false;
                    net_rfilled = 0;
                }
            } else if (fd == g_wl_fd) {
                process_wayland_events();
                if (g_wl_fd < 0) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                }
            } else if (fd == inotify_fd) {
                char ibuf[512];
                while (read(inotify_fd, ibuf, sizeof(ibuf)) > 0) {}
                int openers = count_v4l2_external_openers(g_cfg.loopback_device, server_video_gst_pid, mic_sub_pid);
                if (openers >= 0) {
                    cam_wanted = (openers > 0);
                }
            } else if (fd == mic_sub_fd) {
                char pbuf[512];
                bool has_so = false;
                ssize_t pr;
                while ((pr = read(mic_sub_fd, pbuf, sizeof(pbuf) - 1)) > 0) {
                    pbuf[pr] = '\0';
                    if (strstr(pbuf, "source-output")) has_so = true;
                }
                if (has_so) {
                    mic_wanted = check_mic_recording_active();
                }
            } else if (fd == udp_rx_fd) {
                for (int k = 0; k < 64; k++) {
                    ssize_t n = recv(udp_rx_fd, udp_wire, sizeof(udp_wire), MSG_DONTWAIT);
                    if (n <= 0) break;
                    uint8_t ptype = 0;
                    uint64_t sess = 0, seq = 0;
                    uint16_t plen = 0;
                    if (mstr_open_packet(g_keys[0], udp_wire, (size_t)n, &ptype, &sess, &seq, udp_plain, &plen)) {
                        if (!replay_check_and_update(&udp_rw, sess, seq)) continue;
                        if (ptype == UDP_PKT_AUDIO && plen > 0) {
                            sendto(lo_audio_fd, udp_plain, plen, MSG_DONTWAIT, (struct sockaddr *)&audio_dst, sizeof(audio_dst));
                        } else if (ptype == UDP_PKT_VIDEO && plen > 0) {
                            if (cam_wanted && server_video_gst_pid == 0 && g_v4l2_standby_fd >= 0) {
                                server_video_gst_pid = spawn_server_video_gst(local_video_port, g_v4l2_standby_fd, g_negotiated_width, g_negotiated_height);
                            }
                            if (server_video_gst_pid > 0) {
                                sendto(lo_audio_fd, udp_plain, plen, MSG_DONTWAIT, (struct sockaddr *)&video_dst, sizeof(video_dst));
                            }
                        }
                    }
                }
            } else if (fd == g_peer_tcp_fd) {
                if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, g_peer_tcp_fd, NULL);
                    close(g_peer_tcp_fd);
                    g_peer_tcp_fd = -1;
                    g_server_session_crypto.established = false;
                    cam_active = mic_active = false;
                    terminate_child_pid(&server_video_gst_pid);
                    v4l2_write_blank_frame(g_v4l2_standby_fd, g_negotiated_width, g_negotiated_height);
                    v4l2_ensure_standard_fps();
                    continue;
                }

                ssize_t nr = read(g_peer_tcp_fd, net_rbuf + net_rfilled, sizeof(net_rbuf) - net_rfilled);
                if (nr <= 0) {
                    if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                    epoll_ctl(epfd, EPOLL_CTL_DEL, g_peer_tcp_fd, NULL);
                    close(g_peer_tcp_fd);
                    g_peer_tcp_fd = -1;
                    g_server_session_crypto.established = false;
                    cam_active = mic_active = false;
                    terminate_child_pid(&server_video_gst_pid);
                    v4l2_write_blank_frame(g_v4l2_standby_fd, g_negotiated_width, g_negotiated_height);
                    v4l2_ensure_standard_fps();
                    continue;
                }
                net_rfilled += (size_t)nr;

                /* Handle Handshake */
                if (!g_server_session_crypto.established) {
                    if (net_rfilled >= sizeof(HandshakePacket)) {
                        HandshakePacket hp;
                        memcpy(&hp, net_rbuf, sizeof(hp));
                        if (ntohl(hp.magic) == FULLMOON_MAGIC && hp.version == FULLMOON_VERSION && hp.type == PKT_HANDSHAKE_REQ) {
                            bool authed = false;
                            int matched_key = 0;
                            for (int ki = 0; ki < g_key_count; ki++) {
                                uint8_t exp_mac[32];
                                hmac_sha256(g_keys[ki], 32, "FULLMOON_SALT", 13, hp.salt, SALT_LEN, exp_mac);
                                if (crypto_memcmp(exp_mac, hp.hmac, 16) == 0) {
                                    authed = true;
                                    matched_key = ki;
                                    break;
                                }
                            }
                            if (authed) {
                                derive_session_keys(g_keys[matched_key], hp.salt, false, &g_server_session_crypto);
                                HandshakePacket resp;
                                memset(&resp, 0, sizeof(resp));
                                resp.magic = htonl(FULLMOON_MAGIC);
                                resp.version = FULLMOON_VERSION;
                                resp.type = PKT_HANDSHAKE_RESP;
                                memcpy(resp.salt, hp.salt, SALT_LEN);
                                uint8_t resp_mac[32];
                                hmac_sha256(g_server_session_crypto.key_send, 32, "FULLMOON_AUTH", 13, hp.salt, SALT_LEN, resp_mac);
                                memcpy(resp.hmac, resp_mac, 16);
                                send(g_peer_tcp_fd, &resp, sizeof(resp), MSG_NOSIGNAL);
                            }
                        }
                        size_t rem = net_rfilled - sizeof(HandshakePacket);
                        if (rem > 0) memmove(net_rbuf, net_rbuf + sizeof(HandshakePacket), rem);
                        net_rfilled = rem;
                    }
                }

                /* Handle Encrypted Frames */
                while (g_server_session_crypto.established && net_rfilled >= sizeof(FrameHeader)) {
                    FrameHeader *hdr = (FrameHeader *)net_rbuf;
                    uint16_t ch_len = ntohs(hdr->chunk_len);
                    size_t frame_total = sizeof(FrameHeader) + ch_len;
                    if (net_rfilled < frame_total) break;

                    uint8_t plain[CHUNK_SIZE_BYTES];
                    FrameHeader opened;
                    if (open_frame(&g_server_session_crypto, net_rbuf, frame_total, &opened, plain)) {
                        if (opened.type == PKT_CLIP_START || opened.type == PKT_CLIP_CHUNK || opened.type == PKT_CLIP_END) {
                            handle_net_frame(&opened, plain, ch_len);
                        } else if (opened.type == PKT_MEDIA_CAPS && ch_len >= sizeof(MediaVideoCaps)) {
                            MediaVideoCaps mc;
                            memcpy(&mc, plain, sizeof(mc));
                            uint16_t w = ntohs(mc.width);
                            uint16_t h = ntohs(mc.height);
                            uint16_t num = ntohs(mc.fps_num);
                            uint16_t denom = ntohs(mc.fps_denom);
                            if (w >= 320 && w <= 1920 && h >= 240 && h <= 1080) {
                                v4l2_apply_negotiated_format(w, h, num, denom);
                            }
                        }
                    }
                    if (net_rfilled > frame_total) {
                        memmove(net_rbuf, net_rbuf + frame_total, net_rfilled - frame_total);
                    }
                    net_rfilled -= frame_total;
                }
            }
        }

        /* Check camera demand state */
        v4l2_ensure_standard_fps();

        if (cam_wanted) {
            cam_idle_since = 0;
            if (!cam_active) {
                send_tcp_media_command(PKT_MEDIA_REQ_START, MEDIA_FLAG_CAM);
                cam_active = true;
            }
            if (server_video_gst_pid == 0 && g_v4l2_standby_fd >= 0) {
                server_video_gst_pid = spawn_server_video_gst(local_video_port, g_v4l2_standby_fd, g_negotiated_width, g_negotiated_height);
            }
        } else if (cam_active || server_video_gst_pid > 0) {
            if (cam_idle_since == 0) {
                cam_idle_since = now;
                v4l2_ensure_standard_fps();
            } else if (now - cam_idle_since >= 5000) {
                send_tcp_media_command(PKT_MEDIA_REQ_STOP, MEDIA_FLAG_CAM);
                cam_active = false;
                cam_idle_since = 0;
                terminate_child_pid(&server_video_gst_pid);
                v4l2_write_blank_frame(g_v4l2_standby_fd, g_negotiated_width, g_negotiated_height);
                v4l2_ensure_standard_fps();
            }
        }

        /* Check mic demand state */
        if (mic_wanted) {
            mic_idle_since = 0;
            if (!mic_active) {
                send_tcp_media_command(PKT_MEDIA_REQ_START, MEDIA_FLAG_MIC);
                mic_active = true;
            }
        } else if (mic_active) {
            if (mic_idle_since == 0) {
                mic_idle_since = now;
            } else if (now - mic_idle_since >= 5000) {
                send_tcp_media_command(PKT_MEDIA_REQ_STOP, MEDIA_FLAG_MIC);
                mic_active = false;
                mic_idle_since = 0;
            }
        }
    }

    terminate_child_pid(&server_video_gst_pid);
    v4l2_standby_close();
}

/* ============================================================================
 * Client Mode: Moonlight Wrapper, Clipboard Sync & Automatic Forwarding
 * ============================================================================ */
static int client_connect_to_host(void) {
    char target[128];
    resolve_target_host(target, sizeof(target));

    struct sockaddr_in sin;
    if (!resolve_ipv4_sockaddr(target, g_cfg.tcp_port, &sin)) return -1;

    int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;

    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    connect(s, (struct sockaddr *)&sin, sizeof(sin));

    struct pollfd pfd = {.fd = s, .events = POLLOUT};
    if (poll(&pfd, 1, HANDSHAKE_TIMEOUT_MSEC) <= 0) {
        close(s);
        return -1;
    }
    int err = 0;
    socklen_t elen = sizeof(err);
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
        close(s);
        return -1;
    }

    uint8_t salt[SALT_LEN];
    fill_random_bytes(salt, SALT_LEN);

    HandshakePacket hp;
    memset(&hp, 0, sizeof(hp));
    hp.magic = htonl(FULLMOON_MAGIC);
    hp.version = FULLMOON_VERSION;
    hp.type = PKT_HANDSHAKE_REQ;
    memcpy(hp.salt, salt, SALT_LEN);
    uint8_t hmac_out[32];
    hmac_sha256(g_keys[0], 32, "FULLMOON_SALT", 13, salt, SALT_LEN, hmac_out);
    memcpy(hp.hmac, hmac_out, 16);

    if (send(s, &hp, sizeof(hp), MSG_NOSIGNAL) != sizeof(hp)) {
        close(s);
        return -1;
    }

    pfd.events = POLLIN;
    if (poll(&pfd, 1, HANDSHAKE_TIMEOUT_MSEC) <= 0) {
        close(s);
        return -1;
    }

    HandshakePacket resp;
    if (recv(s, &resp, sizeof(resp), 0) != sizeof(resp)) {
        close(s);
        return -1;
    }
    if (ntohl(resp.magic) != FULLMOON_MAGIC || resp.version != FULLMOON_VERSION || resp.type != PKT_HANDSHAKE_RESP) {
        close(s);
        return -1;
    }

    SessionCrypto sc;
    derive_session_keys(g_keys[0], salt, true, &sc);

    uint8_t exp_auth[32];
    hmac_sha256(sc.key_recv, 32, "FULLMOON_AUTH", 13, salt, SALT_LEN, exp_auth);
    if (crypto_memcmp(exp_auth, resp.hmac, 16) != 0) {
        close(s);
        return -1;
    }

    g_server_session_crypto = sc;
    return s;
}

static void run_client_wrapper(char **argv_child) {
    if (!ensure_keys()) {
        fprintf(stderr, "fullmoon: failed to load secret key from %s\n", g_cfg.secret_key_file);
        exit(1);
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    int sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sig_fd < 0) { perror("signalfd"); exit(1); }

    pid_t child_pid = fork();
    if (child_pid == 0) {
        setpgid(0, 0);
        sigprocmask(SIG_UNBLOCK, &mask, NULL);
        signal(SIGPIPE, SIG_DFL);
        execvp(argv_child[0], argv_child);
        perror("execvp");
        _exit(127);
    }

    uint64_t launch_time = now_msec();
    wayland_init();

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = sig_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sig_fd, &ev);

    if (g_wl_fd >= 0) {
        ev.events = EPOLLIN;
        ev.data.fd = g_wl_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, g_wl_fd, &ev);
    }

    uint64_t next_reconnect = 0;
    int backoff_ms = 200;
    uint8_t net_rbuf[sizeof(FrameHeader) + CHUNK_SIZE_BYTES];
    size_t net_rfilled = 0;

    bool child_running = true;
    int exit_code = 0;

    /* Client on-demand media streaming state */
    bool media_streaming = false;
    pid_t audio_pid = 0, video_pid = 0;
    int media_udp_out_fd = -1;
    int local_audio_rx_fd = -1, local_video_rx_fd = -1;
    uint64_t media_sess_id = 0, media_seq = 1;

    uint8_t rtp_buf[MSTR_MAX_PAYLOAD];
    uint8_t wire_media_buf[MSTR_HDR_LEN + MSTR_MAX_PAYLOAD];

    uint64_t client_next_wl_attempt = 0;

    while (child_running) {
        uint64_t now = now_msec();

        if (g_wl_fd < 0 && now >= client_next_wl_attempt) {
            if (wayland_init()) {
                ev.events = EPOLLIN;
                ev.data.fd = g_wl_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, g_wl_fd, &ev);
            }
            client_next_wl_attempt = now + 1500;
        }

        if (g_peer_tcp_fd < 0 && now >= next_reconnect) {
            g_peer_tcp_fd = client_connect_to_host();
            if (g_peer_tcp_fd >= 0) {
                ev.events = EPOLLIN | EPOLLRDHUP;
                ev.data.fd = g_peer_tcp_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, g_peer_tcp_fd, &ev);
                backoff_ms = 200;

                load_persistent_state();
                DeviceItem cams[MAX_DEVICES];
                int cc = discover_cameras(cams);
                bool cfb = false;
                int ci = resolve_active_camera(cams, cc, &cfb);
                if (ci >= 0 && !cams[ci].is_disabled_entry && strcmp(cams[ci].target, "none") != 0) {
                    CamCaps ccaps;
                    if (probe_camera_best_caps(cams[ci].target, &ccaps)) {
                        send_tcp_media_caps(ccaps.width, ccaps.height, ccaps.fps_num, ccaps.fps_denom);
                    }
                }
            } else {
                next_reconnect = now + (uint64_t)backoff_ms;
                backoff_ms = (backoff_ms < 5000) ? backoff_ms * 2 : 5000;
            }
        }

        struct epoll_event events[16];
        int nfds = epoll_wait(epfd, events, 16, 250);

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == sig_fd) {
                struct signalfd_siginfo ssi;
                while (read(sig_fd, &ssi, sizeof(ssi)) > 0) {
                    if (ssi.ssi_signo == SIGCHLD) {
                        int status = 0;
                        pid_t p = waitpid(child_pid, &status, WNOHANG);
                        if (p == child_pid) {
                            if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
                            else if (WIFSIGNALED(status)) exit_code = 128 + WTERMSIG(status);

                            if (exit_code == 0 && (now_msec() - launch_time < 1000)) {
                                DIR *dir = opendir("/proc");
                                bool found = false;
                                if (dir) {
                                    struct dirent *de;
                                    while ((de = readdir(dir))) {
                                        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
                                        char cpath[256];
                                        snprintf(cpath, sizeof(cpath), "/proc/%s/cmdline", de->d_name);
                                        FILE *cf = fopen(cpath, "r");
                                        if (cf) {
                                            char cmd[256] = "";
                                            size_t cr = fread(cmd, 1, sizeof(cmd) - 1, cf);
                                            fclose(cf);
                                            if (cr > 0 && strstr(cmd, "moonlight")) {
                                                child_pid = (pid_t)atoi(de->d_name);
                                                found = true;
                                                break;
                                            }
                                        }
                                    }
                                    closedir(dir);
                                }
                                if (found) continue;
                            }
                            child_running = false;
                        }
                    } else if (ssi.ssi_signo == SIGINT || ssi.ssi_signo == SIGTERM) {
                        kill(-child_pid, (int)ssi.ssi_signo);
                    }
                }
            } else if (fd == g_wl_fd) {
                process_wayland_events();
                if (g_wl_fd < 0) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                }
            } else if (fd == local_audio_rx_fd && media_streaming) {
                for (int k = 0; k < 32; k++) {
                    ssize_t n = recv(local_audio_rx_fd, rtp_buf, sizeof(rtp_buf), MSG_DONTWAIT);
                    if (n <= 0) break;
                    size_t wlen = mstr_seal_packet(g_keys[0], UDP_PKT_AUDIO, media_sess_id, media_seq++, rtp_buf, (uint16_t)n, wire_media_buf);
                    send(media_udp_out_fd, wire_media_buf, wlen, MSG_DONTWAIT);
                }
            } else if (fd == local_video_rx_fd && media_streaming) {
                for (int k = 0; k < 64; k++) {
                    ssize_t n = recv(local_video_rx_fd, rtp_buf, sizeof(rtp_buf), MSG_DONTWAIT);
                    if (n <= 0) break;
                    size_t wlen = mstr_seal_packet(g_keys[0], UDP_PKT_VIDEO, media_sess_id, media_seq++, rtp_buf, (uint16_t)n, wire_media_buf);
                    send(media_udp_out_fd, wire_media_buf, wlen, MSG_DONTWAIT);
                }
            } else if (fd == g_peer_tcp_fd) {
                if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, g_peer_tcp_fd, NULL);
                    close(g_peer_tcp_fd);
                    g_peer_tcp_fd = -1;
                    g_server_session_crypto.established = false;
                    next_reconnect = now_msec() + (uint64_t)backoff_ms;
                    continue;
                }

                ssize_t nr = read(g_peer_tcp_fd, net_rbuf + net_rfilled, sizeof(net_rbuf) - net_rfilled);
                if (nr <= 0) {
                    if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                    epoll_ctl(epfd, EPOLL_CTL_DEL, g_peer_tcp_fd, NULL);
                    close(g_peer_tcp_fd);
                    g_peer_tcp_fd = -1;
                    g_server_session_crypto.established = false;
                    next_reconnect = now_msec() + (uint64_t)backoff_ms;
                    continue;
                }
                net_rfilled += (size_t)nr;

                while (net_rfilled >= sizeof(FrameHeader)) {
                    FrameHeader *hdr = (FrameHeader *)net_rbuf;
                    uint16_t ch_len = ntohs(hdr->chunk_len);
                    size_t frame_total = sizeof(FrameHeader) + ch_len;
                    if (net_rfilled < frame_total) break;

                    uint8_t plain[CHUNK_SIZE_BYTES];
                    FrameHeader opened_h;
                    if (open_frame(&g_server_session_crypto, net_rbuf, frame_total, &opened_h, plain)) {
                        if (opened_h.type == PKT_CLIP_START || opened_h.type == PKT_CLIP_CHUNK || opened_h.type == PKT_CLIP_END) {
                            handle_net_frame(&opened_h, plain, ch_len);
                        } else if (opened_h.type == PKT_MEDIA_REQ_START) {
                            uint16_t flags = ntohs(opened_h.mime_code);
                            if (flags == 0) flags = MEDIA_FLAG_CAM | MEDIA_FLAG_MIC;

                            load_persistent_state();
                            DeviceItem cams[MAX_DEVICES], mics[MAX_DEVICES];
                            int cc = discover_cameras(cams);
                            int mc = discover_microphones(mics);
                            bool cfb = false, mfb = false;
                            int ci = resolve_active_camera(cams, cc, &cfb);
                            int mi = resolve_active_microphone(mics, mc, &mfb);

                            if (media_udp_out_fd < 0) {
                                char thost[128];
                                resolve_target_host(thost, sizeof(thost));
                                struct sockaddr_in d_sin;
                                resolve_ipv4_sockaddr(thost, g_cfg.udp_port, &d_sin);

                                media_udp_out_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
                                connect(media_udp_out_fd, (struct sockaddr *)&d_sin, sizeof(d_sin));
                                fill_random_bytes((uint8_t *)&media_sess_id, sizeof(media_sess_id));
                                media_seq = 1;
                            }

                            if ((flags & MEDIA_FLAG_MIC) && audio_pid == 0 && !g_pref.mic_disabled && mc > 0) {
                                uint16_t a_port = 0;
                                if (local_audio_rx_fd < 0) {
                                    local_audio_rx_fd = bind_local_ephemeral_udp(&a_port);
                                    if (local_audio_rx_fd >= 0) {
                                        ev.events = EPOLLIN;
                                        ev.data.fd = local_audio_rx_fd;
                                        epoll_ctl(epfd, EPOLL_CTL_ADD, local_audio_rx_fd, &ev);
                                    }
                                }
                                audio_pid = spawn_client_audio_gst(mics[mi].target, a_port);
                            }

                            if ((flags & MEDIA_FLAG_CAM) && video_pid == 0 && !g_pref.cam_disabled && cc > 0) {
                                uint16_t v_port = 0;
                                if (local_video_rx_fd < 0) {
                                    local_video_rx_fd = bind_local_ephemeral_udp(&v_port);
                                    if (local_video_rx_fd >= 0) {
                                        ev.events = EPOLLIN;
                                        ev.data.fd = local_video_rx_fd;
                                        epoll_ctl(epfd, EPOLL_CTL_ADD, local_video_rx_fd, &ev);
                                    }
                                }
                                CamCaps ccaps;
                                probe_camera_best_caps(cams[ci].target, &ccaps);
                                video_pid = spawn_client_video_gst(cams[ci].target, v_port, ccaps.width, ccaps.height, ccaps.is_mjpeg);
                            }

                            media_streaming = (audio_pid > 0 || video_pid > 0);
                        } else if (opened_h.type == PKT_MEDIA_REQ_STOP) {
                            uint16_t flags = ntohs(opened_h.mime_code);
                            if (flags == 0) flags = MEDIA_FLAG_CAM | MEDIA_FLAG_MIC;

                            if (flags & MEDIA_FLAG_MIC) {
                                terminate_child_pid(&audio_pid);
                                if (local_audio_rx_fd >= 0) {
                                    epoll_ctl(epfd, EPOLL_CTL_DEL, local_audio_rx_fd, NULL);
                                    close(local_audio_rx_fd);
                                    local_audio_rx_fd = -1;
                                }
                            }
                            if (flags & MEDIA_FLAG_CAM) {
                                terminate_child_pid(&video_pid);
                                if (local_video_rx_fd >= 0) {
                                    epoll_ctl(epfd, EPOLL_CTL_DEL, local_video_rx_fd, NULL);
                                    close(local_video_rx_fd);
                                    local_video_rx_fd = -1;
                                }
                            }
                            media_streaming = (audio_pid > 0 || video_pid > 0);
                            if (!media_streaming && media_udp_out_fd >= 0) {
                                close(media_udp_out_fd);
                                media_udp_out_fd = -1;
                            }
                        }
                    }
                    if (net_rfilled > frame_total) {
                        memmove(net_rbuf, net_rbuf + frame_total, net_rfilled - frame_total);
                    }
                    net_rfilled -= frame_total;
                }
            }
        }
    }

    if (media_streaming) {
        terminate_child_pid(&audio_pid);
        terminate_child_pid(&video_pid);
        if (local_audio_rx_fd >= 0) close(local_audio_rx_fd);
        if (local_video_rx_fd >= 0) close(local_video_rx_fd);
        if (media_udp_out_fd >= 0) close(media_udp_out_fd);
    }
    if (g_peer_tcp_fd >= 0) close(g_peer_tcp_fd);
    if (g_wl_fd >= 0) close(g_wl_fd);
    close(epfd);
    close(sig_fd);
    exit(exit_code);
}


/* ============================================================================
 * Self-Test Suite & Status
 * ============================================================================ */
static int run_self_test(void) {
    printf("Running fullmoon self-test suite...\n");
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    uint8_t salt[16];
    for (int i = 0; i < 16; i++) salt[i] = (uint8_t)(0xA0 + i);

    SessionCrypto sc_c, sc_s;
    derive_session_keys(key, salt, true, &sc_c);
    derive_session_keys(key, salt, false, &sc_s);

    const char *test_msg = "fullmoon clipboard & media control test vector";
    size_t tlen = strlen(test_msg);

    uint8_t wire[sizeof(FrameHeader) + 128];
    size_t wlen = 0;
    if (!seal_frame(&sc_c, PKT_CLIP_START, MIME_CODE_TEXT_UTF8, (uint32_t)tlen, 0, (const uint8_t *)test_msg, (uint16_t)tlen, wire, &wlen)) {
        fprintf(stderr, "FAIL: seal_frame failed\n");
        return 1;
    }

    FrameHeader opened;
    uint8_t plain[128];
    if (!open_frame(&sc_s, wire, wlen, &opened, plain)) {
        fprintf(stderr, "FAIL: open_frame failed\n");
        return 1;
    }
    plain[ntohs(opened.chunk_len)] = '\0';
    if (strcmp((char *)plain, test_msg) != 0) {
        fprintf(stderr, "FAIL: decrypted content mismatch\n");
        return 1;
    }

    /* Test UDP media packet framing */
    uint8_t udp_wire[MSTR_HDR_LEN + 128];
    size_t uwlen = mstr_seal_packet(key, UDP_PKT_AUDIO, 0x12345678, 1, (const uint8_t *)test_msg, (uint16_t)tlen, udp_wire);
    uint8_t out_type = 0;
    uint64_t out_sess = 0, out_seq = 0;
    uint16_t out_plen = 0;
    uint8_t out_plain[128];
    if (!mstr_open_packet(key, udp_wire, uwlen, &out_type, &out_sess, &out_seq, out_plain, &out_plen)) {
        fprintf(stderr, "FAIL: mstr_open_packet failed\n");
        return 1;
    }
    if (out_type != UDP_PKT_AUDIO || out_sess != 0x12345678 || out_seq != 1 || out_plen != tlen) {
        fprintf(stderr, "FAIL: mstr packet metadata mismatch\n");
        return 1;
    }

    printf("ALL TESTS PASSED: Crypto, directional keys, and TCP/UDP packet framing verified.\n");
    return 0;
}

/* ============================================================================
 * Main Entry Point
 * ============================================================================ */
static void init_paths(void) {
    const char *home = getenv("HOME");
    snprintf(g_home_dir, sizeof(g_home_dir), "%s", (home && *home) ? home : "/tmp");

    const char *rdir = getenv("XDG_RUNTIME_DIR");
    if (rdir && *rdir) snprintf(g_runtime_dir, sizeof(g_runtime_dir), "%s", rdir);
    else snprintf(g_runtime_dir, sizeof(g_runtime_dir), "/run/user/%u", (unsigned)getuid());

    const char *cdir = getenv("XDG_CONFIG_HOME");
    if (cdir && *cdir) snprintf(g_config_dir, sizeof(g_config_dir), "%s", cdir);
    else snprintf(g_config_dir, sizeof(g_config_dir), "%s/.config", g_home_dir);

    snprintf(g_state_dir, sizeof(g_state_dir), "%s/fullmoon", g_runtime_dir);
    snprintf(g_pid_file, sizeof(g_pid_file), "%s/daemon.pid", g_state_dir);
    snprintf(g_status_file, sizeof(g_status_file), "%s/status", g_state_dir);
    snprintf(g_pref_file, sizeof(g_pref_file), "%s/meet-stream-state", g_config_dir);
    snprintf(g_legacy_cam_file, sizeof(g_legacy_cam_file), "%s/meet-stream-cam", g_config_dir);
    snprintf(g_legacy_mic_file, sizeof(g_legacy_mic_file), "%s/meet-stream-mic", g_config_dir);
}

int main(int argc, char **argv) {
    init_paths();
    load_config();

    struct rlimit rl = {0, 0};
    setrlimit(RLIMIT_CORE, &rl);
    signal(SIGPIPE, SIG_IGN);

    if (argc >= 2) {
        if (strcmp(argv[1], "--self-test") == 0) {
            return run_self_test();
        } else if (strcmp(argv[1], "--serve") == 0) {
            run_server();
            return 0;
        } else if (strcmp(argv[1], "--select") == 0 || strcmp(argv[1], "select") == 0) {
            return run_wayland_selector(UI_MODE_ALL);
        } else if (strcmp(argv[1], "--select-cam") == 0 || strcmp(argv[1], "select-cam") == 0) {
            return run_wayland_selector(UI_MODE_CAM);
        } else if (strcmp(argv[1], "--select-mic") == 0 || strcmp(argv[1], "select-mic") == 0) {
            return run_wayland_selector(UI_MODE_MIC);
        } else if (strcmp(argv[1], "--start") == 0 || strcmp(argv[1], "start") == 0) {
            if (get_running_daemon_pid() == 0) run_client_daemon(false);
            return 0;
        } else if (strcmp(argv[1], "--stop") == 0 || strcmp(argv[1], "stop") == 0) {
            stop_client_stream(true);
            return 0;
        } else if (strcmp(argv[1], "--sync-key") == 0) {
            const char *th = (argc >= 3 && argv[2][0] != '-') ? argv[2] : NULL;
            return run_sync_key(th);
        } else if (strcmp(argv[1], "--status") == 0) {
            load_persistent_state();
            DeviceItem cams[MAX_DEVICES], mics[MAX_DEVICES];
            int cc = discover_cameras(cams);
            int mc = discover_microphones(mics);
            bool cfb = false, mfb = false;
            int ci = resolve_active_camera(cams, cc, &cfb);
            int mi = resolve_active_microphone(mics, mc, &mfb);
            char thost[128];
            resolve_target_host(thost, sizeof(thost));
            pid_t dpid = get_running_daemon_pid();
            printf("Daemon:      %s (PID %d)\n", dpid > 0 ? "STREAMING" : "IDLE", (int)dpid);
            printf("Target Host: %s (TCP: %d, UDP: %d)\n", thost, g_cfg.tcp_port, g_cfg.udp_port);
            printf("Webcam:      %s [%s]%s\n", cams[ci].label, cams[ci].target, cfb ? " (auto-fallback)" : "");
            printf("Microphone:  %s [%s]%s\n", mics[mi].label, mics[mi].target, mfb ? " (auto-fallback)" : "");
            return 0;
        } else if (strcmp(argv[1], "--wrap") == 0) {
            char **child_args = &argv[2];
            char *fallback_flatpak[] = {"flatpak", "run", "com.moonlight_stream.Moonlight", NULL};
            char *fallback_native[] = {"moonlight", NULL};
            if (argc < 3) {
                if (system("which moonlight >/dev/null 2>&1") == 0) child_args = fallback_native;
                else child_args = fallback_flatpak;
            } else {
                for (int i = 2; i < argc; i++) {
                    if (strcmp(argv[i], "%u") == 0 || strcmp(argv[i], "%U") == 0) {
                        argv[i] = NULL;
                        break;
                    }
                }
            }
            run_client_wrapper(child_args);
            return 0;
        } else if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            printf("Usage: fullmoon [OPTIONS]\n\n"
                   "Unified Moonlight companion daemon (Clipboard sync + On-demand Camera & Mic)\n\n"
                   "Options:\n"
                   "  (no args), --wrap <cmd...>   Launch Moonlight under supervisor with auto clipboard & on-demand media\n"
                   "  --serve                      Run host server daemon (systemd socket-activated)\n"
                   "  --select                     Open Wayland Layer-Shell device selector ($mod+s)\n"
                   "  --select-cam                 Open Wayland Layer-Shell webcam selector\n"
                   "  --select-mic                 Open Wayland Layer-Shell microphone selector\n"
                   "  --start                      Start streaming feed manually\n"
                   "  --stop                       Stop active streaming feed\n"
                   "  --status                     Print target host, active camera and microphone\n"
                   "  --sync-key [HOST]            Authorize secret key on remote host via SSH\n"
                   "  --self-test                  Run cryptographic and protocol test vectors\n"
                   "  -h, --help                   Show this help message\n");
            return 0;
        }
    }

    /* Default client action: wrap Moonlight */
    char *default_child[] = {"flatpak", "run", "com.moonlight_stream.Moonlight", NULL};
    char *native_child[] = {"moonlight", NULL};
    if (system("which moonlight >/dev/null 2>&1") == 0) {
        run_client_wrapper(native_child);
    } else {
        run_client_wrapper(default_child);
    }
    return 0;
}

