# Integración del anemómetro (velocidad + dirección de viento) — RS485 / Modbus RTU

4º sensor del nodo T3-S3: **anemómetro integrado de fibra de carbono** que mide
**velocidad** (0–60 m/s) y **dirección** (0–360°) del viento. Salida **RS485 con
protocolo Modbus RTU**. Rama `feature/anemometro`.

---

## 1. Hardware

### El sensor
Según la etiqueta y la hoja del proveedor:
- **Salida:** RS485 (Modbus RTU estándar, **dirección de esclavo `01`** por defecto).
- **Alimentación (rojo +, negro GND):** **5–24 V DC** con salida RS485.
- **Cables:** rojo = +V · negro = GND · **amarillo = RS485 A** · **verde = RS485 B**.
- **Calefacción (marrón/blanco):** 12–24 V, 15 W (anti-hielo/condensación). **Opcional, no se cablea.**
- Velocidad de arranque ≤0.3 m/s · precisión ±(0.3+0.03V) m/s y ±1°.

### El adaptador RS485↔TTL
El ESP32-S3 **no** tiene transceptor RS485; se usa un módulo adaptador
(MAX485/MAX3485 o similar) entre el sensor y UART2:

```
Anemómetro     Adaptador RS485↔TTL      ESP32-S3
 rojo  ── +5..24V (fuente externa)
 negro ── GND ─────────────────────────── GND (masa común)
 amaril(A) ── A
 verde (B) ── B
                DI ───────────────────── GPIO39 (TX, UART2)
                RO ───────────────────── GPIO38 (RX, UART2)
                DE+RE ────────────────── GPIO42 (opcional)
                VCC/GND ──────────────── 3V3 / GND
```

> ⚠️ **Alimentar el adaptador a 3.3 V** (no a 5 V) para que su lógica TTL no meta
> 5 V en el RX del ESP32-S3. La **masa** del sensor, del adaptador y de la fuente
> externa **deben unirse** a la del ESP32.
>
> ⚠️ El sensor pide **5–24 V**: la placa T3-S3 **no** da ese riel → **fuente
> externa** (una de 12 V vale).

### Control de dirección (half-duplex)
RS485 es half-duplex: el pin **DE/RE** del adaptador decide si transmite o recibe.
- Adaptador **con DE/RE**: se cablea a **GPIO42**; el driver lo pone alto durante
  el envío de la petición y bajo el resto del tiempo.
- Adaptador de **conmutación automática** (sin pin DE/RE): **no cablear GPIO42** y
  borrar la línea `wind-de-gpios` del `zephyr,user` en el overlay. El driver lo
  detecta ausente y asume modo automático.

---

## 2. Software

| Pieza | Fichero |
|---|---|
| Driver (Modbus RTU manual sobre UART) | `src/sensors/anemometer.c` · `include/sensors/anemometer.h` |
| UART2 + pinctrl + DE/RE + alias `wind-uart` | `boards/esp32s3_devkitc_esp32s3_procpu.overlay` |
| Init + lectura + payload | `src/main.c` |
| Snapshot al portal | `include/portal/portal.h`, `src/portal/http_routes.c` |
| Decoder ChirpStack | `tools/chirpstack_decoder.js` |

El driver implementa **Modbus RTU a mano** (`uart_poll_in/out` + CRC-16/Modbus),
igual estilo que el ZE15-CO, para no arrastrar el subsistema `CONFIG_MODBUS` (que
tomaría el control exclusivo del UART). Solo hace una lectura de registros
*holding* (función `0x03`) por ciclo, con hasta 3 reintentos.

---

## 3. Mapa Modbus (POR DEFECTO — ajustar tras probar el sensor real)

El proveedor **no** entrega la tabla de registros. Se usa el mapa más habitual de
estos sensores integrados; si el sensor real difiere, tocar **solo** las
constantes al principio de `src/sensors/anemometer.c` (y `current-speed` del
`&uart2` en el overlay para el baudrate):

| Parámetro | Valor por defecto | Constante |
|---|---|---|
| Baudrate | **4800** 8N1 (muchos vienen a 9600) | `WIND_BAUDRATE` + `current-speed` overlay |
| Dirección esclavo | **0x01** (confirmado por la spec) | `WIND_SLAVE_ADDR` |
| Función | **0x03** (read holding registers) | `WIND_FUNC_READ` |
| Registro inicial | **0x0000** | `WIND_REG_START` |
| Nº registros | **2** | `WIND_REG_COUNT` |
| Velocidad | `reg[0] × 0.1` → m/s | `WIND_SPEED_REG_IDX`, `WIND_SPEED_SCALE` |
| Dirección | `reg[1]` → grados (0–359) | `WIND_DIR_REG_IDX` |

**Variante alternativa común:** dirección como "rumbo" 0–7 en `reg[1]` y grados en
`reg[2]` → subir `WIND_REG_COUNT` a 3 y `WIND_DIR_REG_IDX` a 2.

### Cómo verificar en banco
1. Alimentar el sensor (5–24 V) y conectar A/B al adaptador.
2. Encender el nodo y mirar el log: `Anemometro listo` y luego `Viento: X.X m/s, dir N deg`.
3. Si sale **timeout**: casi siempre es **baudrate** (probar 9600) o **A/B invertidos** (intercambiar amarillo/verde).
4. Si sale **valor absurdo**: el **registro/escala** no coincide → ajustar las constantes de la tabla.

---

## 4. Payload y decoder

Payload **v3**, 33 bytes (antes 29). Se añaden 4 bytes al final y el **bit4** de
flags = anemómetro OK:

| Offset | Tipo | Campo | Escala |
|---|---|---|---|
| 29–30 | uint16 | Velocidad viento | ÷10 → m/s |
| 31–32 | uint16 | Dirección viento | → ° (0–359) |

Salida del decoder:
```json
"status": { ..., "anemometer": true },
"anemometer": { "wind_speed_ms": 5.2, "wind_dir_deg": 180 }
```

Ver el formato completo en `docs/PAYLOAD_DECODER.md` y el pinout en
`docs/PINOUT_SENSORES.md`.
