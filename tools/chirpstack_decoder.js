/*
 * ChirpStack v4 — Codec (Device Profile > Codec > JavaScript functions).
 * Decoder para el payload v2 (29 bytes, FPort 2) del nodo T3-S3
 * (BM688 + ZE15-CO + SEN65). Ver docs/PAYLOAD_DECODER.md.
 *
 * DevEUI de pruebas: 1CDBD4FFFEBD2965
 */

function decodeUplink(input) {
  var b = input.bytes;

  // Helpers little-endian
  function u16(i) { return b[i] | (b[i + 1] << 8); }
  function s16(i) { var v = u16(i); return v > 32767 ? v - 65536 : v; }
  function u32(i) { return (b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | (b[i + 3] << 24)) >>> 0; }

  // FPort 3 = boton de EMERGENCIA (SOS), no datos de sensores.
  if (input.fPort === 3) {
    return { data: { alert: "SOS", source: "panic_button" } };
  }

  // FPort 4 = ALERTA automatica por UMBRAL (threshold). 15 bytes.
  if (input.fPort === 4) {
    if (b.length < 15) {
      return { errors: ["alerta demasiado corta: " + b.length + " (esperado 15)"] };
    }
    var m = b[0];
    var fire = (m & 0x80) !== 0;
    return { data: {
      alert: fire ? "FIRE" : "THRESHOLD",   // FUEGO confirmado (multicriterio) vs umbral simple
      fire_confirmed: fire,                 // EN 54-30/31: coincidencia de >=2 familias
      triggered: {
        temperature: (m & 0x01) !== 0,
        co:          (m & 0x02) !== 0,
        pm2_5:       (m & 0x04) !== 0,
        pm10:        (m & 0x08) !== 0,
        voc:         (m & 0x10) !== 0,
        gas:         (m & 0x20) !== 0,
        heat_rate:   (m & 0x40) !== 0,      // EN 54-5 rate-of-rise (subida rapida)
        fire:        fire
      },
      values: {
        temperature_c:      s16(1) / 100,
        co_ppm:             u16(3) / 10,
        pm2p5_ugm3:         u16(5) / 10,
        pm10_ugm3:          u16(7) / 10,
        voc_index:          u16(9) / 10,
        gas_resistance_ohm: u32(11)
      }
    }};
  }

  if (b.length < 29) {
    return { errors: ["payload demasiado corto: " + b.length + " (esperado 29)"] };
  }

  var flags = b[0];
  var bmOk  = (flags & 0x01) !== 0;
  var coOk  = (flags & 0x02) !== 0;
  var coFlt = (flags & 0x04) !== 0;
  var senOk = (flags & 0x08) !== 0;

  var data = {
    status: {
      bm688: bmOk,
      ze15co: coOk,
      ze15co_fault: coFlt,
      sen65: senOk
    }
  };

  // --- BM688 (ambiental) ---
  if (bmOk) {
    data.bm688 = {
      temperature_c:      s16(1) / 100,   // grados C
      humidity_pct:       u16(3) / 100,   // %RH
      pressure_hpa:       u16(5) / 10,    // hPa
      gas_resistance_ohm: u32(7)          // Ohm (mas alto = aire mas limpio)
    };
  }

  // --- ZE15-CO (monoxido de carbono) ---
  if (coOk) {
    data.ze15co = {
      co_ppm: u16(11) / 10                // ppm
    };
  }

  // --- SEN65 (calidad de aire) ---
  if (senOk) {
    data.sen65 = {
      pm1p0_ugm3:    u16(13) / 10,        // ug/m3
      pm2p5_ugm3:    u16(15) / 10,        // ug/m3
      pm4p0_ugm3:    u16(17) / 10,        // ug/m3
      pm10_ugm3:     u16(19) / 10,        // ug/m3
      humidity_pct:  u16(21) / 100,       // %RH
      temperature_c: s16(23) / 100,       // grados C
      voc_index:     u16(25) / 10,        // indice (1..500, ~100 nominal)
      nox_index:     u16(27) / 10         // indice (1..500, ~1 en aire limpio)
    };
  }

  return { data: data };
}

// (Opcional) sin downlinks de aplicacion; el FPort 10 del HTML-OTA se gestiona
// fuera de este codec.
function encodeDownlink(input) {
  return { bytes: [] };
}
