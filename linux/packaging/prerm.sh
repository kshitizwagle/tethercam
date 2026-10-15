#!/bin/sh
# .deb prerm (remove) and .rpm %preun (0 = erase). Upgrades keep the service enabled.
case "$1" in remove|0) ;; *) exit 0 ;; esac
systemctl --global disable tethercam-agent.service >/dev/null 2>&1 || true
exit 0
