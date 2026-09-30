#!/bin/sh
# /etc/NetworkManager/dispatcher.d/09-timezone
# Automatically updates system timezone (/etc/localtime via timedatectl)
# and caches geolocation data for statusbar prayer times and clocks on network changes.

INTERFACE="$1"
ACTION="$2"

# Run on connection up, connectivity change, or manual execution
if [ -n "$ACTION" ] && [ "$ACTION" != "up" ] && [ "$ACTION" != "connectivity-change" ]; then
    exit 0
fi

# Try multiple geolocation JSON endpoints with fallback
RESP=""
NEW_TZ=""

for url in \
    "http://ip-api.com/json" \
    "https://ipapi.co/json/" \
    "https://ipinfo.io/json"
do
    CANDIDATE_RESP=$(curl -s --max-time 4 -H "User-Agent: statusbar-tz/1.0" "$url" 2>/dev/null)
    if [ -n "$CANDIDATE_RESP" ]; then
        TZ_CANDIDATE=$(echo "$CANDIDATE_RESP" | tr -d '\r\n' | sed -n 's/.*"timezone"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
        if [ -n "$TZ_CANDIDATE" ] && [ -f "/usr/share/zoneinfo/$TZ_CANDIDATE" ]; then
            RESP="$CANDIDATE_RESP"
            NEW_TZ="$TZ_CANDIDATE"
            break
        fi
    fi
done

if [ -z "$NEW_TZ" ]; then
    exit 0
fi

# Cache geolocation JSON for statusbar (prayer times + clock deduplication)
mkdir -p /var/cache
printf "%s\n" "$RESP" > /var/cache/statusbar_geoloc.json.tmp && \
    chmod 644 /var/cache/statusbar_geoloc.json.tmp && \
    mv -f /var/cache/statusbar_geoloc.json.tmp /var/cache/statusbar_geoloc.json

# Also sync to user cache directories if present
for udir in /home/*/.cache /usr/local/google/home/*/.cache; do
    if [ -d "$udir" ]; then
        cp -f /var/cache/statusbar_geoloc.json "$udir/statusbar_geoloc.json" 2>/dev/null && \
            chown --reference="$udir" "$udir/statusbar_geoloc.json" 2>/dev/null && \
            chmod 644 "$udir/statusbar_geoloc.json" 2>/dev/null
    fi
done

# Compare with current system timezone
CURRENT_TZ=$(timedatectl show -p Timezone --value 2>/dev/null)
if [ -z "$CURRENT_TZ" ] && [ -L /etc/localtime ]; then
    CURRENT_TZ=$(readlink /etc/localtime | sed 's#.*/zoneinfo/##')
fi

if [ "$NEW_TZ" != "$CURRENT_TZ" ]; then
    logger -t 09-timezone "Updating system timezone from '$CURRENT_TZ' to '$NEW_TZ'"
    timedatectl set-timezone "$NEW_TZ" 2>/dev/null || ln -sf "/usr/share/zoneinfo/$NEW_TZ" /etc/localtime
fi

# Notify running statusbar instances to reload location and timezone immediately
pkill -USR2 statusbar 2>/dev/null || true

