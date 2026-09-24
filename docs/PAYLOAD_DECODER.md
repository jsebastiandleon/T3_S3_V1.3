# Payload LoRaWAN + Decoder — T3-S3 (BM688 + ZE15-CO + SEN65)

Nodo multisensor de calidad de aire. Uplink LoRaWAN (EU868, OTAA, **FPort 2**,
**unconfirmed**, **ADR on**). Payload **v2**: binario, little-endian, **29 bytes**.

**DevEUI = MAC del ESP32** (eFuse), derivada en runtime y cargada sola en cada
arranque (EUI-64 estándar insertando `FF FE`). Se imprime en el log de boot como
`DevEUI(MAC)=...`; para esta placa es **`1CDBD4FFFEBD2444`**. JoinEUI:
`0000000000000000` · AppKey: `062635ACC3BBC92C2FEF994F5EF0F69B`.

**Mapa de FPorts (canales lógicos)** — cada tipo de mensaje va por su propio
FPort para poder enrutarlo/actuar por separado en ChirpStack:

| FPort | Sentido | Contenido |
|---|---|---|
| **2** | uplink | **Datos** periódicos (promedio de la ventana), 29 B. Este doc §2. |
| **3** | uplink | **Aviso de incidencia** del portal (4 B): `msg_type`=1, `source` (1=962878800, 2=092), `count` u16 LE → `{"alert":"INCIDENCIA","source":"portal_call","llamada":"962878800","avisos":N}`. Los nodos con firmware antiguo mandan aquí el ASCII `"SOS"` (3 B) del botón retirado; el decoder lo sigue reconociendo con `legacy:true`. |
| **4** | uplink | **Alerta por umbral** (threshold), 17 B desde v2.9 (15 B hasta v2.8). Ver §2.1. |
| **10** | downlink | Actualización OTA del HTML del portal (BEGIN/DATA/COMMIT). |

Los FPorts de envío se definen en el bloque *CANALES (FPorts)* al principio de
`src/main.c` (`FPORT_DATA`, `FPORT_INCID`, `FPORT_ALERT`).

---

## 1. Qué mide cada sensor (capacidades completas)

### BM688 (Bosch BME688) — sensor ambiental, I2C0 @0x77
| Magnitud | Unidad | Notas |
|---|---|---|
| Temperatura | °C | rango -40..85 °C |
| Humedad relativa | %RH | 0..100 % |
| Presión barométrica | Pa / hPa | 300..1100 hPa |
| Resistencia de gas | Ω | sensor MOX; **más alto = aire más limpio**. Correlaciona con VOCs. |

> El BME688 también puede dar índice IAQ / eCO2 / bVOC, **pero solo con la
> librería Bosch BSEC** (propietaria). El driver `bosch,bme680` de Zephyr
> entrega los 4 valores crudos de arriba; es lo que enviamos.

### ZE15-CO (Winsen) — monóxido de carbono, UART1
| Parámetro | Valor |
|---|---|
| Gas | CO |
| Rango | 0–500 ppm |
| Resolución | 0.1 ppm |
| Salidas | UART 9600 8N1 (0/3V), analógica PIN10 (0.4–2V ↔ 0–500 ppm), fault PIN3 (1Hz) |
| Alimentación | **5–12 V DC** (PIN15) |
| Precalentamiento | 30 s · Respuesta/recuperación ≤30 s |
| Vida útil | 3–5 años |

> Único dato digital: **concentración de CO (ppm)** + bit de **fallo** del sensor.

### SEN65 (Sensirion SEN6x) — calidad de aire, I2C1 @0x6B
| Magnitud | Unidad | Notas |
|---|---|---|
| PM1.0 / PM2.5 / PM4.0 / PM10.0 | µg/m³ | masa de partículas |
| Humedad relativa | %RH | sensor RH&T propio |
| Temperatura | °C | sensor RH&T propio |
| Índice VOC | 1–500 | ~100 nominal; converge ~1 min |
| Índice NOx | 1–500 | ~1 en aire limpio; converge ~1 min |

> El SEN65 también puede dar **número de partículas** (PM0.5/1/2.5/4/10 part/cm³)
> con el comando 0x0316; no se lee por ahora (la masa µg/m³ es lo estándar).

---

## 2. Formato del mensaje (payload v2, 29 bytes, little-endian)

| Offset | Tipo | Campo | Escala → unidad |
|---|---|---|---|
| 0 | uint8 | **flags** | bit0 BM688 ok · bit1 CO ok · bit2 CO fault · bit3 SEN65 ok |
| 1–2 | int16 | BM688 temperatura | ÷100 → °C |
| 3–4 | uint16 | BM688 humedad | ÷100 → %RH |
| 5–6 | uint16 | BM688 presión | ÷10 → hPa |
| 7–10 | uint32 | BM688 gas | → Ω |
| 11–12 | uint16 | CO | ÷10 → ppm |
| 13–14 | uint16 | PM1.0 | ÷10 → µg/m³ |
| 15–16 | uint16 | PM2.5 | ÷10 → µg/m³ |
| 17–18 | uint16 | PM4.0 | ÷10 → µg/m³ |
| 19–20 | uint16 | PM10.0 | ÷10 → µg/m³ |
| 21–22 | uint16 | SEN65 humedad | ÷100 → %RH |
| 23–24 | int16 | SEN65 temperatura | ÷100 → °C |
| 25–26 | uint16 | VOC index | ÷10 |
| 27–28 | uint16 | NOx index | ÷10 |

Los campos de un sensor ausente van a **0**; usar **flags** (byte 0) para saber
qué es válido. No hay CRC de aplicación: LoRaWAN ya protege la trama (MIC).

### 2.1 Payload de ALERTA por umbral (FPort 4, 17 bytes, little-endian)

Se envía **automáticamente** cuando una lectura instantánea **cruza** un umbral
configurado (flanco de subida). No se reenvía hasta que el valor baje del umbral
(con histéresis) y lo vuelva a cruzar. Es autodescriptivo (máscara + valores):

| Offset | Tipo | Campo | Escala → unidad |
|---|---|---|---|
| 0 | uint8 | **alert_mask** | bit0 temp · bit1 CO · bit2 PM2.5 · bit3 PM10 · bit4 VOC · bit5 gas · **bit6 rate-of-rise térmico (EN 54-5)** · **bit7 FUEGO confirmado (EN 54-30/31)** |
| 1–2 | int16 | temperatura | ÷100 → °C |
| 3–4 | uint16 | CO | ÷10 → ppm |
| 5–6 | uint16 | PM2.5 | ÷10 → µg/m³ |
| 7–8 | uint16 | PM10 | ÷10 → µg/m³ |
| 9–10 | uint16 | VOC index | ÷10 |
| 11–14 | uint32 | gas resistance | → Ω |
| 15 | uint8 | **valid_mask** | qué sensor respalda cada valor: bit0 BM688 · bit1 ZE15-CO · bit3 SEN65 (mismos bits que el byte 0 del FPort 2) · **bit6 = la temperatura viene del SEN65 (respaldo), no del BM688** |
| 16 | uint8 | **active_mask** | qué umbrales estaban **por encima** en ese instante (mismos bits que `alert_mask`) |

Los bytes **15 y 16 se añaden al final** en la v2.9: los offsets 0–14 no se
mueven, así que un decoder antiguo sigue leyendo la trama y uno nuevo acepta
las tramas de 15 B de los nodos aún sin actualizar (`legacy_payload: true`).

**`alert_mask` es FLANCO, `active_mask` es NIVEL.** Es la distinción que más
confusión ha causado, así que conviene tenerla clara:

- `alert_mask` = qué **cruzó** el umbral en ese ciclo. Un bit que ya disparó
  **no vuelve a aparecer** hasta que el valor baje de `umbral × 0.9` y lo cruce
  otra vez. Nunca es 0.
- `active_mask` = qué **estaba por encima** del umbral en ese mismo instante.

Caso real: PM2.5 = 433 µg/m³ llevaba rato alto (ya había disparado) y el PM10
cruzó los 150 por primera vez. `alert_mask` marcaba **solo PM10**, que es
correcto pero parecía un fallo. Con `active_mask` se ve que **ambos** estaban
altos. Para pintar estado usar `active_mask`; para el evento, `alert_mask`.

**Temperatura: dos fuentes.** El nodo tiene dos termómetros. Desde la **v2.10**
la detección térmica usa el del BM688 como primaria y **cae a la del SEN65** si
el BM688 falla, en vez de apagarse. Por eso:

- `temperature_c` es válida si **bit0 O bit6**; `gas_resistance_ohm` solo si bit0.
- El decoder expone `temperature_source`: `"bm688"`, `"sen65"` o `null`.
- Con el respaldo activo el umbral fijo y el rate-of-rise **siguen vigilando**,
  pero con el sesgo del SEN65 (mide su propio die, con ventilador y láser
  dentro del módulo). Si solo se ha calibrado `TEMP_OFFSET_BM688_C`, el umbral
  de 58 °C sobre el respaldo arrastra ese sesgo.
- Una trama **legacy de 15 B** (≤ v2.8) no tiene este bit: ahí la temperatura
  siempre es del BM688.

**`valid_mask` distingue "0" de "sin dato".** El campo de un sensor cuyo bit
está a 0 vale 0 pero **no es una medida**; el decoder lo devuelve como `null`.
Hasta la v2.8 este payload copiaba el último valor sin comprobar validez, y un
nodo cuyo sensor nunca había leído bien desde el arranque emitía memoria sin
inicializar — de ahí los **CO = 6553.5 ppm** (0xFFFF, imposible: el driver
enmascara con `0x1F`, tope 819.1 ppm) y las **temperaturas/gas a 0** vistos en
campo. Un sensor caído además **apaga sus umbrales**: sin BM688 no hay
temperatura fija, ni rate-of-rise, ni familia *calor* para el criterio de
FUEGO, así que el decoder lo avisa en `warnings`.

Los valores son los del **instante del disparo** (incluido `active_mask`, para
que toda la trama describa un solo momento: el envío puede retrasarse por el
cooldown o por la prioridad del FPort 2). **bit7 = FUEGO** se activa por
coincidencia multicriterio (ver abajo) y ese uplink se envía **de inmediato**
(salta el cooldown). El decoder pone `alert:"FIRE"` y `fire_confirmed:true`.

Salida del decoder (FPort 4) — ejemplo de FUEGO confirmado (humo + CO):
```json
{ "alert": "FIRE", "fire_confirmed": true,
  "triggered": { "temperature": false, "co": true, "pm2_5": true, "pm10": true,
                 "voc": false, "gas": false, "heat_rate": false, "fire": true },
  "above":     { "temperature": false, "co": true, "pm2_5": true, "pm10": true,
                 "voc": false, "gas": false, "heat_rate": false, "fire": true },
  "values": { "temperature_c": 33.0, "co_ppm": 50.0, "pm2p5_ugm3": 2918.0,
              "pm10_ugm3": 873.5, "voc_index": 9.0, "gas_resistance_ohm": 20700 },
  "sensors_ok": { "bm688": true, "ze15co": true, "sen65": true },
  "legacy_payload": false }
```

Ejemplo con el BM688 caído — el `null` y el aviso son la diferencia frente a
leer un 0 como si fuese una temperatura:
```json
{ "alert": "THRESHOLD", "fire_confirmed": false,
  "triggered": { "pm10": true, "...": false },
  "above":     { "pm2_5": true, "pm10": true, "...": false },
  "values": { "temperature_c": null, "pm2p5_ugm3": 433.0, "pm10_ugm3": 160.0,
              "gas_resistance_ohm": null },
  "sensors_ok": { "bm688": false, "ze15co": true, "sen65": true },
  "warnings": ["sensor sin lectura valida: bm688 (sus umbrales no estan vigilando)"] }
```

**Dónde se definen los umbrales:** bloque *UMBRALES DE ALERTA* al principio de
`src/main.c`. Cada uno tiene un `_EN` (1/0 para activar/desactivar) y su valor.
Valores orientados a **detección de incendio** (base en estándares):
`TH_CO_MAX 10.0` ppm (EPA AQI CO "USG"), `TH_PM25_MAX 35.0` µg/m³ (EPA AQI PM2.5
"USG"; principal marcador de humo), `TH_PM10_MAX 150.0` µg/m³, `TH_TEMP_MAX 58.0`
°C (**EN 54-5**, banda de respuesta clase A1 54–65 °C), `TH_VOC_MAX 150.0` (índice
Sensirion, base ~100). El gas del BM688 es de tipo "mínimo" (alerta si CAE por
debajo de `TH_GAS_MIN`) y viene **desactivado** (MOX sin calibrar). Ajustables:
`TH_HYSTERESIS_PCT` (histéresis de re-armado) y `ALERT_MIN_INTERVAL_S` (cooldown).

**Normas de incendio implementadas:**
- **EN 54-5 · rate-of-rise térmico** (`TH_ROR_CPMIN`, def. 8 °C/min sobre ventana
  `TH_ROR_WINDOW_S`): dispara **bit6** si la temperatura sube rápido, aunque no
  llegue al umbral fijo — detecta un fuego cercano antes que el umbral absoluto.
- **EN 54-30/31 · confirmación multicriterio** (`FIRE_MIN_CRITERIA`, def. 2):
  declara **FUEGO** (bit7) solo si coinciden ≥2 **familias** de indicio —
  **humo** (PM2.5/PM10), **CO** y **calor** (temp fija o rate-of-rise). Evita
  falsas alarmas (polvo = solo PM, cocina = solo CO, calor solar = solo temp);
  un incendio real activa varias a la vez. El uplink de FUEGO **ignora el
  cooldown** y sale de inmediato.

### Cómo se conforma (en `src/main.c`)
Un `struct __packed` con esos campos en ese orden exacto. Cada campo es el
**PROMEDIO** de las lecturas tomadas dentro de la ventana de envío (los sensores
se leen cada `SENSOR_READ_PERIOD_S` y se acumulan; al enviar se divide entre el
nº de muestras), no la última lectura. Se manda con `lorawan_send(2, &payload,
sizeof(payload), LORAWAN_MSG_UNCONFIRMED)`. La estrategia de envío: **ADR
activado** (el server baja SF12→SF7), **unconfirmed** (sin ACK), **cadencia
`LORA_SEND_PERIOD_S` = 180 s** (respeta el 1 % de duty-cycle EU868 incluso en
SF12). Para cambiar la cadencia, editar `LORA_SEND_PERIOD_S` (bloque
*CONFIGURACION DE TIEMPOS* al principio de `src/main.c`).

---

## 3. Decoder ChirpStack v4

Copia el contenido de [`tools/chirpstack_decoder.js`](../tools/chirpstack_decoder.js)
en **Device Profile → Codec → JavaScript functions**. Resumen de la salida:

```json
{
  "status": { "bm688": true, "ze15co": true, "ze15co_fault": false, "sen65": true },
  "bm688":  { "temperature_c": 26.68, "humidity_pct": 44.1, "pressure_hpa": 1008.9, "gas_resistance_ohm": 85000 },
  "ze15co": { "co_ppm": 0.5 },
  "sen65":  { "pm1p0_ugm3": 8.7, "pm2p5_ugm3": 9.1, "pm4p0_ugm3": 9.1, "pm10_ugm3": 9.1,
              "humidity_pct": 40.0, "temperature_c": 26.7, "voc_index": 56.0, "nox_index": 1.0 }
}
```

---

## 4. Alta en ChirpStack (resumen)
1. **Device profile**: LoRaWAN 1.0.x, región EU868, **ADR habilitado**, pega el
   codec JS de arriba.
2. **Device**: DevEUI = la que imprime el boot (`DevEUI(MAC)=`, p.ej.
   `1CDBD4FFFEBD2444`), JoinEUI `0000000000000000`.
3. **OTAA keys**: AppKey `062635ACC3BBC92C2FEF994F5EF0F69B`.
4. Encender el nodo → Join → llegan uplinks en FPort 2 decodificados.
