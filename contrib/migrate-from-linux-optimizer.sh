#!/usr/bin/env bash
# One-time migration from the former name "linux-optimizer" to "ml-optimizer".
#
# What it does (nothing is deleted):
#   1. optionally rolls back and stops the OLD daemon (stopping it restores every value it changed)
#   2. MOVES the state directory (rollback journal, telemetry database, ML models, policy state)
#   3. COPIES the configuration file
#   4. RENAMES the admin group
# It refuses to touch an existing, non-empty /var/lib/ml-optimizer.
# Old binaries, the old unit file and /etc/linux-optimizer are left in place for you to remove.
set -euo pipefail

OLD=linux-optimizer
NEW=ml-optimizer

[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo $0)" >&2; exit 1; }

if systemctl is-active --quiet "${OLD}d" 2>/dev/null; then
  echo "The old daemon (${OLD}d) is running. It must be stopped first; stopping it rolls back its changes."
  read -r -p "Roll back and stop ${OLD}d now? [yes/NO] " ans
  [ "$ans" = "yes" ] || { echo "aborted, nothing changed"; exit 1; }
  if command -v "$OLD" >/dev/null 2>&1; then
    "$OLD" reset || echo "warning: '$OLD reset' failed; stopping the daemon will still attempt a rollback" >&2
  fi
  systemctl disable --now "${OLD}d"
fi

if [ -d "/var/lib/$OLD" ]; then
  if [ -e "/var/lib/$NEW" ] && [ -n "$(ls -A "/var/lib/$NEW" 2>/dev/null)" ]; then
    echo "/var/lib/$NEW already exists and is not empty; refusing to merge. Resolve it manually." >&2
    exit 1
  fi
  rmdir "/var/lib/$NEW" 2>/dev/null || true
  mv -v "/var/lib/$OLD" "/var/lib/$NEW"
else
  echo "no /var/lib/$OLD found, skipping state migration"
fi

if [ -f "/etc/$OLD/$OLD.conf" ] && [ ! -e "/etc/$NEW/$NEW.conf" ]; then
  install -d -m 0755 "/etc/$NEW"
  cp -v "/etc/$OLD/$OLD.conf" "/etc/$NEW/$NEW.conf"
fi

if getent group "$OLD" >/dev/null && ! getent group "$NEW" >/dev/null; then
  groupmod -n "$NEW" "$OLD"
  echo "renamed group $OLD -> $NEW (members keep access; they must log in again)"
fi

cat <<MSG

Done. Next steps:
  - install the new build:   sudo cmake --install build
  - start it:                sudo systemctl enable --now ${NEW}d
  - update scripts that use \$LINUX_OPTIMIZER_SOCKET to \$ML_OPTIMIZER_SOCKET
  - once satisfied, remove leftovers yourself: old binaries, ${OLD}d.service, /etc/$OLD
MSG
