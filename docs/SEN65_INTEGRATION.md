# SEN65 Integration — T3-S3 V1.3 / Zephyr RTOS

> 🔌 Cableado y pines: ver [PINOUT_SENSORES.md](PINOUT_SENSORES.md).

Integración del nodo de calidad de aire **Sensirion SEN65** (familia SEN6x)
sobre la rama `feature/Sensirion`.
Datasheet: https://www.farnell.com/datasheets/4601635.pdf (SEN6x, v0.91, ago-2025).

Como **Zephyr no incluye driver nativo** para la familia SEN6x en este árbol
(solo shtcx, scd4x, sht3xd, sts4x, sgp40, sht4x), la integración es a nivel de
aplicación, igual que el ZE15-CO.

---

## 1. El sensor

| Parámetro | Valor |
|---|---|
| Señales | PM1.0/2.5/4.0/10.0, RH, T, índice VOC, índice NOx |
| Alimentación (VDD) | **3.3 V** (3.15–3.45 V) — ⚠️ NO 5 V |
| Pico de corriente | ~200 mA (lleva ventilador + láser) |
| Interfaz | **I2C1** (bus propio), 100 kbit/s (standard mode), sin clock-stretching |
| Dirección I2C | **0x6B** (7-bit) |
| Niveles SDA/SCL | TTL compatibles 5 V (funcionan a 3.3 V) |
| Formato de datos | palabras de 16 bits MSB-first + CRC-8 por palabra |
| Convergencia | ~1.1 s primer dato; VOC/NOx ~10–11 s extra |

### Pines del conector (6 pines: ACES 51468-0064N-001; lado cable JST GHR-06V-S)

| Pin SEN65 | Función | Conexión (T3-S3) | Nota |
|---|---|---|---|
| **1** | VDD | **3V3** | 3.15–3.45 V (⚠️ no 5 V) |
| **2** | GND | **GND** | masa común |
| **3** | SDA | **GPIO15** | I2C1 (bus propio) |
| **4** | SCL | **GPIO16** | I2C1 (bus propio) |
| 5 | GND | — (NC) | unido internamente al pin 2 |
| 6 | VDD | — (NC) | unido internamente al pin 1 |

> Solo se cablean los pines 1–4. Internamente pin1≡pin6 y pin2≡pin5.

### Diagrama

```
   SEN65 (conector 6 pines)              T3-S3 V1.3
 ┌──────────────────────────┐
 │ 1 VDD ───────────────────┼──────────► 3V3
 │ 2 GND ───────────────────┼──────────► GND
 │ 3 SDA ───────────────────┼────┬─────► GPIO15  (I2C1 SDA)
 │ 4 SCL ───────────────────┼──┬─┼─────► GPIO16  (I2C1 SCL)
 │ 5 GND  (NC)              │  │ │
 │ 6 VDD  (NC)              │  │ │
 └──────────────────────────┘  │ │
                               4k7 4k7   ◄── OPCIONALES (a 3V3). El overlay
                                │ │          activa los pull-ups INTERNOS del
                               3V3 3V3        ESP32; bastan con cable corto.
```

> ⚠️ **Bus propio I2C1, separado del BME688** (que está en I2C0/GPIO47-48). El
> pinctrl `i2c1_sen65` del overlay declara `bias-pull-up`, así que el bus usa los
> **pull-ups internos** del ESP32-S3 (~45 kΩ): verificados suficientes a 100 kHz
> con cable corto. Añade **4.7k externos a 3V3** solo si alargas el cable, si
> cuelgas más dispositivos del bus o si aparecen errores de CRC.
> Las líneas SDA/SCL toleran 5 V, pero VDD es 3.3 V.
> Mantén el cable I2C corto (< 10 cm) y/o blindado para evitar errores de CRC.
> Si el regulador 3V3 de la placa no cubre el pico de ~200 mA junto con
> WiFi+LoRa, alimenta el SEN65 desde una fuente 3V3 externa **con GND común**.

---

## 2. Decisiones de diseño

| Decisión | Elección | Motivo |
|---|---|---|
| Bus | **I2C1 propio** (GPIO15 SDA / GPIO16 SCL) | A petición: el SEN65 NO comparte pines con el BME688 (que sigue en I2C0/47-48). Segundo controlador I2C del ESP32-S3, libre. |
| Pines | GPIO15/16 | Libres en este proyecto, sin función de strapping; enrutados a I2C1 por la GPIO matrix. |
| Pinctrl | `i2c1_sen65` sobrescribe `i2c1_default` | El board enruta I2C1 a GPIO4/5 por defecto; se reasigna a 15/16. |
| Pull-ups | **Internos del ESP32** (`bias-pull-up` en el overlay) | Suficientes a 100 kHz con cable corto. 4.7k externos solo si alargas el cable, añades dispositivos o ves errores de CRC. |
| Velocidad | 100 kHz (heredada) | Es exactamente el máximo del SEN6x (standard mode). |
| Driver | Wrapper de aplicación en C | No hay driver SEN6x nativo en Zephyr; se usa la API `i2c_*` directa. |
| Resolución bus+dir | `I2C_DT_SPEC_GET` sobre nodo DT | Idiomático; el nodo del overlay aporta bus y `reg`. **Al estar `sen65` bajo `&i2c1`, el driver toma el bus correcto sin cambios en C.** |

`CONFIG_I2C=y` ya estaba activo (lo arrastra el BME688); habilita ambos
controladores. No se añadió ningún Kconfig nuevo.

---

## 3. Protocolo (I2C, según datasheet SEN6x)

- Los **comandos** son IDs de 16 bits (MSB-first) y **no** llevan CRC.
- Los **datos** van en palabras de 16 bits (MSB-first), cada una seguida de su
  **CRC-8** (Dallas/Maxim, poly `0x31`, init `0xFF`, sin reflexión, XOR final 0).
  Comprobación del datasheet: `CRC(0xBEEF) = 0x92` ✔.
- Tras un comando de lectura hay que esperar su tiempo de ejecución antes del
  header de lectura (el sensor NACKea mientras procesa).

**Comandos usados:**

| Comando | ID | Exec (ms) | Uso |
|---|---|---|---|
| Device Reset SEN6x | `0xD304` | ~1200 (reinit) | estado conocido (idle) en `init` |
| Start Continuous Measurement | `0x0021` | 50 | arranca la medición |
| Get Data Ready SEN6x | `0x0202` | 20 | comprueba dato nuevo (byte1 = 0x01) |
| Read Measured Values SEN65 | `0x0446` | 20 | 8 palabras (24 bytes con CRC) |

**`Read Measured Values SEN65` (0x0446) — 8 palabras:**

| # | Señal | Tipo | Escala | Unidad | Centinela "desconocido" |
|---|---|---|---|---|---|
| 0 | PM1.0 | uint16 | /10 | µg/m³ | 0xFFFF |
| 1 | PM2.5 | uint16 | /10 | µg/m³ | 0xFFFF |
| 2 | PM4.0 | uint16 | /10 | µg/m³ | 0xFFFF |
| 3 | PM10.0 | uint16 | /10 | µg/m³ | 0xFFFF |
| 4 | Humedad | int16 | /100 | %RH | 0x7FFF |
| 5 | Temperatura | int16 | /200 | °C | 0x7FFF |
| 6 | Índice VOC | int16 | /10 | — | 0x7FFF |
| 7 | Índice NOx | int16 | /10 | — | 0x7FFF |

> El wrapper mapea cualquier valor centinela a `0.0` para no propagar valores
> falsos. Si "Get Data Ready" indica que aún no hay dato nuevo, el sensor
> conserva la última lectura, así que la llamada **no falla** por ese motivo.

---

## 4. Estructura de código

| Archivo | Rol |
|---|---|
| `include/sensors/sen6x.h` | API del wrapper (`sen6x_init`, `sen6x_read`, `struct sen6x_data`) |
| `src/sensors/sen6x.c` | Implementación I2C: CRC-8, comandos, lectura y escalado |
| `dts/bindings/sensor/sensirion,sen65.yaml` | Binding mínimo (autodescubierto) para el `compatible` |
| `boards/...overlay` | nodo `sen65@6b` en `&i2c0` + alias `sen65-sensor` |
| `CMakeLists.txt` | `+ src/sensors/sen6x.c` |
| `src/main.c` | init del SEN65 + lectura en el loop + campos en payload/portal |
| `include/portal/portal.h`, `src/portal/http_routes.c` | campos y claves JSON nuevas |

### Device Tree (overlay extendido, no duplicado)

```dts
&pinctrl {
    i2c1_sen65: i2c1_sen65 {
        group1 {
            pinmux = <I2C1_SDA_GPIO15>, <I2C1_SCL_GPIO16>;
            bias-pull-up;
            drive-open-drain;
            output-high;
        };
    };
};

&i2c1 {
    status = "okay";
    pinctrl-0 = <&i2c1_sen65>;
    pinctrl-names = "default";

    sen65: sen65@6b {
        compatible = "sensirion,sen65";
        reg = <0x6b>;
    };
};
/* en aliases: */
sen65-sensor = &sen65;
```

El SEN65 va en su propio controlador `&i2c1` (no toca el BME688 en `&i2c0`). El
binding local resuelve el `compatible` sin warnings; Zephyr añade
`<app>/dts/bindings` automáticamente a la ruta de búsqueda.

---

## 5. Payload LoRaWAN (ampliado de 14 a 26 bytes, little-endian)

Los bytes **[0-13] son idénticos** a la versión previa (BM688 + CO): el decoder
existente sigue válido; el SEN65 solo **añade** campos al final.

| Offset | Tipo | Campo | Origen |
|---|---|---|---|
| [0-1] | int16 | temperatura × 100 | BM688 |
| [2-3] | uint16 | humedad × 100 | BM688 |
| [4-7] | uint32 | presión (Pa) | BM688 |
| [8-11] | uint32 | gas resistance (Ω) | BM688 |
| [12-13] | uint16 | CO ppm × 10 | ZE15-CO |
| **[14-15]** | **uint16** | **PM1.0 × 10** (µg/m³) | **SEN65** |
| **[16-17]** | **uint16** | **PM2.5 × 10** (µg/m³) | **SEN65** |
| **[18-19]** | **uint16** | **PM4.0 × 10** (µg/m³) | **SEN65** |
| **[20-21]** | **uint16** | **PM10.0 × 10** (µg/m³) | **SEN65** |
| **[22-23]** | **int16** | **índice VOC × 10** | **SEN65** |
| **[24-25]** | **int16** | **índice NOx × 10** | **SEN65** |

(0 en los campos SEN65 si no hay sensor presente.)

### Portal cautivo `/api/sensors`

Claves JSON añadidas: `sen65` (bool), `pm1_0`, `pm2_5`, `pm4_0`, `pm10_0`
(µg/m³, 1 decimal), `voc`, `nox` (índices enteros).

⚠️ El **decoder de ChirpStack** debe ampliarse para leer los 12 bytes nuevos
[14-25]. El **HTML por defecto** del portal aún no muestra estos campos (el JSON
ya los expone; se puede actualizar el HTML por downlink LoRa, FPort 10).

---

## 6. Validación esperada (tras cablear y flashear)

El `i2c0_scan()` de `main.c` solo recorre **I2C0**, así que el SEN65 (ahora en
**I2C1**) **no** aparece ahí — solo el BME688:
```
I2C0 SCAN (100kHz, SDA=47 SCL=48):
  ACK 0x77        ← BME688 (el SEN65 está en I2C1, no se lista aquí)
I2C0 SCAN done: 1 dispositivo(s)
```

La presencia del SEN65 se confirma con su propio log de arranque (el `init`
hace un Device Reset que NACKea si no hay sensor en el bus I2C1):
```
SEN65 listo en I2C 0x6b (bus i2c@60027000)
SEN65 READY
```

En el loop (VOC/NOx pueden tardar ~10 s en converger; antes salen 0):
```
SEN65 PM2.5=12.5 ug/m3 VOC=100 NOx=1 RH=53% T=27C
```

Fallos posibles y su significado:
- `SEN65 INIT ERR: -19` → bus I2C1 no listo (revisa overlay/Kconfig).
- `SEN65 INIT ERR: -5`, con `E: SEN65: no responde en 0x6b ...: -14` justo antes
  → **el bus está bien; el problema está del cable para allá.** El `-14`
  (`EFAULT`) es un **NACK limpio**: el driver `i2c_esp32` solo lo devuelve en
  `I2C_STATUS_ACK_ERROR`, lo que significa que sacó los 9 pulsos de SCL y vio
  SDA **en alto** durante el bit de ACK. Si faltaran pull-ups o hubiera un corto
  saldría `-116` (`ETIMEDOUT`), no `-14`. Revisa, por este orden:
  **1)** VDD en el conector del sensor = 3.15–3.45 V (⚠️ el ZE15-CO comparte
  nodo y va a 5–12 V: equivocar el raíl puede dañar el SEN65, cuyo máximo
  absoluto es 3.45 V); **2)** continuidad pin 3→GPIO15 y pin 4→GPIO16, que de
  paso descarta el par cruzado y el cable suelto; **3)** GND común si lo
  alimentas de una fuente aparte.
  *No te fíes del ventilador:* solo arranca tras `Start Continuous Measurement`,
  que no llegamos a enviar si el init falla, así que que no gire no prueba nada.
- `sen6x_init()` se llama **una sola vez, en el arranque**. Si falla, el sensor
  queda descartado hasta el siguiente reinicio: arreglar el cableado en caliente
  no lo recupera.
- `SEN65 READ ERR: -5` → CRC inválido en la respuesta (ruido / cable I2C largo /
  pull-ups demasiado débiles).

---

## 7. Pendiente

- Validación en hardware (requiere el sensor cableado a 3V3).
- Ampliar el decoder de ChirpStack con los campos SEN65 [14-25].
- Mostrar PM/VOC/NOx en el HTML del portal (opcional, vía downlink FPort 10).
