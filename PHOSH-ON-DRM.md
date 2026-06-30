# phosh on the wlroots DRM backend, via hybris/HWC2 shims (FuriPhone FLX1)

Run **phoc 0.55 / wlroots 0.20 on the wlroots *DRM* backend** (not the hwcomposer
backend) on a Mali/Android-HWC2 device, with **phosh 0.55** as the client —
GPU-accelerated, touch-working, stable/flicker-free — using only LD_PRELOAD/EGL
shims plus a tiny wlroots patch. phoc and phosh sources are **unpatched**.

This file is the reproduction recipe + the hard-won gotchas. Everything needed
is on GitHub (see Components); the device-local scratchpad is not required.

## Why this is hard
HWC2/Android has no KMS for the compositor (the Android composer owns the CRTC,
no DRM master). wlroots' DRM backend expects real atomic KMS + a GLES2 renderer
on a Mesa-style EGL. We fake KMS, bridge the Mali (libhybris) EGL into wlroots,
and scan out through the Android HWC2 path — all from outside phoc/wlroots.

## Components (all D0gg0Man unless noted)
| repo | branch | role |
|---|---|---|
| `libdrm-hybris` | `forky` | LD_PRELOAD: fake libseat + fake atomic KMS + synthetic page-flip clock + present wiring |
| `eglplatform-drmadapter` | `main` | hybris EGL platform `drmadapter`: HWC2 init + the GLES2 **blitter** that scans wlroots' output out via an HWCNativeWindow |
| `libgbm-hybris` | `main` | `hybris_gbm.so`: gbm backend allocating gralloc bos for wlroots' swapchain |
| `libhybris` | `master` | `eglplatformcommon`: dmabuf→native EGLImage bridge + blitter helpers + `eglBindWaylandDisplayWL`→android_wlegl |
| `wlroots` | `furios-0.20` | upstream v0.20.0 + 2 tiny patches (layer-shell leniency, EGL wl bind) |
| phoc | `v0.55.0` (gitlab.gnome.org/World/Phosh/phoc) | UNPATCHED, built `-Dembed-wlroots=disabled` against our wlroots |
| phosh | `v0.55.0` (gitlab.gnome.org/World/Phosh/phosh) | UNPATCHED client |

phoc and phosh must be the SAME release (0.55 ↔ 0.55); a mismatch trips
wlroots-0.20 layer-shell strictness (see gotchas).

## Build
- **wlroots 0.20** (D0gg0Man/wlroots `furios-0.20`): `meson setup build -Dxwayland=enabled` (needs xcb-* dev), `ninja -C build install` → `/usr/local`. The 2 FuriOS patches are already on that branch.
- **phoc 0.55**: `meson setup build -Dembed-wlroots=disabled` (needs gnome-desktop-3-dev, wlr/xwayland.h), `ninja -C build` → run `build/src/phoc`.
- **phosh 0.55**: build deps from its `debian/control`; `meson setup build --prefix=/usr -Dgtk_doc=false -Dtests=false -Dman=false`; `ninja -C build` → `build/src/phosh`. Run with `GSETTINGS_SCHEMA_DIR=build/data` (the installed 0.53 schemas lack `mobi.phosh.shell.plugins` → fatal otherwise).
- **shims**: `make` in each repo; install `libdrm-hybris.so` (ld.so.preload), `hybris_gbm.so` → `…/gbm/`, `eglplatform_drmadapter.so` → `…/libhybris/`, `libhybris-eglplatformcommon.so.1.0.0`.

## Launch — compositor (phoc on DRM)
```sh
export XDG_RUNTIME_DIR=/run/user/<uid>
export XDG_SEAT=seat0 XDG_VTNR=7
export XDG_DATA_DIRS=/usr/local/share:/usr/share
export LD_LIBRARY_PATH=/usr/local/lib/aarch64-linux-gnu:<phoc>/build/src
export WLR_DRM_DEVICES=/dev/dri/card0 WLR_RENDERER=gles2     # NB: do NOT set WLR_BACKENDS (auto = DRM+libinput)
export GBM_BACKEND=hybris GBM_BACKENDS_PATH=/usr/lib/aarch64-linux-gnu/gbm HYBRIS_EGLPLATFORM=drmadapter
export HYBRIS_WLROOTS=1                                       # gates the wlroots-specific shim paths
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_libhybris.json   # libhybris-only GLVND vendor
export LD_PRELOAD=/usr/lib/aarch64-linux-gnu/libdrm-hybris.so:/usr/local/lib/wlegl_server.so
exec <phoc>/build/src/phoc -v -C /etc/phosh/phoc.ini
```

## Launch — client (phosh)
Plain wayland client; must NOT inherit the compositor's HYBRIS_WLROOTS/GBM_*:
```sh
export WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/<uid>
export EGL_PLATFORM=wayland HYBRIS_EGLPLATFORM=drmadapter
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_libhybris.json
export XDG_CURRENT_DESKTOP=Phosh:GNOME XDG_SESSION_TYPE=wayland GNOME_SHELL_SESSION_MODE=phosh
unset HYBRIS_WLROOTS GBM_BACKEND GBM_BACKENDS_PATH WLR_DRM_DEVICES WLR_RENDERER WLR_BACKENDS
export GSETTINGS_SCHEMA_DIR=<phosh>/build/data LD_LIBRARY_PATH=<phosh>/build/src
# MUST disable idle blanking (see gotchas):
dbus-run-session -- sh -c '
  gsettings set org.gnome.desktop.session idle-delay 0
  gsettings set org.gnome.settings-daemon.plugins.power sleep-inactive-battery-type nothing
  gsettings set org.gnome.settings-daemon.plugins.power sleep-inactive-ac-type nothing
  exec <phosh>/build/src/phosh'
```

## The hard-won gotchas (in causal order)
1. **GLES2 renderer init**: force libhybris-only GLVND vendor; advertise `EGL_EXT_platform_base` + the Mali display exts (`EGL_KHR_no_config_context`, `surfaceless_context`, `image_dma_buf_import`) only under HYBRIS_WLROOTS. (eglplatformcommon / eglglvnd.)
2. **Atomic KMS faked**: `drmModeAtomicCommit` returns 0; capture the real **FB_ID** and **CRTC_ID** from `drmModeAtomicAddProperty` — present the buffer the commit actually scans out, and tag the synthetic flip with the CRTC wlroots matches on (`handle_page_flip`/`drm_page_flip_pop`) or it drops the event and `frame_pending` wedges.
3. **Synthetic page-flip clock**: wlroots commits non-blocking and waits for a flip event. Synthesize `DRM_EVENT_FLIP_COMPLETE` and deliver it through BOTH `poll/ppoll` AND `epoll` — phoc (GLib loop) nests the wayland epoll fd inside ppoll, so wake the outer ppoll on the epoll fd and inject the DRM-fd readiness in `epoll_wait`. Deliver IMMEDIATELY (vsync-pacing the synth flip → black). Real CRTC id required.
4. **dmabuf → native EGLImage**: Mali imports gralloc ANativeWindowBuffers, not generic dmabufs. `eglplatformcommon` intercepts `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)` and re-imports as `EGL_NATIVE_BUFFER_ANDROID` (recovers the gralloc handle from the dmabuf fd via libgbm-hybris's registration). This is how wlroots' GLES2 renderer gets its output FBO.
5. **Scan-out = blitter, NOT set_client_target**: HWC2 only scans out buffers from its own `HWCNativeWindow` queue (mutter's `eglSwapBuffers`→`present_cb`). Handing HWC2 a raw gbm buffer via `set_client_target` returns 0 but shows **nothing**. So `drmadapter_present_gralloc` runs a tiny GLES2 blit: import the committed buffer as a texture, draw it into an HWCNativeWindow surface, `eglSwapBuffers`→`present_cb`→HWC2. CPU gralloc sampling is a misleading "it works" signal — those HW_RENDER buffers aren't reliably CPU-mappable.
6. **Blit requirements (all mandatory):**
   - import a **FRESH** RemoteWindowBuffer (`eglplatformcommon_wlr_make_anwb`) — Mali rejects a 2nd `eglCreateImageKHR` on wlroots' already-imported ANWB (EGL_BAD_PARAMETER);
   - create the EGLImage on **wlroots' own EGLDisplay** handle (`eglplatformcommon_wlr_display`) — a fresh `eglGetDisplay` handle → EGL_BAD_PARAMETER;
   - **re-bind the EGLImage to the texture EVERY frame** (`glEGLImageTargetTexture2DOES`) — the GL texture cache holds a stale snapshot otherwise → **that is the flicker**.
   - Do NOT glFinish on wlroots' context to sync (corrupts its rendering → black). Do NOT disable `DRM_CAP_SYNCOBJ_TIMELINE` (→ black).
7. **GPU resolve sync (load-bearing!)**: before presenting, `copy_to_dumb` does a **full-frame read** of the committed gralloc buffer. Touching every pixel forces Mali to resolve its tiled render into the buffer before the blit samples it. It LOOKS like a wasteful 10 MB/frame memcpy but removing/shrinking it → black. Do not "optimize" it.
8. **GPU client buffers (android_wlegl)**: the libhybris wayland-EGL client (phosh, GTK4) hard-requires the `android_wlegl` global, which only appears when the compositor calls `eglBindWaylandDisplayWL`. wlroots 0.20 dropped that (advertises its own wl_drm + linux-dmabuf), so without the wlroots patch phosh falls back to wl_shm software rendering. Patch: call `eglBindWaylandDisplayWL` from `wlr_renderer_init_wl_shm` (the path phoc uses).
9. **layer-shell leniency (wlroots patch)**: phosh's draggable "phosh home" bar commits height 0 while only bottom-anchored before its drag surface sizes it; wlroots-0.20 protocol-errors and aborts phosh. Patch `wlr_layer_shell_v1.c` to warn instead.
10. **Idle blanking must be off**: phosh DPMS-blanks the lockscreen after ~10s and the screen can't wake — phoc drops touch to a disabled output, so phosh never gets the wake input (DPMS-on through faked KMS is the dead end). Disable idle-delay/sleep in the session gsettings.

## Productionized session (systemd)
`session/` wires the stack as a real boot session on FuriOS, mirroring the
gnome-mali installer pattern. Run `session/swap-to-drm.sh` to install + switch
(`swap-to-drm.sh revert` to go back to stock phosh).
- `phosh-drm-session` (→ `/usr/libexec`): launches phoc on DRM, waits for the
  output to come up, **then** launches the client; owns clean teardown.
- `phosh-drm-client` (→ `/usr/libexec`): scrubs the compositor-only env, runs
  phosh under `dbus-run-session` with idle-blanking off.
- `phosh-drm.service`: `OnFailure=phosh.service` (stock phosh is the fallback).
- `android-uphold-phosh-drm.conf`: shadows the packaged drop-in so the
  hwcomposer service upholds phosh-drm on boot.

Two service-specific gotchas (cost a lot of black screens):
11. **Seatless session, mandatory.** phoc drives the panel through the Android
    HWC2 composer (which owns DRM master on card0) via the faked-libseat
    blitter — NOT through logind. If pam_systemd registers a real seat0/VT7
    graphical session (via `TTYPath=/dev/tty7` or `XDG_SEAT`/`XDG_VTNR`), logind
    does VT/session-device management on card0 that fights the composer → black.
    So the unit is deliberately seatless (no TTYPath, no XDG_SEAT/XDG_VTNR); the
    logind session stays `Class=user Seat=none`, like the manual launch.
12. **Composer client is a singleton — phoc MUST exit cleanly.** A SIGKILL'd or
    orphaned phoc leaks the HWC2 composer client; the next phoc then aborts with
    `failed to create composer client` and the HAL is wedged (recover with
    `setprop ctl.start vendor.hwcomposer-2-3`, or reboot). phoc DOES release the
    client on a clean SIGTERM, so the unit uses **`KillMode=control-group`**:
    systemd SIGTERMs the whole cgroup, phoc gets the signal directly and shuts
    down cleanly. Do NOT use `KillMode=mixed` — it signals only the launcher,
    which is blocked waiting on the client, so phoc never gets SIGTERM, hits the
    stop timeout, escapes the cgroup on the final SIGKILL, and leaks the client.
    (The launcher also runs the client in the background + `wait` so its own
    SIGTERM trap stays responsive for the phosh-exits-on-its-own path.)
    NB: while `android-service@hwcomposer` `Upholds=phosh-drm.service`, systemd
    cancels a manual `stop`/`restart` of phosh-drm (the uphold re-queues a
    start) — fine for boot/crash-restart, but `swap-to-drm.sh revert` removes
    the uphold first so the stop sticks.
13. **Blitter-init race.** Launching phosh before phoc's output/blitter is fully
    up leaves a persistent black screen (the client's first buffers race the
    blitter's EGL setup). The launcher waits for `Output '…' added` + a settle
    before starting the client.

## Status / open items
- Works **warm**: phoc-on-DRM + GPU-accelerated phosh, flicker-free,
  touch/gestures, started from a running system (the systemd session was
  confirmed flicker-free + touch when switched into from stock phosh).
- **UNSOLVED — cold boot renders black.** After a fresh reboot, phosh-drm comes
  up but phosh's content never paints: phoc renders fine (flip clock healthy,
  `present_hwc2 rc=0`, ~1500 frames) but every composited buffer is black
  (`center=000000`), phoc never logs `Stacked surface`, and phosh logs
  `configures pending` — i.e. phosh creates its layer surfaces but never draws
  into them. phosh reaches "ready" (main loop runs) yet its surfaces stay black.
  Strongly correlated on a fresh boot with a session-service storm in the
  throwaway `dbus-run-session`: `org.freedesktop.secrets` (gnome-keyring) times
  out (120s), `org.gnome.Calls GetManagedObjects` times out (~27s). The graphics
  shim is NOT at fault (the present path provably works). Once a fresh boot is
  in this state it stays black **even warm**, so it is not purely uptime/timing.
  Mitigations added but NOT yet shown to fix it (couldn't validate — black on
  the test boot): (a) `phosh-drm-client` starts `gnome-keyring-daemon` in the
  session bus so secrets resolves; (b) `phosh-drm-session` has a render watchdog
  — if phosh never composites within ~30s it fails the unit so
  `OnFailure=phosh.service` falls back to stock phosh (and `Restart=no` so the
  failover doesn't flap). The clean fix is probably to run phosh inside a real
  `gnome-session --session=phosh` (like stock `phosh-session`, which is
  cold-boot-safe), but that is blocked here: our phoc 0.55 build's `-E` does not
  spawn its child the way stock phoc 0.44 does, and `gnome-session` won't start
  cleanly standalone against the shared systemd --user session state.
- Open: DPMS screen-wake through faked KMS.
