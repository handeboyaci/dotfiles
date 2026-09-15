#!/bin/bash
# ==============================================================================
# Production Netlink IPv6 Address Event Monitor
# Listens for kernel RTM_NEWADDR / RTM_DELADDR events on the physical interface.
# Implements sliding window debouncing and hard burst ceilings.
# ==============================================================================
set -euo pipefail

INTERFACE="enp0s31f6"
DEBOUNCE_SEC=5
MAX_BURST_SEC=15
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UPDATER="${SCRIPT_DIR}/update-duckdns-adguard.sh"

echo "[$(date -Iseconds)] [INFO] Starting Netlink IPv6 Monitor on interface: ${INTERFACE}"

while true; do
    # Verify interface exists before attaching monitor
    if ! ip link show dev "${INTERFACE}" >/dev/null 2>&1; then
        echo "[$(date -Iseconds)] [WARN] Interface ${INTERFACE} not found. Retrying in 5s..." >&2
        sleep 5
        continue
    fi

    # Monitor kernel address events in line-buffered mode
    ip -6 monitor address dev "${INTERFACE}" 2>/dev/null | while true; do
        if ! read -r line; then
            echo "[$(date -Iseconds)] [WARN] Netlink pipe closed. Restarting monitor..." >&2
            break
        fi

        # Filter strictly for global unicast prefix lines (2000::/3 e.g. 2xxx: or 3xxx:)
        # Ignore link-local (fe80:), ULA (fd00:), and loopback (::1)
        if [[ ! "$line" =~ (inet6 [23][0-9a-fA-F]{3}:|scope global) ]]; then
            continue
        fi

        BURST_START=$(date +%s)
        DRAINED_COUNT=0

        # Sliding window debounce loop
        while true; do
            if read -t "${DEBOUNCE_SEC}" -r extra_line; then
                ((DRAINED_COUNT++)) || true
                NOW=$(date +%s)
                # Hard ceiling: Prevent starvation during infinite flapping
                if (( NOW - BURST_START >= MAX_BURST_SEC )); then
                    echo "[$(date -Iseconds)] [WARN] Debounce ceiling (${MAX_BURST_SEC}s) reached. Drained ${DRAINED_COUNT} events."
                    break
                fi
            else
                # Window expired (silence on interface for DEBOUNCE_SEC)
                break
            fi
        done

        # Execute updater under non-blocking lock
        echo "[$(date -Iseconds)] [EVENT] Settled after ${DRAINED_COUNT} burst events. Invoking synchronizer..."
        "$UPDATER" || true
    done

    sleep 5
done
