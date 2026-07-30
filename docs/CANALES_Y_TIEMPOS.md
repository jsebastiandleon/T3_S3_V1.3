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
| **5** `FPORT_DIAG` | uplink | Salud del nodo (14 B) | al arrancar y al caer/recuperarse un sensor |
| **10** `PORTAL_HTML_OTA_FPORT` | downlink | Actualización OTA del HTML | cuando mandas el downlink |

Los FPort de subida se definen arriba de `main.c`:

```c
#define FPORT_DATA    2
#define FPORT_SOS     3
#define FPORT_ALERT   4
#define FPORT_DIAG    5
```

### FPort 5 — salud del nodo

Existe porque **el fallo era invisible desde el servidor**. Dos casos:

- Un **reinicio**: el nodo volvía, hacía OTAA y el único rastro era un `devAddr`
  nuevo y un `fCnt` desde cero. En los logs del 2026-07-29 hubo dos reinicios en
  11 minutos y 10 h de silencio sin que nada lo señalara.
- Un **sensor caído**: su criterio desaparecía del multicriterio EN 54-30/31 sin
  ninguna señal. El nodo seguía diciendo "todo bien" con la capacidad de
  detección mermada; el único indicio era un bit a 0 en el byte 0 del FPort 2,
  que nadie vigila.

El primer byte del payload es el **`msg_type`**: `1` = arranque, `2` = fallo de
sensor.

#### `msg_type = 1` — arranque (14 B)

| Campo | Bytes | Contenido |
|---|---|---|
| `msg_type` | [0] | 1 = arranque |
| `reset_code` | [1] | 0 desconocida · 1 POR · 2 PIN · 3 SW · 4 WDT · 5 low-power · **6 CPU lockup** · **7 brownout** · 8 USB/JTAG · **9 pinchazo de alimentación** |
| `reset_raw` | [2-5] | máscara cruda de `hwinfo` |
| `boot_count` | [6-9] | contador persistente en NVS (`diag/boots`) |
| `fw_version` | [10-11] | `FW_VERSION` de `main.c` |
| `sensors_ok` | [12] | sensores presentes al arrancar (bit0 BM688, bit1 CO, bit3 SEN65) |
| `soc_reason` | [13] | `esp_reset_reason()` en crudo (0-15) |

- **`reset_code` 7 (brownout) o 9 (pinchazo)** → problema de **alimentación** (panel, batería, rail de 5 V). El decoder los agrupa en `suspect: "power"`.
- **`reset_code = 6` (CPU lockup)** → **pánico del kernel**, típicamente stack overflow.

> **Por qué se manda además el motivo crudo del SoC.** `hwinfo` no basta: el
> driver de Zephyr (`zephyr/drivers/hwinfo/hwinfo_esp32.c`) sólo traduce **9 de
> las 16** causas que define ESP-IDF, y las demás devuelven 0 = *desconocida*.
> Entre las que se deja está **`ESP_RST_PWR_GLITCH`** — un pinchazo de
> alimentación, justo la causa que se busca en un nodo solar. Se comprobó en
> banco: un reset por USB salía como `RESET CAUSE: DESCONOCIDA (raw=0x0)`. Un
> instrumento ciego precisamente donde importa no sirve, así que el nodo lee
> también `esp_reset_reason()` y lo usa cuando `hwinfo` no sabe.
- `boot_count` detecta reinicios **mudos**: si salta de N a N+3, hubo 2 arranques cuyo DIAG no llegó.

Se envía **CONFIRMED** (evento único: si se pierde, la causa se pierde con él) y
con **prioridad mínima**: sólo sale si en ese ciclo no tocaba enviar datos, no hay
alerta ni fallo pendiente, y han pasado `DIAG_MIN_SPACING_S` (150 s) desde el
último envío. Nunca puede robarle airtime al FPort 2, a una alarma ni a una señal
de avería. Tras `DIAG_MAX_ATTEMPTS` (5) intentos sin ACK se abandona.

#### `msg_type = 2` — fallo de sensor (14 B)

| Campo | Bytes | Contenido |
|---|---|---|
| `msg_type` | [0] | 2 = fallo de sensor |
| `faulted` | [1] | quién está en fallo **ahora** (verdad de campo, útil aunque se pierda un reporte anterior) |
| `went_down` | [2] | transiciones OK→FALLO desde el último reporte |
| `came_up` | [3] | transiciones FALLO→OK desde el último reporte |
| `uptime_s` | [4-7] | segundos desde el arranque |
| `bm_fails` | [8-9] | lecturas fallidas acumuladas del BM688 |
| `co_fails` | [10-11] | íd. ZE15-CO |
| `sen_fails` | [12-13] | íd. SEN65 |

Los tres *mask* usan **los mismos bits que el byte 0 del FPort 2**: bit0 BM688,
bit1 ZE15-CO, bit3 SEN65.

Un sensor se declara en fallo tras `FAULT_CONSEC_FAILS` (3) lecturas fallidas
**seguidas** — no a la primera, porque las lecturas fallan de forma esporádica
por ruido en el bus y declarar al primer fallo daría falsas señales de avería.

Prioridad: **por debajo de los datos y de las alarmas** (una alarma de incendio
manda sobre una señal de avería, igual que en EN 54) pero **por encima del DIAG
de arranque**, y con una separación mucho más corta (`FAULT_MIN_SPACING_S`, 30 s):
una avería es un evento de seguridad y debe salir pronto. Va **CONFIRMED** y queda
pendiente hasta que hay ACK — si esta señal se pierde, el sistema vuelve a estar
degradado en silencio, que es justo el defecto que este canal elimina.

Los contadores acumulados distinguen una avería **intermitente** (contador que
crece sin que el sensor llegue a caer) de una **dura** (cae y no vuelve).

#### El ZE15-CO se pausa, no se abandona

Cada lectura fallida del CO cuesta hasta **4,5 s de lazo** (3 intentos × 1500 ms
de timeout), y ése es tiempo en que el nodo no lee ningún sensor. Por eso tras 3
fallos seguidos la lectura se **pausa**. Pero se pausa con reintento
automático — nunca se abandona:

```c
#define CO_RETRY_MIN_S   60     // primera pausa
#define CO_RETRY_MAX_S   900    // tope (15 min)
```

La pausa dobla en cada fallo (60 → 120 → 240 → 480 → 900 → 900…) y **vuelve al
mínimo en cuanto hay una lectura buena**. El driver documenta fallos transitorios
(desbordamiento del FIFO RX, el sensor abortando una trama al conmutar entre Q&A
y upload — `src/sensors/ze15_co.c:37-41` y `:95-104`); un glitch de 15-30 s no
puede costar el criterio de CO durante semanas en un detector de incendios.

Coste medido en simulación con un sensor **permanentemente ausente**: 9 lecturas
en una hora en vez de ~650, y **1,1 % del tiempo de lazo** perdido. Con el tope
de 15 min alcanzado, 4,5 s de cada 900 = 0,5 %.

> **Al probarlo en banco:** tras reconectar el sensor, la recuperación tarda lo
> que quede de la pausa en curso. Si desconectas y reconectas pronto la espera es
> de 1-2 min; si lo dejas fallando mucho rato, el backoff habrá llegado al tope y
> puede tardar hasta 15. No es que no funcione: está esperando.

> En el SCADA, `degraded: true` del decoder debe generar aviso: significa que el
> nodo está detectando con menos criterios de los que debería.

> **`FW_VERSION` hay que subirlo en cada firmware que se flashee.** Sin eso no se
> puede saber qué código corre en un nodo desplegado — que es exactamente lo que
> ocurrió el 2026-07-29, cuando la cadencia en campo (~727-732 s) no correspondía
> a ningún valor jamás commiteado.

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
#define LORA_SEND_PERIOD_S    720   // cada cuánto se ENVÍA por LoRa (FPort 2)
#define SENSOR_READ_PERIOD_S  5     // cada cuánto se LEEN los sensores
```

Están desacopladas a propósito:

- Lectura (5 s): el lazo `while(1)` lee los 3 sensores, refresca el portal y
  acumula para el promedio.
- Envío (`LORA_SEND_PERIOD_S`):** solo cuando ha pasado ese tiempo se arma el
  paquete con el promedio de la ventana y se transmite.

### Esquema del lazo (simplificado)

Todo comparte **una sola radio** y **un solo presupuesto de duty-cycle**, así que
el orden del lazo *es* la política de prioridad:

```
while (1) {
    if (SOS pendiente)  -> lorawan_send(FPORT_SOS,...)          // inmediato
    leer BM688 / ZE15-CO / SEN65
      -> acumular (sumas + contador), publicar al portal
      -> actualizar SALUD: 3 fallos seguidos = sensor EN FALLO
    now = k_uptime_get()
    revisar UMBRALES (+ rate-of-rise EN 54-5, multicriterio EN 54-30/31)

    // --- por orden de prioridad de airtime ---
    1) DATOS   FPort 2  si toca por LORA_SEND_PERIOD_S      <- SIEMPRE PRIMERO
    2) ALERTA  FPort 4  si hay pendiente y no tocaban datos  (salvo FUEGO)
    3) FALLO   FPort 5  si un sensor cayo/volvio             (>= 30 s de hueco)
    4) BOOT    FPort 5  una vez por arranque                 (>= 150 s de hueco)

    k_sleep(SENSOR_READ_PERIOD_S)
}
```

Cada nivel sólo transmite si los de arriba no reclaman la radio en ese ciclo. Lo
que no sale queda **pendiente** y se reintenta; nada se descarta en silencio.

### Cuántas muestras entran en cada envío
Nominalmente `LORA_SEND_PERIOD_S / SENSOR_READ_PERIOD_S`. Con 720/5 = 144
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

## 5 bis. Franja nocturna del SoftAP (ahorro de energía)

El SoftAP es el mayor consumidor del nodo: la radio WiFi emite balizas las 24 h.
En una instalación alimentada por **panel solar** merece la pena apagarlo cuando
no hay nadie que pueda usar el portal.

```c
#define AP_OFF_FROM_H   19    // apagado desde las 19:00 (hora LOCAL)
#define AP_OFF_TO_H      6    // encendido de nuevo a las 06:00
#define TIME_RESYNC_H   24    // cada cuánto se repide la hora a la red
```

Para **desactivar** la función y dejar el AP siempre encendido, pon los dos
valores iguales. La ventana puede cruzar medianoche.

### ¿De dónde sale la hora?

El nodo **no tiene RTC con pila** ni salida a Internet (el SoftAP no enruta). La
única fuente es el comando MAC **`DeviceTimeReq`** de LoRaWAN:

- Se pide con `lorawan_request_device_time(false)` — el `false` es importante:
  el comando viaja **piggyback en el siguiente uplink de datos**, así que **no
  gasta airtime ni duty-cycle propios**. Con `true` forzaría un uplink vacío.
- `lorawan_device_time_get()` devuelve la **época GPS**; UTC = GPS + 315 964 800.
  Se ignoran los 18 s de segundos intercalares: irrelevantes frente a una
  frontera horaria y mantener la tabla no compensa.
- Entre sincronizaciones el reloj lo mantiene loramac-node. El XTAL del
  ESP32-S3 deriva ~2 s/día, así que resincronizar cada 24 h sobra de largo.

### Hora local y cambio de hora

`src/wallclock.c` aplica las reglas de la UE (Directiva 2000/84/CE): **UTC+1** en
invierno y **UTC+2** desde el último domingo de marzo a las 01:00 UTC hasta el
último domingo de octubre a las 01:00 UTC. Con un offset fijo, la ventana estaría
desplazada una hora durante siete meses al año.

Los algoritmos de calendario son los de Howard Hinnant (aritmética entera, sin
tablas ni bucles). **Verificado contra la base de datos tz (`Europe/Madrid`) en
40 424 instantes** —barrido horario de 4 años, 5 000 instantes aleatorios sobre
10 años y los bordes exactos de cambio de 2025 a 2030— con **cero discrepancias**.

### Reglas de seguridad

No hay anulación manual, así que las tres reglas importan:

1. **Sin hora, el AP se queda ENCENDIDO.** Nunca se apaga por una suposición: si
   se apagara sin fundamento, el portal quedaría inaccesible sin forma de
   recuperarlo salvo reiniciando el nodo físicamente.
2. **Reintento automático.** `portal_ap_set()` no actualiza su estado interno si
   el driver falla, y se llama en cada ciclo → una reactivación fallida se
   reintenta sola. Es el escenario grave: un AP que no vuelve por la mañana.
3. **Observable desde el servidor.** El bit `0x10` del byte 0 del FPort 2
   (`status.wifi_ap` en el decoder) indica si la radio estaba encendida. Sirve
   para confirmar desde ChirpStack que el AP **vuelve** cada mañana. Lo que no
   es observable no es verificable.

> Sólo se apaga la **radio** del AP. El servidor HTTP, el responder DNS y el
> servidor DHCP siguen levantados (no consumen sin estaciones asociadas) y no hay
> que reconstruirlos. Al encender se vuelve a afirmar la IP estática, que es
> idempotente, para no depender de si el ciclo de bajada del enlace la conservó.

---

## 5 ter. Perro guardián

Antes no había **ninguno**: si el lazo principal se bloqueaba —en un mutex, en un
semáforo sin liberar, en un bucle— el nodo se quedaba mudo indefinidamente
sirviendo aún el portal WiFi, y nadie se enteraba. Los 10 h de silencio del
2026-07-29 son compatibles con ese escenario.

`src/nodewdt.c` monta **dos niveles**, porque un solo umbral no sirve:

| Nivel | Qué detecta | Tiempo |
|---|---|---|
| WDT hardware alimentado por el hilo supervisor | Cuelgue del sistema entero (kernel muerto, interrupciones desactivadas, inversión de prioridad) | ~20 s |
| El supervisor vigila que el lazo principal **avance** | Lazo bloqueado aunque el resto del sistema viva | 45 min |

```c
#define NODEWDT_FEED_S          2        // el supervisor alimenta cada 2 s
#define NODEWDT_HW_TIMEOUT_MS   20000
#define NODEWDT_STALL_S         (45*60)  // lazo sin avanzar -> reset
```

### Por qué el umbral del lazo es tan largo

Porque el lazo se bloquea de forma **legítima** durante mucho tiempo:
`lorawan_send()` hace `k_sem_take(..., K_FOREVER)` esperando a que el MAC
termine (`zephyr/subsys/lorawan/loramac-node/lorawan.c:726`), y un uplink
**CONFIRMED** sin ACK reintenta hasta 8 veces respetando el duty-cycle del 1 %:

| Escenario | Bloqueo |
|---|---|
| CONFIRMED sin ACK, LoRaMac bajando el DR cada 2 intentos | ~10 min |
| CONFIRMED sin ACK clavado en SF12 | ~22 min |

Un watchdog agresivo reiniciaría el nodo en mitad de un envío válido y
provocaría **bucles de reinicio, que es peor que no tener watchdog**. Y hay un
segundo motivo, más sutil: un falso reinicio **falsearía el diagnóstico** del
FPort 5 — ver `RESET CAUSE: WATCHDOG` cuando en realidad hubo un envío largo y
correcto mandaría a buscar un cuelgue inexistente.

El bucle de join y el de rejoin llaman a `nodewdt_alive()` en cada vuelta:
reintentar la unión **es progreso**, no un cuelgue, y así un nodo sin cobertura
durante horas no se reinicia solo.

> La solución de fondo a los bloqueos es sacar la radio a su propio hilo
> (Fase 3). Hasta entonces, el watchdog se dimensiona alrededor del problema.

### Cierra el círculo con el FPort 5

Un reinicio por watchdog **no es silencioso**: `hwinfo` lo reporta como
`RESET_WATCHDOG` y el uplink de arranque lo manda al servidor con
`reset_cause: "WATCHDOG"` y `suspect: "watchdog"`. Por eso el supervisor deja de
alimentar en vez de llamar a `sys_reboot()`: dejar que muerda el hardware no
depende de que el kernel siga sano, y además deja la causa registrada.

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
