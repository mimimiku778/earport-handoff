#!/usr/bin/env bash

set -euo pipefail

readonly UPSTREAM_URL="https://github.com/Anoryth/earport.git"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
readonly REPO_DIR
readonly BASE_FILE="$REPO_DIR/UPSTREAM_BASE"

if [ ! -f "$BASE_FILE" ]; then
    echo "Missing UPSTREAM_BASE" >&2
    exit 1
fi

base_revision="$(tr -d '[:space:]' < "$BASE_FILE")"
if [[ ! "$base_revision" =~ ^[0-9a-f]{40}$ ]]; then
    echo "UPSTREAM_BASE must contain one full Git commit ID" >&2
    exit 1
fi

if git -C "$REPO_DIR" remote get-url upstream >/dev/null 2>&1; then
    configured_url="$(git -C "$REPO_DIR" remote get-url upstream)"
    if [ "$configured_url" != "$UPSTREAM_URL" ]; then
        echo "The upstream remote points somewhere unexpected: $configured_url" >&2
        exit 1
    fi
else
    git -C "$REPO_DIR" remote add upstream "$UPSTREAM_URL"
fi

git -C "$REPO_DIR" fetch --quiet upstream main
upstream_head="$(git -C "$REPO_DIR" rev-parse upstream/main)"

if ! git -C "$REPO_DIR" merge-base --is-ancestor "$base_revision" "$upstream_head"; then
    echo "Recorded base $base_revision is not an ancestor of upstream/main" >&2
    exit 1
fi

echo "Reviewed upstream base: $base_revision"
echo "Current upstream head:  $upstream_head"

if [ "$base_revision" = "$upstream_head" ]; then
    echo "Status: up to date"
    exit 0
fi

echo "Status: updates available"
echo
git -C "$REPO_DIR" log \
    --no-merges \
    --format='- %h %s' \
    "$base_revision..$upstream_head"
