# Flujo de datos y mensaje LoRa — T3-S3 (BM688 + ZE15-CO + SEN65)

Documento de referencia del nodo multisensor: **qué se envía, cómo se envía,
cada cuánto, cómo se conforma el mensaje y cómo se decodifica en ChirpStack.**

```
 ┌─────────┐  I2C0/UART1/I2C1   ┌──────────────┐   LoRaWAN EU868     ┌────────────┐
 │ 3 sensores ───────────────► │  ESP32-S3     │ ─── FPort 2, OTAA ─►│ Gateway →   │
 │ BM688/CO/SEN65 │  (cada 5 s) │  (T3-S3)      │   29 B, unconfirmed │ ChirpStack  │
 └─────────┘                    │  arma payload │   cada 180 s        │ + decoder JS│
                                └──────┬───────┘                      └────────────┘
                                       │ WiFi AP "Gesinen_WildFire"
                                       ▼  http://192.168.4.1 (dashboard live)
```

---

## 1. Qué se envía

Un **paquete binario de 29 bytes** con TODOS los datos medibles de los 3
sensores + un byte de estado. No se manda texto/JSON por radio (ineficiente);
el JSON solo existe en el portal WiFi y en la salida del decoder de ChirpStack.

| Sensor | Datos que viajan en el paquete |
|---|---|
| **BM688** | temperatura, humedad, presión, resistencia de gas |
| **ZE15-CO** | concentración de CO + bit de fallo |
| **SEN65** | PM1.0, PM2.5, PM4.0, PM10.0, humedad, temperatura, índice VOC, índice NOx |

---

## 2. Cada cuánto cada sensor entrega dato al microcontrolador

Hay **tres ritmos distintos** (desacoplados a propósito):

| Ritmo | Periodo | Qué pasa |
|---|---|---|
| **Muestreo interno del sensor** | 1 s (CO y SEN65) / on-demand (BM688) | el sensor genera una medición nueva |
| **Lectura del firmware** | **5 s** (`SENSOR_PERIOD_S`) | el MCU lee los 3 sensores y refresca el portal |
| **Envío LoRa** | **180 s** (`LORA_PERIOD_S`) | se transmite un uplink por radio |

Detalle por sensor:

| Sensor | Bus | Muestreo interno | Lo lee el firmware | Notas de tiempo |
|---|---|---|---|---|
| **BM688** | I2C0 @0x77, 100 kHz | *forced mode*: mide cuando se le pide | cada 5 s (`sensor_sample_fetch`) | el heater de gas tarda ~150 ms por medición |
| **ZE15-CO** | UART1 9600 8N1 | *active-upload* cada 1 s (o responde a Q&A) | cada 5 s (consulta Q&A) | preheat 30 s; respuesta ≤30 s |
| **SEN65** | I2C1 @0x6B, 100 kHz | *continuous*: dato nuevo cada 1 s | cada 5 s (data-ready + read) | VOC/NOx **convergen ~1 min** (antes salen 0) |

> El dashboard del portal (`/api/sensors`) se refresca con cada lectura de 5 s,
> así que está "vivo" aunque el envío LoRa sea cada 3 min.

---

## 3. Cómo se conforma el mensaje (29 bytes, little-endian)

En `src/main.c` se rellena un `struct __packed` y se envía tal cual con
`lorawan_send(2, &payload, sizeof(payload), LORAWAN_MSG_UNCONFIRMED)`.

| Offset | Tipo | Campo | Escala → unidad | Origen |
|---|---|---|---|---|
| 0 | uint8 | **flags** | bit0 BM688 ok · bit1 CO ok · bit2 CO fault · bit3 SEN65 ok | — |
| 1–2 | int16 | temperatura | ÷100 → °C | BM688 |
| 3–4 | uint16 | humedad | ÷100 → %RH | BM688 |
| 5–6 | uint16 | presión | ÷10 → hPa | BM688 |
| 7–10 | uint32 | gas resistance | → Ω | BM688 |
| 11–12 | uint16 | CO | ÷10 → ppm | ZE15-CO |
| 13–14 | uint16 | PM1.0 | ÷10 → µg/m³ | SEN65 |
| 15–16 | uint16 | PM2.5 | ÷10 → µg/m³ | SEN65 |
| 17–18 | uint16 | PM4.0 | ÷10 → µg/m³ | SEN65 |
| 19–20 | uint16 | PM10.0 | ÷10 → µg/m³ | SEN65 |
| 21–22 | uint16 | humedad SEN65 | ÷100 → %RH | SEN65 |
| 23–24 | int16 | temperatura SEN65 | ÷100 → °C | SEN65 |
| 25–26 | uint16 | índice VOC | ÷10 | SEN65 |
| 27–28 | uint16 | índice NOx | ÷10 | SEN65 |

Reglas:
- **Little-endian** (byte bajo primero). Ej.: `8f 0a` = `0x0A8F` = 2703 → 27.03 °C.
- Si un sensor no aportó dato, sus bytes van a **0**; mirar **flags** (byte 0).
- **Sin CRC de aplicación**: LoRaWAN ya protege la trama con su MIC.
- Tamaño fijo 29 B → cabe de sobra en EU868 (máx. 51 B a DR0/SF12).

---

## 4. Cómo se envía (LoRaWAN)

| Parámetro | Valor | Por qué |
|---|---|---|
| Región | EU868 | — |
| Activación | **OTAA** | claves de sesión negociadas en el join (más seguro que ABP) |
| FPort | **2** | datos de sensores (el 10 se reserva para HTML-OTA del portal) |
| Tipo | **UNCONFIRMED** | sin ACK → menos airtime, mejor duty-cycle y batería |
| **ADR** | **activado** | el server baja SF12→SF7 según el enlace: menos airtime/consumo |
| Cadencia | **180 s** (+1 al unir) | a SF12, 29 B ≈ 1.5 s de aire; el 1 % de duty obliga a ~150 s |

**Credenciales (dispositivo de pruebas):**
```
DevEUI : 1CDBD4FFFEBD2965
JoinEUI: 0000000000000000
AppKey : 062635ACC3BBC92C2FEF994F5EF0F69B
```

Secuencia al arrancar: `lorawan_start()` → `lorawan_enable_adr(true)` →
`lorawan_join()` (OTAA, reintenta cada 10 s hasta unir) → bucle de envío.

---

## 5. Cómo se decodifica en ChirpStack

ChirpStack muestra el uplink **en HEX** hasta que se le da el decoder. Pega el
contenido de [`tools/chirpstack_decoder.js`](../tools/chirpstack_decoder.js) en
**Device Profile → Codec → JavaScript functions** (función `decodeUplink`).

### Ejemplo real (uplink capturado)
Bytes recibidos:
```
0b 8f 0a 12 12 68 27 9c 8f 01 00 05 00 41 00 5d 00 72 00 7c 00 11 12 54 0a 88 04 0a 00
```
El decoder lo convierte en:
```json
{
  "status": { "bm688": true, "ze15co": true, "ze15co_fault": false, "sen65": true },
  "bm688":  { "temperature_c": 27.03, "humidity_pct": 46.26,
              "pressure_hpa": 1008.8, "gas_resistance_ohm": 102300 },
  "ze15co": { "co_ppm": 0.5 },
  "sen65":  { "pm1p0_ugm3": 6.5, "pm2p5_ugm3": 9.3, "pm4p0_ugm3": 11.4,
              "pm10_ugm3": 12.4, "humidity_pct": 46.25, "temperature_c": 26.44,
              "voc_index": 116.0, "nox_index": 1.0 }
}
```

Verificación manual de un campo: bytes `8f 0a` (offset 1–2) → little-endian
`0x0A8F` = 2703 → ÷100 = **27.03 °C**. ✔

---

## 6. Cómo interpretar los valores

| Valor | Lectura |
|---|---|
| **Gas resistance (BM688)** | en Ω; **más alto = aire más limpio**. Es relativo (sensor MOX), no una unidad absoluta de gas. |
| **Índice VOC (SEN65)** | escala 1–500, **~100 = condición normal**; sube con compuestos orgánicos volátiles. Converge ~1 min tras arrancar. |
| **Índice NOx (SEN65)** | escala 1–500, **~1 en aire limpio**; sube con NOx. |
| **PM (SEN65)** | masa de partículas en µg/m³ (PM2.5 es el indicador de calidad de aire más usado). |
| **CO (ZE15-CO)** | ppm; 0–500, resolución 0.1. En aire limpio ronda 0. |
| **flags = 0** en un sensor | ese sensor no respondió ese ciclo (cableado/ausente); sus campos son 0. |

---

## 7. Cosas de interés / lecciones

- **Buses separados:** BM688 en I2C0 (GPIO47/48), SEN65 en I2C1 propio
  (GPIO15/16), CO en UART1 (GPIO40/41), LoRa SX1262 en SPI2. Sin colisiones.
- **Pull-ups I2C:** se usan los **internos** del ESP32-S3 (`bias-pull-up` en el
  pinctrl). No hacen falta resistencias externas para cableado corto a 100 kHz.
- **GND común obligatorio:** si un sensor se alimenta de otra fuente, su GND
  DEBE unirse al del T3-S3 o el I2C no funciona (fue la causa de un fallo real).
- **Alimentación:** BM688 y SEN65 a 3.3 V; **ZE15-CO necesita 5–12 V** (su UART
  sí es 3 V). SEN65 pide ~200 mA (ventilador+láser).
- **Cadencias desacopladas:** sensores/portal cada 5 s (dashboard vivo) e
  independientes del envío LoRa cada 180 s (regulado por duty-cycle).
- **Portal cautivo:** AP abierto `Gesinen_WildFire`, se auto-abre (redirect 302
  + DNS por DHCP) y muestra los 3 sensores en `http://192.168.4.1`. El HTML es
  actualizable por downlink LoRa en **FPort 10** (protocolo BEGIN/DATA/COMMIT).
- **Ajustar el ritmo de envío:** cambiar `LORA_PERIOD_S` en `main.c`. Bajarlo
  mucho a SF12 da error de duty-cycle (-111); con ADR a SF7 hay margen.
- **Datos extra disponibles no enviados:** el SEN65 puede dar *número* de
  partículas (part/cm³, cmd 0x0316) y el BME688 índice IAQ/eCO2 con la librería
  Bosch BSEC (propietaria, no incluida). Se podrían añadir si se requiere.

---

*Archivos relacionados: `src/main.c` (armado del payload), `tools/chirpstack_decoder.js`
(decoder), `docs/PAYLOAD_DECODER.md` (tabla + alta ChirpStack), `docs/SEN65_INTEGRATION.md`,
`docs/ZE15_CO_INTEGRATION.md`, `docs/BM688_INTEGRATION.md`, `docs/PORTAL_CAUTIVO.md`.*
