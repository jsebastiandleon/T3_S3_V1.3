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

/* Dashboard: tarjetas por sensor + indicadores de estado + barras de nivel
 * con zonas de umbral (verde/ambar/rojo) para las magnitudes criticas.
 *
 * Por que barras y no sparklines: el nodo NO guarda historico, la serie solo
 * existia en la memoria del navegador desde que se abria la pagina, asi que
 * la grafica no representaba nada util (arrancaba vacia en cada visita y se
 * perdia al recargar). La barra con umbrales SI da contexto a una lectura
 * instantanea: donde cae el valor respecto a los limites de referencia.
 * (La version con sparklines SVG queda en el historial de git.)
 *
 * 100% autocontenido (sin imagenes ni librerias externas) -> rinde en el
 * navegador cautivo de iOS/Android sin acceso a internet. */
static const char default_html[] =
	"<!DOCTYPE html><html lang=\"es\"><head><meta charset=\"utf-8\">"
	"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
	"<title>Gandia WildFire</title><style>"
	"*{box-sizing:border-box}body{font-family:system-ui,sans-serif;margin:0;"
	"background:#0f172a;color:#e2e8f0}header{padding:14px;text-align:center}"
	"h1{margin:0;font-size:20px}.sub{color:#94a3b8;font-size:12px;margin-top:4px}"
	".wrap{max-width:520px;margin:0 auto;padding:0 12px 24px}"
	/* --- SOS DESACTIVADO: estilos del boton, se conservan para reactivarlo ---
	".sos{display:block;width:100%;padding:16px;margin:6px 0 16px;border:0;"
	"border-radius:12px;background:#dc2626;color:#fff;font-size:18px;"
	"font-weight:700}.sos:active{background:#991b1b}"
	".sos.arm{background:#f59e0b;color:#111}"
	   --- fin estilos SOS --- */
	".card{background:#1e293b;border-radius:12px;padding:12px 14px;margin:10px 0}"
	".card:first-child{margin-top:16px}"
	".card h2{margin:0 0 8px;font-size:15px;color:#38bdf8;display:flex;"
	"align-items:center;gap:8px}.dot{width:10px;height:10px;border-radius:50%;"
	"background:#475569;display:inline-block}.dot.on{background:#22c55e}"
	".dot.err{background:#ef4444}.grid{display:grid;grid-template-columns:1fr 1fr;"
	"gap:4px 14px}.kv{display:flex;justify-content:space-between;font-size:14px;"
	"padding:2px 0}.kv b{font-variant-numeric:tabular-nums}.kv u{color:#94a3b8;"
	"font-weight:400;font-size:11px;text-decoration:none;margin-left:3px}"
	/* Barra de nivel: la pista es el degradado de zonas (los cortes en % son
	 * los umbrales sobre el fondo de escala) y el cursor marca el valor. */
	".lbl{font-size:11px;color:#94a3b8;margin-top:12px;display:flex;"
	"justify-content:space-between;align-items:center;gap:8px}"
	".tag{font-size:10px;font-weight:700;padding:2px 8px;border-radius:99px;"
	"background:#334155;color:#e2e8f0;white-space:nowrap}"
	".bar{position:relative;height:9px;border-radius:5px;margin-top:5px}"
	".bar.off{filter:grayscale(1);opacity:.35}"
	".bar i{position:absolute;top:-4px;left:0;margin-left:-2px;width:4px;"
	"height:17px;border-radius:2px;background:#f8fafc;"
	"box-shadow:0 0 0 1.5px #0f172a;transition:left .4s}"
	".bt{background:linear-gradient(90deg,#22c55e 0 58%,#f59e0b 58% 75%,#ef4444 75%)}"
	/* CO a fondo de escala 150 ppm: 9->6%, 35->23%, 100->67%. */
	".bc{background:linear-gradient(90deg,#22c55e 0 6%,#f59e0b 6% 23%,"
	"#ef4444 23% 67%,#a855f7 67%)}"
	".bp{background:linear-gradient(90deg,#22c55e 0 12%,#f59e0b 12% 35%,"
	"#ef4444 35% 55%,#a855f7 55%)}"
	".sc{display:flex;justify-content:space-between;font-size:10px;"
	"color:#64748b;margin-top:3px}.sc b{color:#cbd5e1;font-weight:600;"
	"font-variant-numeric:tabular-nums}"
	"footer{text-align:center;color:#94a3b8;font-size:11px;padding:12px 8px;"
	"line-height:1.5}footer b{color:#cbd5e1}</style></head><body>"
	"<header><h1>Gandia WildFire</h1><div class=\"sub\" id=\"age\">cargando...</div></header>"
	"<div class=\"wrap\">"
	/* --- SOS DESACTIVADO: descomentar esta linea para volver a mostrarlo ---
	"<button class=\"sos\" id=\"sos\">ENVIAR SOS</button>"
	   --- fin markup SOS --- */
	"<div class=\"card\"><h2><span class=\"dot\" id=\"da\"></span>Ambiente</h2>"
	"<div class=\"grid\">"
	"<div class=\"kv\"><span>Temperatura</span><b><span id=\"t\">--</span> &deg;C</b></div>"
	"<div class=\"kv\"><span>Humedad</span><b><span id=\"h\">--</span> %</b></div>"
	"<div class=\"kv\"><span>Presi&oacute;n</span><b><span id=\"p\">--</span> hPa</b></div></div>"
	"<div class=\"lbl\"><span>Temperatura</span><span class=\"tag\" id=\"qt\">--</span></div>"
	"<div class=\"bar bt\" id=\"gt\"><i></i></div>"
	"<div class=\"sc\"><span>0</span><span>aviso 35</span><span>riesgo 45</span>"
	"<span><b id=\"vt\">--</b> &deg;C</span></div></div>"
	"<div class=\"card\"><h2><span class=\"dot\" id=\"dq\"></span>Calidad del aire</h2>"
	"<div class=\"grid\">"
	"<div class=\"kv\"><span>Mon&oacute;xido CO</span><b><span id=\"co\">--</span> ppm</b></div>"
	"<div class=\"kv\"><span>PM2.5</span><b><span id=\"pm25\">--</span> <u>&micro;g/m&sup3;</u></b></div>"
	"<div class=\"kv\"><span>PM10</span><b><span id=\"pm10\">--</span> <u>&micro;g/m&sup3;</u></b></div>"
	"<div class=\"kv\"><span>PM1.0</span><b><span id=\"pm1\">--</span> <u>&micro;g/m&sup3;</u></b></div>"
	"<div class=\"kv\"><span>PM4.0</span><b><span id=\"pm4\">--</span> <u>&micro;g/m&sup3;</u></b></div>"
	"<div class=\"kv\"><span>COV</span><b><span id=\"voc\">--</span> <u>&iacute;ndice</u></b></div>"
	"<div class=\"kv\"><span>NOx</span><b><span id=\"nox\">--</span> <u>&iacute;ndice</u></b></div>"
	/* El BM688 entrega OHMIOS de un MOX: un numero que no significa nada
	 * para quien abre el portal. Se muestra traducido (ver GS() abajo):
	 * palabra + indice 0-100, sin unidad electrica. */
	"<div class=\"kv\"><span>Gases</span><b><span id=\"g\">--</span></b></div></div>"
	"<div class=\"lbl\"><span>Mon&oacute;xido CO</span><span class=\"tag\" id=\"qc\">--</span></div>"
	"<div class=\"bar bc\" id=\"gc\"><i></i></div>"
	"<div class=\"sc\"><span>0</span><span>9</span><span>35</span><span>100</span>"
	"<span><b id=\"vc\">--</b> ppm</span></div>"
	"<div class=\"lbl\"><span>PM2.5</span><span class=\"tag\" id=\"qp\">--</span></div>"
	"<div class=\"bar bp\" id=\"gp\"><i></i></div>"
	"<div class=\"sc\"><span>0</span><span>12</span><span>35</span><span>55</span>"
	"<span><b id=\"vp\">--</b> &micro;g/m&sup3;</span></div></div>"
	"</div><footer>Lectura instant&aacute;nea &middot; el nodo no guarda hist&oacute;rico<br>"
	"Dise&ntilde;ado por <b>Gesinen</b> &middot; Hecho en Espa&ntilde;a<br>"
	"Portal cautivo &middot; http://192.168.4.1</footer>"
	"<script>"
	"function $(i){return document.getElementById(i);}"
	"function S(i,v){$(i).textContent=v;}"
	"function D(i,o){$(i).className='dot'+(o?' on':' err');}"
	/* Colores de zona, en el mismo orden que los umbrales de cada barra. */
	"var Q=['#22c55e','#f59e0b','#ef4444','#a855f7'];"
	"var NT=['Normal','Aviso','Riesgo'];"
	"var NC=['Bueno','Moderado','Alto','Peligroso'];"
	"var NP=['Bueno','Moderado','Malo','Muy malo'];"
	"var NG=['Limpio','Regular','Malo','Muy malo'];"
	/* GS(): la resistencia del MOX sube con aire limpio y baja cuando hay
	 * gases/COV, de forma LOGARITMICA. Se mapea el rango 5 kOhm (saturado)
	 * a 500 kOhm (limpio) sobre un indice 0-100 y se acompana de una
	 * palabra, para no ensenar ohmios a quien no le dicen nada. Es una
	 * escala ORIENTATIVA: el MOX no esta calibrado y deriva, por eso el
	 * firmware no lo usa para alertas (TH_GAS_EN=0). Si tu unidad se queda
	 * siempre en la misma palabra, ajusta LO/HI a lo que de en reposo. */
	"function GS(r){var e=$('g');"
	"if(!r||r<=0){e.textContent='--';e.style.color='';return;}"
	"var LO=Math.log(5000),HI=Math.log(500000);"
	"var v=Math.round(100*(Math.log(r)-LO)/(HI-LO));"
	"v=Math.max(0,Math.min(100,v));"
	"var k=v>=70?0:(v>=40?1:(v>=20?2:3));"
	"e.textContent=NG[k]+' ('+v+')';e.style.color=Q[k];}"
	/* G(barra,etiqueta,valor,valor_texto,fondo_escala,umbrales,nombres):
	 * coloca el cursor en la pista y clasifica la lectura por umbrales.
	 * Sin dato -> barra en gris y etiqueta '--'. */
	"function G(id,tg,v,vt,mx,th,nm){var e=$(id),k=(v==null||isNaN(v));"
	"e.classList.toggle('off',k);"
	"e.firstChild.style.left=(k?0:Math.max(0,Math.min(100,v/mx*100)))+'%';"
	"var t=$(tg);S(vt,k?'--':v.toFixed(1));"
	"if(k){t.textContent='--';t.style.background='#334155';"
	"t.style.color='#e2e8f0';return;}"
	"var i=0;while(i<th.length&&v>=th[i])i++;"
	"t.textContent=nm[i];t.style.background=Q[i];t.style.color='#0f172a';}"
	"async function u(){try{var d=await(await fetch('/api/sensors')).json();"
	/* Unifica temp/humedad: BM688 preferente, si no SEN65. */
	"var tm=d.bm688?d.temperature:(d.sen65?d.s_temp:null);"
	"var hm=d.bm688?d.humidity:(d.sen65?d.s_hum:null);"
	"D('da',d.bm688||d.sen65);D('dq',d.co||d.sen65||d.bm688);"
	"S('t',tm!=null?tm.toFixed(1):'--');"
	"S('h',hm!=null?hm.toFixed(1):'--');"
	"S('p',d.bm688?(d.pressure/100).toFixed(1):'--');"
	"GS(d.bm688?d.gas:null);"
	"S('co',d.co?d.co_ppm.toFixed(1):'--');"
	"S('pm25',d.sen65?d.pm2_5.toFixed(1):'--');"
	"S('pm10',d.sen65?d.pm10_0.toFixed(1):'--');"
	"S('pm1',d.sen65?d.pm1_0.toFixed(1):'--');"
	"S('pm4',d.sen65?d.pm4_0.toFixed(1):'--');"
	"S('voc',d.sen65?d.voc:'--');S('nox',d.sen65?d.nox:'--');"
	"G('gt','qt',tm,'vt',60,[35,45],NT);"
	"G('gc','qc',d.co?d.co_ppm:null,'vc',150,[9,35,100],NC);"
	"G('gp','qp',d.sen65?d.pm2_5:null,'vp',100,[12,35,55],NP);"
	"S('age',d.age_ms<0?'sin lecturas a\\u00fan':'actualizado hace '+(d.age_ms/1000).toFixed(0)+' s');"
	"}catch(e){S('age','sin datos');}}"
	/* --- SOS DESACTIVADO: logica de armado + envio a /api/sos (el endpoint
	 *     sigue vivo en el firmware). Descomentar junto al markup y los
	 *     estilos de arriba para reactivar el boton. ---
	"var armed=false,at;"
	"$('sos').onclick=function(){var b=this;"
	"if(!armed){armed=true;b.classList.add('arm');"
	"b.textContent='CONFIRMAR SOS (toca otra vez)';"
	"at=setTimeout(function(){armed=false;b.classList.remove('arm');"
	"b.textContent='ENVIAR SOS';},5000);return;}"
	"clearTimeout(at);armed=false;b.classList.remove('arm');"
	"b.textContent='Enviando SOS...';"
	"fetch('/api/sos').then(function(){b.textContent='SOS ENVIADO';})"
	".catch(function(){b.textContent='Error, reintenta';});"
	"setTimeout(function(){b.textContent='ENVIAR SOS';},4000);};"
	   --- fin logica SOS --- */
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
