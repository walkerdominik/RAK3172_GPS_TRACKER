// TTN Payload Formatter (Uplink + Downlink) für GPS-Tracker Firmware v6
// In der TTN Console: Application → Payload formatters → Uplink / Downlink → "Custom Javascript formatter"

var MODES = ["normal", "continuous", "theft"];

function decodeUplink(input) {
  var b = input.bytes;
  if (b.length < 16) return { errors: ["Payload zu kurz: " + b.length + " Byte"] };

  function u16(i) { return (b[i] << 8) | b[i + 1]; }
  function i16(i) { var v = u16(i); return v & 0x8000 ? v - 0x10000 : v; }
  function i32(i) { return (b[i] << 24) | (b[i + 1] << 16) | (b[i + 2] << 8) | b[i + 3]; }

  var flags = b[0];
  var d = {
    hasPosition:  (flags & 0x01) !== 0,
    motionUplink: (flags & 0x02) !== 0,
    accelActive:  (flags & 0x04) !== 0,
    accelXYZ:     (flags & 0x08) !== 0,
    highAccuracy: (flags & 0x10) !== 0,
    sbasUsed:     (flags & 0x20) !== 0,
    averaged:     (flags & 0x40) !== 0,
    satellites:   b[1],
    hAccuracy_m:  u16(2)
  };

  if (d.hasPosition) {
    d.latitude   = i32(4) / 1e7;
    d.longitude  = i32(8) / 1e7;
    d.altitude_m = i16(12);
  }
  d.batteryVoltage_mV = u16(14);
  d.batteryVoltage_V  = d.batteryVoltage_mV / 1000;

  var i = 16;
  if (d.accelXYZ && b.length >= i + 6) {
    d.accelX_mg = i16(i); d.accelY_mg = i16(i + 2); d.accelZ_mg = i16(i + 4);
    i += 6;
  }

  // ab Firmware v6: aktive Konfiguration
  if (b.length >= i + 5) {
    d.mode               = MODES[b[i]] || ("unbekannt_" + b[i]);
    d.interval_s         = u16(i + 1);
    d.heartbeat_h        = b[i + 3];
    d.motionThreshold_mg = b[i + 4] * 16;
  }

  return { data: d };
}

// Ermöglicht in der TTN Console Downlinks als JSON, z.B.
// { "mode": "normal", "interval_s": 300, "heartbeat_h": 12, "motionThreshold_mg": 256 }
function encodeDownlink(input) {
  var d = input.data;
  var m = MODES.indexOf(d.mode);
  if (m < 0) return { errors: ["mode muss normal, continuous oder theft sein"] };
  var iv = d.interval_s || 300, hb = d.heartbeat_h || 12, th = Math.round((d.motionThreshold_mg || 256) / 16);
  return { fPort: 10, bytes: [0x01, m, (iv >> 8) & 0xff, iv & 0xff, hb & 0xff, th & 0x7f] };
}

function decodeDownlink(input) {
  var b = input.bytes;
  if (input.fPort !== 10 || b[0] !== 0x01) return { data: { raw: b } };
  return { data: {
    mode: MODES[b[1]], interval_s: (b[2] << 8) | b[3], heartbeat_h: b[4], motionThreshold_mg: b[5] * 16
  } };
}
