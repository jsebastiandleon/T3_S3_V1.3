/*
 * Tabla de recursos del portal. Aqui es donde se "escala": cada funcion nueva
 * = un HTTP_RESOURCE_DEFINE mas. De momento:
 *   GET /             -> dashboard HTML (mutable, desde el almacen portal_html)
 *   GET /api/sensors  -> JSON con el ultimo snapshot de sensores
 *   GET /gorila.jpg   -> imagen embebida
 *   <fallback>        -> 302 redirect a http://192.168.4.1/ ante CUALQUIER ruta
 *                        no mapeada. Las URLs de sondeo del SO (Android
 *                        /generate_204, Apple /hotspot-detect.html, Windows
 *                        connecttest) reciben el redirect -> el movil abre el
 *                        portal cautivo SOLO, sin teclear la IP.
 */
#include <zephyr/kernel.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/http/server.h>
#include <zephyr/logging/log.h>
#include <stdio.h>

#include "portal/portal.h"

LOG_MODULE_REGISTER(portal_http, LOG_LEVEL_INF);

static const struct http_header html_ctype[] = {
	{ .name = "Content-Type", .value = "text/html; charset=utf-8" },
};
static const struct http_header json_ctype[] = {
	{ .name = "Content-Type", .value = "application/json" },
};

/* ---- GET /gorila.jpg: imagen embebida (recurso estatico) ---------------- */
static const uint8_t gorila_jpg[] = {
#include "gorila.jpg.inc"
};

static struct http_resource_detail_static gorila_resource = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_type = "image/jpeg",
	},
	.static_data = gorila_jpg,
	.static_data_len = sizeof(gorila_jpg),
};

/* ---- GET / (y fallback): dashboard HTML --------------------------------- */
static int index_handler(struct http_client_ctx *client,
			 enum http_transaction_status status,
			 const struct http_request_ctx *req,
			 struct http_response_ctx *rsp, void *user_data)
{
	ARG_UNUSED(client); ARG_UNUSED(req); ARG_UNUSED(user_data);

	if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
		const uint8_t *html;
		size_t len;

		portal_html_get(&html, &len);
		rsp->status = HTTP_200_OK;
		rsp->headers = html_ctype;
		rsp->header_count = ARRAY_SIZE(html_ctype);
		rsp->body = html;
		rsp->body_len = len;
		rsp->final_chunk = true;
	}
	return 0;
}

static struct http_resource_detail_dynamic index_resource = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
	},
	.cb = index_handler,
};

/* ---- GET /api/sensors: JSON --------------------------------------------- */
static int sensors_handler(struct http_client_ctx *client,
			   enum http_transaction_status status,
			   const struct http_request_ctx *req,
			   struct http_response_ctx *rsp, void *user_data)
{
	ARG_UNUSED(client); ARG_UNUSED(req); ARG_UNUSED(user_data);
	static char json[384];

	if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
		struct portal_sensors s;

		portal_get_sensors(&s);

		int64_t age = (s.updated_uptime_ms == 0)
			      ? -1 : (k_uptime_get() - s.updated_uptime_ms);

		/* Formateo entero a proposito (sin printf de floats): cada
		 * valor sale como numero JSON valido con sus decimales. */
		int t_i  = (int)s.temperature;
		int t_f  = (int)(s.temperature * 100) % 100; if (t_f < 0) t_f = -t_f;
		int h_i  = (int)s.humidity;
		int h_f  = (int)(s.humidity * 100) % 100;    if (h_f < 0) h_f = -h_f;
		int co_i = (int)s.co_ppm;
		int co_f = (int)(s.co_ppm * 10) % 10;        if (co_f < 0) co_f = -co_f;
		/* SEN65: PM en ug/m3 (1 decimal) e indices VOC/NOx (enteros). */
		int p1_i  = (int)s.pm1_0;  int p1_f  = (int)(s.pm1_0 * 10) % 10;
		int p25_i = (int)s.pm2_5;  int p25_f = (int)(s.pm2_5 * 10) % 10;
		int p4_i  = (int)s.pm4_0;  int p4_f  = (int)(s.pm4_0 * 10) % 10;
		int p10_i = (int)s.pm10_0; int p10_f = (int)(s.pm10_0 * 10) % 10;

		int len = snprintf(json, sizeof(json),
			"{\"bm688\":%s,\"temperature\":%d.%02d,\"humidity\":%d.%02d,"
			"\"pressure\":%d,\"gas\":%d,"
			"\"co\":%s,\"co_ppm\":%d.%d,"
			"\"sen65\":%s,\"pm1_0\":%d.%d,\"pm2_5\":%d.%d,"
			"\"pm4_0\":%d.%d,\"pm10_0\":%d.%d,\"voc\":%d,\"nox\":%d,"
			"\"age_ms\":%lld}",
			s.bm688_valid ? "true" : "false", t_i, t_f, h_i, h_f,
			(int)s.pressure, (int)s.gas_resistance,
			s.co_valid ? "true" : "false", co_i, co_f,
			s.sen65_valid ? "true" : "false",
			p1_i, p1_f, p25_i, p25_f, p4_i, p4_f, p10_i, p10_f,
			(int)s.voc_index, (int)s.nox_index,
			(long long)age);

		rsp->status = HTTP_200_OK;
		rsp->headers = json_ctype;
		rsp->header_count = ARRAY_SIZE(json_ctype);
		rsp->body = json;
		rsp->body_len = (len > 0) ? len : 0;
		rsp->final_chunk = true;
	}
	return 0;
}

static struct http_resource_detail_dynamic sensors_resource = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
	},
	.cb = sensors_handler,
};

/* ---- Fallback de portal cautivo: 302 redirect a la pagina --------------- */
/* Responder con redirect (en vez de servir el HTML directo) hace que el
 * Captive Network Assistant de Android/iOS/Windows abra la pagina de forma
 * automatica. El Location apunta a la raiz, que SI esta mapeada -> sin bucle. */
static const struct http_header captive_redirect_hdrs[] = {
	{ .name = "Location", .value = "http://192.168.4.1/" },
};

static int captive_handler(struct http_client_ctx *client,
			   enum http_transaction_status status,
			   const struct http_request_ctx *req,
			   struct http_response_ctx *rsp, void *user_data)
{
	ARG_UNUSED(client); ARG_UNUSED(req); ARG_UNUSED(user_data);

	if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
		rsp->status = HTTP_302_FOUND;
		rsp->headers = captive_redirect_hdrs;
		rsp->header_count = ARRAY_SIZE(captive_redirect_hdrs);
		rsp->body = NULL;
		rsp->body_len = 0;
		rsp->final_chunk = true;
	}
	return 0;
}

static struct http_resource_detail_dynamic captive_resource = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
	},
	.cb = captive_handler,
};

/* ---- Servicio HTTP en :80 ----------------------------------------------- */
static uint16_t http_port = 80;

/* El fallback (7o arg) redirige (302) cualquier ruta no mapeada a la pagina,
 * disparando la apertura automatica del portal cautivo del SO. */
HTTP_SERVICE_DEFINE(portal_http_service, NULL, &http_port, 2, 4, NULL,
		    &captive_resource.common, NULL);

HTTP_RESOURCE_DEFINE(portal_index, portal_http_service, "/", &index_resource);
HTTP_RESOURCE_DEFINE(portal_sensors, portal_http_service, "/api/sensors",
		     &sensors_resource);
HTTP_RESOURCE_DEFINE(portal_gorila, portal_http_service, "/gorila.jpg",
		     &gorila_resource);
