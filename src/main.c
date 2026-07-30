#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <string.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/lorawan/lorawan.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include "sensors/bm688.h"
#include "sensors/ze15_co.h"
#include "sensors/sen6x.h"
#include "portal/portal.h"
#include "wallclock.h"
#include "nodewdt.h"

/* Motivo crudo de reset del SoC: hwinfo no cubre todas las causas (ver
   soc_reason_to_code() mas abajo). */
#include <esp_system.h>

/* Escaner de diagnostico del bus I2C0: lista las direcciones que hacen ACK.
   Util para verificar si el bus esta electricamente vivo (pull-ups OK) y
   si el BME688 aparece en 0x76. */
static void i2c0_scan(void)
{
    const struct device *i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
    if (!device_is_ready(i2c)) {
        printk("I2C0 SCAN: bus no listo\n");
        return;
    }
    printk("I2C0 SCAN (100kHz, SDA=47 SCL=48):\n");
    int found = 0;
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        uint8_t b;
        struct i2c_msg msg = {
            .buf = &b, .len = 1, .flags = I2C_MSG_READ | I2C_MSG_STOP,
        };
        if (i2c_transfer(i2c, &msg, 1, addr) == 0) {
            printk("  ACK 0x%02x\n", addr);
            found++;
        }
    }
    printk("I2C0 SCAN done: %d dispositivo(s)\n", found);
}

/* Escaner de diagnostico del bus I2C1 (SEN65). Mismo proposito que el de I2C0:
   confirmar si ALGO responde en el bus (pull-ups/cableado/alimentacion OK). */
static void i2c1_scan(void)
{
    const struct device *i2c = DEVICE_DT_GET(DT_NODELABEL(i2c1));
    if (!device_is_ready(i2c)) {
        printk("I2C1 SCAN: bus no listo\n");
        return;
    }
    printk("I2C1 SCAN (100kHz, SDA=15 SCL=16):\n");
    int found = 0;
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        uint8_t b;
        struct i2c_msg msg = {
            .buf = &b, .len = 1, .flags = I2C_MSG_READ | I2C_MSG_STOP,
        };
        if (i2c_transfer(i2c, &msg, 1, addr) == 0) {
            printk("  ACK 0x%02x\n", addr);
            found++;
        }
    }
    printk("I2C1 SCAN done: %d dispositivo(s)\n", found);
}

/* DevEUI: se deriva en runtime de la MAC del ESP32 (eFuse) -> EUI-64 estandar
   insertando FF FE en el medio. Se carga sola en cada arranque y se imprime por
   consola al unir, para darla de alta en ChirpStack. (Ver build en main().) */

/* JoinEUI: todos ceros (Chirpstack acepta cualquier valor) */
#define JOIN_EUI { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }

/* AppKey proporcionada */
#define APP_KEY  { 0x06, 0x26, 0x35, 0xAC, 0xC3, 0xBB, 0xC9, 0x2C, \
                   0x2F, 0xEF, 0x99, 0x4F, 0x5E, 0xF0, 0xF6, 0x9B }

/* Flags de estado (byte 0 del payload): que sensores aportaron dato este ciclo. */
#define FLAG_BM688_OK  0x01
#define FLAG_CO_OK     0x02
#define FLAG_CO_FAULT  0x04
#define FLAG_SEN65_OK  0x08
/* Estado de la radio del SoftAP en el momento del envio. Sin este bit, la
   franja nocturna seria invisible desde el servidor: no habria forma de saber
   si el AP se apago cuando debia, ni —lo que importa de verdad— si volvio a
   encenderse por la mañana. Lo que no es observable no es verificable. */
#define FLAG_AP_ON     0x10

/* =======================================================================
 *  CONFIGURACION DE TIEMPOS  <-- CAMBIA AQUI EL INTERVALO DE ENVIO
 * =======================================================================
 * LORA_SEND_PERIOD_S: cada cuantos SEGUNDOS se envia el uplink a ChirpStack.
 *   Es el unico valor que hay que tocar para controlar la cadencia de envio.
 *
 *   OJO duty-cycle EU868 (1%): a SF12 un uplink de ~29 B dura ~1.5 s de
 *   airtime, lo que obliga a >=~150 s entre paquetes. Si pones menos, el
 *   stack devuelve "SEND ERR: -111" (duty-cycle) y NO envia. 180 s es el
 *   valor conservador (aguanta incluso SF12). Cuando el ADR sube el DR a
 *   SF7 (~70 ms de airtime) se podria bajar bastante con seguridad.
 *
 * SENSOR_READ_PERIOD_S: cada cuantos segundos se LEEN los sensores (y se
 *   refresca el dashboard del portal). El uplink LoRa NO envia la ultima
 *   lectura, sino el PROMEDIO de todas las lecturas tomadas dentro de la
 *   ventana de envio (LORA_SEND_PERIOD_S / SENSOR_READ_PERIOD_S muestras).
 * ======================================================================= */
/* 720 s = 12 min exactos. Valor OFICIAL, fijado tras comprobar que el nodo en
   campo venia enviando cada ~727-732 s (medido sobre los uplinks del
   2026-07-29) mientras el codigo declaraba 180: el binario desplegado no
   correspondia a ningun commit. 720 cae dentro de la banda medida, deja
   margen de duty-cycle incluso a SF12 y es el valor que documenta
   docs/CANALES_Y_TIEMPOS.md. NO cambiar sin subir FW_VERSION. */
#define LORA_SEND_PERIOD_S    720
#define SENSOR_READ_PERIOD_S  5

/* =======================================================================
 *  SUPERVISION DEL ENLACE  (keepalive confirmado + auto-rejoin)
 * =======================================================================
 * Un uplink UNCONFIRMED no da NINGUNA pista de si llego: el nodo puede
 * transmitir horas al vacio y decir "SEND OK" siempre. Para detectar un
 * enlace muerto y recuperarlo solo:
 *   - LINK_KEEPALIVE_EVERY: 1 de cada N envios de datos va CONFIRMED, para
 *     forzar un ACK del servidor (prueba de vida real del enlace).
 *   - LINK_MAX_FAILS: si ese numero de keepalives seguidos NO reciben ACK,
 *     se asume enlace caido y se hace REJOIN (vuelve a SF12, sesion nueva).
 * Con N=6 y periodo 180 s, se sondea ~cada 18 min; 3 fallos ~= 54 min de
 * silencio antes de re-unirse (robusto ante un -111 puntual de duty-cycle). */
#define LINK_KEEPALIVE_EVERY  6
#define LINK_MAX_FAILS        3

/* =======================================================================
 *  CANALES (FPorts) — cada tipo de mensaje va por su PROPIO FPort, para
 *  poder enrutarlos/actuar por separado en ChirpStack.
 * ======================================================================= */
#define FPORT_DATA    2   /* datos periodicos (promedio de la ventana)   */
#define FPORT_SOS     3   /* boton de emergencia del portal cautivo      */
#define FPORT_ALERT   4   /* alertas automaticas por umbral (threshold)  */
#define FPORT_DIAG    5   /* salud del nodo: arranque, causa de reset    */

/* =======================================================================
 *  IDENTIFICACION DE BUILD   <-- SUBE ESTO EN CADA FIRMWARE QUE FLASHEES
 * =======================================================================
 * Se reporta en el uplink de arranque (FPORT_DIAG). Sin esto no hay forma
 * de saber que codigo corre realmente en un nodo desplegado: el analisis de
 * los logs del 2026-07-29 demostro que la cadencia en campo (~727-732 s) no
 * correspondia a ningun valor jamas commiteado de LORA_SEND_PERIOD_S, y no
 * habia manera de confirmarlo desde el servidor. Regla: si cambias algo que
 * se flashea, SUBE FW_VERSION. */
#define FW_VERSION    0x0206   /* v2.6 — causa de reset cruda del SoC */

/* =======================================================================
 *  UMBRALES DE ALERTA   <-- DEFINE AQUI LOS VALORES
 * =======================================================================
 * Se comparan contra la lectura INSTANTANEA de cada ciclo (cada
 * SENSOR_READ_PERIOD_S). Al CRUZAR un umbral se envia UNA alerta por
 * FPORT_ALERT; NO se reenvia hasta que el valor baje del umbral (con
 * histeresis) y lo vuelva a cruzar -> asi no se inunda la radio.
 *
 * Para DESACTIVAR un umbral, pon su *_EN a 0.
 * Casi todos son "maximo" (alerta si SUPERA el valor). El gas del BM688 es
 * al reves: alerta si CAE por DEBAJO del valor (menos ohmios = aire peor).
 *
 * Valores orientados a DETECCION TEMPRANA DE INCENDIO (humo/combustion),
 * apoyados en estandares. Referencias:
 *   - CO:    EPA AQI de CO -> "insalubre p/sensibles" arranca ~9-12 ppm;
 *            fondo exterior <1 ppm. NIOSH 8h=35 / OSHA 8h=50 ppm son limites
 *            OCUPACIONALES (demasiado altos para deteccion temprana).
 *   - PM2.5: EPA AQI -> "USG" desde 35 ug/m3; marcador PRINCIPAL del humo.
 *   - PM10:  EPA AQI -> "USG" ~155 ug/m3 (dejamos 150 como corroboracion).
 *   - Temp:  EN 54-5 (detectores termicos): alarma clase A1 (respuesta
 *            estatica entre 54-65 C). 58 C = punto medio de esa banda, por
 *            encima de olas de calor + autocalentamiento de la caja.
 *   - VOC:   indice Sensirion (base ~100); la combustion lo dispara >>100.
 *   - Gas (BM688): MOX sin calibrar y con deriva -> umbral fijo poco fiable;
 *            se deja DESACTIVADO (primarios: PM2.5, CO, VOC).
 */
#define TH_TEMP_EN     1
#define TH_TEMP_MAX    58.0      /* grados C  (EN 54-5 termico clase A1 54-65) */
#define TH_CO_EN       1
#define TH_CO_MAX      10.0      /* ppm  (EPA AQI CO "USG" ~9-12; fondo <1)    */
#define TH_PM25_EN     1
#define TH_PM25_MAX    35.0      /* ug/m3  (EPA AQI PM2.5 "USG"; humo)         */
#define TH_PM10_EN     1
#define TH_PM10_MAX    150.0     /* ug/m3  (~EPA AQI PM10 "USG" 155)           */
#define TH_VOC_EN      1
#define TH_VOC_MAX     150.0     /* indice VOC (base ~100; humo real ~175)     */
#define TH_GAS_EN      0         /* MOX sin calibrar/deriva -> desactivado     */
#define TH_GAS_MIN     10000.0   /* Ohm  (si se activa: alerta si BAJA)        */

/* Histeresis (%): el valor debe alejarse este % del umbral para re-armar la
   alerta (evita reenvios cuando oscila justo en el borde). */
#define TH_HYSTERESIS_PCT     10

/* Cooldown minimo entre uplinks de alerta (segundos): respeta el duty-cycle
   EU868 aunque crucen varios umbrales seguidos. Un FUEGO confirmado
   (multicriterio) IGNORA este cooldown y sale de inmediato. */
#define ALERT_MIN_INTERVAL_S  60

/* Uplink de arranque (FPORT_DIAG): separacion minima respecto a cualquier otro
   envio, para no chocar con el duty-cycle (150 s cubre el peor caso a SF12), y
   tope de intentos para no reintentar indefinidamente si el enlace esta mal
   (la causa del reset queda igualmente en la consola y en boot_count). */
#define DIAG_MIN_SPACING_S    150
#define DIAG_MAX_ATTEMPTS     5

/* =======================================================================
 *  SEÑAL DE FALLO DE SENSOR  (FPORT_DIAG, msg_type 2)
 * =======================================================================
 * FAULT_CONSEC_FAILS: lecturas fallidas SEGUIDAS antes de declarar un sensor
 *   en fallo. Las lecturas fallan de forma esporadica (ruido en el bus, una
 *   trama perdida del ZE15-CO), asi que declarar a la primera daria falsas
 *   señales de averia. 3 es el mismo umbral que ya usaba el ZE15-CO.
 * FAULT_MIN_SPACING_S: separacion minima entre reportes de fallo. Mucho mas
 *   corto que el del DIAG de arranque porque un fallo de sensor es un evento
 *   de seguridad y debe salir pronto; si el duty-cycle lo rechaza (a SF12),
 *   queda pendiente y se reintenta sin coste de airtime. */
#define FAULT_CONSEC_FAILS    3
#define FAULT_MIN_SPACING_S   30

/* =======================================================================
 *  REINTENTO DEL ZE15-CO TRAS UN FALLO PERSISTENTE
 * =======================================================================
 * Cada lectura fallida del CO cuesta hasta 4.5 s de lazo (3 intentos x 1500
 * ms de timeout), y ese es tiempo que el nodo NO dedica a detectar. Por eso,
 * tras FAULT_CONSEC_FAILS fallos seguidos, se PAUSA la lectura.
 *
 * Pero se pausa, no se abandona. El driver documenta fallos transitorios
 * (desbordamiento del FIFO RX, el sensor abortando una trama al conmutar
 * entre Q&A y upload — ver src/sensors/ze15_co.c); un glitch de ~20 s no
 * puede costar el criterio de CO durante semanas en un detector de incendios.
 *
 * La pausa crece exponencialmente (60 s, 120, 240... hasta CO_RETRY_MAX_S) y
 * vuelve al minimo en cuanto hay una lectura buena. Con el tope de 15 min, un
 * sensor realmente ausente cuesta 4.5 s de cada 900 = 0.5% del tiempo de lazo.
 * ======================================================================= */
#define CO_RETRY_MIN_S        60
#define CO_RETRY_MAX_S        900

/* =======================================================================
 *  FRANJA NOCTURNA DEL SOFTAP   <-- CAMBIA AQUI EL HORARIO
 * =======================================================================
 * El SoftAP es el mayor consumidor del nodo (radio WiFi emitiendo balizas
 * las 24 h). En una instalacion solar merece la pena apagarlo cuando no hay
 * nadie que pueda usar el portal.
 *
 * La franja se expresa en HORA LOCAL (peninsula: UTC+1 / UTC+2 en verano; el
 * cambio se aplica solo, ver src/wallclock.c). El AP esta APAGADO desde
 * AP_OFF_FROM_H:00 hasta AP_OFF_TO_H:00. La ventana puede cruzar medianoche.
 *   19 -> 6  = apagado de 19:00 a 05:59, encendido a las 06:00.
 * Para DESACTIVAR la funcion y dejar el AP siempre encendido, pon los dos
 * valores iguales.
 *
 * SEGURIDAD: no hay anulacion manual. Si el nodo no tiene la hora, el AP se
 * queda ENCENDIDO — nunca se apaga por una suposicion. Ver AP_SCHED_* abajo.
 * ======================================================================= */
#define AP_OFF_FROM_H   19
#define AP_OFF_TO_H     6

/* Cada cuanto se vuelve a pedir la hora a la red (DeviceTimeReq). La peticion
   viaja PIGGYBACK en el siguiente uplink de datos, no gasta airtime propio.
   El XTAL del ESP32-S3 deriva ~2 s/dia, asi que resincronizar a diario sobra
   de largo para una frontera horaria. */
#define TIME_RESYNC_H   24

/* =======================================================================
 *  NORMA EN 54-5 — RATE-OF-RISE TERMICO
 * =======================================================================
 * Ademas del umbral fijo (TH_TEMP_MAX), se detecta una SUBIDA RAPIDA de
 * temperatura: un incendio cercano calienta el aire deprisa mucho antes de
 * llegar a 58 C. Se mide sobre una ventana deslizante y se alarma si el ritmo
 * supera TH_ROR_CPMIN (grados C por minuto). Valor tipico de detectores
 * rate-of-rise EN 54-5: ~8 C/min. */
#define TH_ROR_EN        1
#define TH_ROR_CPMIN     8.0     /* grados C por minuto */
#define TH_ROR_WINDOW_S  60      /* ventana deslizante para medir el ritmo */

/* =======================================================================
 *  NORMA EN 54-30/31 — CONFIRMACION MULTICRITERIO DE FUEGO
 * =======================================================================
 * Los detectores multisensor declaran FUEGO por COINCIDENCIA de varias
 * familias de indicio, no por una sola -> muchas menos falsas alarmas
 * (polvo=solo PM, cocina=solo CO, sauna=solo calor; INCENDIO = varios a la
 * vez). Aqui las familias son: HUMO (PM2.5/PM10), CO y CALOR (temp fija o
 * rate-of-rise). Si coinciden >= FIRE_MIN_CRITERIA, se declara FUEGO y el
 * uplink de alerta se envia de inmediato (salta el cooldown). */
#define FIRE_MIN_CRITERIA  2

/* Bits del byte 0 del payload de alerta (FPORT_ALERT). */
#define ALERT_TEMP     0x01
#define ALERT_CO       0x02
#define ALERT_PM25     0x04
#define ALERT_PM10     0x08
#define ALERT_VOC      0x10
#define ALERT_GAS      0x20
#define ALERT_HEAT_ROR 0x40   /* subida rapida de temperatura (EN 54-5)        */
#define ALERT_FIRE     0x80   /* FUEGO confirmado multicriterio (EN 54-30/31)  */

/* =======================================================================
 *  SALUD DEL NODO: causa de reset + contador de arranques  (FPORT_DIAG)
 * =======================================================================
 * Un reinicio hoy es INVISIBLE desde el servidor: el nodo vuelve, hace OTAA,
 * y el unico rastro es un devAddr nuevo y un fCnt que empieza de cero. Los
 * logs del 2026-07-29 mostraron dos reinicios en 11 minutos y 10 h de
 * silencio sin que nada lo señalara.
 *
 * hwinfo del ESP32-S3 distingue las causas que separan las hipotesis:
 *   BROWNOUT   -> caida del rail de alimentacion (panel/bateria/5V)
 *   CPU_LOCKUP -> panic del kernel (p.ej. stack overflow)
 *   WATCHDOG   -> perro guardian (aun no hay ninguno configurado: Fase 1)
 *   POR        -> corte de alimentacion real
 *   PIN / SW   -> reset externo o provocado por software
 *
 * OJO: hwinfo_clear_reset_cause() NO esta implementado en el driver del
 * ESP32 (zephyr/drivers/hwinfo/hwinfo_esp32.c solo define get_device_id,
 * get_supported_reset_cause y get_reset_cause). No se llama: esp_reset_reason()
 * ya latchea el valor correcto de un arranque al siguiente. */
#define RSTC_UNKNOWN     0
#define RSTC_POR         1
#define RSTC_PIN         2
#define RSTC_SOFTWARE    3
#define RSTC_WATCHDOG    4
#define RSTC_LOWPOWER    5
#define RSTC_LOCKUP      6
#define RSTC_BROWNOUT    7
#define RSTC_USB_JTAG    8   /* reset del host: banco, no campo */
#define RSTC_PWR_GLITCH  9   /* pinchazo de alimentacion */

/*
 * hwinfo NO basta en este SoC. El driver de Zephyr
 * (zephyr/drivers/hwinfo/hwinfo_esp32.c) solo traduce 9 de las 16 causas que
 * define ESP-IDF; el resto caen en 'default' y devuelven 0, o sea DESCONOCIDA.
 *
 * Entre las NO mapeadas esta ESP_RST_PWR_GLITCH —un pinchazo de alimentacion—,
 * que es justo la causa que se busca en un nodo alimentado por panel solar. Se
 * comprobo en banco: un reset por USB salia como "DESCONOCIDA (raw=0x0)".
 * Un instrumento ciego precisamente donde importa no sirve, asi que se lee
 * tambien el motivo crudo del SoC y se usa cuando hwinfo no sabe.
 */
static uint8_t soc_reason_to_code(uint32_t soc)
{
    switch (soc) {
    case ESP_RST_POWERON:    return RSTC_POR;
    case ESP_RST_EXT:        return RSTC_PIN;
    case ESP_RST_SW:         return RSTC_SOFTWARE;
    case ESP_RST_PANIC:
    case ESP_RST_CPU_LOCKUP: return RSTC_LOCKUP;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:        return RSTC_WATCHDOG;
    case ESP_RST_DEEPSLEEP:  return RSTC_LOWPOWER;
    case ESP_RST_BROWNOUT:   return RSTC_BROWNOUT;
    case ESP_RST_PWR_GLITCH: return RSTC_PWR_GLITCH;
    case ESP_RST_USB:
    case ESP_RST_JTAG:       return RSTC_USB_JTAG;
    default:                 return RSTC_UNKNOWN;
    }
}

static uint8_t reset_cause_code(uint32_t raw, uint32_t soc)
{
    /* Orden por criticidad de diagnostico: brownout y lockup mandan si
       aparecen combinados con otro bit. */
    if (raw & RESET_BROWNOUT)       return RSTC_BROWNOUT;
    if (raw & RESET_CPU_LOCKUP)     return RSTC_LOCKUP;
    if (raw & RESET_WATCHDOG)       return RSTC_WATCHDOG;
    if (raw & RESET_POR)            return RSTC_POR;
    if (raw & RESET_PIN)            return RSTC_PIN;
    if (raw & RESET_SOFTWARE)       return RSTC_SOFTWARE;
    if (raw & RESET_LOW_POWER_WAKE) return RSTC_LOWPOWER;

    /* hwinfo no lo reconoce: preguntar al SoC directamente. */
    return soc_reason_to_code(soc);
}

static const char *reset_cause_str(uint8_t code)
{
    switch (code) {
    case RSTC_POR:      return "POR (alimentacion)";
    case RSTC_PIN:      return "PIN (reset externo)";
    case RSTC_SOFTWARE: return "SOFTWARE";
    case RSTC_WATCHDOG: return "WATCHDOG";
    case RSTC_LOWPOWER: return "LOW-POWER WAKE";
    case RSTC_LOCKUP:   return "CPU LOCKUP (panic)";
    case RSTC_BROWNOUT: return "BROWNOUT (caida de tension)";
    case RSTC_USB_JTAG: return "USB/JTAG (reset del host)";
    case RSTC_PWR_GLITCH: return "PWR GLITCH (pinchazo de alimentacion)";
    default:            return "DESCONOCIDA";
    }
}

/* =======================================================================
 *  SALUD POR SENSOR
 * =======================================================================
 * Hasta ahora, si un sensor dejaba de responder su criterio de deteccion
 * desaparecia del multicriterio EN 54-30/31 SIN ninguna señal: el nodo seguia
 * diciendo "todo bien" con la capacidad de deteccion mermada. El unico indicio
 * era un bit a 0 en el byte 0 del FPort 2, que nadie vigila. */
struct sensor_health {
    uint16_t consec_fails;  /* fallos seguidos ahora mismo   */
    uint16_t total_fails;   /* acumulado (satura en 0xFFFF)  */
    bool     faulted;       /* declarado en fallo            */
};

/*
 * Registra el resultado de una lectura y actualiza el estado de fallo.
 *   ok        - la lectura fue valida
 *   bit       - identificador del sensor (mismos bits que el byte 0 del FPort 2)
 *   down/up   - mascaras de transiciones aun sin reportar; se acumulan con |=
 *               para que una caida y una recuperacion simultaneas convivan.
 *
 * Un sensor se declara EN FALLO tras FAULT_CONSEC_FAILS lecturas fallidas
 * SEGUIDAS, no a la primera: las lecturas fallan de forma esporadica (ruido en
 * el bus, una trama perdida del ZE15-CO) y declarar al primer fallo daria
 * falsas señales de averia.
 */
static void health_update(struct sensor_health *h, bool ok, uint8_t bit,
                          uint8_t *down, uint8_t *up)
{
    if (ok) {
        h->consec_fails = 0;
        if (h->faulted) {
            h->faulted = false;
            *up |= bit;
            printk("SALUD: sensor 0x%02x RECUPERADO\n", bit);
        }
        return;
    }

    if (h->consec_fails < UINT16_MAX) {
        h->consec_fails++;
    }
    if (h->total_fails < UINT16_MAX) {
        h->total_fails++;
    }
    if (!h->faulted && h->consec_fails >= FAULT_CONSEC_FAILS) {
        h->faulted = true;
        *down |= bit;
        printk("SALUD: sensor 0x%02x EN FALLO (%u lecturas seguidas)\n",
               bit, (unsigned int)h->consec_fails);
    }
}

/*
 * ¿Cae 'hour' (hora local) dentro de la franja en la que el AP debe estar
 * apagado? La ventana puede cruzar medianoche (19 -> 6). Si los dos extremos
 * son iguales, la funcion queda desactivada y el AP no se apaga nunca.
 */
static bool ap_in_off_window(uint8_t hour)
{
	if (AP_OFF_FROM_H == AP_OFF_TO_H) {
		return false;
	}
	if (AP_OFF_FROM_H < AP_OFF_TO_H) {
		return hour >= AP_OFF_FROM_H && hour < AP_OFF_TO_H;
	}
	return hour >= AP_OFF_FROM_H || hour < AP_OFF_TO_H;
}

/* Contador de arranques, persistido en NVS via Settings ("diag/boots").
   Permite detectar reinicios aunque el uplink de arranque se pierda: si el
   boot_count del siguiente DIAG salta de N a N+3, hubo 2 reinicios mudos. */
static uint32_t boot_count;

static int diag_settings_set(const char *name, size_t len,
                             settings_read_cb read_cb, void *cb_arg)
{
    if (settings_name_steq(name, "boots", NULL)) {
        if (len != sizeof(boot_count)) {
            return -EINVAL;
        }
        return (read_cb(cb_arg, &boot_count, sizeof(boot_count)) < 0)
               ? -EIO : 0;
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(diag, "diag", NULL, diag_settings_set,
                               NULL, NULL);

static void downlink_cb(uint8_t port, uint8_t flags, int16_t rssi,
                        int8_t snr, uint8_t len, const uint8_t *data)
{
    printk("DL port=%d rssi=%d snr=%d len=%d\n", port, rssi, snr, len);

    /* Downlinks en el FPort del portal -> protocolo de actualizacion de HTML.
       Reensambla por trozos (BEGIN/DATA/COMMIT) y valida CRC antes de aplicar. */
    if (port == PORTAL_HTML_OTA_FPORT && data != NULL) {
        portal_html_ota_rx(data, len);
    }
}

int main(void)
{
    printk("BOOT\n");

    /* ---- Salud del nodo: por que hemos arrancado -------------------- */
    /* Lo PRIMERO, antes de tocar perifericos: es solo una lectura de registro
       y su valor de diagnostico no debe depender de que el resto arranque. */
    uint32_t reset_raw = 0;
    if (hwinfo_get_reset_cause(&reset_raw) < 0) {
        reset_raw = 0;
    }
    const uint32_t soc_reason = (uint32_t)esp_reset_reason();
    const uint8_t  reset_code  = reset_cause_code(reset_raw, soc_reason);
    printk("RESET CAUSE: %s (hwinfo=0x%08x soc=%u)\n",
           reset_cause_str(reset_code), reset_raw, soc_reason);

    /* Settings/NVS: hay que inicializar el subsistema ANTES de que nadie
       cargue su subtree. Sin esta llamada, settings_load_subtree() recorre
       una lista de backends vacia y no hace NADA en silencio (ver
       zephyr/subsys/settings/src/settings_store.c:41). Eso afectaba tambien
       a portal_html_init(), que carga "portal" desde portal_start() — es
       decir, ANTES de lorawan_start(), que era quien acababa inicializando
       el subsistema: el HTML guardado en NVS nunca se restauraba.
       Es idempotente, asi que la llamada posterior de lorawan_start() es
       inocua. */
    int sret = settings_subsys_init();
    if (sret < 0) {
        printk("SETTINGS INIT ERR: %d (contador de arranques no persistira)\n",
               sret);
    } else {
        (void)settings_load_subtree("diag");
        boot_count++;
        int wr = settings_save_one("diag/boots", &boot_count,
                                   sizeof(boot_count));
        if (wr < 0) {
            printk("SETTINGS SAVE ERR: %d\n", wr);
        }
    }
    printk("BOOT #%u  FW=0x%04X\n", boot_count, FW_VERSION);

    i2c0_scan();
    i2c1_scan();

    /* ---- BM688 init ------------------------------------------------- */
    const struct device *bm688_dev = NULL;
    int bm688_ok = bm688_init(&bm688_dev);
    if (bm688_ok < 0) {
        printk("BM688 INIT ERR: %d (continua sin sensor)\n", bm688_ok);
    } else {
        printk("BM688 READY\n");
    }
    /* ------------------------------------------------------------------ */

    /* ---- ZE15-CO init ----------------------------------------------- */
    const struct device *co_dev = NULL;
    int co_ok = ze15co_init(&co_dev);
    if (co_ok < 0) {
        printk("ZE15-CO INIT ERR: %d (continua sin sensor)\n", co_ok);
    } else {
        printk("ZE15-CO READY\n");
    }
    /* ------------------------------------------------------------------ */

    /* ---- SEN65 init (calidad de aire: PM, VOC, NOx, RH&T) ----------- */
    /* Bus propio I2C1 (SDA=GPIO15, SCL=GPIO16), separado del BM688 (I2C0).
       Arranca la medicion continua; los indices VOC/NOx tardan ~10s en converger. */
    const struct device *sen65_dev = NULL;
    int sen65_ok = sen6x_init(&sen65_dev);
    if (sen65_ok < 0) {
        printk("SEN65 INIT ERR: %d (continua sin sensor)\n", sen65_ok);
    } else {
        printk("SEN65 READY\n");
    }
    /* ------------------------------------------------------------------ */

    /* ---- Portal cautivo (WiFi AP + dashboard) ----------------------- */
    /* Concurrente con LoRaWAN + sensores: se levanta aqui y queda sirviendo
       en http://192.168.4.1. El HTML es actualizable por downlink LoRa. */
    int portal_ok = portal_start();
    if (portal_ok < 0) {
        printk("PORTAL ERR: %d (continua sin portal)\n", portal_ok);
    }
    /* ------------------------------------------------------------------ */

    const struct device *lora_dev = DEVICE_DT_GET(DT_ALIAS(lora0));
    if (!device_is_ready(lora_dev)) {
        printk("LORA NOT READY\n");
        return 0;
    }
    printk("LORA READY\n");

    int ret = lorawan_set_region(LORAWAN_REGION_EU868);
    if (ret < 0) {
        printk("SET REGION ERR: %d\n", ret);
        return 0;
    }

    ret = lorawan_start();
    if (ret < 0) {
        printk("LORAWAN START ERR: %d\n", ret);
        return 0;
    }
    printk("LORAWAN STARTED\n");

    /*
     * ADR (Adaptive Data Rate) — optimizacion clave del envio: el network server
     * ajusta SF/DR del nodo. Arranca en DR0/SF12 (max alcance, ~1.5 s de airtime
     * para 26 B) y, si el enlace lo permite, ChirpStack lo sube hasta SF7
     * (~70 ms): menos airtime, menos consumo y mas margen de duty-cycle. Funciona
     * con uplinks unconfirmed (mecanismo ADRACKReq gestionado por el stack).
     */
    lorawan_enable_adr(true);

    static struct lorawan_downlink_cb dl = {
        .port = LW_RECV_PORT_ANY,
        .cb   = downlink_cb,
    };
    lorawan_register_downlink_callback(&dl);

    /* DevEUI = MAC del chip (eFuse via hwinfo) -> EUI-64 (insertar FF FE). */
    uint8_t dev_eui[8];
    uint8_t mac[6];
    ssize_t mlen = hwinfo_get_device_id(mac, sizeof(mac));
    if (mlen == (ssize_t)sizeof(mac)) {
        dev_eui[0] = mac[0]; dev_eui[1] = mac[1]; dev_eui[2] = mac[2];
        dev_eui[3] = 0xFF;   dev_eui[4] = 0xFE;
        dev_eui[5] = mac[3]; dev_eui[6] = mac[4]; dev_eui[7] = mac[5];
    } else {
        printk("HWINFO MAC err %d -> DevEUI de respaldo\n", (int)mlen);
        static const uint8_t fb[8] = {0x1C, 0xDB, 0xD4, 0xFF,
                                      0xFE, 0xBD, 0x29, 0x65};
        memcpy(dev_eui, fb, sizeof(fb));
    }
    printk("DevEUI(MAC)=%02X%02X%02X%02X%02X%02X%02X%02X\n",
           dev_eui[0], dev_eui[1], dev_eui[2], dev_eui[3],
           dev_eui[4], dev_eui[5], dev_eui[6], dev_eui[7]);

    uint8_t join_eui[] = JOIN_EUI;
    uint8_t app_key[]  = APP_KEY;

    /*
     * dev_nonce NO se fija aqui: con CONFIG_LORAWAN_NVM_SETTINGS=y el backend
     * de NVM gestiona el DevNonce (lo restaura de flash y lo incrementa de
     * forma monotona en cada join, persistiendolo). El campo join_cfg.otaa.
     * dev_nonce solo lo usaria el stack bajo CONFIG_LORAWAN_NVM_NONE
     * (ver subsys/lorawan/.../lorawan.c, guardado por IS_ENABLED(NVM_NONE));
     * con NVM activa se ignora. Por eso aqui no se inicializa ni se incrementa:
     * hacerlo daria una falsa sensacion de control y no tiene efecto.
     */
    struct lorawan_join_config join_cfg = {
        .mode          = LORAWAN_ACT_OTAA,
        .dev_eui       = dev_eui,
        .otaa = {
            .join_eui  = join_eui,
            .app_key   = app_key,
            .nwk_key   = app_key,   /* LoRaWAN 1.0.x: nwk_key == app_key */
        },
    };

    /* Perro guardian ANTES del bucle de join: ese bucle puede girar horas si
       no hay cobertura, y tiene que quedar supervisado igual. Marca vida en
       cada vuelta, de modo que reintentar el join cuenta como PROGRESO y no
       provoca reinicios: el nodo esta haciendo justo lo que debe. */
    if (nodewdt_start() < 0) {
        printk("AVISO: el nodo arranca SIN perro guardian\n");
    }

    for (int attempt = 1; ; attempt++) {
        nodewdt_alive();
        printk("JOINING... attempt %d\n", attempt);
        ret = lorawan_join(&join_cfg);
        if (ret == 0) {
            break;
        }
        printk("JOIN ERR: %d (retry in 10s)\n", ret);
        k_sleep(K_SECONDS(10));
    }
    printk("JOINED!\n");

    /*
     * Payload LoRaWAN v2 (29 bytes, little-endian, FPort 2).
     * Incluye TODOS los datos medibles de los 3 sensores + flags de estado.
     * Cada campo es el PROMEDIO de las lecturas tomadas en la ventana de
     * envio (ver acumuladores 'acc' mas abajo), no la ultima lectura.
     * (Ver el decoder de ChirpStack en docs/PAYLOAD_DECODER.md.)
     *   [0]     uint8  flags: bit0 BM688 ok, bit1 CO ok, bit2 CO fault, bit3 SEN65 ok
     *   --- BM688 (ambiental) --------------------------------------------
     *   [1-2]   int16  temperatura  × 100  (°C)
     *   [3-4]   uint16 humedad      × 100  (%RH)
     *   [5-6]   uint16 presion      × 10   (hPa; = Pa/10, p.ej. 10091 = 1009.1 hPa)
     *   [7-10]  uint32 gas resistance      (Ohm)
     *   --- ZE15-CO ------------------------------------------------------
     *   [11-12] uint16 CO           × 10   (ppm)
     *   --- SEN65 (calidad de aire) --------------------------------------
     *   [13-14] uint16 PM1.0        × 10   (µg/m³)
     *   [15-16] uint16 PM2.5        × 10   (µg/m³)
     *   [17-18] uint16 PM4.0        × 10   (µg/m³)
     *   [19-20] uint16 PM10.0       × 10   (µg/m³)
     *   [21-22] uint16 humedad SEN65 × 100 (%RH)
     *   [23-24] int16  temp SEN65    × 100 (°C)
     *   [25-26] uint16 VOC index    × 10
     *   [27-28] uint16 NOx index    × 10
     * Los campos de un sensor ausente van a 0 (consultar los flags en byte 0).
     */
    struct {
        uint8_t  flags;
        int16_t  bm_temp_cdeg;
        uint16_t bm_hum_cpct;
        uint16_t bm_press_dhpa;
        uint32_t bm_gas_ohm;
        uint16_t co_ppm_x10;
        uint16_t pm1_0_x10;
        uint16_t pm2_5_x10;
        uint16_t pm4_0_x10;
        uint16_t pm10_0_x10;
        uint16_t sen_hum_cpct;
        int16_t  sen_temp_cdeg;
        uint16_t voc_x10;
        uint16_t nox_x10;
    } __packed payload;

    /*
     * Payload de ALERTA por umbral (FPORT_ALERT, 15 bytes, little-endian).
     * Autodescriptivo: mascara de que umbrales estan superados + los valores
     * instantaneos relevantes en el momento de la alerta.
     *   [0]     uint8  alert_mask: bit0 temp, bit1 CO, bit2 PM2.5, bit3 PM10,
     *                              bit4 VOC, bit5 gas
     *   [1-2]   int16  temperatura × 100 (°C)
     *   [3-4]   uint16 CO          × 10  (ppm)
     *   [5-6]   uint16 PM2.5       × 10  (µg/m³)
     *   [7-8]   uint16 PM10        × 10  (µg/m³)
     *   [9-10]  uint16 VOC index   × 10
     *   [11-14] uint32 gas resistance    (Ohm)
     */
    struct {
        uint8_t  alert_mask;
        int16_t  temp_cdeg;
        uint16_t co_ppm_x10;
        uint16_t pm2_5_x10;
        uint16_t pm10_0_x10;
        uint16_t voc_x10;
        uint32_t gas_ohm;
    } __packed alert;

    /*
     * Payload de DIAGNOSTICO de arranque (FPORT_DIAG, 13 bytes, little-endian).
     * Se envia UNA vez por arranque. Responde a "¿por que ha vuelto a arrancar
     * este nodo y que firmware lleva?", que hoy no se puede contestar desde el
     * servidor.
     *   [0]     uint8  msg_type = 1 (BOOT)
     *   [1]     uint8  reset_code (0 desconocida, 1 POR, 2 PIN, 3 SW,
     *                              4 WDT, 5 low-power, 6 CPU lockup, 7 brownout)
     *   [2-5]   uint32 reset_raw  (mascara cruda de hwinfo, por si hay combinaciones)
     *   [6-9]   uint32 boot_count (persistente en NVS: detecta reinicios mudos)
     *   [10-11] uint16 fw_version (FW_VERSION)
     *   [12]    uint8  sensors_ok (mismos bits que el byte 0 del FPort 2:
     *                              bit0 BM688, bit1 CO, bit3 SEN65)
     *   [13]    uint8  soc_reason (esp_reset_reason() en crudo; hwinfo no
     *                              cubre todas las causas — ver arriba)
     */
    struct {
        uint8_t  msg_type;
        uint8_t  reset_code;
        uint32_t reset_raw;
        uint32_t boot_count;
        uint16_t fw_version;
        uint8_t  sensors_ok;
        uint8_t  soc_reason;
    } __packed diag = {
        .msg_type   = 1,
        .reset_code = reset_code,
        .reset_raw  = reset_raw,
        .boot_count = boot_count,
        .fw_version = FW_VERSION,
        .sensors_ok = (uint8_t)((bm688_dev  != NULL ? FLAG_BM688_OK : 0) |
                                (co_dev     != NULL ? FLAG_CO_OK    : 0) |
                                (sen65_dev  != NULL ? FLAG_SEN65_OK : 0)),
        .soc_reason = (uint8_t)soc_reason,
    };

    /*
     * Payload de FALLO DE SENSOR (FPORT_DIAG, msg_type 2, 14 bytes, LE).
     * Convierte la degradacion SILENCIOSA en degradacion ANUNCIADA.
     *   [0]     uint8  msg_type = 2 (FAULT)
     *   [1]     uint8  faulted   — quien esta en fallo AHORA (verdad de campo,
     *                              util aunque se pierda un reporte anterior)
     *   [2]     uint8  went_down — transiciones OK->FALLO desde el ultimo reporte
     *   [3]     uint8  came_up   — transiciones FALLO->OK desde el ultimo reporte
     *   [4-7]   uint32 uptime_s
     *   [8-9]   uint16 fallos totales BM688   (desde el arranque)
     *   [10-11] uint16 fallos totales ZE15-CO
     *   [12-13] uint16 fallos totales SEN65
     * Los tres mask usan los mismos bits que el byte 0 del FPort 2:
     * bit0 BM688, bit1 ZE15-CO, bit3 SEN65.
     */
    struct {
        uint8_t  msg_type;
        uint8_t  faulted;
        uint8_t  went_down;
        uint8_t  came_up;
        uint32_t uptime_s;
        uint16_t bm_fails;
        uint16_t co_fails;
        uint16_t sen_fails;
    } __packed fault = { .msg_type = 2 };

    /* El formato en el aire es un contrato con el decoder de ChirpStack: si
       el compilador metiese padding, el servidor decodificaria basura sin que
       nada fallase visiblemente. Que rompa la compilacion, no el diagnostico. */
    BUILD_ASSERT(sizeof(diag)  == 14, "FPORT_DIAG BOOT debe ocupar 14 bytes");
    BUILD_ASSERT(sizeof(fault) == 14, "FPORT_DIAG FAULT debe ocupar 14 bytes");

    /* El DIAG de arranque es de MINIMA prioridad: nunca debe robarle airtime
       al FPort 2, a una alerta ni a una señal de averia. Va CONFIRMED porque
       es un evento unico e irrepetible (si se pierde, la causa del reinicio se
       pierde con el), y por eso 'ret == 0' aqui significa ACK recibido, no
       solo "transmitido". */
    bool    diag_pending  = true;
    int64_t last_diag_ms  = 0;
    int     diag_attempts = 0;

    /* Snapshot que se publica al portal en cada ciclo (lectura instantanea). */
    struct portal_sensors ps;

    /*
     * Acumuladores para el PROMEDIO del uplink. Cada lectura valida suma aqui;
     * al enviar se divide entre el nº de muestras (bm_n/co_n/sen_n) y se
     * resetea la ventana. Asi el uplink lleva la media del periodo, no la
     * ultima foto. El portal sigue mostrando la lectura instantanea (ps).
     *
     * Cadencias DESACOPLADAS: sensores/portal cada SENSOR_READ_PERIOD_S;
     * envio LoRa cada LORA_SEND_PERIOD_S (ver la CONFIGURACION DE TIEMPOS
     * arriba del fichero para cambiarlas).
     */
    struct {
        double   temp_sum, hum_sum, press_sum, gas_sum; /* BM688 */
        uint32_t bm_n;
        double   co_sum;                                /* ZE15-CO */
        uint32_t co_n;
        bool     co_fault_seen;
        double   pm1_sum, pm25_sum, pm4_sum, pm10_sum;  /* SEN65 */
        double   shum_sum, stemp_sum, voc_sum, nox_sum;
        uint32_t sen_n;
    } acc;
    memset(&acc, 0, sizeof(acc));

    int64_t last_send_ms = 0;

    /* Salud por sensor. Un sensor que no llego a inicializarse nace YA en
       fallo: si no, la mascara 'faulted' del reporte lo daria por sano
       simplemente porque nunca se le lee. Eso no genera transicion (nunca
       estuvo OK); su ausencia la reporta el mensaje de arranque. */
    struct sensor_health h_bm  = { .faulted = (bm688_dev == NULL) };
    struct sensor_health h_co  = { .faulted = (co_dev    == NULL) };
    struct sensor_health h_sen = { .faulted = (sen65_dev == NULL) };

    /* Transiciones acumuladas aun sin reportar. Se acumulan (|=) en vez de
       sobrescribirse: si un sensor cae y otro se recupera antes de que la
       radio quede libre, el reporte lleva ambas cosas. */
    uint8_t fault_down_pending = 0;   /* OK   -> FALLO */
    uint8_t fault_up_pending   = 0;   /* FALLO-> OK    */
    int64_t last_fault_ms      = 0;

    /* Pausa con backoff del ZE15-CO. 'co_retry_at_ms' = instante a partir del
       cual se vuelve a intentar leer; 0 = leer ya. */
    int64_t  co_retry_at_ms = 0;
    uint32_t co_backoff_s   = CO_RETRY_MIN_S;

    /* Hora de red (DeviceTimeReq) para la franja nocturna del SoftAP. La
       peticion viaja PIGGYBACK en el siguiente uplink, no gasta airtime. */
    int64_t last_time_req_ms = 0;
    bool    time_ever_synced = false;

    /* Supervision del enlace: cuenta de envios de datos (para el keepalive) y
       de keepalives consecutivos sin ACK (para disparar el rejoin). */
    uint32_t data_send_count = 0;
    int      link_fails = 0;

    /* Estado de las alertas por umbral: 'armed' = que umbrales pueden disparar
       (flanco de subida); 'pending' = alertas cruzadas aun sin enviar (p.ej.
       en cooldown o tras un -111). Todos armados al arrancar. */
    uint8_t alert_armed   = 0xFF;
    uint8_t alert_pending = 0;
    int64_t last_alert_ms = 0;

    /* Ventana deslizante de temperatura para el rate-of-rise EN 54-5:
       guarda (tiempo, temperatura) de las ultimas ~TH_ROR_WINDOW_S y calcula
       el ritmo (C/min) entre la muestra mas antigua y la mas reciente. */
#define ROR_N ((TH_ROR_WINDOW_S / SENSOR_READ_PERIOD_S) + 1)
    int64_t ror_ms[ROR_N];
    double  ror_temp[ROR_N];
    int     ror_count = 0;

    while (1) {
        /* Marca de tiempo del INICIO del ciclo. Hace falta aparte de 'now'
           (que se toma mas abajo, DESPUES de leer los sensores) porque las
           lecturas pueden tardar varios segundos y la pausa del CO hay que
           evaluarla antes de decidir si se lee. */
        const int64_t cycle_start_ms = k_uptime_get();

        /* "Sigo vivo" para el perro guardian. Va lo PRIMERO del ciclo: marca
           que el lazo avanza, no que los envios funcionen. */
        nodewdt_alive();

        /* Boton de emergencia: si el portal pidio SOS, enviar uplink inmediato
           en FPort 3 (mensaje "SOS", no datos). Latencia <= 1 ciclo (~5 s). */
        if (portal_take_sos()) {
            static const uint8_t sos_msg[] = { 'S', 'O', 'S' };
            int sret = lorawan_send(FPORT_SOS, (uint8_t *)sos_msg, sizeof(sos_msg),
                                    LORAWAN_MSG_UNCONFIRMED);
            printk("SOS enviado (FPort %d): %d\n", FPORT_SOS, sret);
        }

        ps.bm688_valid = false;
        ps.co_valid    = false;
        ps.sen65_valid = false;

        /* BM688 (si esta presente): suma al acumulador + snapshot al portal. */
        if (bm688_dev != NULL) {
            struct bm688_data sd;
            int r = bm688_read_data(bm688_dev, &sd);
            if (r == 0) {
                acc.temp_sum  += sd.temperature;
                acc.hum_sum   += sd.humidity;
                acc.press_sum += sd.pressure;
                acc.gas_sum   += sd.gas_resistance;
                acc.bm_n++;
                ps.bm688_valid    = true;
                ps.temperature    = sd.temperature;
                ps.humidity       = sd.humidity;
                ps.pressure       = sd.pressure;
                ps.gas_resistance = sd.gas_resistance;
            } else {
                printk("BM688 READ ERR: %d\n", r);
            }
            health_update(&h_bm, r == 0, FLAG_BM688_OK,
                          &fault_down_pending, &fault_up_pending);
        }

        /* ZE15-CO (si esta presente) -> fluye al portal aunque falte el BM688.
           Tras FAULT_CONSEC_FAILS fallos seguidos la lectura se PAUSA con
           backoff exponencial (ver CO_RETRY_* arriba), nunca se abandona: el
           criterio de CO tiene que poder volver solo. */
        if (co_dev != NULL && cycle_start_ms >= co_retry_at_ms) {
            struct ze15co_data cd;
            int cr = ze15co_read(co_dev, &cd);
            bool co_ok = (cr == 0 && !cd.sensor_fault);

            if (co_ok) {
                acc.co_sum += cd.co_ppm;
                acc.co_n++;
                ps.co_valid = true;
                ps.co_ppm   = cd.co_ppm;
                co_backoff_s = CO_RETRY_MIN_S;   /* sano: reintento rapido otra vez */
            } else if (cr == 0 && cd.sensor_fault) {
                acc.co_fault_seen = true;  /* sensor reporta fallo interno */
            }
            health_update(&h_co, co_ok, FLAG_CO_OK,
                          &fault_down_pending, &fault_up_pending);

            if (!co_ok && h_co.consec_fails >= FAULT_CONSEC_FAILS) {
                co_retry_at_ms = k_uptime_get() + (int64_t)co_backoff_s * 1000;
                printk("ZE15-CO: %u fallos seguidos, pausa de %u s "
                       "(se reintentara solo)\n",
                       (unsigned int)h_co.consec_fails,
                       (unsigned int)co_backoff_s);
                co_backoff_s = MIN(co_backoff_s * 2U, (uint32_t)CO_RETRY_MAX_S);
            }
        }

        /* SEN65 (si esta presente): calidad de aire en su bus propio I2C1
           (GPIO15/16); conserva la ultima lectura si aun no hay dato nuevo. */
        if (sen65_dev != NULL) {
            struct sen6x_data ad;
            int ar = sen6x_read(sen65_dev, &ad);
            if (ar == 0) {
                acc.pm1_sum   += ad.pm1_0;
                acc.pm25_sum  += ad.pm2_5;
                acc.pm4_sum   += ad.pm4_0;
                acc.pm10_sum  += ad.pm10_0;
                acc.shum_sum  += ad.humidity;
                acc.stemp_sum += ad.temperature;
                acc.voc_sum   += ad.voc_index;
                acc.nox_sum   += ad.nox_index;
                acc.sen_n++;
                ps.sen65_valid = true;
                ps.pm1_0       = ad.pm1_0;
                ps.pm2_5       = ad.pm2_5;
                ps.pm4_0       = ad.pm4_0;
                ps.pm10_0      = ad.pm10_0;
                ps.voc_index   = ad.voc_index;
                ps.nox_index   = ad.nox_index;
                ps.sen65_temp  = ad.temperature;
                ps.sen65_hum   = ad.humidity;
            } else {
                printk("SEN65 READ ERR: %d\n", ar);
            }
            health_update(&h_sen, ar == 0, FLAG_SEN65_OK,
                          &fault_down_pending, &fault_up_pending);
        }

        /* Portal: siempre actualizado (lo sirve /api/sensors). */
        portal_update_sensors(&ps);

        int64_t now = k_uptime_get();

        /* ---- Hora de red y franja nocturna del SoftAP ------------------
         * La unica fuente de hora del nodo es el DeviceTimeReq de LoRaWAN
         * (no hay RTC con pila, y el SoftAP no da salida a Internet). Se pide
         * con force_request=false: el comando MAC viaja piggyback en el
         * siguiente uplink de datos, sin gastar airtime ni duty-cycle propios.
         */
        if (last_time_req_ms == 0 ||
            (now - last_time_req_ms) >= (int64_t)TIME_RESYNC_H * 3600 * 1000) {
            int tr = lorawan_request_device_time(false);
            if (tr < 0) {
                printk("DeviceTimeReq err: %d\n", tr);
            }
            last_time_req_ms = now;
        }

        uint32_t gps_now = 0;
        struct wallclock_tm lt;
        bool have_time = (lorawan_device_time_get(&gps_now) == 0) &&
                         wallclock_from_gps(gps_now, &lt);

        if (have_time && !time_ever_synced) {
            time_ever_synced = true;
            printk("HORA de red: %04d-%02u-%02u %02u:%02u local (%s)\n",
                   lt.year, lt.month, lt.day, lt.hour, lt.min,
                   lt.dst ? "verano UTC+2" : "invierno UTC+1");
        }

        /*
         * FAIL-SAFE: sin hora fiable, el AP se queda ENCENDIDO. No hay
         * anulacion manual, asi que apagarlo por una suposicion dejaria el
         * portal inaccesible sin forma de recuperarlo salvo reiniciando el
         * nodo fisicamente. El fallo debe caer del lado de la disponibilidad.
         *
         * portal_ap_set() es idempotente y NO actualiza su estado interno si
         * el driver falla, asi que llamarla en cada ciclo reintenta sola una
         * reactivacion fallida — que es el escenario grave (AP que no vuelve).
         */
        bool ap_should_be_on = !have_time || !ap_in_off_window(lt.hour);
        (void)portal_ap_set(ap_should_be_on);

        /* Rate-of-rise termico (EN 54-5): mete (now, temp) en la ventana
           deslizante y calcula el ritmo en C/min entre la muestra mas antigua
           y la actual. Solo con dato fresco del BM688. */
        double temp_ror_cpmin = 0.0;
        bool   ror_ready = false;
        if (ps.bm688_valid) {
            if (ror_count < ROR_N) {
                ror_ms[ror_count]   = now;
                ror_temp[ror_count] = ps.temperature;
                ror_count++;
            } else {
                memmove(ror_ms,   ror_ms + 1,   (ROR_N - 1) * sizeof(ror_ms[0]));
                memmove(ror_temp, ror_temp + 1, (ROR_N - 1) * sizeof(ror_temp[0]));
                ror_ms[ROR_N - 1]   = now;
                ror_temp[ROR_N - 1] = ps.temperature;
            }
            if (ror_count >= ROR_N) {
                double dt_min = (double)(ror_ms[ROR_N - 1] - ror_ms[0]) / 60000.0;
                if (dt_min > 0.0) {
                    temp_ror_cpmin = (ror_temp[ROR_N - 1] - ror_temp[0]) / dt_min;
                    ror_ready = true;
                }
            }
        }

        /* ---- Alertas por umbral (FPORT_ALERT) --------------------------
           Compara la lectura INSTANTANEA con los umbrales configurados. Al
           cruzar (flanco de subida) marca la alerta como pendiente; se re-arma
           cuando el valor se aleja del umbral (histeresis). El envio respeta
           un cooldown para no chocar con el duty-cycle (salvo FUEGO). */
        uint8_t alert_active = 0;
        uint8_t alert_fired  = 0;

#define TH_CHECK_MAX(en, ok, val, thr, bit) do {                              \
            if ((en) && (ok)) {                                               \
                if ((val) >= (thr)) {                                         \
                    alert_active |= (bit);                                    \
                    if (alert_armed & (bit)) {                                \
                        alert_fired |= (bit); alert_armed &= ~(bit);          \
                    }                                                         \
                } else if ((val) < (thr) * (1.0 - TH_HYSTERESIS_PCT / 100.0)) { \
                    alert_armed |= (bit);                                     \
                }                                                             \
            }                                                                 \
        } while (0)
#define TH_CHECK_MIN(en, ok, val, thr, bit) do {                              \
            if ((en) && (ok)) {                                               \
                if ((val) <= (thr)) {                                         \
                    alert_active |= (bit);                                    \
                    if (alert_armed & (bit)) {                                \
                        alert_fired |= (bit); alert_armed &= ~(bit);          \
                    }                                                         \
                } else if ((val) > (thr) * (1.0 + TH_HYSTERESIS_PCT / 100.0)) { \
                    alert_armed |= (bit);                                     \
                }                                                             \
            }                                                                 \
        } while (0)
#define TH_CHECK_BOOL(en, cond, bit) do {                                     \
            if (en) {                                                         \
                if (cond) {                                                   \
                    alert_active |= (bit);                                    \
                    if (alert_armed & (bit)) {                                \
                        alert_fired |= (bit); alert_armed &= ~(bit);          \
                    }                                                         \
                } else {                                                      \
                    alert_armed |= (bit);                                     \
                }                                                             \
            }                                                                 \
        } while (0)

        TH_CHECK_MAX(TH_TEMP_EN, ps.bm688_valid, ps.temperature,    TH_TEMP_MAX, ALERT_TEMP);
        TH_CHECK_MAX(TH_CO_EN,   ps.co_valid,     ps.co_ppm,         TH_CO_MAX,   ALERT_CO);
        TH_CHECK_MAX(TH_PM25_EN, ps.sen65_valid,  ps.pm2_5,          TH_PM25_MAX, ALERT_PM25);
        TH_CHECK_MAX(TH_PM10_EN, ps.sen65_valid,  ps.pm10_0,         TH_PM10_MAX, ALERT_PM10);
        TH_CHECK_MAX(TH_VOC_EN,  ps.sen65_valid,  ps.voc_index,      TH_VOC_MAX,  ALERT_VOC);
        TH_CHECK_MIN(TH_GAS_EN,  ps.bm688_valid,  ps.gas_resistance, TH_GAS_MIN,  ALERT_GAS);

        /* EN 54-5 rate-of-rise: subida rapida de temperatura (aunque no llegue
           al umbral fijo). */
        TH_CHECK_BOOL(TH_ROR_EN && ror_ready, temp_ror_cpmin >= TH_ROR_CPMIN,
                      ALERT_HEAT_ROR);

        /* EN 54-30/31 multicriterio: FUEGO si coinciden >= FIRE_MIN_CRITERIA
           familias de indicio (HUMO / CO / CALOR), sobre el estado activo. */
        {
            int fam_smoke = (alert_active & (ALERT_PM25 | ALERT_PM10))    ? 1 : 0;
            int fam_co    = (alert_active &  ALERT_CO)                    ? 1 : 0;
            int fam_heat  = (alert_active & (ALERT_TEMP | ALERT_HEAT_ROR))? 1 : 0;
            int criteria  = fam_smoke + fam_co + fam_heat;
            TH_CHECK_BOOL(1, criteria >= FIRE_MIN_CRITERIA, ALERT_FIRE);
        }

#undef TH_CHECK_MAX
#undef TH_CHECK_MIN
#undef TH_CHECK_BOOL

        alert_pending |= alert_fired;

        /* Al DISPARAR (flanco) se capturan los valores de ESE instante en el
           payload de alerta. Asi el mask y los valores del uplink corresponden
           al evento que cruzo, aunque el envio se retrase por el cooldown y la
           medida ya haya bajado (evita mandar una alerta con valores ya
           recuperados). */
        if (alert_fired != 0) {
            alert.temp_cdeg  = (int16_t)(ps.temperature * 100.0);
            alert.co_ppm_x10 = (uint16_t)(ps.co_ppm * 10.0);
            alert.pm2_5_x10  = (uint16_t)(ps.pm2_5 * 10.0);
            alert.pm10_0_x10 = (uint16_t)(ps.pm10_0 * 10.0);
            alert.voc_x10    = (uint16_t)(ps.voc_index * 10.0);
            alert.gas_ohm    = (uint32_t)ps.gas_resistance;
        }

        /* Un FUEGO recien confirmado (EN 54-30/31) es critico -> se envia YA,
           ignorando el cooldown. El resto de alertas respetan el cooldown. */
        bool fire_now = (alert_fired & ALERT_FIRE) != 0;

        /* =============================================================
         *  PRIORIDAD DE RADIO: FPort 2 (datos) SIEMPRE PRIMERO
         * =============================================================
         * Todo comparte la MISMA radio y el MISMO presupuesto de
         * duty-cycle EU868 (1%). Los tres canales (SOS/alerta/datos) son
         * envios BLOQUEANTES y secuenciales en este unico hilo. Si una
         * alerta (FPort 4) consume el airtime justo antes de un envio de
         * datos programado, el FPort 2 se rechazaria por duty-cycle
         * (-EAGAIN/"-111") y se perderia ese periodo entero.
         *
         * Regla: si toca enviar datos en este ciclo, el FPort 2 reclama
         * el airtime PRIMERO; la alerta se aplaza (queda en alert_pending
         * y sale en un ciclo posterior). Unica excepcion: un FUEGO
         * confirmado, que por criticidad intenta salir igualmente.
         * Asi el FPort 4 NUNCA puede quitarle el slot al FPort 2.
         * ============================================================= */
        bool data_due = (last_send_ms == 0 ||
                         (now - last_send_ms) >= (int64_t)LORA_SEND_PERIOD_S * 1000);

        /* ---- 1) DATOS (FPort 2) — MAXIMA PRIORIDAD ------------------- */
        if (data_due) {
            memset(&payload, 0, sizeof(payload));

            if (acc.bm_n > 0) {
                payload.bm_temp_cdeg  = (int16_t)((acc.temp_sum  / acc.bm_n) * 100.0);
                payload.bm_hum_cpct   = (uint16_t)((acc.hum_sum  / acc.bm_n) * 100.0);
                payload.bm_press_dhpa = (uint16_t)((acc.press_sum / acc.bm_n) / 10.0);
                payload.bm_gas_ohm    = (uint32_t)(acc.gas_sum   / acc.bm_n);
                payload.flags |= FLAG_BM688_OK;
            }
            if (acc.co_n > 0) {
                payload.co_ppm_x10 = (uint16_t)((acc.co_sum / acc.co_n) * 10.0);
                payload.flags |= FLAG_CO_OK;
            }
            if (acc.co_fault_seen) {
                payload.flags |= FLAG_CO_FAULT;
            }
            if (portal_ap_is_on()) {
                payload.flags |= FLAG_AP_ON;
            }
            if (acc.sen_n > 0) {
                payload.pm1_0_x10     = (uint16_t)((acc.pm1_sum  / acc.sen_n) * 10.0);
                payload.pm2_5_x10     = (uint16_t)((acc.pm25_sum / acc.sen_n) * 10.0);
                payload.pm4_0_x10     = (uint16_t)((acc.pm4_sum  / acc.sen_n) * 10.0);
                payload.pm10_0_x10    = (uint16_t)((acc.pm10_sum / acc.sen_n) * 10.0);
                payload.sen_hum_cpct  = (uint16_t)((acc.shum_sum / acc.sen_n) * 100.0);
                payload.sen_temp_cdeg = (int16_t)((acc.stemp_sum / acc.sen_n) * 100.0);
                payload.voc_x10       = (uint16_t)((acc.voc_sum  / acc.sen_n) * 10.0);
                payload.nox_x10       = (uint16_t)((acc.nox_sum  / acc.sen_n) * 10.0);
                payload.flags |= FLAG_SEN65_OK;
            }

            /* Keepalive: 1 de cada LINK_KEEPALIVE_EVERY envios va CONFIRMED
               para obtener ACK y comprobar que el enlace sigue vivo. El resto
               UNCONFIRMED (mas barato, sin espera de ACK). */
            data_send_count++;
            bool keepalive = (data_send_count % LINK_KEEPALIVE_EVERY) == 0;

            printk("SEND avg (bm=%u co=%u sen=%u muestras)%s\n",
                   acc.bm_n, acc.co_n, acc.sen_n,
                   keepalive ? " [keepalive/CONFIRMED]" : "");

            ret = lorawan_send(FPORT_DATA, (uint8_t *)&payload, sizeof(payload),
                               keepalive ? LORAWAN_MSG_CONFIRMED
                                         : LORAWAN_MSG_UNCONFIRMED);
            if (ret < 0) {
                /* -111 (duty-cycle) es normal/regulatorio en EU868, no fatal. */
                printk("SEND ERR: %d\n", ret);
            } else {
                printk("SEND OK\n");
            }
            last_send_ms = now;
            memset(&acc, 0, sizeof(acc));   /* nueva ventana de promediado */

            /* Prueba de vida del enlace: un keepalive CONFIRMED que devuelve 0
               = ACK recibido = enlace vivo. Si falla, cuenta; tras LINK_MAX_FAILS
               seguidos se asume enlace muerto y se hace REJOIN (a SF12). */
            if (keepalive) {
                if (ret == 0) {
                    if (link_fails > 0) {
                        printk("ENLACE OK (keepalive con ACK), fails=0\n");
                    }
                    link_fails = 0;
                } else {
                    link_fails++;
                    printk("KEEPALIVE sin ACK (%d/%d) ret=%d\n",
                           link_fails, LINK_MAX_FAILS, ret);
                    if (link_fails >= LINK_MAX_FAILS) {
                        printk("ENLACE CAIDO -> REJOIN\n");
                        for (int a = 1; a <= 3; a++) {
                            /* Reunirse es PROGRESO, no un cuelgue: cada
                               lorawan_join() bloquea y encadenados podrian
                               pasar del umbral de estancamiento. */
                            nodewdt_alive();
                            int jr = lorawan_join(&join_cfg);
                            if (jr == 0) {
                                printk("REJOIN OK (intento %d)\n", a);
                                break;
                            }
                            printk("REJOIN ERR %d (intento %d/3)\n", jr, a);
                            k_sleep(K_SECONDS(10));
                        }
                        link_fails = 0;   /* re-armar; si sigue mal, reintenta */
                    }
                }
            }
        }

        /* ---- 2) ALERTAS (FPort 4) — SECUNDARIO, NO BLOQUEANTE -------
         * Se envia DESPUES de los datos y solo si este ciclo NO tocaba
         * enviar datos (protege el slot de duty-cycle del FPort 2). Un
         * FUEGO confirmado es la unica excepcion: intenta salir aunque
         * coincida con un envio de datos.
         *
         * Nunca bloquea al FPort 2: si el envio de alerta falla (p.ej.
         * -111 duty-cycle), la alerta queda PENDIENTE (alert_pending) y
         * se reintenta en un ciclo posterior; el bucle de lectura y el
         * envio periodico de datos siguen su marcha con normalidad.
         */
        bool alert_slot_ok = (!data_due || fire_now);
        bool cooldown_ok   = (fire_now || last_alert_ms == 0 ||
                              (now - last_alert_ms) >= (int64_t)ALERT_MIN_INTERVAL_S * 1000);

        if (alert_pending != 0 && alert_slot_ok && cooldown_ok) {
            /* El mask reporta que umbrales CRUZARON en la ventana (pending),
               nunca 0; alert_active es solo informativo (que sigue alto ahora). */
            alert.alert_mask = alert_pending;

            int aret = lorawan_send(FPORT_ALERT, (uint8_t *)&alert,
                                    sizeof(alert), LORAWAN_MSG_UNCONFIRMED);
            printk("ALERTA%s disparo=0x%02x activo_ahora=0x%02x -> FPort %d: %d\n",
                   (alert_pending & ALERT_FIRE) ? " [FUEGO]" : "",
                   alert_pending, alert_active, FPORT_ALERT, aret);
            if (aret == 0) {
                alert_pending = 0;      /* enviada: limpia lo pendiente */
                last_alert_ms = now;
            }
            /* si falla queda pendiente y reintenta luego, SIN tocar el FPort 2 */
        } else if (alert_pending != 0) {
            /* Diagnostico: por que se aplaza la alerta (nunca se pierde). */
            printk("ALERTA aplazada pend=0x%02x (data_due=%d cooldown_ok=%d)\n",
                   alert_pending, (int)data_due, (int)cooldown_ok);
        }

        /* ---- 3) FALLO DE SENSOR (FPort 5, msg_type 2) ----------------
         * Se envia cuando algun sensor CAE o se RECUPERA. Va por debajo de
         * los datos y de las alarmas (una alarma de incendio manda sobre una
         * señal de averia, igual que en EN 54), pero por encima del DIAG de
         * arranque y con una separacion mucho mas corta: una averia es un
         * evento de seguridad y debe salir pronto.
         *
         * CONFIRMED y pendiente hasta que hay ACK: si esta señal se pierde,
         * el sistema vuelve a estar degradado en silencio, que es justo el
         * defecto que este canal existe para eliminar.
         */
        bool fault_pending = (fault_down_pending | fault_up_pending) != 0;

        if (fault_pending && !data_due && alert_pending == 0 &&
            (now - last_send_ms) >= (int64_t)FAULT_MIN_SPACING_S * 1000 &&
            (last_fault_ms == 0 ||
             (now - last_fault_ms) >= (int64_t)FAULT_MIN_SPACING_S * 1000)) {

            last_fault_ms = now;

            fault.faulted   = (uint8_t)((h_bm.faulted  ? FLAG_BM688_OK : 0) |
                                        (h_co.faulted  ? FLAG_CO_OK    : 0) |
                                        (h_sen.faulted ? FLAG_SEN65_OK : 0));
            fault.went_down = fault_down_pending;
            fault.came_up   = fault_up_pending;
            fault.uptime_s  = (uint32_t)(now / 1000);
            fault.bm_fails  = h_bm.total_fails;
            fault.co_fails  = h_co.total_fails;
            fault.sen_fails = h_sen.total_fails;

            int fret = lorawan_send(FPORT_DIAG, (uint8_t *)&fault,
                                    sizeof(fault), LORAWAN_MSG_CONFIRMED);
            printk("FALLO caidos=0x%02x recuperados=0x%02x en_fallo_ahora=0x%02x"
                   " -> FPort %d: %d\n",
                   fault.went_down, fault.came_up, fault.faulted,
                   FPORT_DIAG, fret);

            if (fret == 0) {
                /* Solo se limpia lo que se acaba de reportar: si una nueva
                   transicion ocurrio durante el envio, no se pierde. */
                fault_down_pending &= ~fault.went_down;
                fault_up_pending   &= ~fault.came_up;
            }
        }

        /* ---- 4) DIAGNOSTICO DE ARRANQUE (FPort 5) — MINIMA PRIORIDAD -
         * Sale UNA vez por arranque y solo cuando la radio esta libre: ni
         * este ciclo tocaba enviar datos, ni hay alerta o fallo pendiente,
         * y ha pasado DIAG_MIN_SPACING_S desde el ultimo envio de datos y
         * desde el intento anterior. Asi el instrumento de diagnostico no
         * puede degradar jamas al canal critico, a una alarma ni a una
         * señal de averia.
         */
        if (diag_pending && !data_due && alert_pending == 0 && !fault_pending &&
            (now - last_send_ms) >= (int64_t)DIAG_MIN_SPACING_S * 1000 &&
            (last_diag_ms == 0 ||
             (now - last_diag_ms) >= (int64_t)DIAG_MIN_SPACING_S * 1000)) {

            last_diag_ms = now;
            diag_attempts++;

            int dret = lorawan_send(FPORT_DIAG, (uint8_t *)&diag, sizeof(diag),
                                    LORAWAN_MSG_CONFIRMED);
            printk("DIAG arranque #%u causa=%s fw=0x%04X -> FPort %d: %d\n",
                   diag.boot_count, reset_cause_str(diag.reset_code),
                   FW_VERSION, FPORT_DIAG, dret);

            if (dret == 0) {
                diag_pending = false;   /* CONFIRMED con ACK: entregado */
            } else if (diag_attempts >= DIAG_MAX_ATTEMPTS) {
                printk("DIAG: %d intentos sin ACK, se abandona "
                       "(boot_count lo reportara el proximo arranque)\n",
                       diag_attempts);
                diag_pending = false;
            }
        }

        k_sleep(K_SECONDS(SENSOR_READ_PERIOD_S));
    }
}
