#!/bin/bash
exec >/run/user/32011/gs.log 2>&1
echo "=== gs-wrap start $(date +%T) ==="
# Scrub COMPOSITOR-only env, but KEEP the drmadapter client EGL env so every
# client launched in the session (phosh AND apps) renders via drmadapter.
unset HYBRIS_WLROOTS GBM_BACKEND GBM_BACKENDS_PATH WLR_DRM_DEVICES WLR_RENDERER WLR_BACKENDS LD_PRELOAD
export EGL_PLATFORM=wayland HYBRIS_EGLPLATFORM=drmadapter
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_libhybris.json
export WAYLAND_DISPLAY=wayland-0
export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=Phosh:GNOME XDG_SESSION_DESKTOP=phosh
# Make sure apps launched via the systemd --user manager inherit the EGL env too.
systemctl --user import-environment EGL_PLATFORM HYBRIS_EGLPLATFORM __EGL_VENDOR_LIBRARY_FILENAMES WAYLAND_DISPLAY 2>/dev/null
exec gnome-session --session=phosh
