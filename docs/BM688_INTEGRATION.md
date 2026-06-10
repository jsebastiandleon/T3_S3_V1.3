# BM688 Integration — T3-S3 V1.3 / Zephyr RTOS

## 1. Descripción del módulo BM688

El **Bosch BM688** es un sensor ambiental 4-en-1 que mide:

| Canal | Unidad | Rango típico |
|---|---|---|
| Temperatura | °C | −40 … +85 |
| Presión barométrica | Pa | 300 … 1100 hPa |
| Humedad relativa | %RH | 0 … 100 |
| Resistencia de gas (VOC/IAQ) | Ω | 1 kΩ … 100 MΩ |

Es hardware-compatible con el BME680 (mismos registros, mismo protocolo).
El driver `bosch,bme680` de Zephyr soporta ambos chips sin modificación.

---

## 2. Decisión de diseño: I2C vs SPI

### Contexto del proyecto

El proyecto ya ocupa **SPI2** con el SX1262 (LoRa):

| SPI2 | GPIO |
|---|---|
| MISO | GPIO3 |
| SCLK | GPIO5 |
| MOSI | GPIO6 |
| CS (SX1262) | GPIO7 |
| RESET (SX1262) | GPIO8 |
| DIO1 (SX1262) | GPIO33 |
| BUSY (SX1262) | GPIO34 |

### Decisión: I2C

| Criterio | I2C | SPI (SPI3) |
|---|---|---|
| Pines necesarios | 2 (SDA+SCL) | 4 (MOSI+MISO+SCLK+CS) |
| Conflicto con SPI2 | Ninguno | Requiere nuevo pinctrl |
| Pinctrl ya definido | Sí (`i2c0_default` en board dtsi) | No |
| Cambio en overlay | Mínimo (solo `status="okay"`) | Requiere grupo pinctrl nuevo |
| Velocidad | 100–400 kHz (suficiente para sensor 1 Hz) | 10 MHz (sobredimensionado) |

**Conclusión:** I2C es la opción correcta. Menos pines, menos overhead de
overlay, y el pinctrl `i2c0_default` ya existe en `esp32s3_devkitc-pinctrl.dtsi`.

---

## 3. Mapa de GPIOs del T3-S3 V1.3 (análisis completo)

| GPIO | Función en placa | Libre |
|---|---|---|
| 0 | Botón BOOT | ❌ |
| 1 | Battery ADC | ❌ |
| 2 | SD MISO | ❌ |
| 3 | LoRa MISO | ❌ |
| 5 | LoRa SCK | ❌ |
| 6 | LoRa MOSI | ❌ |
| 7 | LoRa CS | ❌ |
| 8 | LoRa RESET | ❌ |
| 10 | QWIIC IO10 → SX1262 DIO3 | ⚠️ requiere quitar resistor |
| 11 | SD MOSI | ❌ |
| 13 | SD CS | ❌ |
| 14 | SD SCK | ❌ |
| 17 | SCL OLED (no sale a header) | ❌ |
| 18 | SDA OLED (no sale a header) | ❌ |
| 21 | QWIIC IO21 → SX1262 DIO4 | ⚠️ requiere quitar resistor |
| 33 | LoRa DIO1 | ❌ |
| 34 | LoRa BUSY | ❌ |
| 37 | LED onboard | ❌ |
| **43** | **QWIIC UART1 TX** | **✅ libre** |
| **44** | **QWIIC UART1 RX** | **✅ libre** |

GPIO43 y GPIO44 son los **únicos GPIOs libres** sin modificar hardware.
Salen físicamente por el conector **QWIIC (JST1.0)** de la placa.

## 4. Tabla de pines

### Conexión física BM688 ↔ T3-S3 V1.3

| Pin BM688 | Pin ESP32-S3 | Ubicación física | Notas |
|---|---|---|---|
| VCC | 3.3 V | Header 3V3 | No conectar a 5 V |
| GND | GND | Header GND | — |
| SDA | **GPIO47** | Header lateral JP1 | Requiere pull-up externo |
| SCL | **GPIO48** | Header lateral JP1 | Requiere pull-up externo |
| SD0 | **GND** | Header GND | GND → dirección 0x76 |
| CS | **VCC** | Header 3V3 | **Obligatorio** a VCC; no dejar flotante |

> **Pull-ups OBLIGATORIOS:** al estar en pines de header pelado (no en el
> QWIIC, que los traía integrados), debes añadir resistores externos de
> **4.7 kΩ** entre SDA→3.3V y SCL→3.3V. El `bias-pull-up` interno
> (~45 kΩ) es demasiado débil para un bus I2C fiable por sí solo.
>
> **Histórico:** la primera implementación usó I2C1 en GPIO43/44 (conector
> QWIIC). Se reasignó a I2C0/GPIO47-48 para liberar el QWIIC (módulos
> plug-and-play) y recuperar UART0.

---

## 5. Modificaciones al overlay existente

Archivo: `boards/esp32s3_devkitc_esp32s3_procpu.overlay`

Se agregaron tres bloques. **No se modificó ningún nodo existente**
(SPI2, SX1262, flash0, psram0).

```dts
/* 1. Nuevo pinctrl para I2C0 en pines de header libres (GPIO47/48).
      Sobrescribe el i2c0_default del board (GPIO1/2, ocupados). */
&pinctrl {
    i2c0_bm688: i2c0_bm688 {
        group1 {
            pinmux = <I2C0_SDA_GPIO47>,
                     <I2C0_SCL_GPIO48>;
            bias-pull-up;
            drive-open-drain;
            output-high;
        };
    };
};

/* 2. Habilitar I2C0 con BM688 */
&i2c0 {
    status = "okay";
    pinctrl-0 = <&i2c0_bm688>;
    pinctrl-names = "default";

    bme688: bme688@76 {
        compatible = "bosch,bme680";
        reg = <0x76>;
    };
};

/* 3. Alias */
&{/} {
    aliases {
        lora0         = &sx1262;
        bme688-sensor = &bme688;
    };
};
```

**Por qué GPIO47/48:** el `i2c0_default` del board dtsi usa GPIO1
(Battery ADC) y GPIO2 (SD MISO) — ocupados por hardware. GPIO47/48 son
pines de header lateral (JP1) libres y sin función de strapping, así que
un pinctrl personalizado en I2C0 los reasigna sin tocar nada más.
Ya **no** se deshabilita UART0 (la versión anterior lo hacía para liberar
GPIO43/44); ahora UART0 y el conector QWIIC quedan disponibles.

---

## 6. Cambios en prj.conf

```kconfig
# Logging minimal — redirige LOG_* a printk() sin necesitar UART console
CONFIG_LOG=y
CONFIG_LOG_MODE_MINIMAL=y

# I2C + BM688
CONFIG_I2C=y        # auto-seleccionado por BME680, declarado explicitamente
CONFIG_SENSOR=y
CONFIG_BME680=y
```

> `CONFIG_LOG_MODE_MINIMAL` usa `printk()` internamente.
> Es compatible con `CONFIG_UART_CONSOLE=n` y `CONFIG_CONSOLE=n`.
> El proyecto ya tenia `CONFIG_PRINTK=y`, por lo que no hay dependencia nueva.

---

## 7. Arquitectura del módulo

```
T3S3/
├── include/
│   └── sensors/
│       └── bm688.h          ← API pública (struct bm688_data, init, read)
├── src/
│   ├── main.c               ← integración (extendido, nada eliminado)
│   └── sensors/
│       └── bm688.c          ← implementación (wrapper del driver Zephyr)
└── boards/
    └── esp32s3_devkitc_esp32s3_procpu.overlay  ← extendido
```

### Flujo de llamadas

```
main()
  └── bm688_init(&dev)
        └── DEVICE_DT_GET(DT_ALIAS(bme688_sensor))
              └── driver bosch,bme680 (I2C0)

main() loop
  └── bm688_read_data(dev, &data)
        ├── sensor_sample_fetch(dev)    ← dispara medicion forced-mode
        ├── sensor_channel_get(TEMP)
        ├── sensor_channel_get(PRESS)
        ├── sensor_channel_get(HUMIDITY)
        └── sensor_channel_get(GAS_RES) ← puede ser 0 en primeras lecturas
```

---

## 8. API pública

### `bm688_data`

```c
struct bm688_data {
    double temperature;     /* °C  */
    double pressure;        /* Pa  */
    double humidity;        /* %RH */
    double gas_resistance;  /* Ω   */
};
```

### `bm688_init()`

```c
int bm688_init(const struct device **dev);
```

- Obtiene el device por alias `bme688-sensor`.
- Llama a `device_is_ready()`.
- Retorna 0 en éxito, `-ENODEV` si el bus I2C no respondió.

### `bm688_read_data()`

```c
int bm688_read_data(const struct device *dev, struct bm688_data *data);
```

- Llama a `sensor_sample_fetch()` + `sensor_channel_get()`.
- Retorna 0 en éxito, código de error Zephyr en fallo.
- La resistencia de gas se inicializa a 0.0 si el calentador no estabilizó.

---

## 9. Ejemplo de integración en main.c

```c
#include "sensors/bm688.h"

int main(void)
{
    /* Inicializar BM688 */
    const struct device *bm688_dev = NULL;
    if (bm688_init(&bm688_dev) < 0) {
        printk("BM688 no disponible\n");
    }

    /* ... inicializar LoRaWAN como siempre ... */

    while (1) {
        struct bm688_data d;
        if (bm688_dev && bm688_read_data(bm688_dev, &d) == 0) {
            /* Usar d.temperature, d.pressure, d.humidity, d.gas_resistance */
        }
        k_sleep(K_SECONDS(30));
    }
}
```

### Payload LoRaWAN (12 bytes, little-endian)

| Bytes | Tipo | Campo | Escala |
|---|---|---|---|
| 0–1 | `int16_t` | Temperatura | × 100 (ej: 2550 = 25.50 °C) |
| 2–3 | `uint16_t` | Humedad | × 100 (ej: 6500 = 65.00 %) |
| 4–7 | `uint32_t` | Presión | Pa directo |
| 8–11 | `uint32_t` | Gas resistance | Ω directo |

---

## 10. Problemas comunes y soluciones

### BM688 no aparece en bus (`-ENODEV`)

**Causa más probable:** CS no conectado a VCC → el chip está en modo SPI.
**Solución:** Verificar CS → VCC y SD0 → GND con multímetro.

### Gas resistance siempre 0

**Causa:** El calentador interno (320 °C) no ha alcanzado temperatura de
régimen. El tiempo de calentamiento configurado es 197 ms (`BME680_HEATR_DUR_LP`).
**Solución:** Normal en las primeras 1-2 lecturas. Tras eso, el valor
debe estar en el rango 10 kΩ – 1 MΩ en aire limpio.

### Presión con valor incorrecto

**Causa:** La compensación de temperatura afecta el cálculo de presión.
La primera lectura puede tener un `t_fine` incorrecto.
**Solución:** Descartar la primera lectura o agregar un retardo de
`k_sleep(K_MSEC(250))` antes del primer `bm688_read_data()`.

### Error de compilación: `DT_ALIAS(bme688_sensor)` no encontrado

**Causa:** El alias `bme688-sensor` no está en el overlay o el overlay
no tiene el nombre correcto.
**Solución:** Verificar que el archivo se llama exactamente
`esp32s3_devkitc_esp32s3_procpu.overlay` (coincide con el board target).

### LOG_* no produce salida

**Causa:** `CONFIG_LOG=n` o falta `CONFIG_LOG_MODE_MINIMAL=y`.
**Solución:** Agregar a `prj.conf`:
```
CONFIG_LOG=y
CONFIG_LOG_MODE_MINIMAL=y
```

### `%f` en LOG_DBG no funciona

**Causa:** El formateo flotante en cbprintf requiere `CONFIG_CBPRINTF_FP_SUPPORT=y`.
**Solución:** El módulo bm688.c usa formato entero (`%d.%02d`) para evitar
esta dependencia. Si necesitas `%f`, agrega `CONFIG_CBPRINTF_FP_SUPPORT=y`.

---

## 11. Buenas prácticas

- Siempre verificar el retorno de `bm688_init()` antes de llamar `bm688_read_data()`.
- No llamar `bm688_read_data()` más rápido de 1 Hz; el sensor opera en
  *forced mode* y necesita tiempo de conversión (~200 ms para gas).
- En producción, considerar `BME680_HEATR_TEMP_ULP` (400 °C) para mejor
  sensibilidad a VOCs, a costa de mayor tiempo de calentamiento (1943 ms).
- El `__packed` en el payload LoRaWAN es obligatorio para evitar padding
  en arquitecturas donde `int16_t` no está alineado.
- Conectar pull-ups físicos de 4.7 kΩ entre SDA/SCL y VCC cuando se usen
  cables largos o breadboards con alta capacidad parásita.
