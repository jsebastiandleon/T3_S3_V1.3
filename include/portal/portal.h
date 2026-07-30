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
	bool    sen65_valid;
	double  pm1_0;           /* ug/m3           */
	double  pm2_5;           /* ug/m3           */
	double  pm4_0;           /* ug/m3           */
	double  pm10_0;          /* ug/m3           */
	double  voc_index;       /* indice VOC      */
	double  nox_index;       /* indice NOx      */
	double  sen65_temp;      /* grados C (SEN65)*/
	double  sen65_hum;       /* %RH (SEN65)     */
	int64_t updated_uptime_ms;
};

/* Arranca AP + DHCP + DNS cautivo + HTTP. Idempotente. 0 = OK. */
int portal_start(void);

/* ---- Encendido/apagado del SoftAP -------------------------------------- */
/*
 * El SoftAP es, con diferencia, el mayor consumidor del nodo (radio WiFi
 * siempre emitiendo balizas). En una instalacion alimentada por panel solar
 * merece la pena apagarlo cuando no hay nadie que pueda usar el portal.
 *
 * portal_ap_set() apaga/enciende SOLO la radio del AP: el servidor HTTP, el
 * responder DNS y el servidor DHCP siguen levantados (no consumen si no hay
 * estaciones asociadas) y no hay que reconstruirlos. Al encender se vuelve a
 * afirmar la IP estatica, que es idempotente, para no depender de si el ciclo
 * de bajada del enlace la conservo.
 *
 * Es idempotente: llamarla con el estado actual no hace nada y devuelve 0.
 * Devuelve <0 si el driver rechaza el cambio; en ese caso el estado interno
 * NO cambia, para que el llamante pueda reintentar.
 */
int  portal_ap_set(bool on);

/* Estado actual de la radio del AP. */
bool portal_ap_is_on(void);

/* Publica nuevas lecturas (thread-safe) para /api/sensors. */
void portal_update_sensors(const struct portal_sensors *s);

/* Copia atomica del snapshot actual (para el handler HTTP). */
void portal_get_sensors(struct portal_sensors *out);

/* ---- Boton de emergencia (SOS) ----------------------------------------- */
/* portal_request_sos(): lo llama el handler HTTP /api/sos (hilo del servidor).
 * portal_take_sos():    lo consume el lazo principal; devuelve true UNA vez si
 *                       habia un SOS pendiente (y lo limpia) -> envia uplink LoRa
 *                       de SOS en FPort 3. Thread-safe (atomic). */
void portal_request_sos(void);
bool portal_take_sos(void);

/* ---- HTML mutable del portal -------------------------------------------- */

/* Tamano maximo del HTML servido/actualizable. Subido a 10K para el dashboard
   con graficos. OJO: hay 2 buffers de este tamano (vivo + staging OTA). */
#define PORTAL_HTML_MAX 10240

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
