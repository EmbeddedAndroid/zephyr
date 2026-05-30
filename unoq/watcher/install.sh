#!/bin/sh
# Install the zephyr-flash watcher on the device. Run ON the device (via
# `adb exec-out sh /home/root/zephyr-flash/install.sh`) after the unit files
# and flash-incoming.sh have been pushed into /home/root/zephyr-flash/.
set -eu

FLASH_DIR=/home/root/zephyr-flash
# /etc is writable on the ostree qcom-distro image; /usr is read-only.
UNIT_DIR=/etc/systemd/system

chmod +x "$FLASH_DIR/flash-incoming.sh"
chmod +x "$FLASH_DIR/oo/flash.sh" "$FLASH_DIR/oo/recover.sh" \
         "$FLASH_DIR/oo/bin/openocd" "$FLASH_DIR/oo/bin/gpioset" 2>/dev/null || true

install -m 0644 "$FLASH_DIR/zephyr-flash.path"    "$UNIT_DIR/zephyr-flash.path"
install -m 0644 "$FLASH_DIR/zephyr-flash.service" "$UNIT_DIR/zephyr-flash.service"

systemctl daemon-reload
systemctl enable --now zephyr-flash.path

echo "installed. watching $FLASH_DIR/incoming.bin"
systemctl --no-pager status zephyr-flash.path | head -5 || true
