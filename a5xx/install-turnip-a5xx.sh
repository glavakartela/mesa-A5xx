#!/bin/sh
# install-turnip-a5xx.sh - install a locally built Turnip A509 Vulkan ICD.
#
# Requirements:
#   * SSH key access to user@host (this script never handles passwords)
#   * passwordless sudo on the device, or run the sudo commands manually
#   * a built libvulkan_freedreno.so
#
# Usage:
#   DRIVER=/path/to/libvulkan_freedreno.so ./install-turnip-a5xx.sh user@host
#   ./install-turnip-a5xx.sh user@host --rollback
#
# The installer backs up the distro Mesa files under
# $HOME/turnip-a5xx-backup-<timestamp> and installs:
#   /usr/lib/turnip-a5xx/libvulkan_freedreno.so
#   /usr/share/vulkan/icd.d/freedreno_icd.json
set -eu

HOST="${1:-${WHYRED_HOST:-}}"
[ -n "$HOST" ] || {
   echo "usage: DRIVER=... $0 user@host [--rollback]" >&2
   exit 2
}
ACTION="${2:-install}"
ACTION="${ACTION#--}"
SSH_OPTS="-o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new"
SUDO="${SUDO:-sudo -n}"

rssh() { ssh $SSH_OPTS "$@"; }
rscp() { scp $SSH_OPTS "$@"; }

if ! rssh "$HOST" "$SUDO true" >/dev/null 2>&1; then
   echo "error: passwordless sudo is required for $HOST." >&2
   echo "Configure it for the install user, or run the sudo commands manually." >&2
   exit 1
fi

RHOME=$(rssh "$HOST" 'printf %s "$HOME"')
[ -n "$RHOME" ] || { echo "cannot determine remote \$HOME" >&2; exit 1; }

if [ "$ACTION" = rollback ]; then
   BK=$(rssh "$HOST" "ls -dt '$RHOME'/turnip-a5xx-backup-* 2>/dev/null | head -1")
   [ -n "$BK" ] || { echo "no backup found under $RHOME" >&2; exit 1; }
   echo "restoring from $BK"
   rssh "$HOST" "$SUDO cp '$BK/freedreno_icd.json.mesa26' /usr/share/vulkan/icd.d/freedreno_icd.json"
   rssh "$HOST" "sed -i '/^KWIN_COMPOSE=/d;/^KWIN_DISABLE_VULKAN=/d' '$RHOME/.config/environment.d/99-a5xx-kwin.conf' 2>/dev/null || true"
   rssh "$HOST" "$SUDO sed -i '/^KWIN_COMPOSE=/d;/^KWIN_DISABLE_VULKAN=/d' /var/lib/plasmalogin/.config/environment.d/99-a5xx-kwin.conf 2>/dev/null || true"
   echo "rollback done; restart Plasma or reboot to return to the distro Mesa"
   exit 0
fi

DRIVER="${DRIVER:-}"
if [ -z "$DRIVER" ]; then
   for candidate in \
      build-a5xx/src/freedreno/vulkan/libvulkan_freedreno.so \
      src/freedreno/vulkan/libvulkan_freedreno.so; do
      [ -f "$candidate" ] && DRIVER="$candidate" && break
   done
fi
[ -n "$DRIVER" ] && [ -f "$DRIVER" ] || {
   echo "error: set DRIVER=/path/to/libvulkan_freedreno.so" >&2
   exit 1
}

TS=$(date +%Y%m%d-%H%M%S)
BACKUP="$RHOME/turnip-a5xx-backup-$TS"

echo "== installing $DRIVER on $HOST"
rssh "$HOST" "$SUDO mkdir -p /usr/lib/turnip-a5xx '$BACKUP'"
rssh "$HOST" "$SUDO cp -a /usr/share/vulkan/icd.d/freedreno_icd.json '$BACKUP/freedreno_icd.json.mesa26' 2>/dev/null || true"
rssh "$HOST" "$SUDO cp -a /usr/lib/libvulkan_freedreno.so '$BACKUP/libvulkan_freedreno.so.mesa26' 2>/dev/null || true"

rscp "$DRIVER" "$HOST:/tmp/libvulkan_freedreno.a5xx.so"
rssh "$HOST" "$SUDO cp /tmp/libvulkan_freedreno.a5xx.so /usr/lib/turnip-a5xx/libvulkan_freedreno.so"

cat > /tmp/freedreno_a5xx_icd.json <<'EOF'
{
  "file_format_version": "1.0.1",
  "ICD": {
    "api_version": "1.3.368",
    "library_path": "/usr/lib/turnip-a5xx/libvulkan_freedreno.so"
  }
}
EOF
rscp /tmp/freedreno_a5xx_icd.json "$HOST:/tmp/freedreno_a5xx_icd.json"
rssh "$HOST" "$SUDO cp /tmp/freedreno_a5xx_icd.json /usr/share/vulkan/icd.d/freedreno_icd.json"

echo "== disabling KWin Vulkan (keep the distro OpenGL compositor)"
rssh "$HOST" "set -e
   mkdir -p '$RHOME/.config/environment.d'
   printf '%s\n' 'KWIN_USE_BUFFER_AGE=0' 'KWIN_DISABLE_VULKAN=1' 'FD_MESA_DEBUG=sysmem' \
      'KWIN_DRM_NO_DIRECT_SCANOUT=1' 'KWIN_DRM_USE_MODIFIERS=0' \
      > '$RHOME/.config/environment.d/99-a5xx-kwin.conf'
   $SUDO mkdir -p /var/lib/plasmalogin/.config/environment.d
   $SUDO sh -c \"printf '%s\\n' 'KWIN_USE_BUFFER_AGE=0' 'KWIN_DISABLE_VULKAN=1' 'FD_MESA_DEBUG=sysmem' 'KWIN_DRM_NO_DIRECT_SCANOUT=1' 'KWIN_DRM_USE_MODIFIERS=0' > /var/lib/plasmalogin/.config/environment.d/99-a5xx-kwin.conf\"
   $SUDO chown -R plasmalogin:plasmalogin /var/lib/plasmalogin/.config/environment.d 2>/dev/null || true
   $SUDO sh -c \"printf '%s\\n' 'export KWIN_DRM_NO_DIRECT_SCANOUT=1' 'export KWIN_DRM_USE_MODIFIERS=0' > /etc/profile.d/99-kwin-a5xx.sh\"
   $SUDO sh -c \"printf '%s\\n' '# Adreno 5xx Mesa issue 8442 workaround: sysmem instead of noblit' 'export FD_MESA_DEBUG=sysmem' > /etc/profile.d/sdm660-adreno-quirks.sh\"
"

echo "== verifying"
rssh "$HOST" "env -u VK_ICD_FILENAMES -u VK_DRIVER_FILES vulkaninfo --summary 2>&1 | sed -n '/Devices:/,/^$/p' | grep -E 'deviceName|driverName|driverInfo' || true"
if ! rssh "$HOST" "env -u VK_ICD_FILENAMES -u VK_DRIVER_FILES vulkaninfo --summary 2>/dev/null | grep -q 'Turnip Adreno (TM) 509'"; then
   echo "error: Turnip not visible, restoring the previous ICD" >&2
   rssh "$HOST" "$SUDO cp '$BACKUP/freedreno_icd.json.mesa26' /usr/share/vulkan/icd.d/freedreno_icd.json"
   exit 1
fi

if [ -n "${TU5_TRI_TEST:-}" ] && [ -f "$TU5_TRI_TEST" ]; then
   echo "== smoke test"
   rssh "$HOST" "mkdir -p /tmp/tu5-test"
   rscp "$TU5_TRI_TEST" "$HOST:/tmp/tu5-test/"
   rssh "$HOST" "cd /tmp/tu5-test && env -u VK_ICD_FILENAMES -u VK_DRIVER_FILES TU5_GFX_NO_PRELOAD=1 timeout 30 ./$(basename "$TU5_TRI_TEST") | tail -2" || true
fi

echo "done. backup: $BACKUP"
echo "rollback: $0 $HOST --rollback"
