#!/bin/sh
# install-turnip-a5xx.sh — установка native Turnip A509 как системного Vulkan ICD.
# Usage:
#   ./install-turnip-a5xx.sh [user@host]
#   ./install-turnip-a5xx.sh [user@host] --texture   # WIP textures, не для daily
#   ./install-turnip-a5xx.sh [user@host] --ops       # CPU copy/clear + textures
#   ./install-turnip-a5xx.sh [user@host] --rollback
set -eu

HOST="${1:-user@192.0.2.10}"
ACTION="${2:-install}"
ACTION="${ACTION#--}"
BASE=/home/user/7.0-mainline-whyred/gpu-fix/mesa-a5xx
STABLE=/tmp/libvulkan_freedreno_stable.so
TEXTURE=/tmp/libvulkan_freedreno_tex.so
OPS=/tmp/libvulkan_freedreno_final2.so
# SSH key/agent expected
: # SSH agent expected

# helper removed
rssh() { setsid -w ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 -p 22 "$@"; }
rscp() { setsid -w scp -P 22 "$@"; }

if [ "$ACTION" = rollback ]; then
  BK=$(rssh "$HOST" 'ls -dt /home/user/turnip-a5xx-backup-* 2>/dev/null | head -1')
  [ -n "$BK" ] || { echo "no backup found"; exit 1; }
  echo "restore from $BK"
  rssh "$HOST" "set -e
    sudo -n cp '$BK/freedreno_icd.json.mesa26' /usr/share/vulkan/icd.d/freedreno_icd.json 2>/dev/null || \
      sudo -n rm -f /usr/share/vulkan/icd.d/freedreno_icd.json
    sed -i '/^KWIN_COMPOSE=/d;/^KWIN_DISABLE_VULKAN=/d' ~/.config/environment.d/99-a5xx-kwin.conf 2>/dev/null || true
    sudo -n sed -i '/^KWIN_COMPOSE=/d;/^KWIN_DISABLE_VULKAN=/d' /var/lib/plasmalogin/.config/environment.d/99-a5xx-kwin.conf 2>/dev/null || true
  "
  echo "rollback done; reboot/restart Plasma to return to Mesa 26"
  exit 0
fi

SRC="$STABLE"
[ "$ACTION" = texture ] && { SRC="$TEXTURE"; echo "WARNING: texture WIP"; }
[ "$ACTION" = ops ] && { SRC="$OPS"; echo "WARNING: ops WIP (textures + CPU copy/clear)"; }
[ -f "$SRC" ] || { echo "missing driver: $SRC"; exit 1; }

TS=$(date +%Y%m%d-%H%M%S)
BACKUP="/home/user/turnip-a5xx-backup-$TS"

echo "== ssh"; rssh "$HOST" 'uname -a; uptime' >/dev/null

echo "== install driver + backup"
rssh "$HOST" "sudo -n mkdir -p /usr/lib/turnip-a5xx '$BACKUP'"
rssh "$HOST" "set -e
  sudo -n cp -a /usr/share/vulkan/icd.d/freedreno_icd.json '$BACKUP/freedreno_icd.json.mesa26' 2>/dev/null || true
  sudo -n cp -a /usr/lib/libvulkan_freedreno.so '$BACKUP/libvulkan_freedreno.so.mesa26' 2>/dev/null || true
"
rscp "$SRC" "$HOST:/tmp/libvulkan_freedreno.a5xx.so"
rssh "$HOST" "sudo -n cp /tmp/libvulkan_freedreno.a5xx.so /usr/lib/turnip-a5xx/libvulkan_freedreno.so"

echo "== install ICD"
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
rssh "$HOST" "sudo -n cp /tmp/freedreno_a5xx_icd.json /usr/share/vulkan/icd.d/freedreno_icd.json"

echo "== disable KWin Vulkan (keep default OpenGL)"
rssh "$HOST" "set -e
  mkdir -p ~/.config/environment.d
  printf '%s\n' 'KWIN_USE_BUFFER_AGE=0' 'KWIN_DISABLE_VULKAN=1' 'FD_MESA_DEBUG=sysmem' 'KWIN_DRM_NO_DIRECT_SCANOUT=1' 'KWIN_DRM_USE_MODIFIERS=0' > ~/.config/environment.d/99-a5xx-kwin.conf
  sudo -n mkdir -p /var/lib/plasmalogin/.config/environment.d
  sudo -n sh -c \"printf '%s\\n' 'KWIN_USE_BUFFER_AGE=0' 'KWIN_DISABLE_VULKAN=1' 'FD_MESA_DEBUG=sysmem' 'KWIN_DRM_NO_DIRECT_SCANOUT=1' 'KWIN_DRM_USE_MODIFIERS=0' > /var/lib/plasmalogin/.config/environment.d/99-a5xx-kwin.conf\"
  sudo -n chown -R plasmalogin:plasmalogin /var/lib/plasmalogin/.config/environment.d 2>/dev/null || true
  sudo -n sh -c \"printf '%s\\n' 'export KWIN_DRM_NO_DIRECT_SCANOUT=1' 'export KWIN_DRM_USE_MODIFIERS=0' > /etc/profile.d/99-kwin-a5xx.sh\"
  sudo -n sh -c \"printf '%s\\n' '# Adreno 5xx Mesa issue 8442 workaround: sysmem instead of noblit' 'export FD_MESA_DEBUG=sysmem' > /etc/profile.d/sdm660-adreno-quirks.sh\"
"

echo "== verify"
rssh "$HOST" "env -u VK_ICD_FILENAMES -u VK_DRIVER_FILES vulkaninfo --summary 2>&1 | sed -n '/Devices:/,/^$/p' | grep -E 'deviceName|driverName|driverInfo' || true"
if ! rssh "$HOST" "env -u VK_ICD_FILENAMES -u VK_DRIVER_FILES vulkaninfo --summary 2>/dev/null | grep -q 'Turnip Adreno (TM) 509'"; then
  echo "!! driver not visible, rollback"
  rssh "$HOST" "sudo -n cp '$BACKUP/freedreno_icd.json.mesa26' /usr/share/vulkan/icd.d/freedreno_icd.json"
  exit 1
fi

echo "== smoke test"
rssh "$HOST" "mkdir -p /tmp/tu5-test"
rscp "$BASE/turnip-test/tu5_tri_test" "$HOST:/tmp/tu5-test/" 2>/dev/null || true
rssh "$HOST" "cd /tmp/tu5-test && env -u VK_ICD_FILENAMES -u VK_DRIVER_FILES TU5_GFX_NO_PRELOAD=1 timeout 30 ./tu5_tri_test | tail -2" || true

echo "done. backup: $BACKUP"
echo "rollback: $0 $HOST --rollback"
