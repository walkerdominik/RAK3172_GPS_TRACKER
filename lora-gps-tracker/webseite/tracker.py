#!/usr/bin/env python3
"""
LoRa GPS Tracker – lokales Dashboard für The Things Network (TTN v3)

- Empfängt Uplinks live per MQTT von TTN und speichert sie in einer SQLite-Datei
- Zeigt Karte, Track und alle Payload-Felder im Browser (http://localhost:8080)
- Sendet Downlinks (Hex oder JSON) an das Gerät
- Holt beim Start optional verpasste Uplinks aus der TTN Storage Integration

Start:  python tracker.py            (normal)
        python tracker.py --no-mqtt  (nur gespeicherte Daten ansehen, offline)
"""
import base64
import csv
import io
import json
import sqlite3
import sys
import threading
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

BASE = Path(__file__).resolve().parent
CFG_PATH = BASE / "config.json"
DB_PATH = BASE / "tracker.db"
INDEX_PATH = BASE / "index.html"

STATUS = {"mqtt": "aus", "last_error": None, "storage_sync": None}


# --------------------------------------------------------------------------- Konfiguration
def load_config():
    if not CFG_PATH.exists():
        sys.exit("config.json fehlt – kopiere config.example.json nach config.json und trage deine Daten ein.")
    cfg = json.loads(CFG_PATH.read_text(encoding="utf-8"))
    cfg.setdefault("region", "eu1")
    cfg.setdefault("tenant", "ttn")
    cfg.setdefault("http_port", 8080)
    cfg.setdefault("presets", [])
    cfg.setdefault("storage_sync_hours", 24)
    cfg.setdefault("own_gateway", "mein-gateway")   # eigenes Gateway fuer rssi_eigen/snr_eigen im CSV
    if not cfg.get("application_id"):
        sys.exit("Bitte application_id in config.json eintragen.")
    return cfg


# --------------------------------------------------------------------------- Datenbank
class Store:
    def __init__(self, path):
        self.db = sqlite3.connect(str(path), check_same_thread=False)
        self.lock = threading.Lock()
        with self.lock:
            self.db.executescript(
                """
                CREATE TABLE IF NOT EXISTS uplinks(
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    received_at TEXT, device_id TEXT, raw TEXT);
                CREATE UNIQUE INDEX IF NOT EXISTS ux_up ON uplinks(device_id, received_at);
                CREATE TABLE IF NOT EXISTS events(
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    at TEXT, device_id TEXT, kind TEXT, raw TEXT);
                """
            )
            self.db.commit()

    def add_uplink(self, msg):
        if "data" in msg and "uplink_message" in msg.get("data", {}):  # Console-Export-Format
            msg = msg["data"]
        if "uplink_message" not in msg:
            return False
        dev = msg.get("end_device_ids", {}).get("device_id", "?")
        at = msg.get("received_at") or msg["uplink_message"].get("received_at") or now_iso()
        with self.lock:
            cur = self.db.execute(
                "INSERT OR IGNORE INTO uplinks(received_at, device_id, raw) VALUES (?,?,?)",
                (at, dev, json.dumps(msg)),
            )
            self.db.commit()
            return cur.rowcount > 0

    def add_event(self, device_id, kind, raw):
        with self.lock:
            self.db.execute(
                "INSERT INTO events(at, device_id, kind, raw) VALUES (?,?,?,?)",
                (now_iso(), device_id, kind, json.dumps(raw)),
            )
            self.db.commit()

    def uplinks(self, after=0, limit=5000):
        with self.lock:
            rows = self.db.execute(
                "SELECT id, raw FROM uplinks WHERE id > ? ORDER BY received_at ASC LIMIT ?",
                (after, limit),
            ).fetchall()
        return [summarize(i, json.loads(r)) for i, r in rows]

    def events(self, limit=50):
        with self.lock:
            rows = self.db.execute(
                "SELECT id, at, device_id, kind, raw FROM events ORDER BY id DESC LIMIT ?", (limit,)
            ).fetchall()
        return [{"id": i, "at": a, "device_id": d, "kind": k, "raw": json.loads(r)} for i, a, d, k, r in rows]

    def devices(self):
        with self.lock:
            return [r[0] for r in self.db.execute("SELECT DISTINCT device_id FROM uplinks ORDER BY device_id")]


def now_iso():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def summarize(row_id, msg):
    """Bereitet einen Uplink fürs Frontend auf."""
    up = msg.get("uplink_message", {})
    dp = up.get("decoded_payload") or {}
    rx = up.get("rx_metadata") or []
    best = max(rx, key=lambda r: r.get("rssi", -999)) if rx else {}
    lora = up.get("settings", {}).get("data_rate", {}).get("lora", {})

    lat, lon = dp.get("latitude"), dp.get("longitude")
    if lat is None or lon is None:
        loc = (up.get("locations") or {}).get("frm-payload") or {}
        lat, lon = loc.get("latitude"), loc.get("longitude")
    has_pos = (
        isinstance(lat, (int, float)) and isinstance(lon, (int, float))
        and not (lat == 0 and lon == 0) and dp.get("hasPosition", True) is not False
    )
    return {
        "id": row_id,
        "device_id": msg.get("end_device_ids", {}).get("device_id"),
        "received_at": msg.get("received_at") or up.get("received_at"),
        "f_port": up.get("f_port"),
        "f_cnt": up.get("f_cnt"),
        "frm_payload": up.get("frm_payload"),
        "lat": lat if has_pos else None,
        "lon": lon if has_pos else None,
        "decoded": dp,
        "radio": {
            "gateway": best.get("gateway_ids", {}).get("gateway_id"),
            "gateways": len(rx),
            "rssi": best.get("rssi"),
            "snr": best.get("snr"),
            "sf": lora.get("spreading_factor"),
            "bw": lora.get("bandwidth"),
            "frequency": up.get("settings", {}).get("frequency"),
            "airtime": up.get("consumed_airtime"),
        },
        "raw": msg,
    }


# --------------------------------------------------------------------------- TTN Storage Integration
def storage_sync(cfg, store):
    """Holt Uplinks der letzten Stunden (falls Storage Integration aktiviert ist)."""
    url = (
        f"https://{cfg['region']}.cloud.thethings.network/api/v3/as/applications/"
        f"{cfg['application_id']}/packages/storage/uplink_message?"
        + urllib.parse.urlencode({"last": f"{int(cfg['storage_sync_hours'])}h"})
    )
    req = urllib.request.Request(
        url, headers={"Authorization": f"Bearer {cfg['api_key']}", "Accept": "text/event-stream"}
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            body = r.read().decode("utf-8")
        added = 0
        for line in body.splitlines():
            line = line.strip()
            if not line:
                continue
            obj = json.loads(line)
            if store.add_uplink(obj.get("result", obj)):
                added += 1
        STATUS["storage_sync"] = f"{added} neue Uplinks aus TTN-Speicher geholt"
    except Exception as e:  # Storage Integration nicht aktiv o.ä. – kein Problem
        STATUS["storage_sync"] = f"Storage-Abgleich nicht möglich ({e})"
    print("[storage]", STATUS["storage_sync"])


# --------------------------------------------------------------------------- MQTT
class TTN:
    def __init__(self, cfg, store):
        import paho.mqtt.client as mqtt

        self.cfg, self.store = cfg, store
        self.user = f"{cfg['application_id']}@{cfg['tenant']}"
        try:
            self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"dash-{int(time.time())}")
        except AttributeError:  # paho-mqtt < 2.0
            self.client = mqtt.Client(client_id=f"dash-{int(time.time())}")
        self.client.username_pw_set(self.user, cfg["api_key"])
        self.client.tls_set()
        self.client.on_connect = self.on_connect
        self.client.on_disconnect = self.on_disconnect
        self.client.on_message = self.on_message

    def start(self):
        host = f"{self.cfg['region']}.cloud.thethings.network"
        STATUS["mqtt"] = "verbinde…"
        self.client.connect_async(host, 8883, keepalive=60)
        self.client.loop_start()

    def on_connect(self, client, userdata, flags, rc, *args):
        ok = getattr(rc, "is_failure", None) is False or rc == 0
        if ok:
            STATUS["mqtt"] = "verbunden"
            STATUS["last_error"] = None
            client.subscribe(f"v3/{self.user}/devices/+/#")
            print("[mqtt] verbunden mit TTN")
        else:
            STATUS["mqtt"] = "Fehler"
            STATUS["last_error"] = f"Verbindung abgelehnt: {rc} (API-Key / Application-ID prüfen)"
            print("[mqtt]", STATUS["last_error"])

    def on_disconnect(self, client, userdata, *args):
        STATUS["mqtt"] = "getrennt – verbinde neu…"

    def on_message(self, client, userdata, m):
        parts = m.topic.split("/")  # v3/<app>@ttn/devices/<dev>/<kind...>
        if len(parts) < 5:
            return
        dev, kind = parts[3], "/".join(parts[4:])
        if kind in ("down/push", "down/replace"):
            return
        try:
            msg = json.loads(m.payload.decode("utf-8"))
        except Exception:
            return
        if kind == "up":
            self.store.add_uplink(msg)
            print(f"[up] {dev} f_cnt={msg.get('uplink_message', {}).get('f_cnt')}")
        else:
            self.store.add_event(dev, kind, msg)
            print(f"[{kind}] {dev}")

    def downlink(self, device_id, f_port, frm_hex=None, decoded=None, confirmed=False, mode="push"):
        dl = {"f_port": f_port, "priority": "NORMAL", "confirmed": bool(confirmed)}
        if decoded is not None:
            dl["decoded_payload"] = decoded
        else:
            dl["frm_payload"] = base64.b64encode(bytes.fromhex(frm_hex)).decode()
        topic = f"v3/{self.user}/devices/{device_id}/down/{mode}"
        info = self.client.publish(topic, json.dumps({"downlinks": [dl]}), qos=1)
        return info.rc == 0, dl


# --------------------------------------------------------------------------- HTTP
def make_handler(cfg, store, ttn):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send_json(self, obj, code=200):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            u = urllib.parse.urlparse(self.path)
            q = urllib.parse.parse_qs(u.query)
            if u.path in ("/", "/index.html"):
                body = INDEX_PATH.read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif u.path == "/api/uplinks":
                self.send_json(store.uplinks(after=int(q.get("after", ["0"])[0])))
            elif u.path == "/api/status":
                self.send_json({
                    **STATUS,
                    "application_id": cfg["application_id"],
                    "devices": sorted(set(store.devices()) | set(cfg.get("devices", []))),
                    "presets": cfg["presets"],
                    "downlink_enabled": ttn is not None,
                    "events": store.events(),
                })
            elif u.path == "/api/export.csv":
                rows = store.uplinks(limit=10**9)          # alle Uplinks, nicht nur die ersten 5000
                keys = sorted({k for r in rows for k in r["decoded"]})

                def num(v, scale=1.0, unit=""):
                    """'0.452608s' -> 0.452608, '867100000' -> 867.1 (fuer Excel als Zahl)."""
                    try:
                        x = round(float(str(v).rstrip(unit)) / scale, 6)
                    except (TypeError, ValueError):
                        return None
                    return int(x) if x.is_integer() else x

                own_gw = cfg["own_gateway"]

                def gw_id(g):
                    return g.get("gateway_ids", {}).get("gateway_id", "?")

                def gw_text(g):
                    """'mein-gateway (-109 dBm / -8.2 dB)'"""
                    rssi, snr = g.get("rssi"), g.get("snr")
                    return f"{gw_id(g)} ({'?' if rssi is None else rssi} dBm / {'?' if snr is None else snr} dB)"

                out = io.StringIO()
                w = csv.writer(out, lineterminator="\n")
                w.writerow(["received_at", "device_id", "f_cnt", "rssi", "snr", "sf", "bandwidth_khz",
                            "airtime_s", "frequency_mhz", "gateway", "gateways",
                            "rssi_eigen", "snr_eigen", "gateways_alle"] + keys)
                for r in rows:
                    rd = r["radio"]
                    # rssi/snr/gateway = bestes Gateway (wie bisher).
                    # rssi_eigen/snr_eigen = nur das eigene Gateway, leer wenn es die Nachricht nicht empfangen hat.
                    # gateways_alle = alle empfangenden Gateways, stärkstes zuerst.
                    rx = r["raw"].get("uplink_message", {}).get("rx_metadata") or []
                    own = next((g for g in rx if gw_id(g) == own_gw), {})
                    alle = " | ".join(gw_text(g) for g in sorted(rx, key=lambda g: g.get("rssi", -999), reverse=True))
                    vals = [r["received_at"], r["device_id"], r["f_cnt"], rd["rssi"], rd["snr"],
                            rd["sf"], num(rd["bw"], 1000), num(rd["airtime"], unit="s"),
                            num(rd["frequency"], 1e6), rd["gateway"], rd["gateways"],
                            own.get("rssi"), own.get("snr"), alle] \
                        + [r["decoded"].get(k, "") for k in keys]
                    w.writerow(["" if v is None else v for v in vals])
                body = out.getvalue().encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/csv; charset=utf-8")
                self.send_header("Content-Disposition", 'attachment; filename="tracker.csv"')
                self.end_headers()
                self.wfile.write(body)
            else:
                self.send_json({"error": "nicht gefunden"}, 404)

        def do_POST(self):
            u = urllib.parse.urlparse(self.path)
            try:
                data = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
            except Exception:
                return self.send_json({"error": "Ungültiges JSON"}, 400)

            if u.path == "/api/sync":
                if not cfg.get("api_key"):
                    return self.send_json({"error": "Kein API-Key"}, 400)
                storage_sync(cfg, store)
                return self.send_json({"ok": True, "message": STATUS["storage_sync"]})

            if u.path != "/api/downlink":
                return self.send_json({"error": "nicht gefunden"}, 404)
            if ttn is None:
                return self.send_json({"error": "Downlinks deaktiviert (--no-mqtt)"}, 400)

            dev = str(data.get("device_id", "")).strip()
            try:
                port = int(data.get("f_port"))
            except (TypeError, ValueError):
                port = 0
            if not dev:
                return self.send_json({"error": "Gerät fehlt"}, 400)
            if not 1 <= port <= 223:
                return self.send_json({"error": "FPort muss zwischen 1 und 223 liegen"}, 400)
            mode = "replace" if data.get("mode") == "replace" else "push"

            hexstr, decoded = None, None
            if "json" in data:
                decoded = data["json"]
            else:
                hexstr = "".join(str(data.get("hex", "")).split()).replace("0x", "")
                try:
                    if not hexstr or len(bytes.fromhex(hexstr)) > 51:
                        raise ValueError
                except ValueError:
                    return self.send_json({"error": "Ungültiger Hex-Payload (1–51 Bytes)"}, 400)

            ok, dl = ttn.downlink(dev, port, hexstr, decoded, data.get("confirmed"), mode)
            store.add_event(dev, f"local/{mode}", dl)
            if not ok:
                return self.send_json({"error": "Senden an TTN fehlgeschlagen (MQTT nicht verbunden?)"}, 502)
            self.send_json({"ok": True, "downlink": dl})

    return H


# --------------------------------------------------------------------------- Main
def main():
    cfg = load_config()
    offline = "--no-mqtt" in sys.argv
    if not offline and (not cfg.get("api_key") or "XXXX" in cfg["api_key"]):
        sys.exit("Bitte api_key in config.json eintragen (oder mit --no-mqtt starten).")

    store = Store(DB_PATH)
    ttn = None
    if not offline:
        try:
            import paho.mqtt.client  # noqa: F401
        except ImportError:
            sys.exit("paho-mqtt fehlt – installiere es mit:  pip install paho-mqtt")
        threading.Thread(target=storage_sync, args=(cfg, store), daemon=True).start()
        ttn = TTN(cfg, store)
        ttn.start()

    port = int(cfg["http_port"])
    srv = ThreadingHTTPServer(("127.0.0.1", port), make_handler(cfg, store, ttn))
    print(f"Dashboard läuft:  http://localhost:{port}   (Beenden mit Ctrl+C)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nBeendet.")


if __name__ == "__main__":
    main()
