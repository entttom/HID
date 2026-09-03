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
HID_USER="hid-automation"

if [[ ! -S /run/kvmd/kvmd.sock ]]; then
  echo "Fehler: /run/kvmd/kvmd.sock wurde nicht gefunden. Läuft dieses Script auf PiKVM?"
  exit 1
fi

if ! command -v kvmd-pstrun >/dev/null 2>&1; then
  echo "Fehler: kvmd-pstrun fehlt. Bitte PiKVM aktualisieren bzw. eine aktuelle OS-Version verwenden."
  exit 1
fi

PASSWORD="$(python - <<'PY'
import secrets
print(secrets.token_urlsafe(18))
PY
)"

# PiKVM root filesystem is read-only by default.
rw
trap 'ro >/dev/null 2>&1 || true' EXIT

if ! id "$HID_USER" >/dev/null 2>&1; then
  useradd --system --no-create-home --shell /usr/bin/nologin "$HID_USER"
fi

mkdir -p "$APP_DIR/web" /etc/kvmd/override.d
install -m 0755 "$SRC_DIR/app.py" "$APP_DIR/app.py"
install -m 0755 "$SRC_DIR/server.py" "$APP_DIR/server.py"
install -m 0644 "$SRC_DIR/web/index.html" "$APP_DIR/web/index.html"
install -m 0644 "$SRC_DIR/hid-automation.service" "$SERVICE_FILE"

cat > "$ENV_FILE" <<EOF
HID_AUTOMATION_HOST=0.0.0.0
HID_AUTOMATION_PORT=8081
HID_AUTOMATION_USER=admin
HID_AUTOMATION_PASSWORD=$PASSWORD
HID_AUTOMATION_KVMD_USER=$HID_USER
EOF
chmod 0600 "$ENV_FILE"

# Dedicated Unix Socket Credentials identity for local KVMD API access.
cat > "$USC_FILE" <<EOF
kvmd:
    auth:
        usc:
            users: [$HID_USER]
EOF

kvmd -M >/dev/null
systemctl daemon-reload
systemctl restart kvmd
sleep 2

if ! runuser -u "$HID_USER" -- curl --fail --silent --unix-socket /run/kvmd/kvmd.sock http://localhost/info >/dev/null; then
  echo
  echo "Fehler: Unix-Socket-Authentifizierung für $HID_USER funktioniert nicht."
  echo "Prüfe, ob kvmd/auth/usc/users bereits in /etc/kvmd/override.yaml überschrieben wird."
  echo "Füge dort $HID_USER zur bestehenden users-Liste hinzu und starte kvmd neu."
  exit 1
fi

kvmd-pstrun -- mkdir -p /var/lib/kvmd/pst/data/hid-automation
systemctl enable --now hid-automation.service

ro
trap - EXIT

echo
echo "PiKVM HID Automation wurde installiert."
echo "Webinterface: http://$(hostname -I | awk '{print $1}'):8081/"
echo "Benutzer: admin"
echo "Passwort:  $PASSWORD"
echo
echo "Das Passwort steht zusätzlich in $ENV_FILE (nur root lesbar)."
echo "Status: systemctl status hid-automation"
echo "Logs:   journalctl -u hid-automation -f"
