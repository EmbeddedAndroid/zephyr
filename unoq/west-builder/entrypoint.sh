#!/bin/sh
# Entrypoint for the west-builder image. Runs west (or any command) inside the
# mounted Zephyr workspace with ZEPHYR_BASE/SDK wired up. Drops to the caller's
# uid:gid (passed via -e LOCAL_UID/LOCAL_GID) so build/ artifacts aren't
# root-owned on the host.
set -eu

: "${ZEPHYR_WORKSPACE:=/work}"
export ZEPHYR_BASE="$ZEPHYR_WORKSPACE/zephyr"

# west needs a HOME for its config; give it a writable one.
export HOME=/tmp

# If the SDK's cmake package isn't registered, point CMake at it explicitly.
# (ZEPHYR_SDK_INSTALL_DIR is already set in the image env.)

cd "$ZEPHYR_WORKSPACE"

if [ "$#" -eq 0 ]; then
    exec /bin/sh
fi

# Run the requested command. If LOCAL_UID is provided and we're root, re-exec
# as that uid so output files are owned by the host user.
if [ "${LOCAL_UID:-0}" != "0" ] && [ "$(id -u)" = "0" ]; then
    # gosu-free: use a throwaway user matching the host uid/gid.
    addgroup --gid "${LOCAL_GID:-$LOCAL_UID}" builder 2>/dev/null || true
    adduser --uid "$LOCAL_UID" --gid "${LOCAL_GID:-$LOCAL_UID}" \
        --disabled-password --gecos "" builder 2>/dev/null || true
    export HOME=/tmp
    exec setpriv --reuid "$LOCAL_UID" --regid "${LOCAL_GID:-$LOCAL_UID}" \
        --init-groups --inh-caps=-all env \
        ZEPHYR_BASE="$ZEPHYR_BASE" \
        ZEPHYR_SDK_INSTALL_DIR="$ZEPHYR_SDK_INSTALL_DIR" \
        ZEPHYR_TOOLCHAIN_VARIANT="$ZEPHYR_TOOLCHAIN_VARIANT" \
        HOME=/tmp PATH="$PATH" \
        "$@"
fi

exec "$@"
