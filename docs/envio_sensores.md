# envio_sensores — mensaje LoRa: conformación, decodificación y tiempos

Documento práctico de dos cosas: **(1)** cómo se arma el mensaje que sale por
LoRa y **(2)** cómo se decodifica al llegar a ChirpStack; más un análisis de
**cada cuánto llega dato de cada sensor** para determinar el **tiempo mínimo**
entre envíos.

---

## 1. Cómo se conforma el mensaje

En `src/main.c` se rellena un `struct __packed` de **29 bytes** y se manda tal
cual: `lorawan_send(2, &payload, sizeof(payload), LORAWAN_MSG_UNCONFIRMED)`.

- **Codificación:** binaria, **little-endian** (byte bajo primero).
- **FPort:** 2. **Sin CRC de aplicación** (LoRaWAN ya protege con su MIC).
- **Escalas enteras:** cada magnitud se multiplica por un factor fijo para no
  enviar floats (ej. 27.03 °C → `2703`). El decoder divide por ese factor.
- Si un sensor no entregó dato ese ciclo, sus bytes van a **0**; el byte 0
  (flags) indica qué es válido.

| Offset | Tipo | Campo | Escala → unidad | Sensor |
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

**Total: 29 bytes** (cabe de sobra en EU868, máx. 51 B a DR0/SF12).

---

## 2. Cómo lo decodifico (ChirpStack)

ChirpStack muestra el uplink en HEX hasta que se le pega el decoder. Copia
[`tools/chirpstack_decoder.js`](../tools/chirpstack_decoder.js) en
**Device Profile → Codec → JavaScript functions** (`decodeUplink`).

Idea del decoder (little-endian):
```js
function u16(i){ return b[i] | (b[i+1]<<8); }            // 2 bytes sin signo
function s16(i){ var v=u16(i); return v>32767?v-65536:v; }// 2 bytes con signo
function u32(i){ return (b[i]|(b[i+1]<<8)|(b[i+2]<<16)|(b[i+3]<<24))>>>0; }
// luego cada campo: valor = uXX(offset) / escala
```

### Ejemplo real
Payload recibido (29 bytes):
```
0b 8f 0a 12 12 68 27 9c 8f 01 00 05 00 41 00 5d 00 72 00 7c 00 11 12 54 0a 88 04 0a 00
```
Decodificado:
```json
{
  "status": { "bm688": true, "ze15co": true, "ze15co_fault": false, "sen65": true },
  "bm688":  { "temperature_c": 27.03, "humidity_pct": 46.26, "pressure_hpa": 1008.8, "gas_resistance_ohm": 102300 },
  "ze15co": { "co_ppm": 0.5 },
  "sen65":  { "pm1p0_ugm3": 6.5, "pm2p5_ugm3": 9.3, "pm4p0_ugm3": 11.4, "pm10_ugm3": 12.4,
              "humidity_pct": 46.25, "temperature_c": 26.44, "voc_index": 116.0, "nox_index": 1.0 }
}
```
Comprobación de un campo: bytes `8f 0a` (offset 1–2) → LE `0x0A8F` = 2703 →
÷100 = **27.03 °C** ✔.

---

## 3. Cada cuánto llega dato de cada sensor → tiempo mínimo de envío

### 3.1 Ritmos del sistema
| Ritmo | Periodo | Constante |
|---|---|---|
| Lectura de los 3 sensores + refresco del portal | **5 s** | `SENSOR_PERIOD_S` |
| Envío por LoRa | **180 s** | `LORA_PERIOD_S` |

### 3.2 Muestreo de cada sensor (hacia el micro)
| Sensor | Muestra interna | Lo lee el firmware | Notas |
|---|---|---|---|
| **BM688** | on-demand (forced) | cada **5 s** | el heater de gas tarda ~150 ms/medición |
| **ZE15-CO** | cada **1 s** (active-upload) | cada **5 s** (Q&A) | preheat 30 s; respuesta ≤30 s |
| **SEN65** | cada **1 s** (continuous) | cada **5 s** | VOC/NOx **convergen ~1 min** tras encender (antes salen 0) |

> Conclusión parcial: **el firmware tiene dato fresco cada 5 s**. Por el lado de
> los sensores podrías enviar hasta cada 5 s. La única espera "de una vez" es el
> calentamiento de ~1 min para que VOC/NOx sean válidos al arrancar.

### 3.3 El límite real: duty-cycle de LoRaWAN (EU868, 1 %)
El tiempo mínimo entre envíos **no lo ponen los sensores, lo pone la radio**. En
EU868 cada sub-banda permite transmitir solo el **1 %** del tiempo, así que:

> **intervalo mínimo = tiempo_en_aire (ToA) ÷ 0.01 = ToA × 100**

El ToA depende del **Spreading Factor** (Data Rate). Para nuestro payload de
29 B (≈42 B en PHY: +13 B de cabecera LoRaWAN), BW125, CR 4/5:

| DR | SF | ToA aprox. | **Intervalo mínimo (1 %)** |
|---|---|---|---|
| DR5 | SF7 | ~87 ms | **~9 s** |
| DR4 | SF8 | ~154 ms | ~15 s |
| DR3 | SF9 | ~288 ms | ~29 s |
| DR2 | SF10 | ~534 ms | ~53 s |
| DR1 | SF11 | ~1.15 s | ~115 s |
| DR0 | SF12 | ~2.14 s | **~214 s** |

### 3.4 Conclusión — tiempo mínimo para enviar
- **Limitado por sensores:** ~5 s (y ~1 min de calentamiento inicial para VOC/NOx).
- **Limitado por el duty-cycle (lo que manda):**
  - A **SF12 (peor caso, máximo alcance):** ~**214 s**. Nuestro `LORA_PERIOD_S=180 s`
    queda *justo por debajo*; pinneado a SF12 podría dar algún `-111` (duty-cycle).
    Si vas a forzar SF12, pon **≥ 215 s** (p.ej. 240 s).
  - **Con ADR activo** (lo normal): el server baja el SF y el mínimo cae rápido —
    a SF9 ~29 s, a SF7 ~9 s. Ahí 180 s sobra y hasta podrías bajarlo.
- **Recomendación práctica:** dejar **180 s** con ADR (equilibrio alcance/consumo);
  si necesitas más frecuencia, espera a que ADR suba el DR y baja `LORA_PERIOD_S`
  vigilando que no aparezca `SEND ERR: -111`.

> Nota: los ToA son aproximados (dependen de cabecera, FOpts y DR exactos). Para
> el valor fino usa una calculadora de airtime LoRaWAN con 42 B de PHY payload.

---

## Anexo — Capacidad de dispositivos en el portal (no afecta al envío LoRa)
Se ampliaron los límites del portal cautivo: **pool DHCP a 10 IPs** y **4
clientes HTTP**. El SoftAP del ESP32-S3 topa en **10 estaciones**; el driver de
Zephyr trae `.max_connection = 5` *hardcodeado* (sin Kconfig), así que se parchea
a 10 en `zephyr/drivers/wifi/esp32/src/esp_wifi_drv.c`:

```c
/* zephyr/drivers/wifi/esp32/src/esp_wifi_drv.c, esp32_wifi_ap_enable() */
.max_connection = 10,   /* era 5 */
```
⚠️ Ese parche vive en el árbol de Zephyr (NO en este repo) y hay que
**re-aplicarlo tras `west update`**.
