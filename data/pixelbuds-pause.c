// BlueZ Pixel Buds media-pausing helper.
#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

static void pause_players(void) {
    int status = system("playerctl pause -a >/dev/null 2>&1");
    (void)status;
}

static int property_changed(sd_bus_message *message, void *userdata,
                            sd_bus_error *error) {
    const char *interface = NULL;
    int result;

    (void)userdata;
    (void)error;

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
            if (strcmp(state, "idle") == 0) pause_players();
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
            if (!connected) pause_players();
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

int main(int argc, char **argv) {
    const char *device_path;
    char media_match[512];
    char device_match[512];
    sd_bus *bus = NULL;
    sd_bus_slot *media_slot = NULL;
    sd_bus_slot *device_slot = NULL;
    int result;

    if (argc != 2 || strncmp(argv[1], "/org/bluez/", 11) != 0) {
        fprintf(stderr, "usage: %s /org/bluez/.../dev_XX_XX_XX_XX_XX_XX\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    device_path = argv[1];

    snprintf(media_match, sizeof(media_match),
             "type='signal',sender='org.bluez',"
             "interface='org.freedesktop.DBus.Properties',"
             "member='PropertiesChanged',path_namespace='%s',"
             "arg0='org.bluez.MediaTransport1'",
             device_path);
    snprintf(device_match, sizeof(device_match),
             "type='signal',sender='org.bluez',"
             "interface='org.freedesktop.DBus.Properties',"
             "member='PropertiesChanged',path='%s',"
             "arg0='org.bluez.Device1'",
             device_path);

    result = sd_bus_open_system(&bus);
    if (result < 0) {
        fprintf(stderr, "pixelbuds-pause: system bus: %s\n",
                strerror(-result));
        return EXIT_FAILURE;
    }

    result = sd_bus_add_match(bus, &media_slot, media_match,
                              property_changed, NULL);
    if (result < 0) goto fail;
    result = sd_bus_add_match(bus, &device_slot, device_match,
                              property_changed, NULL);
    if (result < 0) goto fail;

    for (;;) {
        result = sd_bus_process(bus, NULL);
        if (result < 0) goto fail;
        if (result > 0) continue;
        result = sd_bus_wait(bus, UINT64_MAX);
        if (result < 0) goto fail;
    }

fail:
    fprintf(stderr, "pixelbuds-pause: D-Bus error: %s\n", strerror(-result));
    sd_bus_slot_unref(device_slot);
    sd_bus_slot_unref(media_slot);
    sd_bus_unref(bus);
    return EXIT_FAILURE;
}
