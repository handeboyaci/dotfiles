/*
 * meet-stream.c - Secure, configurable, zero-idle-overhead webcam & microphone
 *                 forwarding daemon, on-demand remote receiver, and native
 *                 Wayland Layer-Shell device selector overlay.
 *
 * Features:
 * - End-to-end authenticated encryption (ChaCha20 + HMAC-SHA256-128 + 64-bit
 *   replay protection) over a single multiplexed UDP port.
 * - Zero RAM / Zero CPU when idle on both client and remote host:
 *   - Client daemon runs only while streaming is enabled.
 *   - Remote receiver (--serve) is socket-activated on demand via systemd
 *     (moonlight-cam.socket), verifies packet MACs before spawning GStreamer,
 *     forwards decrypted Opus RTP to 127.0.0.1:46000 (PipeWire loopback source)
 *     without needing an extra audio GStreamer process, spawns the local H.264
 *     v4l2sink pipeline only when video frames arrive, and exits immediately on
 *     authenticated STOP packet or inactivity timeout.
 * - Smart Docked vs. Undocked device persistence & fallback:
 *   - Remembers an ordered MRU preference list of webcams and microphones.
 *   - If the preferred docked camera/mic is unplugged, automatically falls back
 *     to an available camera/mic without overwriting the saved docked preference.
 * - Custom Wayland Layer-Shell (zwlr_layer_shell_v1) interactive device picker
 *   rendered directly to wl_shm via FreeType + Noto Color Emoji / Symbola.
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
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
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
void *dlopen(const char *filename, int flags) {
    (void)filename;
    (void)flags;
    return NULL;
}
void *dlsym(void *handle, const char *symbol) {
    (void)handle;
    (void)symbol;
    return NULL;
}
int dlclose(void *handle) {
    (void)handle;
    return 0;
}
char *dlerror(void) {
    return NULL;
}

/* ============================================================================
 * Constants & Protocol Definitions
 * ============================================================================ */
#define MSTR_MAGIC_0 'M'
#define MSTR_MAGIC_1 'S'
#define MSTR_MAGIC_2 'T'
#define MSTR_MAGIC_3 'R'
#define MSTR_VERSION 1

typedef enum {
    PKT_AUDIO       = 1,
    PKT_VIDEO       = 2,
    PKT_HEARTBEAT   = 3,
    PKT_RESET_VIDEO = 4,
    PKT_STOP        = 5
} PacketType;

/* 36-byte wire header:
 * [0..3]   magic "MSTR"
 * [4]      version (1)
 * [5]      pkt_type
 * [6..7]   payload_len (big-endian uint16)
 * [8..15]  session_id (big-endian uint64)
 * [16..23] seq (big-endian uint64)
 * [24..39] HMAC-SHA256-128 authentication tag (16 bytes)
 */
#define MSTR_HDR_LEN 40
#define MSTR_MAX_PAYLOAD 65000
#define KEY_LEN 32

#define MAX_DEVICES 32
#define MAX_PREFS   16

typedef struct {
    char label[128];
    char target[256];
    char devnode[64];
    bool is_disabled_entry;
} DeviceItem;

typedef struct {
    char target_host[128];
    char lan_host[128];
    char wan_host[128];
    char lan_ssid[128];
    int port;
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
} StreamConfig;

typedef struct {
    char cam_prefs[MAX_PREFS][256];
    int cam_pref_count;
    bool cam_disabled;

    char mic_prefs[MAX_PREFS][256];
    int mic_pref_count;
    bool mic_disabled;
} PersistentState;

static StreamConfig g_cfg;
static PersistentState g_pref;
#define MAX_KEYS 16
static uint8_t g_keys[MAX_KEYS][KEY_LEN];
static int g_key_count = 0;
#define g_master_key (g_keys[0])
static bool g_key_loaded = false;

static char g_home_dir[256] = "";
static char g_runtime_dir[256] = "";
static char g_config_dir[256] = "";
static char g_state_dir[256] = "";
static char g_pid_file[512] = "";
static char g_status_file[512] = "";
static char g_pref_file[512] = "";
static char g_legacy_cam_file[512] = "";
static char g_legacy_mic_file[512] = "";

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

static const uint32_t K_SHA256[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static void sha256_transform(Sha256Ctx *ctx, const uint8_t *data) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = load_be32(data + i * 4);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + K_SHA256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(Sha256Ctx *ctx) {
    ctx->state[0] = 0x6a09e667U; ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U; ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU; ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU; ctx->state[7] = 0x5be0cd19U;
    ctx->bitcount = 0;
}

static void sha256_update(Sha256Ctx *ctx, const uint8_t *data, size_t len) {
    size_t idx = (size_t)((ctx->bitcount >> 3) & 0x3F);
    ctx->bitcount += (uint64_t)len << 3;
    size_t part = 64 - idx;
    size_t i = 0;
    if (len >= part) {
        memcpy(ctx->buffer + idx, data, part);
        sha256_transform(ctx, ctx->buffer);
        for (i = part; i + 64 <= len; i += 64) {
            sha256_transform(ctx, data + i);
        }
        idx = 0;
    }
    if (len > i) {
        memcpy(ctx->buffer + idx, data + i, len - i);
    }
}

static void sha256_final(Sha256Ctx *ctx, uint8_t digest[32]) {
    uint8_t pad[64];
    size_t idx = (size_t)((ctx->bitcount >> 3) & 0x3F);
    size_t pad_len = (idx < 56) ? (56 - idx) : (120 - idx);
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    uint8_t bits[8];
    store_be64(bits, ctx->bitcount);
    sha256_update(ctx, pad, pad_len);
    sha256_update(ctx, bits, 8);
    for (int i = 0; i < 8; i++) {
        store_be32(digest + i * 4, ctx->state[i]);
    }
}

static void hmac_sha256(const uint8_t key[32], const uint8_t *msg1, size_t len1,
                        const uint8_t *msg2, size_t len2, uint8_t out[32]) {
    uint8_t k_ipad[64], k_opad[64], inner[32];
    for (int i = 0; i < 32; i++) {
        k_ipad[i] = key[i] ^ 0x36;
        k_opad[i] = key[i] ^ 0x5c;
    }
    memset(k_ipad + 32, 0x36, 32);
    memset(k_opad + 32, 0x5c, 32);

    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, k_ipad, 64);
    if (msg1 && len1 > 0) sha256_update(&ctx, msg1, len1);
    if (msg2 && len2 > 0) sha256_update(&ctx, msg2, len2);
    sha256_final(&ctx, inner);

    sha256_init(&ctx);
    sha256_update(&ctx, k_opad, 64);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
}

#define QR(a, b, c, d) do { \
    a += b; d ^= a; d = rotl32(d, 16); \
    c += d; b ^= c; b = rotl32(b, 12); \
    a += b; d ^= a; d = rotl32(d, 8);  \
    c += d; b ^= c; b = rotl32(b, 7);  \
} while (0)

static void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
                         const uint8_t *in, uint8_t *out, size_t len) {
    uint32_t state[16];
    state[0] = 0x61707865U;
    state[1] = 0x3320646eU;
    state[2] = 0x79622d32U;
    state[3] = 0x6b206574U;
    for (int i = 0; i < 8; i++) state[4 + i] = load_le32(key + i * 4);
    state[12] = counter;
    state[13] = load_le32(nonce + 0);
    state[14] = load_le32(nonce + 4);
    state[15] = load_le32(nonce + 8);

    size_t off = 0;
    while (off < len) {
        uint32_t x[16];
        memcpy(x, state, sizeof(x));
        for (int i = 0; i < 10; i++) {
            QR(x[0], x[4], x[8],  x[12]);
            QR(x[1], x[5], x[9],  x[13]);
            QR(x[2], x[6], x[10], x[14]);
            QR(x[3], x[7], x[11], x[15]);
            QR(x[0], x[5], x[10], x[15]);
            QR(x[1], x[6], x[11], x[12]);
            QR(x[2], x[7], x[8],  x[13]);
            QR(x[3], x[4], x[9],  x[14]);
        }
        uint8_t block[64];
        for (int i = 0; i < 16; i++) {
            store_le32(block + i * 4, x[i] + state[i]);
        }
        state[12]++;
        size_t chunk = (len - off < 64) ? (len - off) : 64;
        for (size_t i = 0; i < chunk; i++) {
            out[off + i] = in[off + i] ^ block[i];
        }
        off += chunk;
    }
}

static bool crypto_verify_16(const uint8_t a[16], const uint8_t b[16]) {
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) {
        diff |= (a[i] ^ b[i]);
    }
    return diff == 0;
}

/* Seal packet into wire buffer `out_pkt` (returns total wire length: MSTR_HDR_LEN + payload_len) */
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
        chacha20_xor(key, nonce, 1, payload, out_pkt + MSTR_HDR_LEN, payload_len);
    }

    uint8_t full_mac[32];
    hmac_sha256(key, out_pkt, 24, out_pkt + MSTR_HDR_LEN, payload_len, full_mac);
    memcpy(out_pkt + 24, full_mac, 16);

    return (size_t)MSTR_HDR_LEN + payload_len;
}

/* Open and authenticate wire packet `in_pkt` of `in_len` bytes */
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
    hmac_sha256(key, in_pkt, 24, in_pkt + MSTR_HDR_LEN, plen, full_mac);
    if (!crypto_verify_16(in_pkt + 24, full_mac)) {
        return false;
    }

    uint64_t session_id = load_be64(in_pkt + 8);
    uint64_t seq = load_be64(in_pkt + 16);

    if (plen > 0 && out_payload) {
        uint8_t nonce[12];
        store_be32(nonce, (uint32_t)(session_id >> 32) ^ (uint32_t)session_id);
        store_be64(nonce + 4, seq);
        chacha20_xor(key, nonce, 1, in_pkt + MSTR_HDR_LEN, out_payload, plen);
    }

    if (out_type) *out_type = pkt_type;
    if (out_session) *out_session = session_id;
    if (out_seq) *out_seq = seq;
    if (out_payload_len) *out_payload_len = plen;
    return true;
}

/* Anti-replay window state for receiver */
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
    if (diff >= 64) return false; /* Too old */
    uint64_t mask = 1ULL << diff;
    if (rw->window_bitmap & mask) return false; /* Duplicate / replay */
    rw->window_bitmap |= mask;
    return true;
}

/* ============================================================================
 * String, Path, Key & Configuration Utilities
 * ============================================================================ */
static inline int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

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
        int hi = hex_nibble(hex_str[i * 2]);
        int lo = hex_nibble(hex_str[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    char trail = hex_str[KEY_LEN * 2];
    if (trail != '\0' && !isspace((unsigned char)trail) && trail != '#') {
        return false;
    }
    return true;
}

static bool ensure_master_key(void) {
    if (g_key_loaded) return true;
    g_key_count = 0;

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
                    /* Avoid duplicate keys */
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

    if (g_key_count > 0) {
        g_key_loaded = true;
        return true;
    }

    /* Generate new 256-bit key with 0600 permissions */
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

    g_key_loaded = true;
    return true;
}

static void init_default_config(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    snprintf(g_cfg.target_host, sizeof(g_cfg.target_host), "auto");
    snprintf(g_cfg.lan_host, sizeof(g_cfg.lan_host), "192.168.1.12");
    snprintf(g_cfg.wan_host, sizeof(g_cfg.wan_host), "sncr.duckdns.org");
    snprintf(g_cfg.lan_ssid, sizeof(g_cfg.lan_ssid), "ev");
    g_cfg.port = 46002;
    g_cfg.local_audio_port = 46000;
    snprintf(g_cfg.secret_key_file, sizeof(g_cfg.secret_key_file), "%s/.dotfiles/tmp/meet-stream.key", g_home_dir);
    snprintf(g_cfg.ssh_host, sizeof(g_cfg.ssh_host), "ev");
    snprintf(g_cfg.loopback_device, sizeof(g_cfg.loopback_device), "/dev/video10");
    snprintf(g_cfg.video_encoder, sizeof(g_cfg.video_encoder), "auto");
    g_cfg.video_bitrate = 2000;
    g_cfg.audio_bitrate = 64000;
    g_cfg.idle_timeout_sec = 5;

    g_cfg.col_bg          = 0xD90E1016U; /* rgba(14, 16, 22, 0.85) */
    g_cfg.col_card_bg     = 0xF5181A20U; /* rgba(24, 26, 32, 0.96) */
    g_cfg.col_card_border = 0xFF3EAEFFU; /* #3eaeff */
    g_cfg.col_text        = 0xFFFFFFFFU; /* #ffffff */
    g_cfg.col_muted       = 0xFF888888U; /* #888888 */
    g_cfg.col_accent      = 0xFF3EAEFFU; /* #3eaeff */
    g_cfg.col_active      = 0xFF2ECC71U; /* #2ecc71 */
    g_cfg.col_danger      = 0xFFCC6666U; /* #cc6666 */
}

static bool load_config_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *s = line;
        while (*s && isspace((unsigned char)*s)) s++;
        if (*s == '#' || *s == '\0') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = s;
        char *val = eq + 1;
        trim_and_unquote(key);
        trim_and_unquote(val);

        if (strcasecmp(key, "target_host") == 0) snprintf(g_cfg.target_host, sizeof(g_cfg.target_host), "%s", val);
        else if (strcasecmp(key, "lan_host") == 0) snprintf(g_cfg.lan_host, sizeof(g_cfg.lan_host), "%s", val);
        else if (strcasecmp(key, "wan_host") == 0) snprintf(g_cfg.wan_host, sizeof(g_cfg.wan_host), "%s", val);
        else if (strcasecmp(key, "lan_ssid") == 0) snprintf(g_cfg.lan_ssid, sizeof(g_cfg.lan_ssid), "%s", val);
        else if (strcasecmp(key, "port") == 0) g_cfg.port = atoi(val);
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
    fclose(f);
    return true;
}

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

static pid_t spawn_client_video_gst(const char *cam_target, uint16_t local_port) {
    if (!cam_target || !cam_target[0] || strcmp(cam_target, "none") == 0) return 0;
    if (access(cam_target, R_OK) != 0) return 0;

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
    snprintf(br_arg, sizeof(br_arg), "bitrate=%d", g_cfg.video_bitrate > 0 ? g_cfg.video_bitrate : 2000);
    char port_arg[32];
    snprintf(port_arg, sizeof(port_arg), "port=%u", (unsigned)local_port);

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

        const char *argv[36];
        int a = 0;
        argv[a++] = "gst-launch-1.0";
        argv[a++] = "-q";
        argv[a++] = "v4l2src";
        argv[a++] = dev_arg;
        argv[a++] = "!";
        argv[a++] = "videoconvert";
        argv[a++] = "!";
        if (use_vah264) {
            argv[a++] = "vah264enc";
        } else if (use_vaapi) {
            argv[a++] = "vaapih264enc";
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

static pid_t spawn_server_video_gst(uint16_t local_port) {
    if (access(g_cfg.loopback_device, W_OK) != 0) return 0;

    char port_arg[32];
    snprintf(port_arg, sizeof(port_arg), "port=%u", (unsigned)local_port);
    char dev_arg[160];
    snprintf(dev_arg, sizeof(dev_arg), "device=%s", g_cfg.loopback_device);

    pid_t parent_pid = getpid();
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent_pid) _exit(0);
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }

        const char *argv[] = {
            "gst-launch-1.0", "-q",
            "udpsrc", "address=127.0.0.1", port_arg,
            "caps=application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96",
            "!", "rtph264depay",
            "!", "avdec_h264",
            "!", "videoconvert",
            "!", "video/x-raw,format=YUY2",
            "!", "v4l2sink", dev_arg, "sync=false",
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
    if (!resolve_ipv4_sockaddr(target_host, g_cfg.port, &dst_sin)) {
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
    pid_t video_pid = spawn_client_video_gst(cur_cam_target, video_port);

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
                video_pid = spawn_client_video_gst(cur_cam_target, video_port);
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

/* Remote Host On-Demand Receiver (--serve)
 * Activated by systemd user socket (moonlight-cam.socket, fd 3) or binds UDP port.
 * Uses 0 RAM / 0 CPU when nothing is streamed because it exits immediately on
 * authenticated PKT_STOP or after idle_timeout_sec seconds of inactivity.
 */
static int run_server_receiver(void) {
    if (!ensure_master_key()) {
        fprintf(stderr, "meet-stream --serve: failed to load secret key\n");
        return 1;
    }

    int rx_fd = -1;
    const char *listen_fds = getenv("LISTEN_FDS");
    if (listen_fds && atoi(listen_fds) >= 1) {
        rx_fd = 3; /* SD_LISTEN_FDS_START */
        int flags = fcntl(rx_fd, F_GETFL, 0);
        if (flags >= 0) fcntl(rx_fd, F_SETFL, flags | O_NONBLOCK);
    } else {
        rx_fd = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (rx_fd >= 0) {
            int off = 0;
            setsockopt(rx_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
            int reuse = 1;
            setsockopt(rx_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            struct sockaddr_in6 sin6;
            memset(&sin6, 0, sizeof(sin6));
            sin6.sin6_family = AF_INET6;
            sin6.sin6_addr = in6addr_any;
            sin6.sin6_port = htons((uint16_t)g_cfg.port);
            if (bind(rx_fd, (struct sockaddr *)&sin6, sizeof(sin6)) < 0) {
                close(rx_fd);
                rx_fd = -1;
            }
        }
        if (rx_fd < 0) {
            rx_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (rx_fd < 0) return 1;
            int reuse = 1;
            setsockopt(rx_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            struct sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_addr.s_addr = htonl(INADDR_ANY);
            sin.sin_port = htons((uint16_t)g_cfg.port);
            if (bind(rx_fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
                close(rx_fd);
                return 1;
            }
        }
    }

    /* Local loopback socket to forward decrypted Audio & Video RTP packets to 127.0.0.1 */
    int lo_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (lo_fd < 0) return 1;

    struct sockaddr_in audio_dst, video_dst;
    memset(&audio_dst, 0, sizeof(audio_dst));
    audio_dst.sin_family = AF_INET;
    audio_dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    audio_dst.sin_port = htons((uint16_t)(g_cfg.local_audio_port > 0 ? g_cfg.local_audio_port : 46000));

    /* Find an available ephemeral loopback port for the local video GStreamer receiver */
    uint16_t local_video_port = 0;
    int probe_fd = bind_local_ephemeral_udp(&local_video_port);
    if (probe_fd >= 0) close(probe_fd);
    if (local_video_port == 0) local_video_port = 46004;

    memset(&video_dst, 0, sizeof(video_dst));
    video_dst.sin_family = AF_INET;
    video_dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    video_dst.sin_port = htons(local_video_port);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = daemon_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    ReplayWindow rw;
    memset(&rw, 0, sizeof(rw));

    int active_key_idx = -1;
    pid_t video_gst_pid = 0;
    int idle_ms = (g_cfg.idle_timeout_sec > 0 ? g_cfg.idle_timeout_sec : 5) * 1000;
    bool got_valid_stream_pkt = false;

    uint8_t wire_buf[MSTR_HDR_LEN + MSTR_MAX_PAYLOAD];
    uint8_t plain_buf[MSTR_MAX_PAYLOAD];

    struct pollfd pfd = {.fd = rx_fd, .events = POLLIN};

    while (!g_daemon_stop) {
        /* If woken by stray/invalid packet, only wait 1500ms before exiting */
        int wait_ms = got_valid_stream_pkt ? idle_ms : 1500;
        int r = poll(&pfd, 1, wait_ms);
        if (r == 0) {
            /* Idle timeout reached: exit cleanly so 0 RAM / 0 CPU is used */
            break;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }

        bool should_stop = false;
        for (int k = 0; k < 128; k++) {
            ssize_t n = recv(rx_fd, wire_buf, sizeof(wire_buf), MSG_DONTWAIT);
            if (n <= 0) break;

            uint8_t pkt_type = 0;
            uint64_t sess = 0, seq = 0;
            uint16_t plen = 0;
            bool authed = false;

            /* Fast path: check previously authenticated key first */
            if (active_key_idx >= 0 && active_key_idx < g_key_count) {
                if (mstr_open_packet(g_keys[active_key_idx], wire_buf, (size_t)n,
                                      &pkt_type, &sess, &seq, plain_buf, &plen)) {
                    authed = true;
                }
            }
            if (!authed) {
                for (int ki = 0; ki < g_key_count; ki++) {
                    if (ki == active_key_idx) continue;
                    if (mstr_open_packet(g_keys[ki], wire_buf, (size_t)n,
                                          &pkt_type, &sess, &seq, plain_buf, &plen)) {
                        active_key_idx = ki;
                        authed = true;
                        break;
                    }
                }
            }
            if (!authed) {
                continue; /* Drop unauthenticated / corrupted packet */
            }

            uint64_t prev_sess = rw.initialized ? rw.session_id : sess;
            if (!replay_check_and_update(&rw, sess, seq)) {
                continue; /* Drop replayed packet */
            }

            if (pkt_type == PKT_STOP) {
                should_stop = true;
                break;
            }

            got_valid_stream_pkt = true;

            if (sess != prev_sess && video_gst_pid > 0) {
                terminate_child_pid(&video_gst_pid);
            }

            if (pkt_type == PKT_RESET_VIDEO) {
                terminate_child_pid(&video_gst_pid);
            } else if (pkt_type == PKT_AUDIO && plen > 0) {
                sendto(lo_fd, plain_buf, plen, MSG_DONTWAIT,
                       (struct sockaddr *)&audio_dst, sizeof(audio_dst));
            } else if (pkt_type == PKT_VIDEO && plen > 0) {
                if (video_gst_pid > 0) {
                    int st = 0;
                    if (waitpid(video_gst_pid, &st, WNOHANG) == video_gst_pid) {
                        video_gst_pid = 0;
                    }
                }
                if (video_gst_pid == 0) {
                    video_gst_pid = spawn_server_video_gst(local_video_port);
                    usleep(15000); /* Give gst udpsrc 15ms to bind 127.0.0.1 */
                }
                sendto(lo_fd, plain_buf, plen, MSG_DONTWAIT,
                       (struct sockaddr *)&video_dst, sizeof(video_dst));
            }
        }

        if (should_stop) {
            /* Drain any redundant PKT_STOP packets so systemd socket doesn't re-trigger */
            usleep(25000);
            while (recv(rx_fd, wire_buf, sizeof(wire_buf), MSG_DONTWAIT) > 0) {}
            break;
        }
    }

    terminate_child_pid(&video_gst_pid);
    close(lo_fd);
    return 0;
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
static int g_wl_fd = -1;
static uint32_t g_wl_next_id = 2;
static uint32_t g_id_registry = 0;
static uint32_t g_id_compositor = 0;
static uint32_t g_id_shm = 0;
static uint32_t g_id_seat = 0;
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
static void wl_send_u32_3(uint32_t obj, uint16_t op, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t p[3] = {a, b, c};
    wl_send_msg(obj, op, p, 12, -1);
}
static void wl_send_u32_4(uint32_t obj, uint16_t op, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t p[4] = {a, b, c, d};
    wl_send_msg(obj, op, p, 16, -1);
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
    wl_send_msg(g_id_layer_shell, 0, buf, (size_t)idx * 4, -1);
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

    uint32_t pool_id = g_wl_next_id++;
    uint32_t buf_id = g_wl_next_id++;
    uint32_t cp_args[2] = {pool_id, (uint32_t)sz};
    wl_send_msg(g_id_shm, 0, cp_args, 8, mfd);
    close(mfd);

    uint32_t cb_args[6] = {buf_id, 0, (uint32_t)g_surf_w, (uint32_t)g_surf_h, (uint32_t)(g_surf_w * 4), 0};
    wl_send_msg(pool_id, 0, cb_args, 24, -1);

    wl_send_u32_3(g_id_surface, 1 /* attach */, buf_id, 0, 0);
    wl_send_u32_4(g_id_surface, 2 /* damage */, 0, 0, (uint32_t)g_surf_w, (uint32_t)g_surf_h);
    wl_send_msg(g_id_surface, 6 /* commit */, NULL, 0, -1);

    wl_send_msg(buf_id, 0 /* destroy buffer */, NULL, 0, -1);
    wl_send_msg(pool_id, 1 /* destroy pool */, NULL, 0, -1);
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

static void process_wayland_events(void) {
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

        ssize_t n = recvmsg(g_wl_fd, &msg, MSG_DONTWAIT);
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

        if (obj_id == g_id_registry && opcode == 0 /* global */) {
            uint32_t name = *(const uint32_t *)p;
            uint32_t slen = *(const uint32_t *)(p + 4);
            const char *iface = (const char *)(p + 8);
            uint32_t ver_off = 8 + ((slen + 3) & ~3);
            uint32_t ver = *(const uint32_t *)(p + ver_off);

            if (strcmp(iface, "wl_compositor") == 0) {
                g_id_compositor = g_wl_next_id++;
                wl_registry_bind(name, iface, ver < 4 ? ver : 4, g_id_compositor);
            } else if (strcmp(iface, "wl_shm") == 0) {
                g_id_shm = g_wl_next_id++;
                wl_registry_bind(name, iface, 1, g_id_shm);
            } else if (strcmp(iface, "wl_seat") == 0) {
                g_id_seat = g_wl_next_id++;
                wl_registry_bind(name, iface, ver < 5 ? ver : 5, g_id_seat);
            } else if (strcmp(iface, "zwlr_layer_shell_v1") == 0) {
                g_id_layer_shell = g_wl_next_id++;
                wl_registry_bind(name, iface, ver < 4 ? ver : 4, g_id_layer_shell);
            }
        } else if (obj_id == g_id_seat && opcode == 0 /* capabilities */) {
            uint32_t caps = *(const uint32_t *)p;
            if ((caps & 1 /* POINTER */) && !g_id_pointer) {
                g_id_pointer = g_wl_next_id++;
                wl_send_u32_1(g_id_seat, 0 /* get_pointer */, g_id_pointer);
            }
            if ((caps & 2 /* KEYBOARD */) && !g_id_keyboard) {
                g_id_keyboard = g_wl_next_id++;
                wl_send_u32_1(g_id_seat, 1 /* get_keyboard */, g_id_keyboard);
            }
        } else if (obj_id == g_id_layer) {
            if (opcode == 0 /* configure */) {
                uint32_t serial = *(const uint32_t *)p;
                uint32_t w = *(const uint32_t *)(p + 4);
                uint32_t h = *(const uint32_t *)(p + 8);
                if (w > 0) g_surf_w = (int)w;
                if (h > 0) g_surf_h = (int)h;
                wl_send_u32_1(g_id_layer, 6 /* ack_configure */, serial);
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
        g_wl_fd = atoi(w_sock);
        fcntl(g_wl_fd, F_SETFD, FD_CLOEXEC);
        unsetenv("WAYLAND_SOCKET");
    } else {
        const char *wd = getenv("WAYLAND_DISPLAY");
        char wl_path[512];
        if (wd && *wd) snprintf(wl_path, sizeof(wl_path), "%s/%s", g_runtime_dir, wd);
        else snprintf(wl_path, sizeof(wl_path), "%s/wayland-1", g_runtime_dir);

        g_wl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_un waddr;
        memset(&waddr, 0, sizeof(waddr));
        waddr.sun_family = AF_UNIX;
        snprintf(waddr.sun_path, sizeof(waddr.sun_path), "%s", wl_path);
        if (connect(g_wl_fd, (struct sockaddr *)&waddr, sizeof(waddr)) < 0) {
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

    g_id_registry = g_wl_next_id++;
    wl_send_u32_1(1 /* wl_display */, 1 /* get_registry */, g_id_registry);
    uint32_t sync_cb = g_wl_next_id++;
    wl_send_u32_1(1 /* wl_display */, 0 /* sync */, sync_cb);

    struct pollfd pfd = {.fd = g_wl_fd, .events = POLLIN};
    for (int i = 0; i < 15 && (!g_id_compositor || !g_id_shm || !g_id_layer_shell); i++) {
        if (poll(&pfd, 1, 100) > 0) process_wayland_events();
    }

    if (!g_id_compositor || !g_id_shm || !g_id_layer_shell) {
        fprintf(stderr, "meet-stream: compositor missing required Wayland globals\n");
        close(g_wl_fd);
        if (lock_fd >= 0) { close(lock_fd); unlink(lock_path); }
        return 1;
    }

    g_id_surface = g_wl_next_id++;
    wl_send_u32_1(g_id_compositor, 0 /* create_surface */, g_id_surface);

    g_id_layer = g_wl_next_id++;
    wl_get_layer_surface(g_id_layer, g_id_surface, 0, 3 /* OVERLAY */, "meet-stream");
    wl_send_u32_2(g_id_layer, 0 /* set_size */, 0, 0);
    wl_send_u32_1(g_id_layer, 1 /* set_anchor */, 1 | 2 | 4 | 8 /* TOP|BOTTOM|LEFT|RIGHT */);
    wl_send_u32_1(g_id_layer, 2 /* set_exclusive_zone */, (uint32_t)-1);
    wl_send_u32_1(g_id_layer, 4 /* set_keyboard_interactivity */, 1 /* EXCLUSIVE */);
    wl_send_msg(g_id_surface, 6 /* commit */, NULL, 0, -1);

    while (g_ui_running) {
        int r = poll(&pfd, 1, 1000);
        if (r > 0) {
            if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
            if (pfd.revents & POLLIN) process_wayland_events();
        }
    }

    close(g_wl_fd);
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        unlink(lock_path);
    }
    return 0;
}

/* ============================================================================
 * Self-Test & Render-Test Modes
 * ============================================================================ */
static int run_render_test(const char *ppm_path) {
    memset(&g_ui, 0, sizeof(g_ui));
    g_ui.mode = UI_MODE_ALL;
    init_fonts();
    ui_refresh_devices();

    int w = 1920, h = 1080;
    uint32_t *px = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!px) return 1;

    SurfaceBuf sb = {.px = px, .w = w, .h = h};
    ui_render_surface(&sb);

    FILE *fp = fopen(ppm_path, "wb");
    if (!fp) {
        free(px);
        return 1;
    }
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint8_t rgb[3] = {
            (uint8_t)((px[i] >> 16) & 0xFF),
            (uint8_t)((px[i] >> 8) & 0xFF),
            (uint8_t)(px[i] & 0xFF)
        };
        fwrite(rgb, 1, 3, fp);
    }
    fclose(fp);
    free(px);
    return 0;
}

static int run_self_test(void) {
    /* 1. Verify ChaCha20 + HMAC-SHA256-128 seal/open & anti-replay */
    uint8_t key[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 7 + 3);
    const char *sample = "Opus/H264 RTP test payload for meet-stream";
    uint8_t wire[256], opened[256];
    size_t wlen = mstr_seal_packet(key, PKT_AUDIO, 0x1122334455667788ULL, 42,
                                   (const uint8_t *)sample, (uint16_t)strlen(sample), wire);
    uint8_t otype = 0;
    uint64_t osess = 0, oseq = 0;
    uint16_t olen = 0;
    if (!mstr_open_packet(key, wire, wlen, &otype, &osess, &oseq, opened, &olen) ||
        otype != PKT_AUDIO || osess != 0x1122334455667788ULL || oseq != 42 ||
        olen != strlen(sample) || memcmp(opened, sample, olen) != 0) {
        fprintf(stderr, "Self-test FAILED: crypto seal/open mismatch\n");
        return 1;
    }

    /* Tamper with ciphertext -> must reject */
    wire[MSTR_HDR_LEN] ^= 0x01;
    if (mstr_open_packet(key, wire, wlen, &otype, &osess, &oseq, opened, &olen)) {
        fprintf(stderr, "Self-test FAILED: tampered ciphertext accepted\n");
        return 1;
    }

    /* Verify anti-replay window */
    ReplayWindow rw;
    memset(&rw, 0, sizeof(rw));
    if (!replay_check_and_update(&rw, 100, 1) ||
        replay_check_and_update(&rw, 100, 1) ||
        !replay_check_and_update(&rw, 100, 2)) {
        fprintf(stderr, "Self-test FAILED: replay window check failed\n");
        return 1;
    }

    /* Test multi-key authentication */
    uint8_t key2[32];
    for (int i = 0; i < 32; i++) key2[i] = (uint8_t)(i * 13 + 7);
    uint8_t wire2[256], opened2[256];
    size_t wlen2 = mstr_seal_packet(key2, PKT_AUDIO, 0x1122334455667788ULL, 1,
                                    (const uint8_t *)sample, (uint16_t)strlen(sample), wire2);
    /* key1 cannot open wire2 */
    if (mstr_open_packet(key, wire2, wlen2, &otype, &osess, &oseq, opened2, &olen)) {
        fprintf(stderr, "Self-test FAILED: wrong key opened packet\n");
        return 1;
    }
    /* key2 can open wire2 */
    if (!mstr_open_packet(key2, wire2, wlen2, &otype, &osess, &oseq, opened2, &olen) ||
        memcmp(opened2, sample, olen) != 0) {
        fprintf(stderr, "Self-test FAILED: key2 open failed\n");
        return 1;
    }

    /* Test key parsing with comments, whitespace, and reject invalid */
    char test_hex_line[128];
    snprintf(test_hex_line, sizeof(test_hex_line),
             "  0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  # laptop device \n");
    uint8_t parsed_k[32];
    if (!parse_hex_key(test_hex_line, parsed_k)) {
        fprintf(stderr, "Self-test FAILED: parse_hex_key failed on line with comments\n");
        return 1;
    }
    if (parse_hex_key("0123456789abcdef", parsed_k)) {
        fprintf(stderr, "Self-test FAILED: parse_hex_key accepted short hex key\n");
        return 1;
    }

    /* Test multi-key file loading */
    char tmp_kf[] = "/tmp/meet_stream_test_XXXXXX";
    int tmp_fd = mkstemp(tmp_kf);
    if (tmp_fd >= 0) {
        const char *test_file_content =
            "# First key from laptop\n"
            "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f # laptop\n"
            "\n"
            "# Duplicate should be ignored\n"
            "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n"
            "# Second key from ofis\n"
            "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f # ofis\n";
        ssize_t tw = write(tmp_fd, test_file_content, strlen(test_file_content));
        (void)tw;
        close(tmp_fd);

        StreamConfig orig_cfg = g_cfg;
        bool orig_loaded = g_key_loaded;
        int orig_count = g_key_count;

        g_key_loaded = false;
        g_cfg.secret_key_hex[0] = '\0';
        snprintf(g_cfg.secret_key_file, sizeof(g_cfg.secret_key_file), "%s", tmp_kf);

        if (!ensure_master_key() || g_key_count != 2) {
            fprintf(stderr, "Self-test FAILED: ensure_master_key did not load exactly 2 keys (got %d)\n", g_key_count);
            unlink(tmp_kf);
            return 1;
        }
        unlink(tmp_kf);

        g_cfg = orig_cfg;
        g_key_loaded = orig_loaded;
        g_key_count = orig_count;
    }

    /* 2. Verify Docked vs. Undocked MRU fallback without clobbering preference */
    memset(&g_pref, 0, sizeof(g_pref));
    mru_promote(g_pref.cam_prefs, &g_pref.cam_pref_count, "/dev/v4l/by-id/usb-Internal_Cam-video-index0");
    mru_promote(g_pref.cam_prefs, &g_pref.cam_pref_count, "/dev/v4l/by-id/usb-Docked_4K_Webcam-video-index0");

    DeviceItem mock_undocked[2];
    memset(mock_undocked, 0, sizeof(mock_undocked));
    snprintf(mock_undocked[0].label, sizeof(mock_undocked[0].label), "Internal Laptop Cam (/dev/video0)");
    snprintf(mock_undocked[0].target, sizeof(mock_undocked[0].target), "/dev/v4l/by-id/usb-Internal_Cam-video-index0");
    snprintf(mock_undocked[0].devnode, sizeof(mock_undocked[0].devnode), "/dev/video0");
    mock_undocked[1].is_disabled_entry = true;
    snprintf(mock_undocked[1].target, sizeof(mock_undocked[1].target), "none");

    bool fb = false;
    int chosen_undocked = resolve_active_camera(mock_undocked, 2, &fb);
    if (chosen_undocked != 0 || !fb ||
        strcmp(g_pref.cam_prefs[0], "/dev/v4l/by-id/usb-Docked_4K_Webcam-video-index0") != 0) {
        fprintf(stderr, "Self-test FAILED: undocked fallback did not preserve docked #1 preference\n");
        return 1;
    }

    /* Now simulate re-docking: Docked 4K webcam is connected again */
    DeviceItem mock_docked[3];
    memset(mock_docked, 0, sizeof(mock_docked));
    mock_docked[0] = mock_undocked[0];
    snprintf(mock_docked[1].label, sizeof(mock_docked[1].label), "Docked 4K Webcam (/dev/video2)");
    snprintf(mock_docked[1].target, sizeof(mock_docked[1].target), "/dev/v4l/by-id/usb-Docked_4K_Webcam-video-index0");
    snprintf(mock_docked[1].devnode, sizeof(mock_docked[1].devnode), "/dev/video2");
    mock_docked[2] = mock_undocked[1];

    fb = true;
    int chosen_docked = resolve_active_camera(mock_docked, 3, &fb);
    if (chosen_docked != 1 || fb) {
        fprintf(stderr, "Self-test FAILED: re-docked resolution did not pick primary docked preference\n");
        return 1;
    }

    /* 3. Verify offscreen Wayland UI rendering */
    init_fonts();
    memset(&g_ui, 0, sizeof(g_ui));
    g_ui.mode = UI_MODE_ALL;
    ui_refresh_devices();
    uint32_t *px = calloc(1920 * 1080, sizeof(uint32_t));
    if (!px) return 1;
    SurfaceBuf sb = {.px = px, .w = 1920, .h = 1080};
    ui_render_surface(&sb);
    free(px);

    printf("meet-stream self-test OK: crypto=PASS | replay_window=PASS | docked_undocked_fallback=PASS | ui_render=PASS\n");
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

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n\n"
           "Secure, configurable webcam & microphone streaming client, on-demand remote\n"
           "receiver, and custom Wayland Layer-Shell device selector.\n\n"
           "Options:\n"
           "  (no args), --toggle      Toggle webcam & microphone streaming ($mod+s)\n"
           "  --start                  Start streaming if not already active\n"
           "  --stop                   Stop active streaming\n"
           "  --select                 Open Wayland Layer-Shell device selector ($mod+Shift+s)\n"
           "  --select-cam             Open Wayland Layer-Shell webcam selector\n"
           "  --select-mic             Open Wayland Layer-Shell microphone selector\n"
           "  --sync-key [HOST]        Authorize secret key on remote host via SSH (defaults to ssh_host)\n"
           "  --serve                  Run on-demand remote receiver (systemd socket activated)\n"
           "  --status                 Print current streaming status and selected devices\n"
           "  --config <PATH>          Load configuration from custom file\n"
           "  --render-test <OUT.PPM>  Render Wayland selector overlay to PPM image\n"
           "  --self-test              Run cryptographic, fallback, and UI self-tests\n"
           "  -h, --help               Show this help message\n",
           prog);
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);

    const char *home = getenv("HOME");
    snprintf(g_home_dir, sizeof(g_home_dir), "%s", (home && *home) ? home : "/tmp");

    const char *rdir = getenv("XDG_RUNTIME_DIR");
    if (rdir && *rdir) snprintf(g_runtime_dir, sizeof(g_runtime_dir), "%s", rdir);
    else snprintf(g_runtime_dir, sizeof(g_runtime_dir), "/run/user/%u", (unsigned)getuid());

    const char *cdir = getenv("XDG_CONFIG_HOME");
    if (cdir && *cdir) snprintf(g_config_dir, sizeof(g_config_dir), "%s", cdir);
    else snprintf(g_config_dir, sizeof(g_config_dir), "%s/.config", g_home_dir);

    snprintf(g_state_dir, sizeof(g_state_dir), "%s/meet-stream", g_runtime_dir);
    snprintf(g_pid_file, sizeof(g_pid_file), "%s/daemon.pid", g_state_dir);
    snprintf(g_status_file, sizeof(g_status_file), "%s/status", g_state_dir);
    snprintf(g_pref_file, sizeof(g_pref_file), "%s/meet-stream-state", g_config_dir);
    snprintf(g_legacy_cam_file, sizeof(g_legacy_cam_file), "%s/meet-stream-cam", g_config_dir);
    snprintf(g_legacy_mic_file, sizeof(g_legacy_mic_file), "%s/meet-stream-mic", g_config_dir);

    init_default_config();

    char cpath1[512], cpath2[512], cpath3[512];
    snprintf(cpath1, sizeof(cpath1), "%s/meet-stream.conf", g_config_dir);
    snprintf(cpath2, sizeof(cpath2), "%s/sway/meet-stream.conf", g_config_dir);
    snprintf(cpath3, sizeof(cpath3), "%s/.dotfiles/meet-stream.conf", g_home_dir);
    if (!load_config_file(cpath1)) {
        if (!load_config_file(cpath2)) {
            load_config_file(cpath3);
        }
    }

    enum {
        ACT_TOGGLE = 0,
        ACT_START,
        ACT_STOP,
        ACT_SELECT_ALL,
        ACT_SELECT_CAM,
        ACT_SELECT_MIC,
        ACT_SERVE,
        ACT_STATUS,
        ACT_SYNC_KEY,
        ACT_SELF_TEST,
        ACT_RENDER_TEST
    } action = ACT_TOGGLE;

    const char *render_out = NULL;
    const char *sync_target_host = NULL;
    bool force_headless = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            char exp[512];
            expand_tilde_path(argv[++i], exp, sizeof(exp));
            load_config_file(exp);
        } else if (strcmp(argv[i], "--toggle") == 0 || strcmp(argv[i], "toggle") == 0) {
            action = ACT_TOGGLE;
        } else if (strcmp(argv[i], "--start") == 0 || strcmp(argv[i], "start") == 0) {
            action = ACT_START;
        } else if (strcmp(argv[i], "--stop") == 0 || strcmp(argv[i], "stop") == 0) {
            action = ACT_STOP;
        } else if (strcmp(argv[i], "--select") == 0 || strcmp(argv[i], "select") == 0) {
            action = ACT_SELECT_ALL;
        } else if (strcmp(argv[i], "--select-cam") == 0 || strcmp(argv[i], "select-cam") == 0) {
            action = ACT_SELECT_CAM;
        } else if (strcmp(argv[i], "--select-mic") == 0 || strcmp(argv[i], "select-mic") == 0) {
            action = ACT_SELECT_MIC;
        } else if (strcmp(argv[i], "--sync-key") == 0 || strcmp(argv[i], "sync-key") == 0) {
            action = ACT_SYNC_KEY;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                sync_target_host = argv[++i];
            }
        } else if (strcmp(argv[i], "--serve") == 0 || strcmp(argv[i], "serve") == 0) {
            action = ACT_SERVE;
        } else if (strcmp(argv[i], "--status") == 0 || strcmp(argv[i], "status") == 0) {
            action = ACT_STATUS;
        } else if (strcmp(argv[i], "--self-test") == 0) {
            action = ACT_SELF_TEST;
        } else if (strcmp(argv[i], "--render-test") == 0 && i + 1 < argc) {
            action = ACT_RENDER_TEST;
            render_out = argv[++i];
        } else if (strcmp(argv[i], "--force") == 0) {
            force_headless = true;
        }
    }

    if (action == ACT_SELF_TEST) return run_self_test();
    if (action == ACT_RENDER_TEST) return run_render_test(render_out);
    if (action == ACT_SERVE) return run_server_receiver();
    if (action == ACT_SYNC_KEY) return run_sync_key(sync_target_host);

    if (action == ACT_STATUS) {
        load_persistent_state();
        DeviceItem cams[MAX_DEVICES], mics[MAX_DEVICES];
        int cc = discover_cameras(cams);
        int mc = discover_microphones(mics);
        bool cfb = false, mfb = false;
        int ci = resolve_active_camera(cams, cc, &cfb);
        int mi = resolve_active_microphone(mics, mc, &mfb);
        pid_t dpid = get_running_daemon_pid();
        char thost[128];
        resolve_target_host(thost, sizeof(thost));
        printf("State:      %s (PID %d)\n", dpid > 0 ? "STREAMING" : "IDLE", (int)dpid);
        printf("Target:     %s:%d\n", thost, g_cfg.port);
        printf("Webcam:     %s [%s]%s\n", cams[ci].label, cams[ci].target, cfb ? " (auto-fallback)" : "");
        printf("Microphone: %s [%s]%s\n", mics[mi].label, mics[mi].target, mfb ? " (auto-fallback)" : "");
        return 0;
    }

    /* Guard: do not run client actions inside headless Sunshine Sway session */
    const char *headless = getenv("SWAY_HEADLESS");
    if (!force_headless && headless && *headless) {
        return 0;
    }

    if (action == ACT_SELECT_ALL) return run_wayland_selector(UI_MODE_ALL);
    if (action == ACT_SELECT_CAM) return run_wayland_selector(UI_MODE_CAM);
    if (action == ACT_SELECT_MIC) return run_wayland_selector(UI_MODE_MIC);
    if (action == ACT_STOP) {
        stop_client_stream(true);
        return 0;
    }
    if (action == ACT_START) {
        if (get_running_daemon_pid() > 0) return 0;
        return run_client_daemon(false);
    }

    /* Default: ACT_TOGGLE ($mod+s) */
    if (get_running_daemon_pid() > 0) {
        stop_client_stream(true);
        return 0;
    }
    return run_client_daemon(false);
}
