/*
 * Almacen mutable del HTML del portal + actualizacion por downlink LoRa.
 *
 * El HTML vive en un buffer estatico (servido por el HTTP server) y se
 * persiste en Settings/NVS bajo la clave "portal/html". Al arrancar se
 * restaura de NVS; si no hay nada, se usa el default empotrado.
 *
 * La actualizacion por LoRa reensambla en un buffer 'staging' separado, de
 * modo que una transferencia a medias NUNCA corrompe la pagina viva: solo
 * en COMMIT (con longitud y CRC validados) se vuelca a vivo + NVS.
 */
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "portal/portal.h"

LOG_MODULE_REGISTER(portal_html, LOG_LEVEL_INF);

/* Dashboard minimo: lista los sensores y refresca por fetch cada 2 s.
 * Deliberadamente simple (sin CSS sofisticado): es la base para escalar. */
static const char default_html[] =
	"<!DOCTYPE html><html lang=\"es\"><head><meta charset=\"utf-8\">"
	"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
	"<title>Gesinen WildFire</title><style>"
	"body{font-family:sans-serif;margin:0;background:#0f172a;color:#e2e8f0;"
	"text-align:center}h2{margin:12px}.c{background:#1e293b;border-radius:10px;"
	"margin:10px auto;max-width:430px;padding:8px 12px}.c h3{margin:4px 0 6px;"
	"color:#38bdf8}table{width:100%;border-collapse:collapse}td{padding:3px 6px;"
	"text-align:left}td.v{text-align:right;font-weight:bold}"
	"small{color:#94a3b8}</style></head><body>"
	"<h2>Gesinen WildFire</h2>"
	"<img src=\"/gorila.jpg\" alt=\"gorila\" width=\"120\" "
	"style=\"border-radius:8px\">"
	"<div class=\"c\"><h3>BM688 &mdash; Ambiente</h3><table>"
	"<tr><td>Temperatura</td><td class=\"v\"><span id=\"t\">--</span> &deg;C</td></tr>"
	"<tr><td>Humedad</td><td class=\"v\"><span id=\"h\">--</span> %</td></tr>"
	"<tr><td>Presion</td><td class=\"v\"><span id=\"p\">--</span> Pa</td></tr>"
	"<tr><td>Gas</td><td class=\"v\"><span id=\"g\">--</span> &Omega;</td></tr>"
	"</table></div>"
	"<div class=\"c\"><h3>ZE15-CO &mdash; Monoxido</h3><table>"
	"<tr><td>CO</td><td class=\"v\"><span id=\"co\">--</span> ppm</td></tr>"
	"</table></div>"
	"<div class=\"c\"><h3>SEN65 &mdash; Calidad de aire</h3><table>"
	"<tr><td>PM1.0</td><td class=\"v\"><span id=\"pm1\">--</span> &micro;g/m&sup3;</td></tr>"
	"<tr><td>PM2.5</td><td class=\"v\"><span id=\"pm25\">--</span> &micro;g/m&sup3;</td></tr>"
	"<tr><td>PM4.0</td><td class=\"v\"><span id=\"pm4\">--</span> &micro;g/m&sup3;</td></tr>"
	"<tr><td>PM10</td><td class=\"v\"><span id=\"pm10\">--</span> &micro;g/m&sup3;</td></tr>"
	"<tr><td>Indice VOC</td><td class=\"v\"><span id=\"voc\">--</span></td></tr>"
	"<tr><td>Indice NOx</td><td class=\"v\"><span id=\"nox\">--</span></td></tr>"
	"<tr><td>Temperatura</td><td class=\"v\"><span id=\"st\">--</span> &deg;C</td></tr>"
	"<tr><td>Humedad</td><td class=\"v\"><span id=\"sh\">--</span> %</td></tr>"
	"</table></div>"
	"<p><small id=\"age\">cargando...</small></p>"
	"<script>"
	"function s(i,v){document.getElementById(i).textContent=v;}"
	"async function u(){try{let r=await fetch('/api/sensors');let d=await r.json();"
	"s('t',d.bm688?d.temperature.toFixed(2):'--');"
	"s('h',d.bm688?d.humidity.toFixed(2):'--');"
	"s('p',d.bm688?d.pressure:'--');"
	"s('g',d.bm688?d.gas:'--');"
	"s('co',d.co?d.co_ppm.toFixed(1):'--');"
	"s('pm1',d.sen65?d.pm1_0.toFixed(1):'--');"
	"s('pm25',d.sen65?d.pm2_5.toFixed(1):'--');"
	"s('pm4',d.sen65?d.pm4_0.toFixed(1):'--');"
	"s('pm10',d.sen65?d.pm10_0.toFixed(1):'--');"
	"s('voc',d.sen65?d.voc:'--');"
	"s('nox',d.sen65?d.nox:'--');"
	"s('st',d.sen65?d.s_temp.toFixed(2):'--');"
	"s('sh',d.sen65?d.s_hum.toFixed(2):'--');"
	"s('age','actualizado hace '+d.age_ms+' ms');"
	"}catch(e){s('age','sin datos');}}"
	"u();setInterval(u,2000);"
	"</script></body></html>";

/* Buffer vivo (servido) y su longitud. Protegido por html_lock. */
static uint8_t  html_buf[PORTAL_HTML_MAX];
static size_t   html_len;
static struct k_mutex html_lock;

/* ---- Settings backend: clave "portal/html" ------------------------------ */

static int portal_settings_set(const char *name, size_t len,
			       settings_read_cb read_cb, void *cb_arg)
{
	if (settings_name_steq(name, "html", NULL)) {
		if (len > PORTAL_HTML_MAX) {
			LOG_WRN("HTML en NVS (%zu) > max (%d), se ignora",
				len, PORTAL_HTML_MAX);
			return -EINVAL;
		}
		k_mutex_lock(&html_lock, K_FOREVER);
		ssize_t r = read_cb(cb_arg, html_buf, PORTAL_HTML_MAX);
		html_len = (r > 0) ? (size_t)r : 0;
		k_mutex_unlock(&html_lock);
		LOG_INF("HTML restaurado de NVS: %zu bytes", html_len);
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(portal, "portal", NULL,
			       portal_settings_set, NULL, NULL);

int portal_html_init(void)
{
	k_mutex_init(&html_lock);

	/* Default empotrado por si NVS no trae nada. */
	html_len = sizeof(default_html) - 1;
	memcpy(html_buf, default_html, html_len);

	/* settings_subsys_init() ya lo invoca lorawan_start(); volver a
	 * llamarlo es idempotente. Cargar solo el subtree "portal". */
	(void)settings_load_subtree("portal");
	return 0;
}

void portal_html_get(const uint8_t **buf, size_t *len)
{
	*buf = html_buf;
	*len = html_len;
}

/* ---- Reensamblado por downlink LoRa ------------------------------------- */

static uint8_t  staging[PORTAL_HTML_MAX];
static size_t   ota_expected_len;
static uint16_t ota_expected_crc;
static size_t   ota_received;       /* bytes recibidos (acumulado)       */
static bool     ota_active;

void portal_html_ota_rx(const uint8_t *data, uint8_t len)
{
	if (len < 1) {
		return;
	}

	switch (data[0]) {
	case 0x01: /* BEGIN */
		if (len < 5) {
			LOG_WRN("OTA BEGIN corto (%u)", len);
			return;
		}
		ota_expected_len = (size_t)data[1] | ((size_t)data[2] << 8);
		ota_expected_crc = (uint16_t)data[3] | ((uint16_t)data[4] << 8);
		if (ota_expected_len == 0 || ota_expected_len > PORTAL_HTML_MAX) {
			LOG_WRN("OTA BEGIN len invalida: %zu", ota_expected_len);
			ota_active = false;
			return;
		}
		memset(staging, 0, sizeof(staging));
		ota_received = 0;
		ota_active = true;
		LOG_INF("OTA BEGIN: len=%zu crc=0x%04x", ota_expected_len,
			ota_expected_crc);
		break;

	case 0x02: /* DATA: [off_lo][off_hi][bytes...] */
		if (!ota_active || len < 3) {
			return;
		}
		{
			size_t off = (size_t)data[1] | ((size_t)data[2] << 8);
			size_t n = len - 3;

			if (off + n > PORTAL_HTML_MAX) {
				LOG_WRN("OTA DATA fuera de rango off=%zu n=%zu",
					off, n);
				return;
			}
			memcpy(&staging[off], &data[3], n);
			ota_received += n;
			LOG_DBG("OTA DATA off=%zu n=%zu (rx=%zu/%zu)", off, n,
				ota_received, ota_expected_len);
		}
		break;

	case 0x03: /* COMMIT */
		if (!ota_active) {
			return;
		}
		ota_active = false;
		if (ota_received != ota_expected_len) {
			LOG_ERR("OTA COMMIT: rx=%zu != esperado=%zu",
				ota_received, ota_expected_len);
			return;
		}
		{
			uint16_t crc = crc16_ccitt(0xFFFF, staging,
						   ota_expected_len);
			if (crc != ota_expected_crc) {
				LOG_ERR("OTA COMMIT: CRC 0x%04x != 0x%04x",
					crc, ota_expected_crc);
				return;
			}
		}
		/* Validado: a vivo + NVS. */
		k_mutex_lock(&html_lock, K_FOREVER);
		memcpy(html_buf, staging, ota_expected_len);
		html_len = ota_expected_len;
		k_mutex_unlock(&html_lock);

		{
			int err = settings_save_one("portal/html", html_buf,
						    html_len);
			if (err) {
				LOG_ERR("OTA persistir NVS err %d", err);
			} else {
				LOG_INF("OTA COMMIT OK: HTML %zu bytes en vivo+NVS",
					html_len);
			}
		}
		break;

	default:
		LOG_WRN("OTA opcode desconocido 0x%02x", data[0]);
		break;
	}
}
