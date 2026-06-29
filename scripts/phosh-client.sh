#!/bin/bash
# Launch phosh 0.55 as a client of the DRM phoc above. Run AFTER phoc is up.
PHOSH="${PHOSH:-$HOME/repos/phosh-0.55/build/src/phosh}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export EGL_PLATFORM=wayland HYBRIS_EGLPLATFORM=drmadapter
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_libhybris.json
export XDG_CURRENT_DESKTOP=Phosh:GNOME XDG_SESSION_TYPE=wayland GNOME_SHELL_SESSION_MODE=phosh
export XDG_DATA_DIRS=/usr/local/share:/usr/share
unset HYBRIS_WLROOTS GBM_BACKEND GBM_BACKENDS_PATH WLR_DRM_DEVICES WLR_RENDERER WLR_BACKENDS
export LD_LIBRARY_PATH="$(dirname "$PHOSH")":$LD_LIBRARY_PATH
export GSETTINGS_SCHEMA_DIR="$(dirname "$PHOSH")/../data"
exec dbus-run-session -- sh -c '
  gsettings set org.gnome.desktop.session idle-delay 0
  gsettings set org.gnome.settings-daemon.plugins.power sleep-inactive-battery-type nothing
  gsettings set org.gnome.settings-daemon.plugins.power sleep-inactive-ac-type nothing
  exec "$0"' "$PHOSH"
