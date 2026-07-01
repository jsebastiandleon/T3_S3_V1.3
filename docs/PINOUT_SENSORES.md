# Pinout — Asignación de pines sensores ↔ ESP32-S3 (T3-S3 V1.3)

Tabla de referencia del cableado de los 3 sensores al ESP32-S3. Todo según el
overlay `boards/esp32s3_devkitc_esp32s3_procpu.overlay`.

---

## 1. Tabla resumen (lo que se cablea)

| Sensor | Señal | Pin del sensor | **GPIO ESP32-S3** | Bus | Dir./Notas |
|---|---|---|---|---|---|
| **BM688** (BME688) | VDD | VDD | **3V3** | — | 3.3 V |
| | GND | GND | **GND** | — | masa común |
| | SDA | SDA | **GPIO47** | I2C0 | dir. **0x77** |
| | SCL | SCL | **GPIO48** | I2C0 | 100 kHz |
| **ZE15-CO** (Winsen) | Vin | PIN15 | **5V** | — | ⚠️ **5–12 V** (no 3.3) |
| | GND | PIN5 / PIN14 | **GND** | — | masa común |
| | RXD (sensor) | PIN7 | **GPIO41** (ESP **TX**) | UART1 | TTL 3 V |
| | TXD (sensor) | PIN8 | **GPIO40** (ESP **RX**) | UART1 | 9600 8N1 |
| **SEN65** (Sensirion) | VDD | PIN1 / PIN6 | **3V3** | — | 3.3 V (~200 mA) |
| | GND | PIN2 / PIN5 | **GND** | — | masa común |
| | SDA | PIN3 | **GPIO15** | I2C1 | dir. **0x6B** |
| | SCL | PIN4 | **GPIO16** | I2C1 | 100 kHz |
| **Anemómetro** (RS485) | +V | rojo | **5–24 V ext.** | — | ⚠️ fuente externa (no la placa) |
| | GND | negro | **GND** | — | masa común con ESP32 |
| | RS485 A | amarillo | **A del adaptador** | RS485 | Modbus RTU, esclavo **0x01** |
| | RS485 B | verde | **B del adaptador** | RS485 | 4800 8N1 (por defecto) |
| Adaptador RS485↔TTL | DI | — | **GPIO39** (ESP **TX**) | UART2 | lógica TTL 3.3 V |
| | RO | — | **GPIO38** (ESP **RX**) | UART2 | |
| | DE+RE | — | **GPIO42** | — | dirección half-duplex (opcional) |
| | VCC / GND | — | **3V3 / GND** | — | alimentar el adaptador a 3.3 V |

> Regla de oro: **todas las masas (GND) en común** con el ESP32-S3. El ZE15-CO se
> alimenta a 5 V pero su GND debe unirse al GND de la placa igualmente.

---

## 2. Detalle por bus

| Bus | Pines ESP32 | Velocidad | Dispositivos | Pull-ups |
|---|---|---|---|---|
| **I2C0** | SDA=GPIO47, SCL=GPIO48 | 100 kHz | BM688 (0x77) | internos del ESP32 (`bias-pull-up`) |
| **I2C1** | SDA=GPIO15, SCL=GPIO16 | 100 kHz | SEN65 (0x6B) | internos del ESP32 (`bias-pull-up`) |
| **UART1** | TX=GPIO41, RX=GPIO40 | 9600 8N1 | ZE15-CO | — |
| **UART2** | TX=GPIO39, RX=GPIO38 | 4800 8N1 | Anemómetro (RS485/Modbus, vía adaptador) | — |

- BM688 y SEN65 están en **buses I2C separados** (no comparten pines).
- I2C cruzado: **ESP TX→sensor RXD** y **ESP RX←sensor TXD** (UART siempre cruzado).
- Pull-ups: se usan los **internos** del ESP32-S3 (suficientes para cable corto a
  100 kHz). Si el cable es largo o hay errores de CRC, añadir 4.7 kΩ externos a 3V3.

---

## 3. Alimentación

| Riel | Sensores | Notas |
|---|---|---|
| **3.3 V** | BM688, SEN65, adaptador RS485 | SEN65 pide ~200 mA (ventilador + láser); el adaptador RS485↔TTL a 3.3 V para no meter 5 V al RX |
| **5 V** | ZE15-CO | 5–12 V DC; **VDD del SEN65/BM688 NUNCA a 5 V** |
| **5–24 V ext.** | Anemómetro | ⚠️ **fuente externa** (la placa no da este riel); rojo=+, negro=GND |
| **GND** | los 4 | masa común obligatoria (incluida la fuente externa del anemómetro) |

Las líneas de señal (SDA/SCL/UART) son **3 V**; el ZE15-CO tolera 3 V en su UART
aunque se alimente a 5 V (no necesita level shifter).

---

## 4. Ubicación física en la placa (T3-S3 V1.3)

Según el pinout serigrafiado (`docs/T3S3.png`):

- **Header IZQUIERDO** (de arriba abajo): GPIO16, GPIO15, RST, GPIO18, **GPIO48,
  GPIO47**, GPIO33, GPIO34, GPIO35, GPIO36, GPIO37, GND.
  → Aquí están **SEN65 (15/16)** y **BM688 (47/48)**.
- **Header DERECHO**: GPIO38, GPIO43, GPIO44, GPIO39, **GPIO40, GPIO41**, GPIO45,
  GPIO46, GPIO42, GND, **3.3V, 5V**, VBAT.
  → Aquí están **ZE15-CO UART (40/41)** y los rieles **3.3V / 5V / GND**.

> Otros pines del board: OLED I2C interno en GPIO17(SCL)/18(SDA); QWIIC en
> GPIO10/21; USB-Serial/JTAG (consola/printk) en GPIO19/20.

---

## 5. Mapa completo de GPIOs usados (incl. radio onboard)

| GPIO | Uso | Periférico |
|---|---|---|
| 3 | MISO | SX1262 (LoRa, SPI2) — onboard |
| 5 | SCLK | SX1262 (SPI2) |
| 6 | MOSI | SX1262 (SPI2) |
| 7 | CS | SX1262 |
| 8 | RESET | SX1262 |
| 33 | DIO1 | SX1262 (IRQ) |
| 34 | BUSY | SX1262 |
| 15 | **SDA** | **SEN65 (I2C1)** |
| 16 | **SCL** | **SEN65 (I2C1)** |
| 40 | **RX** | **ZE15-CO (UART1)** |
| 41 | **TX** | **ZE15-CO (UART1)** |
| 38 | **RX** | **Anemómetro (UART2, RS485)** |
| 39 | **TX** | **Anemómetro (UART2, RS485)** |
| 42 | **DE/RE** | **Anemómetro (dirección RS485, opcional)** |
| 47 | **SDA** | **BM688 (I2C0)** |
| 48 | **SCL** | **BM688 (I2C0)** |
| 19/20 | USB D-/D+ | consola/printk (USB-Serial/JTAG) |

> El SX1262 (radio LoRa) va **soldado en la placa**; no se cablea. Solo se cablean
> los 3 sensores de la tabla 1.

---

## 6. Diagramas rápidos de conexión

```
BM688            ESP32-S3            SEN65
 VDD ── 3V3        3V3 ── VDD(1/6)
 GND ── GND        GND ── GND(2/5)
 SDA ── GPIO47    GPIO15 ── SDA(3)
 SCL ── GPIO48    GPIO16 ── SCL(4)
   (I2C0 @0x77)      (I2C1 @0x6B)

ZE15-CO                 ESP32-S3
 Vin(PIN15) ── 5V
 GND(5/14)  ── GND
 RXD(PIN7)  ── GPIO41 (TX)
 TXD(PIN8)  ── GPIO40 (RX)
   (UART1 9600 8N1)

Anemómetro     Adaptador RS485↔TTL      ESP32-S3
 rojo  ── +5..24V (fuente ext.)
 negro ── GND ─────────────────────────── GND (masa común)
 amaril(A) ── A
 verde (B) ── B
                DI ───────────────────── GPIO39 (TX, UART2)
                RO ───────────────────── GPIO38 (RX, UART2)
                DE+RE ────────────────── GPIO42 (opcional)
                VCC/GND ──────────────── 3V3 / GND
   (RS485 Modbus RTU, esclavo 0x01, 4800 8N1)
```

*Relacionado: `docs/BM688_INTEGRATION.md`, `docs/ZE15_CO_INTEGRATION.md`,
`docs/SEN65_INTEGRATION.md`, `docs/T3S3.png` (pinout de la placa).*
