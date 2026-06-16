/*
 * Tabla de recursos del portal. Aqui es donde se "escala": cada funcion nueva
 * = un HTTP_RESOURCE_DEFINE mas. De momento:
 *   GET /             -> dashboard HTML (mutable, desde el almacen portal_html)
 *   GET /api/sensors  -> JSON con el ultimo snapshot de sensores
 *   <fallback>        -> sirve el dashboard ante CUALQUIER ruta (captura del
 *                        portal: las URLs de sondeo del SO reciben la pagina)
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
	static char json[256];

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

		int len = snprintf(json, sizeof(json),
			"{\"bm688\":%s,\"temperature\":%d.%02d,\"humidity\":%d.%02d,"
			"\"pressure\":%d,\"gas\":%d,"
			"\"co\":%s,\"co_ppm\":%d.%d,\"age_ms\":%lld}",
			s.bm688_valid ? "true" : "false", t_i, t_f, h_i, h_f,
			(int)s.pressure, (int)s.gas_resistance,
			s.co_valid ? "true" : "false", co_i, co_f,
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

/* ---- Servicio HTTP en :80 ----------------------------------------------- */
static uint16_t http_port = 80;

/* El fallback (ultimo arg util) sirve el dashboard ante cualquier ruta no
 * mapeada -> dispara la deteccion de portal cautivo del SO. */
HTTP_SERVICE_DEFINE(portal_http_service, NULL, &http_port, 2, 4, NULL,
		    &index_resource, NULL);

HTTP_RESOURCE_DEFINE(portal_index, portal_http_service, "/", &index_resource);
HTTP_RESOURCE_DEFINE(portal_sensors, portal_http_service, "/api/sensors",
		     &sensors_resource);
HTTP_RESOURCE_DEFINE(portal_gorila, portal_http_service, "/gorila.jpg",
		     &gorila_resource);
