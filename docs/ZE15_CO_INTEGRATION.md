# ZE15-CO Integration — T3-S3 V1.3 / Zephyr RTOS

Integración del módulo electroquímico de monóxido de carbono **Winsen ZE15-CO**
sobre la rama `feat/module_CO` (derivada de `feature/BM688`).
Datasheet: https://www.winsen-sensor.com/d/files/ZE15-CO.pdf (v1.1, 2018-04-12).

---

## 1. El sensor

| Parámetro | Valor |
|---|---|
| Gas | Monóxido de carbono (CO) |
| Rango / resolución | 0–500 ppm / 0.1 ppm |
| Alimentación | **5~12 V DC** (PIN15) — ⚠️ NO 3.3 V |
| Salida | UART (TTL **3 V**) + voltaje analógico (PIN10: 0.4–2 V ↔ 0–500 ppm) |
| UART | 9600 bps, 8 bits, 1 stop, sin paridad |
| Precalentamiento | 30 s (≥5 min en primer uso) |
| Tiempo de respuesta/recuperación | ≤30 s |

### Pines usados del módulo
| Pin sensor | Función | Conexión |
|---|---|---|
| PIN15 | Vin 5~12 V | Rail de 5 V (p.ej. VBUS USB del T3-S3) |
| PIN5 / PIN14 | GND | GND |
| PIN8 | UART TXD (3 V) | → ESP32 **GPIO40** (RX) |
| PIN7 | UART RXD (3 V) | ← ESP32 **GPIO41** (TX) |
| PIN10 | Salida analógica | (no usado) |
| PIN3 | Fault output (1 Hz, 10 % duty) | (no usado) |

> El nivel lógico del UART es 3 V → compatible directo con el GPIO del ESP32-S3,
> **sin level shifter**. Lo único que requiere conversión es la alimentación: el
> sensor necesita 5 V, no se alimenta del 3.3 V del MCU.

---

## 2. Decisiones de diseño

| Decisión | Elección | Motivo |
|---|---|---|
| Bus | **UART1** (GPIO40 RX / GPIO41 TX) | SPI2 ocupado por SX1262; I2C0 por BM688. UART1 vía GPIO matrix a pines libres. |
| Pines | GPIO40/41 | Libres en header lateral, sin strapping. Se evita 43/44 (UART0/ROM=printk) y 47/48 (I2C0/BM688). |
| Modo | **Q&A (pregunta/respuesta)** | Lectura determinista sincronizada con el ciclo de 30 s del uplink LoRaWAN. El sensor conmuta a Q&A al recibir la consulta. |
| API UART | Polling (`uart_poll_in/out`) | Una trama de 9 bytes a 9600 bps; no justifica IRQ ni ring buffer. |

`CONFIG_SERIAL=y` habilita el driver. La consola/printk **no** depende de él
(va por ROM UART/USB); `CONFIG_UART_CONSOLE`/`CONSOLE` siguen en `n`.

---

## 3. Protocolo (modo Q&A)

**Comando de consulta** (MCU → sensor), 9 bytes:
```
FF 01 86 00 00 00 00 00 79
```

**Respuesta** (sensor → MCU), 9 bytes:
```
Byte0  0xFF  start
Byte1  0x86  command
Byte2  concentración (high byte) — bit7 = fallo de sensor
Byte3  concentración (low byte)
Byte4  reservado
Byte5  reservado
Byte6  concentración (high) [repetido]
Byte7  concentración (low)  [repetido]
Byte8  checksum
```

**Concentración:** `CO_ppm = ((high & 0x1F) * 256 + low) * 0.1`
**Fallo de sensor:** `(high & 0x80) != 0`

**Checksum** (complemento a 2 de bytes 1..7):
```
check = (~(byte1 + byte2 + ... + byte7)) + 1
```

> Nota del datasheet: en modo Q&A el módulo vuelve a *active upload* automático si
> no recibe consulta en 30 s. Como se le consulta cada 30 s, se mantiene en Q&A;
> el wrapper además vacía el RX antes de cada consulta para descartar tramas
> residuales del modo upload, y resincroniza al start byte `0xFF`.

---

## 4. Estructura de código

| Archivo | Rol |
|---|---|
| `include/sensors/ze15_co.h` | API del wrapper (`ze15co_init`, `ze15co_read`, `struct ze15co_data`) |
| `src/sensors/ze15_co.c` | Implementación Q&A sobre `uart_poll_*` + checksum |
| `boards/...overlay` | pinctrl `uart1_co` (GPIO40/41) + `&uart1` okay 9600 + alias `co-uart` |
| `prj.conf` | `CONFIG_SERIAL=y` |
| `src/main.c` | init del CO + lectura en el loop + campo en el payload |

---

## 5. Payload LoRaWAN (ampliado a 14 bytes, little-endian)

| Offset | Tipo | Campo | Ejemplo |
|---|---|---|---|
| [0-1] | int16 | temperatura × 100 | 2759 = 27.59 °C |
| [2-3] | uint16 | humedad × 100 | 5335 = 53.35 % |
| [4-7] | uint32 | presión (Pa) | 100873 |
| [8-11] | uint32 | gas resistance (Ω) | 13007335 |
| **[12-13]** | **uint16** | **CO ppm × 10** | **125 = 12.5 ppm** (0 si no hay sensor/fault) |

⚠️ El decoder de ChirpStack debe ampliarse para leer los 2 bytes nuevos [12-13].

---

## 6. Validación esperada (tras cablear y flashear)

Arranque:
```
ZE15-CO listo: uart@... (o similar)
ZE15-CO READY
```
En el loop (tras ~30 s de precalentamiento):
```
ZE15-CO CO=0.0ppm
```
soplando CO / en presencia de gas, el valor debe subir. En aire limpio ronda 0.

Fallos posibles y su significado:
- `ZE15-CO INIT ERR: -19` → UART1 no listo (revisa overlay/Kconfig).
- `ZE15-CO READ ERR: -116` → timeout: sin respuesta (revisa cableado TX/RX cruzado,
  alimentación 5 V, GND común).
- `ZE15-CO READ ERR: -5` → cabecera/checksum inválidos (ruido, baudrate, o TX/RX invertidos).
- `ZE15-CO READ ERR: 0 (fault)` → el sensor reporta fallo interno (bit7).

---

## 7. Pendiente

- Validación en hardware (requiere el sensor cableado con rail de 5 V).
- Ampliar el decoder de ChirpStack con el campo CO [12-13].
