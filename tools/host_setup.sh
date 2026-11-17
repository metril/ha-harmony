#!/bin/sh
# ha-harmony host setup: make the Harmony Hub and remote reachable from userspace.
#
# The kernel leaves both devices unbound (Driver=[none]), so no /dev/hidraw node
# exists for them. We ask usbhid to stop ignoring them (HID_QUIRK_NO_IGNORE,
# 0x40000000) and give the plugdev group access to both the hidraw node and the
# raw USB node.
#
# Run once, as root:  sudo tools/host_setup.sh
# Undo:               sudo tools/host_setup.sh --undo
set -eu

RULES=/etc/udev/rules.d/99-ha-harmony.rules
MODCONF=/etc/modprobe.d/ha-harmony.conf
QUIRKS="0x046d:0xc129:0x40000000,0x046d:0xc12b:0x40000000"
HERE=$(dirname "$0")

[ "$(id -u)" = 0 ] || { echo "run as root: sudo $0" >&2; exit 1; }

if [ "${1:-}" = "--undo" ]; then
	rm -f "$RULES" "$MODCONF"
	udevadm control --reload
	udevadm trigger
	echo "removed $RULES and $MODCONF; reboot or reload usbhid to drop the quirks"
	exit 0
fi

cp "$HERE/udev/99-ha-harmony.rules" "$RULES"
printf 'options usbhid quirks=%s\n' "$QUIRKS" >"$MODCONF"

udevadm control --reload
udevadm trigger

# usbhid's `quirks` parameter is 0444: it can only be set as the module loads, so
# apply it by reloading usbhid. Any USB keyboard or mouse it drives drops out for
# about a second. The reload is made atomic with a trap, so usbhid comes back even
# if this script is interrupted. If usbhid is built into the kernel instead, say so
# and leave the boot-time options file as the way in.
if grep -q '^usbhid ' /proc/modules; then
	echo "reloading usbhid with the no-ignore quirks (USB input drops out briefly)..."
	trap 'modprobe usbhid 2>/dev/null || true' EXIT HUP INT TERM
	if modprobe -r usbhid; then
		modprobe usbhid "quirks=$QUIRKS"
	else
		echo "error: could not unload usbhid (a device may be holding it open)." >&2
		echo "       reboot to pick up $MODCONF, or close whatever holds it and retry." >&2
	fi
	trap - EXIT HUP INT TERM
	# Give usb core a moment to re-probe the devices it just lost.
	i=0
	while [ $i -lt 20 ] && [ ! -e /sys/bus/usb/drivers/usbhid/module ]; do
		i=$((i + 1))
		sleep 0.1
	done
	udevadm settle 2>/dev/null || true
elif [ -e /sys/module/usbhid ]; then
	echo "note: usbhid is built into the kernel, so it cannot be reloaded." >&2
	echo "      add this to the kernel command line and reboot:" >&2
	echo "      usbhid.quirks=$QUIRKS" >&2
else
	echo "note: usbhid is not loaded; $MODCONF applies when it loads." >&2
fi

# Rebind by unbinding/rebinding the USB interface, so usbhid re-probes with the
# new quirks. Harmless if the device is already bound.
for dev in /sys/bus/usb/devices/*; do
	[ -r "$dev/idVendor" ] || continue
	vid=$(cat "$dev/idVendor")
	pid=$(cat "$dev/idProduct")
	[ "$vid" = "046d" ] || continue
	case "$pid" in c129|c12b) ;; *) continue ;; esac
	for iface in "$dev":*; do
		[ -d "$iface" ] || continue
		name=$(basename "$iface")
		echo "$name" >/sys/bus/usb/drivers/usbhid/unbind 2>/dev/null || true
		echo "$name" >/sys/bus/usb/drivers/usbhid/bind 2>/dev/null ||
			echo "note: usbhid did not claim $name" >&2
	done
done

echo "--- result:"
for d in /sys/class/hidraw/hidraw*; do
	[ -e "$d" ] || continue
	printf '%s %s\n' "$(basename "$d")" "$(sed -n 's/^HID_NAME=//p' "$d/device/uevent")"
done
ls -l /dev/hidraw* 2>/dev/null || echo "no hidraw nodes"
