#!/bin/sh
# Shared by the .deb (postinst configure) and .rpm (%post). Never fails the install: a missing module
# (e.g. Secure Boot without an enrolled key) is reported and fixed later, see docs/USER_GUIDE.md.
case "$1" in configure|1|2|"") ;; *) exit 0 ;; esac
if ! modprobe v4l2loopback 2>/dev/null; then
    echo "tethercam: could not load v4l2loopback now; reboot, or see the Linux section of the user guide" >&2
fi
gtk-update-icon-cache -q -t /usr/share/icons/hicolor 2>/dev/null || true
systemctl --global enable tethercam-agent.service >/dev/null 2>&1 || true
exit 0
