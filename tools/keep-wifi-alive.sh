#!/bin/sh
# keep-wifi-alive.sh — stop the rtl8821cs driver from dropping the link.
#
# The RG34XX's WiFi module loads with rtw_power_mgnt=2 (maximum power save),
# which drops the connection during long idle or low-traffic periods. That breaks
# SSH and the SMB share while the game runs, and it made device testing
# unreliable.
#
# /etc is an overlay that does not survive a reboot and /boot is read-only, so
# modprobe.d cannot be used: the module is loaded with insmod from
# /etc/init.d/S03modules, which ignores it. The setting therefore has to be
# re-applied at runtime, which is what this does.
#
# Run it on the device (root):
#     sh keep-wifi-alive.sh
# and from the port launcher, which calls it if it is installed.

PARAM=/sys/module/8821cs/parameters/rtw_power_mgnt
[ -w "$PARAM" ] || exit 0

current=$(cat "$PARAM" 2>/dev/null)
if [ "$current" != "0" ]; then
    echo 0 > "$PARAM" 2>/dev/null
    echo "wifi: rtw_power_mgnt $current -> $(cat "$PARAM" 2>/dev/null)"
fi
