#!/usr/bin/env bash
# Builds the Steam Deck bundle build/deck/dq9-deck/ inside Valve's public
# Steam Runtime 3 "sniper" SDK image, so the binary only needs what SteamOS
# and the sniper runtime provide (glibc <= 2.31, SDL2).
#
# usage: tools/build_deck.sh [--gen DIR] [--jobs N] [--tarball] [--smoke] [--rom PATH]
#   --gen DIR    separately supplied generated sources with arm9/ and arm7/
#                (required unless DQ9_GEN is set)
#   --jobs N     parallel compile jobs inside the container
#   --tarball    also write build/deck/dq9-deck.tar.gz
#   --smoke      afterwards run `--headless --frames 5` with the selected image
#                read-only inside the sniper runtime container and natively
# Environment: DQ9_GEN, DQ9_ROM, DQ9_DECK_SDK_IMAGE, DQ9_DECK_RUNTIME_IMAGE.
# Images are only pulled, never pushed.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
sdk_image="${DQ9_DECK_SDK_IMAGE:-registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest}"
runtime_image="${DQ9_DECK_RUNTIME_IMAGE:-registry.gitlab.steamos.cloud/steamrt/sniper/platform:latest}"
gen="${DQ9_GEN:-}"
jobs=""
tarball=0
smoke=0
smoke_rom="${DQ9_ROM:-}"

usage() {
    cat <<'EOF'
Usage: tools/build_deck.sh [--gen DIR] [--jobs N] [--tarball] [--smoke] [--rom PATH]
Generated sources must be supplied with --gen DIR or DQ9_GEN.
--smoke uses an already available compatible image selected with --rom PATH or
DQ9_ROM; the existing --smoke PATH form is also accepted.
Environment: DQ9_DECK_SDK_IMAGE, DQ9_DECK_RUNTIME_IMAGE.
EOF
    exit "$1"
}
need_value() { [ "$#" -ge 2 ] || { echo "build_deck.sh: $1 requires a value" >&2; usage 2; }; }
while [ $# -gt 0 ]; do
    case "$1" in
        --gen) need_value "$@"; gen="$2"; shift 2 ;;
        --jobs) need_value "$@"; jobs="$2"; shift 2 ;;
        --tarball) tarball=1; shift ;;
        --rom) need_value "$@"; smoke_rom="$2"; shift 2 ;;
        --smoke)
            smoke=1; shift
            if [ $# -gt 0 ] && [[ "$1" != -* ]]; then smoke_rom="$1"; shift; fi
            ;;
        -h|--help) usage 0 ;;
        *) echo "build_deck.sh: unknown argument '$1'" >&2; usage 2 ;;
    esac
done
if [ -n "$jobs" ] && ! [[ "$jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "build_deck.sh: --jobs must be a positive integer" >&2
    exit 2
fi
if [ "$smoke" = 1 ]; then
    if [ -z "$smoke_rom" ]; then
        echo "build_deck.sh: --smoke requires --rom PATH or DQ9_ROM" >&2
        exit 2
    fi
    if [ ! -f "$smoke_rom" ]; then
        echo "build_deck.sh: smoke ROM not found: $smoke_rom" >&2
        exit 1
    fi
fi

has_gen() { [ -f "$1/arm9/fn_table.cpp" ] && [ -f "$1/arm7/fn_table.cpp" ]; }
if [ -z "$gen" ]; then
    echo "build_deck.sh: separately supplied generated sources are required; use --gen DIR or DQ9_GEN" >&2
    exit 2
fi
has_gen "$gen" || { echo "build_deck.sh: $gen lacks arm9/fn_table.cpp or arm7/fn_table.cpp" >&2; exit 1; }
gen=$(cd "$gen" && pwd)
echo "== generated sources: $gen"
echo "== SDK image: $sdk_image"

docker image inspect "$sdk_image" >/dev/null 2>&1 || docker pull "$sdk_image"
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
    ${jobs:+-e DQ9_DECK_JOBS="$jobs"} \
    -v "$repo:/src" -v "$gen:/gen:ro" \
    "$sdk_image" /src/packaging/steamdeck/build-in-sniper.sh

bundle="$repo/build/deck/dq9-deck"
if [ "$tarball" = 1 ]; then
    tar -C "$repo/build/deck" -czf "$repo/build/deck/dq9-deck.tar.gz" dq9-deck
    echo "== tarball: $repo/build/deck/dq9-deck.tar.gz"
fi

echo "== package audit"
audit_paths=("$bundle")
if [ "$tarball" = 1 ]; then
    audit_paths+=("$repo/build/deck/dq9-deck.tar.gz")
fi
"$repo/.venv/bin/python" "$repo/tools/audit_package.py" "${audit_paths[@]}"

if [ "$smoke" = 1 ]; then
    smoke_rom=$(readlink -f "$smoke_rom")
    docker image inspect "$runtime_image" >/dev/null 2>&1 || docker pull "$runtime_image"
    echo "== smoke (container $runtime_image)"
    set +e
    docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
        -v "$bundle:/opt/dq9-deck:ro" -v "$smoke_rom:/rom.nds:ro" "$runtime_image" \
        /opt/dq9-deck/dq9.sh --rom /rom.nds --save /tmp/smoke.sav --headless --frames 5
    container_status=$?
    echo "== container exit status: $container_status"
    echo "== smoke (native host)"
    smoke_dir=$(mktemp -d)
    HOME="$smoke_dir" "$bundle/dq9.sh" --rom "$smoke_rom" --save "$smoke_dir/smoke.sav" --headless --frames 5
    native_status=$?
    echo "== native exit status: $native_status"
    rm -rf "$smoke_dir"
    set -e
    if [ "$container_status" -ne 0 ]; then exit "$container_status"; fi
    if [ "$native_status" -ne 0 ]; then exit "$native_status"; fi
fi
