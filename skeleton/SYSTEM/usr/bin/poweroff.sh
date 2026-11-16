#!/bin/sh
##
# @file poweroff.sh (/sbin/shutdown is a symlink to it)
# @brief Power off (or reboot) the RG35XX, leaving /userdata clean.
#
# MinUI runs "shutdown" to power off; /sbin/halt (reboot, poweroff) runs it too, as "shutdown -r now" for a
# reboot or "shutdown -h now" to halt.
#
# The ext4 partitions have no journal, and the ramdisk's /cfw.sh runs fsck on boot whenever /userdata wasn't
# cleanly unmounted. rcK's K01mount unmounts /userdata, which writes the "clean" flag; if it was busy, it's
# remounted read-only here (the same flag), and if a process still has a file open for writing, which makes
# that fail too (EBUSY), the kernel's emergency remount (sysrq u) forces it. That one runs in the background,
# hence the second of grace before the power goes.
##

alsactl store 0 /userdata/system/.asound.state

amixer -q set "speaker on off switch" "off"

sync

/etc/init.d/rcK

if grep -q " /userdata " /proc/mounts && ! mount -o remount,ro /userdata 2>/dev/null; then
	echo u > /proc/sysrq-trigger
	sleep 1
fi

case " $* " in
	*" -r "*) echo b > /proc/sysrq-trigger ;;   # reboot
	*)        echo o > /proc/sysrq-trigger ;;   # power off
esac
