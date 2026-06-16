#ifndef PORTAL_PORTAL_H_
#define PORTAL_PORTAL_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Portal cautivo del T3-S3.
 *
 * Arquitectura por capas (todas piezas nativas de Zephyr):
 *   WiFi SoftAP (driver esp32)  -> AP "T3S3-Setup-XXYY"
 *   IPv4 estatica 192.168.4.1   + servidor DHCPv4 (reparte .10+)
 *   responder DNS UDP:53        -> resuelve TODO a 192.168.4.1 (dispara el
 *                                   pop-up de portal del SO)
 *   HTTP_SERVER (:80)           -> sirve el dashboard + /api/sensors
 *
 * Convive de forma concurrente con LoRaWAN + sensores. El HTML del portal
 * es MUTABLE (se guarda en NVS/Settings) y puede actualizarse por downlink
 * LoRa, de ahi que se sirva como recurso dinamico y no compilado.
 */

/* Snapshot de las ultimas lecturas, publicado por el lazo principal y
 * consumido por el handler HTTP /api/sensors. */
struct portal_sensors {
	bool    bm688_valid;
	double  temperature;     /* grados C        */
	double  humidity;        /* %               */
	double  pressure;        /* Pa              */
	double  gas_resistance;  /* Ohm             */
	bool    co_valid;
	double  co_ppm;          /* ppm             */
	int64_t updated_uptime_ms;
};

/* Arranca AP + DHCP + DNS cautivo + HTTP. Idempotente. 0 = OK. */
int portal_start(void);

/* Publica nuevas lecturas (thread-safe) para /api/sensors. */
void portal_update_sensors(const struct portal_sensors *s);

/* Copia atomica del snapshot actual (para el handler HTTP). */
void portal_get_sensors(struct portal_sensors *out);

/* ---- HTML mutable del portal -------------------------------------------- */

/* Tamano maximo del HTML servido/actualizable. */
#define PORTAL_HTML_MAX 4096

/* Carga el HTML desde Settings (NVS); si no existe usa el default empotrado.
 * Lo llama portal_start(); expuesto por claridad. */
int portal_html_init(void);

/* Devuelve el HTML vivo y su longitud (puntero estable a buffer estatico). */
void portal_html_get(const uint8_t **buf, size_t *len);

/* ---- Actualizacion del HTML via downlink LoRa --------------------------- */

/* FPort reservado para el protocolo de actualizacion de HTML por downlink. */
#define PORTAL_HTML_OTA_FPORT 10

/*
 * Protocolo (1 frame por downlink, little-endian en longitudes):
 *   BEGIN  [0x01][len_lo][len_hi][crc_lo][crc_hi]
 *            -> reinicia staging; fija longitud y CRC-16/CCITT esperados.
 *   DATA   [0x02][off_lo][off_hi][bytes...]
 *            -> copia 'bytes' en staging[off]. Admite cualquier MTU/orden.
 *   COMMIT [0x03]
 *            -> si len y CRC cuadran: persiste en Settings y lo pone en vivo.
 *
 * Se invoca desde el downlink_cb cuando port == PORTAL_HTML_OTA_FPORT.
 */
void portal_html_ota_rx(const uint8_t *data, uint8_t len);

#endif /* PORTAL_PORTAL_H_ */
