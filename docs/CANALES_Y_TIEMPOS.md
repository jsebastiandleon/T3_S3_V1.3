# Canales (FPorts) y tiempos del nodo T3-S3

---

## 1. Canales lógicos = FPort

En LoRaWAN no hay "canales de aplicación" como tal; el campo que separa **tipos
de mensaje** es el **FPort** (0–255) de cada trama. El nodo usa uno distinto por
tipo, para que en ChirpStack puedas **enrutar/actuar cada uno por separado**
(p.ej. datos → base de datos; SOS y alertas → webhook/email).

| FPort | Sentido | Contenido | Cuándo se envía |
|---|---|---|---|
| **2** `FPORT_DATA` | uplink | Datos de sensores (promedio), 29 B | cada `LORA_SEND_PERIOD_S` |
| **3** `FPORT_SOS` | uplink | `"SOS"` (3 B) del botón del portal | al pulsar el botón (≤1 ciclo) |
| **4** `FPORT_ALERT` | uplink | Alerta por umbral (15 B) | al cruzar un umbral (flanco) |
| **10** `PORTAL_HTML_OTA_FPORT` | downlink | Actualización OTA del HTML | cuando mandas el downlink |

Los tres FPort de subida se definen arriba de `main.c`:

```c
#define FPORT_DATA    2
#define FPORT_SOS     3
#define FPORT_ALERT   4
```

> Para cambiar un canal, hay que editar el `#define` y vuelve a compilar. En ChirpStack no
> hay que declarar FPorts: el decoder ya distingue 2/3/4 y devuelve el JSON
> correcto para cada uno (ver `tools/chirpstack_decoder.js`).

---

## 2. Los relojes del sistema (¿ciclos de reloj?)

No intervienen ciclos de reloj de CPU. Toda la temporización se apoya en el
reloj del kernel de Zephyr (uptime en milisegundos), no en contar
instrucciones del procesador:

- `k_uptime_get()` devuelve los ms transcurridos desde el arranque. Se usa
  para decidir "¿ya toca enviar?" comparando marcas de tiempo (`last_send_ms`,
  `last_alert_ms`).
- `k_sleep(K_SECONDS(n))` duerme el hilo `main` n segundos (cede la CPU; no
  es una espera activa). El tick del kernel corre a `CONFIG_SYS_CLOCK_TICKS_PER_SEC`
  y de ahí sale la resolución de esos temporizadores, pero tú razonas en
  segundos/milisegundos de pared, no en ciclos.

Entones, el nodo mide el tiempo en tiempo real (ms), no en ciclos de
reloj, por eso los intervalos se expresan y se cambian en segundos.

---

## 3. Las dos cadencias base

```c
#define LORA_SEND_PERIOD_S    729   // cada cuánto se ENVÍA por LoRa (FPort 2)
#define SENSOR_READ_PERIOD_S  5     // cada cuánto se LEEN los sensores
```

Están desacopladas a propósito:

- Lectura (5 s): el lazo `while(1)` lee los 3 sensores, refresca el portal y
  acumula para el promedio.
- Envío (`LORA_SEND_PERIOD_S`):** solo cuando ha pasado ese tiempo se arma el
  paquete con el promedio de la ventana y se transmite.

### Esquema del lazo (simplificado)

```
while (1) {
    if (SOS pendiente)        -> lorawan_send(FPORT_SOS,...)     // inmediato
    leer BM688 / ZE15-CO / SEN65
    acumular (sumas + contador)   y   publicar lectura al portal
    now = k_uptime_get()
    revisar UMBRALES  -> si cruza, lorawan_send(FPORT_ALERT,...) // con cooldown
    if (now - last_send >= LORA_SEND_PERIOD_S) {
        payload = promedio(acumulador);  lorawan_send(FPORT_DATA,...)
        reset acumulador
    }
    k_sleep(SENSOR_READ_PERIOD_S)
}
```

### Cuántas muestras entran en cada envío
Nominalmente `LORA_SEND_PERIOD_S / SENSOR_READ_PERIOD_S`. Con 729/5 ≈ 145
muestras. En la práctica salen un poco menos porque cada vuelta dura algo
más de 5 s: al `k_sleep(5 s)` se le suma el tiempo de leer los sensores (heater
del BM688 ~150 ms, la consulta Q&A del ZE15-CO, la lectura del SEN65). Por eso en
el log de 180 s se veían 35 en vez de 36. Es normal; el promedio divide entre
el número real de muestras (`bm_n`/`co_n`/`sen_n`), no entre el teórico.

---

## 4. El promedio (FPort 2)

Cada campo del paquete de datos es la media de las lecturas de la ventana, no
la última foto:

1. Cada 5 s, cada lectura válida se suma a `acc` y sube su contador.
2. Al enviar: `valor = suma / nº_muestras` por magnitud, se pone el flag del
   sensor (si tuvo ≥1 muestra) y se resetea la ventana.
3. El portal, en cambio, muestra el valor instantáneo cada 5 s.

El log lo indica: `SEND avg (bm=145 co=145 sen=145 muestras)`.

> Matiz: VOC/NOx del SEN65 tardan ~1 min en converger tras arrancar; esas
> primeras muestras sesgan a la baja la media de la primera ventana. Detalle
> en [`docs/envio_sensores.md`](envio_sensores.md).

---

## 5. Las alertas por umbral (FPort 4)

Independientes del ciclo de envío: se evalúan en cada lectura (5 s) contra la
medida instantánea, así reaccionan rápido.

```c
// Valores orientados a DETECCION DE INCENDIO (base en estandares)
#define TH_TEMP_EN  1 ; #define TH_TEMP_MAX  58.0   // °C   (EN 54-5 termico A1 54-65)
#define TH_CO_EN    1 ; #define TH_CO_MAX    10.0   // ppm  (EPA AQI CO "USG" ~9-12; fondo <1)
#define TH_PM25_EN  1 ; #define TH_PM25_MAX  35.0   // µg/m³(EPA AQI PM2.5 "USG"; humo)
#define TH_PM10_EN  1 ; #define TH_PM10_MAX  150.0  // µg/m³(~EPA AQI PM10 "USG" 155)
#define TH_VOC_EN   1 ; #define TH_VOC_MAX   150.0  // índice (base ~100; humo real ~175)
#define TH_GAS_EN   0 ; #define TH_GAS_MIN   10000.0// Ω  (MOX sin calibrar -> desactivado)
```

**Normas de incendio implementadas** (además de los umbrales fijos):

- **EN 54-5 · rate-of-rise térmico** — dispara si la temperatura **sube rápido**
  (`TH_ROR_CPMIN`, def. 8 °C/min, sobre ventana deslizante `TH_ROR_WINDOW_S`),
  aunque no llegue al umbral fijo. Es el bit **0x40** del mask (un fuego cercano
  calienta el aire deprisa antes de alcanzar 58 °C).
- **EN 54-30/31 · confirmación multicriterio** — declara **FUEGO** (bit **0x80**)
  solo si coinciden **≥ `FIRE_MIN_CRITERIA`** (def. 2) **familias**: humo
  (PM2.5/PM10), CO y calor (temp fija o rate-of-rise). Reduce falsas alarmas
  (polvo = solo PM, cocina = solo CO, sol = solo calor). El uplink de FUEGO
  **salta el cooldown** y sale de inmediato; el decoder lo marca `alert:"FIRE"`.

Reglas anti-spam:

- Flanco de subida: se envía una alerta al cruzar el umbral; no se repite
  mientras siga alto.
- Histéresis (`TH_HYSTERESIS_PCT = 10 %`): para volver a poder disparar, el
  valor debe bajar por debajo de `umbral − 10 %` (re-armado). Evita reenvíos
  cuando oscila justo en el borde.
- Cooldown (`ALERT_MIN_INTERVAL_S = 60 s`): mínimo entre uplinks de alerta,
  para respetar el duty-cycle. Si un envío falla por `-111`, la alerta queda
  pendiente y se reintenta (no se pierde).

Payload de 15 B autodescriptivo (máscara de qué cruzó + valores). Formato y
salida del decoder en [`docs/PAYLOAD_DECODER.md`](PAYLOAD_DECODER.md) §2.1.

---

## 6. El límite que manda: duty-cycle EU868 (1 %)

Aunque leas cada 5 s, no se puede transmitir tan seguido. EU868 permite ocupar
solo el 1 % del tiempo por sub-banda:

> intervalo mínimo ≈ tiempo_en_aire (ToA) × 100

El ToA depende del Spreading Factor (que el ADR ajusta):

| DR / SF | ToA aprox. (29 B) | Mínimo por duty-cycle |
|---|---|---|
| DR5 / SF7 | ~87 ms | ~9 s |
| DR0 / SF12 | ~2.1 s | ~215 s |

- El nodo arranca en SF12 → mínimo seguro ~215 s (por eso 180 s podía dar
  algún `-111` puntual justo tras el join; con ≥240 s no aparece nunca).
- Con ADR y buen enlace baja a SF7 → podrías enviar hasta ~10 s (ya limitado
  por el lazo de 5 s, no por la radio).
- `SEND ERR: -111` = duty-cycle, es regulatorio y normal, no un fallo; el
  paquete simplemente no sale y se reintenta a la siguiente ventana.

Detalle completo (tabla por SF, condiciones) en
[`docs/envio_sensores.md`](envio_sensores.md) §3.

---

## 7. Resumen para "tocar tiempos"

| Quiero… | Cambio | Dónde |
|---|---|---|
| Enviar datos más/menos seguido | `LORA_SEND_PERIOD_S` (s) | `src/main.c` (bloque *CONFIGURACION DE TIEMPOS*) |
| Muestrear/promediar más fino | `SENSOR_READ_PERIOD_S` (s) | íd. |
| Cambiar un canal | `FPORT_DATA/SOS/ALERT` | `src/main.c` (bloque *CANALES*) |
| Ajustar cuándo salta una alerta | `TH_*_MAX/MIN`, `TH_*_EN` | `src/main.c` (bloque *UMBRALES*) |
| Evitar alertas repetidas | `TH_HYSTERESIS_PCT`, `ALERT_MIN_INTERVAL_S` | íd. |

> Regla de oro: baja `LORA_SEND_PERIOD_S` con cuidado. Por debajo de ~215 s
> puede salir `-111` mientras el ADR no haya bajado el SF; con ADR estable a SF7,
> márgenes mucho menores son viables.
