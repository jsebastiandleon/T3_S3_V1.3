/*
 * ChirpStack v4 — Codec (Device Profile > Codec > JavaScript functions).
 * Decoder del nodo T3-S3 (BM688 + ZE15-CO + SEN65). Ver docs/PAYLOAD_DECODER.md.
 *   FPort 2 -> datos v2 (29 B)      FPort 4 -> alerta por umbral (17 B)
 *   FPort 3 -> aviso incidencia (4 B)  FPort 5 -> salud del nodo (13 B)
 *
 * DevEUI de pruebas: 1CDBD4FFFEBD2965
 */

function decodeUplink(input) {
  var b = input.bytes;

  // Helpers little-endian
  function u16(i) { return b[i] | (b[i + 1] << 8); }
  function s16(i) { var v = u16(i); return v > 32767 ? v - 65536 : v; }
  function u32(i) { return (b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | (b[i + 3] << 24)) >>> 0; }

  // FPort 3 = AVISO DE INCIDENCIA: alguien ha tocado un telefono de la
  // Policia Local en el portal cautivo del nodo. No son datos de sensores:
  // es una persona diciendo que esta comunicando algo.
  //
  // 'avisos' es el contador desde el arranque del nodo. Si salta de 3 a 5, un
  // aviso intermedio se perdio en el aire (van UNCONFIRMED): el hueco es la
  // unica forma que tiene el servidor de enterarse.
  if (input.fPort === 3) {
    // Nodos antiguos mandaban los 3 bytes ASCII "SOS" del boton retirado.
    if (b.length === 3 && b[0] === 0x53 && b[1] === 0x4F && b[2] === 0x53) {
      return { data: { alert: "SOS", source: "panic_button", legacy: true } };
    }
    if (b.length < 4) {
      return { errors: ["aviso demasiado corto: " + b.length + " (esperado 4)"] };
    }
    if (b[0] !== 1) {
      return { errors: ["msg_type desconocido en FPort 3: " + b[0]] };
    }
    var tel = b[1] === 1 ? "962878800" : (b[1] === 2 ? "092" : "desconocido");
    return { data: {
      alert:   "INCIDENCIA",
      source:  "portal_call",          // toque en un telefono del portal
      llamada: tel,                    // a que numero se llamo
      destino: b[1] === 2 ? "urgencias" : "policia_local_gandia",
      avisos:  u16(2)                  // acumulado desde el arranque del nodo
    } };
  }

  // FPort 4 = ALERTA automatica por UMBRAL (threshold). 17 bytes desde v2.9.
  //
  // Hay DOS mascaras y no significan lo mismo:
  //   triggered (byte 0)  = FLANCO: que cruzo el umbral en ese ciclo. No se
  //                         re-arma hasta que el valor baja de umbral*0.9, asi
  //                         que un valor alto desde hace rato NO aparece aqui.
  //   above     (byte 16) = NIVEL: que estaba por encima del umbral en ese
  //                         instante. Es el que hay que mirar para pintar
  //                         estado; 'triggered' es para el evento.
  //
  // valid (byte 15) dice que sensor respalda cada valor. Un campo cuyo sensor
  // tiene el bit a 0 vale 0 pero NO es una medida: es "sin dato" (se devuelve
  // null). Los nodos <= v2.8 mandan 15 bytes y no traen estos dos bytes.
  if (input.fPort === 4) {
    if (b.length < 15) {
      return { errors: ["alerta demasiado corta: " + b.length + " (esperado 17)"] };
    }
    var m = b[0];
    var fire = (m & 0x80) !== 0;
    var legacy = b.length < 17;             // nodo v2.8 o anterior

    function bits(x) {
      return {
        temperature: (x & 0x01) !== 0,
        co:          (x & 0x02) !== 0,
        pm2_5:       (x & 0x04) !== 0,
        pm10:        (x & 0x08) !== 0,
        voc:         (x & 0x10) !== 0,
        gas:         (x & 0x20) !== 0,
        heat_rate:   (x & 0x40) !== 0,      // EN 54-5 rate-of-rise (subida rapida)
        fire:        (x & 0x80) !== 0
      };
    }

    // Sin byte de validez (nodo antiguo) no se puede afirmar nada: se asumen
    // validos, que es como se venia interpretando hasta ahora.
    var v     = legacy ? 0xFF : b[15];
    var bmOk  = (v & 0x01) !== 0;
    var coOk  = (v & 0x02) !== 0;
    var senOk = (v & 0x08) !== 0;

    var out = {
      alert: fire ? "FIRE" : "THRESHOLD",   // FUEGO confirmado (multicriterio) vs umbral simple
      fire_confirmed: fire,                 // EN 54-30/31: coincidencia de >=2 familias
      triggered: bits(m),                   // FLANCO: que cruzo
      values: {
        temperature_c:      bmOk  ? s16(1) / 100 : null,
        co_ppm:             coOk  ? u16(3) / 10  : null,
        pm2p5_ugm3:         senOk ? u16(5) / 10  : null,
        pm10_ugm3:          senOk ? u16(7) / 10  : null,
        voc_index:          senOk ? u16(9) / 10  : null,
        gas_resistance_ohm: bmOk  ? u32(11)      : null
      },
      sensors_ok: { bm688: bmOk, ze15co: coOk, sen65: senOk },
      legacy_payload: legacy
    };

    if (!legacy) {
      out.above = bits(b[16]);              // NIVEL: que seguia por encima
    }

    // Un sensor caido no solo deja un hueco: apaga los umbrales que dependen
    // de el. Sin BM688 no hay temperatura fija, ni rate-of-rise, ni familia
    // CALOR para el criterio de FUEGO -> la deteccion de ese nodo queda
    // reducida. Que se vea en el JSON y no haya que deducirlo.
    var down = [];
    if (!bmOk)  { down.push("bm688"); }
    if (!coOk)  { down.push("ze15co"); }
    if (!senOk) { down.push("sen65"); }
    if (down.length > 0) {
      out.warnings = ["sensor sin lectura valida: " + down.join(", ") +
                      " (sus umbrales no estan vigilando)"];
    }

    return { data: out };
  }

  // FPort 5 = SALUD DEL NODO. El primer byte es el msg_type:
  //   1 = BOOT  (13 B) — por que ha arrancado el nodo y que firmware lleva
  //   2 = FAULT (14 B) — un sensor ha caido o se ha recuperado
  // Este canal existe para que un reinicio y una averia dejen de ser
  // invisibles: sin el, el nodo sigue diciendo "todo bien" con la capacidad
  // de deteccion mermada, y un reinicio solo deja un devAddr nuevo.
  if (input.fPort === 5) {
    if (b.length < 1) {
      return { errors: ["diag vacio"] };
    }

    // Mascara de sensores: mismos bits que el byte 0 del FPort 2.
    function sensorMask(m) {
      return { bm688: (m & 0x01) !== 0, ze15co: (m & 0x02) !== 0,
               sen65: (m & 0x08) !== 0 };
    }
    function sensorNames(m) {
      var n = [];
      if (m & 0x01) { n.push("bm688"); }
      if (m & 0x02) { n.push("ze15co"); }
      if (m & 0x08) { n.push("sen65"); }
      return n;
    }

    if (b[0] === 1) {
      if (b.length < 13) {
        return { errors: ["diag BOOT corto: " + b.length + " (esperado 13)"] };
      }
      var causes = ["UNKNOWN", "POR", "PIN", "SOFTWARE", "WATCHDOG",
                    "LOW_POWER_WAKE", "CPU_LOCKUP", "BROWNOUT"];
      var code = b[1];
      var fw   = u16(10);
      return { data: {
        event:       "BOOT",
        reset_cause: causes[code] || ("INVALID_" + code),
        reset_code:  code,
        reset_raw:   u32(2),
        boot_count:  u32(6),
        // Alimentacion o software: es la pregunta que este canal resuelve.
        suspect:     (code === 7) ? "power"
                   : (code === 6) ? "firmware"
                   : (code === 4) ? "watchdog"
                   : "normal",
        fw_version:  "v" + (fw >> 8) + "." + (fw & 0xFF),
        sensors_at_boot: sensorMask(b[12])
      }};
    }

    if (b[0] === 2) {
      if (b.length < 14) {
        return { errors: ["diag FAULT corto: " + b.length + " (esperado 14)"] };
      }
      var nowFaulted = b[1];
      var down = b[2];
      var up   = b[3];
      return { data: {
        event:          "SENSOR_FAULT",
        // degraded = el nodo esta detectando con menos criterios de los que
        // deberia. Es la condicion que debe disparar aviso en el SCADA.
        degraded:       nowFaulted !== 0,
        faulted:        sensorMask(nowFaulted),
        faulted_list:   sensorNames(nowFaulted),
        went_down:      sensorNames(down),
        came_up:        sensorNames(up),
        uptime_s:       u32(4),
        total_failed_reads: {
          bm688:  u16(8),
          ze15co: u16(10),
          sen65:  u16(12)
        }
      }};
    }

    return { errors: ["msg_type de diag desconocido: " + b[0]] };
  }

  if (b.length < 29) {
    return { errors: ["payload demasiado corto: " + b.length + " (esperado 29)"] };
  }

  var flags = b[0];
  var bmOk  = (flags & 0x01) !== 0;
  var coOk  = (flags & 0x02) !== 0;
  var coFlt = (flags & 0x04) !== 0;
  var senOk = (flags & 0x08) !== 0;
  var apOn  = (flags & 0x10) !== 0;

  var data = {
    status: {
      bm688: bmOk,
      ze15co: coOk,
      ze15co_fault: coFlt,
      sen65: senOk,
      // Radio del SoftAP en el momento del envio. Se apaga en la franja
      // nocturna para ahorrar energia (nodo solar). Sirve para confirmar que
      // el AP VUELVE por la mañana, que es el fallo que importa.
      wifi_ap: apOn
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
