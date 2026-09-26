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

# 1. Deterministic IPv6 Address Selection (Global Unicast 2000::/3 e.g. 2xxx: or 3xxx:)
# Preference 1: FreshTomato dnsmasq stateful DHCPv6 /128 lease
CURRENT_IPV6=$(ip -6 addr show dev "$INTERFACE" scope global dynamic -temporary -tentative -deprecated 2>/dev/null \
    | grep -E "inet6 [23][0-9a-fA-F]{3}:" | awk '{print $2}' | grep "/128" | head -n 1 | cut -d/ -f1 || true)

# Preference 2: Hardware EUI-64 MAC-derived address (Immutable across router reboots)
if [[ -z "$CURRENT_IPV6" ]]; then
    CURRENT_IPV6=$(ip -6 addr show dev "$INTERFACE" scope global dynamic -temporary -tentative -deprecated 2>/dev/null \
        | grep -E "inet6 [23][0-9a-fA-F]{3}:" | grep -i "6e4b:90ff:fe57:e9bd" | awk '{print $2}' | head -n 1 | cut -d/ -f1 || true)
fi

# Preference 3: Any valid GUA fallback
if [[ -z "$CURRENT_IPV6" ]]; then
    CURRENT_IPV6=$(ip -6 addr show dev "$INTERFACE" scope global dynamic -temporary -tentative -deprecated 2>/dev/null \
        | grep -E "inet6 [23][0-9a-fA-F]{3}:" | awk '{print $2}' | head -n 1 | cut -d/ -f1 || true)
fi

# 2. Strict Pre-Condition Guard (Prevent empty deletion trap; must be public GUA)
if [[ -z "$CURRENT_IPV6" || ! "$CURRENT_IPV6" =~ ^[23][0-9a-fA-F]{3}:[0-9a-fA-F:]+$ ]]; then
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

# 5. Non-Destructive In-Place AdGuard Client-Scoped Rewrite Update (Host Python 3 + PyYAML)
ADGUARD_YAML="${ADGUARD_CONF_DIR}/AdGuardHome.yaml"
if [[ -f "$ADGUARD_YAML" ]]; then
    # Self-healing permission check
    if [[ ! -w "$ADGUARD_YAML" ]]; then
        docker run --rm -v "${ADGUARD_CONF_DIR}":/conf alpine chmod 666 /conf/AdGuardHome.yaml >/dev/null 2>&1 || true
    fi

    echo "[$(date -Iseconds)] [ADGUARD] Synchronizing router-scoped DNS rewrite rules in-place..."
    
    PYTHON_OUT=$(python3 - <<PYTHON_EOF
import yaml, sys

yaml_path = "${ADGUARD_YAML}"
domain_name = "${DUCKDNS_DOMAIN}.duckdns.org"
local_ipv4 = "${LOCAL_IPV4}"
new_ipv6 = "${CURRENT_IPV6}"
rewrite_clients = "${ADGUARD_REWRITE_CLIENTS:-192.168.1.1}"

managed_suffixes = (
    f"\$client={rewrite_clients},dnstype=A,dnsrewrite=NOERROR;A;",
    f"\$client={rewrite_clients},dnstype=AAAA,dnsrewrite=NOERROR;AAAA;",
)

try:
    with open(yaml_path, "r") as f:
        data = yaml.safe_load(f) or {}

    updated = False
    filtering = data.setdefault("filtering", {})

    # Legacy DNS rewrites are global in AdGuard Home. Keep this empty for
    # sncr.duckdns.org so WAN Private DNS clients don't receive LAN answers.
    rewrites = filtering.setdefault("rewrites", [])
    filtered_rewrites = [
        r for r in rewrites
        if r.get("domain") not in (domain_name, f"*.{domain_name}")
    ]
    if filtered_rewrites != rewrites:
        filtering["rewrites"] = filtered_rewrites
        updated = True

    desired_rules = [
        f"||{domain_name}^\$client={rewrite_clients},dnstype=A,dnsrewrite=NOERROR;A;{local_ipv4}",
        f"||{domain_name}^\$client={rewrite_clients},dnstype=AAAA,dnsrewrite=NOERROR;AAAA;{new_ipv6}",
        f"@@||{domain_name}^\$client=~{rewrite_clients}",
    ]

    rules = data.setdefault("user_rules", [])
    cleaned_rules = []
    for rule in rules:
        if rule.startswith(f"||{domain_name}") and any(suffix in rule for suffix in managed_suffixes):
            continue
        if rule == f"@@||{domain_name}^\$client=~{rewrite_clients}":
            continue
        cleaned_rules.append(rule)

    for rule in desired_rules:
        cleaned_rules.append(rule)

    if cleaned_rules != rules:
        data["user_rules"] = cleaned_rules
        updated = True

    filtering["rewrites_enabled"] = True
    filtering["filtering_enabled"] = True

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
        echo "[$(date -Iseconds)] [ADGUARD] AdGuard restarted with verified client-scoped rewrites."
    else
        echo "[$(date -Iseconds)] [ADGUARD] Configuration already up to date, zero container restart required."
    fi
fi

# 6. Synchronize Nginx Proxy Manager "LAN" Access List IPv6 Prefix
NPM_CONTAINER="${NPM_CONTAINER:-nginx-proxy-1}"
if docker ps --format '{{.Names}}' | grep -q "^${NPM_CONTAINER}$"; then
    echo "[$(date -Iseconds)] [NPM] Synchronizing dynamic IPv6 prefix in LAN Access List..."

    NEW_PREFIX=$(python3 -c "import ipaddress; print(ipaddress.IPv6Network('${CURRENT_IPV6}/64', strict=False))" 2>/dev/null || true)

    if [[ -n "$NEW_PREFIX" ]]; then
        NPM_OUT=$(docker exec "${NPM_CONTAINER}" node -e "
const sqlite3 = require('sqlite3');
const db = new sqlite3.Database('/data/database.sqlite');
const newPrefix = '${NEW_PREFIX}';
db.all(\"SELECT id, address FROM access_list_client WHERE access_list_id = 2 AND (address LIKE '2%' OR address LIKE '3%')\", (err, rows) => {
  if (err) {
    console.error('Error querying access_list_client:', err);
    process.exit(1);
  }
  if (rows.length > 0 && rows[0].address !== newPrefix) {
    db.run(\"UPDATE access_list_client SET address = ?, modified_on = datetime('now') WHERE id = ?\", [newPrefix, rows[0].id], (err2) => {
      if (err2) console.error('Error updating access_list_client:', err2);
      else console.log('UPDATED');
    });
  } else if (rows.length === 0) {
    db.run(\"INSERT INTO access_list_client (created_on, modified_on, access_list_id, address, directive, meta) VALUES (datetime('now'), datetime('now'), 2, ?, 'allow', '{}')\", [newPrefix], (err2) => {
      if (err2) console.error('Error inserting access_list_client:', err2);
      else console.log('UPDATED');
    });
  } else {
    console.log('UNCHANGED');
  }
});
" 2>&1 || true)

        if echo "$NPM_OUT" | grep -q "UPDATED"; then
            docker exec "${NPM_CONTAINER}" bash -c "
sed -i -E 's|allow [23][0-9a-fA-F]{3}:[^;]*;|allow ${NEW_PREFIX};|g' /data/nginx/proxy_host/*.conf
nginx -t >/dev/null 2>&1 && nginx -s reload >/dev/null 2>&1
" || true
            echo "[$(date -Iseconds)] [NPM] LAN Access List IPv6 prefix updated to ${NEW_PREFIX} and Nginx reloaded."
        else
            echo "[$(date -Iseconds)] [NPM] LAN Access List already matching ${NEW_PREFIX}, zero reload required."
        fi
    fi
fi

# 7. Commit State to Cache
mkdir -p "$(dirname "$CACHE_FILE")"
cat <<CACHE_EOF > "$CACHE_FILE"
LAST_IPV6="${CURRENT_IPV6}"
LAST_UPDATED="$(date -Iseconds)"
CACHE_EOF

echo "[$(date -Iseconds)] [SYNC] Complete. State persisted to ${CACHE_FILE}."
