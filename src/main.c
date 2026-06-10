#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/lorawan/lorawan.h>
#include <zephyr/sys/printk.h>
#include "sensors/bm688.h"

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

/* DevEUI: MAC 1c:db:d4:bd:29:40 -> EUI-64 estandar (insert FF FE) */
#define DEV_EUI  { 0x1C, 0xDB, 0xD4, 0xFF, 0xFE, 0xBD, 0x29, 0x40 }

/* JoinEUI: todos ceros (Chirpstack acepta cualquier valor) */
#define JOIN_EUI { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }

/* AppKey proporcionada */
#define APP_KEY  { 0x06, 0x26, 0x35, 0xAC, 0xC3, 0xBB, 0xC9, 0x2C, \
                   0x2F, 0xEF, 0x99, 0x4F, 0x5E, 0xF0, 0xF6, 0x9B }

static void downlink_cb(uint8_t port, uint8_t flags, int16_t rssi,
                        int8_t snr, uint8_t len, const uint8_t *data)
{
    printk("DL port=%d rssi=%d snr=%d len=%d\n", port, rssi, snr, len);
}

int main(void)
{
    printk("BOOT\n");

    i2c0_scan();

    /* ---- BM688 init ------------------------------------------------- */
    const struct device *bm688_dev = NULL;
    int bm688_ok = bm688_init(&bm688_dev);
    if (bm688_ok < 0) {
        printk("BM688 INIT ERR: %d (continua sin sensor)\n", bm688_ok);
    } else {
        printk("BM688 READY\n");
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

    static struct lorawan_downlink_cb dl = {
        .port = LW_RECV_PORT_ANY,
        .cb   = downlink_cb,
    };
    lorawan_register_downlink_callback(&dl);

    uint8_t dev_eui[]  = DEV_EUI;
    uint8_t join_eui[] = JOIN_EUI;
    uint8_t app_key[]  = APP_KEY;

    struct lorawan_join_config join_cfg = {
        .mode          = LORAWAN_ACT_OTAA,
        .dev_eui       = dev_eui,
        .otaa = {
            .join_eui  = join_eui,
            .app_key   = app_key,
            .nwk_key   = app_key,   /* LoRaWAN 1.0.x: nwk_key == app_key */
            .dev_nonce = 0,
        },
    };

    for (int attempt = 1; ; attempt++) {
        printk("JOINING... attempt %d\n", attempt);
        ret = lorawan_join(&join_cfg);
        if (ret == 0) {
            break;
        }
        printk("JOIN ERR: %d (retry in 10s)\n", ret);
        join_cfg.otaa.dev_nonce++;   /* incrementar para cada intento */
        k_sleep(K_SECONDS(10));
    }
    printk("JOINED!\n");

    /*
     * Payload LoRaWAN con datos del BM688 (12 bytes, little-endian):
     *   [0-1]  int16  temperatura × 100  (ej: 2550 = 25.50 °C)
     *   [2-3]  uint16 humedad × 100      (ej: 6500 = 65.00 %)
     *   [4-7]  uint32 presion en Pa      (ej: 101325)
     *   [8-11] uint32 gas_resistance Ohm (ej: 50000)
     */
    struct {
        int16_t  temp_cdeg;
        uint16_t hum_cpct;
        uint32_t press_pa;
        uint32_t gas_ohm;
    } __packed bm_payload;

    while (1) {
        /* Leer BM688 y construir payload */
        if (bm688_dev != NULL) {
            struct bm688_data sensor_data;
            int r = bm688_read_data(bm688_dev, &sensor_data);
            if (r == 0) {
                bm_payload.temp_cdeg = (int16_t)(sensor_data.temperature * 100.0);
                bm_payload.hum_cpct  = (uint16_t)(sensor_data.humidity    * 100.0);
                bm_payload.press_pa  = (uint32_t)(sensor_data.pressure);
                bm_payload.gas_ohm   = (uint32_t)(sensor_data.gas_resistance);

                printk("BM688 T=%d.%02dC H=%d.%02d%% P=%dPa G=%dOhm\n",
                       (int)sensor_data.temperature,
                       (int)(sensor_data.temperature * 100) % 100,
                       (int)sensor_data.humidity,
                       (int)(sensor_data.humidity * 100) % 100,
                       (int)sensor_data.pressure,
                       (int)sensor_data.gas_resistance);
            } else {
                printk("BM688 READ ERR: %d\n", r);
                /* Payload de error: todos los campos a cero */
                bm_payload.temp_cdeg = 0;
                bm_payload.hum_cpct  = 0;
                bm_payload.press_pa  = 0;
                bm_payload.gas_ohm   = 0;
            }
        } else {
            /* Sensor no disponible: fallback al payload de texto original */
            static const uint8_t fallback[] = "HELLO";
            ret = lorawan_send(2, fallback, sizeof(fallback) - 1,
                               LORAWAN_MSG_UNCONFIRMED);
            if (ret < 0) {
                printk("SEND ERR: %d\n", ret);
            } else {
                printk("SEND OK (fallback)\n");
            }
            k_sleep(K_SECONDS(30));
            continue;
        }

        ret = lorawan_send(2, (uint8_t *)&bm_payload, sizeof(bm_payload),
                           LORAWAN_MSG_UNCONFIRMED);
        if (ret < 0) {
            printk("SEND ERR: %d\n", ret);
        } else {
            printk("SEND OK\n");
        }
        k_sleep(K_SECONDS(30));
    }
}
