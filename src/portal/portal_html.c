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

/* Dashboard: tarjetas por sensor + indicadores de estado + mini-graficos SVG
 * (sparklines) dibujados en el cliente con la historia de las ultimas lecturas.
 * 100% autocontenido (sin imagenes ni librerias externas) -> rinde en el
 * navegador cautivo de iOS/Android sin acceso a internet. */
static const char default_html[] =
	"<!DOCTYPE html><html lang=\"es\"><head><meta charset=\"utf-8\">"
	"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
	"<title>Gesinen WildFire</title><style>"
	"*{box-sizing:border-box}body{font-family:system-ui,sans-serif;margin:0;"
	"background:#0f172a;color:#e2e8f0}header{padding:14px;text-align:center}"
	"h1{margin:0;font-size:20px}.sub{color:#94a3b8;font-size:12px;margin-top:4px}"
	".wrap{max-width:520px;margin:0 auto;padding:0 12px 24px}"
	".sos{display:block;width:100%;padding:16px;margin:6px 0 16px;border:0;"
	"border-radius:12px;background:#dc2626;color:#fff;font-size:18px;"
	"font-weight:700}.sos:active{background:#991b1b}"
	".card{background:#1e293b;border-radius:12px;padding:12px 14px;margin:10px 0}"
	".card h2{margin:0 0 8px;font-size:15px;color:#38bdf8;display:flex;"
	"align-items:center;gap:8px}.dot{width:10px;height:10px;border-radius:50%;"
	"background:#475569;display:inline-block}.dot.on{background:#22c55e}"
	".dot.err{background:#ef4444}.grid{display:grid;grid-template-columns:1fr 1fr;"
	"gap:4px 14px}.kv{display:flex;justify-content:space-between;font-size:14px;"
	"padding:2px 0}.kv b{font-variant-numeric:tabular-nums}.lbl{font-size:11px;"
	"color:#94a3b8;margin-top:8px}.spark{width:100%;height:40px;margin-top:4px;"
	"background:#0b1220;border-radius:6px}footer{text-align:center;color:#94a3b8;"
	"font-size:11px;padding:8px}</style></head><body>"
	"<header><h1>Gesinen WildFire</h1><div class=\"sub\" id=\"age\">cargando...</div></header>"
	"<div class=\"wrap\">"
	"<button class=\"sos\" id=\"sos\">ENVIAR SOS</button>"
	"<div class=\"card\"><h2><span class=\"dot\" id=\"d1\"></span>BM688 &mdash; Ambiente</h2>"
	"<div class=\"grid\">"
	"<div class=\"kv\"><span>Temp</span><b><span id=\"t\">--</span> &deg;C</b></div>"
	"<div class=\"kv\"><span>Humedad</span><b><span id=\"h\">--</span> %</b></div>"
	"<div class=\"kv\"><span>Presion</span><b><span id=\"p\">--</span> hPa</b></div>"
	"<div class=\"kv\"><span>Gas</span><b><span id=\"g\">--</span> &Omega;</b></div></div>"
	"<div class=\"lbl\">Temperatura (historico)</div>"
	"<svg class=\"spark\" id=\"spt\" viewBox=\"0 0 240 40\" preserveAspectRatio=\"none\"></svg></div>"
	"<div class=\"card\"><h2><span class=\"dot\" id=\"d2\"></span>ZE15-CO &mdash; Monoxido</h2>"
	"<div class=\"kv\"><span>CO</span><b><span id=\"co\">--</span> ppm</b></div>"
	"<div class=\"lbl\">CO ppm (historico)</div>"
	"<svg class=\"spark\" id=\"spco\" viewBox=\"0 0 240 40\" preserveAspectRatio=\"none\"></svg></div>"
	"<div class=\"card\"><h2><span class=\"dot\" id=\"d3\"></span>SEN65 &mdash; Calidad de aire</h2>"
	"<div class=\"grid\">"
	"<div class=\"kv\"><span>PM1.0</span><b><span id=\"pm1\">--</span></b></div>"
	"<div class=\"kv\"><span>PM2.5</span><b><span id=\"pm25\">--</span></b></div>"
	"<div class=\"kv\"><span>PM4.0</span><b><span id=\"pm4\">--</span></b></div>"
	"<div class=\"kv\"><span>PM10</span><b><span id=\"pm10\">--</span></b></div>"
	"<div class=\"kv\"><span>VOC</span><b><span id=\"voc\">--</span></b></div>"
	"<div class=\"kv\"><span>NOx</span><b><span id=\"nox\">--</span></b></div>"
	"<div class=\"kv\"><span>Temp</span><b><span id=\"st\">--</span> &deg;C</b></div>"
	"<div class=\"kv\"><span>Humedad</span><b><span id=\"sh\">--</span> %</b></div></div>"
	"<div class=\"lbl\">PM2.5 &micro;g/m&sup3; (azul) &middot; VOC (verde)</div>"
	"<svg class=\"spark\" id=\"sppm\" viewBox=\"0 0 240 40\" preserveAspectRatio=\"none\"></svg></div>"
	"</div><footer>Portal cautivo &middot; http://192.168.4.1</footer>"
	"<script>"
	"var H={t:[],co:[],pm:[],voc:[]};"
	"function $(i){return document.getElementById(i);}"
	"function S(i,v){$(i).textContent=v;}"
	"function D(i,o){$(i).className='dot'+(o?' on':' err');}"
	"function P(a,v){a.push(v);if(a.length>40)a.shift();}"
	"function L(a,c){var n=a.length;if(!n)return '';"
	"var mn=Math.min.apply(null,a),mx=Math.max.apply(null,a),r=(mx-mn)||1,p='';"
	"for(var i=0;i<n;i++){p+=(i*(240/((n-1)||1))).toFixed(1)+','+(38-((a[i]-mn)/r)*36).toFixed(1)+' ';}"
	"return '<polyline fill=\"none\" stroke=\"'+c+'\" stroke-width=\"2\" points=\"'+p+'\"/>';}"
	"function G(id,a,c,a2,c2){var s=L(a,c);if(a2)s+=L(a2,c2);$(id).innerHTML=s;}"
	"async function u(){try{var d=await(await fetch('/api/sensors')).json();"
	"D('d1',d.bm688);D('d2',d.co);D('d3',d.sen65);"
	"S('t',d.bm688?d.temperature.toFixed(1):'--');"
	"S('h',d.bm688?d.humidity.toFixed(1):'--');"
	"S('p',d.bm688?(d.pressure/100).toFixed(1):'--');"
	"S('g',d.bm688?d.gas:'--');"
	"S('co',d.co?d.co_ppm.toFixed(1):'--');"
	"S('pm1',d.sen65?d.pm1_0.toFixed(1):'--');"
	"S('pm25',d.sen65?d.pm2_5.toFixed(1):'--');"
	"S('pm4',d.sen65?d.pm4_0.toFixed(1):'--');"
	"S('pm10',d.sen65?d.pm10_0.toFixed(1):'--');"
	"S('voc',d.sen65?d.voc:'--');S('nox',d.sen65?d.nox:'--');"
	"S('st',d.sen65?d.s_temp.toFixed(1):'--');"
	"S('sh',d.sen65?d.s_hum.toFixed(1):'--');"
	"if(d.bm688)P(H.t,d.temperature);if(d.co)P(H.co,d.co_ppm);"
	"if(d.sen65){P(H.pm,d.pm2_5);P(H.voc,d.voc);}"
	"G('spt',H.t,'#f59e0b');G('spco',H.co,'#ef4444');G('sppm',H.pm,'#38bdf8',H.voc,'#22c55e');"
	"S('age','actualizado hace '+d.age_ms+' ms');"
	"}catch(e){S('age','sin datos');}}"
	"$('sos').onclick=function(){var b=this;b.textContent='Enviando SOS...';"
	"fetch('/api/sos').then(function(){b.textContent='SOS ENVIADO';})"
	".catch(function(){b.textContent='Error, reintenta';});"
	"setTimeout(function(){b.textContent='ENVIAR SOS';},4000);};"
	"u();setInterval(u,2000);"
	"</script></body></html>";

/* No desbordar los buffers (vivo + staging) al cargar el default. */
BUILD_ASSERT(sizeof(default_html) <= PORTAL_HTML_MAX,
	     "default_html no cabe en PORTAL_HTML_MAX");

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
