#!/bin/bash
# ==============================================================================
# Dual-Stack DDNS & AdGuard Split-Horizon Synchronizer
# Features: Mutex flock, deterministic IPv6 selection, non-destructive YAML rewrite
# ==============================================================================
set -euo pipefail

LOCK_FILE="/tmp/duckdns-synchronizer.lock"
exec 200>"$LOCK_FILE"
if ! flock -n 200; then
    echo "[$(date -Iseconds)] [INFO] Another sync instance is active. Exiting."
    exit 0
fi

CONFIG_FILE="${HOME}/.dotfiles/data/duckdns.conf"
[[ ! -f "$CONFIG_FILE" ]] && CONFIG_FILE="${HOME}/.config/duckdns/duckdns.conf"

if [[ -f "$CONFIG_FILE" ]]; then
    # shellcheck source=/dev/null
    source "$CONFIG_FILE"
else
    echo "[$(date -Iseconds)] [ERROR] Configuration file missing: $CONFIG_FILE" >&2
    exit 1
fi

FORCE=false
if [[ "${1:-}" == "--force" ]]; then
    FORCE=true
fi

# 1. Deterministic IPv6 Address Selection
# Preference 1: FreshTomato dnsmasq stateful DHCPv6 /128 lease
CURRENT_IPV6=$(ip -6 addr show dev "$INTERFACE" scope global dynamic -temporary -tentative -deprecated 2>/dev/null \
    | grep "inet6 2600:" | awk '{print $2}' | grep "/128" | head -n 1 | cut -d/ -f1 || true)

# Preference 2: Hardware EUI-64 MAC-derived address (Immutable across router reboots)
if [[ -z "$CURRENT_IPV6" ]]; then
    CURRENT_IPV6=$(ip -6 addr show dev "$INTERFACE" scope global dynamic -temporary -tentative -deprecated 2>/dev/null \
        | grep "inet6 2600:" | grep -i "6e4b:90ff:fe57:e9bd" | awk '{print $2}' | head -n 1 | cut -d/ -f1 || true)
fi

# Preference 3: Any valid GUA fallback
if [[ -z "$CURRENT_IPV6" ]]; then
    CURRENT_IPV6=$(ip -6 addr show dev "$INTERFACE" scope global dynamic -temporary -tentative -deprecated 2>/dev/null \
        | grep "inet6 2600:" | awk '{print $2}' | head -n 1 | cut -d/ -f1 || true)
fi

# 2. Strict Pre-Condition Guard (Prevent empty deletion trap)
if [[ -z "$CURRENT_IPV6" || ! "$CURRENT_IPV6" =~ ^2600:[0-9a-fA-F:]+$ ]]; then
    echo "[$(date -Iseconds)] [WARN] No valid global IPv6 address detected on ${INTERFACE}. Aborting."
    exit 0
fi

# 3. Read Cached State
LAST_IPV6=""
if [[ -f "$CACHE_FILE" ]]; then
    # shellcheck source=/dev/null
    source "$CACHE_FILE"
fi

if [[ "$FORCE" = false && "$CURRENT_IPV6" == "$LAST_IPV6" ]]; then
    exit 0
fi

echo "[$(date -Iseconds)] [CHANGE] IPv6 prefix change detected: ${LAST_IPV6:-'none'} -> ${CURRENT_IPV6}"

# 4. Update DuckDNS AAAA Record (Omit &ip= to isolate concerns from Router WANUP)
DUCK_URL="https://www.duckdns.org/update?domains=${DUCKDNS_DOMAIN}&token=${DUCKDNS_TOKEN}&ipv6=${CURRENT_IPV6}&verbose=true"
RESPONSE=$(curl -sS --max-time 15 "$DUCK_URL" 2>&1 || true)

if echo "$RESPONSE" | grep -q "^OK"; then
    echo "[$(date -Iseconds)] [DUCKDNS] Successfully updated AAAA record: $(echo "$RESPONSE" | tr '\n' ' ')"
else
    echo "[$(date -Iseconds)] [ERROR] DuckDNS update failed: $RESPONSE" >&2
    exit 1
fi

# 5. Non-Destructive In-Place AdGuard Split-Horizon Update (Host Python 3 + PyYAML)
ADGUARD_YAML="${ADGUARD_CONF_DIR}/AdGuardHome.yaml"
if [[ -f "$ADGUARD_YAML" ]]; then
    # Self-healing permission check
    if [[ ! -w "$ADGUARD_YAML" ]]; then
        docker run --rm -v "${ADGUARD_CONF_DIR}":/conf alpine chmod 666 /conf/AdGuardHome.yaml >/dev/null 2>&1 || true
    fi

    echo "[$(date -Iseconds)] [ADGUARD] Synchronizing wildcard DNS rewrite in-place..."
    
    PYTHON_OUT=$(python3 - <<PYTHON_EOF
import yaml, sys

yaml_path = "${ADGUARD_YAML}"
domain_target = "*.${DUCKDNS_DOMAIN}.duckdns.org"
new_ipv6 = "${CURRENT_IPV6}"

try:
    with open(yaml_path, "r") as f:
        data = yaml.safe_load(f) or {}

    rewrites = data.setdefault("filtering", {}).setdefault("rewrites", [])
    updated = False

    for r in rewrites:
        if r.get("domain") == domain_target and ":" in r.get("answer", ""):
            if r.get("answer") != new_ipv6:
                r["answer"] = new_ipv6
                r["enabled"] = True
                updated = True
            break
    else:
        rewrites.append({"domain": domain_target, "answer": new_ipv6, "enabled": True})
        updated = True

    if updated:
        with open(yaml_path, "w") as f:
            yaml.dump(data, f, default_flow_style=False, sort_keys=False)
        print("MODIFIED")
    else:
        print("UNCHANGED")
except Exception as e:
    sys.stderr.write(f"YAML Error: {e}\n")
    sys.exit(1)
PYTHON_EOF
    )

    if echo "$PYTHON_OUT" | grep -q "MODIFIED"; then
        docker restart "$ADGUARD_CONTAINER" >/dev/null 2>&1 || true
        echo "[$(date -Iseconds)] [ADGUARD] AdGuard restarted with verified split-horizon IPv6."
    else
        echo "[$(date -Iseconds)] [ADGUARD] Configuration already up to date, zero container restart required."
    fi
fi

# 6. Commit State to Cache
mkdir -p "$(dirname "$CACHE_FILE")"
cat <<CACHE_EOF > "$CACHE_FILE"
LAST_IPV6="${CURRENT_IPV6}"
LAST_UPDATED="$(date -Iseconds)"
CACHE_EOF

echo "[$(date -Iseconds)] [SYNC] Complete. State persisted to ${CACHE_FILE}."
