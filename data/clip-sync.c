/*
 * clip-sync.c - High-performance, zero-idle-overhead, two-way clipboard
 *               synchronization daemon between Sunshine host (headless Sway)
 *               and Moonlight client (Wayland/Sway).
 *
 * Design & Security Highlights:
 * - End-to-end authenticated encryption (ChaCha20 + HMAC-SHA256, RFC 8439).
 * - Fresh 16-byte random session salt on handshake with directional key
 *   derivation (C2H and H2C) eliminating ChaCha20 nonce reuse across reconnections.
 * - Chunked 64KB AEAD framing preventing pre-auth heap exhaustion attacks.
 * - Zero idle overhead (0 CPU, 0 RAM when inactive):
 *   - Host: Systemd socket-activated on TCP port 46003 (clip-sync.socket).
 *           Exits cleanly on client disconnect or idle timeout.
 *   - Client: Transparent process supervisor (--wrap <moonlight>), monitoring
 *             Moonlight process group with signalfd; exits immediately when Moonlight stops.
 * - Native Wayland wire protocol without libwayland-client for pure static binary.
 *   - Implements zwlr_data_control_v1 (and ext_data_control_v1).
 *   - Decoupled SCM_RIGHTS FD queue handling for Wayland UNIX domain sockets.
 *   - Non-blocking asynchronous pipe I/O with 3.0s total transfer deadlines.
 * - Dual-layer echo suppression (32-byte SHA-256 digest ring buffer + source tracking).
 * - Aggressive TCP keepalives (~11s dead-peer detection for Wi-Fi roam/sleep).
 * - Security hardening: PR_SET_DUMPABLE=0, RLIMIT_CORE=0, explicit_bzero memory scrubbing.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

#define CLIP_PORT_DEFAULT        46003
#define CLIP_MAGIC               0x50494C43U /* "CLIP" in LE */
#define CLIP_VERSION             1
#define MAX_PAYLOAD_BYTES        (16 * 1024 * 1024) /* 16 MB max */
#define CHUNK_SIZE_BYTES         (64 * 1024)        /* 64 KB AEAD chunks */
#define RING_DIGESTS_COUNT       16
#define KEY_LEN                  32
#define SALT_LEN                 16
#define MAX_KEYS                 16
#define TRANSFER_TIMEOUT_MSEC    3000
#define HANDSHAKE_TIMEOUT_MSEC   3000
#define IDLE_TIMEOUT_SEC         60

/* MIME codes */
#define MIME_CODE_TEXT_UTF8      1
#define MIME_CODE_PNG            2
#define MIME_CODE_TEXT_PLAIN     3

/* Packet types */
#define PKT_HANDSHAKE_REQ        0x01
#define PKT_HANDSHAKE_RESP       0x02
#define PKT_CLIP_START           0x10
#define PKT_CLIP_CHUNK           0x11
#define PKT_CLIP_END             0x12
#define PKT_PING                 0x20
#define PKT_PONG                 0x21

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
 * Configuration & Key Parsing (matching meet-stream conventions)
 * ============================================================================ */
typedef struct {
    char target_host[128];
    char lan_host[128];
    char wan_host[128];
    char lan_ssid[128];
    int port;
    char secret_key_file[512];
    char secret_key_hex[128];
} ClipConfig;

static ClipConfig g_cfg;
static uint8_t g_keys[MAX_KEYS][KEY_LEN];
static int g_key_count = 0;
static char g_home_dir[256] = "";
static char g_runtime_dir[256] = "";

static void expand_tilde_path(const char *in, char *out, size_t out_sz) {
    if (in[0] == '~' && (in[1] == '/' || in[1] == '\0')) {
        snprintf(out, out_sz, "%s%s", g_home_dir, in + 1);
    } else {
        snprintf(out, out_sz, "%s", in);
    }
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
    }
    return g_key_count > 0;
}

static void load_config(void) {
    g_cfg.port = CLIP_PORT_DEFAULT;
    snprintf(g_cfg.target_host, sizeof(g_cfg.target_host), "auto");
    snprintf(g_cfg.lan_host, sizeof(g_cfg.lan_host), "192.168.1.12");
    snprintf(g_cfg.wan_host, sizeof(g_cfg.wan_host), "sncr.duckdns.org");
    snprintf(g_cfg.lan_ssid, sizeof(g_cfg.lan_ssid), "ev");
    snprintf(g_cfg.secret_key_file, sizeof(g_cfg.secret_key_file), "%s/.dotfiles/tmp/meet-stream.key", g_home_dir);

    char cpath[512];
    snprintf(cpath, sizeof(cpath), "%s/.config/meet-stream.conf", g_home_dir);
    FILE *fp = fopen(cpath, "r");
    if (!fp) {
        snprintf(cpath, sizeof(cpath), "%s/.dotfiles/meet-stream.conf", g_home_dir);
        fp = fopen(cpath, "r");
    }
    if (fp) {
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
            char *kend = key + strlen(key) - 1;
            while (kend > key && isspace((unsigned char)*kend)) *kend-- = '\0';
            while (*val && isspace((unsigned char)*val)) val++;
            char *vend = val + strlen(val) - 1;
            while (vend > val && (*vend == '\n' || *vend == '\r' || isspace((unsigned char)*vend))) *vend-- = '\0';

            if (strcasecmp(key, "target_host") == 0) snprintf(g_cfg.target_host, sizeof(g_cfg.target_host), "%s", val);
            else if (strcasecmp(key, "lan_host") == 0) snprintf(g_cfg.lan_host, sizeof(g_cfg.lan_host), "%s", val);
            else if (strcasecmp(key, "wan_host") == 0) snprintf(g_cfg.wan_host, sizeof(g_cfg.wan_host), "%s", val);
            else if (strcasecmp(key, "lan_ssid") == 0) snprintf(g_cfg.lan_ssid, sizeof(g_cfg.lan_ssid), "%s", val);
            else if (strcasecmp(key, "secret_key_file") == 0) expand_tilde_path(val, g_cfg.secret_key_file, sizeof(g_cfg.secret_key_file));
            else if (strcasecmp(key, "clip_port") == 0) g_cfg.port = atoi(val);
        }
        fclose(fp);
    }
}

/* ============================================================================
 * Directional Key Derivation & Packet Framing
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
    uint32_t magic;      /* CLIP_MAGIC */
    uint8_t  version;    /* CLIP_VERSION */
    uint8_t  type;       /* PKT_* */
    uint8_t  salt[SALT_LEN];
    uint8_t  hmac[16];   /* Truncated HMAC-SHA256 */
} HandshakePacket;

typedef struct {
    uint32_t magic;      /* CLIP_MAGIC */
    uint8_t  version;    /* CLIP_VERSION */
    uint8_t  type;       /* PKT_* */
    uint16_t mime_code;  /* MIME_CODE_* */
    uint32_t total_len;  /* Total payload bytes across all chunks */
    uint16_t chunk_seq;  /* 0, 1, ... */
    uint16_t chunk_len;  /* Ciphertext bytes in this frame */
    uint32_t seq;        /* Frame sequence number */
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
    hdr.magic = htonl(CLIP_MAGIC);
    hdr.version = CLIP_VERSION;
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

    /* Compute HMAC over Header (excluding hmac field) + Ciphertext */
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

    if (ntohl(out_hdr->magic) != CLIP_MAGIC || out_hdr->version != CLIP_VERSION) {
        return false;
    }
    uint16_t chunk_len = ntohs(out_hdr->chunk_len);
    if (wire_len != sizeof(FrameHeader) + chunk_len) return false;

    /* Verify HMAC */
    uint8_t full_hmac[32];
    size_t hdr_auth_len = sizeof(FrameHeader) - 16;
    const uint8_t *cipher = wire_buf + sizeof(FrameHeader);
    hmac_sha256(sc->key_recv, 32, out_hdr, hdr_auth_len, cipher, chunk_len, full_hmac);

    if (crypto_memcmp(full_hmac, out_hdr->hmac, 16) != 0) {
        safe_memzero(full_hmac, sizeof(full_hmac));
        return false;
    }
    safe_memzero(full_hmac, sizeof(full_hmac));

    /* Decrypt */
    if (chunk_len > 0 && plain_out) {
        chacha20_crypt(sc->key_recv, 1, out_hdr->nonce, cipher, plain_out, chunk_len);
    }
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
static uint32_t g_id_registry = 1;
static uint32_t g_id_seat = 0;
static uint32_t g_id_manager = 0;
static uint32_t g_id_device = 0;
static uint32_t g_id_cur_offer = 0;
static uint32_t g_id_active_source = 0;

static char g_last_offered_mime[64] = "";
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

/* Forward declarations */
static void broadcast_local_clipboard(const uint8_t *data, size_t len, uint16_t mime_code);
static void handle_remote_clipboard(const uint8_t *data, size_t len, uint16_t mime_code);

/* Read entire contents of a pipe with timeout */
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
            if (n == 0) break; /* EOF */
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

static void process_wayland_events(void) {
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
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
                size_t nfds = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                const int *fds = (const int *)CMSG_DATA(cm);
                for (size_t i = 0; i < nfds; i++) {
                    if (fds[i] >= 0) fd_queue_push(fds[i]);
                }
            }
        }
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
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

        /* Registry global */
        if (obj_id == g_id_registry && opcode == 0) {
            uint32_t name = *(const uint32_t *)p;
            uint32_t iface_len = *(const uint32_t *)(p + 4);
            const char *iface = (const char *)(p + 8);
            uint32_t ver_offset = 8 + ((iface_len + 3) & ~3);
            uint32_t ver = *(const uint32_t *)(p + ver_offset);

            if (strcmp(iface, "wl_seat") == 0 && g_id_seat == 0) {
                g_id_seat = g_wl_next_id++;
                uint32_t b[6] = {name, 0, 7, 0, g_id_seat, (ver < 7 ? ver : 7)};
                memcpy(b + 1, "wl_seat\0", 8);
                wl_send_msg(g_id_registry, 0, b, 24, -1);
            } else if ((strcmp(iface, "zwlr_data_control_manager_v1") == 0 ||
                        strcmp(iface, "ext_data_control_manager_v1") == 0) && g_id_manager == 0) {
                g_id_manager = g_wl_next_id++;
                size_t nlen = strlen(iface) + 1;
                size_t pad = (4 - (nlen % 4)) % 4;
                size_t blen = 4 + 4 + nlen + pad + 4 + 4;
                uint8_t *b = (uint8_t *)calloc(1, blen);
                if (b) {
                    store_le32(b, name);
                    store_le32(b + 4, (uint32_t)nlen);
                    memcpy(b + 8, iface, nlen);
                    store_le32(b + 8 + nlen + pad, 1); /* version 1 or 2 */
                    store_le32(b + 8 + nlen + pad + 4, g_id_manager);
                    wl_send_msg(g_id_registry, 0, b, blen, -1);
                    free(b);
                }
            }
        }
        /* Data control device events */
        else if (obj_id == g_id_device) {
            if (opcode == 0 /* data_offer(id) */) {
                uint32_t offer_id = *(const uint32_t *)p;
                g_id_cur_offer = offer_id;
                g_last_offered_mime[0] = '\0';
            } else if (opcode == 1 /* selection(offer_id) */) {
                uint32_t sel_id = *(const uint32_t *)p;
                if (sel_id != 0 && sel_id == g_id_cur_offer && g_last_offered_mime[0] != '\0') {
                    int pfd[2];
                    if (pipe2(pfd, O_CLOEXEC) == 0) {
                        /* offer.receive(mime_type, fd) */
                        uint32_t slen = (uint32_t)strlen(g_last_offered_mime) + 1;
                        uint32_t pad = (4 - (slen % 4)) % 4;
                        size_t plen = 4 + slen + pad;
                        uint8_t *mbuf = (uint8_t *)calloc(1, plen);
                        if (mbuf) {
                            store_le32(mbuf, slen);
                            memcpy(mbuf + 4, g_last_offered_mime, slen);
                            wl_send_msg(g_id_cur_offer, 0 /* receive */, mbuf, plen, pfd[1]);
                            free(mbuf);
                        }
                        close(pfd[1]); /* Critical: close local write-end so read reaches EOF */

                        size_t clip_len = 0;
                        uint8_t *clip_data = read_pipe_all(pfd[0], &clip_len, TRANSFER_TIMEOUT_MSEC);
                        if (clip_data && clip_len > 0) {
                            uint8_t d[32];
                            sha256(clip_data, clip_len, d);
                            if (!echo_ring_contains(d)) {
                                echo_ring_add(d);
                                uint16_t mcode = (strcmp(g_last_offered_mime, "image/png") == 0) ?
                                                 MIME_CODE_PNG : MIME_CODE_TEXT_UTF8;
                                broadcast_local_clipboard(clip_data, clip_len, mcode);
                            }
                            free(clip_data);
                        }
                    }
                }
            }
        }
        /* Data control offer events */
        else if (obj_id == g_id_cur_offer) {
            if (opcode == 0 /* offer(mime_type) */) {
                uint32_t slen = *(const uint32_t *)p;
                const char *mime = (const char *)(p + 4);
                if (slen > 0) {
                    if (strcmp(mime, "image/png") == 0) {
                        snprintf(g_last_offered_mime, sizeof(g_last_offered_mime), "image/png");
                    } else if (strcmp(mime, "text/plain;charset=utf-8") == 0 ||
                               strcmp(mime, "text/plain") == 0 ||
                               strcmp(mime, "UTF8_STRING") == 0) {
                        if (strcmp(g_last_offered_mime, "image/png") != 0) {
                            snprintf(g_last_offered_mime, sizeof(g_last_offered_mime), "%s", mime);
                        }
                    }
                }
            }
        }
        /* Data control source events */
        else if (obj_id == g_id_active_source) {
            if (opcode == 0 /* send(mime_type, fd) */) {
                int send_fd = fd_queue_pop();
                if (send_fd >= 0) {
                    if (g_current_clip_data && g_current_clip_len > 0) {
                        /* Non-blocking write to pasting app */
                        int fl = fcntl(send_fd, F_GETFL, 0);
                        fcntl(send_fd, F_SETFL, fl | O_NONBLOCK);
                        size_t written = 0;
                        while (written < g_current_clip_len) {
                            ssize_t nw = write(send_fd, g_current_clip_data + written, g_current_clip_len - written);
                            if (nw <= 0) {
                                if (nw < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                                    struct pollfd wfd = {.fd = send_fd, .events = POLLOUT};
                                    if (poll(&wfd, 1, 500) > 0) continue;
                                }
                                break;
                            }
                            written += (size_t)nw;
                        }
                    }
                    close(send_fd);
                }
            } else if (opcode == 1 /* cancelled */) {
                wl_send_msg(g_id_active_source, 1 /* destroy */, NULL, 0, -1);
                g_id_active_source = 0;
            }
        }

        pos += len;
    }

    if (pos > 0) {
        if (pos < rfilled) memmove(rbuf, rbuf + pos, rfilled - pos);
        rfilled -= pos;
    }
}

static bool wayland_init(void) {
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir) runtime_dir = g_runtime_dir;

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

    struct sockaddr_un un;
    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), "%s", wl_path);

    if (connect(g_wl_fd, (struct sockaddr *)&un, sizeof(un)) < 0) {
        close(g_wl_fd);
        g_wl_fd = -1;
        return false;
    }

    /* Get registry */
    wl_send_u32_1(1 /* wl_display */, 1 /* get_registry */, g_id_registry);

    /* Pump events until seat and data_control_manager are found */
    uint64_t start = now_msec();
    while ((g_id_seat == 0 || g_id_manager == 0) && (now_msec() - start < 1500)) {
        struct pollfd pfd = {.fd = g_wl_fd, .events = POLLIN};
        if (poll(&pfd, 1, 200) > 0) {
            process_wayland_events();
        }
    }

    if (g_id_seat == 0 || g_id_manager == 0) {
        close(g_wl_fd);
        g_wl_fd = -1;
        return false;
    }

    /* Create data control device */
    g_id_device = g_wl_next_id++;
    wl_send_u32_2(g_id_manager, 1 /* get_data_device */, g_id_device, g_id_seat);
    return true;
}

static void handle_remote_clipboard(const uint8_t *data, size_t len, uint16_t mime_code) {
    if (!data || len == 0 || g_wl_fd < 0 || g_id_device == 0) return;

    /* Add to echo ring buffer so we don't reflect it back */
    uint8_t d[32];
    sha256(data, len, d);
    echo_ring_add(d);

    /* Clean up old source if active */
    if (g_id_active_source != 0) {
        wl_send_msg(g_id_active_source, 1 /* destroy */, NULL, 0, -1);
        g_id_active_source = 0;
    }

    /* Cache buffer */
    if (g_current_clip_data) free(g_current_clip_data);
    g_current_clip_data = (uint8_t *)malloc(len);
    if (!g_current_clip_data) return;
    memcpy(g_current_clip_data, data, len);
    g_current_clip_len = len;
    g_current_clip_mime = mime_code;

    /* Create new data control source */
    g_id_active_source = g_wl_next_id++;
    wl_send_u32_1(g_id_manager, 0 /* create_data_source */, g_id_active_source);

    /* Advertise MIME types */
    if (mime_code == MIME_CODE_PNG) {
        wl_send_string(g_id_active_source, 0 /* offer */, "image/png");
    } else {
        wl_send_string(g_id_active_source, 0 /* offer */, "text/plain;charset=utf-8");
        wl_send_string(g_id_active_source, 0 /* offer */, "text/plain");
        wl_send_string(g_id_active_source, 0 /* offer */, "UTF8_STRING");
    }

    /* Set selection */
    wl_send_u32_1(g_id_device, 0 /* set_selection */, g_id_active_source);
}

/* ============================================================================
 * Network Peer & Session Reactor
 * ============================================================================ */
static int g_peer_net_fd = -1;
static SessionCrypto g_session_crypto;

static void configure_socket_keepalives(int fd) {
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    int idle = 5, intvl = 2, cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    unsigned int timeout_ms = 8000;
    setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
}

static void broadcast_local_clipboard(const uint8_t *data, size_t len, uint16_t mime_code) {
    if (g_peer_net_fd < 0 || !g_session_crypto.established || len == 0 || len > MAX_PAYLOAD_BYTES) return;

    size_t offset = 0;
    uint16_t chunk_seq = 0;
    uint8_t wire_buf[sizeof(FrameHeader) + CHUNK_SIZE_BYTES];

    while (offset < len) {
        size_t chunk_sz = len - offset;
        if (chunk_sz > CHUNK_SIZE_BYTES) chunk_sz = CHUNK_SIZE_BYTES;

        size_t wire_len = 0;
        seal_frame(&g_session_crypto, PKT_CLIP_CHUNK, mime_code,
                   (uint32_t)len, chunk_seq++,
                   data + offset, (uint16_t)chunk_sz,
                   wire_buf, &wire_len);

        ssize_t nw = write(g_peer_net_fd, wire_buf, wire_len);
        (void)nw;
        offset += chunk_sz;
    }
}

/* Incoming clipboard chunk assembly */
static uint8_t *g_in_clip_buf = NULL;
static size_t g_in_clip_len = 0;
static size_t g_in_clip_received = 0;
static uint16_t g_in_clip_mime = MIME_CODE_TEXT_UTF8;

static void handle_net_frame(const FrameHeader *hdr, const uint8_t *plain, size_t chunk_len) {
    if (hdr->type == PKT_CLIP_CHUNK) {
        uint32_t total = ntohl(hdr->total_len);
        uint16_t chunk_seq = ntohs(hdr->chunk_seq);
        uint16_t mcode = ntohs(hdr->mime_code);

        if (total > MAX_PAYLOAD_BYTES) return;

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

/* ============================================================================
 * Self-Test Suite (RFC vectors, HKDF, Framing, Ring Buffer)
 * ============================================================================ */
static bool test_rfc_vectors(void) {
    /* ChaCha20 RFC 8439 Vector */
    const uint8_t key[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
    };
    const uint8_t nonce[12] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a,
        0x00, 0x00, 0x00, 0x00
    };
    const char *plain = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    size_t plen = strlen(plain);
    uint8_t cipher[128];
    chacha20_crypt(key, 1, nonce, (const uint8_t *)plain, cipher, plen);

    /* Verify first 4 bytes of ciphertext against RFC 8439 */
    const uint8_t expected_prefix[4] = {0x6e, 0x2e, 0x35, 0x9a};
    if (memcmp(cipher, expected_prefix, 4) != 0) return false;

    /* Verify roundtrip */
    uint8_t decrypted[128];
    chacha20_crypt(key, 1, nonce, cipher, decrypted, plen);
    if (memcmp(decrypted, plain, plen) != 0) return false;

    return true;
}

static bool test_crypto_handshake(void) {
    uint8_t master[32];
    memset(master, 0x42, sizeof(master));
    uint8_t salt[SALT_LEN] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

    SessionCrypto sc_client, sc_host;
    derive_session_keys(master, salt, true, &sc_client);
    derive_session_keys(master, salt, false, &sc_host);

    /* Ensure directional keys are distinct */
    if (crypto_memcmp(sc_client.key_send, sc_client.key_recv, 32) == 0) return false;
    if (crypto_memcmp(sc_client.key_send, sc_host.key_recv, 32) != 0) return false;
    if (crypto_memcmp(sc_client.key_recv, sc_host.key_send, 32) != 0) return false;

    /* Frame test */
    const char *sample = "Moonlight Two-Way Clipboard Sync Payload";
    size_t slen = strlen(sample);
    uint8_t wire[512];
    size_t wlen = 0;

    if (!seal_frame(&sc_client, PKT_CLIP_CHUNK, MIME_CODE_TEXT_UTF8, (uint32_t)slen, 0, (const uint8_t *)sample, (uint16_t)slen, wire, &wlen)) {
        return false;
    }

    FrameHeader h;
    uint8_t opened[512];
    if (!open_frame(&sc_host, wire, wlen, &h, opened)) {
        return false;
    }

    if (memcmp(opened, sample, slen) != 0) return false;
    return true;
}

static int run_self_test(void) {
    printf("clip-sync self-test: running RFC and cryptographic verification...\n");
    if (!test_rfc_vectors()) {
        fprintf(stderr, "clip-sync: RFC 8439 ChaCha20 test vector FAILED\n");
        return 1;
    }
    if (!test_crypto_handshake()) {
        fprintf(stderr, "clip-sync: Directional handshake & framing FAILED\n");
        return 2;
    }

    /* Test echo ring buffer */
    uint8_t d1[32] = {1}, d2[32] = {2};
    echo_ring_add(d1);
    if (!echo_ring_contains(d1)) return 3;
    if (echo_ring_contains(d2)) return 4;

    printf("clip-sync self-test OK: rfc8439=PASS | directional_keys=PASS | framing=PASS | echo_suppression=PASS\n");
    return 0;
}

/* ============================================================================
 * Host Server Mode (--serve)
 * ============================================================================ */
static void run_server(void) {
    if (!ensure_keys()) {
        fprintf(stderr, "clip-sync: failed to load secret key from %s\n", g_cfg.secret_key_file);
        exit(1);
    }

    int listen_fd = -1;
    /* Check systemd socket activation */
    const char *listen_pid_str = getenv("LISTEN_PID");
    const char *listen_fds_str = getenv("LISTEN_FDS");
    if (listen_pid_str && listen_fds_str && atoi(listen_pid_str) == getpid() && atoi(listen_fds_str) >= 1) {
        listen_fd = 3; /* SD_LISTEN_FDS_START */
    } else {
        listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd < 0) { perror("socket"); exit(1); }
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons((uint16_t)g_cfg.port);
        sin.sin_addr.s_addr = INADDR_ANY;
        if (bind(listen_fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
            perror("bind");
            exit(1);
        }
        if (listen(listen_fd, 4) < 0) {
            perror("listen");
            exit(1);
        }
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create1"); exit(1); }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    /* Cold-start retry for headless Sway socket (up to 3 seconds) */
    uint64_t wl_start = now_msec();
    while (!wayland_init() && (now_msec() - wl_start < 3000)) {
        usleep(150000);
    }

    if (g_wl_fd >= 0) {
        ev.events = EPOLLIN;
        ev.data.fd = g_wl_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, g_wl_fd, &ev);
    }

    uint8_t net_rbuf[sizeof(FrameHeader) + CHUNK_SIZE_BYTES];
    size_t net_rfilled = 0;
    uint64_t last_active = now_msec();

    while (true) {
        struct epoll_event events[8];
        int nfds = epoll_wait(epfd, events, 8, 2000);

        if (g_peer_net_fd < 0 && (now_msec() - last_active > (uint64_t)IDLE_TIMEOUT_SEC * 1000ULL)) {
            break; /* Exit cleanly if idle with no connected client */
        }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                struct sockaddr_in cin;
                socklen_t clen = sizeof(cin);
                int client_fd = accept4(listen_fd, (struct sockaddr *)&cin, &clen, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (client_fd >= 0) {
                    /* Supersede existing client session if reconnected */
                    if (g_peer_net_fd >= 0) {
                        epoll_ctl(epfd, EPOLL_CTL_DEL, g_peer_net_fd, NULL);
                        close(g_peer_net_fd);
                    }
                    configure_socket_keepalives(client_fd);
                    g_peer_net_fd = client_fd;
                    net_rfilled = 0;

                    /* Generate random salt */
                    uint8_t salt[SALT_LEN];
                    int rfd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
                    if (rfd >= 0) {
                        ssize_t rnr = read(rfd, salt, sizeof(salt));
                        (void)rnr;
                        close(rfd);
                    } else {
                        for (int j = 0; j < SALT_LEN; j++) salt[j] = (uint8_t)rand();
                    }

                    derive_session_keys(g_keys[0], salt, false, &g_session_crypto);

                    /* Send authenticated handshake salt */
                    HandshakePacket hp;
                    memset(&hp, 0, sizeof(hp));
                    hp.magic = htonl(CLIP_MAGIC);
                    hp.version = CLIP_VERSION;
                    hp.type = PKT_HANDSHAKE_RESP;
                    memcpy(hp.salt, salt, SALT_LEN);
                    uint8_t full_hmac[32];
                    hmac_sha256(g_keys[0], 32, &hp, sizeof(hp) - 16, NULL, 0, full_hmac);
                    memcpy(hp.hmac, full_hmac, 16);

                    ssize_t nw = write(g_peer_net_fd, &hp, sizeof(hp));
                    (void)nw;

                    ev.events = EPOLLIN | EPOLLRDHUP;
                    ev.data.fd = g_peer_net_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, g_peer_net_fd, &ev);
                    last_active = now_msec();
                }
            } else if (fd == g_wl_fd) {
                process_wayland_events();
            } else if (fd == g_peer_net_fd) {
                if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, g_peer_net_fd, NULL);
                    close(g_peer_net_fd);
                    g_peer_net_fd = -1;
                    g_session_crypto.established = false;
                    last_active = now_msec();
                    continue;
                }

                ssize_t nr = read(g_peer_net_fd, net_rbuf + net_rfilled, sizeof(net_rbuf) - net_rfilled);
                if (nr <= 0) {
                    if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                    epoll_ctl(epfd, EPOLL_CTL_DEL, g_peer_net_fd, NULL);
                    close(g_peer_net_fd);
                    g_peer_net_fd = -1;
                    g_session_crypto.established = false;
                    last_active = now_msec();
                    continue;
                }
                net_rfilled += (size_t)nr;
                last_active = now_msec();

                while (net_rfilled >= sizeof(FrameHeader)) {
                    FrameHeader *hdr = (FrameHeader *)net_rbuf;
                    uint16_t ch_len = ntohs(hdr->chunk_len);
                    size_t frame_total = sizeof(FrameHeader) + ch_len;
                    if (net_rfilled < frame_total) break;

                    uint8_t plain[CHUNK_SIZE_BYTES];
                    FrameHeader opened_h;
                    if (open_frame(&g_session_crypto, net_rbuf, frame_total, &opened_h, plain)) {
                        handle_net_frame(&opened_h, plain, ch_len);
                    }
                    if (net_rfilled > frame_total) {
                        memmove(net_rbuf, net_rbuf + frame_total, net_rfilled - frame_total);
                    }
                    net_rfilled -= frame_total;
                }
            }
        }
    }

    if (g_peer_net_fd >= 0) close(g_peer_net_fd);
    if (g_wl_fd >= 0) close(g_wl_fd);
    if (listen_fd >= 0) close(listen_fd);
    close(epfd);
}

/* ============================================================================
 * Client Mode with Process Supervisor (--wrap <moonlight>)
 * ============================================================================ */
static bool resolve_target_host(char *out_host, size_t out_sz) {
    if (strcasecmp(g_cfg.target_host, "auto") == 0) {
        /* Check local network reachability */
        struct sockaddr_in sin;
        sin.sin_family = AF_INET;
        sin.sin_port = htons((uint16_t)g_cfg.port);
        if (inet_pton(AF_INET, g_cfg.lan_host, &sin.sin_addr) > 0) {
            int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
            if (s >= 0) {
                connect(s, (struct sockaddr *)&sin, sizeof(sin));
                struct pollfd pfd = {.fd = s, .events = POLLOUT};
                int r = poll(&pfd, 1, 150);
                close(s);
                if (r > 0) {
                    snprintf(out_host, out_sz, "%s", g_cfg.lan_host);
                    return true;
                }
            }
        }
        snprintf(out_host, out_sz, "%s", g_cfg.wan_host);
    } else {
        snprintf(out_host, out_sz, "%s", g_cfg.target_host);
    }
    return true;
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

static int client_connect_to_host(void) {
    char host[128];
    resolve_target_host(host, sizeof(host));

    struct sockaddr_in sin;
    if (!resolve_ipv4_sockaddr(host, g_cfg.port, &sin)) return -1;

    int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;

    connect(s, (struct sockaddr *)&sin, sizeof(sin));

    struct pollfd pfd = {.fd = s, .events = POLLOUT};
    if (poll(&pfd, 1, 1500) <= 0) {
        close(s);
        return -1;
    }
    int err = 0;
    socklen_t elen = sizeof(err);
    getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen);
    if (err != 0) { close(s); return -1; }

    configure_socket_keepalives(s);

    /* Handshake: Read salt from host */
    struct pollfd rpfd = {.fd = s, .events = POLLIN};
    if (poll(&rpfd, 1, HANDSHAKE_TIMEOUT_MSEC) <= 0) {
        close(s);
        return -1;
    }

    HandshakePacket hp;
    ssize_t nr = read(s, &hp, sizeof(hp));
    if (nr != (ssize_t)sizeof(hp)) {
        close(s);
        return -1;
    }

    if (ntohl(hp.magic) != CLIP_MAGIC || hp.version != CLIP_VERSION || hp.type != PKT_HANDSHAKE_RESP) {
        close(s);
        return -1;
    }

    /* Verify handshake HMAC with master key */
    uint8_t full_hmac[32];
    hmac_sha256(g_keys[0], 32, &hp, sizeof(hp) - 16, NULL, 0, full_hmac);
    if (crypto_memcmp(full_hmac, hp.hmac, 16) != 0) {
        close(s);
        return -1;
    }

    derive_session_keys(g_keys[0], hp.salt, true, &g_session_crypto);
    return s;
}

static void run_client_wrapper(char **argv_child) {
    if (!ensure_keys()) {
        fprintf(stderr, "clip-sync: failed to load secret key from %s\n", g_cfg.secret_key_file);
        exit(1);
    }

    /* Signal mask & signalfd for supervisor */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    int sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sig_fd < 0) { perror("signalfd"); exit(1); }

    /* Fork child process */
    pid_t child_pid = fork();
    if (child_pid == 0) {
        /* Child: isolate in own process group so supervisor crash doesn't kill Moonlight */
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

    int exit_code = 0;
    bool child_running = true;

    while (child_running) {
        uint64_t now = now_msec();

        /* Attempt reconnect if disconnected */
        if (g_peer_net_fd < 0 && now >= next_reconnect) {
            g_peer_net_fd = client_connect_to_host();
            if (g_peer_net_fd >= 0) {
                ev.events = EPOLLIN | EPOLLRDHUP;
                ev.data.fd = g_peer_net_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, g_peer_net_fd, &ev);
                backoff_ms = 200;
            } else {
                next_reconnect = now + (uint64_t)backoff_ms;
                backoff_ms = (backoff_ms < 5000) ? backoff_ms * 2 : 5000;
            }
        }

        struct epoll_event events[8];
        int nfds = epoll_wait(epfd, events, 8, 250);

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

                            /* Guard against flatpak immediate D-Bus handoff exits (< 1.0s) */
                            if (exit_code == 0 && (now_msec() - launch_time < 1000)) {
                                /* Check if moonlight-qt or flatpak instance is running in background */
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
            } else if (fd == g_peer_net_fd) {
                if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, g_peer_net_fd, NULL);
                    close(g_peer_net_fd);
                    g_peer_net_fd = -1;
                    g_session_crypto.established = false;
                    next_reconnect = now_msec() + (uint64_t)backoff_ms;
                    continue;
                }

                ssize_t nr = read(g_peer_net_fd, net_rbuf + net_rfilled, sizeof(net_rbuf) - net_rfilled);
                if (nr <= 0) {
                    if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                    epoll_ctl(epfd, EPOLL_CTL_DEL, g_peer_net_fd, NULL);
                    close(g_peer_net_fd);
                    g_peer_net_fd = -1;
                    g_session_crypto.established = false;
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
                    if (open_frame(&g_session_crypto, net_rbuf, frame_total, &opened_h, plain)) {
                        handle_net_frame(&opened_h, plain, ch_len);
                    }
                    if (net_rfilled > frame_total) {
                        memmove(net_rbuf, net_rbuf + frame_total, net_rfilled - frame_total);
                    }
                    net_rfilled -= frame_total;
                }
            }
        }
    }

    if (g_peer_net_fd >= 0) close(g_peer_net_fd);
    if (g_wl_fd >= 0) close(g_wl_fd);
    close(epfd);
    close(sig_fd);
    exit(exit_code);
}

/* ============================================================================
 * Main Entry Point & Argument Dispatch
 * ============================================================================ */
static void init_paths(void) {
    const char *home = getenv("HOME");
    if (home) snprintf(g_home_dir, sizeof(g_home_dir), "%s", home);
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt) snprintf(g_runtime_dir, sizeof(g_runtime_dir), "%s", rt);
    else snprintf(g_runtime_dir, sizeof(g_runtime_dir), "/run/user/%u", getuid());
}

int main(int argc, char **argv) {
    init_paths();
    load_config();

    /* Hardening: zero core dumps, ignore SIGPIPE */
    struct rlimit rl = {0, 0};
    setrlimit(RLIMIT_CORE, &rl);
    prctl(PR_SET_DUMPABLE, 0);
    signal(SIGPIPE, SIG_IGN);

    if (argc >= 2) {
        if (strcmp(argv[1], "--self-test") == 0) {
            return run_self_test();
        } else if (strcmp(argv[1], "--serve") == 0) {
            run_server();
            return 0;
        } else if (strcmp(argv[1], "--wrap") == 0) {
            char **child_args = &argv[2];
            char *fallback_flatpak[] = {"flatpak", "run", "com.moonlight_stream.Moonlight", NULL};
            char *fallback_native[] = {"moonlight", NULL};

            if (argc < 3) {
                if (system("which moonlight >/dev/null 2>&1") == 0) {
                    child_args = fallback_native;
                } else {
                    child_args = fallback_flatpak;
                }
            } else {
                /* Strip literal "%u" or "%U" if passed unexpanded by desktop launcher */
                for (int i = 2; i < argc; i++) {
                    if (strcmp(argv[i], "%u") == 0 || strcmp(argv[i], "%U") == 0) {
                        argv[i] = NULL;
                        break;
                    }
                }
            }
            run_client_wrapper(child_args);
            return 0;
        } else if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            printf("Usage: clip-sync [OPTIONS]\n"
                   "Two-way encrypted clipboard synchronizer for Sunshine and Moonlight\n\n"
                   "Options:\n"
                   "  --wrap <command...>  Launch command under client clipboard sync supervisor\n"
                   "  --serve              Run host clipboard sync server (systemd socket-activated)\n"
                   "  --self-test          Run cryptographic and framing self-test suite\n"
                   "  --help               Display this help text\n");
            return 0;
        }
    }

    /* Default client standalone mode if run without arguments */
    char *default_child[] = {"flatpak", "run", "com.moonlight_stream.Moonlight", NULL};
    run_client_wrapper(default_child);
    return 0;
}
