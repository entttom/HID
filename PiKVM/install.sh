#!/bin/bash
set -euo pipefail

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "Bitte als root auf dem PiKVM ausführen."
  exit 1
fi

SRC_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="/opt/hid-automation"
ENV_FILE="/etc/hid-automation.env"
SERVICE_FILE="/etc/systemd/system/hid-automation.service"
USC_FILE="/etc/kvmd/override.d/9980-hid-automation.yaml"
EXTRA_DIR="/usr/share/kvmd/extras/hid-automation"
HID_USER="hid-automation"

if [[ ! -S /run/kvmd/kvmd.sock ]]; then
  echo "Fehler: /run/kvmd/kvmd.sock wurde nicht gefunden. Läuft dieses Script auf PiKVM?"
  exit 1
fi

if ! command -v kvmd-pstrun >/dev/null 2>&1; then
  echo "Fehler: kvmd-pstrun fehlt. Bitte PiKVM aktualisieren bzw. eine aktuelle OS-Version verwenden."
  exit 1
fi

if ! command -v kvmd-override >/dev/null 2>&1; then
  echo "Fehler: kvmd-override wurde nicht gefunden. Bitte PiKVM aktualisieren."
  exit 1
fi

if ! command -v nginx >/dev/null 2>&1; then
  echo "Fehler: nginx wurde nicht gefunden."
  exit 1
fi

for required in \
  "$SRC_DIR/app.py" \
  "$SRC_DIR/server.py" \
  "$SRC_DIR/web/index.html" \
  "$SRC_DIR/web/integration.js" \
  "$SRC_DIR/integration/manifest.yaml" \
  "$SRC_DIR/integration/nginx.ctx-server.conf" \
  "$SRC_DIR/hid-automation.service"; do
  if [[ ! -f "$required" ]]; then
    echo "Fehler: Installationsdatei fehlt: $required"
    exit 1
  fi
done

# PiKVM root filesystem is read-only by default.
rw
trap 'ro >/dev/null 2>&1 || true' EXIT

if ! id "$HID_USER" >/dev/null 2>&1; then
  useradd --system --no-create-home --shell /usr/bin/nologin "$HID_USER"
fi

mkdir -p "$APP_DIR/web" /etc/kvmd/override.d "$EXTRA_DIR"
install -m 0755 "$SRC_DIR/app.py" "$APP_DIR/app.py"
install -m 0755 "$SRC_DIR/server.py" "$APP_DIR/server.py"
install -m 0644 "$SRC_DIR/web/index.html" "$APP_DIR/web/index.html"
install -m 0644 "$SRC_DIR/web/integration.js" "$APP_DIR/web/integration.js"
install -m 0644 "$SRC_DIR/hid-automation.service" "$SERVICE_FILE"
install -m 0644 "$SRC_DIR/integration/manifest.yaml" "$EXTRA_DIR/manifest.yaml"
install -m 0644 "$SRC_DIR/integration/nginx.ctx-server.conf" "$EXTRA_DIR/nginx.ctx-server.conf"

# The automation backend is deliberately loopback-only. PiKVM's nginx performs
# the normal PiKVM authentication before proxying /hid-automation/ to port 8081.
cat > "$ENV_FILE" <<EOF
HID_AUTOMATION_HOST=127.0.0.1
HID_AUTOMATION_PORT=8081
HID_AUTOMATION_USER=admin
HID_AUTOMATION_PASSWORD=
HID_AUTOMATION_KVMD_USER=$HID_USER
EOF
chmod 0600 "$ENV_FILE"

# Keep an atomic vendor-style override as a baseline.
cat > "$USC_FILE" <<EOF
kvmd:
    auth:
        usc:
            users: [$HID_USER]
EOF

# PiKVM loads override.yaml AFTER override.d. If override.yaml already contains
# kvmd/auth/usc/users, it replaces the list from our fragment. Read the effective
# list, preserve all existing users, add hid-automation, and write that final
# list through the official kvmd-override helper into override.yaml.
USC_USERS_JSON="$(kvmd -M | python -c '
import json, sys, yaml
user = sys.argv[1]
data = yaml.safe_load(sys.stdin) or {}
users = (((data.get("kvmd") or {}).get("auth") or {}).get("usc") or {}).get("users") or []
users = [str(x) for x in users]
if user not in users:
    users.append(user)
print(json.dumps(users, separators=(",", ":")))
' "$HID_USER")"

kvmd-override --set "kvmd/auth/usc/users=$USC_USERS_JSON"

# Validate KVMD config before restarting it.
kvmd -M >/dev/null

# The running nginx config contains a wildcard include for PiKVM extras, so a
# syntax test here also validates our newly installed location/sub_filter rules.
if ! nginx -t -c /run/kvmd/nginx.conf >/tmp/hid-automation-nginx-test.log 2>&1; then
  echo "Fehler: PiKVM-nginx akzeptiert die HID-Automation-Integration nicht:"
  cat /tmp/hid-automation-nginx-test.log
  rm -rf "$EXTRA_DIR"
  exit 1
fi
rm -f /tmp/hid-automation-nginx-test.log

systemctl daemon-reload
systemctl restart kvmd
sleep 2

if ! runuser -u "$HID_USER" -- curl --fail --silent --unix-socket /run/kvmd/kvmd.sock http://localhost/info >/dev/null; then
  echo
  echo "Fehler: Unix-Socket-Authentifizierung für $HID_USER funktioniert weiterhin nicht."
  echo "Effektive USC-Konfiguration:"
  kvmd -M | sed -n '/usc:/,+8p' || true
  echo
  echo "Bitte diese Ausgabe zusammen mit /etc/kvmd/override.yaml prüfen."
  exit 1
fi

kvmd-pstrun -- mkdir -p /var/lib/kvmd/pst/data/hid-automation
systemctl enable --now hid-automation.service
sleep 1

if ! curl --fail --silent http://127.0.0.1:8081/api/status >/dev/null; then
  echo "Fehler: HID-Automation-Backend antwortet nicht auf 127.0.0.1:8081."
  systemctl status hid-automation.service --no-pager || true
  exit 1
fi

systemctl restart kvmd-nginx

ro
trap - EXIT

PIKVM_IP="$(hostname -I | awk '{print $1}')"
echo
echo "PiKVM HID Automation wurde installiert."
echo "Öffne die normale PiKVM-Weboberfläche und gehe zu KVM."
echo "Dort erscheint neben Macro/Text der neue Menüpunkt: Automation"
echo
echo "Direkter Pfad über PiKVM: https://$PIKVM_IP/hid-automation/"
echo "Die normale PiKVM-Anmeldung schützt Weboberfläche und API."
echo "Port 8081 lauscht nur auf 127.0.0.1 und ist nicht im LAN erreichbar."
echo
echo "Status: systemctl status hid-automation"
echo "Logs:   journalctl -u hid-automation -f"
