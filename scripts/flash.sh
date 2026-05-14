#!/usr/bin/env bash
#
# scripts/flash.sh — build + flash a deskhop-bt UF2 via picotool.
#
# Usage:
#   scripts/flash.sh                       # build pico2_w, wait for BOOTSEL, flash
#   scripts/flash.sh --no-build            # skip build, just flash the existing UF2
#   scripts/flash.sh --board pico          # original Pico (RP2040)
#   scripts/flash.sh --build-dir build-x   # use a non-default build directory
#   scripts/flash.sh --info                # show info on whatever is in BOOTSEL right now
#
# Env overrides:
#   PICO_BOARD   default pico2_w
#   BUILD_DIR    default build-prod (pico2_w) or build (pico)
#   ARM_PATH     default /Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin
#
# Pico-side prep: hold BOOTSEL while plugging USB so the chip enumerates as
# RP2350 / RP2 Boot. This script polls picotool until that device appears.

set -euo pipefail

BOARD="${PICO_BOARD:-pico2_w}"
BUILD_DIR_DEFAULT=""
BUILD=1
INFO_ONLY=0
ARM_PATH="${ARM_PATH:-/Applications/ArmGNUToolchain/15.2.rel1/arm-none-eabi/bin}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-build) BUILD=0; shift ;;
        --board)    BOARD="$2"; shift 2 ;;
        --build-dir) BUILD_DIR_DEFAULT="$2"; shift 2 ;;
        --info)     INFO_ONLY=1; shift ;;
        -h|--help)  sed -n '2,/^$/p' "$0"; exit 0 ;;
        *)          echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$BUILD_DIR_DEFAULT" ]]; then
    case "$BOARD" in
        pico2_w) BUILD_DIR="build-prod" ;;
        pico)    BUILD_DIR="build" ;;
        *)       BUILD_DIR="build-$BOARD" ;;
    esac
else
    BUILD_DIR="$BUILD_DIR_DEFAULT"
fi

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

wait_for_bootsel() {
    if picotool info >/dev/null 2>&1; then
        return 0
    fi
    echo ">> waiting for BOOTSEL device (hold BOOTSEL + plug USB)..."
    local tries=0
    until picotool info >/dev/null 2>&1; do
        sleep 0.5
        tries=$((tries + 1))
        if (( tries == 20 )); then
            echo "   still waiting... (Ctrl-C to abort)"
        fi
    done
}

if (( INFO_ONLY )); then
    wait_for_bootsel
    picotool info -a
    exit 0
fi

if (( BUILD )); then
    export PATH="$ARM_PATH:$PATH"
    echo ">> cmake configure ($BOARD -> $BUILD_DIR)"
    cmake -S . -B "$BUILD_DIR" -DPICO_BOARD="$BOARD" >/dev/null
    echo ">> cmake build"
    cmake --build "$BUILD_DIR" -j
fi

UF2="$BUILD_DIR/deskhop.uf2"
if [[ ! -f "$UF2" ]]; then
    echo "no UF2 at $UF2 — build first or check --build-dir" >&2
    exit 1
fi

wait_for_bootsel

echo ">> picotool info (target):"
picotool info | sed 's/^/   /'

echo ">> flashing $UF2"
picotool load -fx "$UF2"

echo ">> done — device should now be running deskhop"
