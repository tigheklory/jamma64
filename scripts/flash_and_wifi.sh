#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/flash_and_wifi.sh [options]

Options:
  --uf2 PATH           UF2 file to flash (default: build/jamma64.uf2)
  --ssid NAME          Wi-Fi SSID (or set WIFI_SSID env var)
  --password PASS      Wi-Fi password (or set WIFI_PASSWORD env var)
  --bootsel-drive LTR  Windows BOOTSEL drive letter (required), e.g. d
  --runtime-drive LTR  Windows runtime drive letter (required), e.g. h
  --timeout SEC        Wait timeout per mount stage (default: 120)
  --no-pauses          Disable Enter-key pauses
  -h, --help           Show this help

Examples:
  scripts/flash_and_wifi.sh --bootsel-drive d --runtime-drive h --ssid "MyWifi" --password "secret123"
  WIFI_SSID=MyWifi WIFI_PASSWORD=secret123 scripts/flash_and_wifi.sh
EOF
}

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UF2_PATH="$PROJECT_DIR/build/jamma64.uf2"
WIFI_SSID="${WIFI_SSID:-}"
WIFI_PASSWORD="${WIFI_PASSWORD:-}"
TIMEOUT_SEC=120
USE_PAUSES=1
BOOTSEL_DRIVE="${BOOTSEL_DRIVE:-}"
RUNTIME_DRIVE="${RUNTIME_DRIVE:-}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --uf2)
      UF2_PATH="$2"
      shift 2
      ;;
    --ssid)
      WIFI_SSID="$2"
      shift 2
      ;;
    --password)
      WIFI_PASSWORD="$2"
      shift 2
      ;;
    --bootsel-drive)
      BOOTSEL_DRIVE="$2"
      shift 2
      ;;
    --runtime-drive)
      RUNTIME_DRIVE="$2"
      shift 2
      ;;
    --timeout)
      TIMEOUT_SEC="$2"
      shift 2
      ;;
    --no-pauses)
      USE_PAUSES=0
      shift 1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ ! -f "$UF2_PATH" ]]; then
  echo "UF2 not found: $UF2_PATH" >&2
  exit 1
fi

if [[ -z "$WIFI_SSID" ]]; then
  read -r -p "Wi-Fi SSID: " WIFI_SSID
fi
if [[ -z "$WIFI_PASSWORD" ]]; then
  read -r -s -p "Wi-Fi Password: " WIFI_PASSWORD
  echo
fi

if [[ -z "$WIFI_SSID" ]]; then
  echo "SSID cannot be empty." >&2
  exit 1
fi
if [[ -z "$BOOTSEL_DRIVE" || -z "$RUNTIME_DRIVE" ]]; then
  echo "--bootsel-drive and --runtime-drive are required." >&2
  usage
  exit 1
fi

normalize_drive_letter() {
  local d="$1"
  d="${d%:}"
  d="${d#/mnt/}"
  d="$(echo "$d" | tr '[:upper:]' '[:lower:]')"
  echo "$d"
}

pause_if_enabled() {
  local prompt="$1"
  if (( USE_PAUSES == 0 )); then
    return 0
  fi
  read -r -p "$prompt Press Enter to continue..."
}

is_wsl() {
  grep -qiE "(microsoft|wsl)" /proc/version 2>/dev/null
}

try_wsl_drvfs_mounts() {
  is_wsl || return 0
  local l=""
  for l in {d..z}; do
    local mount_dir="/mnt/$l"
    local drive="${l^^}:"
    [[ -d "$mount_dir" ]] || mkdir -p "$mount_dir" 2>/dev/null || true
    if command -v mountpoint >/dev/null 2>&1 && mountpoint -q "$mount_dir"; then
      continue
    fi
    mount -t drvfs "$drive" "$mount_dir" 2>/dev/null || true
  done
}

ensure_drive_mounted() {
  local letter="$1"
  local d
  d="$(normalize_drive_letter "$letter")"
  [[ -n "$d" ]] || return 1
  local mount_dir="/mnt/$d"
  local drive="${d^^}:"

  [[ -d "$mount_dir" ]] || mkdir -p "$mount_dir" 2>/dev/null || true
  if command -v mountpoint >/dev/null 2>&1 && ! mountpoint -q "$mount_dir"; then
    mount -t drvfs "$drive" "$mount_dir" 2>/dev/null || true
  fi
  [[ -d "$mount_dir" ]] || return 1
  echo "$mount_dir"
  return 0
}

find_file_ci() {
  local dir="$1"
  local name="$2"
  find "$dir" -maxdepth 1 -type f -iname "$name" -print -quit 2>/dev/null || true
}

wait_for_bootsel_mount() {
  local mount_dir="$1"
  local timeout="$2"
  local elapsed=0

  while (( elapsed < timeout )); do
    try_wsl_drvfs_mounts
    if [[ -n "$(find_file_ci "$mount_dir" "INFO_UF2.TXT")" || -n "$(find_file_ci "$mount_dir" "INDEX.HTM")" ]]; then
      echo "$mount_dir"
      return 0
    fi
    sleep 1
    (( elapsed += 1 ))
  done

  return 1
}

wait_for_runtime_mount() {
  local mount_dir="$1"
  local timeout="$2"
  local elapsed=0

  while (( elapsed < timeout )); do
    try_wsl_drvfs_mounts
    if [[ -n "$(find_file_ci "$mount_dir" "WIFI.TXT")" ]]; then
      echo "$mount_dir"
      return 0
    fi
    sleep 1
    (( elapsed += 1 ))
  done

  return 1
}

BOOTSEL_MOUNT="$(ensure_drive_mounted "$BOOTSEL_DRIVE")" || {
  echo "Could not access BOOTSEL drive path for '$BOOTSEL_DRIVE'." >&2
  exit 1
}
RUNTIME_MOUNT="$(ensure_drive_mounted "$RUNTIME_DRIVE")" || {
  echo "Could not access runtime drive path for '$RUNTIME_DRIVE'." >&2
  exit 1
}

pause_if_enabled "Put the Pico in BOOTSEL mode now. "
echo "Waiting for Pico BOOTSEL drive (INFO_UF2.TXT / INDEX.HTM)..."
wait_for_bootsel_mount "$BOOTSEL_MOUNT" "$TIMEOUT_SEC" >/dev/null || {
  echo "Timed out waiting for BOOTSEL mount." >&2
  echo "Checked: $BOOTSEL_MOUNT" >&2
  exit 1
}
echo "Found: $BOOTSEL_MOUNT"

echo "Copying UF2: $UF2_PATH"
cp "$UF2_PATH" "$BOOTSEL_MOUNT/"
sync
echo "UF2 copied."

pause_if_enabled "Wait for reboot and JAMMA64 runtime drive mount. "
echo "Waiting for runtime drive (WIFI.TXT)..."
wait_for_runtime_mount "$RUNTIME_MOUNT" "$TIMEOUT_SEC" >/dev/null || {
  echo "Timed out waiting for runtime drive mount." >&2
  echo "Checked: $RUNTIME_MOUNT" >&2
  exit 1
}
echo "Found: $RUNTIME_MOUNT"

WIFI_FILE="$(find_file_ci "$RUNTIME_MOUNT" "WIFI.TXT")"
if [[ -z "$WIFI_FILE" ]]; then
  WIFI_FILE="$RUNTIME_MOUNT/WIFI.TXT"
fi
cat > "$WIFI_FILE" <<EOF
# JAMMA64 Wi-Fi config
# Auto-written by flash_and_wifi.sh
SSID=$WIFI_SSID
PASSWORD=$WIFI_PASSWORD
# Optional: REBOOT=1
EOF
sync

echo "Wrote Wi-Fi config to: $WIFI_FILE"
echo "Done."
