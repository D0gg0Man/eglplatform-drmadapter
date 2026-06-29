#!/bin/bash
# Swap the FuriPhone FLX1 session from stock phosh (hwcomposer backend) to
# phosh 0.55 on the wlroots DRM backend (hybris/HWC2 shims).
#
# Reversible: ./swap-to-drm.sh revert  -> restores stock phosh.
# Run as root (uses sudo -A if not root).
set -euo pipefail

S="$(cd "$(dirname "$0")" && pwd)"
LIBEXEC=/usr/libexec
SYSD=/etc/systemd/system
UPHOLD_DIR="$SYSD/android-service@hwcomposer.service.d"
UPHOLD="$UPHOLD_DIR/20-phosh.conf"            # shadows the packaged drop-in
PRELOAD=/etc/ld.so.preload
LIBDRM=/usr/lib/aarch64-linux-gnu/libdrm-hybris.so

as_root() { if [ "$(id -u)" = 0 ]; then "$@"; else sudo -A "$@"; fi; }

revert() {
    echo ">> Reverting to stock phosh"
    as_root systemctl stop phosh-drm.service 2>/dev/null || true
    as_root rm -f "$UPHOLD"                    # un-shadow -> packaged Upholds=phosh.service
    as_root systemctl daemon-reload
    as_root systemctl start phosh.service
    echo ">> Stock phosh restored. (phosh-drm unit files left in place; disabled.)"
    exit 0
}

[ "${1:-}" = revert ] && revert

echo ">> Installing phosh-on-DRM session files"
as_root install -m 755 "$S/phosh-drm-session" "$LIBEXEC/phosh-drm-session"
as_root install -m 755 "$S/phosh-drm-client"  "$LIBEXEC/phosh-drm-client"
as_root install -m 644 "$S/phosh-drm.service" "$SYSD/phosh-drm.service"
# PAM stack: reuse phosh's.
as_root install -m 644 /etc/pam.d/phosh /etc/pam.d/phosh-drm

echo ">> Ensuring libdrm-hybris.so is globally preloaded"
if ! as_root grep -qxF "$LIBDRM" "$PRELOAD" 2>/dev/null; then
    as_root sh -c "echo '$LIBDRM' >> '$PRELOAD'"
fi

echo ">> Pointing the hwcomposer service at phosh-drm.service"
as_root install -d "$UPHOLD_DIR"
as_root install -m 644 "$S/android-uphold-phosh-drm.conf" "$UPHOLD"

echo ">> Switching now"
as_root systemctl daemon-reload
as_root systemctl stop phosh.service 2>/dev/null || true
as_root systemctl start phosh-drm.service

echo ">> Done. phosh-drm.service started; stock phosh is the OnFailure fallback."
echo "   journalctl -u phosh-drm -f    # to watch"
echo "   $0 revert                     # to go back to stock phosh"
