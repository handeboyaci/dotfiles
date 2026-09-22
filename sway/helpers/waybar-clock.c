// Waybar JSON clock and calendar helper.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/prctl.h>

int tz_index = 0;
int show_seconds = 0;

const char* timezones[] = {"America/New_York", "Europe/Zurich", "Asia/Istanbul"};
const char* flags[] = {"", "🇨🇭", "🇹🇷"};
const char* city_names[] = {"New York", "Zurich", "Istanbul"};
const char* formats[] = {"%H:%M", "%H:%M:%S"};
const char* month_names[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};


void generate_month_lines(int year, int month, int highlight_day, char lines[8][150]) {
    struct tm start = {0};
    start.tm_year = year - 1900;
    start.tm_mon = month;
    start.tm_mday = 1;
    mktime(&start);

    int start_day = start.tm_wday; // 0 is Sunday
    int month_lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
        month_lengths[1] = 29;
    }
    int len = month_lengths[month];

    int line_idx = 0;

    // Center title in 24 spaces
    int name_len = strlen(month_names[month]);
    int total_padding = 24 - name_len;
    int left_padding = total_padding / 2;
    int right_padding = total_padding - left_padding;

    snprintf(lines[line_idx++], 150, "<span color='#ffead3'><b>%*s%s%*s</b></span>", left_padding, "", month_names[month], right_padding, "");
    snprintf(lines[line_idx++], 150, "<span color='#7d6b5e'>Wk</span> Su Mo Tu We Th Fr Sa ");

    int day = 1;
    for (int i = 0; i < 6; i++) {
        if (day > len) {
            snprintf(lines[line_idx++], 150, "                        ");
            continue;
        }

        // Calculate ISO week number based on Thursday of this week row
        struct tm current = start;
        current.tm_mday = (4 - start_day) + (i * 7) + 1;
        mktime(&current);
        char wk_str[5];
        strftime(wk_str, sizeof(wk_str), "%V", &current);

        char row_buf[150] = "";
        char *p = row_buf;
        int rem = sizeof(row_buf);

        int n = snprintf(p, rem, "<span color='#7d6b5e'>%s</span> ", wk_str);
        p += n; rem -= n;

        for (int j = 0; j < 7; j++) {
            if (i == 0 && j < start_day) {
                n = snprintf(p, rem, "   ");
            } else if (day <= len) {
                if (day == highlight_day) {
                    n = snprintf(p, rem, "<span background='#ffead3' color='#4a3e35'><b>%2d</b></span> ", day);
                } else {
                    n = snprintf(p, rem, "%2d ", day);
                }
                day++;
            } else {
                n = snprintf(p, rem, "   ");
            }
            if (n > 0 && n < rem) { p += n; rem -= n; }
        }
        snprintf(lines[line_idx++], 150, "%s", row_buf);
    }
}

void update_display(int signum) {
    if (signum == SIGUSR1) {
        tz_index = (tz_index + 1) % 3;
    } else if (signum == SIGUSR2) {
        show_seconds = !show_seconds;
    }

    char time_str[30];
    char date_str[30];
    time_t now;
    struct tm *tm_info;

    setenv("TZ", timezones[tz_index], 1);
    tzset();

    time(&now);
    tm_info = localtime(&now);

    strftime(time_str, sizeof(time_str), formats[show_seconds], tm_info);
    strftime(date_str, sizeof(date_str), "%a %d/%b/%Y", tm_info);

    char tooltip[40000] = ""; // Large buffer to hold full year with markup
    char *ptr = tooltip;
    int remaining = sizeof(tooltip);

    char tz_buf[400] = "";
    char *tz_ptr = tz_buf;
    int tz_rem = sizeof(tz_buf);
    int tz_visible_len = 0;

    // Add all timezones to buffer, highlighting current one
    for (int i = 0; i < 3; i++) {
        setenv("TZ", timezones[i], 1);
        tzset();
        struct tm *tm_other = localtime(&now);
        char t_str[30];
        strftime(t_str, sizeof(t_str), formats[show_seconds], tm_other);

        if (tz_ptr != tz_buf) {
            int n = snprintf(tz_ptr, tz_rem, "  |  ");
            if (n > 0 && n < tz_rem) { tz_ptr += n; tz_rem -= n; tz_visible_len += 5; }
        }
        int n;
        if (i == tz_index) {
            n = snprintf(tz_ptr, tz_rem, "<span color='#ffead3'><b>%s: %s%s</b></span>", city_names[i], t_str, flags[i]);
        } else {
            n = snprintf(tz_ptr, tz_rem, "%s: %s%s", city_names[i], t_str, flags[i]);
        }
        if (n > 0 && n < tz_rem) {
            tz_ptr += n;
            tz_rem -= n;
            tz_visible_len += strlen(city_names[i]) + 2 + (show_seconds ? 8 : 5) + (strlen(flags[i]) > 0 ? 2 : 0);
        }
    }

    // Calendar width is 24*3 + 4*2 = 72 + 8 = 80 characters
    int total_padding = 80 - tz_visible_len;
    if (total_padding < 0) total_padding = 0;
    int left_padding = total_padding / 2;
    int right_padding = total_padding - left_padding;

    int n = snprintf(ptr, remaining, "%*s%s%*s\\n\\n", left_padding, "", tz_buf, right_padding, "");
    if (n > 0 && n < remaining) { ptr += n; remaining -= n; }

    // Generate calendar for current year in 4x3 grid
    int year = tm_info->tm_year + 1900;
    int current_mon = tm_info->tm_mon;
    int current_day = tm_info->tm_mday;

    for (int r = 0; r < 4; r++) {
        char month_lines[3][8][150];
        memset(month_lines, 0, sizeof(month_lines));

        for (int c = 0; c < 3; c++) {
            int m = r * 3 + c;
            generate_month_lines(year, m, (m == current_mon) ? current_day : 0, month_lines[c]);
        }

        for (int l = 0; l < 8; l++) {
            int n = snprintf(ptr, remaining, "%s    %s    %s\\n",
                             month_lines[0][l], month_lines[1][l], month_lines[2][l]);
            if (n > 0 && n < remaining) {
                ptr += n;
                remaining -= n;
            }
        }
        if (r < 3) {
            int n = snprintf(ptr, remaining, "\\n");
            if (n > 0 && n < remaining) { ptr += n; remaining -= n; }
        }
    }

    // Strip trailing whitespace and newlines (represented as \\n) from tooltip
    int len = strlen(tooltip);
    while (len > 0) {
        if (tooltip[len-1] == ' ') {
            len--;
        } else if (len >= 2 && tooltip[len-1] == 'n' && tooltip[len-2] == '\\') {
            len -= 2;
        } else {
            break;
        }
    }
    tooltip[len] = '\0';

    setenv("TZ", timezones[tz_index], 1);
    tzset();

    printf("{\"text\": \"%s <span color='#7d6b5e'>|</span> %s%s\", \"tooltip\": \"<tt>%s</tt>\", \"class\": \"clock\"}\n", date_str, time_str, flags[tz_index], tooltip);
    fflush(stdout);
}

int main() {
    // Exit immediately if Waybar's launcher shell dies during a reload.
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() == 1) return 0;

    // systemd ignores SIGPIPE for services; restore the normal behavior so this
    // helper exits when Waybar closes its stdout pipe during a reload.
    signal(SIGPIPE, SIG_DFL);
    signal(SIGUSR1, update_display);
    signal(SIGUSR2, update_display);

    update_display(0);

    while (1) {
        update_display(0);
        if (show_seconds) {
            sleep(1);
        } else {
            time_t now = time(NULL);
            struct tm *tm_info = localtime(&now);
            int seconds_left = 60 - tm_info->tm_sec;
            sleep(seconds_left);
        }
    }
    return 0;
}
