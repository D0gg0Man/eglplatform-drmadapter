#!/bin/bash
# Compositor side of the phosh-on-DRM session for the FuriLabs stack
# (wlroots 0.17.4 + phoc 0.44 + stock phosh) via the hybris/HWC2 shims.
# Launched by phosh-drm.service. phoc and the shell run as SEPARATE processes;
# this script owns both lifetimes.
#
# Two device hazards:
#   1. The HWC2 composer client is a singleton -> phoc MUST exit cleanly to
#      release it or the next phoc aborts. On teardown: SIGTERM phoc + wait.
#   2. The shell must not attach before the blitter/output is up -> wait for the
#      output to come up, then settle, before launching the client.
#
# Cold-boot paint race: occasionally the shell never paints on the first try.
# We detect that via the drmadapter present heartbeat (written once the shell
# actually paints a frame) and exit non-zero so systemd restarts us; after
# StartLimitBurst, OnFailure=phosh.service falls back to stock phosh rather than
# stranding the user at black.
set -u

PHOC="${PHOC:-/home/furios/repos/furilabs-phoc/build/src/phoc}"
CLIENT="${CLIENT:-/usr/libexec/phosh-drm-client-furi}"

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export XDG_SEAT="${XDG_SEAT:-seat0}" XDG_VTNR="${XDG_VTNR:-7}"
export XDG_DATA_DIRS=/usr/local/share:/usr/share
# wlroots 0.17.4 (/usr/local) first, then the phoc build dir.
export LD_LIBRARY_PATH=/usr/local/lib/aarch64-linux-gnu:$(dirname "$PHOC")
export WLR_BACKENDS=drm,libinput WLR_RENDERER=gles2 WLR_DRM_DEVICES=/dev/dri/card0
export GBM_BACKEND=hybris GBM_BACKENDS_PATH=/usr/lib/aarch64-linux-gnu/gbm HYBRIS_EGLPLATFORM=drmadapter
export HYBRIS_WLROOTS=1
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_libhybris.json
export LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libdrm-hybris.so:/usr/local/lib/wlegl_server.so

# Warm up the HWC2 composer: the FIRST client after the HAL starts (cold boot)
# gets a mis-initialised display pipeline (black-flash flicker / blank session);
# any client after that is clean. Connect-init-disconnect once so the real
# compositor starts in the good state. Harmless when already warm.
/usr/libexec/hwc2-warmup 2>&1 | sed 's/^/phosh-drm: /' || true

PHOC_LOG="$XDG_RUNTIME_DIR/phoc-drm.log"
HEARTBEAT="$XDG_RUNTIME_DIR/drmadapter.heartbeat"
export DRMADAPTER_HEARTBEAT="$HEARTBEAT"
rm -f "$HEARTBEAT"

# Relight the panel: the soft-blank leaves the backlight sysfs at 0, so a fresh
# session (or a restart after a blank) must restore a sane level or it comes up
# black. Derived from the panel's own max, never hardcoded.
BL=/sys/class/leds/lcd-backlight/brightness
if [ -w "$BL" ] && [ "$(cat "$BL" 2>/dev/null || echo 0)" = "0" ]; then
    MX=$(cat /sys/class/leds/lcd-backlight/max_brightness 2>/dev/null || echo 0)
    [ "$MX" -gt 0 ] 2>/dev/null && echo $((MX / 2)) > "$BL" 2>/dev/null
fi

unset WAYLAND_DISPLAY

: > "$PHOC_LOG"
"$PHOC" -C /etc/phosh/phoc.ini >"$PHOC_LOG" 2>&1 &
PHOC_PID=$!
tail -n +1 -F "$PHOC_LOG" --pid "$PHOC_PID" 2>/dev/null &
TAIL_PID=$!

CLIENT_PID=""
# Clean teardown is mandatory: it releases the singleton HWC2 composer client.
cleanup() {
    [ -n "$CLIENT_PID" ] && kill -TERM "$CLIENT_PID" 2>/dev/null
    if kill -0 "$PHOC_PID" 2>/dev/null; then
        kill -TERM "$PHOC_PID" 2>/dev/null
        for _ in $(seq 1 80); do kill -0 "$PHOC_PID" 2>/dev/null || break; sleep 0.1; done
        kill -KILL "$PHOC_PID" 2>/dev/null   # last resort if it hung
    fi
    kill "$TAIL_PID" 2>/dev/null || true
}
on_term() { cleanup; exit 0; }
trap on_term TERM INT      # fires promptly: the client runs in the background
trap cleanup EXIT

# Wait for the output to actually come up (not just the wayland socket).
ready=0
for _ in $(seq 1 150); do   # up to ~30s
    if grep -qE "Output '.*' added" "$PHOC_LOG" 2>/dev/null; then ready=1; break; fi
    kill -0 "$PHOC_PID" 2>/dev/null || { echo "phosh-drm: phoc exited before output came up" >&2; exit 1; }
    sleep 0.2
done
[ "$ready" = 1 ] || { echo "phosh-drm: timed out waiting for phoc output" >&2; exit 1; }
sleep 2   # settle: let the blitter finish its first present cycle

export WAYLAND_DISPLAY=wayland-0
"$CLIENT" &
CLIENT_PID=$!

# Paint watchdog: the drmadapter present heartbeat file appears the moment the
# shell paints a real frame. If it never shows within the grace period the
# screen is black (cold-boot paint race) -> fail so systemd retries / falls back.
painted=0
for _ in $(seq 1 150); do   # ~30s grace
    [ -s "$HEARTBEAT" ] && { painted=1; break; }
    kill -0 "$CLIENT_PID" 2>/dev/null || break   # client gone; wait below handles it
    sleep 0.2
done
if [ "$painted" != 1 ]; then
    echo "phosh-drm: shell never painted within grace period — failing over" >&2
    exit 1   # EXIT trap tears down phoc + client cleanly
fi
echo "phosh-drm: shell painted (heartbeat $(cat "$HEARTBEAT" 2>/dev/null)) — healthy" >&2

wait "$CLIENT_PID"
