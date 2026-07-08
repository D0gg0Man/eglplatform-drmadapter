#!/bin/bash
# Bring up the Plasma Mobile test stack (KWin on drmadapter + plasmashell mobile).
# Stops phosh to free the composer (DRM master). Throwaway test partition.
set -u
ASKPASS=/home/furios/.sudo_askpass
export SUDO_ASKPASS=$ASKPASS
PROF="${1:-}"   # pass "prof" to enable HYBRIS_WL_SHM_PROF

mkdir -p /home/furios/.config/kwin-mali

echo "== stopping phosh =="
sudo -A systemctl stop phosh-drm.service 2>/dev/null
sleep 1

echo "== onlining CPU cores (vendor power daemon parks them at boot) =="
for c in /sys/devices/system/cpu/cpu[0-9]*; do
  echo 1 | sudo -A tee "$c/online" >/dev/null 2>&1
done
for p in /sys/devices/system/cpu/cpufreq/policy*; do
  echo performance | sudo -A tee "$p/scaling_governor" >/dev/null 2>&1
done
echo "cores online: $(cat /sys/devices/system/cpu/online)"

echo "== starting KWin (kwin-mali-test) =="
sudo -A systemctl restart kwin-mali-test.service 2>/dev/null
for i in $(seq 1 20); do
  [ -S /run/user/32011/wayland-kwin ] && break
  sleep 0.3
done
ls -la /run/user/32011/wayland-kwin 2>/dev/null || { echo "!! kwin socket missing"; }

echo "== starting kactivitymanagerd (plasmashell hard-depends on it) =="
sudo -A systemctl stop kactivity.service 2>/dev/null
sudo -A systemd-run --unit=kactivity --collect \
  -p User=32011 -p Group=32011 -p LimitNOFILE=1048576 \
  --setenv=XDG_RUNTIME_DIR=/run/user/32011 \
  --setenv=WAYLAND_DISPLAY=wayland-kwin \
  --setenv=DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus \
  --setenv=HOME=/home/furios \
  --setenv=QT_QPA_PLATFORM=wayland \
  /usr/lib/aarch64-linux-gnu/libexec/kactivitymanagerd 2>&1 | tail -1
sleep 2

echo "== rebuilding sycoca in the session env (stale caches empty the app drawer) =="
sudo -A -u furios env HOME=/home/furios XDG_RUNTIME_DIR=/run/user/32011 \
  DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus \
  XDG_MENU_PREFIX=plasma- XDG_CURRENT_DESKTOP=KDE \
  XDG_DATA_DIRS=/var/lib/flatpak/exports/share:/usr/local/share/:/usr/share/ \
  XDG_CONFIG_DIRS=/home/furios/.config/plasma-mobile:/etc/xdg \
  kbuildsycoca6 2>/dev/null

echo "== launching plasmashell (pshell.service) =="
sudo -A systemctl stop pshell.service 2>/dev/null
EXTRA=""
[ "$PROF" = "prof" ] && EXTRA="--setenv=HYBRIS_WL_SHM_PROF=1"
sudo -A systemd-run --unit=pshell --collect \
  -p User=32011 -p Group=32011 -p LimitNOFILE=1048576 \
  --setenv=XDG_RUNTIME_DIR=/run/user/32011 \
  --setenv=WAYLAND_DISPLAY=wayland-kwin \
  --setenv=DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus \
  --setenv=HOME=/home/furios \
  --setenv=QT_QPA_PLATFORM=wayland \
  --setenv=QSG_RENDER_LOOP=threaded \
  --setenv=QSG_RHI_BACKEND=opengl \
  --setenv=GBM_BACKEND=hybris \
  --setenv=GBM_BACKENDS_PATH=/usr/lib/aarch64-linux-gnu/gbm \
  --setenv=GBM_HYBRIS_LINEAR=1 \
  --setenv=HYBRIS_EGLPLATFORM=wayland \
  --setenv=HYBRIS_WL_SHM=1 \
  --setenv=HYBRIS_SWAP_INTERVAL=0 \
  $EXTRA \
  --setenv=__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_libhybris.json \
  --setenv=QT_QPA_PLATFORMTHEME=kde \
  --setenv=PLASMA_DEFAULT_SHELL=org.kde.plasma.mobileshell \
  --setenv=QT_QUICK_CONTROLS_MOBILE=true \
  --setenv=QT_QUICK_CONTROLS_STYLE=org.kde.breeze \
  --setenv=PLASMA_INTEGRATION_USE_PORTAL=1 \
  --setenv=XDG_CONFIG_DIRS=/home/furios/.config/plasma-mobile:/etc/xdg \
  --setenv=PLASMA_PLATFORM=phone:handset \
  --setenv=XDG_CURRENT_DESKTOP=KDE \
  --setenv=XDG_MENU_PREFIX=plasma- \
  /usr/bin/plasmashell 2>&1
sleep 3

echo "== propagating session env to user manager + dbus activation =="
# Apps launched from the shell go through systemd user scopes / dbus
# activation and inherit the USER MANAGER's env, not plasmashell's. Without
# this they get no WAYLAND_DISPLAY/hybris env -> wrong display, android_wlegl
# instead of wl_shm, portal stalls.
SESSION_ENVS="QT_QUICK_CONTROLS_MOBILE=true QT_QUICK_CONTROLS_STYLE=org.kde.breeze PLASMA_PLATFORM=phone:handset XDG_CONFIG_DIRS=/home/furios/.config/plasma-mobile:/etc/xdg WAYLAND_DISPLAY=wayland-kwin QT_QPA_PLATFORM=wayland GDK_BACKEND=wayland QT_QPA_PLATFORMTHEME=kde XDG_CURRENT_DESKTOP=KDE XDG_MENU_PREFIX=plasma- GBM_BACKEND=hybris GBM_BACKENDS_PATH=/usr/lib/aarch64-linux-gnu/gbm GBM_HYBRIS_LINEAR=1 HYBRIS_EGLPLATFORM=wayland HYBRIS_WL_SHM=1 HYBRIS_WL_SHM_XRGB=auto HYBRIS_SWAP_INTERVAL=0 QSG_RENDER_LOOP=threaded QSG_RHI_BACKEND=opengl __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_libhybris.json"
sudo -A -u furios env XDG_RUNTIME_DIR=/run/user/32011 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus \
  systemctl --user set-environment $SESSION_ENVS 2>/dev/null
sudo -A -u furios env XDG_RUNTIME_DIR=/run/user/32011 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/32011/bus \
  dbus-update-activation-environment $SESSION_ENVS 2>/dev/null

echo "== state =="
echo "kwin:  $(sudo -A systemctl is-active kwin-mali-test.service 2>/dev/null)"
echo "pshell:$(sudo -A systemctl is-active pshell.service 2>/dev/null)"
pgrep -a plasmashell | grep -v defunct | head -1
