#!/bin/bash
set -e

PATCH_FILE="$(dirname "$0")/roundcube-search-date-operators.patch"

if [ ! -f "$PATCH_FILE" ]; then
    echo "Error: Patch file $PATCH_FILE not found."
    exit 1
fi

echo "Applying search patch to roundcube-mailcow container..."
docker compose exec -T roundcube-mailcow patch -p1 -d /var/www/html < "$PATCH_FILE"
echo "Patch applied successfully."
