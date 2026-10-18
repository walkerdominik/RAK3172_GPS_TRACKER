# GPS Tracker Dashboard (TTN)

Lokale Webseite für deinen LoRa-Tracker. Sie zeigt die Live-Position auf einer Karte, den Track, alle Payload- und Funkdaten und den Verlauf. Ausserdem kannst du Downlinks senden. Läuft nur auf deinem Laptop unter http://localhost:8080 und kostet nichts.

## 1. Einmalig einrichten

1. **Python 3** installieren (https://python.org, unter Windows „Add to PATH“ anhaken).
2. Im Ordner ein Terminal öffnen und ausführen:
   ```
   pip install -r requirements.txt
   ```
3. **API-Key in TTN erstellen:** Console → deine Application → *API keys* → *Add API key*. Diese Rechte anhaken:
   - *Read application traffic (uplink and downlink)*
   - *Write downlink application traffic*

   Den Key gleich kopieren, er wird nur einmal angezeigt.
4. `config.example.json` nach `config.json` kopieren und den Key bei `api_key` eintragen.
5. Optional: *Integrations → Storage Integration* in TTN aktivieren. Dann holt das Dashboard beim Start Uplinks nach, die ankamen, während dein Laptop aus war.

## 2. Starten

```
python tracker.py
```
Danach http://localhost:8080 im Browser öffnen. Beenden mit `Ctrl+C`.
Alle empfangenen Uplinks landen in `tracker.db`. Mit `python tracker.py --no-mqtt` kannst du sie auch offline ansehen.

## 3. Downlinks

- Gerät oben auswählen, FPort und Payload eingeben (Hex, z.B. `01 00 3C`), dann **In Warteschlange** klicken.
- **JSON** funktioniert nur, wenn du in TTN einen Downlink-Encoder im Payload Formatter hast.
- Der Tracker ist ein Klasse-A-Gerät. Er empfängt den Downlink erst nach seinem nächsten Uplink. Der Status (in Warteschlange → gesendet → bestätigt) erscheint unter dem Formular.
- Häufige Befehle kannst du in `config.json` unter `presets` als Knöpfe hinterlegen. Die beiden Einträge dort sind nur Platzhalter.

## CSV-Export

Der Link **CSV** oben auf der Webseite lädt alle gespeicherten Uplinks als `tracker.csv` herunter.

- `rssi`, `snr`, `gateway`: das Gateway mit dem stärksten Empfang, egal welches.
- `rssi_eigen`, `snr_eigen`: nur dein eigenes Gateway. Leer, wenn es die Nachricht nicht empfangen hat. Welches Gateway als eigenes gilt, steht in `config.json` unter `own_gateway` (Standard: `mein-gateway`).
- `gateways_alle`: alle Gateways, die die Nachricht empfangen haben, mit RSSI und SNR, das stärkste zuerst.

## Hinweise

- Der Server ist nur von deinem Laptop aus erreichbar (127.0.0.1), weil man darüber Downlinks schicken kann.
- `config.json` enthält deinen API-Key. Gib diese Datei nicht weiter.

Einmalig: In der TTN Console unter *Payload formatters* den Inhalt von `ttn-payload-formatter.js` als **Uplink-** und **Downlink-Formatter** einfügen (Typ „Custom Javascript formatter“).

Auf der Webseite im Feld **Modus**:

| Modus | Was passiert | Einstellungen |
|---|---|---|
| Normal | Position im Intervall, Bewegung meldet zusätzlich | Intervall (min), Bewegungsschwelle |
| Dauer-Tracking | GPS bleibt an, Position im Intervall | Intervall (s, mind. 15) |
| Diebstahlschutz | Schläft, sendet nur bei Bewegung + Lebenszeichen | Lebenszeichen (h), Bewegungsschwelle |

- Nach **Modus übernehmen** steht der neue Modus als „Ausstehend“ da, bis der Tracker das nächste Mal sendet. Danach bestätigt der Tracker den neuen Modus innerhalb von ca. 10 s.
- Den Uplink kannst du mit dem **Taster am Tracker** sofort auslösen.
- Der Modus bleibt im Flash gespeichert, auch nach einem Neustart.
- **TTN Fair Use:** max. 30 s Sendezeit pro Tag und Gerät. Die Webseite rechnet das aus und warnt, wenn das Intervall zu kurz ist. Kurze Intervalle sind kurzfristig ok (z.B. eine Verfolgung), aber nicht als Dauerbetrieb.
