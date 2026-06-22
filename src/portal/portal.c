/*
 * Orquestacion del portal cautivo: levanta el SoftAP, fija IP estatica,
 * arranca el servidor DHCPv4, el responder DNS cautivo y el HTTP server.
 * Mantiene ademas el snapshot thread-safe de sensores que sirve /api/sensors.
 */
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/http/server.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>

#include "portal/portal.h"

LOG_MODULE_REGISTER(portal, LOG_LEVEL_INF);

/* Definido en captive_dns.c */
extern void captive_dns_start(void);

#define AP_IP      "192.168.4.1"
#define AP_NETMASK "255.255.255.0"
#define AP_SSID    "Gesinen_WildFire"   /* nombre fijo de la red del portal */
/* AP ABIERTO: sin contrasena (WIFI_SECURITY_TYPE_NONE). El usuario se conecta
   sin teclear clave y el portal cautivo se abre solo. */

/* ---- Snapshot de sensores ----------------------------------------------- */
static struct portal_sensors snapshot;
static struct k_mutex snapshot_lock;

void portal_update_sensors(const struct portal_sensors *s)
{
	k_mutex_lock(&snapshot_lock, K_FOREVER);
	snapshot = *s;
	snapshot.updated_uptime_ms = k_uptime_get();
	k_mutex_unlock(&snapshot_lock);
}

void portal_get_sensors(struct portal_sensors *out)
{
	k_mutex_lock(&snapshot_lock, K_FOREVER);
	*out = snapshot;
	k_mutex_unlock(&snapshot_lock);
}

/* ---- Boton de emergencia (SOS) ----------------------------------------- */
static atomic_t sos_pending;

void portal_request_sos(void)
{
	atomic_set(&sos_pending, 1);
}

bool portal_take_sos(void)
{
	/* atomic_set devuelve el valor previo: limpia y avisa si estaba pendiente. */
	return atomic_set(&sos_pending, 0) == 1;
}

/* ---- Arranque ------------------------------------------------------------ */
static bool started;

static int setup_ap_ip(struct net_if *ap)
{
	struct in_addr ip, mask, base;

	if (net_addr_pton(AF_INET, AP_IP, &ip) ||
	    net_addr_pton(AF_INET, AP_NETMASK, &mask)) {
		LOG_ERR("IP/netmask invalida");
		return -EINVAL;
	}

	net_if_ipv4_set_gw(ap, &ip);
	if (net_if_ipv4_addr_add(ap, &ip, NET_ADDR_MANUAL, 0) == NULL) {
		LOG_ERR("no se pudo fijar IP del AP");
		return -EIO;
	}
	if (!net_if_ipv4_set_netmask_by_addr(ap, &ip, &mask)) {
		LOG_ERR("no se pudo fijar netmask del AP");
	}

	/* Pool DHCP a partir de .10 */
	base = ip;
	base.s4_addr[3] += 9;
	if (net_dhcpv4_server_start(ap, &base) != 0) {
		LOG_ERR("no arranco el servidor DHCPv4");
		return -EIO;
	}
	LOG_INF("AP IP %s, DHCPv4 desde 192.168.4.10", AP_IP);
	return 0;
}

static int enable_softap(struct net_if *ap)
{
	struct wifi_connect_req_params p = {
		.ssid = (const uint8_t *)AP_SSID,
		.ssid_length = strlen(AP_SSID),
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
		.security = WIFI_SECURITY_TYPE_NONE,   /* red ABIERTA, sin clave */
	};

	int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, ap, &p,
			   sizeof(struct wifi_connect_req_params));
	if (ret) {
		LOG_ERR("AP_ENABLE err %d", ret);
		return ret;
	}
	LOG_INF("SoftAP ABIERTO (sin clave): SSID=\"%s\"", AP_SSID);
	return 0;
}

int portal_start(void)
{
	if (started) {
		return 0;
	}

	k_mutex_init(&snapshot_lock);
	portal_html_init();

	struct net_if *ap = net_if_get_wifi_sap();
	if (ap == NULL) {
		ap = net_if_get_first_wifi();
	}
	if (ap == NULL) {
		LOG_ERR("interfaz WiFi AP no disponible");
		return -ENODEV;
	}

	int ret = enable_softap(ap);
	if (ret) {
		return ret;
	}
	ret = setup_ap_ip(ap);
	if (ret) {
		return ret;
	}

	captive_dns_start();

	ret = http_server_start();
	if (ret) {
		LOG_ERR("http_server_start err %d", ret);
		return ret;
	}

	started = true;
	LOG_INF("PORTAL READY (http://%s)", AP_IP);
	return 0;
}
