#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/netlink.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include <json-c/json.h>

/* Stub dlopen symbols so static libfreetype never pulls in glibc dynamic loader warnings */
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
 * Constants & Theme Visuals (matching ~/.config/waybar/style.css & config.jsonc)
 * ============================================================================ */
#define BAR_HEIGHT 26
#define COL_BAR_BG 0xCC4A3E35U       /* rgba(74, 62, 53, 0.8) */
#define COL_BAR_BORDER 0xFF7D6B5EU   /* #7d6b5e */
#define COL_TEXT 0xFFF5E4C6U         /* #f5e4c6 */
#define COL_WS_IDLE 0xFFB2A090U      /* #b2a090 */
#define COL_WS_FOC_BG 0xFF6B5A4DU    /* #6b5a4d */
#define COL_WS_FOC_BDR 0xFFD9A066U   /* #d9a066 */
#define COL_URGENT 0xFFCC6666U       /* #cc6666 */
#define COL_WARNING 0xFFE5A454U      /* #e5a454 */
#define COL_TIP_BG 0xFF4A3E35U       /* #4a3e35 */
#define COL_TIP_BDR 0xFF7D6B5EU      /* #7d6b5e */
#define COL_CLD_MONTH 0xFFFFEAD3U    /* #ffead3 */
#define COL_CLD_WKDAY 0xFF7D6B5EU    /* #7d6b5e */
#define COL_CLD_TODAY_FG 0xFF4A3E35U /* #4a3e35 */

#define BTN_LEFT_CODE 0x110
#define BTN_RIGHT_CODE 0x111
#define BTN_MIDDLE_CODE 0x112

/* ============================================================================
 * Configuration State
 * ============================================================================ */
#define MAX_TZS 16
typedef struct {
    char tz[64];
    char label[64]; /* Emoji / label per timezone (can be empty) */
} TzEntry;

static TzEntry g_tzs[MAX_TZS];
static int g_tz_count = 0;
static int g_tz_idx = 0;
static bool g_clock_alt = false; /* Toggled by right-click (format-alt-click: 3) */
static char g_gcal_token_arg[512] = "";
static char g_config_path[512] = "";
static char g_runtime_dir[256] = "";
static char g_home_dir[256] = "";

/* ============================================================================
 * Module Data Structures
 * ============================================================================ */
typedef struct {
    char name[32];
    bool focused;
    bool urgent;
    int x1, x2;
} WorkspaceItem;

typedef struct {
    char summary[128];
    char location[128];
    char start_time[32];
    char end_time[32];
    char color[16];
    int minutes_until;
} GCalEvent;

typedef struct {
    char service[128];
    char path[128];
    char title[128];
    uint32_t pixels[16 * 16];
    bool has_pixels;
    int x1, x2;
} TrayItem;

typedef enum {
    MOD_WORKSPACES = 0,
    MOD_MODE,
    MOD_SCRATCHPAD,
    MOD_CLOCK,
    MOD_QUIET,
    MOD_GCAL,
    MOD_MPRIS,
    MOD_PULSE,
    MOD_NAMAZ,
    MOD_BATTERY,
    MOD_SCREENSAVER,
    MOD_TRAY,
    MOD_COUNT
} ModuleId;

typedef struct {
    int x1, x2;
    bool visible;
    char tooltip[8192];
} ModuleBox;

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_wakeup_pipe[2] = {-1, -1};

/* Workspaces, Mode & Scratchpad */
static WorkspaceItem g_workspaces[32];
static int g_ws_count = 0;
static char g_sway_mode[64] = "";
static int g_scratch_count = 0;
static char g_scratch_tooltip[2048] = "";
static char g_sway_bar_id[64] = "";
static char g_sway_bar_mode[32] = "dock";
static char g_sway_bar_hidden_state[32] = "hide";
static bool g_sway_visible_by_modifier = false;
static bool g_manual_hidden = false;
static bool g_bar_visible = false;
static volatile sig_atomic_t g_toggle_requested = 0;

/* Quiet Notification Daemon */
static uint32_t g_quiet_count = 0;
static uint32_t g_quiet_urgency = 0;
static char g_quiet_preview[256] = "";
static time_t g_quiet_preview_until = 0;

/* Google Calendar */
static GCalEvent g_gcal_events[16];
static int g_gcal_count = 0;
static bool g_gcal_show_details = false;
static bool g_gcal_fetching = false;
static char g_gcal_access_token[1024] = "";
static char g_gcal_client_id[256] = "";
static char g_gcal_client_secret[256] = "";
static char g_gcal_refresh_token[512] = "";
static char g_gcal_token_file[512] = "";
static time_t g_gcal_token_expiry = 0;

/* MPRIS */
static int g_mpris_state = 0; /* 0=hidden, 1=Playing, 2=Paused */
static char g_mpris_tooltip[256] = "";

/* PulseAudio / PipeWire */
static int g_pulse_vol = 40;
static bool g_pulse_muted = false;

/* Namaz (Offline Astronomical Moonsighting Committee) */
static char g_namaz_city[64] = "Jersey City";
static double g_namaz_lat = 40.7137;
static double g_namaz_lon = -74.0399;
static char g_namaz_today[6][16]; /* Fajr, Sunrise, Dhuhr, Asr, Maghrib, Isha */
static char g_namaz_tom_fajr[16] = "";
static bool g_namaz_loaded = false;
static bool g_namaz_show_full = false;

/* Battery */
static bool g_bat_present = false;
static int g_bat_capacity = 100;
static char g_bat_status[32] = "Full";
static char g_bat_time_str[32] = "";
static bool g_bat_notified_crit = false;
static bool g_bat_blink_on = false;

/* Screensaver */
static bool g_screensaver_active = true;

/* Tray */
static TrayItem g_tray_items[16];
static int g_tray_count = 0;

/* Hit boxes */
static ModuleBox g_mod_boxes[MOD_COUNT];
static int g_hovered_mod = -1;
static int g_hovered_tray_idx = -1;
static int g_pointer_x = -1;
static int g_pointer_y = -1;
static uint32_t g_last_click_ms = 0;
static int g_last_click_mod = -1;

static void notify_main_loop(void) {
    if (g_wakeup_pipe[1] >= 0) {
        char c = 1;
        ssize_t r = write(g_wakeup_pipe[1], &c, 1);
        (void)r;
    }
}

static void spawn_detached(const char *cmd) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        pid_t pid2 = fork();
        if (pid2 < 0) _exit(127);
        if (pid2 > 0) _exit(0); /* Intermediate child exits immediately */
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
}

static char *run_cmd_capture(const char *cmd, size_t max_len) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    char *buf = calloc(1, max_len + 1);
    if (!buf) {
        pclose(fp);
        return NULL;
    }
    size_t total = 0;
    while (total < max_len) {
        size_t n = fread(buf + total, 1, max_len - total, fp);
        if (n == 0) break;
        total += n;
    }
    buf[total] = '\0';
    pclose(fp);
    return buf;
}

/* ============================================================================
 * FreeType Font + OpenType GSUB Emoji Ligature + Pango Markup Renderer
 * ============================================================================ */
static FT_Library g_ft;
static FT_Face g_face_reg;
static FT_Face g_face_bold;
static FT_Face g_face_sym;
static FT_Face g_face_symbola;
static FT_Face g_face_emoji;
static uint8_t *g_emoji_gsub = NULL;
static size_t g_emoji_gsub_len = 0;

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static FT_UInt emoji_gsub_ligature(FT_UInt g1, FT_UInt g2) {
    if (!g_emoji_gsub || g_emoji_gsub_len < 12) return 0;
    uint16_t lookup_list_off = be16(g_emoji_gsub + 8);
    if ((size_t)lookup_list_off + 2 > g_emoji_gsub_len) return 0;
    uint16_t lookup_cnt = be16(g_emoji_gsub + lookup_list_off);
    for (uint16_t i = 0; i < lookup_cnt; i++) {
        uint32_t loff = lookup_list_off + be16(g_emoji_gsub + lookup_list_off + 2 + 2 * i);
        if (loff + 6 > g_emoji_gsub_len) continue;
        uint16_t ltype = be16(g_emoji_gsub + loff);
        uint16_t sub_cnt = be16(g_emoji_gsub + loff + 4);
        if (ltype != 4) continue;
        for (uint16_t s = 0; s < sub_cnt; s++) {
            uint32_t sub_off = loff + be16(g_emoji_gsub + loff + 6 + 2 * s);
            if (sub_off + 6 > g_emoji_gsub_len) continue;
            if (be16(g_emoji_gsub + sub_off) != 1) continue;
            uint32_t cov_off = sub_off + be16(g_emoji_gsub + sub_off + 2);
            if (cov_off + 4 > g_emoji_gsub_len) continue;
            uint16_t cov_fmt = be16(g_emoji_gsub + cov_off);
            int cov_idx = -1;
            if (cov_fmt == 1) {
                uint16_t cnt = be16(g_emoji_gsub + cov_off + 2);
                for (uint16_t k = 0; k < cnt; k++) {
                    if (cov_off + 6 + 2 * k <= g_emoji_gsub_len &&
                        be16(g_emoji_gsub + cov_off + 4 + 2 * k) == g1) {
                        cov_idx = k;
                        break;
                    }
                }
            }
            if (cov_idx >= 0) {
                uint32_t lset_off = sub_off + be16(g_emoji_gsub + sub_off + 6 + 2 * cov_idx);
                if (lset_off + 2 > g_emoji_gsub_len) continue;
                uint16_t lcnt = be16(g_emoji_gsub + lset_off);
                for (uint16_t j = 0; j < lcnt; j++) {
                    uint32_t lig_off = lset_off + be16(g_emoji_gsub + lset_off + 2 + 2 * j);
                    if (lig_off + 6 > g_emoji_gsub_len) continue;
                    uint16_t lig_glyph = be16(g_emoji_gsub + lig_off);
                    uint16_t comp_cnt = be16(g_emoji_gsub + lig_off + 2);
                    if (comp_cnt == 2 && be16(g_emoji_gsub + lig_off + 4) == g2) {
                        return lig_glyph;
                    }
                }
            }
        }
    }
    return 0;
}

static void init_fonts(void) {
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
    FT_New_Face(g_ft, "/usr/share/fonts/truetype/ancient-scripts/Symbola_hint.ttf", 0, &g_face_symbola);
    if (FT_New_Face(g_ft, "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf", 0, &g_face_emoji) == 0) {
        if (g_face_emoji->num_fixed_sizes > 0) {
            FT_Select_Size(g_face_emoji, 0);
        }
        FT_ULong len = 0;
        if (FT_Load_Sfnt_Table(g_face_emoji, FT_MAKE_TAG('G', 'S', 'U', 'B'), 0, NULL, &len) == 0 && len > 0) {
            g_emoji_gsub = malloc(len);
            if (g_emoji_gsub) {
                FT_Load_Sfnt_Table(g_face_emoji, FT_MAKE_TAG('G', 'S', 'U', 'B'), 0, g_emoji_gsub, &len);
                g_emoji_gsub_len = len;
            }
        }
    }
}

typedef struct {
    uint32_t *px;
    int w, h;
} SurfaceBuf;

static inline uint32_t premul_argb(uint32_t argb) {
    uint32_t a = (argb >> 24) & 0xFF;
    if (a == 255) return argb;
    if (a == 0) return 0;
    uint32_t r = (((argb >> 16) & 0xFF) * a + 127) / 255;
    uint32_t g = (((argb >> 8) & 0xFF) * a + 127) / 255;
    uint32_t b = ((argb & 0xFF) * a + 127) / 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static inline void blend_pixel(SurfaceBuf *s, int x, int y, uint32_t src_argb) {
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) return;
    uint32_t sa = (src_argb >> 24) & 0xFF;
    if (sa == 0) return;
    if (sa == 255) {
        s->px[y * s->w + x] = src_argb;
        return;
    }
    uint32_t dst = s->px[y * s->w + x];
    uint32_t da = (dst >> 24) & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >> 8) & 0xFF;
    uint32_t db = dst & 0xFF;

    uint32_t sr = (((src_argb >> 16) & 0xFF) * sa + 127) / 255;
    uint32_t sg = (((src_argb >> 8) & 0xFF) * sa + 127) / 255;
    uint32_t sb = ((src_argb & 0xFF) * sa + 127) / 255;

    uint32_t inv = 255 - sa;
    uint32_t oa = sa + (da * inv + 127) / 255;
    uint32_t or_ = sr + (dr * inv + 127) / 255;
    uint32_t og = sg + (dg * inv + 127) / 255;
    uint32_t ob = sb + (db * inv + 127) / 255;
    s->px[y * s->w + x] = (oa << 24) | (or_ << 16) | (og << 8) | ob;
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

static void draw_rounded_box(SurfaceBuf *s, int x, int y, int w, int h, int r, uint32_t bg_argb, uint32_t bdr_argb) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int dx = 0, dy = 0;
            if (i < r && j < r) { dx = r - i; dy = r - j; }
            else if (i >= w - r && j < r) { dx = i - (w - r - 1); dy = r - j; }
            else if (i < r && j >= h - r) { dx = r - i; dy = j - (h - r - 1); }
            else if (i >= w - r && j >= h - r) { dx = i - (w - r - 1); dy = j - (h - r - 1); }

            int dist2 = dx * dx + dy * dy;
            if (dist2 > r * r + 1) continue;

            bool is_border = (i == 0 || i == w - 1 || j == 0 || j == h - 1 ||
                              (dx > 0 && dy > 0 && dist2 >= (r - 1) * (r - 1)));
            uint32_t col = (is_border && bdr_argb) ? bdr_argb : bg_argb;
            blend_pixel(s, x + i, y + j, col);
        }
    }
}

static uint32_t utf8_next(const char **p) {
    const uint8_t *s = (const uint8_t *)*p;
    if (!*s) return 0;
    uint32_t cp = 0;
    if (s[0] < 0x80) {
        cp = s[0];
        *p += 1;
    } else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *p += 2;
    } else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *p += 3;
    } else if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        *p += 4;
    } else {
        cp = s[0];
        *p += 1;
    }
    return cp;
}

static uint32_t parse_hex_color(const char *hex, uint32_t fallback) {
    if (!hex) return fallback;
    if (*hex == '#') hex++;
    if (strlen(hex) >= 6) {
        char buf[7];
        memcpy(buf, hex, 6);
        buf[6] = '\0';
        uint32_t rgb = (uint32_t)strtoul(buf, NULL, 16);
        return 0xFF000000U | rgb;
    }
    return fallback;
}

static bool is_color_emoji_cp(uint32_t cp) {
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return true;
    if (cp == 0x26A1) return true; /* ⚡ */
    return false;
}

/* Render or measure a single line of markup text */
static int render_markup_line(SurfaceBuf *s, const char *text, int start_x, int baseline_y,
                              int px_size, uint32_t default_fg, bool force_bold, bool draw) {
    FT_Set_Pixel_Sizes(g_face_reg, 0, px_size);
    FT_Set_Pixel_Sizes(g_face_bold, 0, px_size);
    FT_Set_Pixel_Sizes(g_face_sym, 0, px_size == 16 ? 17 : px_size);

    uint32_t fg_stack[8];
    uint32_t bg_stack[8];
    bool bold_stack[8];
    int sp = 0;
    fg_stack[0] = default_fg;
    bg_stack[0] = 0;
    bold_stack[0] = force_bold;

    int pen_x = start_x;
    const char *p = text;

    while (*p) {
        /* Handle markup tags */
        if (*p == '<') {
            if (strncmp(p, "<b>", 3) == 0) {
                if (sp < 7) {
                    sp++;
                    fg_stack[sp] = fg_stack[sp - 1];
                    bg_stack[sp] = bg_stack[sp - 1];
                    bold_stack[sp] = true;
                }
                p += 3;
                continue;
            } else if (strncmp(p, "</b>", 4) == 0) {
                if (sp > 0) sp--;
                p += 4;
                continue;
            } else if (strncmp(p, "<tt>", 4) == 0) {
                p += 4;
                continue;
            } else if (strncmp(p, "</tt>", 5) == 0) {
                p += 5;
                continue;
            } else if (strncmp(p, "</span>", 7) == 0) {
                if (sp > 0) sp--;
                p += 7;
                continue;
            } else if (strncmp(p, "<span", 5) == 0) {
                const char *end = strchr(p, '>');
                if (end) {
                    uint32_t new_fg = fg_stack[sp];
                    uint32_t new_bg = bg_stack[sp];
                    bool new_bold = bold_stack[sp];

                    const char *c_attr = strstr(p, "color=");
                    if (c_attr && c_attr < end) {
                        c_attr += 6;
                        if (*c_attr == '\'' || *c_attr == '"') c_attr++;
                        new_fg = parse_hex_color(c_attr, new_fg);
                    }
                    const char *bg_attr = strstr(p, "background=");
                    if (bg_attr && bg_attr < end) {
                        bg_attr += 11;
                        if (*bg_attr == '\'' || *bg_attr == '"') bg_attr++;
                        new_bg = parse_hex_color(bg_attr, new_bg);
                    }
                    if (sp < 7) {
                        sp++;
                        fg_stack[sp] = new_fg;
                        bg_stack[sp] = new_bg;
                        bold_stack[sp] = new_bold;
                    }
                    p = end + 1;
                    continue;
                }
            }
        }

        uint32_t cp = utf8_next(&p);
        if (cp == 0) break;
        if (cp == 0xFE0F || cp == 0xFE0E) continue; /* Variation selectors */

        /* Check for 2-codepoint Regional Indicator flag sequence (U+1F1E6..U+1F1FF) */
        FT_UInt emoji_glyph = 0;
        if (cp >= 0x1F1E6 && cp <= 0x1F1FF && g_face_emoji) {
            const char *peek = p;
            uint32_t cp2 = utf8_next(&peek);
            if (cp2 >= 0x1F1E6 && cp2 <= 0x1F1FF) {
                p = peek;
                FT_UInt g1 = FT_Get_Char_Index(g_face_emoji, cp);
                FT_UInt g2 = FT_Get_Char_Index(g_face_emoji, cp2);
                emoji_glyph = emoji_gsub_ligature(g1, g2);
            }
        } else if (is_color_emoji_cp(cp) && g_face_emoji) {
            emoji_glyph = FT_Get_Char_Index(g_face_emoji, cp);
        }

        if (emoji_glyph != 0 && g_face_emoji) {
            if (FT_Load_Glyph(g_face_emoji, emoji_glyph, FT_LOAD_COLOR) == 0 &&
                FT_Render_Glyph(g_face_emoji->glyph, FT_RENDER_MODE_NORMAL) == 0) {
                FT_Bitmap *bm = &g_face_emoji->glyph->bitmap;
                int target_h = px_size + 1;
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
                            uint32_t sum_b = 0, sum_g = 0, sum_r = 0, sum_a = 0, cnt = 0;
                            for (int sy = sy0; sy < sy1 && sy < (int)bm->rows; sy++) {
                                const uint8_t *row = bm->buffer + sy * bm->pitch;
                                for (int sx = sx0; sx < sx1 && sx < (int)bm->width; sx++) {
                                    const uint8_t *px_p = row + sx * 4;
                                    sum_b += px_p[0];
                                    sum_g += px_p[1];
                                    sum_r += px_p[2];
                                    sum_a += px_p[3];
                                    cnt++;
                                }
                            }
                            if (cnt > 0 && sum_a > 0) {
                                uint32_t pre = ((sum_a / cnt) << 24) | ((sum_r / cnt) << 16) |
                                               ((sum_g / cnt) << 8) | (sum_b / cnt);
                                blend_premul(s, pen_x + tx, dst_y0 + ty, pre);
                            }
                        }
                    }
                }
                pen_x += target_w + 1;
                continue;
            }
        }

        /* Standard / symbol glyph rendering */
        FT_Face face = bold_stack[sp] ? g_face_bold : g_face_reg;
        FT_UInt gidx = FT_Get_Char_Index(face, cp);
        if (gidx == 0 && g_face_sym) {
            gidx = FT_Get_Char_Index(g_face_sym, cp);
            if (gidx != 0) face = g_face_sym;
        }
        if (gidx == 0 && g_face_symbola) {
            FT_Set_Pixel_Sizes(g_face_symbola, 0, px_size == 16 ? 18 : px_size);
            gidx = FT_Get_Char_Index(g_face_symbola, cp);
            if (gidx != 0) face = g_face_symbola;
        }
        if (cp == 0x23F8) { /* ⏸ Pause icon: two crisp vertical bars */
            if (draw && s) {
                fill_rect(s, pen_x + 2, baseline_y - 12, 3, 12, fg_stack[sp]);
                fill_rect(s, pen_x + 8, baseline_y - 12, 3, 12, fg_stack[sp]);
            }
            pen_x += 13;
            continue;
        }
        if (cp == 0x25B6) { /* ▶ Play icon: crisp right-pointing triangle */
            if (draw && s) {
                for (int row = 0; row < 12; row++) {
                    int dist = (row < 6) ? row : (11 - row);
                    int w = (dist * 9) / 5 + 1;
                    fill_rect(s, pen_x + 2, baseline_y - 12 + row, w, 1, fg_stack[sp]);
                }
            }
            pen_x += 13;
            continue;
        }
        if (cp == 0x21F8) { /* ⇸ Rightwards Arrow With Vertical Stroke (Sway passthrough mode) */
            if (draw && s) {
                int cy = baseline_y - 6;
                fill_rect(s, pen_x + 1, cy - 1, 11, 2, fg_stack[sp]); /* Horizontal shaft */
                fill_rect(s, pen_x + 5, cy - 5, 2, 10, fg_stack[sp]); /* Vertical stroke */
                for (int d = 0; d < 5; d++) { /* Arrowhead */
                    fill_rect(s, pen_x + 9 + d, cy - (4 - d), 1, 2 * (4 - d) + 1, fg_stack[sp]);
                }
            }
            pen_x += 15;
            continue;
        }

        if (FT_Load_Glyph(face, gidx, FT_LOAD_RENDER) == 0) {
            FT_GlyphSlot slot = face->glyph;
            int adv = (int)(slot->advance.x >> 6);
            if (adv <= 0) adv = px_size / 2;

            if (draw && s) {
                if (bg_stack[sp] != 0) {
                    fill_rect(s, pen_x, baseline_y - px_size + 2, adv, px_size + 3, bg_stack[sp]);
                }
                uint32_t fg = fg_stack[sp];
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

static int measure_markup_line(const char *text, int px_size, bool force_bold) {
    return render_markup_line(NULL, text, 0, 0, px_size, COL_TEXT, force_bold, false);
}

static void draw_separator(SurfaceBuf *s, int x) {
    for (int y = 0; y < BAR_HEIGHT - 1; y++) {
        blend_pixel(s, x, y, COL_BAR_BORDER);
    }
}

/* ============================================================================
 * Clock & ISO-8601 Year Calendar Generator (matching config.jsonc)
 * ============================================================================ */
static void format_tz_time(const char *tz_name, time_t t, struct tm *out_tm) {
    const char *old_tz = getenv("TZ");
    char saved_tz[128] = "";
    bool had_tz = (old_tz != NULL);
    if (had_tz) snprintf(saved_tz, sizeof(saved_tz), "%s", old_tz);

    if (tz_name && *tz_name) setenv("TZ", tz_name, 1);
    else unsetenv("TZ");
    tzset();
    localtime_r(&t, out_tm);

    if (had_tz) setenv("TZ", saved_tz, 1);
    else unsetenv("TZ");
    tzset();
}


static int days_in_month(int y, int m) {
    static const int dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0))) return 29;
    return dm[m - 1];
}

static void build_clock_strings(char *bar_out, size_t bar_sz, char *tip_out, size_t tip_sz) {
    time_t now = time(NULL);
    const char *cur_tz = (g_tz_count > 0) ? g_tzs[g_tz_idx].tz : "";
    const char *cur_lbl = (g_tz_count > 0) ? g_tzs[g_tz_idx].label : "";

    struct tm tm_cur;
    format_tz_time(cur_tz, now, &tm_cur);

    char dt_buf[64];
    strftime(dt_buf, sizeof(dt_buf), g_clock_alt ? "%a %d/%b/%Y | %H:%M:%S" : "%a %d/%b/%Y | %H:%M", &tm_cur);

    if (cur_lbl && *cur_lbl) {
        snprintf(bar_out, bar_sz, "%s %s", dt_buf, cur_lbl);
    } else {
        snprintf(bar_out, bar_sz, "%s", dt_buf);
    }

    /* Build tooltip: {tz_list}\n\n{calendar} */
    size_t off = 0;
    for (int i = 0; i < g_tz_count; i++) {
        struct tm tm_z;
        format_tz_time(g_tzs[i].tz, now, &tm_z);
        char tbuf[16];
        strftime(tbuf, sizeof(tbuf), "%H:%M", &tm_z);

        if (i > 0) {
            off += snprintf(tip_out + off, tip_sz - off, " | ");
        }

        if (g_tzs[i].label[0] != '\0') {
            off += snprintf(tip_out + off, tip_sz - off, "%s %s", tbuf, g_tzs[i].label);
        } else {
            off += snprintf(tip_out + off, tip_sz - off, "%s", tbuf);
        }
    }
    off += snprintf(tip_out + off, tip_sz - off, "\n\n");

    /* 12-month calendar in 4 rows x 3 cols with left ISO week numbers */
    int year = tm_cur.tm_year + 1900;
    int today_m = tm_cur.tm_mon + 1;
    int today_d = tm_cur.tm_mday;

    static const char *mnames[12] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };

    for (int row_m = 0; row_m < 4; row_m++) {
        /* Month headers */
        for (int col_m = 0; col_m < 3; col_m++) {
            int m = row_m * 3 + col_m + 1;
            const char *mn = mnames[m - 1];
            int len = (int)strlen(mn);
            int pad_l = (23 - len) / 2;
            int pad_r = 23 - len - pad_l;
            off += snprintf(tip_out + off, tip_sz - off, "%*s<span color='#ffead3'><b>%s</b></span>%*s%s",
                            pad_l, "", mn, pad_r, "", (col_m < 2) ? "   " : "\n");
        }
        /* Weekday headers */
        for (int col_m = 0; col_m < 3; col_m++) {
            off += snprintf(tip_out + off, tip_sz - off,
                            "   <span color='#7d6b5e'>Mo Tu We Th Fr Sa Su</span>%s",
                            (col_m < 2) ? "   " : "\n");
        }
        /* Up to 6 week rows per month row */
        for (int w = 0; w < 6; w++) {
            for (int col_m = 0; col_m < 3; col_m++) {
                int m = row_m * 3 + col_m + 1;
                struct tm tm_first = {0};
                tm_first.tm_year = year - 1900;
                tm_first.tm_mon = m - 1;
                tm_first.tm_mday = 1;
                tm_first.tm_hour = 12;
                mktime(&tm_first);
                int first_wday = (tm_first.tm_wday + 6) % 7; /* Monday = 0 .. Sunday = 6 */
                int dim = days_in_month(year, m);

                int start_day = w * 7 - first_wday + 1;
                if (start_day > dim) {
                    off += snprintf(tip_out + off, tip_sz - off, "%23s%s", "", (col_m < 2) ? "   " : "\n");
                    continue;
                }

                /* Determine ISO week number for this week row */
                int ref_day = start_day < 1 ? 1 : start_day;
                struct tm tm_wk = {0};
                tm_wk.tm_year = year - 1900;
                tm_wk.tm_mon = m - 1;
                tm_wk.tm_mday = ref_day;
                tm_wk.tm_hour = 12;
                mktime(&tm_wk);
                char wk_str[8];
                strftime(wk_str, sizeof(wk_str), "%V", &tm_wk);
                off += snprintf(tip_out + off, tip_sz - off, "<span color='#7d6b5e'>%2s</span> ", wk_str);

                for (int dow = 0; dow < 7; dow++) {
                    int d = start_day + dow;
                    if (d < 1 || d > dim) {
                        off += snprintf(tip_out + off, tip_sz - off, "  %s", (dow < 6) ? " " : "");
                    } else if (m == today_m && d == today_d) {
                        off += snprintf(tip_out + off, tip_sz - off,
                                        "<span background='#ffead3' color='#4a3e35'><b>%2d</b></span>%s",
                                        d, (dow < 6) ? " " : "");
                    } else {
                        off += snprintf(tip_out + off, tip_sz - off, "%2d%s", d, (dow < 6) ? " " : "");
                    }
                }
                off += snprintf(tip_out + off, tip_sz - off, "%s", (col_m < 2) ? "   " : "\n");
            }
        }
        if (row_m < 3) {
            off += snprintf(tip_out + off, tip_sz - off, "\n");
        }
    }
    while (off > 0 && (tip_out[off - 1] == '\n' || tip_out[off - 1] == ' ')) {
        tip_out[--off] = '\0';
    }
}

/* ============================================================================
 * Sway IPC Wire Protocol Client (Workspaces, Mode & Scratchpad)
 * ============================================================================ */
static char g_swaysock_path[256] = "";

static int try_connect_unix(const char *path) {
    if (!path || !*path) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int sway_ipc_connect(void) {
    if (g_swaysock_path[0]) {
        int fd = try_connect_unix(g_swaysock_path);
        if (fd >= 0) return fd;
        g_swaysock_path[0] = '\0';
    }
    const char *env = getenv("SWAYSOCK");
    if (env && *env) {
        int fd = try_connect_unix(env);
        if (fd >= 0) {
            snprintf(g_swaysock_path, sizeof(g_swaysock_path), "%s", env);
            return fd;
        }
    }
    DIR *d = opendir(g_runtime_dir);
    if (!d) return -1;
    struct dirent *de;
    int fd = -1;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "sway-ipc.", 9) == 0) {
            char candidate[512];
            snprintf(candidate, sizeof(candidate), "%s/%s", g_runtime_dir, de->d_name);
            fd = try_connect_unix(candidate);
            if (fd >= 0) {
                snprintf(g_swaysock_path, sizeof(g_swaysock_path), "%s", candidate);
                break;
            }
        }
    }
    closedir(d);
    return fd;
}

static char *sway_ipc_recv(int fd, uint32_t *out_type) {
    uint8_t rhdr[14];
    size_t got = 0;
    while (got < 14) {
        ssize_t n = read(fd, rhdr + got, 14 - got);
        if (n <= 0) return NULL;
        got += (size_t)n;
    }
    uint32_t rlen = 0, rtype = 0;
    memcpy(&rlen, rhdr + 6, 4);
    memcpy(&rtype, rhdr + 10, 4);
    if (out_type) *out_type = rtype;
    if (rlen > 4 * 1024 * 1024) return NULL;
    char *buf = calloc(1, rlen + 1);
    if (!buf) return NULL;
    got = 0;
    while (got < rlen) {
        ssize_t n = read(fd, buf + got, rlen - got);
        if (n <= 0) {
            free(buf);
            return NULL;
        }
        got += (size_t)n;
    }
    buf[rlen] = '\0';
    return buf;
}

static char *sway_ipc_roundtrip(int fd, uint32_t type, const char *payload) {
    uint32_t len = payload ? (uint32_t)strlen(payload) : 0;
    uint8_t hdr[14];
    memcpy(hdr, "i3-ipc", 6);
    memcpy(hdr + 6, &len, 4);
    memcpy(hdr + 10, &type, 4);
    if (write(fd, hdr, 14) != 14) return NULL;
    if (len > 0 && write(fd, payload, len) != (ssize_t)len) return NULL;
    return sway_ipc_recv(fd, NULL);
}

static void sway_send_cmd(const char *cmd) {
    int fd = sway_ipc_connect();
    if (fd < 0) return;
    char *resp = sway_ipc_roundtrip(fd, 0, cmd);
    free(resp);
    close(fd);
}

static void collect_scratchpad_leaves(struct json_object *node, int *cnt, char *tip, size_t tip_sz, size_t *off) {
    if (!node) return;
    struct json_object *nodes = NULL, *fnodes = NULL;
    int n_len = 0, fn_len = 0;
    if (json_object_object_get_ex(node, "nodes", &nodes) && json_object_is_type(nodes, json_type_array))
        n_len = json_object_array_length(nodes);
    if (json_object_object_get_ex(node, "floating_nodes", &fnodes) && json_object_is_type(fnodes, json_type_array))
        fn_len = json_object_array_length(fnodes);

    if (n_len == 0 && fn_len == 0) {
        struct json_object *j_name = NULL, *j_app = NULL, *j_props = NULL, *j_class = NULL;
        const char *title = "";
        const char *app = "";
        if (json_object_object_get_ex(node, "name", &j_name) && json_object_get_string(j_name))
            title = json_object_get_string(j_name);
        if (json_object_object_get_ex(node, "app_id", &j_app) && json_object_get_string(j_app))
            app = json_object_get_string(j_app);
        else if (json_object_object_get_ex(node, "window_properties", &j_props) &&
                 json_object_object_get_ex(j_props, "class", &j_class) && json_object_get_string(j_class))
            app = json_object_get_string(j_class);
        (*cnt)++;
        if (*off < tip_sz - 1) {
            *off += snprintf(tip + *off, tip_sz - *off, "%s%s: %s",
                             (*off > 0) ? "\n" : "",
                             (*app) ? app : "window",
                             (*title) ? title : "scratchpad");
        }
        return;
    }
    for (int i = 0; i < n_len; i++)
        collect_scratchpad_leaves(json_object_array_get_idx(nodes, i), cnt, tip, tip_sz, off);
    for (int i = 0; i < fn_len; i++)
        collect_scratchpad_leaves(json_object_array_get_idx(fnodes, i), cnt, tip, tip_sz, off);
}

static void find_i3_scratch(struct json_object *node, int *cnt, char *tip, size_t tip_sz, size_t *off) {
    if (!node) return;
    struct json_object *j_name = NULL;
    if (json_object_object_get_ex(node, "name", &j_name) && json_object_get_string(j_name) &&
        strcmp(json_object_get_string(j_name), "__i3_scratch") == 0) {
        struct json_object *fnodes = NULL;
        if (json_object_object_get_ex(node, "floating_nodes", &fnodes) && json_object_is_type(fnodes, json_type_array)) {
            int len = json_object_array_length(fnodes);
            for (int i = 0; i < len; i++) {
                collect_scratchpad_leaves(json_object_array_get_idx(fnodes, i), cnt, tip, tip_sz, off);
            }
        }
        return;
    }
    struct json_object *nodes = NULL;
    if (json_object_object_get_ex(node, "nodes", &nodes) && json_object_is_type(nodes, json_type_array)) {
        int len = json_object_array_length(nodes);
        for (int i = 0; i < len; i++) {
            find_i3_scratch(json_object_array_get_idx(nodes, i), cnt, tip, tip_sz, off);
        }
    }
}

static void refresh_sway_state(void) {
    int fd = sway_ipc_connect();
    if (fd < 0) return;

    char *ws_json = sway_ipc_roundtrip(fd, 1 /* GET_WORKSPACES */, "");
    char *tree_json = sway_ipc_roundtrip(fd, 4 /* GET_TREE */, "");
    char *mode_json = sway_ipc_roundtrip(fd, 12 /* GET_BINDING_STATE */, "");
    close(fd);

    pthread_mutex_lock(&g_mutex);
    if (ws_json) {
        struct json_object *root = json_tokener_parse(ws_json);
        if (root && json_object_is_type(root, json_type_array)) {
            int len = json_object_array_length(root);
            g_ws_count = 0;
            for (int i = 0; i < len && g_ws_count < 32; i++) {
                struct json_object *item = json_object_array_get_idx(root, i);
                struct json_object *j_name = NULL, *j_foc = NULL, *j_urg = NULL;
                if (json_object_object_get_ex(item, "name", &j_name)) {
                    snprintf(g_workspaces[g_ws_count].name, sizeof(g_workspaces[0].name),
                             "%s", json_object_get_string(j_name));
                    g_workspaces[g_ws_count].focused =
                        json_object_object_get_ex(item, "focused", &j_foc) && json_object_get_boolean(j_foc);
                    g_workspaces[g_ws_count].urgent =
                        json_object_object_get_ex(item, "urgent", &j_urg) && json_object_get_boolean(j_urg);
                    g_ws_count++;
                }
            }
            json_object_put(root);
        }
        free(ws_json);
    }

    if (tree_json) {
        struct json_object *troot = json_tokener_parse(tree_json);
        if (troot) {
            int cnt = 0;
            size_t off = 0;
            g_scratch_tooltip[0] = '\0';
            find_i3_scratch(troot, &cnt, g_scratch_tooltip, sizeof(g_scratch_tooltip), &off);
            g_scratch_count = cnt;
            json_object_put(troot);
        }
        free(tree_json);
    }

    if (mode_json) {
        struct json_object *mroot = json_tokener_parse(mode_json);
        if (mroot) {
            struct json_object *j_name = NULL;
            if (json_object_object_get_ex(mroot, "name", &j_name) && json_object_get_string(j_name)) {
                const char *mname = json_object_get_string(j_name);
                if (strcmp(mname, "default") == 0) g_sway_mode[0] = '\0';
                else snprintf(g_sway_mode, sizeof(g_sway_mode), "%s", mname);
            }
            json_object_put(mroot);
        }
        free(mode_json);
    }
    pthread_mutex_unlock(&g_mutex);
}

static void *sway_event_thread(void *arg) {
    (void)arg;
    while (1) {
        refresh_sway_state();
        notify_main_loop();

        int fd = sway_ipc_connect();
        if (fd < 0) {
            sleep(2);
            continue;
        }

        /* Auto-discover bar ID if not passed via -b */
        if (!g_sway_bar_id[0]) {
            char *bars_json = sway_ipc_roundtrip(fd, 6 /* GET_BAR_CONFIG */, "");
            if (bars_json) {
                struct json_object *j_bars = json_tokener_parse(bars_json);
                if (j_bars && json_object_is_type(j_bars, json_type_array) && json_object_array_length(j_bars) > 0) {
                    struct json_object *first_bar = json_object_array_get_idx(j_bars, 0);
                    if (first_bar && json_object_get_string(first_bar)) {
                        pthread_mutex_lock(&g_mutex);
                        snprintf(g_sway_bar_id, sizeof(g_sway_bar_id), "%s", json_object_get_string(first_bar));
                        pthread_mutex_unlock(&g_mutex);
                    }
                }
                if (j_bars) json_object_put(j_bars);
                free(bars_json);
            }
        }
        if (g_sway_bar_id[0]) {
            char *cfg_json = sway_ipc_roundtrip(fd, 6 /* GET_BAR_CONFIG */, g_sway_bar_id);
            if (cfg_json) {
                struct json_object *j_cfg = json_tokener_parse(cfg_json);
                if (j_cfg) {
                    struct json_object *j_mode = NULL, *j_hs = NULL;
                    pthread_mutex_lock(&g_mutex);
                    if (json_object_object_get_ex(j_cfg, "mode", &j_mode) && json_object_get_string(j_mode)) {
                        snprintf(g_sway_bar_mode, sizeof(g_sway_bar_mode), "%s", json_object_get_string(j_mode));
                    }
                    if (json_object_object_get_ex(j_cfg, "hidden_state", &j_hs) && json_object_get_string(j_hs)) {
                        snprintf(g_sway_bar_hidden_state, sizeof(g_sway_bar_hidden_state), "%s", json_object_get_string(j_hs));
                    }
                    pthread_mutex_unlock(&g_mutex);
                    json_object_put(j_cfg);
                }
                free(cfg_json);
            }
        }

        char *sub = sway_ipc_roundtrip(fd, 2 /* SUBSCRIBE */,
                                       "[\"workspace\",\"window\",\"mode\",\"barconfig_update\",\"bar_state_update\"]");
        free(sub);

        while (1) {
            uint32_t ev_type = 0;
            char *ev = sway_ipc_recv(fd, &ev_type);
            if (!ev) break;
            if ((ev_type & 0x7FFFFFFF) == 2 /* IPC_EVENT_MODE */) {
                struct json_object *eroot = json_tokener_parse(ev);
                if (eroot) {
                    struct json_object *j_chg = NULL;
                    if (json_object_object_get_ex(eroot, "change", &j_chg) && json_object_get_string(j_chg)) {
                        const char *chg = json_object_get_string(j_chg);
                        pthread_mutex_lock(&g_mutex);
                        if (strcmp(chg, "default") == 0) g_sway_mode[0] = '\0';
                        else snprintf(g_sway_mode, sizeof(g_sway_mode), "%s", chg);
                        pthread_mutex_unlock(&g_mutex);
                    }
                    json_object_put(eroot);
                }
            } else if ((ev_type & 0x7FFFFFFF) == 4 /* IPC_EVENT_BARCONFIG_UPDATE */) {
                struct json_object *eroot = json_tokener_parse(ev);
                if (eroot) {
                    struct json_object *j_id = NULL, *j_mode = NULL, *j_hs = NULL;
                    json_object_object_get_ex(eroot, "id", &j_id);
                    const char *id = j_id ? json_object_get_string(j_id) : NULL;
                    if (!g_sway_bar_id[0] || !id || strcmp(id, g_sway_bar_id) == 0) {
                        pthread_mutex_lock(&g_mutex);
                        if (json_object_object_get_ex(eroot, "mode", &j_mode) && json_object_get_string(j_mode)) {
                            snprintf(g_sway_bar_mode, sizeof(g_sway_bar_mode), "%s", json_object_get_string(j_mode));
                        }
                        if (json_object_object_get_ex(eroot, "hidden_state", &j_hs) && json_object_get_string(j_hs)) {
                            snprintf(g_sway_bar_hidden_state, sizeof(g_sway_bar_hidden_state), "%s", json_object_get_string(j_hs));
                        }
                        pthread_mutex_unlock(&g_mutex);
                    }
                    json_object_put(eroot);
                }
            } else if ((ev_type & 0x7FFFFFFF) == 20 /* IPC_EVENT_BAR_STATE_UPDATE */) {
                struct json_object *eroot = json_tokener_parse(ev);
                if (eroot) {
                    struct json_object *j_id = NULL, *j_vis = NULL;
                    json_object_object_get_ex(eroot, "id", &j_id);
                    const char *id = j_id ? json_object_get_string(j_id) : NULL;
                    if (!g_sway_bar_id[0] || !id || strcmp(id, g_sway_bar_id) == 0) {
                        if (json_object_object_get_ex(eroot, "visible_by_modifier", &j_vis)) {
                            pthread_mutex_lock(&g_mutex);
                            g_sway_visible_by_modifier = json_object_get_boolean(j_vis);
                            pthread_mutex_unlock(&g_mutex);
                        }
                    }
                    json_object_put(eroot);
                }
            }
            free(ev);
            refresh_sway_state();
            notify_main_loop();
        }
        close(fd);
        sleep(1);
    }
    return NULL;
}

/* ============================================================================
 * Direct D-Bus Wire Protocol Client (Quiet Notifications, same as quietctl.c)
 * ============================================================================ */
static int dbus_append_field(uint8_t *buf, int offset, uint8_t code, char type, const char *val) {
    while (offset % 8 != 0) buf[offset++] = 0;
    buf[offset++] = code;
    buf[offset++] = 1;
    buf[offset++] = (uint8_t)type;
    buf[offset++] = 0;
    uint32_t len = (uint32_t)strlen(val);
    if (type == 'g') {
        /* D-Bus SIGNATURE ('g') uses a 1-byte length prefix, NOT 4-byte uint32 */
        buf[offset++] = (uint8_t)len;
    } else {
        memcpy(buf + offset, &len, 4);
        offset += 4;
    }
    memcpy(buf + offset, val, len + 1);
    offset += (int)len + 1;
    return offset;
}

static int dbus_build_call(uint8_t *buf, uint32_t serial, uint8_t flags,
                           const char *dest, const char *path, const char *iface,
                           const char *member, const char *body_str) {
    buf[0] = 'l';
    buf[1] = 1; /* METHOD_CALL */
    buf[2] = flags;
    buf[3] = 1;
    uint32_t body_len = 0;
    if (body_str) body_len = 4 + (uint32_t)strlen(body_str) + 1;
    memcpy(buf + 4, &body_len, 4);
    memcpy(buf + 8, &serial, 4);
    int offset = 16;
    offset = dbus_append_field(buf, offset, 1, 'o', path);
    offset = dbus_append_field(buf, offset, 3, 's', member);
    if (iface) offset = dbus_append_field(buf, offset, 2, 's', iface);
    if (dest) offset = dbus_append_field(buf, offset, 6, 's', dest);
    if (body_str) offset = dbus_append_field(buf, offset, 8, 'g', "s");
    uint32_t fields_len = (uint32_t)(offset - 16);
    memcpy(buf + 12, &fields_len, 4);
    while (offset % 8 != 0) buf[offset++] = 0;
    if (body_str) {
        uint32_t slen = (uint32_t)strlen(body_str);
        memcpy(buf + offset, &slen, 4);
        offset += 4;
        memcpy(buf + offset, body_str, slen + 1);
        offset += (int)slen + 1;
    }
    return offset;
}

static int dbus_connect_session(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/bus", g_runtime_dir);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void dbus_quiet_call_oneshot(const char *member) {
    int fd = dbus_connect_session();
    if (fd < 0) return;
    uint8_t req[1024];
    static const unsigned char hs[] =
        "\0AUTH EXTERNAL\r\nDATA\r\nBEGIN\r\n";
    int len = (int)sizeof(hs) - 1;
    memcpy(req, hs, len);
    len += dbus_build_call(req + len, 1, 0, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "Hello", NULL);
    len += dbus_build_call(req + len, 2, 0x01, "org.freedesktop.Notifications",
                           "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                           member, NULL);
    if (write(fd, req, len) > 0) {
        uint8_t rbuf[512];
        ssize_t n = read(fd, rbuf, sizeof(rbuf));
        (void)n;
    }
    close(fd);
}

static void fetch_quiet_latest_preview(char *out, size_t out_sz) {
    out[0] = '\0';
    int fd = dbus_connect_session();
    if (fd < 0) return;
    uint8_t req[1024];
    static const unsigned char hs[] = "\0AUTH EXTERNAL\r\nDATA\r\nBEGIN\r\n";
    int len = (int)sizeof(hs) - 1;
    memcpy(req, hs, len);
    len += dbus_build_call(req + len, 1, 0, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "Hello", NULL);
    len += dbus_build_call(req + len, 2, 0, "org.freedesktop.Notifications",
                           "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                           "DumpNotifications", NULL);
    if (write(fd, req, len) <= 0) {
        close(fd);
        return;
    }
    uint8_t resp[16384];
    int resp_len = 0;
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    while (resp_len < (int)sizeof(resp) - 1 && poll(&pfd, 1, 100) > 0) {
        ssize_t n = read(fd, resp + resp_len, sizeof(resp) - 1 - resp_len);
        if (n <= 0) break;
        resp_len += (int)n;
    }
    close(fd);
    resp[resp_len] = '\0';

    /* Find the last summary:"..." and body:"..." in DumpNotifications */
    for (int i = 0; i <= resp_len - 16; i++) {
        if (resp[i] != 'l' || resp[i + 1] != 2) continue;
        uint32_t fields_len = *(uint32_t *)(resp + i + 12);
        if (fields_len >= 8 && memmem(resp + i + 16, fields_len, "\x05\x01\x75\x00\x02\x00\x00\x00", 8)) {
            uint32_t body_len = *(uint32_t *)(resp + i + 4);
            int hdr_total = 16 + ((fields_len + 7) & ~7);
            if (body_len >= 4 && i + hdr_total + 4 <= resp_len) {
                const char *dump_s = (const char *)(resp + i + hdr_total + 4);
                const char *p = dump_s;
                const char *last_sum = NULL, *last_body = NULL;
                while ((p = strstr(p, "summary:\"")) != NULL) {
                    last_sum = p;
                    p += 9;
                }
                p = dump_s;
                while ((p = strstr(p, "body:\"")) != NULL) {
                    last_body = p;
                    p += 6;
                }
                char sum[128] = "", body[128] = "";
                if (last_sum) {
                    last_sum += 9;
                    const char *end = strchr(last_sum, '"');
                    size_t sl = end ? (size_t)(end - last_sum) : 0;
                    if (sl >= sizeof(sum)) sl = sizeof(sum) - 1;
                    memcpy(sum, last_sum, sl);
                    sum[sl] = '\0';
                }
                if (last_body) {
                    last_body += 6;
                    const char *end = strchr(last_body, '"');
                    size_t bl = end ? (size_t)(end - last_body) : 0;
                    if (bl >= sizeof(body)) bl = sizeof(body) - 1;
                    memcpy(body, last_body, bl);
                    body[bl] = '\0';
                }
                if (sum[0] && body[0]) snprintf(out, out_sz, "%s: %s", sum, body);
                else if (sum[0]) snprintf(out, out_sz, "%s", sum);
                else if (body[0]) snprintf(out, out_sz, "%s", body);
            }
            break;
        }
    }
}

static void poll_quiet(void) {
    int fd = dbus_connect_session();
    if (fd < 0) return;
    uint8_t req[1024];
    static const unsigned char hs[] = "\0AUTH EXTERNAL\r\nDATA\r\nBEGIN\r\n";
    int len = (int)sizeof(hs) - 1;
    memcpy(req, hs, len);
    len += dbus_build_call(req + len, 1, 0, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "Hello", NULL);
    len += dbus_build_call(req + len, 2, 0, "org.freedesktop.Notifications",
                           "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                           "ShowNotificationCount", NULL);
    if (write(fd, req, len) <= 0) {
        close(fd);
        return;
    }
    uint8_t resp[2048];
    int resp_len = 0;
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    while (resp_len < (int)sizeof(resp) && poll(&pfd, 1, 100) > 0) {
        ssize_t n = read(fd, resp + resp_len, sizeof(resp) - resp_len);
        if (n <= 0) break;
        resp_len += (int)n;
        for (int i = 0; i <= resp_len - 16; i++) {
            if (resp[i] != 'l' || resp[i + 1] != 2) continue;
            uint32_t fields_len = *(uint32_t *)(resp + i + 12);
            if (fields_len >= 8 && memmem(resp + i + 16, fields_len, "\x05\x01\x75\x00\x02\x00\x00\x00", 8)) {
                uint32_t body_len = *(uint32_t *)(resp + i + 4);
                int hdr_total = 16 + ((fields_len + 7) & ~7);
                if (body_len >= 8 && i + hdr_total + 8 <= resp_len) {
                    uint32_t count = *(uint32_t *)(resp + i + hdr_total);
                    uint32_t urgency = *(uint32_t *)(resp + i + hdr_total + 4);
                    close(fd);

                    uint32_t prev_count;
                    pthread_mutex_lock(&g_mutex);
                    prev_count = g_quiet_count;
                    g_quiet_count = count;
                    g_quiet_urgency = urgency;
                    if (count == 0 || count < prev_count) {
                        g_quiet_preview[0] = '\0';
                        g_quiet_preview_until = 0;
                    }
                    pthread_mutex_unlock(&g_mutex);

                    if (count > prev_count) {
                        char preview[256] = "";
                        fetch_quiet_latest_preview(preview, sizeof(preview));
                        if (preview[0]) {
                            pthread_mutex_lock(&g_mutex);
                            if (!g_quiet_preview[0]) {
                                snprintf(g_quiet_preview, sizeof(g_quiet_preview), "%s", preview);
                                g_quiet_preview_until = time(NULL) + 8;
                            }
                            pthread_mutex_unlock(&g_mutex);
                        }
                    }
                    return;
                }
            }
        }
    }
    close(fd);
}

static void poll_mpris(void);
static void poll_screensaver(void);

static void *quiet_dbus_thread(void *arg) {
    (void)arg;
    poll_quiet();
    poll_mpris();
    poll_screensaver();
    notify_main_loop();

    while (1) {
        int fd = dbus_connect_session();
        if (fd < 0) {
            sleep(3);
            continue;
        }
        uint8_t req[4096];
        static const unsigned char hs[] = "\0AUTH EXTERNAL\r\nDATA\r\nBEGIN\r\n";
        int len = (int)sizeof(hs) - 1;
        memcpy(req, hs, len);
        len += dbus_build_call(req + len, 1, 0, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                               "org.freedesktop.DBus", "Hello", NULL);
        len += dbus_build_call(req + len, 2, 0, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                               "org.freedesktop.DBus", "AddMatch",
                               "type='signal',interface='org.freedesktop.Notifications',member='NotificationsUpdated'");
        len += dbus_build_call(req + len, 3, 0, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                               "org.freedesktop.DBus", "AddMatch",
                               "type='signal',interface='org.freedesktop.Notifications',member='NotificationClosed'");
        len += dbus_build_call(req + len, 4, 0, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                               "org.freedesktop.DBus", "AddMatch",
                               "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',path='/org/mpris/MediaPlayer2'");
        len += dbus_build_call(req + len, 5, 0, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                               "org.freedesktop.DBus", "AddMatch",
                               "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0namespace='org.mpris.MediaPlayer2'");
        len += dbus_build_call(req + len, 6, 0, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                               "org.freedesktop.DBus", "AddMatch",
                               "type='signal',sender='org.freedesktop.systemd1',path='/org/freedesktop/systemd1/unit/swayidle_2eservice'");
        len += dbus_build_call(req + len, 7, 0, "org.freedesktop.Notifications",
                               "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                               "SignalNotificationCount", NULL);
        if (write(fd, req, len) <= 0) {
            close(fd);
            sleep(2);
            continue;
        }

        uint8_t buf[65536];
        int filled = 0;
        while (1) {
            ssize_t n = read(fd, buf + filled, sizeof(buf) - filled);
            if (n <= 0) break;
            filled += (int)n;

            int pos = 0;
            while (pos + 16 <= filled) {
                if (buf[pos] != 'l' || (buf[pos + 1] < 1 || buf[pos + 1] > 4) || buf[pos + 3] != 1) {
                    pos++;
                    continue;
                }
                uint8_t mtype = buf[pos + 1];
                uint32_t body_len = *(uint32_t *)(buf + pos + 4);
                uint32_t fields_len = *(uint32_t *)(buf + pos + 12);
                int hdr_total = 16 + ((fields_len + 7) & ~7);
                int msg_total = hdr_total + (int)body_len;
                if (hdr_total > 4096 || body_len > 32768) {
                    pos += msg_total;
                    continue;
                }
                if (pos + msg_total > filled) break;

                if (mtype == 4) {
                    /* 1. Quiet NotificationsUpdated (uuus) */
                    if (memmem(buf + pos + 16, fields_len, "NotificationsUpdated", 20) && body_len >= 16) {
                        uint8_t *b = buf + pos + hdr_total;
                        uint32_t mode = *(uint32_t *)(b + 0);
                        uint32_t num = *(uint32_t *)(b + 4);
                        uint32_t urg = *(uint32_t *)(b + 8);
                        uint32_t slen = *(uint32_t *)(b + 12);
                        const char *msg = (slen > 0 && 16 + slen <= body_len) ? (const char *)(b + 16) : "";

                        uint32_t prev_num;
                        pthread_mutex_lock(&g_mutex);
                        prev_num = g_quiet_count;
                        g_quiet_count = num;
                        g_quiet_urgency = urg;
                        if (mode != 0 || num < prev_num || num == 0) {
                            g_quiet_preview[0] = '\0';
                            g_quiet_preview_until = 0;
                        } else if (msg && *msg) {
                            snprintf(g_quiet_preview, sizeof(g_quiet_preview), "%s", msg);
                            g_quiet_preview_until = time(NULL) + 10;
                        }
                        pthread_mutex_unlock(&g_mutex);

                        if (mode == 0 && num > prev_num && (!msg || !*msg)) {
                            char preview[256] = "";
                            fetch_quiet_latest_preview(preview, sizeof(preview));
                            if (preview[0]) {
                                pthread_mutex_lock(&g_mutex);
                                snprintf(g_quiet_preview, sizeof(g_quiet_preview), "%s", preview);
                                g_quiet_preview_until = time(NULL) + 10;
                                pthread_mutex_unlock(&g_mutex);
                            }
                        }
                        notify_main_loop();
                    }
                    /* Standard NotificationClosed signal (uu) */
                    else if (memmem(buf + pos + 16, fields_len, "NotificationClosed", 18)) {
                        pthread_mutex_lock(&g_mutex);
                        g_quiet_preview[0] = '\0';
                        g_quiet_preview_until = 0;
                        pthread_mutex_unlock(&g_mutex);
                        poll_quiet();
                        notify_main_loop();
                    }
                    /* 2. MPRIS MediaPlayer2 state/metadata or bus owner change */
                    else if (memmem(buf + pos + 16, fields_len, "/org/mpris/MediaPlayer2", 23) ||
                             memmem(buf + pos + 16, fields_len, "NameOwnerChanged", 16)) {
                        poll_mpris();
                        notify_main_loop();
                    }
                    /* 3. systemd user manager unit state change (swayidle.service) */
                    else if (memmem(buf + pos + 16, fields_len, "org.freedesktop.systemd1", 24)) {
                        poll_screensaver();
                        notify_main_loop();
                    }
                }
                pos += msg_total;
            }
            if (pos > 0) {
                memmove(buf, buf + pos, filled - pos);
                filled -= pos;
            }
        }
        close(fd);
        sleep(2);
    }
    return NULL;
}

/* ============================================================================
 * Google Calendar Module (OAuth2 Refresh + Upcoming Events)
 * ============================================================================ */
static void load_gcal_credentials(void) {
    if (g_gcal_token_arg[0]) {
        if (access(g_gcal_token_arg, R_OK) == 0) {
            snprintf(g_gcal_token_file, sizeof(g_gcal_token_file), "%s", g_gcal_token_arg);
        } else {
            /* Direct raw bearer token passed */
            snprintf(g_gcal_access_token, sizeof(g_gcal_access_token), "%s", g_gcal_token_arg);
            g_gcal_token_expiry = time(NULL) + 86400;
            return;
        }
    } else {
        snprintf(g_gcal_token_file, sizeof(g_gcal_token_file),
                 "%s/.dotfiles/tmp/google_calendar.auth.token", g_home_dir);
    }

    struct json_object *root = json_object_from_file(g_gcal_token_file);
    if (!root) return;
    struct json_object *j_at = NULL, *j_cid = NULL, *j_cs = NULL, *j_rt = NULL;
    if (json_object_object_get_ex(root, "access_token", &j_at) && json_object_get_string(j_at))
        snprintf(g_gcal_access_token, sizeof(g_gcal_access_token), "%s", json_object_get_string(j_at));
    if (json_object_object_get_ex(root, "client_id", &j_cid) && json_object_get_string(j_cid))
        snprintf(g_gcal_client_id, sizeof(g_gcal_client_id), "%s", json_object_get_string(j_cid));
    if (json_object_object_get_ex(root, "client_secret", &j_cs) && json_object_get_string(j_cs))
        snprintf(g_gcal_client_secret, sizeof(g_gcal_client_secret), "%s", json_object_get_string(j_cs));
    if (json_object_object_get_ex(root, "refresh_token", &j_rt) && json_object_get_string(j_rt))
        snprintf(g_gcal_refresh_token, sizeof(g_gcal_refresh_token), "%s", json_object_get_string(j_rt));
    json_object_put(root);
}

static bool refresh_gcal_token(void) {
    if (!g_gcal_refresh_token[0] || !g_gcal_client_id[0] || !g_gcal_client_secret[0]) return false;
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "curl -s --max-time 10 -X POST https://oauth2.googleapis.com/token "
             "-d 'client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token'",
             g_gcal_client_id, g_gcal_client_secret, g_gcal_refresh_token);
    char *resp = run_cmd_capture(cmd, 16384);
    if (!resp) return false;
    struct json_object *root = json_tokener_parse(resp);
    free(resp);
    if (!root) return false;

    bool ok = false;
    struct json_object *j_at = NULL, *j_exp = NULL;
    if (json_object_object_get_ex(root, "access_token", &j_at) && json_object_get_string(j_at)) {
        snprintf(g_gcal_access_token, sizeof(g_gcal_access_token), "%s", json_object_get_string(j_at));
        int exp = json_object_object_get_ex(root, "expires_in", &j_exp) ? json_object_get_int(j_exp) : 3600;
        g_gcal_token_expiry = time(NULL) + exp - 60;
        ok = true;

        if (g_gcal_token_file[0]) {
            struct json_object *froot = json_object_from_file(g_gcal_token_file);
            if (froot) {
                json_object_object_add(froot, "access_token", json_object_new_string(g_gcal_access_token));
                json_object_to_file_ext(g_gcal_token_file, froot, JSON_C_TO_STRING_PRETTY);
                json_object_put(froot);
            }
        }
    }
    json_object_put(root);
    return ok;
}

static void format_iso_12h(const char *iso, char *out, size_t out_sz, int *out_h, int *out_m) {
    out[0] = '\0';
    *out_h = 0;
    *out_m = 0;
    const char *t = strchr(iso, 'T');
    if (t && strlen(t) >= 6) {
        int h = (t[1] - '0') * 10 + (t[2] - '0');
        int m = (t[4] - '0') * 10 + (t[5] - '0');
        *out_h = h;
        *out_m = m;
        const char *ampm = (h >= 12) ? "PM" : "AM";
        int h12 = h % 12;
        if (h12 == 0) h12 = 12;
        snprintf(out, out_sz, "%d:%02d %s", h12, m, ampm);
    }
}

static void fetch_gcal_events(void) {
    pthread_mutex_lock(&g_mutex);
    if (g_gcal_fetching) {
        pthread_mutex_unlock(&g_mutex);
        return;
    }
    g_gcal_fetching = true;
    pthread_mutex_unlock(&g_mutex);

    if (!g_gcal_access_token[0] || time(NULL) >= g_gcal_token_expiry) {
        refresh_gcal_token();
    }
    if (!g_gcal_access_token[0]) {
        pthread_mutex_lock(&g_mutex);
        g_gcal_fetching = false;
        pthread_mutex_unlock(&g_mutex);
        return;
    }

    time_t now = time(NULL);
    struct tm tm_utc;
    char min_buf[64], max_buf[64];
    gmtime_r(&now, &tm_utc);
    strftime(min_buf, sizeof(min_buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    time_t later = now + 12 * 3600;
    gmtime_r(&later, &tm_utc);
    strftime(max_buf, sizeof(max_buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "curl -s --max-time 10 -H 'Authorization: Bearer %s' "
             "'https://www.googleapis.com/calendar/v3/calendars/primary/events?timeMin=%s&timeMax=%s&singleEvents=true&orderBy=startTime'",
             g_gcal_access_token, min_buf, max_buf);

    char *resp = run_cmd_capture(cmd, 131072);
    if (resp && strstr(resp, "\"UNAUTHENTICATED\"")) {
        free(resp);
        if (refresh_gcal_token()) {
            snprintf(cmd, sizeof(cmd),
                     "curl -s --max-time 10 -H 'Authorization: Bearer %s' "
                     "'https://www.googleapis.com/calendar/v3/calendars/primary/events?timeMin=%s&timeMax=%s&singleEvents=true&orderBy=startTime'",
                     g_gcal_access_token, min_buf, max_buf);
            resp = run_cmd_capture(cmd, 131072);
        } else {
            resp = NULL;
        }
    }

    GCalEvent new_evs[16];
    int new_cnt = 0;
    static const char *colors[7] = {
        "#d0e6ff", "#bbdaff", "#99c7ff", "#86bcff", "#62a9ff", "#8c8cff", "#7979ff"
    };

    if (resp) {
        struct json_object *root = json_tokener_parse(resp);
        struct json_object *items = NULL;
        if (root && json_object_object_get_ex(root, "items", &items) && json_object_is_type(items, json_type_array)) {
            int len = json_object_array_length(items);
            struct tm tm_loc;
            localtime_r(&now, &tm_loc);
            int cur_mins = tm_loc.tm_hour * 60 + tm_loc.tm_min;

            for (int i = 0; i < len && new_cnt < 16; i++) {
                struct json_object *it = json_object_array_get_idx(items, i);
                struct json_object *j_sum = NULL, *j_start = NULL, *j_end = NULL, *j_dt = NULL, *j_loc = NULL;
                const char *sum = (json_object_object_get_ex(it, "summary", &j_sum) && json_object_get_string(j_sum))
                                      ? json_object_get_string(j_sum)
                                      : "";
                if (strstr(sum, "CANCELLED") || strstr(sum, "[TBC]")) continue;
                if (!json_object_object_get_ex(it, "start", &j_start)) continue;
                if (json_object_object_get_ex(j_start, "date", &j_dt)) continue; /* Skip all-day */
                if (!json_object_object_get_ex(j_start, "dateTime", &j_dt) || !json_object_get_string(j_dt)) continue;
                const char *start_iso = json_object_get_string(j_dt);
                const char *end_iso = "";
                if (json_object_object_get_ex(it, "end", &j_end) &&
                    json_object_object_get_ex(j_end, "dateTime", &j_dt) && json_object_get_string(j_dt))
                    end_iso = json_object_get_string(j_dt);

                int sh = 0, sm = 0, eh = 0, em = 0;
                GCalEvent *ev = &new_evs[new_cnt];
                memset(ev, 0, sizeof(*ev));
                snprintf(ev->summary, sizeof(ev->summary), "%s", sum);
                format_iso_12h(start_iso, ev->start_time, sizeof(ev->start_time), &sh, &sm);
                format_iso_12h(end_iso, ev->end_time, sizeof(ev->end_time), &eh, &em);
                ev->minutes_until = (sh * 60 + sm) - cur_mins;

                const char *loc = (json_object_object_get_ex(it, "location", &j_loc) && json_object_get_string(j_loc))
                                      ? json_object_get_string(j_loc)
                                      : "";
                if (strncmp(loc, "US-NYC-9TH-", 11) == 0) loc += 11;
                else if (strncmp(loc, "NYC-", 4) == 0) loc += 4;
                snprintf(ev->location, sizeof(ev->location), "%s", loc);
                snprintf(ev->color, sizeof(ev->color), "%s", colors[new_cnt % 7]);
                new_cnt++;
            }
        }
        if (root) json_object_put(root);
        free(resp);
    }

    pthread_mutex_lock(&g_mutex);
    memcpy(g_gcal_events, new_evs, sizeof(new_evs));
    g_gcal_count = new_cnt;
    g_gcal_fetching = false;
    pthread_mutex_unlock(&g_mutex);
    notify_main_loop();
}

/* ============================================================================
 * Namaz Module (Offline Astronomical Calculation — Moonsighting Committee / Jean Meeus)
 * ============================================================================ */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double namaz_to_rad(double deg) { return deg * M_PI / 180.0; }
static double namaz_to_deg(double rad) { return rad * 180.0 / M_PI; }
static double namaz_normalize_angle(double angle) {
    double a = fmod(angle, 360.0);
    return (a < 0) ? a + 360.0 : a;
}

static double namaz_julian_day(int year, int month, int day) {
    if (month <= 2) { year -= 1; month += 12; }
    int A = (int)(year / 100);
    int B = 2 - A + (int)(A / 4);
    return floor(365.25 * (year + 4716))
         + floor(30.6001 * (month + 1))
         + day + B - 1524.5;
}

static double namaz_julian_century(double jd) {
    return (jd - 2451545.0) / 36525.0;
}

static double namaz_mean_solar_longitude(double T) {
    return namaz_normalize_angle(280.4664567 + 36000.76983*T + 0.0003032*T*T);
}

static double namaz_mean_solar_anomaly(double T) {
    return namaz_normalize_angle(357.52911 + 35999.05029*T - 0.0001537*T*T);
}

static double namaz_solar_equation_of_center(double T, double M_deg) {
    double M = namaz_to_rad(M_deg);
    return (1.914602 - 0.004817*T - 0.000014*T*T) * sin(M)
         + (0.019993 - 0.000101*T) * sin(2*M)
         +  0.000289                * sin(3*M);
}

static double namaz_apparent_solar_longitude(double T, double L0) {
    double M  = namaz_mean_solar_anomaly(T);
    double lon = L0 + namaz_solar_equation_of_center(T, M);
    double Omega = 125.04 - 1934.136*T;
    double Lambda = lon - 0.00569 - 0.00478*sin(namaz_to_rad(Omega));
    return namaz_normalize_angle(Lambda);
}

static double namaz_mean_obliquity(double T) {
    return 23.439291111 - 0.013004167*T - 0.0000001639*T*T + 0.0000005036*T*T*T;
}

static double namaz_apparent_obliquity(double T, double eps0) {
    double Omega = 125.04 - 1934.136*T;
    return eps0 + 0.00256*cos(namaz_to_rad(Omega));
}

static double namaz_solar_right_ascension(double T, double apparent_lon, double eps) {
    (void)T;
    double lam = namaz_to_rad(apparent_lon);
    double e   = namaz_to_rad(eps);
    double alpha = namaz_to_deg(atan2(cos(e)*sin(lam), cos(lam)));
    return namaz_normalize_angle(alpha);
}

static double namaz_solar_declination(double T, double apparent_lon, double eps) {
    (void)T;
    double lam = namaz_to_rad(apparent_lon);
    double e   = namaz_to_rad(eps);
    return namaz_to_deg(asin(sin(e)*sin(lam)));
}

static double namaz_equation_of_time(double T, double L0, double eps0, double M_deg) {
    double e = 0.016708634 - 0.000042037*T - 0.0000001267*T*T;
    double eps = namaz_to_rad(eps0 + 0.00256*cos(namaz_to_rad(125.04 - 1934.136*T)));
    double L   = namaz_to_rad(L0);
    double M   = namaz_to_rad(M_deg);
    double y   = tan(eps/2.0) * tan(eps/2.0);

    double eot = y*sin(2*L)
               - 2*e*sin(M)
               + 4*e*y*sin(M)*cos(2*L)
               - 0.5*y*y*sin(4*L)
               - 1.25*e*e*sin(2*M);
    return namaz_to_deg(eot) * 4.0;
}

static double namaz_hour_angle_for_altitude(double altitude_deg, double lat_deg, double decl_deg) {
    double alt  = namaz_to_rad(altitude_deg);
    double lat  = namaz_to_rad(lat_deg);
    double decl = namaz_to_rad(decl_deg);

    double num = sin(alt) - sin(lat)*sin(decl);
    double den = cos(lat)*cos(decl);
    if (fabs(den) < 1e-10) return NAN;

    double val = num / den;
    if (val < -1.0 || val > 1.0) return NAN;

    return namaz_to_deg(acos(val));
}

typedef struct {
    double transit;
    double sunrise;
    double sunset;
    double declination;
    double right_ascension;
} NamazSolarTime;

static NamazSolarTime namaz_make_solar_time(double jd, double lat, double lon) {
    double T   = namaz_julian_century(jd);
    double L0  = namaz_mean_solar_longitude(T);
    double M   = namaz_mean_solar_anomaly(T);
    double eps0= namaz_mean_obliquity(T);
    double eps = namaz_apparent_obliquity(T, eps0);
    double lam = namaz_apparent_solar_longitude(T, L0);
    double decl= namaz_solar_declination(T, lam, eps);
    double ra  = namaz_solar_right_ascension(T, lam, eps);
    double eot = namaz_equation_of_time(T, L0, eps0, M);

    double transit_ut = 12.0 - eot/60.0 - lon/15.0;
    double sunrise_angle = -0.8333;
    double H = namaz_hour_angle_for_altitude(sunrise_angle, lat, decl);

    NamazSolarTime st;
    st.declination = decl;
    st.right_ascension = ra;
    st.transit = transit_ut;

    if (isnan(H)) {
        st.sunrise = NAN;
        st.sunset  = NAN;
    } else {
        double H_hours = H / 15.0;
        st.sunrise = transit_ut - H_hours;
        st.sunset  = transit_ut + H_hours;
    }
    return st;
}

static double namaz_time_for_solar_angle(double angle_deg, double lat, double decl,
                                         double transit_ut, int after_transit) {
    double H = namaz_hour_angle_for_altitude(angle_deg, lat, decl);
    if (isnan(H)) return NAN;
    double H_hours = H / 15.0;
    return after_transit ? transit_ut + H_hours : transit_ut - H_hours;
}

static double namaz_asr_time(int shadow_factor, double lat, double decl, double transit_ut) {
    double target_angle = namaz_to_deg(atan(1.0 / (shadow_factor + tan(fabs(namaz_to_rad(decl - lat))))));
    return namaz_time_for_solar_angle(target_angle, lat, decl, transit_ut, 1);
}

static double namaz_days_since_solstice(int year, int month, int day, double lat) {
    static const int days_in_month[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    int leap = ((year%4==0 && year%100!=0) || year%400==0) ? 1 : 0;
    int doy = day;
    for (int m = 1; m < month; m++) {
        doy += days_in_month[m];
        if (m == 2) doy += leap;
    }
    int total_days = 365 + leap;
    int solstice_doy = (lat >= 0) ? 355 : 172;
    double dyy = (double)(doy - solstice_doy);
    if (dyy < 0) dyy += total_days;
    return dyy;
}

static double namaz_moonsighting_adjustment(double a, double b, double c, double d, double dyy) {
    double res;
    if      (dyy <  91) res = a + (b - a) / 91.0  * dyy;
    else if (dyy < 137) res = b + (c - b) / 46.0  * (dyy -  91);
    else if (dyy < 183) res = c + (d - c) / 46.0  * (dyy - 137);
    else if (dyy < 229) res = d + (c - d) / 46.0  * (dyy - 183);
    else if (dyy < 275) res = c + (b - c) / 46.0  * (dyy - 229);
    else                res = b + (a - b) / 91.0  * (dyy - 275);
    return floor(res + 0.5);
}

static double namaz_moonsighting_fajr_minutes(double lat, int year, int month, int day) {
    double dyy = namaz_days_since_solstice(year, month, day, lat);
    double ab  = fabs(lat);
    return namaz_moonsighting_adjustment(
        75 + 28.65 / 55.0 * ab,
        75 + 19.44 / 55.0 * ab,
        75 + 32.74 / 55.0 * ab,
        75 + 48.10 / 55.0 * ab,
        dyy
    );
}

static double namaz_moonsighting_isha_minutes(double lat, int year, int month, int day) {
    double dyy = namaz_days_since_solstice(year, month, day, lat);
    double ab  = fabs(lat);
    return namaz_moonsighting_adjustment(
        75 + 25.60 / 55.0 * ab,
        75 +  2.05 / 55.0 * ab,
        75 -  9.21 / 55.0 * ab,
        75 +  6.14 / 55.0 * ab,
        dyy
    );
}

typedef struct {
    double fajr;
    double sunrise;
    double dhuhr;
    double asr;
    double maghrib;
    double isha;
} NamazPrayerTimes;

static NamazPrayerTimes namaz_calculate(int year, int month, int day, double lat, double lon, double utc_offset) {
    double jd = namaz_julian_day(year, month, day) - utc_offset / 24.0;
    NamazSolarTime st = namaz_make_solar_time(jd, lat, lon);
    double dhuhr = st.transit;
    double sunrise = st.sunrise;
    double sunset  = st.sunset;
    double maghrib = sunset;
    double asr = namaz_asr_time(1, lat, st.declination, st.transit);
    double fajr_mins = namaz_moonsighting_fajr_minutes(lat, year, month, day);
    double fajr = sunrise - fajr_mins / 60.0;
    double isha_mins = namaz_moonsighting_isha_minutes(lat, year, month, day);
    double isha = sunset + isha_mins / 60.0;

    NamazPrayerTimes pt = { fajr, sunrise, dhuhr, asr, maghrib, isha };
    return pt;
}

static void namaz_format_hm(double hours_ut, double utc_offset, char *out, size_t sz) {
    if (isnan(hours_ut)) {
        snprintf(out, sz, "--:--");
        return;
    }
    double local = hours_ut + utc_offset;
    local = fmod(local, 24.0);
    if (local < 0) local += 24.0;
    int h = (int)local;
    int m = (int)((local - h) * 60.0 + 0.5);
    if (m == 60) { m = 0; h = (h + 1) % 24; }
    snprintf(out, sz, "%02d:%02d", h, m);
}

static void update_namaz_astronomical(void) {
    time_t now = time(NULL);
    struct tm tm_loc;
    localtime_r(&now, &tm_loc);
    double utc_offset = (double)tm_loc.tm_gmtoff / 3600.0;
    int y = tm_loc.tm_year + 1900;
    int m = tm_loc.tm_mon + 1;
    int d = tm_loc.tm_mday;

    NamazPrayerTimes pt = namaz_calculate(y, m, d, g_namaz_lat, g_namaz_lon, utc_offset);

    pthread_mutex_lock(&g_mutex);
    namaz_format_hm(pt.fajr, utc_offset, g_namaz_today[0], sizeof(g_namaz_today[0]));
    namaz_format_hm(pt.sunrise, utc_offset, g_namaz_today[1], sizeof(g_namaz_today[1]));
    namaz_format_hm(pt.dhuhr, utc_offset, g_namaz_today[2], sizeof(g_namaz_today[2]));
    namaz_format_hm(pt.asr, utc_offset, g_namaz_today[3], sizeof(g_namaz_today[3]));
    namaz_format_hm(pt.maghrib, utc_offset, g_namaz_today[4], sizeof(g_namaz_today[4]));
    namaz_format_hm(pt.isha, utc_offset, g_namaz_today[5], sizeof(g_namaz_today[5]));

    /* Tomorrow Fajr */
    time_t tom = now + 86400;
    struct tm tm_tom;
    localtime_r(&tom, &tm_tom);
    int ty = tm_tom.tm_year + 1900;
    int tm_m = tm_tom.tm_mon + 1;
    int td = tm_tom.tm_mday;
    NamazPrayerTimes pt_tom = namaz_calculate(ty, tm_m, td, g_namaz_lat, g_namaz_lon, utc_offset);
    namaz_format_hm(pt_tom.fajr, utc_offset, g_namaz_tom_fajr, sizeof(g_namaz_tom_fajr));

    g_namaz_loaded = true;
    pthread_mutex_unlock(&g_mutex);
}

static void build_namaz_strings(char *bar_out, size_t bar_sz, char *tip_out, size_t tip_sz) {
    if (!g_namaz_loaded) {
        update_namaz_astronomical();
    }

    pthread_mutex_lock(&g_mutex);
    char full_day[256];
    snprintf(full_day, sizeof(full_day), "%s%s[%s | %s | %s | %s | %s | %s]",
             g_namaz_city[0] ? g_namaz_city : "",
             g_namaz_city[0] ? ": " : "",
             g_namaz_today[0], g_namaz_today[1], g_namaz_today[2],
             g_namaz_today[3], g_namaz_today[4], g_namaz_today[5]);
    snprintf(tip_out, tip_sz, "%s", full_day);

    if (g_namaz_show_full) {
        snprintf(bar_out, bar_sz, "%s", full_day);
        pthread_mutex_unlock(&g_mutex);
        return;
    }

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int cur_m = tm_now.tm_hour * 60 + tm_now.tm_min;
    int next_m = -1;
    for (int i = 0; i < 6; i++) {
        if (strlen(g_namaz_today[i]) >= 5) {
            int h = atoi(g_namaz_today[i]);
            int m = atoi(g_namaz_today[i] + 3);
            if (h * 60 + m > cur_m) {
                next_m = h * 60 + m;
                break;
            }
        }
    }
    int diff = -1;
    if (next_m != -1) {
        diff = next_m - cur_m;
    } else if (strlen(g_namaz_tom_fajr) >= 5) {
        int h = atoi(g_namaz_tom_fajr);
        int m = atoi(g_namaz_tom_fajr + 3);
        diff = (1440 - cur_m) + (h * 60 + m);
    }
    if (diff >= 0) {
        snprintf(bar_out, bar_sz, "%dsa%ddk", diff / 60, diff % 60);
    } else {
        snprintf(bar_out, bar_sz, "Calculating...");
    }
    pthread_mutex_unlock(&g_mutex);
}

/* ============================================================================
 * Audio (wpctl / pactl), MPRIS, Battery (/sys/class/power_supply), Screensaver
 * ============================================================================ */
static void poll_pulseaudio(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "XDG_RUNTIME_DIR=%s wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", g_runtime_dir);
    char *out = run_cmd_capture(cmd, 256);
    if (out && strncmp(out, "Volume:", 7) == 0) {
        double v = atof(out + 7);
        int vol = (int)round(v * 100.0);
        bool muted = (strstr(out, "[MUTED]") != NULL);
        pthread_mutex_lock(&g_mutex);
        g_pulse_vol = vol;
        g_pulse_muted = muted;
        pthread_mutex_unlock(&g_mutex);
    }
    free(out);
}

static void pulse_adjust_volume(int delta_pct) {
    if (delta_pct == 0) return;

    pthread_mutex_lock(&g_mutex);
    g_pulse_vol += delta_pct;
    if (g_pulse_vol < 0) g_pulse_vol = 0;
    if (g_pulse_vol > 150) g_pulse_vol = 150;
    pthread_mutex_unlock(&g_mutex);

    char cmd[256];
    if (delta_pct > 0) {
        snprintf(cmd, sizeof(cmd),
                 "XDG_RUNTIME_DIR=%s wpctl set-volume -l 1.5 @DEFAULT_AUDIO_SINK@ %d%%+ >/dev/null 2>&1",
                 g_runtime_dir, delta_pct);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "XDG_RUNTIME_DIR=%s wpctl set-volume -l 1.5 @DEFAULT_AUDIO_SINK@ %d%%- >/dev/null 2>&1",
                 g_runtime_dir, -delta_pct);
    }
    spawn_detached(cmd);
}

static void pulse_toggle_mute(void) {
    pthread_mutex_lock(&g_mutex);
    g_pulse_muted = !g_pulse_muted;
    pthread_mutex_unlock(&g_mutex);

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "XDG_RUNTIME_DIR=%s wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle >/dev/null 2>&1",
             g_runtime_dir);
    spawn_detached(cmd);
}

static void poll_screensaver(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "XDG_RUNTIME_DIR=%s DBUS_SESSION_BUS_ADDRESS=unix:path=%s/bus "
             "systemctl --user is-active --quiet swayidle.service",
             g_runtime_dir, g_runtime_dir);
    int ret = system(cmd);
    pthread_mutex_lock(&g_mutex);
    g_screensaver_active = (ret == 0);
    pthread_mutex_unlock(&g_mutex);
}

static void toggle_screensaver(void) {
    bool was_active;
    pthread_mutex_lock(&g_mutex);
    was_active = g_screensaver_active;
    g_screensaver_active = !was_active;
    pthread_mutex_unlock(&g_mutex);

    char cmd[256];
    if (was_active) {
        snprintf(cmd, sizeof(cmd),
                 "XDG_RUNTIME_DIR=%s DBUS_SESSION_BUS_ADDRESS=unix:path=%s/bus "
                 "systemctl --user stop --quiet swayidle.service >/dev/null 2>&1",
                 g_runtime_dir, g_runtime_dir);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "XDG_RUNTIME_DIR=%s DBUS_SESSION_BUS_ADDRESS=unix:path=%s/bus "
                 "systemctl --user start --quiet swayidle.service >/dev/null 2>&1",
                 g_runtime_dir, g_runtime_dir);
    }
    int r = system(cmd);
    (void)r;
    poll_screensaver();
}

static long read_sysfs_long(const char *dir, const char *file) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static void poll_battery(void) {
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return;
    struct dirent *de;
    char bat_dir[512] = "";
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "BAT", 3) == 0) {
            snprintf(bat_dir, sizeof(bat_dir), "/sys/class/power_supply/%s", de->d_name);
            break;
        }
    }
    closedir(d);

    if (!bat_dir[0]) {
        pthread_mutex_lock(&g_mutex);
        g_bat_present = false;
        pthread_mutex_unlock(&g_mutex);
        return;
    }

    long cap = read_sysfs_long(bat_dir, "capacity");
    if (cap < 0) cap = 100;
    if (cap > 100) cap = 100;

    char status[32] = "Discharging";
    char spath[512];
    snprintf(spath, sizeof(spath), "%s/status", bat_dir);
    FILE *sf = fopen(spath, "r");
    if (sf) {
        if (fgets(status, sizeof(status), sf)) {
            char *nl = strchr(status, '\n');
            if (nl) *nl = '\0';
        }
        fclose(sf);
    }

    long e_now = read_sysfs_long(bat_dir, "energy_now");
    if (e_now < 0) e_now = read_sysfs_long(bat_dir, "charge_now");
    long e_full = read_sysfs_long(bat_dir, "energy_full");
    if (e_full < 0) e_full = read_sysfs_long(bat_dir, "charge_full");
    long p_now = read_sysfs_long(bat_dir, "power_now");
    if (p_now < 0) p_now = read_sysfs_long(bat_dir, "current_now");

    char tstr[32] = "";
    if (p_now > 0 && e_now >= 0) {
        double hrs = 0;
        if (strcmp(status, "Charging") == 0 && e_full > e_now)
            hrs = (double)(e_full - e_now) / (double)p_now;
        else
            hrs = (double)e_now / (double)p_now;
        int total_m = (int)(hrs * 60.0);
        snprintf(tstr, sizeof(tstr), "%d:%02d", total_m / 60, total_m % 60);
    }

    pthread_mutex_lock(&g_mutex);
    g_bat_present = true;
    g_bat_capacity = (int)cap;
    snprintf(g_bat_status, sizeof(g_bat_status), "%s", status);
    snprintf(g_bat_time_str, sizeof(g_bat_time_str), "%s", tstr);
    if (cap <= 5 && strcmp(status, "Charging") != 0 && !g_bat_notified_crit) {
        g_bat_notified_crit = true;
        spawn_detached("notify-send -u critical 'Low Battery' 'Battery is below 5%!'");
    } else if (cap > 5) {
        g_bat_notified_crit = false;
    }
    pthread_mutex_unlock(&g_mutex);
}

static char g_mpris_dest[128] = "";
static time_t g_mpris_lock_until = 0;

static void poll_mpris(void) {
    if (time(NULL) < g_mpris_lock_until) return;

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "DBUS_SESSION_BUS_ADDRESS=unix:path=%s/bus playerctl -a status 2>/dev/null",
             g_runtime_dir);
    char *pst = run_cmd_capture(cmd, 512);
    if (pst && (strstr(pst, "Playing") || strstr(pst, "Paused"))) {
        int state = strstr(pst, "Playing") ? 1 : 2;
        free(pst);

        snprintf(cmd, sizeof(cmd),
                 "DBUS_SESSION_BUS_ADDRESS=unix:path=%s/bus playerctl metadata --format '{{artist}} - {{title}}' 2>/dev/null",
                 g_runtime_dir);
        char *meta = run_cmd_capture(cmd, 256);
        if (meta) {
            char *nl = strchr(meta, '\n');
            if (nl) *nl = '\0';
        }
        pthread_mutex_lock(&g_mutex);
        if (time(NULL) >= g_mpris_lock_until) {
            g_mpris_state = state;
            if (meta && *meta && strcmp(meta, " - ") != 0) {
                snprintf(g_mpris_tooltip, sizeof(g_mpris_tooltip), "%s", meta);
            } else {
                snprintf(g_mpris_tooltip, sizeof(g_mpris_tooltip), "%s", state == 1 ? "Playing" : "Paused");
            }
        }
        pthread_mutex_unlock(&g_mutex);
        free(meta);
        return;
    }
    free(pst);

    /* Fallback: inspect D-Bus org.mpris.MediaPlayer2.* directly */
    snprintf(cmd, sizeof(cmd),
             "DBUS_SESSION_BUS_ADDRESS=unix:path=%s/bus gdbus call --session "
             "--dest org.freedesktop.DBus --object-path /org/freedesktop/DBus "
             "--method org.freedesktop.DBus.ListNames 2>/dev/null",
             g_runtime_dir);
    char *names = run_cmd_capture(cmd, 16384);
    if (!names) return;

    const char *p = names;
    int best_state = 0;
    char best_dest[128] = "";
    while ((p = strstr(p, "org.mpris.MediaPlayer2.")) != NULL) {
        char dest[128] = "";
        int idx = 0;
        while (p[idx] && p[idx] != '\'' && p[idx] != '"' && idx < 120) {
            dest[idx] = p[idx];
            idx++;
        }
        dest[idx] = '\0';
        p += idx;
        if (strcmp(dest, "org.mpris.MediaPlayer2.playerctld") == 0) continue;

        snprintf(cmd, sizeof(cmd),
                 "DBUS_SESSION_BUS_ADDRESS=unix:path=%s/bus gdbus call --session "
                 "--dest '%s' --object-path /org/mpris/MediaPlayer2 "
                 "--method org.freedesktop.DBus.Properties.Get "
                 "org.mpris.MediaPlayer2.Player PlaybackStatus 2>/dev/null",
                 g_runtime_dir, dest);
        char *st = run_cmd_capture(cmd, 512);
        if (st) {
            if (strstr(st, "Playing")) {
                best_state = 1;
                snprintf(best_dest, sizeof(best_dest), "%s", dest);
                free(st);
                break;
            } else if (strstr(st, "Paused") && best_state == 0) {
                best_state = 2;
                snprintf(best_dest, sizeof(best_dest), "%s", dest);
            }
            free(st);
        }
    }
    free(names);

    pthread_mutex_lock(&g_mutex);
    if (time(NULL) >= g_mpris_lock_until) {
        g_mpris_state = best_state;
        snprintf(g_mpris_dest, sizeof(g_mpris_dest), "%s", best_dest);
        if (best_state > 0 && best_dest[0]) {
            snprintf(g_mpris_tooltip, sizeof(g_mpris_tooltip), "%s", best_dest + 23);
        }
    }
    pthread_mutex_unlock(&g_mutex);
}

static void mpris_action(const char *method) {
    char cmd[1024];
    if (strcmp(method, "PlayPause") == 0) {
        int prev_state;
        pthread_mutex_lock(&g_mutex);
        prev_state = g_mpris_state;
        g_mpris_state = (prev_state == 1) ? 2 : 1;
        g_mpris_lock_until = time(NULL) + 2;
        pthread_mutex_unlock(&g_mutex);

        if (prev_state == 1) {
            /* Currently Playing -> Pause all active players so state reliably becomes Paused (▶) */
            snprintf(cmd, sizeof(cmd),
                     "DBUS_SESSION_BUS_ADDRESS=unix:path=%s/bus playerctl -a pause >/dev/null 2>&1; "
                     "%s%s%s",
                     g_runtime_dir,
                     g_mpris_dest[0] ? "DBUS_SESSION_BUS_ADDRESS=unix:path=" : "",
                     g_mpris_dest[0] ? g_runtime_dir : "",
                     g_mpris_dest[0] ? "/bus gdbus call --session --dest '" : "");
            if (g_mpris_dest[0]) {
                snprintf(cmd, sizeof(cmd),
                         "DBUS_SESSION_BUS_ADDRESS=unix:path=%s/bus playerctl -a pause >/dev/null 2>&1 || "
                         "DBUS_SESSION_BUS_ADDRESS=unix:path=%s/bus gdbus call --session --dest '%s' "
                         "--object-path /org/mpris/MediaPlayer2 --method org.mpris.MediaPlayer2.Player.Pause >/dev/null 2>&1",
                         g_runtime_dir, g_runtime_dir, g_mpris_dest);
            }
        } else {
            /* Currently Paused -> Play */
            if (g_mpris_dest[0]) {
                snprintf(cmd, sizeof(cmd),
                         "DBUS_SESSION_BUS_ADDRESS=unix:path=%s/bus playerctl play >/dev/null 2>&1 || "
                         "DBUS_SESSION_BUS_ADDRESS=unix:path=%s/bus gdbus call --session --dest '%s' "
                         "--object-path /org/mpris/MediaPlayer2 --method org.mpris.MediaPlayer2.Player.Play >/dev/null 2>&1",
                         g_runtime_dir, g_runtime_dir, g_mpris_dest);
            } else {
                snprintf(cmd, sizeof(cmd),
                         "DBUS_SESSION_BUS_ADDRESS=unix:path=%s/bus playerctl play >/dev/null 2>&1",
                         g_runtime_dir);
            }
        }
        spawn_detached(cmd);
        return;
    }

    const char *pctl_cmd = (strcmp(method, "Previous") == 0) ? "previous" : "next";
    snprintf(cmd, sizeof(cmd),
             "DBUS_SESSION_BUS_ADDRESS=unix:path=%s/bus playerctl %s >/dev/null 2>&1",
             g_runtime_dir, pctl_cmd);
    spawn_detached(cmd);
}

static void *audio_event_thread(void *arg) {
    (void)arg;
    poll_pulseaudio();
    notify_main_loop();

    while (1) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "XDG_RUNTIME_DIR=%s stdbuf -oL pw-mon --hide-props --hide-params 2>/dev/null",
                 g_runtime_dir);
        FILE *fp = popen(cmd, "r");
        if (!fp) {
            sleep(5);
            continue;
        }
        int pfd_num = fileno(fp);
        struct pollfd pfd = {.fd = pfd_num, .events = POLLIN};
        char drain_buf[4096];

        /* Drain initial pw-mon registry dump (first 150ms) */
        while (poll(&pfd, 1, 150) > 0) {
            if (read(pfd_num, drain_buf, sizeof(drain_buf)) <= 0) break;
        }

        /* Block until PipeWire pushes an audio change event (ignoring pure Client connections) */
        while (poll(&pfd, 1, -1) > 0) {
            ssize_t n = read(pfd_num, drain_buf, sizeof(drain_buf) - 1);
            if (n <= 0) break;
            drain_buf[n] = '\0';

            bool has_audio_change = (strstr(drain_buf, "changed:") != NULL ||
                                     strstr(drain_buf, "Interface:Node") != NULL ||
                                     strstr(drain_buf, "Interface:Device") != NULL);

            /* Coalesce event burst over 50ms so wpctl/swayosd finishes applying volume */
            while (poll(&pfd, 1, 50) > 0) {
                ssize_t m = read(pfd_num, drain_buf, sizeof(drain_buf) - 1);
                if (m <= 0) break;
                drain_buf[m] = '\0';
                if (strstr(drain_buf, "changed:") ||
                    strstr(drain_buf, "Interface:Node") ||
                    strstr(drain_buf, "Interface:Device")) {
                    has_audio_change = true;
                }
            }

            if (has_audio_change) {
                poll_pulseaudio();
                notify_main_loop();
            }
        }
        pclose(fp);
        sleep(2);
    }
    return NULL;
}

static void *battery_uevent_thread(void *arg) {
    (void)arg;
    poll_battery();
    notify_main_loop();

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return NULL;

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1; /* Kernel uevent multicast group */
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return NULL;
    }

    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    char buf[4096];

    while (1) {
        if (poll(&pfd, 1, -1) > 0) {
            ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
            if (n <= 0) continue;
            buf[n] = '\0';
            if (memmem(buf, (size_t)n, "power_supply", 12) != NULL ||
                memmem(buf, (size_t)n, "SUBSYSTEM=power_supply", 22) != NULL) {
                /* AC adapter connected/disconnected or battery charging state change */
                poll_battery();
                notify_main_loop();
            }
        }
    }
    close(fd);
    return NULL;
}

static void *background_worker_thread(void *arg) {
    (void)arg;
    load_gcal_credentials();
    fetch_gcal_events();
    notify_main_loop();

    while (1) {
        sleep(300); /* Refresh remote Google Calendar every 5 minutes */
        fetch_gcal_events();
        notify_main_loop();
    }
    return NULL;
}

/* ============================================================================
 * Bar & Tooltip Rendering (Dynamic Space Calculation + Clock Left-Shift)
 * ============================================================================ */
static void truncate_utf8_to_px(char *str, size_t buf_sz, int max_px, int font_px, bool is_bold) {
    if (max_px <= 16) {
        str[0] = '\0';
        return;
    }
    if (measure_markup_line(str, font_px, is_bold) <= max_px) return;

    size_t len = strlen(str);
    while (len > 0) {
        /* Step back one UTF-8 character */
        do {
            len--;
        } while (len > 0 && ((unsigned char)str[len] & 0xC0) == 0x80);

        if (len + 4 < buf_sz) {
            memcpy(str + len, "\xE2\x80\xA6", 4); /* "…" + NUL */
        } else {
            str[len] = '\0';
        }
        if (measure_markup_line(str, font_px, is_bold) <= max_px) return;
    }
    str[0] = '\0';
}

static void render_bar(SurfaceBuf *s) {
    /* Clear background to rgba(74, 62, 53, 0.8) and draw 1px bottom border #7d6b5e */
    uint32_t bg_pm = premul_argb(COL_BAR_BG);
    for (int y = 0; y < BAR_HEIGHT - 1; y++) {
        for (int x = 0; x < s->w; x++) s->px[y * s->w + x] = bg_pm;
    }
    for (int x = 0; x < s->w; x++) s->px[(BAR_HEIGHT - 1) * s->w + x] = COL_BAR_BORDER;

    for (int i = 0; i < MOD_COUNT; i++) g_mod_boxes[i].visible = false;

    int baseline_y = 18;
    int left_x = 0;

    pthread_mutex_lock(&g_mutex);

    /* 1. LEFT: sway/workspaces */
    int ws_start = left_x;
    for (int i = 0; i < g_ws_count; i++) {
        if (i > 0) {
            left_x += 4;
            draw_separator(s, left_x);
            left_x += 5;
        }
        int tw = measure_markup_line(g_workspaces[i].name, 16, false);
        int bw = tw + 10; /* padding: 0 5px */
        g_workspaces[i].x1 = left_x;
        g_workspaces[i].x2 = left_x + bw;
        if (g_workspaces[i].focused) {
            draw_rounded_box(s, left_x, 1, bw, BAR_HEIGHT - 3, 3, COL_WS_FOC_BG, COL_WS_FOC_BDR);
            render_markup_line(s, g_workspaces[i].name, left_x + 5, baseline_y, 16, COL_TEXT, false, true);
        } else {
            uint32_t col = g_workspaces[i].urgent ? COL_URGENT : COL_WS_IDLE;
            render_markup_line(s, g_workspaces[i].name, left_x + 5, baseline_y, 16, col, false, true);
        }
        left_x += bw;
    }
    if (g_ws_count > 0) {
        g_mod_boxes[MOD_WORKSPACES].x1 = ws_start;
        g_mod_boxes[MOD_WORKSPACES].x2 = left_x;
        g_mod_boxes[MOD_WORKSPACES].visible = true;
        g_mod_boxes[MOD_WORKSPACES].tooltip[0] = '\0';
    }

    /* 2. LEFT: sway/mode (e.g., "⇸" passthrough or "topleft"/"bottomright"/"move") */
    if (g_sway_mode[0] != '\0') {
        if (left_x > 0) {
            left_x += 4;
            draw_separator(s, left_x);
            left_x += 5;
        }
        int tw = measure_markup_line(g_sway_mode, 16, false);
        g_mod_boxes[MOD_MODE].x1 = left_x;
        render_markup_line(s, g_sway_mode, left_x + 4, baseline_y, 16, COL_TEXT, false, true);
        left_x += tw + 8;
        g_mod_boxes[MOD_MODE].x2 = left_x;
        g_mod_boxes[MOD_MODE].visible = true;
        snprintf(g_mod_boxes[MOD_MODE].tooltip, sizeof(g_mod_boxes[0].tooltip), "Mode: %s", g_sway_mode);
    }

    /* 3. LEFT: sway/scratchpad */
    if (g_scratch_count > 0) {
        if (left_x > 0) {
            left_x += 4;
            draw_separator(s, left_x);
            left_x += 5;
        }
        char sc_txt[64];
        snprintf(sc_txt, sizeof(sc_txt), "⌫ %d", g_scratch_count);
        int tw = measure_markup_line(sc_txt, 16, false);
        g_mod_boxes[MOD_SCRATCHPAD].x1 = left_x;
        render_markup_line(s, sc_txt, left_x + 4, baseline_y, 16, COL_TEXT, false, true);
        left_x += tw + 8;
        g_mod_boxes[MOD_SCRATCHPAD].x2 = left_x;
        g_mod_boxes[MOD_SCRATCHPAD].visible = true;
        snprintf(g_mod_boxes[MOD_SCRATCHPAD].tooltip, sizeof(g_mod_boxes[0].tooltip), "%s", g_scratch_tooltip);
    }

    /* 3. COMPACT RIGHT STATUS MODULES (laid out right-to-left first so we know exact remaining width) */
    int right_x = s->w - 4;

    /* 3.6 tray */
    if (g_tray_count > 0) {
        int tray_x2 = right_x;
        for (int i = g_tray_count - 1; i >= 0; i--) {
            right_x -= 16;
            g_tray_items[i].x1 = right_x;
            g_tray_items[i].x2 = right_x + 16;
            if (g_tray_items[i].has_pixels) {
                for (int r = 0; r < 16; r++) {
                    for (int c = 0; c < 16; c++) {
                        blend_pixel(s, right_x + c, 5 + r, g_tray_items[i].pixels[r * 16 + c]);
                    }
                }
            } else {
                draw_rounded_box(s, right_x, 5, 16, 16, 3, COL_WS_FOC_BG, COL_BAR_BORDER);
            }
            if (i > 0) right_x -= 8;
        }
        right_x -= 4;
        g_mod_boxes[MOD_TRAY].x1 = right_x;
        g_mod_boxes[MOD_TRAY].x2 = tray_x2;
        g_mod_boxes[MOD_TRAY].visible = true;
        snprintf(g_mod_boxes[MOD_TRAY].tooltip, sizeof(g_mod_boxes[0].tooltip), "System Tray");
    }

    /* Helper macro to draw a right-side module with right border separator */
#define DRAW_RIGHT_MOD(mod_id, txt_str, fg_col, is_bold, bg_box_col, tip_str)          \
    do {                                                                               \
        int _tw = measure_markup_line((txt_str), 16, (is_bold));                       \
        int _x2 = right_x;                                                             \
        int _sep_x = right_x - 8;                                                      \
        int _tx = _sep_x - 8 - _tw;                                                    \
        int _x1 = _tx - 4;                                                             \
        if ((bg_box_col) != 0) {                                                       \
            fill_rect(s, _x1, 0, _sep_x - _x1, BAR_HEIGHT - 1, (bg_box_col));          \
        }                                                                              \
        draw_separator(s, _sep_x);                                                     \
        render_markup_line(s, (txt_str), _tx, baseline_y, 16, (fg_col), (is_bold), true); \
        g_mod_boxes[(mod_id)].x1 = _x1;                                                \
        g_mod_boxes[(mod_id)].x2 = _x2;                                                \
        g_mod_boxes[(mod_id)].visible = true;                                          \
        snprintf(g_mod_boxes[(mod_id)].tooltip, sizeof(g_mod_boxes[0].tooltip), "%s", (tip_str)); \
        right_x = _x1;                                                                 \
    } while (0)

    /* 3.5 screensaver */
    const char *ss_icon = g_screensaver_active ? "💤" : "👁";
    uint32_t ss_col = g_screensaver_active ? COL_WS_IDLE : COL_TEXT;
    const char *ss_tip = g_screensaver_active
                             ? "Screensaver Active (Display will sleep)"
                             : "Screensaver Inhibited (Display stays on)";
    DRAW_RIGHT_MOD(MOD_SCREENSAVER, ss_icon, ss_col, false, 0, ss_tip);

    /* 3.4 battery */
    if (g_bat_present) {
        static const char *bat_icons[11] = {"▁", "⡀", "⣀", "⣄", "⣤", "⣦", "⣶", "⣷", "⣾", "⣿", "🔋"};
        const char *b_icon = bat_icons[(g_bat_capacity * 10) / 100];
        if (strcmp(g_bat_status, "Charging") == 0) b_icon = "⚡";
        else if (strcmp(g_bat_status, "Full") == 0 || strcmp(g_bat_status, "Not charging") == 0) b_icon = "🔌";

        uint32_t b_fg = COL_TEXT;
        uint32_t b_bg = 0;
        if (g_bat_capacity <= 5 && strcmp(g_bat_status, "Charging") != 0) {
            if (g_bat_blink_on) {
                b_bg = COL_URGENT;
                b_fg = COL_TEXT;
            } else {
                b_fg = COL_URGENT;
            }
        } else if (g_bat_capacity <= 15) {
            b_fg = COL_WARNING;
        }
        char b_tip[128];
        if (g_bat_time_str[0]) snprintf(b_tip, sizeof(b_tip), "%d%% (%s)", g_bat_capacity, g_bat_time_str);
        else snprintf(b_tip, sizeof(b_tip), "%d%%", g_bat_capacity);
        DRAW_RIGHT_MOD(MOD_BATTERY, b_icon, b_fg, false, b_bg, b_tip);
    }

    /* 3.3 namaz */
    pthread_mutex_unlock(&g_mutex);
    char nmz_bar[256], nmz_tip[256];
    build_namaz_strings(nmz_bar, sizeof(nmz_bar), nmz_tip, sizeof(nmz_tip));
    pthread_mutex_lock(&g_mutex);
    DRAW_RIGHT_MOD(MOD_NAMAZ, nmz_bar, COL_TEXT, false, 0, nmz_tip);

    /* 3.2 pulseaudio */
    const char *pa_icon = "🔊";
    if (g_pulse_muted || g_pulse_vol <= 0) pa_icon = "🔇";
    else if (g_pulse_vol <= 50) pa_icon = "🔈";
    char pa_tip[64];
    snprintf(pa_tip, sizeof(pa_tip), "%d%%", g_pulse_vol);
    DRAW_RIGHT_MOD(MOD_PULSE, pa_icon, COL_TEXT, false, 0, pa_tip);

    /* 3.1 mpris */
    if (g_mpris_state > 0) {
        const char *mp_icon = (g_mpris_state == 1) ? "⏸" : "▶";
        DRAW_RIGHT_MOD(MOD_MPRIS, mp_icon, COL_TEXT, false, 0, g_mpris_tooltip);
    }

    /* 4. DYNAMIC CENTER + RIGHT LAYOUT FOR CLOCK, GOOGLE CALENDAR, AND QUIET NOTIFICATIONS */
    char clk_bar[128], clk_tip[8192];
    build_clock_strings(clk_bar, sizeof(clk_bar), clk_tip, sizeof(clk_tip));
    int clk_w = measure_markup_line(clk_bar, 16, false);

    /* Prepare Google Calendar strings & natural width */
    bool has_gcal = (g_gcal_count > 0);
    char gc_sum[256] = "", gc_suffix[256] = "", gc_bar[512] = "", gc_tip[4096] = "";
    int gcal_natural_box_w = 0;
    if (has_gcal) {
        GCalEvent *ev = &g_gcal_events[0];
        snprintf(gc_sum, sizeof(gc_sum), "%s", ev->summary);
        if (!g_gcal_show_details) {
            snprintf(gc_suffix, sizeof(gc_suffix), " (%s - %s)", ev->start_time, ev->end_time);
        } else {
            size_t eoff = 0;
            if (ev->location[0]) eoff += snprintf(gc_suffix + eoff, sizeof(gc_suffix) - eoff, " (%s)", ev->location);
            if (ev->minutes_until > 0 && ev->minutes_until <= 180)
                eoff += snprintf(gc_suffix + eoff, sizeof(gc_suffix) - eoff, " <span color='#ff7f7f'>(%dm)</span>", ev->minutes_until);
        }
        snprintf(gc_bar, sizeof(gc_bar), "<span color='%s'>%s</span>%s", ev->color, gc_sum, gc_suffix);
        gcal_natural_box_w = measure_markup_line(gc_bar, 16, false) + 20;

        size_t toff = 0;
        for (int i = 0; i < g_gcal_count; i++) {
            toff += snprintf(gc_tip + toff, sizeof(gc_tip) - toff, "%s• %s (%s - %s)",
                             (i > 0) ? "\n" : "", g_gcal_events[i].summary,
                             g_gcal_events[i].start_time, g_gcal_events[i].end_time);
            if (g_gcal_events[i].location[0])
                toff += snprintf(gc_tip + toff, sizeof(gc_tip) - toff, "\n  Location: %s", g_gcal_events[i].location);
        }
    }

    /* Prepare Quiet notification strings & natural width */
    if (g_quiet_preview_until > 0 && time(NULL) >= g_quiet_preview_until) {
        g_quiet_preview[0] = '\0';
        g_quiet_preview_until = 0;
    }
    bool has_quiet = (g_quiet_count > 0);
    bool q_urg = (g_quiet_urgency == 2);
    char q_bar[512] = "", q_tip[128] = "";
    int quiet_natural_box_w = 0;
    if (has_quiet) {
        if (g_quiet_preview[0]) snprintf(q_bar, sizeof(q_bar), "%s", g_quiet_preview);
        else snprintf(q_bar, sizeof(q_bar), "%u", g_quiet_count);
        snprintf(q_tip, sizeof(q_tip), "%u unread notification(s)", g_quiet_count);
        quiet_natural_box_w = measure_markup_line(q_bar, 16, q_urg) + 20;
    }

    /* Compute clock X position: centered by default, pushed left if gcal + quiet need more space */
    int clk_x = (s->w - clk_w) / 2;
    int right_needed = gcal_natural_box_w + quiet_natural_box_w;
    if (right_x - right_needed < clk_x + clk_w + 16) {
        clk_x = right_x - right_needed - clk_w - 16;
    }
    if (clk_x < left_x + 12) {
        clk_x = left_x + 12;
    }

    /* Draw clock at computed clk_x */
    render_markup_line(s, clk_bar, clk_x, baseline_y, 16, COL_TEXT, false, true);
    g_mod_boxes[MOD_CLOCK].x1 = clk_x - 4;
    g_mod_boxes[MOD_CLOCK].x2 = clk_x + clk_w + 4;
    g_mod_boxes[MOD_CLOCK].visible = true;
    snprintf(g_mod_boxes[MOD_CLOCK].tooltip, sizeof(g_mod_boxes[0].tooltip), "%s", clk_tip);

    /* Now dynamically fit google_calendar and quiet into [min_right_x .. right_x] */
    int min_right_x = clk_x + clk_w + 16;
    int total_avail = right_x - min_right_x;

    if (has_gcal && total_avail > 40) {
        int gcal_max_box = total_avail;
        if (has_quiet) {
            /* Reserve space for quiet: if quiet is showing preview text, split space; if just count, reserve quiet_natural_box_w */
            int reserve_quiet = quiet_natural_box_w;
            if (g_quiet_preview[0] && gcal_natural_box_w + quiet_natural_box_w > total_avail) {
                int half = total_avail / 2;
                reserve_quiet = (quiet_natural_box_w < half) ? quiet_natural_box_w : half;
            }
            if (gcal_natural_box_w > total_avail - reserve_quiet) {
                gcal_max_box = total_avail - reserve_quiet;
            }
        }
        if (gcal_natural_box_w > gcal_max_box) {
            int suffix_w = measure_markup_line(gc_suffix, 16, false);
            int max_sum_px = gcal_max_box - 20 - suffix_w;
            truncate_utf8_to_px(gc_sum, sizeof(gc_sum), max_sum_px, 16, false);
            snprintf(gc_bar, sizeof(gc_bar), "<span color='%s'>%s</span>%s", g_gcal_events[0].color, gc_sum, gc_suffix);
        }
        if (gc_sum[0] || !has_quiet) {
            DRAW_RIGHT_MOD(MOD_GCAL, gc_bar, COL_TEXT, false, 0, gc_tip);
        }
    }

    if (has_quiet) {
        int rem_box_w = right_x - min_right_x;
        int max_txt_px = rem_box_w - 20;
        if (max_txt_px >= 16) {
            truncate_utf8_to_px(q_bar, sizeof(q_bar), max_txt_px, 16, q_urg);
            if (q_bar[0]) {
                DRAW_RIGHT_MOD(MOD_QUIET, q_bar, q_urg ? COL_URGENT : COL_TEXT, q_urg, 0, q_tip);
            }
        }
    }

    pthread_mutex_unlock(&g_mutex);
}

static void measure_tooltip(const char *tip, int *out_w, int *out_h) {
    int max_w = 0;
    int lines = 0;
    const char *p = tip;
    while (*p) {
        const char *nl = strchr(p, '\n');
        char line[1024];
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        int lw = measure_markup_line(line, 15, false);
        if (lw > max_w) max_w = lw;
        lines++;
        if (!nl) break;
        p = nl + 1;
    }
    *out_w = max_w + 16; /* 6px padding + 1px border on each side */
    *out_h = lines * 18 + 14;
}

static void render_tooltip(SurfaceBuf *s, const char *tip) {
    memset(s->px, 0, (size_t)s->w * (size_t)s->h * 4);
    draw_rounded_box(s, 0, 0, s->w, s->h, 3, COL_TIP_BG, COL_TIP_BDR);
    const char *p = tip;
    int y = 20;
    bool is_first_line = true;
    while (*p) {
        const char *nl = strchr(p, '\n');
        char line[1024];
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';

        int draw_x = 8;
        if (is_first_line) {
            int lw = measure_markup_line(line, 15, false);
            if (s->w > lw + 16) {
                draw_x = (s->w - lw) / 2;
            }
            is_first_line = false;
        }

        render_markup_line(s, line, draw_x, y, 15, COL_TEXT, false, true);
        y += 18;
        if (!nl) break;
        p = nl + 1;
    }
}

/* ============================================================================
 * Direct Wayland + zwlr_layer_shell_v1 Wire Protocol Engine
 * ============================================================================ */
static int g_wl_fd = -1;
static uint32_t g_wl_next_id = 2;
static uint32_t g_id_registry = 0;
static uint32_t g_id_compositor = 0;
static uint32_t g_id_shm = 0;
static uint32_t g_id_seat = 0;
static uint32_t g_id_pointer = 0;
static uint32_t g_id_layer_shell = 0;

static uint32_t g_id_bar_surface = 0;
static uint32_t g_id_bar_layer = 0;
static int g_bar_w = 1920;
static int g_bar_h = BAR_HEIGHT;
static bool g_bar_configured = false;

static uint32_t g_id_tip_surface = 0;
static uint32_t g_id_tip_layer = 0;
static uint32_t g_id_empty_region = 0;
static int g_tip_w = 0, g_tip_h = 0, g_tip_x = 0;
static bool g_tip_configured = false;
static char g_active_tip_text[8192] = "";

static void wl_send_msg(uint32_t obj_id, uint16_t opcode, const void *payload, size_t payload_len, int fd_to_pass) {
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

static void wl_attach_rendered_shm(uint32_t surface_id, int w, int h, void (*render_fn)(SurfaceBuf *)) {
    if (w <= 0 || h <= 0) return;
    size_t sz = (size_t)w * (size_t)h * 4;
    int mfd = memfd_create("waybar-shm", MFD_CLOEXEC);
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

    SurfaceBuf s = {.px = px, .w = w, .h = h};
    render_fn(&s);
    munmap(px, sz);

    uint32_t pool_id = g_wl_next_id++;
    uint32_t buf_id = g_wl_next_id++;

    uint32_t cp_args[2] = {pool_id, (uint32_t)sz};
    wl_send_msg(g_id_shm, 0, cp_args, 8, mfd);
    close(mfd);

    uint32_t cb_args[6] = {buf_id, 0, (uint32_t)w, (uint32_t)h, (uint32_t)(w * 4), 0 /* ARGB8888 */};
    wl_send_msg(pool_id, 0, cb_args, 24, -1);

    wl_send_u32_3(surface_id, 1 /* attach */, buf_id, 0, 0);
    wl_send_u32_4(surface_id, 2 /* damage */, 0, 0, (uint32_t)w, (uint32_t)h);
    wl_send_msg(surface_id, 6 /* commit */, NULL, 0, -1);

    wl_send_msg(buf_id, 0 /* destroy buffer on release */, NULL, 0, -1);
    wl_send_msg(pool_id, 1 /* destroy pool */, NULL, 0, -1);
}

static void render_tip_cb(SurfaceBuf *s) {
    render_tooltip(s, g_active_tip_text);
}

static void destroy_tooltip_surface(void) {
    if (g_id_tip_layer) {
        wl_send_msg(g_id_tip_layer, 7 /* destroy */, NULL, 0, -1);
        g_id_tip_layer = 0;
    }
    if (g_id_tip_surface) {
        wl_send_msg(g_id_tip_surface, 0 /* destroy */, NULL, 0, -1);
        g_id_tip_surface = 0;
    }
    g_tip_configured = false;
}

static void update_tooltip_surface(void) {
    if (g_hovered_mod < 0 || !g_mod_boxes[g_hovered_mod].visible || !g_mod_boxes[g_hovered_mod].tooltip[0]) {
        destroy_tooltip_surface();
        return;
    }

    snprintf(g_active_tip_text, sizeof(g_active_tip_text), "%s", g_mod_boxes[g_hovered_mod].tooltip);
    int tw = 0, th = 0;
    measure_tooltip(g_active_tip_text, &tw, &th);
    int cx = (g_mod_boxes[g_hovered_mod].x1 + g_mod_boxes[g_hovered_mod].x2) / 2;
    int tx = cx - tw / 2;
    if (tx < 4) tx = 4;
    if (tx + tw > g_bar_w - 4) tx = g_bar_w - tw - 4;

    if (g_id_tip_surface && (tw != g_tip_w || th != g_tip_h || tx != g_tip_x)) {
        destroy_tooltip_surface();
    }

    g_tip_w = tw;
    g_tip_h = th;
    g_tip_x = tx;

    if (!g_id_tip_surface) {
        g_id_tip_surface = g_wl_next_id++;
        wl_send_u32_1(g_id_compositor, 0 /* create_surface */, g_id_tip_surface);
        if (!g_id_empty_region) {
            g_id_empty_region = g_wl_next_id++;
            wl_send_u32_1(g_id_compositor, 1 /* create_region */, g_id_empty_region);
        }
        wl_send_u32_1(g_id_tip_surface, 5 /* set_input_region (empty = click-through) */, g_id_empty_region);

        g_id_tip_layer = g_wl_next_id++;
        wl_get_layer_surface(g_id_tip_layer, g_id_tip_surface, 0, 3 /* OVERLAY */, "waybar-tooltip");
        wl_send_u32_2(g_id_tip_layer, 0 /* set_size */, (uint32_t)g_tip_w, (uint32_t)g_tip_h);
        wl_send_u32_1(g_id_tip_layer, 1 /* set_anchor */, 1 | 4 /* TOP | LEFT */);
        wl_send_u32_1(g_id_tip_layer, 2 /* set_exclusive_zone */, (uint32_t)-1);
        wl_send_u32_4(g_id_tip_layer, 3 /* set_margin */, BAR_HEIGHT + 2, 0, 0, (uint32_t)g_tip_x);
        wl_send_u32_1(g_id_tip_layer, 4 /* set_keyboard_interactivity */, 0);
        wl_send_msg(g_id_tip_surface, 6 /* commit */, NULL, 0, -1);
    } else if (g_tip_configured) {
        wl_attach_rendered_shm(g_id_tip_surface, g_tip_w, g_tip_h, render_tip_cb);
    }
}

static void show_bar_surface(void) {
    if (g_bar_visible) return;
    g_bar_visible = true;

    if (!g_id_bar_surface) {
        g_id_bar_surface = g_wl_next_id++;
        wl_send_u32_1(g_id_compositor, 0 /* create_surface */, g_id_bar_surface);
    }
    if (!g_id_bar_layer) {
        g_id_bar_layer = g_wl_next_id++;
        wl_get_layer_surface(g_id_bar_layer, g_id_bar_surface, 0, 2 /* LAYER_TOP */, "waybar");
        wl_send_u32_2(g_id_bar_layer, 0 /* set_size */, 0, BAR_HEIGHT);
        wl_send_u32_1(g_id_bar_layer, 1 /* set_anchor */, 1 | 4 | 8 /* TOP | LEFT | RIGHT */);
        wl_send_u32_1(g_id_bar_layer, 2 /* set_exclusive_zone */, BAR_HEIGHT);
        wl_send_u32_1(g_id_bar_layer, 4 /* set_keyboard_interactivity */, 0);
        wl_send_msg(g_id_bar_surface, 6 /* commit */, NULL, 0, -1);
    }
}

static void hide_bar_surface(void) {
    if (!g_bar_visible) return;
    g_bar_visible = false;
    destroy_tooltip_surface();
    g_hovered_mod = -1;
    g_hovered_tray_idx = -1;

    if (g_id_bar_layer) {
        wl_send_msg(g_id_bar_layer, 7 /* zwlr_layer_surface_v1.destroy */, NULL, 0, -1);
        g_id_bar_layer = 0;
    }
    if (g_id_bar_surface) {
        wl_send_msg(g_id_bar_surface, 0 /* wl_surface.destroy */, NULL, 0, -1);
        g_id_bar_surface = 0;
    }
    g_bar_configured = false;
}

static bool compute_expected_bar_visibility(void) {
    if (g_manual_hidden) return false;
    if (strcmp(g_sway_bar_mode, "invisible") == 0) return false;
    if (strcmp(g_sway_bar_mode, "hide") == 0 && strcmp(g_sway_bar_hidden_state, "hide") == 0) {
        if (g_sway_visible_by_modifier) return true;
        if (g_sway_mode[0] != '\0') return true;
        for (int i = 0; i < g_ws_count; i++) {
            if (g_workspaces[i].urgent) return true;
        }
        return false;
    }
    return true;
}

static void update_bar_visibility_state(void) {
    bool target;
    pthread_mutex_lock(&g_mutex);
    target = compute_expected_bar_visibility();
    pthread_mutex_unlock(&g_mutex);

    if (target != g_bar_visible) {
        if (target) {
            show_bar_surface();
        } else {
            hide_bar_surface();
        }
    }
}

static void handle_toggle_request(void) {
    bool swayed = false;
    int sfd = sway_ipc_connect();
    if (sfd >= 0) {
        char *resp = sway_ipc_roundtrip(sfd, 0 /* RUN_COMMAND */, "bar mode toggle");
        if (resp) {
            if (strstr(resp, "\"success\":true") || strstr(resp, "\"success\": true")) {
                swayed = true;
            }
            free(resp);
        }
        close(sfd);
    }
    if (!swayed) {
        pthread_mutex_lock(&g_mutex);
        g_manual_hidden = !g_manual_hidden;
        pthread_mutex_unlock(&g_mutex);
    }
}

static void sigusr1_handler(int sig) {
    (void)sig;
    g_toggle_requested = 1;
    if (g_wakeup_pipe[1] >= 0) {
        char c = 1;
        ssize_t r = write(g_wakeup_pipe[1], &c, 1);
        (void)r;
    }
}

static void sigterm_handler(int sig) {
    (void)sig;
    char pid_path[512];
    snprintf(pid_path, sizeof(pid_path), "%s/statusbar.pid", g_runtime_dir);
    unlink(pid_path);
    _exit(0);
}

static void redraw_all(void) {
    if (!g_bar_visible || !g_bar_configured) return;
    wl_attach_rendered_shm(g_id_bar_surface, g_bar_w, g_bar_h, render_bar);
    if (g_hovered_mod >= 0) {
        update_tooltip_surface();
    }
}

/* ============================================================================
 * Pointer Hover, Click & Scroll Dispatch
 * ============================================================================ */
static void handle_pointer_motion(int x, int y) {
    if (!g_bar_visible) return;
    g_pointer_x = x;
    g_pointer_y = y;
    int found = -1;
    int tray_idx = -1;
    for (int i = 0; i < MOD_COUNT; i++) {
        if (g_mod_boxes[i].visible && x >= g_mod_boxes[i].x1 && x <= g_mod_boxes[i].x2) {
            found = i;
            if (i == MOD_TRAY) {
                for (int t = 0; t < g_tray_count; t++) {
                    if (x >= g_tray_items[t].x1 && x <= g_tray_items[t].x2) {
                        tray_idx = t;
                        if (g_tray_items[t].title[0]) {
                            snprintf(g_mod_boxes[MOD_TRAY].tooltip, sizeof(g_mod_boxes[0].tooltip),
                                     "%s", g_tray_items[t].title);
                        }
                        break;
                    }
                }
            }
            break;
        }
    }
    if (found != g_hovered_mod || tray_idx != g_hovered_tray_idx) {
        g_hovered_mod = found;
        g_hovered_tray_idx = tray_idx;
        update_tooltip_surface();
    }
}

static void *async_gcal_refresh(void *a) {
    (void)a;
    fetch_gcal_events();
    return NULL;
}

static void handle_pointer_button(uint32_t time_ms, uint32_t button_code) {
    if (!g_bar_visible) return;
    int btn = 0;
    if (button_code == BTN_LEFT_CODE) btn = 1;
    else if (button_code == BTN_MIDDLE_CODE) btn = 2;
    else if (button_code == BTN_RIGHT_CODE) btn = 3;
    if (btn == 0) return;

    if (g_pointer_x >= 0) {
        handle_pointer_motion(g_pointer_x, g_pointer_y);
    }
    if (g_hovered_mod < 0) return;

    bool is_double = (g_hovered_mod == g_last_click_mod && (time_ms - g_last_click_ms) < 350);
    g_last_click_mod = g_hovered_mod;
    g_last_click_ms = time_ms;

    switch (g_hovered_mod) {
        case MOD_WORKSPACES:
            if (btn == 1) {
                for (int i = 0; i < g_ws_count; i++) {
                    if (g_pointer_x >= g_workspaces[i].x1 && g_pointer_x <= g_workspaces[i].x2) {
                        char cmd[64];
                        snprintf(cmd, sizeof(cmd), "workspace %s", g_workspaces[i].name);
                        sway_send_cmd(cmd);
                        refresh_sway_state();
                        redraw_all();
                        break;
                    }
                }
            }
            break;

        case MOD_SCRATCHPAD:
            if (btn == 1) {
                sway_send_cmd("scratchpad show");
                refresh_sway_state();
                redraw_all();
            }
            break;

        case MOD_CLOCK:
            if (btn == 1) { /* on-click: tz_up */
                if (g_tz_count > 0) g_tz_idx = (g_tz_idx + 1) % g_tz_count;
                redraw_all();
            } else if (btn == 2) { /* on-click-middle */
                spawn_detached("google-chrome --app=http://calendar.google.com/");
            } else if (btn == 3) { /* format-alt-click: 3 */
                g_clock_alt = !g_clock_alt;
                redraw_all();
            }
            break;

        case MOD_QUIET:
            if (btn == 1) {
                pthread_mutex_lock(&g_mutex);
                g_quiet_preview[0] = '\0';
                g_quiet_preview_until = 0;
                pthread_mutex_unlock(&g_mutex);
                dbus_quiet_call_oneshot("ShowNotifications");
                redraw_all();
            }
            break;

        case MOD_GCAL:
            if (btn == 3 || (btn == 1 && is_double)) {
                spawn_detached("google-chrome --app=http://calendar.google.com/");
            } else if (btn == 1) {
                pthread_mutex_lock(&g_mutex);
                g_gcal_show_details = !g_gcal_show_details;
                pthread_mutex_unlock(&g_mutex);
                redraw_all();
            } else if (btn == 2) {
                pthread_t th;
                pthread_create(&th, NULL, async_gcal_refresh, NULL);
                pthread_detach(th);
            }
            break;

        case MOD_MPRIS:
            if (btn == 1) mpris_action("PlayPause");
            else if (btn == 2) mpris_action("Previous");
            else if (btn == 3) mpris_action("Next");
            redraw_all();
            break;

        case MOD_PULSE:
            if (btn == 1) {
                pulse_toggle_mute();
                redraw_all();
            }
            break;

        case MOD_NAMAZ:
            if (btn == 1 || btn == 3) {
                pthread_mutex_lock(&g_mutex);
                g_namaz_show_full = !g_namaz_show_full;
                pthread_mutex_unlock(&g_mutex);
                redraw_all();
            } else if (btn == 2) {
                update_namaz_astronomical();
                redraw_all();
            }
            break;

        case MOD_SCREENSAVER:
            if (btn == 1) {
                toggle_screensaver();
                redraw_all();
            }
            break;

        case MOD_TRAY:
            if (g_hovered_tray_idx >= 0 && g_hovered_tray_idx < g_tray_count) {
                const char *method = (btn == 1) ? "Activate" : ((btn == 2) ? "SecondaryActivate" : "ContextMenu");
                char cmd[512];
                snprintf(cmd, sizeof(cmd),
                         "DBUS_SESSION_BUS_ADDRESS=unix:path=%s/bus gdbus call --session "
                         "--dest '%s' --object-path '%s' "
                         "--method org.kde.StatusNotifierItem.%s %d %d >/dev/null 2>&1",
                         g_runtime_dir,
                         g_tray_items[g_hovered_tray_idx].service,
                         g_tray_items[g_hovered_tray_idx].path[0] ? g_tray_items[g_hovered_tray_idx].path : "/StatusNotifierItem",
                         method, g_pointer_x, g_pointer_y);
                spawn_detached(cmd);
            }
            break;

        default:
            break;
    }
}

static int32_t g_scroll_accum = 0;
static int64_t g_last_scroll_ms = 0;

static void handle_pointer_axis(uint32_t axis, int32_t fixed_val) {
    if (!g_bar_visible || axis != 0) return; /* Vertical scroll only */
    if (g_hovered_mod == MOD_PULSE) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t now_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

        /* Reset accumulator if there has been an extended pause or direction reversal */
        if (now_ms - g_last_scroll_ms > 500 ||
            (fixed_val < 0 && g_scroll_accum > 0) ||
            (fixed_val > 0 && g_scroll_accum < 0)) {
            g_scroll_accum = 0;
        }
        g_scroll_accum += fixed_val;
        g_last_scroll_ms = now_ms;

        /* Standard discrete wheel step is 10.0 or 15.0 px (2560 to 3840 in wl_fixed 24.8).
         * Using 1536 (~6.0px) makes both discrete clicks and fine touchpads responsive without lag. */
        const int32_t STEP_THRESHOLD = 1536;
        int steps = 0;
        while (g_scroll_accum <= -STEP_THRESHOLD) {
            steps++;
            g_scroll_accum += STEP_THRESHOLD;
        }
        while (g_scroll_accum >= STEP_THRESHOLD) {
            steps--;
            g_scroll_accum -= STEP_THRESHOLD;
        }

        if (steps != 0) {
            pulse_adjust_volume(steps * 2);
            redraw_all();
        }
    }
}

/* ============================================================================
 * Wayland Socket Event Loop Dispatcher
 * ============================================================================ */
static void process_wayland_events(void) {
    static uint8_t rbuf[65536];
    static size_t rfilled = 0;

    bool read_any = false;
    while (rfilled < sizeof(rbuf)) {
        ssize_t n = recv(g_wl_fd, rbuf + rfilled, sizeof(rbuf) - rfilled, MSG_DONTWAIT);
        if (n <= 0) break;
        rfilled += (size_t)n;
        read_any = true;
    }
    if (!read_any && rfilled < 8) return;

    size_t pos = 0;
    while (pos + 8 <= rfilled) {
        uint32_t obj_id = *(uint32_t *)(rbuf + pos);
        uint32_t sz_op = *(uint32_t *)(rbuf + pos + 4);
        uint16_t len = (uint16_t)(sz_op >> 16);
        uint16_t opcode = (uint16_t)(sz_op & 0xFFFF);
        if (len < 8) {
            pos++;
            continue;
        }
        if (pos + len > rfilled) break;

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
            if ((caps & 1) && !g_id_pointer) {
                g_id_pointer = g_wl_next_id++;
                wl_send_u32_1(g_id_seat, 0 /* get_pointer */, g_id_pointer);
            }
        } else if (obj_id == g_id_bar_layer && opcode == 0 /* configure */) {
            uint32_t serial = *(const uint32_t *)p;
            uint32_t w = *(const uint32_t *)(p + 4);
            uint32_t h = *(const uint32_t *)(p + 8);
            if (w > 0) g_bar_w = (int)w;
            if (h > 0) g_bar_h = (int)h;
            wl_send_u32_1(g_id_bar_layer, 6 /* ack_configure */, serial);
            g_bar_configured = true;
            redraw_all();
        } else if (obj_id == g_id_tip_layer && opcode == 0 /* configure */) {
            uint32_t serial = *(const uint32_t *)p;
            wl_send_u32_1(g_id_tip_layer, 6 /* ack_configure */, serial);
            g_tip_configured = true;
            wl_attach_rendered_shm(g_id_tip_surface, g_tip_w, g_tip_h, render_tip_cb);
        } else if (obj_id == g_id_pointer) {
            if (opcode == 0 /* enter */) {
                uint32_t surf = *(const uint32_t *)(p + 4);
                int32_t sx = *(const int32_t *)(p + 8) / 256;
                int32_t sy = *(const int32_t *)(p + 12) / 256;
                if (surf == g_id_bar_surface) handle_pointer_motion(sx, sy);
            } else if (opcode == 1 /* leave */) {
                uint32_t surf = *(const uint32_t *)(p + 4);
                if (surf == g_id_bar_surface) {
                    g_hovered_mod = -1;
                    g_hovered_tray_idx = -1;
                    g_scroll_accum = 0;
                    destroy_tooltip_surface();
                }
            } else if (opcode == 2 /* motion */) {
                int32_t sx = *(const int32_t *)(p + 4) / 256;
                int32_t sy = *(const int32_t *)(p + 8) / 256;
                handle_pointer_motion(sx, sy);
            } else if (opcode == 3 /* button */) {
                uint32_t t_ms = *(const uint32_t *)(p + 4);
                uint32_t button = *(const uint32_t *)(p + 8);
                uint32_t state = *(const uint32_t *)(p + 12);
                if (state == 1) handle_pointer_button(t_ms, button);
            } else if (opcode == 4 /* axis */) {
                uint32_t axis = *(const uint32_t *)(p + 4);
                int32_t val = *(const int32_t *)(p + 8);
                handle_pointer_axis(axis, val);
            }
        }
        pos += len;
    }

    if (pos > 0) {
        memmove(rbuf, rbuf + pos, rfilled - pos);
        rfilled -= pos;
    }
}

/* ============================================================================
 * Config & CLI Parsing ($XDG_CONFIG_HOME/statusbar.conf + Single-Instance Lock)
 * ============================================================================ */
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

static void add_tz_entry(const char *raw_spec) {
    if (g_tz_count >= MAX_TZS) return;
    char spec[256];
    snprintf(spec, sizeof(spec), "%s", raw_spec);
    trim_and_unquote(spec);
    if (!spec[0]) return;

    const char *eq = strchr(spec, '=');
    if (!eq) eq = strchr(spec, ':'); /* Also allow TZ:emoji */
    if (eq) {
        size_t tz_len = (size_t)(eq - spec);
        if (tz_len >= sizeof(g_tzs[0].tz)) tz_len = sizeof(g_tzs[0].tz) - 1;
        memcpy(g_tzs[g_tz_count].tz, spec, tz_len);
        g_tzs[g_tz_count].tz[tz_len] = '\0';
        trim_and_unquote(g_tzs[g_tz_count].tz);

        char lbl[64];
        snprintf(lbl, sizeof(lbl), "%s", eq + 1);
        trim_and_unquote(lbl);
        snprintf(g_tzs[g_tz_count].label, sizeof(g_tzs[0].label), "%s", lbl);
    } else {
        snprintf(g_tzs[g_tz_count].tz, sizeof(g_tzs[0].tz), "%s", spec);
        g_tzs[g_tz_count].label[0] = '\0';
    }
    g_tz_count++;
}

static bool load_config_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    bool reset_tz = false;
    while (fgets(line, sizeof(line), f)) {
        char *s = line;
        while (*s && isspace((unsigned char)*s)) s++;
        if (*s == '#' || *s == '\0') continue;
        char *nl = strchr(s, '\n');
        if (nl) *nl = '\0';
        char *cr = strchr(s, '\r');
        if (cr) *cr = '\0';

        if ((strncmp(s, "tz", 2) == 0 && (isspace((unsigned char)s[2]) || s[2] == '=')) ||
            (strncmp(s, "timezone", 8) == 0 && (isspace((unsigned char)s[8]) || s[8] == '=' || s[8] == 's'))) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                if (!reset_tz) {
                    g_tz_count = 0;
                    reset_tz = true;
                }
                /* Support comma-separated list or single entry */
                char *tok = strtok(eq, ",");
                while (tok) {
                    add_tz_entry(tok);
                    tok = strtok(NULL, ",");
                }
            }
        } else if (strncmp(s, "gcal_token", 10) == 0 || strncmp(s, "google_calendar_token", 21) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                expand_tilde_path(eq, g_gcal_token_arg, sizeof(g_gcal_token_arg));
            }
        } else if (strncmp(s, "lat", 3) == 0 || strncmp(s, "latitude", 8) == 0) {
            char *eq = strchr(s, '=');
            if (eq) g_namaz_lat = atof(eq + 1);
        } else if (strncmp(s, "lon", 3) == 0 || strncmp(s, "lng", 3) == 0 || strncmp(s, "longitude", 9) == 0) {
            char *eq = strchr(s, '=');
            if (eq) g_namaz_lon = atof(eq + 1);
        } else if (strncmp(s, "city", 4) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                snprintf(g_namaz_city, sizeof(g_namaz_city), "%s", eq);
            }
        }
    }
    fclose(f);
    return true;
}

static pid_t find_running_statusbar_pid(void) {
    pid_t my_pid = getpid();
    uid_t my_uid = getuid();
    char pid_path[512];
    snprintf(pid_path, sizeof(pid_path), "%s/statusbar.pid", g_runtime_dir);

    FILE *pf = fopen(pid_path, "r");
    if (pf) {
        long old_pid = 0;
        if (fscanf(pf, "%ld", &old_pid) == 1 && old_pid > 1 && (pid_t)old_pid != my_pid) {
            if (kill((pid_t)old_pid, 0) == 0) {
                fclose(pf);
                return (pid_t)old_pid;
            }
        }
        fclose(pf);
    }

    DIR *d = opendir("/proc");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (!isdigit((unsigned char)de->d_name[0])) continue;
            pid_t p = (pid_t)atol(de->d_name);
            if (p <= 1 || p == my_pid) continue;

            char cpath[256];
            snprintf(cpath, sizeof(cpath), "/proc/%d/comm", (int)p);
            struct stat st;
            if (stat(cpath, &st) == 0 && st.st_uid == my_uid) {
                FILE *cf = fopen(cpath, "r");
                if (cf) {
                    char comm[64] = "";
                    if (fgets(comm, sizeof(comm), cf)) {
                        char *nl = strchr(comm, '\n');
                        if (nl) *nl = '\0';
                        if (strcmp(comm, "statusbar") == 0) {
                            fclose(cf);
                            closedir(d);
                            return p;
                        }
                    }
                    fclose(cf);
                }
            }
        }
        closedir(d);
    }
    return 0;
}

static void ensure_single_instance(void) {
    pid_t my_pid = getpid();
    pid_t running_pid = find_running_statusbar_pid();
    if (running_pid > 0 && running_pid != my_pid) {
        kill(running_pid, SIGTERM);
        usleep(40000); /* Give previous instance 40ms to release Wayland layer surface cleanly */
    }

    char pid_path[512];
    snprintf(pid_path, sizeof(pid_path), "%s/statusbar.pid", g_runtime_dir);
    FILE *pf = fopen(pid_path, "w");
    if (pf) {
        fprintf(pf, "%d\n", (int)my_pid);
        fclose(pf);
    }
}

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n\n"
           "Pure-C statically linked Wayland Layer-Shell statusbar matching custom Waybar.\n\n"
           "Configurability Options:\n"
           "  --toggle                      Toggle visibility (hide/unhide) of running statusbar (or start if not running)\n"
           "  --kill                        Terminate any running statusbar instance\n"
           "  --tz <TZ>[=<EMOJI_OR_LABEL>]  Add a timezone and optional label/emoji (repeatable;\n"
           "                                label/emoji can be empty, e.g. --tz 'UTC=' or --tz 'America/New_York=NY 🇺🇸')\n"
           "  --gcal-token <PATH_OR_TOKEN>  Path to Google Calendar OAuth JSON token file or raw Bearer token\n"
           "  --lat <DEGREES>               Latitude for offline prayer times (default: 40.7137)\n"
           "  --lon <DEGREES>               Longitude for offline prayer times (default: -74.0399)\n"
           "  --city <NAME>                 City label for prayer times (default: Jersey City)\n"
           "  --config <PATH>               Load configuration from file (default: $XDG_CONFIG_HOME/statusbar.conf)\n"
           "  --self-test                   Render bar & calendar tooltip offscreen and verify all modules\n"
           "  --wayland-test                Connect to live Wayland display, configure layer surface & present frame\n"
           "  -h, --help                    Show this help message\n",
           prog);
}

int main(int argc, char *argv[]) {
    /* Keep SIGCHLD as SIG_DFL so system() and popen()/pclose() return accurate exit codes */
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, sigusr1_handler);
    signal(SIGINT, sigterm_handler);
    signal(SIGTERM, sigterm_handler);

    const char *home = getenv("HOME");
    snprintf(g_home_dir, sizeof(g_home_dir), "%s", home ? home : "/usr/local/google/home/sselcuk");
    const char *rdir = getenv("XDG_RUNTIME_DIR");
    if (rdir && *rdir) snprintf(g_runtime_dir, sizeof(g_runtime_dir), "%s", rdir);
    else snprintf(g_runtime_dir, sizeof(g_runtime_dir), "/run/user/%u", getuid());

    /* Load configuration from $XDG_CONFIG_HOME/statusbar.conf (or ~/.config/statusbar.conf) */
    const char *xdg_cfg = getenv("XDG_CONFIG_HOME");
    char cfg_dir[512];
    if (xdg_cfg && *xdg_cfg) snprintf(cfg_dir, sizeof(cfg_dir), "%s", xdg_cfg);
    else snprintf(cfg_dir, sizeof(cfg_dir), "%s/.config", g_home_dir);

    char cpath1[512], cpath2[512], cpath3[512];
    snprintf(cpath1, sizeof(cpath1), "%s/statusbar.conf", cfg_dir);
    snprintf(cpath2, sizeof(cpath2), "%s/statusbar/statusbar.conf", cfg_dir);
    snprintf(cpath3, sizeof(cpath3), "%s/waybar/statusbar.conf", cfg_dir);

    if (load_config_file(cpath1)) {
        snprintf(g_config_path, sizeof(g_config_path), "%s", cpath1);
    } else if (load_config_file(cpath2)) {
        snprintf(g_config_path, sizeof(g_config_path), "%s", cpath2);
    } else if (load_config_file(cpath3)) {
        snprintf(g_config_path, sizeof(g_config_path), "%s", cpath3);
    }

    bool cli_tz_reset = false;
    bool self_test = false;
    bool wayland_test = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--toggle") == 0) {
            pid_t pid = find_running_statusbar_pid();
            if (pid > 0) {
                kill(pid, SIGUSR1);
                return 0;
            }
            /* Not running: continue and launch */
        } else if (strcmp(argv[i], "--kill") == 0) {
            pid_t pid = find_running_statusbar_pid();
            if (pid > 0) {
                kill(pid, SIGTERM);
            }
            return 0;
        } else if (strcmp(argv[i], "--self-test") == 0) {
            self_test = true;
        } else if (strcmp(argv[i], "--wayland-test") == 0) {
            wayland_test = true;
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            snprintf(g_sway_bar_id, sizeof(g_sway_bar_id), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            char exp_cfg[512];
            expand_tilde_path(argv[++i], exp_cfg, sizeof(exp_cfg));
            load_config_file(exp_cfg);
        } else if (strcmp(argv[i], "--tz") == 0 && i + 1 < argc) {
            if (!cli_tz_reset) {
                g_tz_count = 0;
                cli_tz_reset = true;
            }
            add_tz_entry(argv[++i]);
        } else if (strcmp(argv[i], "--gcal-token") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], g_gcal_token_arg, sizeof(g_gcal_token_arg));
        } else if ((strcmp(argv[i], "--lat") == 0 || strcmp(argv[i], "--latitude") == 0) && i + 1 < argc) {
            g_namaz_lat = atof(argv[++i]);
        } else if ((strcmp(argv[i], "--lon") == 0 || strcmp(argv[i], "--lng") == 0 || strcmp(argv[i], "--longitude") == 0) && i + 1 < argc) {
            g_namaz_lon = atof(argv[++i]);
        } else if (strcmp(argv[i], "--city") == 0 && i + 1 < argc) {
            snprintf(g_namaz_city, sizeof(g_namaz_city), "%s", argv[++i]);
        }
    }

    /* If no timezone was specified in $XDG_CONFIG_HOME/statusbar.conf or CLI, default to single local timezone */
    if (g_tz_count == 0) {
        g_tzs[0].tz[0] = '\0';
        g_tzs[0].label[0] = '\0';
        g_tz_count = 1;
    }

    init_fonts();

    if (self_test) {
        load_gcal_credentials();
        update_namaz_astronomical();
        refresh_sway_state();
        poll_pulseaudio();
        poll_screensaver();
        poll_battery();
        poll_quiet();
        uint32_t *px = calloc(3840 * BAR_HEIGHT, 4);
        SurfaceBuf sb = {.px = px, .w = 3840, .h = BAR_HEIGHT};
        render_bar(&sb);
        printf("Self-test OK: rendered 3840x%d bar | workspaces=%d | tzs=%d (current='%s' label='%s') | clock_tip='%s' | screensaver=%s | quiet=%u (urg=%u preview='%s') | pulse=%d%%\n",
               BAR_HEIGHT, g_ws_count, g_tz_count, g_tzs[0].tz, g_tzs[0].label,
               g_mod_boxes[MOD_CLOCK].tooltip,
               g_screensaver_active ? "active(💤)" : "inhibited(👁)",
               g_quiet_count, g_quiet_urgency, g_quiet_preview, g_pulse_vol);
        free(px);
        return 0;
    }

    if (!wayland_test) {
        ensure_single_instance();
    }

    /* Connect to Wayland display socket (support WAYLAND_SOCKET from compositor or WAYLAND_DISPLAY) */
    const char *w_sock = getenv("WAYLAND_SOCKET");
    if (w_sock && *w_sock) {
        g_wl_fd = atoi(w_sock);
        fcntl(g_wl_fd, F_SETFD, FD_CLOEXEC);
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
            fprintf(stderr, "Failed to connect to Wayland socket at %s\n", wl_path);
            return 1;
        }
    }

    g_id_registry = g_wl_next_id++;
    wl_send_u32_1(1 /* wl_display */, 1 /* get_registry */, g_id_registry);
    uint32_t sync_cb = g_wl_next_id++;
    wl_send_u32_1(1 /* wl_display */, 0 /* sync */, sync_cb);

    /* Read initial globals */
    struct pollfd pfd = {.fd = g_wl_fd, .events = POLLIN};
    for (int i = 0; i < 10 && (!g_id_compositor || !g_id_shm || !g_id_layer_shell); i++) {
        if (poll(&pfd, 1, 200) > 0) process_wayland_events();
    }

    if (!g_id_compositor || !g_id_shm || !g_id_layer_shell) {
        fprintf(stderr, "Compositor missing wl_compositor, wl_shm, or zwlr_layer_shell_v1\n");
        return 1;
    }

    /* Create top bar layer surface */
    show_bar_surface();

    if (wayland_test) {
        update_namaz_astronomical();
        refresh_sway_state();
        poll_pulseaudio();
        poll_screensaver();
        for (int i = 0; i < 10 && !g_bar_configured; i++) {
            if (poll(&pfd, 1, 200) > 0) process_wayland_events();
        }
        printf("Wayland live test OK: layer_surface configured=%d width=%d height=%d\n",
               g_bar_configured ? 1 : 0, g_bar_w, g_bar_h);
        close(g_wl_fd);
        return g_bar_configured ? 0 : 1;
    }

    if (pipe2(g_wakeup_pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
        perror("pipe2");
        return 1;
    }

    poll_battery();
    poll_screensaver();

    pthread_t th_sway, th_quiet, th_audio, th_bat, th_bg;
    pthread_create(&th_sway, NULL, sway_event_thread, NULL);
    pthread_create(&th_quiet, NULL, quiet_dbus_thread, NULL);
    pthread_create(&th_audio, NULL, audio_event_thread, NULL);
    pthread_create(&th_bat, NULL, battery_uevent_thread, NULL);
    pthread_create(&th_bg, NULL, background_worker_thread, NULL);

    struct pollfd fds[2];
    fds[0].fd = g_wl_fd;
    fds[0].events = POLLIN;
    fds[1].fd = g_wakeup_pipe[0];
    fds[1].events = POLLIN;

    while (1) {
        int timeout_ms = 60000;
        time_t now = time(NULL);
        if (g_clock_alt || (g_bat_present && g_bat_capacity <= 5)) {
            timeout_ms = 500;
        } else {
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            int secs_to_next_min = 60 - tm_now.tm_sec;
            if (secs_to_next_min <= 0) secs_to_next_min = 1;
            timeout_ms = secs_to_next_min * 1000;
            pthread_mutex_lock(&g_mutex);
            if (g_quiet_preview_until > now) {
                int rem_ms = (int)(g_quiet_preview_until - now) * 1000;
                if (rem_ms > 0 && rem_ms < timeout_ms) timeout_ms = rem_ms;
            }
            pthread_mutex_unlock(&g_mutex);
        }
        int ret = poll(fds, 2, timeout_ms);
        if (ret > 0) {
            if (fds[0].revents & POLLIN) process_wayland_events();
            if (fds[1].revents & POLLIN) {
                char drain[64];
                while (read(g_wakeup_pipe[0], drain, sizeof(drain)) > 0) {}
                if (g_toggle_requested) {
                    g_toggle_requested = 0;
                    handle_toggle_request();
                }
                update_bar_visibility_state();
                redraw_all();
            }
        } else if (ret == 0) {
            g_bat_blink_on = !g_bat_blink_on;
            poll_battery();
            static int s_last_day = -1;
            struct tm tm_cur;
            time_t t_cur = time(NULL);
            localtime_r(&t_cur, &tm_cur);
            if (tm_cur.tm_mday != s_last_day) {
                s_last_day = tm_cur.tm_mday;
                update_namaz_astronomical();
            }
            update_bar_visibility_state();
            redraw_all();
        }
    }

    return 0;
}
