#!/bin/bash
# Shell side of the phosh-on-DRM session (FuriLabs stack). Launched by
# phoc-drm-session-furi as a SEPARATE wayland client. Scrubs the compositor-only
# env but KEEPS the drmadapter client EGL env so every client in the session
# (phosh AND the apps it launches) renders through drmadapter.
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
exec >"$XDG_RUNTIME_DIR/gs.log" 2>&1
echo "=== phosh-drm-client start $(date +%T) ==="

# Drop the compositor-only knobs; these must NOT leak into the shell/apps.
unset HYBRIS_WLROOTS GBM_BACKEND GBM_BACKENDS_PATH WLR_DRM_DEVICES WLR_RENDERER WLR_BACKENDS LD_PRELOAD DRMADAPTER_HEARTBEAT

# Keep the drmadapter client EGL platform so clients render via hybris.
export EGL_PLATFORM=wayland HYBRIS_EGLPLATFORM=drmadapter
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_libhybris.json
export WAYLAND_DISPLAY=wayland-0
export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=Phosh:GNOME XDG_SESSION_DESKTOP=phosh

# Apps launched via the systemd --user manager must inherit the EGL env too.
systemctl --user import-environment EGL_PLATFORM HYBRIS_EGLPLATFORM __EGL_VENDOR_LIBRARY_FILENAMES WAYLAND_DISPLAY 2>/dev/null

exec gnome-session --session=phosh
