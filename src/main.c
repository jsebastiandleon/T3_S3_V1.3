#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <string.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/lorawan/lorawan.h>
#include <zephyr/sys/printk.h>
#include "sensors/bm688.h"
#include "sensors/ze15_co.h"
#include "sensors/sen6x.h"
#include "portal/portal.h"

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

    for (int attempt = 1; ; attempt++) {
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

    /* Snapshot que se publica al portal en cada ciclo. */
    struct portal_sensors ps;

    /*
     * Cadencias DESACOPLADAS:
     *  - Sensores + portal: cada SENSOR_PERIOD_S -> dashboard "vivo" en el AP,
     *    independiente de la radio.
     *  - Envio LoRa: cada LORA_PERIOD_S. En EU868 a DR0/SF12 un uplink de ~27 B
     *    dura ~1.5 s de airtime; el 1% de duty-cycle obliga a ~150 s entre
     *    paquetes. Enviar mas a menudo solo produce -EMSGSIZE/duty-cycle (-111).
     *    Cuando el ADR suba el DR, podra acelerarse solo.
     */
    const int SENSOR_PERIOD_S = 5;
    const int LORA_PERIOD_S   = 180;
    int64_t last_send_ms = 0;
    int co_fails = 0;   /* tras 3 fallos seguidos se deja de leer el CO */

    while (1) {
        /* Boton de emergencia: si el portal pidio SOS, enviar uplink inmediato
           en FPort 3 (mensaje "SOS", no datos). Latencia <= 1 ciclo (~5 s). */
        if (portal_take_sos()) {
            static const uint8_t sos_msg[] = { 'S', 'O', 'S' };
            int sret = lorawan_send(3, (uint8_t *)sos_msg, sizeof(sos_msg),
                                    LORAWAN_MSG_UNCONFIRMED);
            printk("SOS enviado (FPort 3): %d\n", sret);
        }

        memset(&payload, 0, sizeof(payload));
        ps.bm688_valid = false;
        ps.co_valid    = false;
        ps.sen65_valid = false;

        /* BM688 (si esta presente) */
        if (bm688_dev != NULL) {
            struct bm688_data sd;
            int r = bm688_read_data(bm688_dev, &sd);
            if (r == 0) {
                payload.bm_temp_cdeg  = (int16_t)(sd.temperature * 100.0);
                payload.bm_hum_cpct   = (uint16_t)(sd.humidity   * 100.0);
                payload.bm_press_dhpa = (uint16_t)(sd.pressure / 10.0);
                payload.bm_gas_ohm    = (uint32_t)(sd.gas_resistance);
                payload.flags |= FLAG_BM688_OK;
                ps.bm688_valid    = true;
                ps.temperature    = sd.temperature;
                ps.humidity       = sd.humidity;
                ps.pressure       = sd.pressure;
                ps.gas_resistance = sd.gas_resistance;
            } else {
                printk("BM688 READ ERR: %d\n", r);
            }
        }

        /* ZE15-CO (si esta presente) -> fluye al portal aunque falte el BM688.
           Si falla 3 veces seguidas (p.ej. sin sensor) se deja de leer para no
           inundar el log ni bloquear el lazo ~4.5 s por ciclo. */
        if (co_dev != NULL) {
            struct ze15co_data cd;
            int cr = ze15co_read(co_dev, &cd);
            if (cr == 0 && !cd.sensor_fault) {
                payload.co_ppm_x10 = (uint16_t)(cd.co_ppm * 10.0);
                payload.flags |= FLAG_CO_OK;
                ps.co_valid = true;
                ps.co_ppm   = cd.co_ppm;
                co_fails = 0;
            } else {
                if (cr == 0 && cd.sensor_fault) {
                    payload.flags |= FLAG_CO_FAULT;  /* sensor reporta fallo interno */
                }
                if (++co_fails >= 3) {
                    printk("ZE15-CO: 3 fallos seguidos, se deshabilita la lectura\n");
                    co_dev = NULL;
                }
            }
        }

        /* SEN65 (si esta presente): calidad de aire en su bus propio I2C1
           (GPIO15/16); conserva la ultima lectura si aun no hay dato nuevo. */
        if (sen65_dev != NULL) {
            struct sen6x_data ad;
            int ar = sen6x_read(sen65_dev, &ad);
            if (ar == 0) {
                payload.pm1_0_x10    = (uint16_t)(ad.pm1_0  * 10.0);
                payload.pm2_5_x10    = (uint16_t)(ad.pm2_5  * 10.0);
                payload.pm4_0_x10    = (uint16_t)(ad.pm4_0  * 10.0);
                payload.pm10_0_x10   = (uint16_t)(ad.pm10_0 * 10.0);
                payload.sen_hum_cpct  = (uint16_t)(ad.humidity    * 100.0);
                payload.sen_temp_cdeg = (int16_t)(ad.temperature * 100.0);
                payload.voc_x10      = (uint16_t)(ad.voc_index * 10.0);
                payload.nox_x10      = (uint16_t)(ad.nox_index * 10.0);
                payload.flags |= FLAG_SEN65_OK;
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
        }

        /* Portal: siempre actualizado (lo sirve /api/sensors). */
        portal_update_sensors(&ps);

        /* LoRa: solo en su cadencia, respetando el duty-cycle. */
        int64_t now = k_uptime_get();
        if (last_send_ms == 0 || (now - last_send_ms) >= (int64_t)LORA_PERIOD_S * 1000) {
            ret = lorawan_send(2, (uint8_t *)&payload, sizeof(payload),
                               LORAWAN_MSG_UNCONFIRMED);
            if (ret < 0) {
                /* -111 (duty-cycle) es normal/regulatorio en EU868, no fatal. */
                printk("SEND ERR: %d\n", ret);
            } else {
                printk("SEND OK\n");
            }
            last_send_ms = now;
        }

        k_sleep(K_SECONDS(SENSOR_PERIOD_S));
    }
}
