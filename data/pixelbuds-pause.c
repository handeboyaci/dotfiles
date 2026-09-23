// BlueZ Pixel Buds media helper:
// - Pauses media players when earbuds are removed from ears (MediaTransport1
//   State=="idle" debounced by 600ms so brief RF blips are ignored) or when
//   Bluetooth disconnects (Device1.Connected==false / InterfacesRemoved)
// - Automatically resumes playback if an idle/pause is caused by a momentary
//   Bluetooth connection/transport drop that reconnects within 25s
// - Displays SwayOSD volume popup on Pixel Buds AVRCP volume swipes
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>

#define USEC_PER_SEC  1000000ULL
#define USEC_PER_MSEC 1000ULL

// Debounce before pausing on MediaTransport1 State=="idle" (earbud removal).
// Filters out sub-600ms RF hiccups while still pausing promptly when buds are removed.
#define IDLE_PAUSE_DEBOUNCE_USEC (600ULL * USEC_PER_MSEC)
// If Bluetooth reconnects within 25s of a drop while playing, auto-resume.
#define AUTO_RESUME_WINDOW_USEC  (25ULL * USEC_PER_SEC)
// How long after MediaTransport1 appears to wait for PipeWire sink creation.
#define TRANSPORT_SETTLE_USEC    (700ULL * USEC_PER_MSEC)
// Fallback settle delay after Device1.Connected=true if transport already up.
#define CONNECT_SETTLE_USEC      (1500ULL * USEC_PER_MSEC)
// Window linking an idle-pause to a subsequent HCI link supervision disconnect (~2-5s).
#define DROP_BURST_WINDOW_USEC   (6ULL * USEC_PER_SEC)
// Minimum interval between SwayOSD volume popups on earbud swipe.
#define VOLUME_OSD_DEBOUNCE_USEC (150ULL * USEC_PER_MSEC)

static const char *g_device_prefix = "/org/bluez";
static bool g_transport_active = false;
static uint64_t g_active_when_idled_usec = 0;
static uint64_t g_paused_by_idle_usec = 0;
static uint64_t g_transport_activated_usec = 0;
static uint64_t g_last_vol_osd_usec = 0;

static uint64_t g_pause_at_usec = 0;
static bool g_pending_resume = false;
static uint64_t g_disconnect_usec = 0;
static uint64_t g_resume_at_usec = 0;
static char g_playing_instance[128] = "";

static uint64_t now_usec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return 0;
    return (uint64_t)ts.tv_sec * USEC_PER_SEC + (uint64_t)(ts.tv_nsec / 1000);
}

static bool path_matches_device(const char *path) {
    if (!path || !g_device_prefix) return false;
    size_t len = strlen(g_device_prefix);
    if (strncmp(path, g_device_prefix, len) != 0) return false;
    return path[len] == '\0' || path[len] == '/';
}

// Checks if any MPRIS player is currently Playing, and records its instance name.
static bool check_and_record_playing_player(void) {
    FILE *fp = popen("playerctl -a status -f '{{playerInstance}}\t{{status}}' 2>/dev/null", "r");
    if (!fp) return false;

    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        char *status = tab + 1;
        size_t slen = strlen(status);
        while (slen > 0 && (status[slen - 1] == '\n' || status[slen - 1] == '\r')) {
            status[--slen] = '\0';
        }
        if (strcmp(status, "Playing") == 0) {
            bool safe = line[0] != '\0';
            for (const char *p = line; *p; p++) {
                if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') {
                    safe = false;
                    break;
                }
            }
            if (safe) {
                snprintf(g_playing_instance, sizeof(g_playing_instance), "%.127s", line);
            }
            found = true;
            break;
        }
    }
    pclose(fp);
    return found;
}

static void pause_players(void) {
    fprintf(stderr, "pixelbuds-pause: pausing all active MPRIS players\n");
    fflush(stderr);
    int status = system("playerctl -a pause >/dev/null 2>&1");
    (void)status;
}

static void resume_players(void) {
    char cmd[256];
    fprintf(stderr, "pixelbuds-pause: auto-resuming MPRIS player (%s)\n",
            g_playing_instance[0] ? g_playing_instance : "default");
    fflush(stderr);
    if (g_playing_instance[0] != '\0') {
        snprintf(cmd, sizeof(cmd),
                 "playerctl -p '%s' play >/dev/null 2>&1 || playerctl -a play >/dev/null 2>&1",
                 g_playing_instance);
    } else {
        snprintf(cmd, sizeof(cmd), "playerctl -a play >/dev/null 2>&1");
    }
    int status = system(cmd);
    (void)status;
}

static void show_volume_osd(void) {
    uint64_t now = now_usec();
    if (now - g_last_vol_osd_usec < VOLUME_OSD_DEBOUNCE_USEC) return;
    g_last_vol_osd_usec = now;
    int status = system("swayosd-client --output-volume +0 >/dev/null 2>&1 &");
    (void)status;
}

static uint64_t g_last_disconnect_handled_usec = 0;

static void handle_disconnect_or_transport_drop(void) {
    uint64_t now = now_usec();
    if (g_last_disconnect_handled_usec > 0 &&
        (now - g_last_disconnect_handled_usec) < 1500ULL * USEC_PER_MSEC) {
        return;
    }
    g_last_disconnect_handled_usec = now;

    bool currently_playing = check_and_record_playing_player();
    bool burst_drop = g_transport_active ||
                      (g_active_when_idled_usec > 0 &&
                       (now - g_active_when_idled_usec) <= DROP_BURST_WINDOW_USEC) ||
                      (g_paused_by_idle_usec > 0 &&
                       (now - g_paused_by_idle_usec) <= DROP_BURST_WINDOW_USEC);
    bool was_playing = currently_playing || burst_drop;

    g_transport_active = false;
    g_pause_at_usec = 0;
    g_active_when_idled_usec = 0;
    g_paused_by_idle_usec = 0;

    // Always pause on disconnect/transport loss so audio never leaks to laptop speakers.
    pause_players();

    if (was_playing) {
        g_pending_resume = true;
        g_disconnect_usec = now;
        g_resume_at_usec = 0;
    }
}

static void schedule_auto_resume(uint64_t delay_usec) {
    if (!g_pending_resume) return;
    uint64_t now = now_usec();
    if (now - g_disconnect_usec > AUTO_RESUME_WINDOW_USEC) {
        g_pending_resume = false;
        g_resume_at_usec = 0;
        g_playing_instance[0] = '\0';
        return;
    }
    uint64_t target = now + delay_usec;
    if (g_resume_at_usec == 0 || target < g_resume_at_usec) {
        g_resume_at_usec = target;
    }
}

static int property_changed(sd_bus_message *message, void *userdata,
                            sd_bus_error *error) {
    const char *interface = NULL;
    const char *path = sd_bus_message_get_path(message);
    int result;

    (void)userdata;
    (void)error;

    if (!path_matches_device(path)) return 0;

    result = sd_bus_message_read(message, "s", &interface);
    if (result < 0) return result;

    result = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}");
    if (result < 0) return result;

    while ((result = sd_bus_message_enter_container(
                message, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
        const char *property = NULL;

        result = sd_bus_message_read(message, "s", &property);
        if (result < 0) return result;

        if (strcmp(interface, "org.bluez.MediaTransport1") == 0 &&
            strcmp(property, "State") == 0) {
            const char *state = NULL;

            result = sd_bus_message_enter_container(
                message, SD_BUS_TYPE_VARIANT, "s");
            if (result < 0) return result;
            result = sd_bus_message_read(message, "s", &state);
            if (result < 0) return result;

            uint64_t now = now_usec();
            fprintf(stderr, "pixelbuds-pause: %s MediaTransport1.State=%s\n", path, state);
            fflush(stderr);
            if (strcmp(state, "active") == 0) {
                g_transport_active = true;
                g_transport_activated_usec = now;
                g_pause_at_usec = 0;
                g_active_when_idled_usec = 0;
                g_paused_by_idle_usec = 0;
                if (g_pending_resume) {
                    schedule_auto_resume(200ULL * USEC_PER_MSEC);
                }
            } else if (strcmp(state, "pending") == 0) {
                g_pause_at_usec = 0;
                if (g_pending_resume) {
                    schedule_auto_resume(TRANSPORT_SETTLE_USEC);
                }
            } else if (strcmp(state, "idle") == 0) {
                // On Linux, removing Pixel Buds from ears sets MediaTransport1 State
                // to "idle". Schedule a 600ms debounced pause so earbud removal pauses
                // media promptly while sub-600ms RF hiccups are ignored.
                if (g_transport_active || check_and_record_playing_player()) {
                    g_active_when_idled_usec = now;
                    g_pause_at_usec = now + IDLE_PAUSE_DEBOUNCE_USEC;
                } else {
                    g_active_when_idled_usec = 0;
                    g_pause_at_usec = 0;
                }
                g_transport_active = false;
            }

            result = sd_bus_message_exit_container(message);
            if (result < 0) return result;
        } else if (strcmp(interface, "org.bluez.MediaTransport1") == 0 &&
                   strcmp(property, "Volume") == 0) {
            uint16_t vol = 0;
            result = sd_bus_message_enter_container(
                message, SD_BUS_TYPE_VARIANT, "q");
            if (result < 0) return result;
            result = sd_bus_message_read(message, "q", &vol);
            if (result < 0) return result;
            (void)vol;

            if (g_transport_activated_usec > 0 &&
                (now_usec() - g_transport_activated_usec) > 2ULL * USEC_PER_SEC) {
                show_volume_osd();
            }

            result = sd_bus_message_exit_container(message);
            if (result < 0) return result;
        } else if (strcmp(interface, "org.bluez.Device1") == 0 &&
                   strcmp(property, "Connected") == 0) {
            int connected = 1;

            result = sd_bus_message_enter_container(
                message, SD_BUS_TYPE_VARIANT, "b");
            if (result < 0) return result;
            result = sd_bus_message_read(message, "b", &connected);
            if (result < 0) return result;

            fprintf(stderr, "pixelbuds-pause: %s Device1.Connected=%d\n", path, connected);
            fflush(stderr);
            if (!connected) {
                handle_disconnect_or_transport_drop();
            } else {
                schedule_auto_resume(CONNECT_SETTLE_USEC);
            }

            result = sd_bus_message_exit_container(message);
            if (result < 0) return result;
        } else {
            result = sd_bus_message_skip(message, "v");
            if (result < 0) return result;
        }

        result = sd_bus_message_exit_container(message);
        if (result < 0) return result;
    }

    if (result < 0) return result;
    return sd_bus_message_exit_container(message);
}

static int interfaces_added(sd_bus_message *message, void *userdata,
                            sd_bus_error *error) {
    const char *object_path = NULL;
    int result;

    (void)userdata;
    (void)error;

    result = sd_bus_message_read(message, "o", &object_path);
    if (result < 0) return result;
    if (!path_matches_device(object_path)) return 0;

    result = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sa{sv}}");
    if (result < 0) return result;

    while ((result = sd_bus_message_enter_container(
                message, SD_BUS_TYPE_DICT_ENTRY, "sa{sv}")) > 0) {
        const char *iface = NULL;
        result = sd_bus_message_read(message, "s", &iface);
        if (result < 0) return result;

        if (strcmp(iface, "org.bluez.MediaTransport1") == 0) {
            g_transport_activated_usec = now_usec();
            schedule_auto_resume(TRANSPORT_SETTLE_USEC);
        }

        result = sd_bus_message_skip(message, "a{sv}");
        if (result < 0) return result;
        result = sd_bus_message_exit_container(message);
        if (result < 0) return result;
    }

    if (result < 0) return result;
    return sd_bus_message_exit_container(message);
}

static int interfaces_removed(sd_bus_message *message, void *userdata,
                              sd_bus_error *error) {
    const char *object_path = NULL;
    int result;

    (void)userdata;
    (void)error;

    result = sd_bus_message_read(message, "o", &object_path);
    if (result < 0) return result;
    if (!path_matches_device(object_path)) return 0;

    result = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "s");
    if (result < 0) return result;

    const char *iface = NULL;
    while ((result = sd_bus_message_read(message, "s", &iface)) > 0) {
        if (strcmp(iface, "org.bluez.MediaTransport1") == 0) {
            handle_disconnect_or_transport_drop();
        }
    }

    if (result < 0) return result;
    return sd_bus_message_exit_container(message);
}

int main(int argc, char **argv) {
    sd_bus *bus = NULL;
    sd_bus_slot *props_slot = NULL;
    sd_bus_slot *added_slot = NULL;
    sd_bus_slot *removed_slot = NULL;
    int result;

    if (argc > 2 || (argc == 2 && strncmp(argv[1], "/org/bluez/", 11) != 0)) {
        fprintf(stderr, "usage: %s [/org/bluez/.../dev_XX_XX_XX_XX_XX_XX]\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        g_device_prefix = argv[1];
    }

    if (!getenv("DBUS_SESSION_BUS_ADDRESS")) {
        const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
        if (runtime_dir && runtime_dir[0]) {
            char addr[256];
            snprintf(addr, sizeof(addr), "unix:path=%s/bus", runtime_dir);
            setenv("DBUS_SESSION_BUS_ADDRESS", addr, 0);
        }
    }

    int wp_status = system("wpctl settings --save bluetooth.autoswitch-to-headset-profile false >/dev/null 2>&1");
    (void)wp_status;

    result = sd_bus_open_system(&bus);
    if (result < 0) {
        fprintf(stderr, "pixelbuds-pause: system bus: %s\n",
                strerror(-result));
        return EXIT_FAILURE;
    }

    result = sd_bus_add_match(
        bus, &props_slot,
        "type='signal',sender='org.bluez',"
        "interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',path_namespace='/org/bluez'",
        property_changed, NULL);
    if (result < 0) goto fail;

    result = sd_bus_add_match(
        bus, &added_slot,
        "type='signal',sender='org.bluez',"
        "interface='org.freedesktop.DBus.ObjectManager',"
        "member='InterfacesAdded'",
        interfaces_added, NULL);
    if (result < 0) goto fail;

    result = sd_bus_add_match(
        bus, &removed_slot,
        "type='signal',sender='org.bluez',"
        "interface='org.freedesktop.DBus.ObjectManager',"
        "member='InterfacesRemoved'",
        interfaces_removed, NULL);
    if (result < 0) goto fail;

    for (;;) {
        result = sd_bus_process(bus, NULL);
        if (result < 0) goto fail;

        uint64_t now = now_usec();

        if (g_pause_at_usec > 0 && now >= g_pause_at_usec) {
            g_pause_at_usec = 0;
            if (check_and_record_playing_player()) {
                pause_players();
                g_paused_by_idle_usec = now;
            }
        }

        if (g_pending_resume) {
            if (now - g_disconnect_usec > AUTO_RESUME_WINDOW_USEC) {
                g_pending_resume = false;
                g_resume_at_usec = 0;
                g_playing_instance[0] = '\0';
            } else if (g_resume_at_usec > 0 && now >= g_resume_at_usec) {
                g_pending_resume = false;
                g_resume_at_usec = 0;
                resume_players();
                g_playing_instance[0] = '\0';
            }
        }

        if (result > 0) continue;

        uint64_t wait_usec = UINT64_MAX;
        if (g_pause_at_usec > 0) {
            wait_usec = (g_pause_at_usec > now) ? (g_pause_at_usec - now) : 0;
        }
        if (g_pending_resume) {
            uint64_t expire_at = g_disconnect_usec + AUTO_RESUME_WINDOW_USEC;
            uint64_t next_wakeup = expire_at;
            if (g_resume_at_usec > 0 && g_resume_at_usec < next_wakeup) {
                next_wakeup = g_resume_at_usec;
            }
            uint64_t resume_wait = (next_wakeup > now) ? (next_wakeup - now) : 0;
            if (resume_wait < wait_usec) wait_usec = resume_wait;
        }

        result = sd_bus_wait(bus, wait_usec);
        if (result < 0 && result != -EINTR) goto fail;
    }

fail:
    fprintf(stderr, "pixelbuds-pause: D-Bus error: %s\n", strerror(-result));
    sd_bus_slot_unref(removed_slot);
    sd_bus_slot_unref(added_slot);
    sd_bus_slot_unref(props_slot);
    sd_bus_unref(bus);
    return EXIT_FAILURE;
}
