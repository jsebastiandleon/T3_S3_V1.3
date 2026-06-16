/*
 * Responder DNS cautivo: hilo propio escuchando UDP:53 que contesta CUALQUIER
 * consulta tipo A con la IP del AP (192.168.4.1). Asi, cuando el movil sondea
 * sus URLs de deteccion de portal (captive.apple.com, connectivitycheck...),
 * resuelven al portal y el SO lanza el pop-up automaticamente.
 *
 * Implementacion minima a proposito: no parsea RR, solo refleja la pregunta y
 * adjunta una respuesta A. Suficiente para captura; escalable si se requiere.
 */
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(captive_dns, LOG_LEVEL_INF);

#define DNS_PORT       53
#define DNS_BUF_SIZE   512
#define AP_IP_BE0      192   /* 192.168.4.1 en orden de red */
#define AP_IP_BE1      168
#define AP_IP_BE2      4
#define AP_IP_BE3      1

static void captive_dns_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	int sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		LOG_ERR("socket() err %d", errno);
		return;
	}

	struct sockaddr_in bind_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(DNS_PORT),
		.sin_addr.s_addr = INADDR_ANY,
	};
	if (zsock_bind(sock, (struct sockaddr *)&bind_addr,
		       sizeof(bind_addr)) < 0) {
		LOG_ERR("bind(:53) err %d", errno);
		zsock_close(sock);
		return;
	}
	LOG_INF("DNS cautivo escuchando en :53");

	uint8_t buf[DNS_BUF_SIZE];

	while (1) {
		struct sockaddr_in src;
		socklen_t slen = sizeof(src);

		int n = zsock_recvfrom(sock, buf, sizeof(buf), 0,
				       (struct sockaddr *)&src, &slen);
		if (n < 12) {
			continue; /* mas corto que la cabecera DNS */
		}

		/* Localizar el fin de la seccion de pregunta (QNAME + QTYPE +
		 * QCLASS). Solo procesamos QDCOUNT==1. */
		uint16_t qdcount = (buf[4] << 8) | buf[5];
		if (qdcount != 1) {
			continue;
		}

		size_t q = 12;
		while (q < (size_t)n && buf[q] != 0) {
			q += buf[q] + 1; /* saltar etiqueta */
		}
		q += 1;        /* byte 0x00 final del QNAME */
		q += 4;        /* QTYPE(2) + QCLASS(2)       */
		if (q > (size_t)n || q + 16 > DNS_BUF_SIZE) {
			continue;
		}

		/* Cabecera: marcar respuesta (QR=1, RA=1), ANCOUNT=1. */
		buf[2] = 0x81; /* QR=1, Opcode=0, AA=0, TC=0, RD (espejo) */
		buf[3] = 0x80; /* RA=1, RCODE=0                          */
		buf[6] = 0x00; buf[7] = 0x01; /* ANCOUNT = 1             */
		buf[8] = 0x00; buf[9] = 0x00; /* NSCOUNT = 0             */
		buf[10] = 0x00; buf[11] = 0x00; /* ARCOUNT = 0           */

		/* Respuesta: puntero al QNAME, A/IN, TTL=60, RDATA=AP_IP. */
		size_t a = q;
		buf[a++] = 0xC0; buf[a++] = 0x0C;         /* name -> off 12   */
		buf[a++] = 0x00; buf[a++] = 0x01;         /* TYPE A           */
		buf[a++] = 0x00; buf[a++] = 0x01;         /* CLASS IN         */
		buf[a++] = 0x00; buf[a++] = 0x00;
		buf[a++] = 0x00; buf[a++] = 0x3C;         /* TTL = 60 s       */
		buf[a++] = 0x00; buf[a++] = 0x04;         /* RDLENGTH = 4     */
		buf[a++] = AP_IP_BE0; buf[a++] = AP_IP_BE1;
		buf[a++] = AP_IP_BE2; buf[a++] = AP_IP_BE3;

		(void)zsock_sendto(sock, buf, a, 0,
				   (struct sockaddr *)&src, slen);
	}
}

#define CAPTIVE_DNS_STACK 2048
#define CAPTIVE_DNS_PRIO  7

K_THREAD_STACK_DEFINE(captive_dns_stack, CAPTIVE_DNS_STACK);
static struct k_thread captive_dns_tcb;

void captive_dns_start(void)
{
	k_thread_create(&captive_dns_tcb, captive_dns_stack,
			CAPTIVE_DNS_STACK, captive_dns_thread,
			NULL, NULL, NULL, CAPTIVE_DNS_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&captive_dns_tcb, "captive_dns");
}
