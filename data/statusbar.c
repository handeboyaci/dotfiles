#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <regex.h>
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
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
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
 * Constants & Theme Visuals (configurable via $XDG_CONFIG_HOME/statusbar.conf)
 * Defaults to neutral black with transparency (rgba(0, 0, 0, 0.8))
 * ============================================================================ */
#define BAR_HEIGHT 26

static uint32_t g_col_bar_bg        = 0xCC000000U; /* rgba(0, 0, 0, 0.8) - black with transparency */
static uint32_t g_col_bar_border    = 0x00000000U; /* transparent bottom border */
static uint32_t g_col_text          = 0xFFFFFFFFU; /* #ffffff crisp white text */
static uint32_t g_col_ws_idle       = 0xFF888888U; /* #888888 muted idle workspace */
static uint32_t g_col_ws_foc_bg     = 0xFF333333U; /* #333333 dark grey focused box */
static uint32_t g_col_ws_foc_bdr    = 0xFF3EAEFFU; /* #3eaeff sway blue focused border */
static uint32_t g_col_urgent        = 0xFFCC6666U; /* #cc6666 red */
static uint32_t g_col_warning       = 0xFFE5A454U; /* #e5a454 amber */
static uint32_t g_col_active        = 0xFF2ECC71U; /* #2ecc71 emerald green */
static uint32_t g_col_tip_bg        = 0xF0111111U; /* rgba(17, 17, 17, 0.94) */
static uint32_t g_col_tip_bdr       = 0xFF333333U; /* #333333 */
static uint32_t g_col_cld_month     = 0xFFFFFFFFU; /* #ffffff */
static uint32_t g_col_cld_wkday     = 0xFF888888U; /* #888888 */
static uint32_t g_col_cld_today_bg  = 0xFFFFFFFFU; /* #ffffff */
static uint32_t g_col_cld_today_fg  = 0xFF000000U; /* #000000 */

#define COL_BAR_BG       g_col_bar_bg
#define COL_BAR_BORDER   g_col_bar_border
#define COL_TEXT         g_col_text
#define COL_WS_IDLE      g_col_ws_idle
#define COL_WS_FOC_BG    g_col_ws_foc_bg
#define COL_WS_FOC_BDR   g_col_ws_foc_bdr
#define COL_URGENT       g_col_urgent
#define COL_WARNING      g_col_warning
#define COL_ACTIVE       g_col_active
#define COL_TIP_BG       g_col_tip_bg
#define COL_TIP_BDR      g_col_tip_bdr
#define COL_CLD_MONTH    g_col_cld_month
#define COL_CLD_WKDAY    g_col_cld_wkday
#define COL_CLD_TODAY_BG g_col_cld_today_bg
#define COL_CLD_TODAY_FG g_col_cld_today_fg

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

static TzEntry g_cfg_tzs[MAX_TZS];
static int g_cfg_tz_count = 0;
static TzEntry g_tzs[MAX_TZS];
static int g_tz_count = 0;
static int g_tz_idx = 0;
static char g_detected_tz[64] = "";
static char g_detected_city[64] = "";
static bool g_clock_alt = false; /* Toggled by right-click (format-alt-click: 3) */
static char g_gcal_token_arg[512] = "";
static char g_config_path[512] = "";
static char g_runtime_dir[256] = "";
static char g_home_dir[256] = "";
static char g_cache_dir[512] = "";

/* ============================================================================
 * Module Data Structures
 * ============================================================================ */
typedef struct {
    char name[32];
    int num;
    bool focused;
    bool urgent;
    int x1, x2;
} WorkspaceItem;

typedef struct {
    char summary[128];
    char raw_location[256];
    char location[128];
    char start_short[16];
    char time_range[24];
    time_t start_ts;
    time_t end_ts;
    bool is_all_day;
    bool filtered;
} GCalEvent;

typedef struct {
    time_t start_ts;
    char summary[128];
} GCalNotified;

typedef struct {
    char service[128];
    char path[128];
    char title[128];
    uint32_t pixels[16 * 16];
    bool has_pixels;
    int x1, x2;
} TrayItem;

typedef enum {
    MOD_HEADLESS = 0,
    MOD_WORKSPACES,
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
static pthread_mutex_t g_tz_mutex = PTHREAD_MUTEX_INITIALIZER;
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
static bool g_sway_headless = false;
static bool g_client_passthrough = false;
static int g_passthrough_inotify_fd = -1;
static inline bool is_sway_headless(void) {
    const char *env = getenv("SWAYSOCK");
    (void)env;
    const char *h = getenv("SWAY_HEADLESS");
    return (h && h[0] != '\0' && strcmp(h, "0") != 0 &&
            strcasecmp(h, "false") != 0 && strcasecmp(h, "no") != 0);
}

static bool check_client_passthrough_file(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/fullmoon/passthrough", g_runtime_dir);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char c = '0';
    ssize_t r = read(fd, &c, 1);
    close(fd);
    return (r > 0 && c == '1');
}

static void init_passthrough_inotify(void) {
    if (g_passthrough_inotify_fd >= 0) return;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/fullmoon", g_runtime_dir);
    mkdir(dir, 0700);
    g_passthrough_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_passthrough_inotify_fd >= 0) {
        inotify_add_watch(g_passthrough_inotify_fd, dir, IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_TO | IN_CLOSE_WRITE);
    }
    g_client_passthrough = check_client_passthrough_file();
}
static volatile sig_atomic_t g_toggle_requested = 0;
static volatile sig_atomic_t g_geoloc_update_requested = 0;
static volatile sig_atomic_t g_rename_ws_requested = 0;

/* Inline Workspace Rename Widget */
static bool g_ws_renaming = false;
static char g_ws_rename_buf[32] = "";
static int g_ws_rename_cursor = 0;
static int g_ws_rename_num = -1;


/* Quiet Notification Daemon */
static uint32_t g_quiet_count = 0;
static uint32_t g_quiet_urgency = 0;
static char g_quiet_preview[256] = "";
static time_t g_quiet_preview_until = 0;

/* Google Calendar */
#define MAX_GCAL_EVENTS 32
#define MAX_GCAL_CALS 8
#define MAX_LOC_PREFIXES 16
static GCalEvent g_gcal_events[MAX_GCAL_EVENTS];
static int g_gcal_count = 0;
static GCalNotified g_gcal_notified[MAX_GCAL_EVENTS];
static int g_gcal_notified_count = 0;
static bool g_gcal_show_details = false;
static bool g_gcal_private = false;
static bool g_gcal_fetching = false;
static char g_gcal_access_token[1024] = "";
static char g_gcal_client_id[256] = "";
static char g_gcal_client_secret[256] = "";
static char g_gcal_refresh_token[512] = "";
static char g_gcal_token_file[512] = "";
static time_t g_gcal_token_expiry = 0;
static char g_gcal_calendars[MAX_GCAL_CALS][128] = {"primary"};
static int g_gcal_cal_count = 1;
#define MAX_SCOPED_LOC_RULES 16
typedef struct {
    char scope[128];
    char prefixes[MAX_LOC_PREFIXES][128];
    int prefix_count;
} ScopedLocRule;

static ScopedLocRule g_scoped_loc_rules[MAX_SCOPED_LOC_RULES];
static int g_scoped_loc_rule_count = 0;
static char g_default_loc_prefixes[MAX_LOC_PREFIXES][128];
static int g_default_loc_prefix_count = 0;
static char g_gcal_loc_prefixes[MAX_LOC_PREFIXES][128];
static regex_t g_gcal_loc_re[MAX_LOC_PREFIXES];
static bool g_gcal_loc_has_re[MAX_LOC_PREFIXES];
static int g_gcal_loc_prefix_count = 0;
#define MAX_IGNORED_EVENTS 32
static char g_gcal_ignored_events[MAX_IGNORED_EVENTS][128];
static int g_gcal_ignored_event_count = 0;
static int g_gcal_max_duration_mins = 360; /* Default 6h (ignore_long_event_threshold) */
static int g_gcal_notify_mins = 5;         /* Default 5m urgent notification before start */
static int g_gcal_notify_expire_mins = 8;  /* Default 8m expiration (3m after start) */
static char g_gcal_open_cmd[512] = "google-chrome --profile-directory=Default --app=https://calendar.google.com/";

/* MPRIS */
static int g_mpris_state = 0; /* 0=hidden, 1=Playing, 2=Paused */
static char g_mpris_tooltip[256] = "";

/* PulseAudio / PipeWire */
static int g_pulse_vol = 40;
static bool g_pulse_muted = false;

/* Namaz (Offline Astronomical Moonsighting Committee) */
static char g_namaz_city[64] = "";
static bool g_namaz_city_configured = false;
static double g_namaz_lat = 0.0;
static double g_namaz_lon = 0.0;
static bool g_namaz_auto_loc = false;
static char g_namaz_tz[64] = "";
static bool g_namaz_tz_configured = false;
static char g_namaz_today[6][16]; /* Fajr, Sunrise, Dhuhr, Asr, Maghrib, Isha */
static char g_namaz_tom_fajr[16] = "";
static int g_namaz_loaded_mday = -1;
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

static void update_active_loc_prefixes(void);

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
        unsetenv("WAYLAND_SOCKET");
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

static void notify_urgent(const char *summary, const char *body) {
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
        if (g_runtime_dir[0]) {
            char bus[512];
            snprintf(bus, sizeof(bus), "unix:path=%s/bus", g_runtime_dir);
            setenv("DBUS_SESSION_BUS_ADDRESS", bus, 0);
        }
        int expire_ms = (g_gcal_notify_expire_mins > 0) ? g_gcal_notify_expire_mins * 60 * 1000 : 0;
        if (expire_ms > 0) {
            char exp_str[32];
            snprintf(exp_str, sizeof(exp_str), "%d", expire_ms);
            execlp("notify-send", "notify-send", "-a", "Google Calendar", "-i", "calendar", "-u", "critical",
                   "-t", exp_str, summary ? summary : "", body ? body : "", (char *)NULL);
        } else {
            execlp("notify-send", "notify-send", "-a", "Google Calendar", "-i", "calendar", "-u", "critical",
                   summary ? summary : "", body ? body : "", (char *)NULL);
        }
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
}

static char *run_cmd_capture(const char *cmd, size_t max_len) {
    pthread_mutex_lock(&g_tz_mutex);
    FILE *fp = popen(cmd, "r");
    pthread_mutex_unlock(&g_tz_mutex);
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
static char g_emoji_font_path[512] = "";
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
    if (!g_face_symbola) {
        char u_symb[512];
        snprintf(u_symb, sizeof(u_symb), "%s/.fonts/Symbola.ttf", g_home_dir);
        if (FT_New_Face(g_ft, u_symb, 0, &g_face_symbola) != 0) {
            snprintf(u_symb, sizeof(u_symb), "%s/.local/share/fonts/Symbola.ttf", g_home_dir);
            FT_New_Face(g_ft, u_symb, 0, &g_face_symbola);
        }
    }

    const char *noto_paths[] = {
        "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/google-noto-color-emoji-fonts/NotoColorEmoji.ttf",
        "/usr/share/fonts/noto-color-emoji/NotoColorEmoji.ttf",
        "/usr/share/fonts/TTF/NotoColorEmoji.ttf",
        "/usr/share/fonts/NotoColorEmoji.ttf",
        "/usr/local/share/fonts/NotoColorEmoji.ttf",
        NULL
    };
    for (int i = 0; noto_paths[i]; i++) {
        if (FT_New_Face(g_ft, noto_paths[i], 0, &g_face_emoji) == 0) {
            snprintf(g_emoji_font_path, sizeof(g_emoji_font_path), "%s", noto_paths[i]);
            break;
        }
    }
    if (!g_face_emoji) {
        char u_noto[512];
        snprintf(u_noto, sizeof(u_noto), "%s/.fonts/NotoColorEmoji.ttf", g_home_dir);
        if (FT_New_Face(g_ft, u_noto, 0, &g_face_emoji) == 0) {
            snprintf(g_emoji_font_path, sizeof(g_emoji_font_path), "%s", u_noto);
        } else {
            snprintf(u_noto, sizeof(u_noto), "%s/.local/share/fonts/NotoColorEmoji.ttf", g_home_dir);
            if (FT_New_Face(g_ft, u_noto, 0, &g_face_emoji) == 0) {
                snprintf(g_emoji_font_path, sizeof(g_emoji_font_path), "%s", u_noto);
            }
        }
    }

    if (g_face_emoji) {
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
    } else {
        fprintf(stderr, "statusbar: [WARNING] NotoColorEmoji.ttf not found in standard system font paths.\n"
                        "           Color emojis will fall back to Symbola monochrome.\n"
                        "           To enable full-color emojis, please install: 'fonts-noto-color-emoji' (Debian/Ubuntu) or 'noto-fonts-emoji' (Arch/Fedora).\n");
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

static inline int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static uint32_t parse_color_spec(const char *spec, uint32_t fallback) {
    if (!spec) return fallback;
    while (*spec && isspace((unsigned char)*spec)) spec++;
    if (!*spec) return fallback;

    /* Handle rgba(...) or rgb(...) */
    if (strncasecmp(spec, "rgba", 4) == 0 || strncasecmp(spec, "rgb", 3) == 0) {
        const char *p = strchr(spec, '(');
        if (p) {
            p++;
            int r = 0, g = 0, b = 0;
            double a = 1.0;
            if (sscanf(p, "%d , %d , %d , %lf", &r, &g, &b, &a) >= 3 ||
                sscanf(p, "%d %d %d %lf", &r, &g, &b, &a) >= 3 ||
                sscanf(p, "%d,%d,%d,%lf", &r, &g, &b, &a) >= 3) {
                if (r < 0) r = 0;
                else if (r > 255) r = 255;
                if (g < 0) g = 0;
                else if (g > 255) g = 255;
                if (b < 0) b = 0;
                else if (b > 255) b = 255;
                uint32_t u_a;
                if (a <= 1.0 && a >= 0.0) u_a = (uint32_t)(a * 255.0 + 0.5);
                else if (a > 1.0 && a <= 255.0) u_a = (uint32_t)(a + 0.5);
                else u_a = 255;
                return (u_a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }

    const char *hex = spec;
    if (*hex == '#') hex++;
    size_t len = 0;
    while (isxdigit((unsigned char)hex[len])) len++;

    if (len == 6) {
        uint32_t rgb = (uint32_t)strtoul(hex, NULL, 16);
        return 0xFF000000U | (rgb & 0xFFFFFFU);
    } else if (len == 8) {
        /* Standard #RRGGBBAA */
        char buf[9];
        memcpy(buf, hex, 8);
        buf[8] = '\0';
        uint32_t val = (uint32_t)strtoul(buf, NULL, 16);
        uint32_t rgb = (val >> 8) & 0xFFFFFFU;
        uint32_t a = val & 0xFFU;
        return (a << 24) | rgb;
    } else if (len == 3) {
        int r = hex_nibble(hex[0]);
        int g = hex_nibble(hex[1]);
        int b = hex_nibble(hex[2]);
        return 0xFF000000U | ((uint32_t)(r * 17) << 16) | ((uint32_t)(g * 17) << 8) | (uint32_t)(b * 17);
    } else if (len == 4) {
        int r = hex_nibble(hex[0]);
        int g = hex_nibble(hex[1]);
        int b = hex_nibble(hex[2]);
        int a = hex_nibble(hex[3]);
        return ((uint32_t)(a * 17) << 24) | ((uint32_t)(r * 17) << 16) | ((uint32_t)(g * 17) << 8) | (uint32_t)(b * 17);
    }
    return fallback;
}

static uint32_t parse_hex_color(const char *hex, uint32_t fallback) {
    return parse_color_spec(hex, fallback);
}

static inline bool is_symbola_forced(uint32_t cp) {
    /* Braille patterns (U+2800..U+28FF) */
    if (cp >= 0x2800 && cp <= 0x28FF) return true;
    /* Speaker icons: 🔇 U+1F507, 🔈 U+1F508, 🔉 U+1F509, 🔊 U+1F50A */
    if (cp >= 0x1F507 && cp <= 0x1F50A) return true;
    /* Battery / power icons: 🔋 U+1F50B, ⚡ U+26A1, 🔌 U+1F50C, ▁ U+2581 */
    if (cp == 0x1F50B || cp == 0x26A1 || cp == 0x1F50C || cp == 0x2581) return true;
    /* Screensaver icons: 💤 U+1F4A4, 👁 U+1F441 */
    if (cp == 0x1F4A4 || cp == 0x1F441) return true;
    /* Calendar icons: 📆 U+1F4C6 */
    if (cp == 0x1F4C6) return true;
    /* Headless monitor icons: ⎆ U+2386 */
    if (cp == 0x21E4 ) return true;
    return false;
}

static inline bool is_flag_or_city_emoji(uint32_t cp) {
    /* Explicit city & landmark emojis */
    switch (cp) {
        case 0x1F5FD: /* 🗽 Statue of Liberty (NYC) */
        case 0x1F5FC: /* 🗼 Tokyo Tower */
        case 0x1F5FB: /* 🗻 Mount Fuji */
        case 0x1F3D9: /* 🏙 Cityscape */
        case 0x1F306: /* 🌆 Cityscape at dusk */
        case 0x1F307: /* 🌇 Sunset over buildings */
        case 0x1F309: /* 🌉 Bridge at night */
        case 0x1F3DB: /* 🏛 Classical building */
        case 0x1F3F0: /* 🏰 European castle */
        case 0x1F3EF: /* 🏯 Japanese castle */
        case 0x1F3A1: /* 🎡 Ferris wheel */
        case 0x1F54C: /* 🕌 Mosque */
        case 0x1F54D: /* 🕍 Synagogue */
        case 0x1F6D5: /* 🛕 Hindu temple */
        case 0x26E9:  /* ⛩ Shinto shrine */
        case 0x1F54B: /* 🕋 Kaaba */
        case 0x1F5FF: /* 🗿 Moyai */
        case 0x1F3DD: /* 🏝 Desert island */
        case 0x1F3D4: /* 🏔 Snow-capped mountain */
        case 0x1F3D6: /* 🏖 Beach with umbrella */
        case 0x1F3DC: /* 🏜 Desert */
        case 0x1F30B: /* 🌋 Volcano */
            return true;
        default:
            break;
    }
    /* Any non-ASCII emoji configured as a timezone label in g_cfg_tzs or g_tzs */
    for (int i = 0; i < g_cfg_tz_count; i++) {
        const char *s = g_cfg_tzs[i].label;
        while (*s) {
            uint32_t c = utf8_next(&s);
            if (c == cp && c > 127) return true;
        }
    }
    for (int i = 0; i < g_tz_count; i++) {
        const char *s = g_tzs[i].label;
        while (*s) {
            uint32_t c = utf8_next(&s);
            if (c == cp && c > 127) return true;
        }
    }
    return false;
}

/* Render or measure a single line of markup text */
static int render_markup_line(SurfaceBuf *s, const char *text, int start_x, int baseline_y,
                              int px_size, uint32_t default_fg, bool force_bold, bool draw) {
    FT_Set_Pixel_Sizes(g_face_reg, 0, px_size);
    FT_Set_Pixel_Sizes(g_face_bold, 0, px_size);
    FT_Set_Pixel_Sizes(g_face_sym, 0, px_size == 16 ? 17 : px_size);
    if (g_face_symbola) {
        FT_Set_Pixel_Sizes(g_face_symbola, 0, px_size == 16 ? 18 : px_size);
    }

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

        /* Check for flag sequences or city/landmark emojis (rendered in color via Noto Color Emoji) */
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
        } else if (!is_symbola_forced(cp) && is_flag_or_city_emoji(cp) && g_face_emoji) {
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
        FT_UInt gidx = 0;

        if (is_symbola_forced(cp) && g_face_symbola) {
            gidx = FT_Get_Char_Index(g_face_symbola, cp);
            if (gidx != 0) face = g_face_symbola;
        }
        if (gidx == 0) {
            gidx = FT_Get_Char_Index(face, cp);
        }
        if (gidx == 0 && g_face_sym) {
            gidx = FT_Get_Char_Index(g_face_sym, cp);
            if (gidx != 0) face = g_face_sym;
        }
        if (gidx == 0 && g_face_symbola) {
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
    if ((COL_BAR_BORDER >> 24) == 0) return;
    for (int y = 0; y < BAR_HEIGHT - 1; y++) {
        blend_pixel(s, x, y, COL_BAR_BORDER);
    }
}

/* ============================================================================
 * Clock & ISO-8601 Year Calendar Generator (matching config.jsonc)
 * ============================================================================ */
static void format_tz_time(const char *tz_name, time_t t, struct tm *out_tm) {
    pthread_mutex_lock(&g_tz_mutex);
    if (!tz_name || !*tz_name) {
        localtime_r(&t, out_tm);
        pthread_mutex_unlock(&g_tz_mutex);
        return;
    }

    const char *old_tz = getenv("TZ");
    char saved_tz[128] = "";
    bool had_tz = (old_tz != NULL);
    if (had_tz) snprintf(saved_tz, sizeof(saved_tz), "%s", old_tz);

    setenv("TZ", tz_name, 1);
    tzset();
    localtime_r(&t, out_tm);

    if (had_tz) setenv("TZ", saved_tz, 1);
    else unsetenv("TZ");
    tzset();
    pthread_mutex_unlock(&g_tz_mutex);
}


static void get_system_tz(char *out, size_t sz) {
    char buf[512];
    ssize_t len = readlink("/etc/localtime", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        char *zi = strstr(buf, "/zoneinfo/");
        if (zi) {
            snprintf(out, sz, "%s", zi + 10);
            return;
        }
    }
    char *res = realpath("/etc/localtime", NULL);
    if (res) {
        char *zi = strstr(res, "/zoneinfo/");
        if (zi) {
            snprintf(out, sz, "%s", zi + 10);
            free(res);
            return;
        }
        free(res);
    }
    const char *tz = getenv("TZ");
    if (tz && *tz) {
        snprintf(out, sz, "%s", tz);
        return;
    }
    snprintf(out, sz, "UTC");
}

static bool is_same_timezone(const char *tz1, const char *tz2) {
    if (!tz1 || !tz2 || !*tz1 || !*tz2) return false;
    if (strcmp(tz1, tz2) == 0) return true;

    char p1[512], p2[512];
    snprintf(p1, sizeof(p1), "/usr/share/zoneinfo/%s", tz1);
    snprintf(p2, sizeof(p2), "/usr/share/zoneinfo/%s", tz2);

    char *r1 = realpath(p1, NULL);
    char *r2 = realpath(p2, NULL);
    bool same = false;
    if (r1 && r2 && strcmp(r1, r2) == 0) {
        same = true;
    }
    free(r1);
    free(r2);
    if (same) return true;

    /* Match city suffix (e.g. Asia/Istanbul vs Europe/Istanbul, or New_York vs America/New_York) */
    const char *slash1 = strrchr(tz1, '/');
    const char *s1 = slash1 ? slash1 + 1 : tz1;
    const char *slash2 = strrchr(tz2, '/');
    const char *s2 = slash2 ? slash2 + 1 : tz2;
    if (*s1 && *s2 && strcasecmp(s1, s2) == 0) {
        return true;
    }
    return false;
}

static void update_active_clocks(void) {
    pthread_mutex_lock(&g_mutex);

    char det_tz[64] = "";
    if (g_detected_tz[0]) {
        snprintf(det_tz, sizeof(det_tz), "%s", g_detected_tz);
    } else {
        get_system_tz(det_tz, sizeof(det_tz));
        snprintf(g_detected_tz, sizeof(g_detected_tz), "%s", det_tz);
    }

    int match_idx = -1;
    for (int i = 0; i < g_cfg_tz_count; i++) {
        if (is_same_timezone(det_tz, g_cfg_tzs[i].tz)) {
            match_idx = i;
            break;
        }
    }

    g_tz_count = 0;

    if (match_idx >= 0) {
        /* Current location is one of the configured locations: deduplicate.
         * Default (index 0) is current location, with emoji stripped in status bar.
         * All other configured locations follow, keeping their emojis.
         */
        snprintf(g_tzs[0].tz, sizeof(g_tzs[0].tz), "%s", g_cfg_tzs[match_idx].tz);
        g_tzs[0].label[0] = '\0';
        g_tz_count = 1;

        for (int i = 0; i < g_cfg_tz_count; i++) {
            if (i == match_idx) continue;
            if (g_tz_count < MAX_TZS) {
                snprintf(g_tzs[g_tz_count].tz, sizeof(g_tzs[g_tz_count].tz), "%s", g_cfg_tzs[i].tz);
                snprintf(g_tzs[g_tz_count].label, sizeof(g_tzs[g_tz_count].label), "%s", g_cfg_tzs[i].label);
                g_tz_count++;
            }
        }
    } else {
        /* New location not in configured list: total locations = 1 + configured.
         * Default (index 0) is current location (w/o emoji).
         * All configured locations follow, keeping their emojis.
         */
        snprintf(g_tzs[0].tz, sizeof(g_tzs[0].tz), "%s", det_tz);
        g_tzs[0].label[0] = '\0';
        g_tz_count = 1;

        for (int i = 0; i < g_cfg_tz_count; i++) {
            if (g_tz_count < MAX_TZS) {
                snprintf(g_tzs[g_tz_count].tz, sizeof(g_tzs[g_tz_count].tz), "%s", g_cfg_tzs[i].tz);
                snprintf(g_tzs[g_tz_count].label, sizeof(g_tzs[g_tz_count].label), "%s", g_cfg_tzs[i].label);
                g_tz_count++;
            }
        }
    }

    if (g_tz_count == 0) {
        snprintf(g_tzs[0].tz, sizeof(g_tzs[0].tz), "%s", det_tz);
        g_tzs[0].label[0] = '\0';
        g_tz_count = 1;
    }

    if (g_tz_idx >= g_tz_count) {
        g_tz_idx = 0;
    }

    pthread_mutex_unlock(&g_mutex);
    update_active_loc_prefixes();
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

    /* Emoji is shown only for non-default locations (g_tz_idx != 0) */
    if (g_tz_idx != 0 && cur_lbl && *cur_lbl) {
        snprintf(bar_out, bar_sz, "%s %s", dt_buf, cur_lbl);
    } else {
        snprintf(bar_out, bar_sz, "%s", dt_buf);
    }

    /* Build tooltip: Hover only shows configured locations. All of them. All with emojis, if provided. */
    size_t off = 0;
    int tip_cnt = (g_cfg_tz_count > 0) ? g_cfg_tz_count : g_tz_count;
    for (int i = 0; i < tip_cnt; i++) {
        const char *tz = (g_cfg_tz_count > 0) ? g_cfg_tzs[i].tz : g_tzs[i].tz;
        const char *lbl = (g_cfg_tz_count > 0) ? g_cfg_tzs[i].label : g_tzs[i].label;

        struct tm tm_z;
        format_tz_time(tz, now, &tm_z);
        char tbuf[16];
        strftime(tbuf, sizeof(tbuf), "%H:%M", &tm_z);

        if (i > 0) {
            off += snprintf(tip_out + off, tip_sz - off, " | ");
        }

        if (lbl && *lbl) {
            off += snprintf(tip_out + off, tip_sz - off, "%s %s", tbuf, lbl);
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
            off += snprintf(tip_out + off, tip_sz - off, "%*s<span color='#%06x'><b>%s</b></span>%*s%s",
                            pad_l, "", (unsigned)(COL_CLD_MONTH & 0xFFFFFFU), mn, pad_r, "", (col_m < 2) ? "   " : "\n");
        }
        /* Weekday headers */
        for (int col_m = 0; col_m < 3; col_m++) {
            off += snprintf(tip_out + off, tip_sz - off,
                            "   <span color='#%06x'>Mo Tu We Th Fr Sa Su</span>%s",
                            (unsigned)(COL_CLD_WKDAY & 0xFFFFFFU),
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
                off += snprintf(tip_out + off, tip_sz - off, "<span color='#%06x'>%2s</span> ", (unsigned)(COL_CLD_WKDAY & 0xFFFFFFU), wk_str);

                for (int dow = 0; dow < 7; dow++) {
                    int d = start_day + dow;
                    if (d < 1 || d > dim) {
                        off += snprintf(tip_out + off, tip_sz - off, "  %s", (dow < 6) ? " " : "");
                    } else if (m == today_m && d == today_d) {
                        off += snprintf(tip_out + off, tip_sz - off,
                                        "<span background='#%06x' color='#%06x'><b>%2d</b></span>%s",
                                        (unsigned)(COL_CLD_TODAY_BG & 0xFFFFFFU), (unsigned)(COL_CLD_TODAY_FG & 0xFFFFFFU),
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
    char *buf = malloc(rlen + 1);
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

static bool refresh_sway_state_ex(bool check_tree) {
    int fd = sway_ipc_connect();
    if (fd < 0) return false;

    char *ws_json = sway_ipc_roundtrip(fd, 1 /* GET_WORKSPACES */, "");
    char *tree_json = check_tree ? sway_ipc_roundtrip(fd, 4 /* GET_TREE */, "") : NULL;
    char *mode_json = sway_ipc_roundtrip(fd, 12 /* GET_BINDING_STATE */, "");
    close(fd);

    bool changed = false;
    pthread_mutex_lock(&g_mutex);
    if (ws_json) {
        struct json_object *root = json_tokener_parse(ws_json);
        if (root && json_object_is_type(root, json_type_array)) {
            int len = json_object_array_length(root);
            WorkspaceItem new_ws[32];
            int new_cnt = 0;
            memset(new_ws, 0, sizeof(new_ws));
            for (int i = 0; i < len && new_cnt < 32; i++) {
                struct json_object *item = json_object_array_get_idx(root, i);
                struct json_object *j_name = NULL, *j_num = NULL, *j_foc = NULL, *j_urg = NULL;
                if (json_object_object_get_ex(item, "name", &j_name)) {
                    snprintf(new_ws[new_cnt].name, sizeof(new_ws[0].name),
                             "%s", json_object_get_string(j_name));
                    new_ws[new_cnt].num =
                        json_object_object_get_ex(item, "num", &j_num) ? json_object_get_int(j_num) : -1;
                    new_ws[new_cnt].focused =
                        json_object_object_get_ex(item, "focused", &j_foc) && json_object_get_boolean(j_foc);
                    new_ws[new_cnt].urgent =
                        json_object_object_get_ex(item, "urgent", &j_urg) && json_object_get_boolean(j_urg);
                    new_cnt++;
                }
            }
            if (new_cnt != g_ws_count) {
                changed = true;
            } else {
                for (int i = 0; i < new_cnt; i++) {
                    if (strcmp(new_ws[i].name, g_workspaces[i].name) != 0 ||
                        new_ws[i].num != g_workspaces[i].num ||
                        new_ws[i].focused != g_workspaces[i].focused ||
                        new_ws[i].urgent != g_workspaces[i].urgent) {
                        changed = true;
                        break;
                    }
                }
            }
            if (changed) {
                for (int i = 0; i < new_cnt; i++) {
                    snprintf(g_workspaces[i].name, sizeof(g_workspaces[0].name), "%s", new_ws[i].name);
                    g_workspaces[i].num = new_ws[i].num;
                    g_workspaces[i].focused = new_ws[i].focused;
                    g_workspaces[i].urgent = new_ws[i].urgent;
                }
                g_ws_count = new_cnt;
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
            char new_tip[2048];
            new_tip[0] = '\0';
            find_i3_scratch(troot, &cnt, new_tip, sizeof(new_tip), &off);
            if (cnt != g_scratch_count || strcmp(new_tip, g_scratch_tooltip) != 0) {
                g_scratch_count = cnt;
                snprintf(g_scratch_tooltip, sizeof(g_scratch_tooltip), "%s", new_tip);
                changed = true;
            }
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
                const char *target = (strcmp(mname, "default") == 0) ? "" : mname;
                if (strcmp(g_sway_mode, target) != 0) {
                    snprintf(g_sway_mode, sizeof(g_sway_mode), "%s", target);
                    changed = true;
                }
            }
            json_object_put(mroot);
        }
        free(mode_json);
    }
    pthread_mutex_unlock(&g_mutex);
    return changed;
}

static void refresh_sway_state(void) {
    refresh_sway_state_ex(true);
}

static void *sway_event_thread(void *arg) {
    (void)arg;
    while (1) {
        refresh_sway_state_ex(true);
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
            uint32_t et = (ev_type & 0x7FFFFFFF);
            bool need_notify = false;
            bool need_refresh = true;
            bool need_tree = false;

            if (et == 3 /* IPC_EVENT_WINDOW */) {
                /* Window focus and non-scratchpad title changes do not affect workspaces or scratchpad */
                if (strstr(ev, "\"change\":\"focus\"") != NULL ||
                    strstr(ev, "\"change\": \"focus\"") != NULL) {
                    free(ev);
                    continue;
                }
                if ((strstr(ev, "\"change\":\"title\"") != NULL ||
                     strstr(ev, "\"change\": \"title\"") != NULL) && g_scratch_count == 0) {
                    free(ev);
                    continue;
                }
                need_tree = true;
            } else if (et == 2 /* IPC_EVENT_MODE */) {
                struct json_object *eroot = json_tokener_parse(ev);
                if (eroot) {
                    struct json_object *j_chg = NULL;
                    if (json_object_object_get_ex(eroot, "change", &j_chg) && json_object_get_string(j_chg)) {
                        const char *chg = json_object_get_string(j_chg);
                        const char *target = (strcmp(chg, "default") == 0) ? "" : chg;
                        pthread_mutex_lock(&g_mutex);
                        if (strcmp(g_sway_mode, target) != 0) {
                            snprintf(g_sway_mode, sizeof(g_sway_mode), "%s", target);
                            need_notify = true;
                        }
                        pthread_mutex_unlock(&g_mutex);
                    }
                    json_object_put(eroot);
                }
                need_refresh = false;
            } else if (et == 4 /* IPC_EVENT_BARCONFIG_UPDATE */) {
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
                        need_notify = true;
                    }
                    json_object_put(eroot);
                }
                need_refresh = false;
            } else if (et == 20 /* IPC_EVENT_BAR_STATE_UPDATE */) {
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
                            need_notify = true;
                        }
                    }
                    json_object_put(eroot);
                }
                need_refresh = false;
            }
            free(ev);
            if (need_refresh && refresh_sway_state_ex(need_tree)) {
                need_notify = true;
            }
            if (need_notify) {
                notify_main_loop();
            }
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
static void poll_gcal_private(void);

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
        len += dbus_build_call(req + len, 6, 0, "org.freedesktop.Notifications",
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

static bool parse_iso_datetime(const char *iso, time_t *out_ts, int *out_h, int *out_m) {
    *out_ts = 0;
    if (out_h) *out_h = 0;
    if (out_m) *out_m = 0;
    if (!iso || !*iso) return false;

    int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) {
        return false;
    }

    struct tm tm_utc;
    memset(&tm_utc, 0, sizeof(tm_utc));
    tm_utc.tm_year = Y - 1900;
    tm_utc.tm_mon = M - 1;
    tm_utc.tm_mday = D;
    tm_utc.tm_hour = h;
    tm_utc.tm_min = m;
    tm_utc.tm_sec = s;
    time_t ts = timegm(&tm_utc);

    const char *t = strchr(iso, 'T');
    if (t) {
        const char *tz_p = t + 1;
        while (*tz_p && *tz_p != 'Z' && *tz_p != 'z' && *tz_p != '+' && *tz_p != '-') {
            tz_p++;
        }
        if (*tz_p == '+' || *tz_p == '-') {
            int zh = 0, zm = 0;
            sscanf(tz_p + 1, "%d:%d", &zh, &zm);
            int off_sec = (zh * 3600 + zm * 60) * (*tz_p == '-' ? -1 : 1);
            ts -= off_sec;
        }
    }
    *out_ts = ts;

    if (out_h || out_m) {
        char tz_buf[64] = "";
        pthread_mutex_lock(&g_mutex);
        snprintf(tz_buf, sizeof(tz_buf), "%s", g_detected_tz);
        pthread_mutex_unlock(&g_mutex);

        struct tm tm_local;
        format_tz_time(tz_buf, ts, &tm_local);
        if (out_h) *out_h = tm_local.tm_hour;
        if (out_m) *out_m = tm_local.tm_min;
    }
    return true;
}

static void format_short_12h(int h, int m, char *out, size_t out_sz) {
    const char *ampm = (h >= 12) ? "pm" : "am";
    int h12 = h % 12;
    if (h12 == 0) h12 = 12;
    if (m == 0) {
        snprintf(out, out_sz, "%d%s", h12, ampm);
    } else {
        snprintf(out, out_sz, "%d:%02d%s", h12, m, ampm);
    }
}

static void format_fixed_range_12h(int sh, int sm, int eh, int em, char *out, size_t out_sz) {
    const char *sampm = (sh >= 12) ? "pm" : "am";
    int sh12 = sh % 12;
    if (sh12 == 0) sh12 = 12;
    const char *eampm = (eh >= 12) ? "pm" : "am";
    int eh12 = eh % 12;
    if (eh12 == 0) eh12 = 12;
    snprintf(out, out_sz, "%2d:%02d%s - %2d:%02d%s", sh12, sm, sampm, eh12, em, eampm);
}

static void format_gcal_mins(int mins, char *out, size_t out_sz) {
    const char *sign = (mins < 0) ? "-" : "";
    long abs_m = (mins < 0) ? -(long)mins : (long)mins;
    long h = abs_m / 60;
    long m = abs_m % 60;
    if (h > 0 && m > 0) {
        snprintf(out, out_sz, "%s%ldh%ldm", sign, h, m);
    } else if (h > 0) {
        snprintf(out, out_sz, "%s%ldh", sign, h);
    } else {
        snprintf(out, out_sz, "%s%ldm", sign, m);
    }
}

static void parse_gcal_location_locked(const char *raw_loc, char *out, size_t out_sz) {
    out[0] = '\0';
    if (!raw_loc || !*raw_loc) return;

    /* 1. Strip "\s*\(\d+\) \[GVC[^\]]*\]" capacity/GVC suffixes */
    char cleaned[512];
    size_t clen = 0;
    const char *p = raw_loc;
    while (*p && clen + 1 < sizeof(cleaned)) {
        if (*p == '(') {
            const char *q = p + 1;
            while (*q && isdigit((unsigned char)*q)) q++;
            if (q > p + 1 && strncmp(q, ") [GVC", 6) == 0) {
                const char *close_br = strchr(q + 6, ']');
                if (close_br) {
                    while (clen > 0 && cleaned[clen - 1] == ' ') clen--;
                    p = close_br + 1;
                    continue;
                }
            }
        }
        cleaned[clen++] = *p++;
    }
    cleaned[clen] = '\0';

    /* 2. Split comma-separated locations and trim whitespace */
    char tokens[16][128];
    int tok_cnt = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(cleaned, ",", &saveptr);
    while (tok && tok_cnt < 16) {
        while (*tok && isspace((unsigned char)*tok)) tok++;
        size_t tlen = strlen(tok);
        while (tlen > 0 && isspace((unsigned char)tok[tlen - 1])) tok[--tlen] = '\0';
        if (tlen > 0) {
            snprintf(tokens[tok_cnt], sizeof(tokens[0]), "%s", tok);
            tok_cnt++;
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }
    if (tok_cnt == 0) return;

    /* 3. Match against configured relevant location prefixes/regexes in priority order (cap at 3 locations) */
    for (int pr = 0; pr < g_gcal_loc_prefix_count; pr++) {
        const char *pref = g_gcal_loc_prefixes[pr];
        size_t plen = strlen(pref);
        if (plen == 0) continue;

        int matched = 0;
        size_t off = 0;
        for (int t = 0; t < tok_cnt && matched < 3; t++) {
            const char *sub = NULL;
            regmatch_t pm;
            if (g_gcal_loc_has_re[pr]) {
                if (regexec(&g_gcal_loc_re[pr], tokens[t], 1, &pm, 0) == 0 && pm.rm_so == 0 && pm.rm_eo > 0) {
                    sub = tokens[t] + pm.rm_eo;
                }
            } else if (strncmp(tokens[t], pref, plen) == 0) {
                sub = tokens[t] + plen;
            }
            if (sub) {
                while (*sub && isspace((unsigned char)*sub)) sub++;
                if (*sub != '\0') {
                    off += snprintf(out + off, out_sz - off, "%s%s",
                                    (matched > 0) ? ", " : "", sub);
                    matched++;
                }
            }
        }
        if (matched > 0) return;
    }

    /* 4. Fallback: join up to 3 cleaned locations */
    size_t off = 0;
    for (int t = 0; t < tok_cnt && t < 3; t++) {
        off += snprintf(out + off, out_sz - off, "%s%s", (t > 0) ? ", " : "", tokens[t]);
    }
}

static bool gcal_rsvp_accepted(struct json_object *it) {
    bool i_organized = false;
    struct json_object *j_org = NULL, *j_self = NULL;
    if (json_object_object_get_ex(it, "organizer", &j_org) && j_org &&
        json_object_object_get_ex(j_org, "self", &j_self)) {
        i_organized = json_object_get_boolean(j_self);
    }

    struct json_object *j_att = NULL;
    if (!json_object_object_get_ex(it, "attendees", &j_att) ||
        !json_object_is_type(j_att, json_type_array)) {
        return i_organized;
    }
    int alen = json_object_array_length(j_att);
    if (alen == 0) return i_organized;

    for (int a = 0; a < alen; a++) {
        struct json_object *att = json_object_array_get_idx(j_att, a);
        struct json_object *a_self = NULL, *a_resp = NULL;
        if (att && json_object_object_get_ex(att, "self", &a_self) && json_object_get_boolean(a_self)) {
            if (json_object_object_get_ex(att, "responseStatus", &a_resp) && json_object_get_string(a_resp)) {
                return strcmp(json_object_get_string(a_resp), "accepted") == 0;
            }
            return false;
        }
    }
    return false;
}

static void url_encode_cal_id(const char *in, char *out, size_t out_sz) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 4 < out_sz; i++) {
        if (in[i] == '@') {
            memcpy(out + j, "%40", 3);
            j += 3;
        } else {
            out[j++] = in[i];
        }
    }
    out[j] = '\0';
}

static char *fetch_gcal_calendar_json(const char *cal_id, const char *min_buf, const char *max_buf) {
    char enc_cal[256];
    url_encode_cal_id(cal_id, enc_cal, sizeof(enc_cal));

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "curl -s --max-time 10 -H 'Authorization: Bearer %s' "
             "'https://www.googleapis.com/calendar/v3/calendars/%s/events?timeMin=%s&timeMax=%s&singleEvents=true&orderBy=startTime'",
             g_gcal_access_token, enc_cal, min_buf, max_buf);

    char *resp = run_cmd_capture(cmd, 131072);
    if (resp && strstr(resp, "\"UNAUTHENTICATED\"")) {
        free(resp);
        if (refresh_gcal_token()) {
            snprintf(cmd, sizeof(cmd),
                     "curl -s --max-time 10 -H 'Authorization: Bearer %s' "
                     "'https://www.googleapis.com/calendar/v3/calendars/%s/events?timeMin=%s&timeMax=%s&singleEvents=true&orderBy=startTime'",
                     g_gcal_access_token, enc_cal, min_buf, max_buf);
            resp = run_cmd_capture(cmd, 131072);
        } else {
            resp = NULL;
        }
    }
    return resp;
}

static bool parse_iso_date_local(const char *date_str, time_t *out_ts) {
    *out_ts = 0;
    if (!date_str || !*date_str) return false;
    int Y = 0, M = 0, D = 0;
    if (sscanf(date_str, "%d-%d-%d", &Y, &M, &D) != 3) return false;
    struct tm tm_loc;
    memset(&tm_loc, 0, sizeof(tm_loc));
    tm_loc.tm_year = Y - 1900;
    tm_loc.tm_mon = M - 1;
    tm_loc.tm_mday = D;
    tm_loc.tm_isdst = -1;
    pthread_mutex_lock(&g_tz_mutex);
    *out_ts = mktime(&tm_loc);
    pthread_mutex_unlock(&g_tz_mutex);
    return true;
}

static bool is_gcal_ignored_event(const char *sum) {
    if (!sum || !sum[0]) return false;
    for (int i = 0; i < g_gcal_ignored_event_count; i++) {
        const char *pat = g_gcal_ignored_events[i];
        if (!pat[0]) continue;
        if (strcasecmp(sum, pat) == 0) return true;

        /* If pattern contains regex metacharacters, match with regex */
        if (strpbrk(pat, "^$*+?[]|()\\")) {
            char rpat[160];
            if (pat[0] == '^') snprintf(rpat, sizeof(rpat), "%s", pat);
            else snprintf(rpat, sizeof(rpat), "^(%s)", pat);
            regex_t re;
            if (regcomp(&re, rpat, REG_EXTENDED | REG_ICASE | REG_NOSUB) == 0) {
                int res = regexec(&re, sum, 0, NULL, 0);
                regfree(&re);
                if (res == 0) return true;
            }
        } else {
            /* Prefix match if followed by separator like " - ", ": ", " (", etc. */
            size_t plen = strlen(pat);
            if (strncasecmp(sum, pat, plen) == 0) {
                const char *rem = sum + plen;
                while (*rem == ' ') rem++;
                if (*rem == '-' || *rem == ':' || *rem == '(' || *rem == '[' || *rem == '\0') {
                    return true;
                }
                /* If pattern itself is "Commute" (case-insensitive), also match "Commute to/from ..." */
                if (strcasecmp(pat, "Commute") == 0 && sum[plen] == ' ') {
                    return true;
                }
            }
        }
    }
    return false;
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

    GCalEvent new_evs[MAX_GCAL_EVENTS];
    int new_cnt = 0;

    for (int c = 0; c < g_gcal_cal_count && new_cnt < MAX_GCAL_EVENTS; c++) {
        char *resp = fetch_gcal_calendar_json(g_gcal_calendars[c], min_buf, max_buf);
        if (!resp) continue;

        struct json_object *root = json_tokener_parse(resp);
        struct json_object *items = NULL;
        if (root && json_object_object_get_ex(root, "items", &items) && json_object_is_type(items, json_type_array)) {
            int len = json_object_array_length(items);

            for (int i = 0; i < len && new_cnt < MAX_GCAL_EVENTS; i++) {
                struct json_object *it = json_object_array_get_idx(items, i);
                if (!it) continue;

                struct json_object *j_status = NULL;
                if (json_object_object_get_ex(it, "status", &j_status) && json_object_get_string(j_status) &&
                    strcasecmp(json_object_get_string(j_status), "cancelled") == 0) {
                    continue;
                }

                struct json_object *j_sum = NULL, *j_start = NULL, *j_end = NULL, *j_dt = NULL, *j_loc = NULL;
                const char *raw_sum = (json_object_object_get_ex(it, "summary", &j_sum) && json_object_get_string(j_sum))
                                          ? json_object_get_string(j_sum)
                                          : "";
                char sum[128];
                snprintf(sum, sizeof(sum), "%s", raw_sum);
                /* Trim whitespace */
                char *s_start = sum;
                while (*s_start && isspace((unsigned char)*s_start)) s_start++;
                if (s_start != sum) memmove(sum, s_start, strlen(s_start) + 1);
                size_t slen = strlen(sum);
                while (slen > 0 && isspace((unsigned char)sum[slen - 1])) sum[--slen] = '\0';

                bool is_filtered = false;
                if (strcasestr(sum, "CANCELLED") || strcasestr(sum, "[TBC]")) {
                    is_filtered = true;
                }
                if (!gcal_rsvp_accepted(it)) {
                    is_filtered = true;
                }
                if (is_gcal_ignored_event(sum)) {
                    is_filtered = true;
                }

                if (!json_object_object_get_ex(it, "start", &j_start) ||
                    !json_object_object_get_ex(it, "end", &j_end)) {
                    continue;
                }

                time_t start_ts = 0, end_ts = 0;
                bool is_all_day = false;

                if (json_object_object_get_ex(j_start, "date", &j_dt) && json_object_get_string(j_dt)) {
                    const char *s_date = json_object_get_string(j_dt);
                    struct json_object *j_edt = NULL;
                    const char *e_date = (json_object_object_get_ex(j_end, "date", &j_edt) && json_object_get_string(j_edt))
                                             ? json_object_get_string(j_edt)
                                             : s_date;
                    if (!parse_iso_date_local(s_date, &start_ts) || !parse_iso_date_local(e_date, &end_ts)) {
                        continue;
                    }
                    if (end_ts <= start_ts) end_ts = start_ts + 86400;
                    is_all_day = true;
                    is_filtered = true;
                } else if (json_object_object_get_ex(j_start, "dateTime", &j_dt) && json_object_get_string(j_dt)) {
                    const char *start_iso = json_object_get_string(j_dt);
                    struct json_object *j_edt = NULL;
                    if (!json_object_object_get_ex(j_end, "dateTime", &j_edt) || !json_object_get_string(j_edt)) {
                        continue;
                    }
                    const char *end_iso = json_object_get_string(j_edt);
                    if (!parse_iso_datetime(start_iso, &start_ts, NULL, NULL) ||
                        !parse_iso_datetime(end_iso, &end_ts, NULL, NULL)) {
                        continue;
                    }
                } else {
                    continue;
                }

                if (end_ts <= now) continue;
                if (g_gcal_max_duration_mins > 0 &&
                    (end_ts - start_ts) > (time_t)g_gcal_max_duration_mins * 60) {
                    is_filtered = true;
                }

                /* Deduplicate by (summary, start_ts, end_ts) across calendars */
                bool dup = false;
                for (int k = 0; k < new_cnt; k++) {
                    if (strcmp(new_evs[k].summary, sum) == 0 &&
                        new_evs[k].start_ts == start_ts &&
                        new_evs[k].end_ts == end_ts) {
                        if (new_evs[k].filtered && !is_filtered) {
                            new_evs[k].filtered = false;
                        }
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;

                GCalEvent *ev = &new_evs[new_cnt];
                memset(ev, 0, sizeof(*ev));
                snprintf(ev->summary, sizeof(ev->summary), "%s", sum);
                ev->start_ts = start_ts;
                ev->end_ts = end_ts;
                ev->is_all_day = is_all_day;
                ev->filtered = is_filtered;
                if (is_all_day) {
                    snprintf(ev->start_short, sizeof(ev->start_short), "all-day");
                    snprintf(ev->time_range, sizeof(ev->time_range), "     all-day     ");
                }

                const char *loc = (json_object_object_get_ex(it, "location", &j_loc) && json_object_get_string(j_loc))
                                      ? json_object_get_string(j_loc)
                                      : "";
                snprintf(ev->raw_location, sizeof(ev->raw_location), "%s", loc);
                new_cnt++;
            }
        }
        if (root) json_object_put(root);
        free(resp);
    }

    /* Sort combined events by start_ts ascending */
    for (int i = 0; i < new_cnt - 1; i++) {
        for (int j = i + 1; j < new_cnt; j++) {
            if (new_evs[j].start_ts < new_evs[i].start_ts ||
                (new_evs[j].start_ts == new_evs[i].start_ts && !new_evs[j].filtered && new_evs[i].filtered) ||
                (new_evs[j].start_ts == new_evs[i].start_ts && new_evs[j].filtered == new_evs[i].filtered &&
                 new_evs[j].end_ts < new_evs[i].end_ts)) {
                GCalEvent tmp = new_evs[i];
                new_evs[i] = new_evs[j];
                new_evs[j] = tmp;
            }
        }
    }

    update_active_loc_prefixes();
    pthread_mutex_lock(&g_mutex);
    char cur_tz[64] = "";
    if (g_detected_tz[0]) snprintf(cur_tz, sizeof(cur_tz), "%s", g_detected_tz);
    else get_system_tz(cur_tz, sizeof(cur_tz));
    for (int i = 0; i < new_cnt; i++) {
        GCalEvent *ev = &new_evs[i];
        parse_gcal_location_locked(ev->raw_location, ev->location, sizeof(ev->location));
        if (!ev->is_all_day && ev->start_ts > 0 && ev->end_ts > 0) {
            struct tm tm_s, tm_e;
            format_tz_time(cur_tz, ev->start_ts, &tm_s);
            format_tz_time(cur_tz, ev->end_ts, &tm_e);
            format_short_12h(tm_s.tm_hour, tm_s.tm_min, ev->start_short, sizeof(ev->start_short));
            format_fixed_range_12h(tm_s.tm_hour, tm_s.tm_min, tm_e.tm_hour, tm_e.tm_min,
                                   ev->time_range, sizeof(ev->time_range));
        }
    }
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

static double parse_iso6709_part(const char *s, int len) {
    if (len <= 0) return 0.0;
    double sign = 1.0;
    if (*s == '+') { s++; len--; }
    else if (*s == '-') { sign = -1.0; s++; len--; }

    double deg = 0, min = 0, sec = 0;
    if (len == 4) { /* DDMM */
        char b[3] = {s[0], s[1], 0}; deg = atof(b);
        min = atof(s + 2);
    } else if (len == 5) { /* DDDMM */
        char b[4] = {s[0], s[1], s[2], 0}; deg = atof(b);
        min = atof(s + 3);
    } else if (len == 6) { /* DDMMSS */
        char b[3] = {s[0], s[1], 0}; deg = atof(b);
        char m[3] = {s[2], s[3], 0}; min = atof(m);
        sec = atof(s + 4);
    } else if (len >= 7) { /* DDDMMSS */
        char b[4] = {s[0], s[1], s[2], 0}; deg = atof(b);
        char m[3] = {s[3], s[4], 0}; min = atof(m);
        sec = atof(s + 5);
    } else {
        deg = atof(s);
    }
    return sign * (deg + min / 60.0 + sec / 3600.0);
}

static bool parse_iso6709(const char *coord, double *lat, double *lon) {
    if (!coord || !*coord) return false;
    const char *p = coord + 1;
    while (*p && *p != '+' && *p != '-') p++;
    if (!*p) return false;
    int lat_len = (int)(p - coord);
    int lon_len = (int)strlen(p);
    *lat = parse_iso6709_part(coord, lat_len);
    *lon = parse_iso6709_part(p, lon_len);
    return true;
}

static bool lookup_timezone_from_coords(double lat, double lon, char *out_tz, size_t sz) {
    const char *files[] = {"/usr/share/zoneinfo/zone1970.tab", "/usr/share/zoneinfo/zone.tab", NULL};
    FILE *f = NULL;
    for (int i = 0; files[i]; i++) {
        f = fopen(files[i], "r");
        if (f) break;
    }
    if (!f) {
        /* Fallback: synthesize POSIX Etc/GMT timezone from longitude */
        int gmt_offset = (int)round(lon / 15.0);
        if (gmt_offset == 0) snprintf(out_tz, sz, "Etc/UTC");
        else if (gmt_offset > 0) snprintf(out_tz, sz, "Etc/GMT-%d", gmt_offset);
        else snprintf(out_tz, sz, "Etc/GMT+%d", -gmt_offset);
        return true;
    }

    char line[512];
    double best_dist2 = 1e18;
    char best_tz[128] = "";
    double cos_lat = cos(lat * M_PI / 180.0);

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *tab1 = strchr(line, '\t');
        if (!tab1) continue;
        char *coord = tab1 + 1;
        char *tab2 = strchr(coord, '\t');
        if (!tab2) continue;
        *tab2 = '\0';
        char *tz = tab2 + 1;
        char *tab3 = strchr(tz, '\t');
        if (tab3) *tab3 = '\0';
        char *nl = strchr(tz, '\n');
        if (nl) *nl = '\0';
        char *cr = strchr(tz, '\r');
        if (cr) *cr = '\0';

        double clat = 0, clon = 0;
        if (!parse_iso6709(coord, &clat, &clon)) continue;

        double dlat = lat - clat;
        double dlon = (lon - clon) * cos_lat;
        double dist2 = dlat * dlat + dlon * dlon;
        if (dist2 < best_dist2) {
            best_dist2 = dist2;
            snprintf(best_tz, sizeof(best_tz), "%s", tz);
        }
    }
    fclose(f);

    if (best_tz[0]) {
        snprintf(out_tz, sz, "%s", best_tz);
        return true;
    }
    return false;
}

static bool parse_geoloc_json(const char *resp, double *out_lat, double *out_lon,
                              char *out_tz, size_t tz_sz, char *out_city, size_t city_sz) {
    if (!resp || !*resp) return false;
    struct json_object *root = json_tokener_parse(resp);
    if (!root) return false;

    bool found = false;
    struct json_object *obj_lat = NULL, *obj_lon = NULL, *obj_loc = NULL;
    struct json_object *obj_tz = NULL, *obj_city = NULL;

    if (json_object_object_get_ex(root, "latitude", &obj_lat) &&
        json_object_object_get_ex(root, "longitude", &obj_lon)) {
        *out_lat = json_object_get_double(obj_lat);
        *out_lon = json_object_get_double(obj_lon);
        found = true;
    } else if (json_object_object_get_ex(root, "lat", &obj_lat) &&
               json_object_object_get_ex(root, "lon", &obj_lon)) {
        *out_lat = json_object_get_double(obj_lat);
        *out_lon = json_object_get_double(obj_lon);
        found = true;
    } else if (json_object_object_get_ex(root, "loc", &obj_loc)) {
        const char *loc_str = json_object_get_string(obj_loc);
        if (loc_str && sscanf(loc_str, "%lf,%lf", out_lat, out_lon) == 2) {
            found = true;
        }
    }

    if (found) {
        if (out_tz && tz_sz > 0 && json_object_object_get_ex(root, "timezone", &obj_tz)) {
            const char *tz_str = json_object_get_string(obj_tz);
            if (tz_str && *tz_str) snprintf(out_tz, tz_sz, "%s", tz_str);
        }
        if (out_city && city_sz > 0 && json_object_object_get_ex(root, "city", &obj_city)) {
            const char *city_str = json_object_get_string(obj_city);
            if (city_str && *city_str) snprintf(out_city, city_sz, "%s", city_str);
        }
    }

    json_object_put(root);
    return found;
}

static int64_t g_last_cache_mtime_ns = 0;
static int64_t g_last_localtime_mtime_ns = 0;

static int64_t stat_mtime_ns(const struct stat *st) {
    return (int64_t)st->st_mtim.tv_sec * 1000000000LL + (int64_t)st->st_mtim.tv_nsec;
}

static bool get_best_geoloc_cache_path(char *out_path, size_t sz, time_t *out_mtime, int64_t *out_mtime_ns) {
    char user_file[512] = "";
    if (g_cache_dir[0]) {
        snprintf(user_file, sizeof(user_file), "%s/statusbar_geoloc.json", g_cache_dir);
    }
    const char *sys_file = "/var/cache/statusbar_geoloc.json";

    struct stat st_user, st_sys;
    bool has_user = (user_file[0] && stat(user_file, &st_user) == 0 && st_user.st_size > 0);
    bool has_sys = (stat(sys_file, &st_sys) == 0 && st_sys.st_size > 0);

    if (has_user && has_sys) {
        int64_t ns_user = stat_mtime_ns(&st_user);
        int64_t ns_sys = stat_mtime_ns(&st_sys);
        if (ns_sys > ns_user) {
            snprintf(out_path, sz, "%s", sys_file);
            if (out_mtime) *out_mtime = st_sys.st_mtime;
            if (out_mtime_ns) *out_mtime_ns = ns_sys;
        } else {
            snprintf(out_path, sz, "%s", user_file);
            if (out_mtime) *out_mtime = st_user.st_mtime;
            if (out_mtime_ns) *out_mtime_ns = ns_user;
        }
        return true;
    } else if (has_user) {
        snprintf(out_path, sz, "%s", user_file);
        if (out_mtime) *out_mtime = st_user.st_mtime;
        if (out_mtime_ns) *out_mtime_ns = stat_mtime_ns(&st_user);
        return true;
    } else if (has_sys) {
        snprintf(out_path, sz, "%s", sys_file);
        if (out_mtime) *out_mtime = st_sys.st_mtime;
        if (out_mtime_ns) *out_mtime_ns = stat_mtime_ns(&st_sys);
        return true;
    }
    if (out_mtime) *out_mtime = 0;
    if (out_mtime_ns) *out_mtime_ns = 0;
    return false;
}

static void save_geoloc_cache(double lat, double lon, const char *tz, const char *city) {
    if (!g_cache_dir[0]) return;
    mkdir(g_cache_dir, 0755);

    char cache_file[512];
    snprintf(cache_file, sizeof(cache_file), "%s/statusbar_geoloc.json", g_cache_dir);
    FILE *f = fopen(cache_file, "w");
    if (!f) return;
    fprintf(f, "{\"latitude\":%.6f,\"longitude\":%.6f,\"timezone\":\"%s\",\"city\":\"%s\"}\n",
            lat, lon, tz ? tz : "", city ? city : "");
    fclose(f);

    char best_path[512];
    get_best_geoloc_cache_path(best_path, sizeof(best_path), NULL, &g_last_cache_mtime_ns);
}

static bool load_geoloc_cache(void) {
    char cache_file[512];
    int64_t cache_ns = 0;
    if (!get_best_geoloc_cache_path(cache_file, sizeof(cache_file), NULL, &cache_ns)) {
        return false;
    }
    FILE *f = fopen(cache_file, "r");
    if (!f) return false;

    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return false;
    buf[n] = '\0';

    double lat = 0.0, lon = 0.0;
    char tz[64] = "", city[64] = "";
    if (!parse_geoloc_json(buf, &lat, &lon, tz, sizeof(tz), city, sizeof(city))) {
        return false;
    }
    if (fabs(lat) < 1e-4 && fabs(lon) < 1e-4) return false;

    g_last_cache_mtime_ns = cache_ns;
    if (g_last_localtime_mtime_ns == 0) {
        struct stat st_lt;
        if (lstat("/etc/localtime", &st_lt) == 0) {
            g_last_localtime_mtime_ns = stat_mtime_ns(&st_lt);
        }
    }

    pthread_mutex_lock(&g_mutex);
    if (city[0]) {
        snprintf(g_detected_city, sizeof(g_detected_city), "%s", city);
    }
    if (g_namaz_auto_loc) {
        g_namaz_lat = lat;
        g_namaz_lon = lon;
        if (!g_namaz_tz_configured && tz[0]) {
            snprintf(g_namaz_tz, sizeof(g_namaz_tz), "%s", tz);
        }
        if (tz[0]) {
            snprintf(g_detected_tz, sizeof(g_detected_tz), "%s", tz);
        }
        if (!g_namaz_city_configured && city[0]) {
            snprintf(g_namaz_city, sizeof(g_namaz_city), "%s", city);
        }
    } else {
        if (tz[0]) {
            snprintf(g_detected_tz, sizeof(g_detected_tz), "%s", tz);
        }
    }
    pthread_mutex_unlock(&g_mutex);
    return true;
}


static void update_namaz_astronomical(void) {
    pthread_mutex_lock(&g_mutex);
    double lat = g_namaz_lat;
    double lon = g_namaz_lon;
    char tz[128] = "";
    if (g_namaz_tz[0]) {
        snprintf(tz, sizeof(tz), "%s", g_namaz_tz);
    }
    pthread_mutex_unlock(&g_mutex);

    if (fabs(lat) < 1e-4 && fabs(lon) < 1e-4) {
        return;
    }

    if (!tz[0]) {
        lookup_timezone_from_coords(lat, lon, tz, sizeof(tz));
        if (tz[0]) {
            pthread_mutex_lock(&g_mutex);
            if (!g_namaz_tz[0]) snprintf(g_namaz_tz, sizeof(g_namaz_tz), "%s", tz);
            pthread_mutex_unlock(&g_mutex);
        }
    }

    time_t now = time(NULL);
    struct tm tm_target;
    format_tz_time(tz[0] ? tz : NULL, now, &tm_target);

    double utc_offset = (double)tm_target.tm_gmtoff / 3600.0;
    int y = tm_target.tm_year + 1900;
    int m = tm_target.tm_mon + 1;
    int d = tm_target.tm_mday;

    NamazPrayerTimes pt = namaz_calculate(y, m, d, lat, lon, utc_offset);

    char today_prayers[6][16];
    char tom_fajr[16];

    namaz_format_hm(pt.fajr, utc_offset, today_prayers[0], sizeof(today_prayers[0]));
    namaz_format_hm(pt.sunrise, utc_offset, today_prayers[1], sizeof(today_prayers[1]));
    namaz_format_hm(pt.dhuhr, utc_offset, today_prayers[2], sizeof(today_prayers[2]));
    namaz_format_hm(pt.asr, utc_offset, today_prayers[3], sizeof(today_prayers[3]));
    namaz_format_hm(pt.maghrib, utc_offset, today_prayers[4], sizeof(today_prayers[4]));
    namaz_format_hm(pt.isha, utc_offset, today_prayers[5], sizeof(today_prayers[5]));

    /* Tomorrow Fajr in target timezone */
    time_t tom = now + 86400;
    struct tm tm_tom;
    format_tz_time(tz[0] ? tz : NULL, tom, &tm_tom);

    int ty = tm_tom.tm_year + 1900;
    int tm_m = tm_tom.tm_mon + 1;
    int td = tm_tom.tm_mday;
    double tom_offset = (double)tm_tom.tm_gmtoff / 3600.0;
    NamazPrayerTimes pt_tom = namaz_calculate(ty, tm_m, td, lat, lon, tom_offset);
    namaz_format_hm(pt_tom.fajr, tom_offset, tom_fajr, sizeof(tom_fajr));

    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < 6; i++) {
        snprintf(g_namaz_today[i], sizeof(g_namaz_today[i]), "%s", today_prayers[i]);
    }
    snprintf(g_namaz_tom_fajr, sizeof(g_namaz_tom_fajr), "%s", tom_fajr);
    g_namaz_loaded_mday = d;
    g_namaz_loaded = true;
    pthread_mutex_unlock(&g_mutex);
}

static bool fetch_geolocation_auto(bool force_network) {
    if (!force_network) {
        char cache_file[512];
        time_t cache_mtime = 0;
        if (get_best_geoloc_cache_path(cache_file, sizeof(cache_file), &cache_mtime, NULL) &&
            (time(NULL) - cache_mtime) < 1800 && load_geoloc_cache()) {
            update_active_clocks();
            update_namaz_astronomical();
            notify_main_loop();
            return true;
        }
    }

    const char *endpoints[] = {
        "curl -s --max-time 5 -H 'User-Agent: statusbar/1.0' https://ipapi.co/json/",
        "curl -s --max-time 5 -H 'User-Agent: statusbar/1.0' http://ip-api.com/json",
        "curl -s --max-time 5 -H 'User-Agent: statusbar/1.0' https://ipinfo.io/json",
        NULL
    };

    double lat = 0.0, lon = 0.0;
    char tz[64] = "", city[64] = "";
    bool ok = false;

    for (int i = 0; endpoints[i]; i++) {
        char *resp = run_cmd_capture(endpoints[i], 4096);
        if (resp) {
            if (parse_geoloc_json(resp, &lat, &lon, tz, sizeof(tz), city, sizeof(city))) {
                if (fabs(lat) > 1e-4 || fabs(lon) > 1e-4) {
                    ok = true;
                    free(resp);
                    break;
                }
            }
            free(resp);
        }
    }

    if (!ok) {
        if (load_geoloc_cache()) {
            update_active_clocks();
            update_namaz_astronomical();
            notify_main_loop();
            return true;
        }
        return false;
    }

    if (!tz[0]) {
        lookup_timezone_from_coords(lat, lon, tz, sizeof(tz));
    }

    save_geoloc_cache(lat, lon, tz, city);

    pthread_mutex_lock(&g_mutex);
    if (city[0]) {
        snprintf(g_detected_city, sizeof(g_detected_city), "%s", city);
    }
    g_namaz_lat = lat;
    g_namaz_lon = lon;
    if (!g_namaz_tz_configured && tz[0]) {
        snprintf(g_namaz_tz, sizeof(g_namaz_tz), "%s", tz);
    }
    if (tz[0]) {
        snprintf(g_detected_tz, sizeof(g_detected_tz), "%s", tz);
    }
    if (!g_namaz_city_configured && city[0]) {
        snprintf(g_namaz_city, sizeof(g_namaz_city), "%s", city);
    }
    pthread_mutex_unlock(&g_mutex);

    update_active_clocks();
    update_namaz_astronomical();
    notify_main_loop();
    return true;
}

static void *geoloc_fetch_thread(void *arg) {
    (void)arg;
    fetch_geolocation_auto(true);
    return NULL;
}

static void trigger_geoloc_fetch_async(void) {
    pthread_t th;
    pthread_create(&th, NULL, geoloc_fetch_thread, NULL);
    pthread_detach(th);
}

static time_t g_last_tz_check_ts = 0;

static void check_location_and_timezone(bool force) {
    time_t now = time(NULL);
    if (!force && g_last_tz_check_ts != 0 && (now - g_last_tz_check_ts) <= 30) {
        return;
    }
    g_last_tz_check_ts = now;

    char cache_file[512];
    int64_t cur_cache_ns = 0;
    get_best_geoloc_cache_path(cache_file, sizeof(cache_file), NULL, &cur_cache_ns);

    struct stat st_lt;
    int64_t cur_lt_ns = 0;
    if (lstat("/etc/localtime", &st_lt) == 0) {
        cur_lt_ns = stat_mtime_ns(&st_lt);
    }

    bool changed = false;
    bool cache_changed = (cur_cache_ns > 0 && cur_cache_ns != g_last_cache_mtime_ns);
    bool lt_changed = (cur_lt_ns > 0 && cur_lt_ns != g_last_localtime_mtime_ns);

    if (cache_changed) {
        if (load_geoloc_cache()) {
            changed = true;
        }
        if (lt_changed) {
            g_last_localtime_mtime_ns = cur_lt_ns;
        }
    } else if (lt_changed || cur_cache_ns == 0) {
        g_last_localtime_mtime_ns = cur_lt_ns;
        char sys_tz[64] = "";
        get_system_tz(sys_tz, sizeof(sys_tz));
        pthread_mutex_lock(&g_mutex);
        if (sys_tz[0] && strcmp(g_detected_tz, sys_tz) != 0) {
            if (!is_same_timezone(g_detected_tz, sys_tz)) {
                g_detected_city[0] = '\0';
            }
            snprintf(g_detected_tz, sizeof(g_detected_tz), "%s", sys_tz);
            if (g_namaz_auto_loc && !g_namaz_tz_configured) {
                snprintf(g_namaz_tz, sizeof(g_namaz_tz), "%s", sys_tz);
            }
            changed = true;
        }
        pthread_mutex_unlock(&g_mutex);
    }

    if (changed) {
        update_active_clocks();
        update_namaz_astronomical();
        notify_main_loop();
    }
}


static void build_namaz_strings(char *bar_out, size_t bar_sz, char *tip_out, size_t tip_sz) {
    if (!g_namaz_loaded) {
        update_namaz_astronomical();
    }

    pthread_mutex_lock(&g_mutex);
    char tz[128] = "";
    if (g_namaz_tz[0]) snprintf(tz, sizeof(tz), "%s", g_namaz_tz);
    pthread_mutex_unlock(&g_mutex);

    time_t now = time(NULL);
    struct tm tm_now;
    format_tz_time(tz[0] ? tz : NULL, now, &tm_now);

    pthread_mutex_lock(&g_mutex);
    if (g_namaz_loaded && tm_now.tm_mday != g_namaz_loaded_mday) {
        pthread_mutex_unlock(&g_mutex);
        update_namaz_astronomical();
        pthread_mutex_lock(&g_mutex);
    }

    if (!g_namaz_loaded) {
        snprintf(bar_out, bar_sz, "Calculating...");
        snprintf(tip_out, tip_sz, "Calculating prayer times...");
        pthread_mutex_unlock(&g_mutex);
        return;
    }

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
static void sync_mute_led(const char *path, bool muted) {
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        char buf[2] = { (char)('0' + (muted ? 1 : 0)), '\n' };
        (void)write(fd, buf, 2);
        close(fd);
    }
}

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

        static int last_sink_muted = -1;
        if ((int)muted != last_sink_muted) {
            sync_mute_led("/sys/class/leds/platform::mute/brightness", muted);
            last_sink_muted = (int)muted;
        }
    }
    free(out);

    if (access("/sys/class/leds/platform::micmute/brightness", W_OK) == 0) {
        char mic_cmd[256];
        snprintf(mic_cmd, sizeof(mic_cmd), "XDG_RUNTIME_DIR=%s wpctl get-volume @DEFAULT_AUDIO_SOURCE@ 2>/dev/null", g_runtime_dir);
        char *mic_out = run_cmd_capture(mic_cmd, 256);
        if (mic_out && strncmp(mic_out, "Volume:", 7) == 0) {
            bool mic_muted = (strstr(mic_out, "[MUTED]") != NULL);
            static int last_mic_muted = -1;
            if ((int)mic_muted != last_mic_muted) {
                sync_mute_led("/sys/class/leds/platform::micmute/brightness", mic_muted);
                last_mic_muted = (int)mic_muted;
            }
        }
        free(mic_out);
    }
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

static void sync_idle_inhibitor(void);

static void poll_screensaver(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/statusbar.inhibit", g_runtime_dir);
    bool inhibited = (access(path, F_OK) == 0);
    pthread_mutex_lock(&g_mutex);
    g_screensaver_active = !inhibited;
    pthread_mutex_unlock(&g_mutex);
}

static void toggle_screensaver(void) {
    bool active;
    pthread_mutex_lock(&g_mutex);
    g_screensaver_active = !g_screensaver_active;
    active = g_screensaver_active;
    pthread_mutex_unlock(&g_mutex);

    char path[512];
    snprintf(path, sizeof(path), "%s/statusbar.inhibit", g_runtime_dir);
    if (active) {
        unlink(path);
    } else {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd >= 0) close(fd);
    }
    sync_idle_inhibitor();
}

static void poll_gcal_private(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/statusbar.gcal_private", g_runtime_dir);
    bool is_priv = (access(path, F_OK) == 0);
    pthread_mutex_lock(&g_mutex);
    g_gcal_private = is_priv;
    pthread_mutex_unlock(&g_mutex);
}

static void toggle_gcal_private(void) {
    bool priv;
    pthread_mutex_lock(&g_mutex);
    g_gcal_private = !g_gcal_private;
    priv = g_gcal_private;
    pthread_mutex_unlock(&g_mutex);

    char path[512];
    snprintf(path, sizeof(path), "%s/statusbar.gcal_private", g_runtime_dir);
    if (!priv) {
        unlink(path);
    } else {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd >= 0) close(fd);
    }
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
    int ac_found = 0;
    int ac_online = 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (strncmp(de->d_name, "BAT", 3) == 0) {
            if (!bat_dir[0]) {
                snprintf(bat_dir, sizeof(bat_dir), "/sys/class/power_supply/%s", de->d_name);
            }
        } else {
            char pdir[512];
            snprintf(pdir, sizeof(pdir), "/sys/class/power_supply/%s", de->d_name);
            long onl = read_sysfs_long(pdir, "online");
            if (onl >= 0) {
                ac_found = 1;
                if (onl > 0) ac_online = 1;
            }
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

    long stop_thresh = read_sysfs_long(bat_dir, "charge_control_end_threshold");
    if (stop_thresh <= 0 || stop_thresh > 100)
        stop_thresh = read_sysfs_long(bat_dir, "charge_stop_threshold");
    if (stop_thresh <= 0 || stop_thresh > 100)
        stop_thresh = 100;

    long e_now = read_sysfs_long(bat_dir, "energy_now");
    if (e_now < 0) e_now = read_sysfs_long(bat_dir, "charge_now");
    long e_full = read_sysfs_long(bat_dir, "energy_full");
    if (e_full < 0) e_full = read_sysfs_long(bat_dir, "charge_full");
    long p_now = read_sysfs_long(bat_dir, "power_now");
    if (p_now < 0) p_now = read_sysfs_long(bat_dir, "current_now");

    pthread_mutex_lock(&g_mutex);
    /* Latch Full state while on AC power so EC/BMS trickle top-off oscillations
     * between "Full"/"Not charging" and "Charging" (or 94%+ charged) don't flicker the icon. */
    static bool s_bat_full_latched = false;
    static long s_bat_full_cap = 0;
    if (strcmp(status, "Discharging") == 0 || (ac_found && !ac_online)) {
        s_bat_full_latched = false;
        s_bat_full_cap = 0;
    } else {
        if (strcmp(status, "Full") == 0 || strcmp(status, "Not charging") == 0 ||
            (strcmp(status, "Charging") == 0 && cap >= stop_thresh - 6)) {
            s_bat_full_latched = true;
            if (cap > s_bat_full_cap) s_bat_full_cap = cap;
        }
        if (s_bat_full_latched) {
            if (cap < s_bat_full_cap - 6 && cap < stop_thresh - 6) {
                s_bat_full_latched = false;
            } else {
                snprintf(status, sizeof(status), "Full");
            }
        }
    }

    char tstr[32] = "";
    if (p_now > 0 && e_now >= 0 &&
        (strcmp(status, "Charging") == 0 || strcmp(status, "Discharging") == 0)) {
        double hrs = 0;
        if (strcmp(status, "Charging") == 0 && e_full > e_now)
            hrs = (double)(e_full - e_now) / (double)p_now;
        else if (strcmp(status, "Discharging") == 0)
            hrs = (double)e_now / (double)p_now;
        if (hrs > 0) {
            int total_m = (int)(hrs * 60.0);
            snprintf(tstr, sizeof(tstr), "%d:%02d", total_m / 60, total_m % 60);
        }
    }

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

static int dbus_connect_system_upower(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "/run/dbus/system_bus_socket");
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    uint8_t req[2048];
    static const unsigned char hs[] = "\0AUTH EXTERNAL\r\nDATA\r\nBEGIN\r\n";
    int len = (int)sizeof(hs) - 1;
    memcpy(req, hs, len);
    len += dbus_build_call(req + len, 1, 0, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "Hello", NULL);
    len += dbus_build_call(req + len, 2, 0, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "AddMatch",
                           "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',path_namespace='/org/freedesktop/UPower'");
    len += dbus_build_call(req + len, 3, 0, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus", "AddMatch",
                           "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',path='/org/freedesktop/timedate1'");
    if (write(fd, req, len) <= 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void *battery_uevent_thread(void *arg) {
    (void)arg;
    poll_battery();
    notify_main_loop();

    int nl_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (nl_fd >= 0) {
        struct sockaddr_nl sa;
        memset(&sa, 0, sizeof(sa));
        sa.nl_family = AF_NETLINK;
        sa.nl_groups = 1; /* Kernel uevent multicast group */
        if (bind(nl_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
            close(nl_fd);
            nl_fd = -1;
        }
    }

    int dbus_fd = dbus_connect_system_upower();
    time_t last_dbus_try = time(NULL);
    if (nl_fd < 0 && dbus_fd < 0) return NULL;

    char buf[8192];
    while (1) {
        if (dbus_fd < 0 && time(NULL) - last_dbus_try >= 5) {
            last_dbus_try = time(NULL);
            dbus_fd = dbus_connect_system_upower();
        }
        struct pollfd pfds[2];
        int nfds = 0;
        int nl_idx = -1, dbus_idx = -1;
        if (nl_fd >= 0) {
            nl_idx = nfds;
            pfds[nfds].fd = nl_fd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }
        if (dbus_fd >= 0) {
            dbus_idx = nfds;
            pfds[nfds].fd = dbus_fd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }
        if (nfds == 0) {
            sleep(5);
            continue;
        }

        if (poll(pfds, nfds, -1) > 0) {
            bool updated = false;
            if (nl_idx >= 0 && (pfds[nl_idx].revents & POLLIN)) {
                ssize_t n = recv(nl_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
                if (n > 0) {
                    buf[n] = '\0';
                    if (memmem(buf, (size_t)n, "power_supply", 12) != NULL ||
                        memmem(buf, (size_t)n, "SUBSYSTEM=power_supply", 22) != NULL) {
                        updated = true;
                    }
                }
            }
            if (dbus_idx >= 0 && (pfds[dbus_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
                ssize_t n = read(dbus_fd, buf, sizeof(buf));
                if (n <= 0) {
                    close(dbus_fd);
                    dbus_fd = -1;
                } else {
                    if (memmem(buf, (size_t)n, "/org/freedesktop/UPower", 23) != NULL) {
                        updated = true;
                    }
                    if (memmem(buf, (size_t)n, "/org/freedesktop/timedate1", 26) != NULL) {
                        g_geoloc_update_requested = 1;
                        notify_main_loop();
                    }
                }
            }
            if (updated) {
                poll_battery();
                notify_main_loop();
            }
        }
    }
    if (nl_fd >= 0) close(nl_fd);
    if (dbus_fd >= 0) close(dbus_fd);
    return NULL;
}

static void *background_worker_thread(void *arg) {
    (void)arg;
    load_gcal_credentials();
    if (g_namaz_auto_loc) {
        fetch_geolocation_auto(false);
    }
    fetch_gcal_events();
    notify_main_loop();

    int ticks = 0;
    while (1) {
        sleep(300); /* Refresh remote Google Calendar every 5 minutes */
        ticks++;
        if (g_namaz_auto_loc && (ticks % 6 == 0)) {
            fetch_geolocation_auto(true);
        }
        fetch_gcal_events();
        notify_main_loop();
    }
    return NULL;
}

/* ============================================================================
 * Bar & Tooltip Rendering (Dynamic Space Calculation + Clock Left-Shift)
 * ============================================================================ */
static void truncate_utf8_to_px(char *str, size_t buf_sz, int max_px, int font_px, bool is_bold) {
    if (max_px <= 0) {
        str[0] = '\0';
        return;
    }
    if (measure_markup_line(str, font_px, is_bold) <= max_px) return;
    if (max_px <= 16) {
        str[0] = '\0';
        return;
    }

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
    /* Clear background to COL_BAR_BG and draw 1px bottom border if configured */
    uint32_t bg_pm = premul_argb(COL_BAR_BG);
    for (int y = 0; y < BAR_HEIGHT - 1; y++) {
        for (int x = 0; x < s->w; x++) s->px[y * s->w + x] = bg_pm;
    }
    if ((COL_BAR_BORDER >> 24) == 0) {
        for (int x = 0; x < s->w; x++) s->px[(BAR_HEIGHT - 1) * s->w + x] = bg_pm;
    } else {
        uint32_t bdr_pm = premul_argb(COL_BAR_BORDER);
        for (int x = 0; x < s->w; x++) s->px[(BAR_HEIGHT - 1) * s->w + x] = bdr_pm;
    }

    for (int i = 0; i < MOD_COUNT; i++) g_mod_boxes[i].visible = false;

    int baseline_y = 18;
    int left_x = 0;

    pthread_mutex_lock(&g_mutex);

    /* 0. LEFT: sway/headless monitor icon (leftmost, before workspace 1) */
    if (g_sway_headless) {
        int tw = measure_markup_line("⇸", 16, false);
        g_mod_boxes[MOD_HEADLESS].x1 = left_x;
        uint32_t fg_col = g_client_passthrough ? COL_ACTIVE : COL_WS_IDLE;
        render_markup_line(s, "⇸", left_x + 4, baseline_y, 16, fg_col, false, true);
        left_x += tw + 1;
        g_mod_boxes[MOD_HEADLESS].x2 = left_x;
        g_mod_boxes[MOD_HEADLESS].visible = true;
        if (g_client_passthrough) {
            snprintf(g_mod_boxes[MOD_HEADLESS].tooltip, sizeof(g_mod_boxes[0].tooltip), "Headless Sway Session (Client Passthrough Active)");
        } else {
            snprintf(g_mod_boxes[MOD_HEADLESS].tooltip, sizeof(g_mod_boxes[0].tooltip), "Headless Sway Session");
        }
    }

    /* 1. LEFT: sway/workspaces */
    int ws_start = left_x;
    for (int i = 0; i < g_ws_count; i++) {
        if (i > 0 || (i == 0 && left_x > 0)) {
            left_x += 4;
            draw_separator(s, left_x);
            left_x += 5;
            if (i == 0) ws_start = left_x;
        }
        if (g_ws_renaming && g_workspaces[i].focused) {
            char prefix[16] = "";
            if (g_ws_rename_num >= 0) {
                snprintf(prefix, sizeof(prefix), "%d:", g_ws_rename_num);
            }
            char before_cur[48], after_cur[32];
            snprintf(before_cur, sizeof(before_cur), "%s%.*s", prefix, g_ws_rename_cursor, g_ws_rename_buf);
            snprintf(after_cur, sizeof(after_cur), "%s", g_ws_rename_buf + g_ws_rename_cursor);

            int pw = measure_markup_line(prefix, 16, false);
            int bw_before = measure_markup_line(before_cur, 16, false);
            int bw_after = measure_markup_line(after_cur, 16, false);
            int content_w = bw_before + 2 + bw_after;
            if (g_ws_rename_buf[0] == '\0') {
                int hint_w = pw + 2 + measure_markup_line("...", 16, false);
                if (content_w < hint_w) content_w = hint_w;
            }
            int bw = content_w + 12;
            g_workspaces[i].x1 = left_x;
            g_workspaces[i].x2 = left_x + bw;
            draw_rounded_box(s, left_x, 1, bw, BAR_HEIGHT - 3, 3, COL_WS_FOC_BG, COL_WS_FOC_BDR);
            int tx = left_x + 5;
            render_markup_line(s, before_cur, tx, baseline_y, 16, COL_TEXT, false, true);
            int cur_x = tx + bw_before;
            fill_rect(s, cur_x, 4, 2, BAR_HEIGHT - 8, COL_WS_FOC_BDR);
            if (after_cur[0] != '\0') {
                render_markup_line(s, after_cur, cur_x + 2, baseline_y, 16, COL_TEXT, false, true);
            } else if (g_ws_rename_buf[0] == '\0') {
                render_markup_line(s, "...", cur_x + 3, baseline_y, 16, COL_WS_IDLE, false, true);
            }
            left_x += bw;
        } else {
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

    bool has_right_mod = (g_tray_count > 0);

    /* Helper macro to draw a right-side module (with right border separator if another module is to its right) */
#define DRAW_RIGHT_MOD(mod_id, txt_str, fg_col, is_bold, bg_box_col, tip_str)          \
    do {                                                                               \
        int _tw = measure_markup_line((txt_str), 16, (is_bold));                       \
        int _x2 = right_x;                                                             \
        int _sep_x = has_right_mod ? (right_x - 8) : right_x;                          \
        int _tx = has_right_mod ? (_sep_x - 8 - _tw) : (right_x - _tw);                \
        int _x1 = _tx - 4;                                                             \
        if ((bg_box_col) != 0) {                                                       \
            fill_rect(s, _x1, 0, _sep_x - _x1, BAR_HEIGHT - 1, (bg_box_col));          \
        }                                                                              \
        if (has_right_mod) {                                                           \
            draw_separator(s, _sep_x);                                                 \
        }                                                                              \
        render_markup_line(s, (txt_str), _tx, baseline_y, 16, (fg_col), (is_bold), true); \
        g_mod_boxes[(mod_id)].x1 = _x1;                                                \
        g_mod_boxes[(mod_id)].x2 = _x2;                                                \
        g_mod_boxes[(mod_id)].visible = true;                                          \
        snprintf(g_mod_boxes[(mod_id)].tooltip, sizeof(g_mod_boxes[0].tooltip), "%s", (tip_str)); \
        right_x = _x1;                                                                 \
        has_right_mod = true;                                                          \
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
        if (strcmp(g_bat_status, "Full") == 0 || strcmp(g_bat_status, "Not charging") == 0 ||
            (strcmp(g_bat_status, "Charging") == 0 && g_bat_capacity >= 94)) b_icon = "🔌";
        else if (strcmp(g_bat_status, "Charging") == 0) b_icon = "⚡";

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
    time_t now = time(NULL);
    int gcal_all_idxs[MAX_GCAL_EVENTS];
    int gcal_all_cnt = 0;
    int gcal_vis_idxs[MAX_GCAL_EVENTS];
    int gcal_vis_cnt = 0;
    for (int i = 0; i < g_gcal_count; i++) {
        if (g_gcal_events[i].end_ts > now) {
            gcal_all_idxs[gcal_all_cnt++] = i;
            if (!g_gcal_events[i].filtered) {
                gcal_vis_idxs[gcal_vis_cnt++] = i;
            }
        }
    }
    bool has_gcal = (gcal_all_cnt > 0);
    bool gcal_icon_only = (gcal_vis_cnt == 0 && gcal_all_cnt > 0);
    bool gcal_two = false;
    char gc_sum0[256] = "", gc_suf0[256] = "";
    char gc_sum1[256] = "", gc_suf1[256] = "";
    char gc_bar[1024] = "", gc_tip[8192] = "";
    int gcal_natural_box_w = 0;
    const GCalEvent *ev0 = NULL;
    const GCalEvent *ev1 = NULL;

    if (has_gcal) {
        /* Send an urgent notification once when an unfiltered event is <= g_gcal_notify_mins away */
        for (int i = 0; i < gcal_vis_cnt && g_gcal_notify_mins > 0; i++) {
            const GCalEvent *wev = &g_gcal_events[gcal_vis_idxs[i]];
            int wmins = (wev->start_ts > now) ? (int)((wev->start_ts - now + 59) / 60)
                                              : -(int)((now - wev->start_ts) / 60);
            if (wmins > 0 && wmins <= g_gcal_notify_mins) {
                bool already = false;
                for (int n = 0; n < g_gcal_notified_count; n++) {
                    if (g_gcal_notified[n].start_ts == wev->start_ts &&
                        strcmp(g_gcal_notified[n].summary, wev->summary) == 0) {
                        already = true;
                        break;
                    }
                }
                if (!already) {
                    if (g_gcal_notified_count >= MAX_GCAL_EVENTS) {
                        memmove(&g_gcal_notified[0], &g_gcal_notified[1],
                                sizeof(GCalNotified) * (MAX_GCAL_EVENTS - 1));
                        g_gcal_notified_count = MAX_GCAL_EVENTS - 1;
                    }
                    g_gcal_notified[g_gcal_notified_count].start_ts = wev->start_ts;
                    snprintf(g_gcal_notified[g_gcal_notified_count].summary,
                             sizeof(g_gcal_notified[0].summary), "%s", wev->summary);
                    g_gcal_notified_count++;

                    char nbody[256];
                    char wstr[32];
                    format_gcal_mins(wmins, wstr, sizeof(wstr));
                    if (wev->location[0]) {
                        snprintf(nbody, sizeof(nbody), "in %s @ %s", wstr, wev->location);
                    } else {
                        snprintf(nbody, sizeof(nbody), "in %s", wstr);
                    }
                    notify_urgent(wev->summary, nbody);
                }
            }
        }

        if (gcal_icon_only) {
            snprintf(gc_bar, sizeof(gc_bar), "📆");
        } else {
            ev0 = &g_gcal_events[gcal_vis_idxs[0]];
            int mins0 = (ev0->start_ts > now) ? (int)((ev0->start_ts - now + 59) / 60)
                                              : -(int)((now - ev0->start_ts) / 60);
            snprintf(gc_sum0, sizeof(gc_sum0), "%s", g_gcal_private ? "[private]" : ev0->summary);
            bool show_cd0 = (mins0 < 30);
            if (g_gcal_show_details) show_cd0 = !show_cd0;
            if (show_cd0) {
                char cd0[32];
                format_gcal_mins(mins0, cd0, sizeof(cd0));
                if (mins0 < 0) {
                    if (ev0->location[0]) snprintf(gc_suf0, sizeof(gc_suf0), " in <span color='#cc6666'>%s</span> @ %s", cd0, ev0->location);
                    else snprintf(gc_suf0, sizeof(gc_suf0), " in <span color='#cc6666'>%s</span>", cd0);
                } else {
                    if (ev0->location[0]) snprintf(gc_suf0, sizeof(gc_suf0), " in %s @ %s", cd0, ev0->location);
                    else snprintf(gc_suf0, sizeof(gc_suf0), " in %s", cd0);
                }
            } else {
                snprintf(gc_suf0, sizeof(gc_suf0), " %s", ev0->start_short);
            }

            if (mins0 <= 0 && gcal_vis_cnt > 1) {
                const GCalEvent *cand1 = &g_gcal_events[gcal_vis_idxs[1]];
                int mins1 = (cand1->start_ts > now) ? (int)((cand1->start_ts - now + 59) / 60)
                                                    : -(int)((now - cand1->start_ts) / 60);
                if (mins1 < 30) {
                    gcal_two = true;
                    ev1 = cand1;
                    snprintf(gc_sum1, sizeof(gc_sum1), "%s", g_gcal_private ? "[private]" : ev1->summary);
                    bool show_cd1 = (mins1 < 30);
                    if (g_gcal_show_details) show_cd1 = !show_cd1;
                    if (show_cd1) {
                        char cd1[32];
                        format_gcal_mins(mins1, cd1, sizeof(cd1));
                        if (mins1 < 0) {
                            if (ev1->location[0]) snprintf(gc_suf1, sizeof(gc_suf1), " in <span color='#cc6666'>%s</span> @ %s", cd1, ev1->location);
                            else snprintf(gc_suf1, sizeof(gc_suf1), " in <span color='#cc6666'>%s</span>", cd1);
                        } else {
                            if (ev1->location[0]) snprintf(gc_suf1, sizeof(gc_suf1), " in %s @ %s", cd1, ev1->location);
                            else snprintf(gc_suf1, sizeof(gc_suf1), " in %s", cd1);
                        }
                    } else {
                        snprintf(gc_suf1, sizeof(gc_suf1), " %s", ev1->start_short);
                    }
                }
            }

            if (gcal_two) {
                snprintf(gc_bar, sizeof(gc_bar), "%s%s | %s%s", gc_sum1, gc_suf1, gc_sum0, gc_suf0);
            } else {
                snprintf(gc_bar, sizeof(gc_bar), "%s%s", gc_sum0, gc_suf0);
            }
        }
        gcal_natural_box_w = measure_markup_line(gc_bar, 16, false) + 20;

        size_t toff = 0;
        for (int i = 0; i < gcal_all_cnt && toff + 128 < sizeof(gc_tip); i++) {
            const GCalEvent *tev = &g_gcal_events[gcal_all_idxs[i]];
            if (tev->filtered) {
                if (tev->location[0]) {
                    toff += snprintf(gc_tip + toff, sizeof(gc_tip) - toff, "%s<span color='#%06x'>%s %s, %s</span>",
                                     (i > 0) ? "\n" : "", (unsigned)(COL_WS_IDLE & 0xFFFFFFU), tev->time_range, tev->summary, tev->location);
                } else {
                    toff += snprintf(gc_tip + toff, sizeof(gc_tip) - toff, "%s<span color='#%06x'>%s %s</span>",
                                     (i > 0) ? "\n" : "", (unsigned)(COL_WS_IDLE & 0xFFFFFFU), tev->time_range, tev->summary);
                }
            } else {
                if (tev->location[0]) {
                    toff += snprintf(gc_tip + toff, sizeof(gc_tip) - toff, "%s%s %s, %s",
                                     (i > 0) ? "\n" : "", tev->time_range, tev->summary, tev->location);
                } else {
                    toff += snprintf(gc_tip + toff, sizeof(gc_tip) - toff, "%s%s %s",
                                     (i > 0) ? "\n" : "", tev->time_range, tev->summary);
                }
            }
        }
    }

    /* Prepare Quiet notification strings & natural width */
    if (g_quiet_preview_until > 0 && now >= g_quiet_preview_until) {
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

    if (has_gcal && total_avail > 24) {
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
        if (!gcal_icon_only && gcal_natural_box_w > gcal_max_box) {
            if (gcal_two) {
                int suf_w = measure_markup_line(gc_suf0, 16, false) +
                            measure_markup_line(" | ", 16, false) +
                            measure_markup_line(gc_suf1, 16, false);
                int max_each_px = (gcal_max_box - 20 - suf_w) / 2;
                if (max_each_px > 16) {
                    truncate_utf8_to_px(gc_sum0, sizeof(gc_sum0), max_each_px, 16, false);
                    truncate_utf8_to_px(gc_sum1, sizeof(gc_sum1), max_each_px, 16, false);
                    snprintf(gc_bar, sizeof(gc_bar), "%s%s | %s%s", gc_sum1, gc_suf1, gc_sum0, gc_suf0);
                } else {
                    /* Not enough width for two events: fall back to current/next single event */
                    gcal_two = false;
                }
            }
            if (!gcal_two) {
                int suffix_w = measure_markup_line(gc_suf0, 16, false);
                int max_sum_px = gcal_max_box - 20 - suffix_w;
                truncate_utf8_to_px(gc_sum0, sizeof(gc_sum0), max_sum_px, 16, false);
                snprintf(gc_bar, sizeof(gc_bar), "%s%s", gc_sum0, gc_suf0);
            }
        }
        if (gcal_icon_only || gc_sum0[0] || !has_quiet) {
            DRAW_RIGHT_MOD(MOD_GCAL, gc_bar, COL_TEXT, false, 0, gc_tip);
        }
    }

    if (has_quiet) {
        int rem_box_w = right_x - min_right_x;
        int max_txt_px = rem_box_w - 20;
        if (max_txt_px > 0) {
            truncate_utf8_to_px(q_bar, sizeof(q_bar), max_txt_px, 16, q_urg);
            if (!q_bar[0] && g_quiet_preview[0]) {
                snprintf(q_bar, sizeof(q_bar), "%u", g_quiet_count);
                truncate_utf8_to_px(q_bar, sizeof(q_bar), max_txt_px, 16, q_urg);
            }
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
        if (is_first_line && g_hovered_mod == MOD_CLOCK) {
            int lw = measure_markup_line(line, 15, false);
            if (s->w > lw + 16) {
                draw_x = (s->w - lw) / 2;
            }
        }
        is_first_line = false;

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
#define MAX_WL_SEATS 8
static struct {
    uint32_t seat;
    uint32_t pointer;
    uint32_t keyboard;
} g_seats[MAX_WL_SEATS];
static int g_num_seats = 0;
static uint32_t g_kbd_mods = 0;

static int find_seat_by_obj(uint32_t obj_id) {
    for (int i = 0; i < g_num_seats; i++) {
        if (g_seats[i].seat == obj_id) return i;
    }
    return -1;
}

static bool is_pointer_obj(uint32_t obj_id) {
    for (int i = 0; i < g_num_seats; i++) {
        if (g_seats[i].pointer == obj_id) return true;
    }
    return false;
}

static bool is_keyboard_obj(uint32_t obj_id) {
    for (int i = 0; i < g_num_seats; i++) {
        if (g_seats[i].keyboard == obj_id) return true;
    }
    return false;
}
static uint32_t g_id_layer_shell = 0;
static uint32_t g_id_idle_inhibit_mgr = 0;
static uint32_t g_id_idle_inhibitor = 0;

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

    ssize_t ret = sendmsg(g_wl_fd, &msg, MSG_NOSIGNAL);
    if (ret < 0 && (errno == EPIPE || errno == ECONNRESET || errno == EBADF)) {
        exit(0);
    }
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

static void wl_attach_shm_pixels(uint32_t surface_id, int w, int h, const uint32_t *px) {
    if (w <= 0 || h <= 0 || !px) return;
    size_t sz = (size_t)w * (size_t)h * 4;
    int mfd = memfd_create("waybar-shm", MFD_CLOEXEC);
    if (mfd < 0) return;
    size_t off = 0;
    while (off < sz) {
        ssize_t n = write(mfd, (const uint8_t *)px + off, sz - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(mfd);
            return;
        }
        off += (size_t)n;
    }

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

static void wl_attach_rendered_shm(uint32_t surface_id, int w, int h, void (*render_fn)(SurfaceBuf *)) {
    if (w <= 0 || h <= 0) return;
    size_t sz = (size_t)w * (size_t)h * 4;
    static uint32_t *s_scratch = NULL;
    static size_t s_scratch_cap = 0;
    if (sz > s_scratch_cap) {
        uint32_t *np = realloc(s_scratch, sz);
        if (!np) return;
        s_scratch = np;
        s_scratch_cap = sz;
    }

    SurfaceBuf s = {.px = s_scratch, .w = w, .h = h};
    render_fn(&s);
    wl_attach_shm_pixels(surface_id, w, h, s_scratch);
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
    g_active_tip_text[0] = '\0';
}

static void update_tooltip_surface(void) {
    if (g_hovered_mod < 0 || !g_mod_boxes[g_hovered_mod].visible || !g_mod_boxes[g_hovered_mod].tooltip[0]) {
        destroy_tooltip_surface();
        return;
    }

    bool text_changed = (strcmp(g_active_tip_text, g_mod_boxes[g_hovered_mod].tooltip) != 0);
    snprintf(g_active_tip_text, sizeof(g_active_tip_text), "%s", g_mod_boxes[g_hovered_mod].tooltip);
    int tw = 0, th = 0;
    measure_tooltip(g_active_tip_text, &tw, &th);
    int cx = (g_mod_boxes[g_hovered_mod].x1 + g_mod_boxes[g_hovered_mod].x2) / 2;
    int tx = cx - tw / 2;
    if (tx < 4) tx = 4;
    if (tx + tw > g_bar_w - 4) tx = g_bar_w - tw - 4;

    if (!text_changed && g_id_tip_surface && tw == g_tip_w && th == g_tip_h && tx == g_tip_x) {
        return;
    }

    if (g_id_tip_surface && (tw != g_tip_w || th != g_tip_h || tx != g_tip_x)) {
        destroy_tooltip_surface();
        snprintf(g_active_tip_text, sizeof(g_active_tip_text), "%s", g_mod_boxes[g_hovered_mod].tooltip);
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

static void sync_idle_inhibitor(void) {
    if (g_wl_fd < 0 || !g_id_idle_inhibit_mgr) return;
    bool should_inhibit = !g_screensaver_active && g_id_bar_surface != 0;
    if (should_inhibit && !g_id_idle_inhibitor) {
        g_id_idle_inhibitor = g_wl_next_id++;
        wl_send_u32_2(g_id_idle_inhibit_mgr, 1 /* create_inhibitor */, g_id_idle_inhibitor, g_id_bar_surface);
    } else if (!should_inhibit && g_id_idle_inhibitor) {
        wl_send_msg(g_id_idle_inhibitor, 0 /* zwp_idle_inhibitor_v1.destroy */, NULL, 0, -1);
        g_id_idle_inhibitor = 0;
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
        wl_get_layer_surface(g_id_bar_layer, g_id_bar_surface, 0, g_ws_renaming ? 3 /* LAYER_OVERLAY */ : 2 /* LAYER_TOP */, "waybar");
        wl_send_u32_2(g_id_bar_layer, 0 /* set_size */, 0, BAR_HEIGHT);
        wl_send_u32_1(g_id_bar_layer, 1 /* set_anchor */, 1 | 4 | 8 /* TOP | LEFT | RIGHT */);
        wl_send_u32_1(g_id_bar_layer, 2 /* set_exclusive_zone */, BAR_HEIGHT);
        wl_send_u32_1(g_id_bar_layer, 4 /* set_keyboard_interactivity */, g_ws_renaming ? 1 : 0);
        wl_send_msg(g_id_bar_surface, 6 /* commit */, NULL, 0, -1);
    }
    sync_idle_inhibitor();
}

static void hide_bar_surface(void) {
    if (!g_bar_visible) return;
    g_bar_visible = false;
    destroy_tooltip_surface();
    g_hovered_mod = -1;
    g_hovered_tray_idx = -1;

    if (g_id_idle_inhibitor) {
        wl_send_msg(g_id_idle_inhibitor, 0 /* zwp_idle_inhibitor_v1.destroy */, NULL, 0, -1);
        g_id_idle_inhibitor = 0;
    }
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
    if (g_ws_renaming) return true;
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

static void sigusr2_handler(int sig) {
    (void)sig;
    g_geoloc_update_requested = 1;
    if (g_wakeup_pipe[1] >= 0) {
        char c = 1;
        ssize_t r = write(g_wakeup_pipe[1], &c, 1);
        (void)r;
    }
}

static void sigrtmin_handler(int sig) {
    (void)sig;
    g_rename_ws_requested = 1;
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

static uint32_t *g_bar_staging_px = NULL;
static uint32_t *g_bar_committed_px = NULL;
static size_t g_bar_px_cap = 0;
static uint32_t g_bar_committed_surf = 0;
static int g_bar_committed_w = 0;
static int g_bar_committed_h = 0;

static void redraw_all(void) {
    if (!g_bar_visible || !g_bar_configured || g_bar_w <= 0 || g_bar_h <= 0) return;
    size_t sz = (size_t)g_bar_w * (size_t)g_bar_h * 4;
    if (sz > g_bar_px_cap) {
        uint32_t *ns = realloc(g_bar_staging_px, sz);
        uint32_t *nc = realloc(g_bar_committed_px, sz);
        if (!ns || !nc) return;
        g_bar_staging_px = ns;
        g_bar_committed_px = nc;
        g_bar_px_cap = sz;
        g_bar_committed_surf = 0;
    }
    SurfaceBuf s = {.px = g_bar_staging_px, .w = g_bar_w, .h = g_bar_h};
    render_bar(&s);
    if (g_id_bar_surface != g_bar_committed_surf ||
        g_bar_w != g_bar_committed_w ||
        g_bar_h != g_bar_committed_h ||
        memcmp(g_bar_staging_px, g_bar_committed_px, sz) != 0) {
        memcpy(g_bar_committed_px, g_bar_staging_px, sz);
        g_bar_committed_surf = g_id_bar_surface;
        g_bar_committed_w = g_bar_w;
        g_bar_committed_h = g_bar_h;
        wl_attach_shm_pixels(g_id_bar_surface, g_bar_w, g_bar_h, g_bar_staging_px);
    }
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
    (void)time_ms;
    if (g_hovered_mod < 0) return;

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
                check_location_and_timezone(false);
                if (g_tz_count > 0) g_tz_idx = (g_tz_idx + 1) % g_tz_count;
                redraw_all();
            } else if (btn == 2) { /* on-click-middle */
                spawn_detached(g_gcal_open_cmd);
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
            if (btn == 3) {
                spawn_detached(g_gcal_open_cmd);
            } else if (btn == 1) {
                pthread_mutex_lock(&g_mutex);
                g_gcal_show_details = !g_gcal_show_details;
                pthread_mutex_unlock(&g_mutex);
                redraw_all();
            } else if (btn == 2) {
                toggle_gcal_private();
                redraw_all();
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
                check_location_and_timezone(true);
                if (g_namaz_auto_loc) {
                    trigger_geoloc_fetch_async();
                } else {
                    update_namaz_astronomical();
                    redraw_all();
                }
            }
            break;

        case MOD_HEADLESS:
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
 * Inline Workspace Rename Widget & Keyboard Input
 * ============================================================================ */
static void start_workspace_rename(void) {
    if (g_ws_renaming) return;

    pthread_mutex_lock(&g_mutex);
    g_ws_rename_num = -1;
    for (int i = 0; i < g_ws_count; i++) {
        if (g_workspaces[i].focused) {
            g_ws_rename_num = g_workspaces[i].num;
            break;
        }
    }
    g_ws_rename_buf[0] = '\0';
    g_ws_rename_cursor = 0;
    g_ws_renaming = true;
    pthread_mutex_unlock(&g_mutex);

    destroy_tooltip_surface();
    /* Recreate the layer surface so keyboard_interactivity=EXCLUSIVE is set before initial map */
    if (g_bar_visible) {
        hide_bar_surface();
    }
    update_bar_visibility_state();
    redraw_all();
}

static void cancel_workspace_rename(void) {
    if (!g_ws_renaming) return;

    pthread_mutex_lock(&g_mutex);
    g_ws_renaming = false;
    g_ws_rename_buf[0] = '\0';
    g_ws_rename_cursor = 0;
    pthread_mutex_unlock(&g_mutex);

    if (g_bar_visible) {
        hide_bar_surface();
    }
    update_bar_visibility_state();
    redraw_all();
}

static void commit_workspace_rename(void) {
    if (!g_ws_renaming) return;

    char name[32];
    int num = -1;
    pthread_mutex_lock(&g_mutex);
    snprintf(name, sizeof(name), "%s", g_ws_rename_buf);
    num = g_ws_rename_num;
    pthread_mutex_unlock(&g_mutex);

    if (name[0] != '\0') {
        char escaped[64];
        size_t j = 0;
        for (size_t i = 0; name[i] && j + 2 < sizeof(escaped); i++) {
            if (name[i] == '"' || name[i] == '\\') escaped[j++] = '\\';
            escaped[j++] = name[i];
        }
        escaped[j] = '\0';

        char cmd[128];
        if (num >= 0) {
            snprintf(cmd, sizeof(cmd), "rename workspace to \"%d:%s\"", num, escaped);
        } else {
            snprintf(cmd, sizeof(cmd), "rename workspace to \"%s\"", escaped);
        }
        sway_send_cmd(cmd);
        refresh_sway_state_ex(false);
    }
    cancel_workspace_rename();
}

static char evdev_key_to_ascii(uint32_t key, bool shift, bool caps) {
    static const struct { uint32_t code; char normal; char shifted; } map[] = {
        {KEY_1, '1', '!'}, {KEY_2, '2', '@'}, {KEY_3, '3', '#'}, {KEY_4, '4', '$'},
        {KEY_5, '5', '%'}, {KEY_6, '6', '^'}, {KEY_7, '7', '&'}, {KEY_8, '8', '*'},
        {KEY_9, '9', '('}, {KEY_0, '0', ')'}, {KEY_MINUS, '-', '_'}, {KEY_EQUAL, '=', '+'},
        {KEY_LEFTBRACE, '[', '{'}, {KEY_RIGHTBRACE, ']', '}'},
        {KEY_SEMICOLON, ';', ':'}, {KEY_APOSTROPHE, '\'', '"'},
        {KEY_GRAVE, '`', '~'}, {KEY_BACKSLASH, '\\', '|'},
        {KEY_COMMA, ',', '<'}, {KEY_DOT, '.', '>'}, {KEY_SLASH, '/', '?'},
        {KEY_SPACE, ' ', ' '},
        {KEY_Q, 'q', 'Q'}, {KEY_W, 'w', 'W'}, {KEY_E, 'e', 'E'}, {KEY_R, 'r', 'R'},
        {KEY_T, 't', 'T'}, {KEY_Y, 'y', 'Y'}, {KEY_U, 'u', 'U'}, {KEY_I, 'i', 'I'},
        {KEY_O, 'o', 'O'}, {KEY_P, 'p', 'P'}, {KEY_A, 'a', 'A'}, {KEY_S, 's', 'S'},
        {KEY_D, 'd', 'D'}, {KEY_F, 'f', 'F'}, {KEY_G, 'g', 'G'}, {KEY_H, 'h', 'H'},
        {KEY_J, 'j', 'J'}, {KEY_K, 'k', 'K'}, {KEY_L, 'l', 'L'}, {KEY_Z, 'z', 'Z'},
        {KEY_X, 'x', 'X'}, {KEY_C, 'c', 'C'}, {KEY_V, 'v', 'V'}, {KEY_B, 'b', 'B'},
        {KEY_N, 'n', 'N'}, {KEY_M, 'm', 'M'},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i].code == key) {
            bool is_alpha = (map[i].normal >= 'a' && map[i].normal <= 'z');
            bool upper = is_alpha ? (shift ^ caps) : shift;
            return upper ? map[i].shifted : map[i].normal;
        }
    }
    return 0;
}

static void handle_keyboard_key(uint32_t key) {
    bool shift = (g_kbd_mods & 1) != 0;
    bool caps = (g_kbd_mods & 2) != 0;
    bool ctrl = (g_kbd_mods & 4) != 0;

    if (key == KEY_ESC || (ctrl && (key == KEY_C || key == KEY_G))) {
        cancel_workspace_rename();
        return;
    }
    if (key == KEY_ENTER || key == KEY_KPENTER) {
        commit_workspace_rename();
        return;
    }

    int len = (int)strlen(g_ws_rename_buf);
    if (key == KEY_BACKSPACE) {
        if (g_ws_rename_cursor > 0) {
            if (ctrl) {
                int p = g_ws_rename_cursor;
                while (p > 0 && g_ws_rename_buf[p - 1] == ' ') p--;
                while (p > 0 && g_ws_rename_buf[p - 1] != ' ') p--;
                memmove(g_ws_rename_buf + p, g_ws_rename_buf + g_ws_rename_cursor, (size_t)(len - g_ws_rename_cursor + 1));
                g_ws_rename_cursor = p;
            } else {
                memmove(g_ws_rename_buf + g_ws_rename_cursor - 1, g_ws_rename_buf + g_ws_rename_cursor, (size_t)(len - g_ws_rename_cursor + 1));
                g_ws_rename_cursor--;
            }
            redraw_all();
        }
        return;
    }
    if (key == KEY_DELETE) {
        if (g_ws_rename_cursor < len) {
            memmove(g_ws_rename_buf + g_ws_rename_cursor, g_ws_rename_buf + g_ws_rename_cursor + 1, (size_t)(len - g_ws_rename_cursor));
            redraw_all();
        }
        return;
    }
    if (key == KEY_LEFT) {
        if (g_ws_rename_cursor > 0) {
            g_ws_rename_cursor--;
            redraw_all();
        }
        return;
    }
    if (key == KEY_RIGHT) {
        if (g_ws_rename_cursor < len) {
            g_ws_rename_cursor++;
            redraw_all();
        }
        return;
    }
    if (key == KEY_HOME || (ctrl && key == KEY_A)) {
        if (g_ws_rename_cursor != 0) {
            g_ws_rename_cursor = 0;
            redraw_all();
        }
        return;
    }
    if (key == KEY_END || (ctrl && key == KEY_E)) {
        if (g_ws_rename_cursor != len) {
            g_ws_rename_cursor = len;
            redraw_all();
        }
        return;
    }
    if (ctrl && key == KEY_U) {
        if (len > 0) {
            g_ws_rename_buf[0] = '\0';
            g_ws_rename_cursor = 0;
            redraw_all();
        }
        return;
    }
    if (ctrl && key == KEY_W) {
        if (g_ws_rename_cursor > 0) {
            int p = g_ws_rename_cursor;
            while (p > 0 && g_ws_rename_buf[p - 1] == ' ') p--;
            while (p > 0 && g_ws_rename_buf[p - 1] != ' ') p--;
            memmove(g_ws_rename_buf + p, g_ws_rename_buf + g_ws_rename_cursor, (size_t)(len - g_ws_rename_cursor + 1));
            g_ws_rename_cursor = p;
            redraw_all();
        }
        return;
    }

    if (!ctrl) {
        char ch = evdev_key_to_ascii(key, shift, caps);
        if (ch && len + 1 < (int)sizeof(g_ws_rename_buf)) {
            memmove(g_ws_rename_buf + g_ws_rename_cursor + 1, g_ws_rename_buf + g_ws_rename_cursor, (size_t)(len - g_ws_rename_cursor + 1));
            g_ws_rename_buf[g_ws_rename_cursor] = ch;
            g_ws_rename_cursor++;
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
        if (n == 0) {
            /* Wayland compositor closed connection */
            exit(0);
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            /* Wayland connection error / reset */
            exit(0);
        }
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
                if (g_num_seats < MAX_WL_SEATS) {
                    uint32_t sid = g_wl_next_id++;
                    g_seats[g_num_seats].seat = sid;
                    g_seats[g_num_seats].pointer = 0;
                    g_seats[g_num_seats].keyboard = 0;
                    g_num_seats++;
                    wl_registry_bind(name, iface, ver < 5 ? ver : 5, sid);
                }
            } else if (strcmp(iface, "zwlr_layer_shell_v1") == 0) {
                g_id_layer_shell = g_wl_next_id++;
                wl_registry_bind(name, iface, ver < 4 ? ver : 4, g_id_layer_shell);
            } else if (strcmp(iface, "zwp_idle_inhibit_manager_v1") == 0) {
                g_id_idle_inhibit_mgr = g_wl_next_id++;
                wl_registry_bind(name, iface, 1, g_id_idle_inhibit_mgr);
                sync_idle_inhibitor();
            }
        } else if (opcode == 0 /* capabilities */ && find_seat_by_obj(obj_id) >= 0) {
            int si = find_seat_by_obj(obj_id);
            uint32_t caps = *(const uint32_t *)p;
            if ((caps & 1) && !g_seats[si].pointer) {
                g_seats[si].pointer = g_wl_next_id++;
                wl_send_u32_1(g_seats[si].seat, 0 /* get_pointer */, g_seats[si].pointer);
            }
            if ((caps & 2) && !g_seats[si].keyboard) {
                g_seats[si].keyboard = g_wl_next_id++;
                wl_send_u32_1(g_seats[si].seat, 1 /* get_keyboard */, g_seats[si].keyboard);
            }
        } else if (obj_id == g_id_bar_layer && opcode == 0 /* configure */) {
            uint32_t serial = *(const uint32_t *)p;
            uint32_t w = *(const uint32_t *)(p + 4);
            uint32_t h = *(const uint32_t *)(p + 8);
            if (w > 0) g_bar_w = (int)w;
            if (h > 0) g_bar_h = (int)h;
            wl_send_u32_1(g_id_bar_layer, 6 /* ack_configure */, serial);
            g_bar_configured = true;
            g_bar_committed_surf = 0;
            redraw_all();
            sync_idle_inhibitor();
        } else if (obj_id == g_id_tip_layer && opcode == 0 /* configure */) {
            uint32_t serial = *(const uint32_t *)p;
            wl_send_u32_1(g_id_tip_layer, 6 /* ack_configure */, serial);
            g_tip_configured = true;
            wl_attach_rendered_shm(g_id_tip_surface, g_tip_w, g_tip_h, render_tip_cb);
        } else if (is_pointer_obj(obj_id)) {
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
        } else if (is_keyboard_obj(obj_id)) {
            if (opcode == 2 /* leave */) {
                uint32_t surf = *(const uint32_t *)(p + 4);
                if (surf == g_id_bar_surface && g_ws_renaming) {
                    cancel_workspace_rename();
                }
            } else if (opcode == 3 /* key */) {
                uint32_t key = *(const uint32_t *)(p + 8);
                uint32_t state = *(const uint32_t *)(p + 12);
                if (state == 1 && g_ws_renaming) {
                    handle_keyboard_key(key);
                }
            } else if (opcode == 4 /* modifiers */) {
                uint32_t depressed = *(const uint32_t *)(p + 4);
                uint32_t latched = *(const uint32_t *)(p + 8);
                uint32_t locked = *(const uint32_t *)(p + 12);
                g_kbd_mods = depressed | latched | (locked & 2);
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
    if (g_cfg_tz_count >= MAX_TZS) return;
    char spec[256];
    snprintf(spec, sizeof(spec), "%s", raw_spec);
    trim_and_unquote(spec);
    if (!spec[0]) return;

    const char *eq = strchr(spec, '=');
    if (!eq) eq = strchr(spec, ':'); /* Also allow TZ:emoji */
    if (!eq) eq = strchr(spec, ' '); /* Also allow TZ emoji */
    if (eq) {
        size_t tz_len = (size_t)(eq - spec);
        if (tz_len >= sizeof(g_cfg_tzs[0].tz)) tz_len = sizeof(g_cfg_tzs[0].tz) - 1;
        memcpy(g_cfg_tzs[g_cfg_tz_count].tz, spec, tz_len);
        g_cfg_tzs[g_cfg_tz_count].tz[tz_len] = '\0';
        trim_and_unquote(g_cfg_tzs[g_cfg_tz_count].tz);

        char lbl[64];
        snprintf(lbl, sizeof(lbl), "%s", eq + 1);
        trim_and_unquote(lbl);
        snprintf(g_cfg_tzs[g_cfg_tz_count].label, sizeof(g_cfg_tzs[0].label), "%s", lbl);
    } else {
        snprintf(g_cfg_tzs[g_cfg_tz_count].tz, sizeof(g_cfg_tzs[0].tz), "%s", spec);
        g_cfg_tzs[g_cfg_tz_count].label[0] = '\0';
    }
    g_cfg_tz_count++;
}

static void add_default_loc_prefix(const char *raw_spec) {
    if (g_default_loc_prefix_count >= MAX_LOC_PREFIXES) return;
    char spec[128];
    snprintf(spec, sizeof(spec), "%s", raw_spec);
    trim_and_unquote(spec);
    if (!spec[0]) return;
    snprintf(g_default_loc_prefixes[g_default_loc_prefix_count], sizeof(g_default_loc_prefixes[0]), "%s", spec);
    g_default_loc_prefix_count++;
}

static void add_scoped_loc_prefix(const char *scope, const char *raw_spec) {
    if (!scope || !*scope) {
        add_default_loc_prefix(raw_spec);
        return;
    }
    char spec[128];
    snprintf(spec, sizeof(spec), "%s", raw_spec);
    trim_and_unquote(spec);
    if (!spec[0]) return;

    int rule_idx = -1;
    for (int i = 0; i < g_scoped_loc_rule_count; i++) {
        if (strcasecmp(g_scoped_loc_rules[i].scope, scope) == 0) {
            rule_idx = i;
            break;
        }
    }
    if (rule_idx < 0) {
        if (g_scoped_loc_rule_count >= MAX_SCOPED_LOC_RULES) return;
        rule_idx = g_scoped_loc_rule_count++;
        snprintf(g_scoped_loc_rules[rule_idx].scope, sizeof(g_scoped_loc_rules[0].scope), "%s", scope);
        g_scoped_loc_rules[rule_idx].prefix_count = 0;
    }
    if (g_scoped_loc_rules[rule_idx].prefix_count < MAX_LOC_PREFIXES) {
        snprintf(g_scoped_loc_rules[rule_idx].prefixes[g_scoped_loc_rules[rule_idx].prefix_count],
                 sizeof(g_scoped_loc_rules[0].prefixes[0]), "%s", spec);
        g_scoped_loc_rules[rule_idx].prefix_count++;
    }
}

static void add_loc_prefix(const char *raw_spec) {
    add_default_loc_prefix(raw_spec);
}

static void update_active_loc_prefixes(void) {
    pthread_mutex_lock(&g_mutex);
    char cur_tz[64] = "";
    char cur_city[64] = "";
    if (g_detected_tz[0]) snprintf(cur_tz, sizeof(cur_tz), "%s", g_detected_tz);
    else get_system_tz(cur_tz, sizeof(cur_tz));

    if (g_detected_city[0]) snprintf(cur_city, sizeof(cur_city), "%s", g_detected_city);
    else if (g_namaz_city[0]) snprintf(cur_city, sizeof(cur_city), "%s", g_namaz_city);

    int matched_rule = -1;

    /* Tier 1: Match by City */
    if (cur_city[0]) {
        for (int i = 0; i < g_scoped_loc_rule_count; i++) {
            char scope_copy[128];
            snprintf(scope_copy, sizeof(scope_copy), "%s", g_scoped_loc_rules[i].scope);
            char *saveptr = NULL;
            char *tok = strtok_r(scope_copy, ",", &saveptr);
            while (tok) {
                while (*tok && isspace((unsigned char)*tok)) tok++;
                size_t tlen = strlen(tok);
                while (tlen > 0 && isspace((unsigned char)tok[tlen - 1])) tok[--tlen] = '\0';
                if (tlen > 0) {
                    if (strcasecmp(tok, cur_city) == 0 ||
                        strcasestr(cur_city, tok) != NULL ||
                        strcasestr(tok, cur_city) != NULL) {
                        matched_rule = i;
                        break;
                    }
                }
                tok = strtok_r(NULL, ",", &saveptr);
            }
            if (matched_rule >= 0) break;
        }
    }

    /* Tier 2: Match by Timezone */
    if (matched_rule < 0 && cur_tz[0]) {
        for (int i = 0; i < g_scoped_loc_rule_count; i++) {
            char scope_copy[128];
            snprintf(scope_copy, sizeof(scope_copy), "%s", g_scoped_loc_rules[i].scope);
            char *saveptr = NULL;
            char *tok = strtok_r(scope_copy, ",", &saveptr);
            while (tok) {
                while (*tok && isspace((unsigned char)*tok)) tok++;
                size_t tlen = strlen(tok);
                while (tlen > 0 && isspace((unsigned char)tok[tlen - 1])) tok[--tlen] = '\0';
                if (tlen > 0) {
                    if (is_same_timezone(tok, cur_tz) || strcasecmp(tok, cur_tz) == 0) {
                        matched_rule = i;
                        break;
                    }
                }
                tok = strtok_r(NULL, ",", &saveptr);
            }
            if (matched_rule >= 0) break;
        }
    }

    /* Apply selected rule or default, and check if active prefixes or timezone changed */
    int new_count = (matched_rule >= 0) ? g_scoped_loc_rules[matched_rule].prefix_count
                                        : g_default_loc_prefix_count;
    const char (*new_list)[128] = (matched_rule >= 0) ? g_scoped_loc_rules[matched_rule].prefixes
                                                      : g_default_loc_prefixes;

    static char s_last_eval_tz[64] = "";
    bool tz_changed = (strcmp(s_last_eval_tz, cur_tz) != 0);
    bool prefixes_changed = (new_count != g_gcal_loc_prefix_count);
    if (!prefixes_changed) {
        for (int p = 0; p < new_count; p++) {
            if (strcmp(g_gcal_loc_prefixes[p], new_list[p]) != 0) {
                prefixes_changed = true;
                break;
            }
        }
    }

    if (prefixes_changed) {
        for (int p = 0; p < MAX_LOC_PREFIXES; p++) {
            if (g_gcal_loc_has_re[p]) {
                regfree(&g_gcal_loc_re[p]);
                g_gcal_loc_has_re[p] = false;
            }
        }
        for (int p = 0; p < new_count; p++) {
            snprintf(g_gcal_loc_prefixes[p], sizeof(g_gcal_loc_prefixes[0]), "%s", new_list[p]);
            char pat[160];
            if (new_list[p][0] == '^') snprintf(pat, sizeof(pat), "%s", new_list[p]);
            else snprintf(pat, sizeof(pat), "^(%s)", new_list[p]);
            g_gcal_loc_has_re[p] = (regcomp(&g_gcal_loc_re[p], pat, REG_EXTENDED) == 0);
        }
        g_gcal_loc_prefix_count = new_count;
    }

    if (tz_changed || prefixes_changed) {
        snprintf(s_last_eval_tz, sizeof(s_last_eval_tz), "%s", cur_tz);
        /* Re-evaluate location and display times for any already-fetched calendar events */
        for (int i = 0; i < g_gcal_count; i++) {
            GCalEvent *ev = &g_gcal_events[i];
            parse_gcal_location_locked(ev->raw_location, ev->location, sizeof(ev->location));
            if (!ev->is_all_day && ev->start_ts > 0 && ev->end_ts > 0) {
                struct tm tm_s, tm_e;
                format_tz_time(cur_tz, ev->start_ts, &tm_s);
                format_tz_time(cur_tz, ev->end_ts, &tm_e);
                format_short_12h(tm_s.tm_hour, tm_s.tm_min, ev->start_short, sizeof(ev->start_short));
                format_fixed_range_12h(tm_s.tm_hour, tm_s.tm_min, tm_e.tm_hour, tm_e.tm_min,
                                       ev->time_range, sizeof(ev->time_range));
            }
        }
    }
    pthread_mutex_unlock(&g_mutex);
}


static void add_gcal_calendar(const char *raw_spec) {
    if (g_gcal_cal_count >= MAX_GCAL_CALS) return;
    char spec[128];
    snprintf(spec, sizeof(spec), "%s", raw_spec);
    trim_and_unquote(spec);
    if (!spec[0]) return;
    snprintf(g_gcal_calendars[g_gcal_cal_count], sizeof(g_gcal_calendars[0]), "%s", spec);
    g_gcal_cal_count++;
}

static void add_ignored_event(const char *raw_spec) {
    if (g_gcal_ignored_event_count >= MAX_IGNORED_EVENTS) return;
    char spec[128];
    snprintf(spec, sizeof(spec), "%s", raw_spec);
    trim_and_unquote(spec);
    if (!spec[0]) return;
    snprintf(g_gcal_ignored_events[g_gcal_ignored_event_count], sizeof(g_gcal_ignored_events[0]), "%s", spec);
    g_gcal_ignored_event_count++;
}

static int parse_duration_mins(const char *raw_val) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", raw_val);
    trim_and_unquote(buf);
    if (!buf[0]) return 360;
    double v = atof(buf);
    if (v <= 0) return 0;
    if (strchr(buf, 'h') || strchr(buf, 'H')) {
        return (int)(v * 60.0 + 0.5);
    }
    if (strchr(buf, 'm') || strchr(buf, 'M')) {
        return (int)(v + 0.5);
    }
    /* Bare number: <= 48 treated as hours, otherwise minutes */
    return (v <= 48.0) ? (int)(v * 60.0 + 0.5) : (int)(v + 0.5);
}

static bool load_config_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    bool reset_tz = false;
    bool reset_loc = false;
    bool reset_cals = false;
    bool reset_ignored = false;
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
                    g_cfg_tz_count = 0;
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
        } else if (strncmp(s, "relevant_locations", 18) == 0 || strncmp(s, "location_prefixes", 17) == 0) {
            char *bopen = strchr(s, '[');
            char *bclose = strchr(s, ']');
            char *eq = strchr(s, '=');
            if (eq) {
                char scope[128] = "";
                if (bopen && bclose && bclose > bopen && bclose < eq) {
                    *bclose = '\0';
                    snprintf(scope, sizeof(scope), "%s", bopen + 1);
                    trim_and_unquote(scope);
                }
                eq++;
                trim_and_unquote(eq);
                if (!reset_loc) {
                    g_scoped_loc_rule_count = 0;
                    g_default_loc_prefix_count = 0;
                    g_gcal_loc_prefix_count = 0;
                    reset_loc = true;
                }
                char *tok = strtok(eq, ",");
                while (tok) {
                    if (scope[0]) add_scoped_loc_prefix(scope, tok);
                    else add_default_loc_prefix(tok);
                    tok = strtok(NULL, ",");
                }
            }
        } else if (strncmp(s, "ignore_long_event_threshold", 27) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                g_gcal_max_duration_mins = parse_duration_mins(eq + 1);
            }
        } else if (strncmp(s, "gcal_notify_mins", 16) == 0 || strncmp(s, "warn_threshold", 14) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                g_gcal_notify_mins = atoi(eq + 1);
            }
        } else if (strncmp(s, "gcal_notify_expire_mins", 23) == 0 || strncmp(s, "gcal_expire_mins", 16) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                g_gcal_notify_expire_mins = atoi(eq + 1);
            }
        } else if (strncmp(s, "gcal_open_cmd", 13) == 0 || strncmp(s, "calendar_cmd", 12) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                if (eq[0]) snprintf(g_gcal_open_cmd, sizeof(g_gcal_open_cmd), "%s", eq);
            }
        } else if (strncmp(s, "gcal_private", 12) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                g_gcal_private = (strcasecmp(eq, "true") == 0 || strcmp(eq, "1") == 0 || strcasecmp(eq, "yes") == 0);
            }
        } else if (strncmp(s, "gcal_calendars", 14) == 0 || strncmp(s, "calendars", 9) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                if (!reset_cals) {
                    g_gcal_cal_count = 0;
                    reset_cals = true;
                }
                char *tok = strtok(eq, ",");
                while (tok) {
                    add_gcal_calendar(tok);
                    tok = strtok(NULL, ",");
                }
            }
        } else if (strncmp(s, "ignored_events", 14) == 0 || strncmp(s, "gcal_ignored_events", 19) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                while (*eq && isspace((unsigned char)*eq)) eq++;
                size_t elen = strlen(eq);
                while (elen > 0 && isspace((unsigned char)eq[elen - 1])) eq[--elen] = '\0';
                int qcnt = 0;
                for (size_t qi = 0; qi < elen; qi++) {
                    if (eq[qi] == '"' || eq[qi] == '\'') qcnt++;
                }
                if (qcnt == 2 && elen >= 2 &&
                    ((eq[0] == '"' && eq[elen - 1] == '"') || (eq[0] == '\'' && eq[elen - 1] == '\''))) {
                    memmove(eq, eq + 1, elen - 2);
                    eq[elen - 2] = '\0';
                }
                if (!reset_ignored) {
                    g_gcal_ignored_event_count = 0;
                    reset_ignored = true;
                }
                char *tok = strtok(eq, ",");
                while (tok) {
                    add_ignored_event(tok);
                    tok = strtok(NULL, ",");
                }
            }
        } else if (strncmp(s, "lat", 3) == 0 || strncmp(s, "latitude", 8) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                if (strcasecmp(eq, "auto") == 0) {
                    g_namaz_auto_loc = true;
                } else {
                    g_namaz_lat = atof(eq);
                    g_namaz_auto_loc = false;
                }
            }
        } else if (strncmp(s, "lon", 3) == 0 || strncmp(s, "lng", 3) == 0 || strncmp(s, "longitude", 9) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                if (strcasecmp(eq, "auto") == 0) {
                    g_namaz_auto_loc = true;
                } else {
                    g_namaz_lon = atof(eq);
                    g_namaz_auto_loc = false;
                }
            }
        } else if (strncmp(s, "auto_location", 13) == 0 || strncmp(s, "auto_loc", 8) == 0 ||
                   strncmp(s, "location", 8) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                if (strcasecmp(eq, "true") == 0 || strcasecmp(eq, "1") == 0 ||
                    strcasecmp(eq, "yes") == 0 || strcasecmp(eq, "auto") == 0) {
                    g_namaz_auto_loc = true;
                } else {
                    g_namaz_auto_loc = false;
                }
            }
        } else if (strncmp(s, "city", 4) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                if (strcasecmp(eq, "auto") == 0) {
                    g_namaz_city[0] = '\0';
                    g_namaz_city_configured = false;
                } else {
                    snprintf(g_namaz_city, sizeof(g_namaz_city), "%s", eq);
                    g_namaz_city_configured = true;
                }
            }
        } else if (strncmp(s, "namaz_tz", 8) == 0 || strncmp(s, "namaz_timezone", 14) == 0 ||
                   strncmp(s, "prayer_tz", 9) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                snprintf(g_namaz_tz, sizeof(g_namaz_tz), "%s", eq);
                g_namaz_tz_configured = true;
            }
        } else if (strncmp(s, "bar_bg", 6) == 0 || strncmp(s, "col_bar_bg", 10) == 0 ||
                   strncmp(s, "background", 10) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                g_col_bar_bg = parse_color_spec(eq, g_col_bar_bg);
            }
        } else if (strncmp(s, "bar_border", 10) == 0 || strncmp(s, "col_bar_border", 14) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                g_col_bar_border = parse_color_spec(eq, g_col_bar_border);
            }
        } else if (strncmp(s, "bar_opacity", 11) == 0 || strncmp(s, "bar_alpha", 9) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                double a = atof(eq);
                uint32_t u_a = 255;
                if (strchr(eq, '%')) u_a = (uint32_t)(a * 2.55 + 0.5);
                else if (a <= 1.0 && a >= 0.0) u_a = (uint32_t)(a * 255.0 + 0.5);
                else if (a > 1.0 && a <= 255.0) u_a = (uint32_t)(a + 0.5);
                if (u_a > 255) u_a = 255;
                g_col_bar_bg = (u_a << 24) | (g_col_bar_bg & 0xFFFFFFU);
            }
        } else if (strncmp(s, "text_color", 10) == 0 || strncmp(s, "col_text", 8) == 0 ||
                   strncmp(s, "foreground", 10) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                g_col_text = parse_color_spec(eq, g_col_text);
            }
        } else if (strncmp(s, "ws_idle", 7) == 0 || strncmp(s, "col_ws_idle", 11) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                g_col_ws_idle = parse_color_spec(eq, g_col_ws_idle);
            }
        } else if (strncmp(s, "ws_focused_bg", 13) == 0 || strncmp(s, "col_ws_foc_bg", 13) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                g_col_ws_foc_bg = parse_color_spec(eq, g_col_ws_foc_bg);
            }
        } else if (strncmp(s, "ws_focused_border", 17) == 0 || strncmp(s, "col_ws_foc_bdr", 14) == 0 ||
                   strncmp(s, "ws_focused_bdr", 14) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                g_col_ws_foc_bdr = parse_color_spec(eq, g_col_ws_foc_bdr);
            }
        } else if (strncmp(s, "urgent_color", 12) == 0 || strncmp(s, "col_urgent", 10) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                g_col_urgent = parse_color_spec(eq, g_col_urgent);
            }
        } else if (strncmp(s, "warning_color", 13) == 0 || strncmp(s, "col_warning", 11) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                g_col_warning = parse_color_spec(eq, g_col_warning);
            }
        } else if (strncmp(s, "active_color", 12) == 0 || strncmp(s, "col_active", 10) == 0 ||
                   strncmp(s, "color_active", 12) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                g_col_active = parse_color_spec(eq, g_col_active);
            }
        } else if (strncmp(s, "tip_bg", 6) == 0 || strncmp(s, "col_tip_bg", 10) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                g_col_tip_bg = parse_color_spec(eq, g_col_tip_bg);
            }
        } else if (strncmp(s, "tip_border", 10) == 0 || strncmp(s, "col_tip_bdr", 11) == 0 ||
                   strncmp(s, "tip_bdr", 7) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                g_col_tip_bdr = parse_color_spec(eq, g_col_tip_bdr);
            }
        } else if (strncmp(s, "cld_month", 9) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                g_col_cld_month = parse_color_spec(eq, g_col_cld_month);
            }
        } else if (strncmp(s, "cld_weekday", 11) == 0 || strncmp(s, "cld_wkday", 9) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                g_col_cld_wkday = parse_color_spec(eq, g_col_cld_wkday);
            }
        } else if (strncmp(s, "cld_today_bg", 12) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                g_col_cld_today_bg = parse_color_spec(eq, g_col_cld_today_bg);
            }
        } else if (strncmp(s, "cld_today_fg", 12) == 0) {
            char *eq = strchr(s, '=');
            if (eq) {
                eq++;
                trim_and_unquote(eq);
                g_col_cld_today_fg = parse_color_spec(eq, g_col_cld_today_fg);
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
           "  --toggle                                Toggle visibility (hide/unhide) of running statusbar (or start if not running)\n"
           "  --rename-workspace                      Prompt inline in running statusbar to rename focused workspace\n"
           "  --kill                                  Terminate any running statusbar instance\n"
           "  --tz <TZ>[=<EMOJI_OR_LABEL>]            Add a timezone and optional label/emoji (repeatable;\n"
           "                                          label/emoji can be empty, e.g. --tz 'UTC=' or --tz 'America/New_York=NY 🇺🇸')\n"
           "  --gcal-token <PATH_OR_TOKEN>            Path to Google Calendar OAuth JSON token file or raw Bearer token\n"
           "  --gcal-calendar <CAL_ID>                Calendar ID(s) to query (default: primary)\n"
           "  --relevant-locations <REGEX1,REGEX2>    Location regexes/prefixes to prioritize & strip\n"
           "  --ignore-long-event-threshold <DUR>     Ignore events longer than threshold (default: 6h)\n"
           "  --ignored-events <LIST>                 Comma-separated event titles/patterns to filter (e.g. 'DNS,DND,Commute')\n"
           "  --gcal-notify-mins <MINS>               Urgent notification lead time in minutes (default: 5)\n"
           "  --gcal-notify-expire-mins <MINS>        Urgent notification expiration timeout in minutes (default: 8)\n"
           "  --lat <DEGREES|auto>                    Latitude for offline prayer times (or 'auto')\n"
           "  --lon <DEGREES|auto>                    Longitude for offline prayer times (or 'auto')\n"
           "  --auto-location                         Automatically detect latitude, longitude & timezone\n"
           "  --city <NAME>                           City label for prayer times (default: auto or none)\n"
           "  --namaz-tz <TZ>                         Timezone for prayer times (default: auto-detected from lat/lon)\n"
           "  --config <PATH>                         Load configuration from file (default: $XDG_CONFIG_HOME/statusbar.conf)\n"
#if defined(ENABLE_SELF_TEST) || defined(SELF_TEST)
           "  --self-test                             Render bar & calendar tooltip offscreen and verify all modules\n"
#endif
           "  --wayland-test                          Connect to live Wayland display, configure layer surface & present frame\n"
           "  -h, --help                              Show this help message\n",
           prog);
}

int main(int argc, char *argv[]) {
    mallopt(M_MMAP_THRESHOLD, 2 * 1024 * 1024);
    /* Keep SIGCHLD as SIG_DFL so system() and popen()/pclose() return accurate exit codes */
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, sigusr1_handler);
    signal(SIGUSR2, sigusr2_handler);
    signal(SIGRTMIN, sigrtmin_handler);

    signal(SIGINT, sigterm_handler);
    signal(SIGTERM, sigterm_handler);

    const char *home = getenv("HOME");
    snprintf(g_home_dir, sizeof(g_home_dir), "%s", home ? home : "/usr/local/google/home/sselcuk");
    const char *xdg_cache = getenv("XDG_CACHE_HOME");
    if (xdg_cache && *xdg_cache) snprintf(g_cache_dir, sizeof(g_cache_dir), "%s", xdg_cache);
    else snprintf(g_cache_dir, sizeof(g_cache_dir), "%s/.cache", g_home_dir);
    const char *rdir = getenv("XDG_RUNTIME_DIR");
    if (rdir && *rdir) snprintf(g_runtime_dir, sizeof(g_runtime_dir), "%s", rdir);
    else snprintf(g_runtime_dir, sizeof(g_runtime_dir), "/run/user/%u", getuid());
    get_system_tz(g_detected_tz, sizeof(g_detected_tz));
    g_sway_headless = is_sway_headless();
    {
        struct stat st_lt;
        if (lstat("/etc/localtime", &st_lt) == 0) {
            g_last_localtime_mtime_ns = stat_mtime_ns(&st_lt);
        }
    }

    /* Load configuration from $XDG_CONFIG_HOME/statusbar.conf (or ~/.config/statusbar.conf) */
    const char *xdg_cfg = getenv("XDG_CONFIG_HOME");
    char cfg_dir[512];
    if (xdg_cfg && *xdg_cfg) snprintf(cfg_dir, sizeof(cfg_dir), "%s", xdg_cfg);
    else snprintf(cfg_dir, sizeof(cfg_dir), "%s/.config", g_home_dir);

    char cpath1[512], cpath2[512], cpath3[512], cpath4[512];
    snprintf(cpath1, sizeof(cpath1), "%s/statusbar.conf", cfg_dir);
    snprintf(cpath2, sizeof(cpath2), "%s/statusbar/statusbar.conf", cfg_dir);
    snprintf(cpath3, sizeof(cpath3), "%s/sway/statusbar.conf", cfg_dir);
    snprintf(cpath4, sizeof(cpath4), "%s/.dotfiles/statusbar.conf", g_home_dir);

    if (load_config_file(cpath1)) {
        snprintf(g_config_path, sizeof(g_config_path), "%s", cpath1);
    } else if (load_config_file(cpath2)) {
        snprintf(g_config_path, sizeof(g_config_path), "%s", cpath2);
    } else if (load_config_file(cpath3)) {
        snprintf(g_config_path, sizeof(g_config_path), "%s", cpath3);
    } else if (load_config_file(cpath4)) {
        snprintf(g_config_path, sizeof(g_config_path), "%s", cpath4);
    }

    bool cli_tz_reset = false;
    bool cli_loc_reset = false;
    bool cli_cal_reset = false;
    bool cli_ignored_reset = false;
#if defined(ENABLE_SELF_TEST) || defined(SELF_TEST)
    bool self_test = false;
#endif
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
        } else if (strcmp(argv[i], "--rename-workspace") == 0) {
            pid_t pid = find_running_statusbar_pid();
            if (pid > 0) {
                kill(pid, SIGRTMIN);
                return 0;
            }
            return 1;
        } else if (strcmp(argv[i], "--kill") == 0) {
            pid_t pid = find_running_statusbar_pid();
            if (pid > 0) {
                kill(pid, SIGTERM);
            }
            return 0;
#if defined(ENABLE_SELF_TEST) || defined(SELF_TEST)
        } else if (strcmp(argv[i], "--self-test") == 0) {
            self_test = true;
#else
        } else if (strcmp(argv[i], "--self-test") == 0) {
            fprintf(stderr, "statusbar: --self-test is disabled in production build (rebuild with -DENABLE_SELF_TEST)\n");
            return 1;
#endif
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
                g_cfg_tz_count = 0;
                cli_tz_reset = true;
            }
            char tmp_tz[512];
            snprintf(tmp_tz, sizeof(tmp_tz), "%s", argv[++i]);
            char *tok = strtok(tmp_tz, ",");
            while (tok) {
                add_tz_entry(tok);
                tok = strtok(NULL, ",");
            }
        } else if (strcmp(argv[i], "--gcal-token") == 0 && i + 1 < argc) {
            expand_tilde_path(argv[++i], g_gcal_token_arg, sizeof(g_gcal_token_arg));
        } else if (strcmp(argv[i], "--gcal-calendar") == 0 && i + 1 < argc) {
            if (!cli_cal_reset) {
                g_gcal_cal_count = 0;
                cli_cal_reset = true;
            }
            char tmp_c[256];
            snprintf(tmp_c, sizeof(tmp_c), "%s", argv[++i]);
            char *tok = strtok(tmp_c, ",");
            while (tok) {
                add_gcal_calendar(tok);
                tok = strtok(NULL, ",");
            }
        } else if ((strcmp(argv[i], "--relevant-locations") == 0 ||
                    strcmp(argv[i], "--location-prefix") == 0) && i + 1 < argc) {
            if (!cli_loc_reset) {
                g_scoped_loc_rule_count = 0;
                g_default_loc_prefix_count = 0;
                g_gcal_loc_prefix_count = 0;
                cli_loc_reset = true;
            }
            char tmp_l[256];
            snprintf(tmp_l, sizeof(tmp_l), "%s", argv[++i]);
            char *tok = strtok(tmp_l, ",");
            while (tok) {
                add_loc_prefix(tok);
                tok = strtok(NULL, ",");
            }
        } else if (strcmp(argv[i], "--ignore-long-event-threshold") == 0 && i + 1 < argc) {
            g_gcal_max_duration_mins = parse_duration_mins(argv[++i]);
        } else if ((strcmp(argv[i], "--ignored-events") == 0 ||
                    strcmp(argv[i], "--gcal-ignored-events") == 0) && i + 1 < argc) {
            if (!cli_ignored_reset) {
                g_gcal_ignored_event_count = 0;
                cli_ignored_reset = true;
            }
            char tmp_ig[512];
            snprintf(tmp_ig, sizeof(tmp_ig), "%s", argv[++i]);
            char *sarg = tmp_ig;
            while (*sarg && isspace((unsigned char)*sarg)) sarg++;
            size_t slen = strlen(sarg);
            while (slen > 0 && isspace((unsigned char)sarg[slen - 1])) sarg[--slen] = '\0';
            int qcnt = 0;
            for (size_t qi = 0; qi < slen; qi++) {
                if (sarg[qi] == '"' || sarg[qi] == '\'') qcnt++;
            }
            if (qcnt == 2 && slen >= 2 &&
                ((sarg[0] == '"' && sarg[slen - 1] == '"') || (sarg[0] == '\'' && sarg[slen - 1] == '\''))) {
                memmove(sarg, sarg + 1, slen - 2);
                sarg[slen - 2] = '\0';
            }
            char *tok = strtok(sarg, ",");
            while (tok) {
                add_ignored_event(tok);
                tok = strtok(NULL, ",");
            }
        } else if (strcmp(argv[i], "--gcal-notify-mins") == 0 && i + 1 < argc) {
            g_gcal_notify_mins = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--gcal-notify-expire-mins") == 0 && i + 1 < argc) {
            g_gcal_notify_expire_mins = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "--lat") == 0 || strcmp(argv[i], "--latitude") == 0) && i + 1 < argc) {
            i++;
            if (strcasecmp(argv[i], "auto") == 0) {
                g_namaz_auto_loc = true;
            } else {
                g_namaz_lat = atof(argv[i]);
                g_namaz_auto_loc = false;
            }
        } else if ((strcmp(argv[i], "--lon") == 0 || strcmp(argv[i], "--lng") == 0 || strcmp(argv[i], "--longitude") == 0) && i + 1 < argc) {
            i++;
            if (strcasecmp(argv[i], "auto") == 0) {
                g_namaz_auto_loc = true;
            } else {
                g_namaz_lon = atof(argv[i]);
                g_namaz_auto_loc = false;
            }
        } else if (strcmp(argv[i], "--auto-location") == 0 || strcmp(argv[i], "--auto-loc") == 0) {
            g_namaz_auto_loc = true;
            g_namaz_lat = 0.0;
            g_namaz_lon = 0.0;
        } else if (strcmp(argv[i], "--city") == 0 && i + 1 < argc) {
            i++;
            if (strcasecmp(argv[i], "auto") == 0) {
                g_namaz_city[0] = '\0';
                g_namaz_city_configured = false;
            } else {
                snprintf(g_namaz_city, sizeof(g_namaz_city), "%s", argv[i]);
                g_namaz_city_configured = true;
            }
        } else if ((strcmp(argv[i], "--namaz-tz") == 0 || strcmp(argv[i], "--prayer-tz") == 0) && i + 1 < argc) {
            snprintf(g_namaz_tz, sizeof(g_namaz_tz), "%s", argv[++i]);
            g_namaz_tz_configured = true;
        }
    }

    load_geoloc_cache();
    g_last_tz_check_ts = time(NULL);
    update_active_clocks();

    init_fonts();

#if defined(ENABLE_SELF_TEST) || defined(SELF_TEST)
    if (self_test) {
        if (g_namaz_auto_loc && g_namaz_lat == 0.0 && g_namaz_lon == 0.0) {
            fetch_geolocation_auto(false);
        }
        update_active_clocks();
        load_gcal_credentials();
        update_namaz_astronomical();
        refresh_sway_state();
        poll_pulseaudio();
        poll_screensaver();
        poll_gcal_private();
        poll_battery();
        poll_quiet();
        uint32_t *px = calloc(3840 * BAR_HEIGHT, 4);
        SurfaceBuf sb = {.px = px, .w = 3840, .h = BAR_HEIGHT};
        render_bar(&sb);
        /* Self-test verification of clock rotation, deduplication, and hover behavior */
        {
            char bar_test[128], tip_test[8192];
            char orig_det_tz[64];
            snprintf(orig_det_tz, sizeof(orig_det_tz), "%s", g_detected_tz);

            /* Case 1: Detected location = Europe/Zurich (matches configured location) */
            snprintf(g_detected_tz, sizeof(g_detected_tz), "Europe/Zurich");
            update_active_clocks();
            if (g_tz_count != 3 || strcmp(g_tzs[0].tz, "Europe/Zurich") != 0 || g_tzs[0].label[0] != '\0') {
                fprintf(stderr, "Self-test error: Zurich deduplication failed: count=%d tz0=%s lbl0='%s'\n",
                        g_tz_count, g_tzs[0].tz, g_tzs[0].label);
                return 1;
            }
            g_tz_idx = 0;
            build_clock_strings(bar_test, sizeof(bar_test), tip_test, sizeof(tip_test));
            if (strstr(bar_test, "🇨🇭") != NULL) {
                fprintf(stderr, "Self-test error: default clock in bar must not have emoji: '%s'\n", bar_test);
                return 1;
            }
            if (strstr(tip_test, "🇨🇭") == NULL || strstr(tip_test, "🗽") == NULL || strstr(tip_test, "🇹🇷") == NULL) {
                fprintf(stderr, "Self-test error: tooltip must show all configured locations with emojis\n");
                return 1;
            }
            g_tz_idx = 1;
            build_clock_strings(bar_test, sizeof(bar_test), tip_test, sizeof(tip_test));
            if (strstr(bar_test, "🗽") == NULL && strstr(bar_test, "🇹🇷") == NULL) {
                fprintf(stderr, "Self-test error: non-default cycled clock must show emoji: '%s'\n", bar_test);
                return 1;
            }

            /* Case 2: Detected location = Asia/Tokyo (new location not in config) */
            snprintf(g_detected_tz, sizeof(g_detected_tz), "Asia/Tokyo");
            update_active_clocks();
            if (g_tz_count != 4 || strcmp(g_tzs[0].tz, "Asia/Tokyo") != 0 || g_tzs[0].label[0] != '\0') {
                fprintf(stderr, "Self-test error: new location failed: count=%d tz0=%s lbl0='%s'\n",
                        g_tz_count, g_tzs[0].tz, g_tzs[0].label);
                return 1;
            }
            g_tz_idx = 0;
            build_clock_strings(bar_test, sizeof(bar_test), tip_test, sizeof(tip_test));
            if (strstr(tip_test, "Tokyo") != NULL) {
                fprintf(stderr, "Self-test error: new location must not appear in hover tooltip\n");
                return 1;
            }
            if (strstr(tip_test, "🇨🇭") == NULL || strstr(tip_test, "🗽") == NULL || strstr(tip_test, "🇹🇷") == NULL) {
                fprintf(stderr, "Self-test error: hover tooltip must still show all configured locations\n");
                return 1;
            }

            /* Case 3: Verify flag/city emojis use color emoji, and system icons stay monochrome */
            if (!is_flag_or_city_emoji(0x1F5FD)) { /* 🗽 Statue of Liberty */
                fprintf(stderr, "Self-test error: 🗽 must be recognized as city emoji\n");
                return 1;
            }
            if (is_symbola_forced(0x1F5FD)) {
                fprintf(stderr, "Self-test error: 🗽 must not be forced to Symbola\n");
                return 1;
            }
            if (!is_symbola_forced(0x1F50B) || is_flag_or_city_emoji(0x1F50B)) { /* 🔋 Battery */
                fprintf(stderr, "Self-test error: battery icon must stay forced to Symbola\n");
                return 1;
            }
            if (!is_symbola_forced(0x1F50A) || is_flag_or_city_emoji(0x1F50A)) { /* 🔊 Speaker */
                fprintf(stderr, "Self-test error: speaker icon must stay forced to Symbola\n");
                return 1;
            }

            /* Case 4: Verify scoped relevant_locations resolution & GCal timezone/location re-evaluation */
            snprintf(g_detected_tz, sizeof(g_detected_tz), "Europe/Zurich");
            g_detected_city[0] = '\0';
            update_active_loc_prefixes();
            if (g_gcal_loc_prefix_count == 0 || strstr(g_gcal_loc_prefixes[0], "CH-ZRH") == NULL) {
                fprintf(stderr, "Self-test error: Zurich loc prefix failed: count=%d pref0='%s'\n",
                        g_gcal_loc_prefix_count, g_gcal_loc_prefix_count ? g_gcal_loc_prefixes[0] : "");
                return 1;
            }

            /* Test parse_iso_datetime converts calendar event time into g_detected_tz */
            time_t test_ts = 0;
            int test_h = -1, test_m = -1;
            snprintf(g_detected_tz, sizeof(g_detected_tz), "America/New_York");
            parse_iso_datetime("2026-01-15T16:00:00+01:00", &test_ts, &test_h, &test_m);
            if (test_h != 10 || test_m != 0) {
                fprintf(stderr, "Self-test error: parse_iso_datetime NY conversion failed: got %02d:%02d, expected 10:00\n",
                        test_h, test_m);
                return 1;
            }

            snprintf(g_detected_tz, sizeof(g_detected_tz), "America/Los_Angeles");
            g_detected_city[0] = '\0';
            update_active_loc_prefixes();
            if (g_gcal_loc_prefix_count == 0 || strstr(g_gcal_loc_prefixes[0], "US-MTV") == NULL) {
                fprintf(stderr, "Self-test error: MTV loc prefix failed: count=%d pref0='%s'\n",
                        g_gcal_loc_prefix_count, g_gcal_loc_prefix_count ? g_gcal_loc_prefixes[0] : "");
                return 1;
            }

            snprintf(g_detected_tz, sizeof(g_detected_tz), "America/New_York");
            g_detected_city[0] = '\0';
            update_active_loc_prefixes();
            if (g_gcal_loc_prefix_count == 0 || strstr(g_gcal_loc_prefixes[0], "US-NYC") == NULL) {
                fprintf(stderr, "Self-test error: NYC loc prefix failed: count=%d pref0='%s'\n",
                        g_gcal_loc_prefix_count, g_gcal_loc_prefix_count ? g_gcal_loc_prefixes[0] : "");
                return 1;
            }

            /* Restore original detected timezone */
            snprintf(g_detected_tz, sizeof(g_detected_tz), "%s", orig_det_tz);
            update_active_clocks();
            g_tz_idx = 0;

            /* Case 5: Verify Google Calendar private mode toggle, persistence & rendering */
            char priv_path[512];
            snprintf(priv_path, sizeof(priv_path), "%s/statusbar.gcal_private", g_runtime_dir);
            unlink(priv_path);
            poll_gcal_private();
            if (g_gcal_private) {
                fprintf(stderr, "Self-test error: g_gcal_private should initially be false\n");
                return 1;
            }
            toggle_gcal_private();
            if (!g_gcal_private || access(priv_path, F_OK) != 0) {
                fprintf(stderr, "Self-test error: toggle_gcal_private failed to enable or create file\n");
                return 1;
            }
            g_gcal_private = false;
            poll_gcal_private();
            if (!g_gcal_private) {
                fprintf(stderr, "Self-test error: poll_gcal_private failed to reload persistence\n");
                return 1;
            }
            toggle_gcal_private();
            if (g_gcal_private || access(priv_path, F_OK) == 0) {
                fprintf(stderr, "Self-test error: toggle_gcal_private failed to disable or remove file\n");
                return 1;
            }

            /* Verify event rendering under normal and private modes */
            g_gcal_count = 1;
            snprintf(g_gcal_events[0].summary, sizeof(g_gcal_events[0].summary), "Secret Strategy Meeting");
            snprintf(g_gcal_events[0].location, sizeof(g_gcal_events[0].location), "4W-Conf");
            snprintf(g_gcal_events[0].start_short, sizeof(g_gcal_events[0].start_short), "14:00");
            g_gcal_events[0].start_ts = time(NULL) + 600; /* in 10 mins */
            g_gcal_events[0].end_ts = g_gcal_events[0].start_ts + 1800;
            g_gcal_events[0].filtered = false;

            g_gcal_private = false;
            render_bar(&sb);
            if (strstr(g_mod_boxes[MOD_GCAL].tooltip, "Secret Strategy Meeting") == NULL ||
                strstr(g_mod_boxes[MOD_GCAL].tooltip, "4W-Conf") == NULL) {
                fprintf(stderr, "Self-test error: non-private gcal tooltip missing summary or room\n");
                return 1;
            }

            g_gcal_private = true;
            render_bar(&sb);
            if (strstr(g_mod_boxes[MOD_GCAL].tooltip, "Secret Strategy Meeting") == NULL ||
                strstr(g_mod_boxes[MOD_GCAL].tooltip, "4W-Conf") == NULL) {
                fprintf(stderr, "Self-test error: private mode must NOT hide summary or room in hover tooltip\n");
                return 1;
            }
            if (strstr(g_mod_boxes[MOD_GCAL].tooltip, "[private]") != NULL) {
                fprintf(stderr, "Self-test error: [private] must not appear in hover tooltip\n");
                return 1;
            }
            g_gcal_count = 0;
            g_gcal_private = false;

            /* Verify format_gcal_mins duration formatting */
            char dur_buf[32];
            format_gcal_mins(125, dur_buf, sizeof(dur_buf));
            if (strcmp(dur_buf, "2h5m") != 0) {
                fprintf(stderr, "Self-test error: format_gcal_mins(125) expected '2h5m', got '%s'\n", dur_buf);
                return 1;
            }
            format_gcal_mins(120, dur_buf, sizeof(dur_buf));
            if (strcmp(dur_buf, "2h") != 0) {
                fprintf(stderr, "Self-test error: format_gcal_mins(120) expected '2h', got '%s'\n", dur_buf);
                return 1;
            }
            format_gcal_mins(60, dur_buf, sizeof(dur_buf));
            if (strcmp(dur_buf, "1h") != 0) {
                fprintf(stderr, "Self-test error: format_gcal_mins(60) expected '1h', got '%s'\n", dur_buf);
                return 1;
            }
            format_gcal_mins(65, dur_buf, sizeof(dur_buf));
            if (strcmp(dur_buf, "1h5m") != 0) {
                fprintf(stderr, "Self-test error: format_gcal_mins(65) expected '1h5m', got '%s'\n", dur_buf);
                return 1;
            }
            format_gcal_mins(25, dur_buf, sizeof(dur_buf));
            if (strcmp(dur_buf, "25m") != 0) {
                fprintf(stderr, "Self-test error: format_gcal_mins(25) expected '25m', got '%s'\n", dur_buf);
                return 1;
            }
            format_gcal_mins(0, dur_buf, sizeof(dur_buf));
            if (strcmp(dur_buf, "0m") != 0) {
                fprintf(stderr, "Self-test error: format_gcal_mins(0) expected '0m', got '%s'\n", dur_buf);
                return 1;
            }
            format_gcal_mins(-5, dur_buf, sizeof(dur_buf));
            if (strcmp(dur_buf, "-5m") != 0) {
                fprintf(stderr, "Self-test error: format_gcal_mins(-5) expected '-5m', got '%s'\n", dur_buf);
                return 1;
            }
            format_gcal_mins(-60, dur_buf, sizeof(dur_buf));
            if (strcmp(dur_buf, "-1h") != 0) {
                fprintf(stderr, "Self-test error: format_gcal_mins(-60) expected '-1h', got '%s'\n", dur_buf);
                return 1;
            }
            format_gcal_mins(-125, dur_buf, sizeof(dur_buf));
            if (strcmp(dur_buf, "-2h5m") != 0) {
                fprintf(stderr, "Self-test error: format_gcal_mins(-125) expected '-2h5m', got '%s'\n", dur_buf);
                return 1;
            }
        }

        /* Verification of headless monitor sign rendering and colorless Symbola forcing */
        {
            g_sway_headless = true;
            g_client_passthrough = false;
            render_bar(&sb);
            if (!g_mod_boxes[MOD_HEADLESS].visible || g_mod_boxes[MOD_HEADLESS].x1 != 0 ||
                g_mod_boxes[MOD_HEADLESS].x1 >= g_mod_boxes[MOD_HEADLESS].x2 ||
                (g_ws_count > 0 && g_mod_boxes[MOD_HEADLESS].x2 >= g_mod_boxes[MOD_WORKSPACES].x1)) {
                fprintf(stderr, "Self-test error: MOD_HEADLESS not rendered as leftmost module before workspaces\n");
                return 1;
            }
            if (strcmp(g_mod_boxes[MOD_HEADLESS].tooltip, "Headless Sway Session") != 0) {
                fprintf(stderr, "Self-test error: unexpected MOD_HEADLESS tooltip '%s'\n", g_mod_boxes[MOD_HEADLESS].tooltip);
                return 1;
            }

            /* Test client passthrough active indicator (green) */
            g_client_passthrough = true;
            render_bar(&sb);
            if (!g_mod_boxes[MOD_HEADLESS].visible ||
                strstr(g_mod_boxes[MOD_HEADLESS].tooltip, "Client Passthrough Active") == NULL) {
                fprintf(stderr, "Self-test error: unexpected MOD_HEADLESS passthrough tooltip '%s'\n", g_mod_boxes[MOD_HEADLESS].tooltip);
                return 1;
            }

            g_client_passthrough = false;
            g_sway_headless = is_sway_headless();
            render_bar(&sb);
        }

        printf("Self-test OK: rendered 3840x%d bar | emoji_font='%s' | headless=%s | workspaces=%d | tzs=%d (current='%s' label='%s') | clock_tip='%s' | namaz='%s' (city='%s' tz='%s') | screensaver=%s | quiet=%u (urg=%u preview='%s') | pulse=%d%%\n",
               BAR_HEIGHT, g_emoji_font_path[0] ? g_emoji_font_path : "NONE (install fonts-noto-color-emoji)",
               g_sway_headless ? "true(⎆)" : "false",
               g_ws_count, g_tz_count, g_tzs[0].tz, g_tzs[0].label,
               g_mod_boxes[MOD_CLOCK].tooltip,
               g_mod_boxes[MOD_NAMAZ].tooltip, g_namaz_city, g_namaz_tz,
               g_screensaver_active ? "active(💤)" : "inhibited(👁)",
               g_quiet_count, g_quiet_urgency, g_quiet_preview, g_pulse_vol);
        free(px);
        return 0;
    }
#endif

    if (!wayland_test) {
        ensure_single_instance();
    }

    /* Connect to Wayland display socket (support WAYLAND_SOCKET from compositor or WAYLAND_DISPLAY) */
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

    /* Create top bar layer surface (load screensaver state first so inhibitor attaches if needed) */
    poll_screensaver();
    poll_gcal_private();
    show_bar_surface();

    if (wayland_test) {
        update_namaz_astronomical();
        refresh_sway_state();
        poll_pulseaudio();
        poll_screensaver();
        poll_gcal_private();
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
    poll_gcal_private();
    init_passthrough_inotify();

    pthread_t th_sway, th_quiet, th_audio, th_bat, th_bg;
    pthread_create(&th_sway, NULL, sway_event_thread, NULL);
    pthread_create(&th_quiet, NULL, quiet_dbus_thread, NULL);
    pthread_create(&th_audio, NULL, audio_event_thread, NULL);
    pthread_create(&th_bat, NULL, battery_uevent_thread, NULL);
    pthread_create(&th_bg, NULL, background_worker_thread, NULL);

    struct pollfd fds[3];
    fds[0].fd = g_wl_fd;
    fds[0].events = POLLIN;
    fds[1].fd = g_wakeup_pipe[0];
    fds[1].events = POLLIN;
    int nfds = 2;
    if (g_passthrough_inotify_fd >= 0) {
        fds[2].fd = g_passthrough_inotify_fd;
        fds[2].events = POLLIN;
        nfds = 3;
    }

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
        int ret = poll(fds, nfds, timeout_ms);
        if (ret > 0) {
            if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                exit(0);
            }
            if (fds[0].revents & POLLIN) process_wayland_events();
            if (fds[1].revents & POLLIN) {
                char drain[64];
                while (read(g_wakeup_pipe[0], drain, sizeof(drain)) > 0) {}
                if (g_toggle_requested) {
                    g_toggle_requested = 0;
                    handle_toggle_request();
                }
                if (g_geoloc_update_requested) {
                    g_geoloc_update_requested = 0;
                    check_location_and_timezone(true);
                }
                if (g_rename_ws_requested) {
                    g_rename_ws_requested = 0;
                    start_workspace_rename();
                }
                update_bar_visibility_state();
                redraw_all();
            }
            if (nfds > 2 && (fds[2].revents & POLLIN)) {
                char ibuf[512];
                while (read(g_passthrough_inotify_fd, ibuf, sizeof(ibuf)) > 0) {}
                bool new_pt = check_client_passthrough_file();
                if (new_pt != g_client_passthrough) {
                    pthread_mutex_lock(&g_mutex);
                    g_client_passthrough = new_pt;
                    pthread_mutex_unlock(&g_mutex);
                    redraw_all();
                }
            }
        } else if (ret == 0) {
            g_bat_blink_on = !g_bat_blink_on;
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
