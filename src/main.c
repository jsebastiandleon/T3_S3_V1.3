#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/lorawan/lorawan.h>
#include <zephyr/sys/printk.h>

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

    uint8_t payload[] = "HELLO";

    while (1) {
        ret = lorawan_send(2, payload, sizeof(payload) - 1,
                           LORAWAN_MSG_UNCONFIRMED);
        if (ret < 0) {
            printk("SEND ERR: %d\n", ret);
        } else {
            printk("SEND OK\n");
        }
        k_sleep(K_SECONDS(30));
    }
}
