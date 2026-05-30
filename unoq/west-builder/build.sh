#!/usr/bin/env bash
# Docker-wrapped `west build` for mainline Zephyr on the UNO Q.
#
# Usage:
#   build.sh [-b BOARD] [-p] <app-path> [-- <extra west/cmake args>]
#   build.sh shell                          # interactive shell in the env
#   build.sh -- west <any west subcommand>  # run an arbitrary west command
#
# Defaults: BOARD=arduino_uno_q. <app-path> is relative to the workspace root
# (e.g. zephyr/samples/basic/blinky) or an absolute path inside the workspace.
# Output lands in <workspace>/build/zephyr/zephyr.bin on the host.
set -euo pipefail

IMAGE=unoq-west-builder:v1
WORKSPACE="${ZEPHYR_WORKSPACE:-$HOME/Dev/claude/zephyr-mainline/zephyrproject}"
# Real Zephyr SDK 1.0.1. NOTE: the SDK contains ABSOLUTE symlinks (e.g.
# gnu/arm-zephyr-eabi -> $SDK/arm-zephyr-eabi), so it MUST be mounted at the
# same path inside the container as on the host or the toolchain dangles.
SDK="${ZEPHYR_SDK_DIR:-$HOME/zephyr-sdk-aarch64}"
# Out-of-tree apps mount at /apps so you can build e.g. `./build.sh /apps/spi_echo`
# without copying into the Zephyr workspace.
APPS="${ZEPHYR_APPS_DIR:-$HOME/Dev/claude/zephyr-mainline/apps}"
# Out-of-tree Zephyr modules (drivers/bindings) mount at /modules.
MODULES="${ZEPHYR_MODULES_DIR:-$HOME/Dev/claude/zephyr-mainline/modules}"
BOARD=arduino_uno_q
PRISTINE=""
BUILD_DIR="build"

# Common docker args: workspace at /work, SDK at its own host path (symlinks),
# out-of-tree apps at /apps, out-of-tree modules at /modules.
DOCKER_COMMON=(
    --rm
    -e LOCAL_UID="$(id -u)" -e LOCAL_GID="$(id -g)"
    -e ZEPHYR_WORKSPACE=/work
    -e ZEPHYR_SDK_INSTALL_DIR="$SDK"
    -v "$WORKSPACE":/work
    -v "$SDK":"$SDK"
    -v "$APPS":/apps
    -v "$MODULES":/modules
)

# --- interactive shell shortcut ---
if [ "${1:-}" = "shell" ]; then
    exec docker run -it "${DOCKER_COMMON[@]}" "$IMAGE"
fi

# --- raw passthrough: build.sh -- <cmd...> ---
if [ "${1:-}" = "--" ]; then
    shift
    exec docker run "${DOCKER_COMMON[@]}" "$IMAGE" "$@"
fi

# --- parse build args ---
EXTRA=()
APP=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        -b) BOARD="$2"; shift 2 ;;
        -p) PRISTINE="-p always"; shift ;;
        -d) BUILD_DIR="$2"; shift 2 ;;
        --) shift; EXTRA=("$@"); break ;;
        *)  APP="$1"; shift ;;
    esac
done

if [ -z "$APP" ]; then
    echo "usage: build.sh [-b BOARD] [-p] <app-path> [-- extra args]" >&2
    exit 2
fi

echo "build.sh: board=$BOARD app=$APP workspace=$WORKSPACE sdk=$SDK"

docker run "${DOCKER_COMMON[@]}" \
    "$IMAGE" \
    west build $PRISTINE -b "$BOARD" -d "$BUILD_DIR" "$APP" "${EXTRA[@]}"

echo "build.sh: done -> $WORKSPACE/$BUILD_DIR/zephyr/zephyr.bin"
ls -l "$WORKSPACE/$BUILD_DIR/zephyr/zephyr.bin" 2>/dev/null || true
