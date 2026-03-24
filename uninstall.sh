#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

PKG_NAME="hackberrypi-max17048"
PKG_VER="$(tr -d ' \t\r\n' < "${SCRIPT_DIR}/VERSION")"
DT_NAME="hackberrypicm5"

CONFIG_TXT="/boot/firmware/config.txt"
OVERLAY_DIR="/boot/firmware/overlays"

ts() { date '+%Y-%m-%dT%H:%M:%S%z'; }

log()  { echo "[$(ts)] $*"; }
warn() { echo "[$(ts)] WARNING: $*" >&2; }
err()  { echo "[$(ts)] ERROR: $*" >&2; }
die()  { err "$*"; exit 1; }

need_root() {
  [[ "${EUID}" -eq 0 ]] || die "Please run as root (sudo ./uninstall.sh)"
}

exec_cmd() {
  local cmd=("$@")

  printf '[%s] [*] ' "$(ts)"
  printf '%q ' "${cmd[@]}"
  printf '\n'

  # Allow us to capture status even under `set -e`
  set +e
  "${cmd[@]}"
  local status=$?
  set -e

  if [[ $status -eq 0 ]]; then
    log "[✓] Success"
  else
    err "[✗] Failed (exit code ${status})"
  fi

  return $status
}

must_exec() {
  if ! exec_cmd "$@"; then
    err "Aborting due to previous error"
    exit 1
  fi
}

section() {
  echo
  log "=== $* ==="
}

remove_overlay() {
  section "Remove device-tree overlay"

  if [[ -f "${CONFIG_TXT}" ]]; then
    exec_cmd sed -i "\|^dtoverlay=${DT_NAME}$|d" "${CONFIG_TXT}" || true
  else
    warn "Missing ${CONFIG_TXT} (skipping dtoverlay removal)"
  fi

  exec_cmd rm -f "${OVERLAY_DIR}/${DT_NAME}.dtbo" || true

  log "Overlay removed"
}

remove_dkms_all_versions() {
  section "Remove DKMS module (all versions)"

  if dkms status -m "${PKG_NAME}" >/dev/null 2>&1; then
    must_exec dkms remove -m "${PKG_NAME}" --all
  else
    log "No DKMS entry found for ${PKG_NAME}"
  fi
}

remove_sources_all_versions() {
  section "Remove DKMS sources (/usr/src)"

  # Remove any versioned source trees for this module.
  # Use a glob but keep it safe if nothing matches.
  shopt -s nullglob
  local paths=(/usr/src/"${PKG_NAME}"-*)
  shopt -u nullglob

  if [[ ${#paths[@]} -eq 0 ]]; then
    log "No /usr/src/${PKG_NAME}-* trees found (skipping)"
    return 0
  fi

  for p in "${paths[@]}"; do
    exec_cmd rm -rf "${p}" || true
  done
}

refresh_module_deps() {
  section "Refresh module dependency map (depmod)"

  if command -v depmod >/dev/null 2>&1; then
    exec_cmd depmod -a || true
  else
    warn "depmod not found (skipping)"
  fi
}

print_status() {
  section "Status"

  log "DKMS status (matching package name):"
  dkms status 2>/dev/null | grep -F "${PKG_NAME}" | while IFS= read -r line; do
    log "  ${line}"
  done || true

  log "Overlay present: $(test -f "${OVERLAY_DIR}/${DT_NAME}.dtbo" && echo yes || echo no)"
  log "Overlay enabled: $(test -f "${CONFIG_TXT}" && grep -qx "dtoverlay=${DT_NAME}" "${CONFIG_TXT}" && echo yes || echo no)"
}

main() {
  need_root

  remove_overlay
  remove_dkms_all_versions
  remove_sources_all_versions
  refresh_module_deps
  print_status

  echo
  log "[✓] Uninstall complete. Reboot recommended: sudo reboot"
}

main "$@"

