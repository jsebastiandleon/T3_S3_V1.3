/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Driver de aplicacion para el ZE15-CO (Winsen) en modo Q&A sobre UART.
 * Protocolo: tramas de 9 bytes a 9600 8N1.
 */

#include "sensors/ze15_co.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ze15_co, LOG_LEVEL_DBG);

#define ZE15CO_FRAME_LEN   9U
#define ZE15CO_START_BYTE  0xFFU
#define ZE15CO_CMD_READ    0x86U

/* Comando de consulta (Q&A): FF 01 86 00 00 00 00 00 79 */
static const uint8_t ze15co_cmd_read[ZE15CO_FRAME_LEN] = {
	0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79
};

/* Timeout total para recibir la respuesta de 9 bytes (ms). A 9600 baud una
   trama tarda <10ms; el sensor responde casi de inmediato. 1s es holgado. */
#define ZE15CO_RX_TIMEOUT_MS 1000

/*
 * Checksum del protocolo Winsen: complemento a 2 de la suma de los bytes 1..7
 * (se excluyen el start byte 0 y el propio checksum byte 8).
 *   check = (~(b1 + b2 + ... + b7)) + 1
 */
static uint8_t ze15co_checksum(const uint8_t *frame)
{
	uint8_t sum = 0;

	for (size_t i = 1; i < ZE15CO_FRAME_LEN - 1; i++) {
		sum += frame[i];
	}
	return (uint8_t)((~sum) + 1U);
}

int ze15co_init(const struct device **dev)
{
	if (dev == NULL) {
		return -EINVAL;
	}

	*dev = DEVICE_DT_GET(DT_ALIAS(co_uart));

	if (!device_is_ready(*dev)) {
		LOG_ERR("ZE15-CO UART no listo (UART1 RX=GPIO40 TX=GPIO41); revisa "
			"cableado y alimentacion 5V del sensor");
		*dev = NULL;
		return -ENODEV;
	}

	LOG_INF("ZE15-CO listo: %s", (*dev)->name);
	return 0;
}

int ze15co_read(const struct device *dev, struct ze15co_data *data)
{
	uint8_t frame[ZE15CO_FRAME_LEN];
	uint8_t scratch;

	if (dev == NULL || data == NULL) {
		return -EINVAL;
	}

	/* Vaciar cualquier resto en el RX (p.ej. tramas del modo upload). */
	while (uart_poll_in(dev, &scratch) == 0) {
		/* descartar */
	}

	/* Enviar el comando de consulta. */
	for (size_t i = 0; i < ZE15CO_FRAME_LEN; i++) {
		uart_poll_out(dev, ze15co_cmd_read[i]);
	}

	/* Leer 9 bytes con deadline. Resincroniza al start byte 0xFF por si
	   habia bytes residuales de una trama de upload anterior. */
	size_t idx = 0;
	int64_t deadline = k_uptime_get() + ZE15CO_RX_TIMEOUT_MS;

	while (idx < ZE15CO_FRAME_LEN) {
		uint8_t b;

		if (uart_poll_in(dev, &b) == 0) {
			if (idx == 0 && b != ZE15CO_START_BYTE) {
				continue; /* esperar al inicio de trama */
			}
			frame[idx++] = b;
		} else if (k_uptime_get() >= deadline) {
			LOG_WRN("ZE15-CO timeout: %u/%u bytes", (unsigned)idx,
				ZE15CO_FRAME_LEN);
			return -ETIMEDOUT;
		} else {
			k_msleep(1);
		}
	}

	/* Validar cabecera y checksum. */
	if (frame[0] != ZE15CO_START_BYTE || frame[1] != ZE15CO_CMD_READ) {
		LOG_ERR("ZE15-CO cabecera invalida: %02x %02x", frame[0], frame[1]);
		return -EIO;
	}
	if (frame[ZE15CO_FRAME_LEN - 1] != ze15co_checksum(frame)) {
		LOG_ERR("ZE15-CO checksum invalido");
		return -EIO;
	}

	/* Concentracion: bit7 del high byte = fallo de sensor.
	   ppm = ((high & 0x1F) * 256 + low) * 0.1 */
	data->sensor_fault = (frame[2] & 0x80U) != 0U;
	data->co_ppm = (double)(((frame[2] & 0x1FU) << 8) | frame[3]) * 0.1;

	LOG_INF("CO=%d.%01d ppm%s", (int)data->co_ppm,
		(int)(data->co_ppm * 10) % 10,
		data->sensor_fault ? " (FAULT)" : "");

	return 0;
}
