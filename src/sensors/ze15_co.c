/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Driver de aplicacion para el ZE15-CO (Winsen) en modo Q&A sobre UART.
 * Protocolo: tramas de 9 bytes a 9600 8N1.
 */

#include "sensors/ze15_co.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ze15_co, LOG_LEVEL_DBG);

#define ZE15CO_FRAME_LEN   9U
#define ZE15CO_START_BYTE  0xFFU
#define ZE15CO_CMD_READ    0x86U
#define ZE15CO_GAS_CO      0x04U

/* Comando de consulta (Q&A): FF 01 86 00 00 00 00 00 79 */
static const uint8_t ze15co_cmd_read[ZE15CO_FRAME_LEN] = {
	0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79
};

/* Timeout total de RX (ms). El sensor por defecto esta en modo upload
   activo y emite una trama cada 1s (solo conmuta a Q&A si recibe la
   consulta, y revierte tras 30s sin consultas — mas que nuestro periodo
   de envio). Con 1500ms se garantiza capturar una trama de upload
   completa aunque la consulta no llegue al sensor; si el sensor si
   responde en Q&A, la respuesta llega en ms y no se espera de mas. */
#define ZE15CO_RX_TIMEOUT_MS 1500

/* Si la consulta llega al sensor justo mientras emite una trama de upload,
   este la aborta a mitad y conmuta a Q&A sin responder (observado en HW:
   se reciben 2 bytes "ff 04" y luego silencio). En el reintento el sensor
   ya esta en Q&A y responde en milisegundos. */
#define ZE15CO_READ_ATTEMPTS 3

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

/* 9600 8N1: misma configuracion que el overlay; solo se usa para forzar
   el reset de los FIFOs via uart_configure(). */
static const struct uart_config ze15co_uart_cfg = {
	.baudrate  = 9600,
	.parity    = UART_CFG_PARITY_NONE,
	.stop_bits = UART_CFG_STOP_BITS_1,
	.data_bits = UART_CFG_DATA_BITS_8,
	.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
};

/* Un intento de consulta: vacia RX, envia el comando y espera una trama
   valida (Q&A FF 86 o upload FF 04) hasta el deadline.
   Retorna 0 con la trama en *frame, o -ETIMEDOUT. */
static int ze15co_try_query(const struct device *dev, uint8_t *frame)
{
	uint8_t scratch;
	int err;

	/* Resetear los FIFOs de hardware. En modo upload el sensor emite
	   9 bytes/s y durante los ~30s entre lecturas desborda el FIFO RX de
	   128 bytes del ESP32-S3; tras el overflow los punteros del FIFO
	   quedan corruptos y poll_in repite bytes fantasma para siempre (el
	   driver uart_esp32 no maneja RXFIFO_OVF en modo polled). La ruta de
	   uart_configure() ejecuta uart_hal_rxfifo_rst() y lo recupera. */
	err = uart_configure(dev, &ze15co_uart_cfg);
	if (err < 0) {
		LOG_WRN("uart_configure fallo: %d (sigo sin reset de FIFO)", err);
	}

	/* Vaciar cualquier resto en el RX (p.ej. tramas del modo upload). */
	while (uart_poll_in(dev, &scratch) == 0) {
		/* descartar */
	}

	/* Enviar el comando de consulta. */
	for (size_t i = 0; i < ZE15CO_FRAME_LEN; i++) {
		uart_poll_out(dev, ze15co_cmd_read[i]);
	}

	/* Leer hasta encontrar una trama valida antes del deadline. Acepta
	   tanto la respuesta Q&A (FF 86 ...) como una trama de upload activo
	   (FF 04 ...): el sensor solo opera en Q&A si la consulta le llega,
	   asi que el upload es el camino de respaldo. Ante trama corrupta o
	   falso start byte se desliza un byte y se resincroniza. */
	size_t idx = 0;
	int64_t deadline = k_uptime_get() + ZE15CO_RX_TIMEOUT_MS;

	while (true) {
		uint8_t b;

		if (uart_poll_in(dev, &b) != 0) {
			if (k_uptime_get() >= deadline) {
				LOG_WRN("ZE15-CO timeout: %u/%u bytes",
					(unsigned)idx, ZE15CO_FRAME_LEN);
				LOG_HEXDUMP_WRN(frame, idx, "ZE15-CO parcial");
				return -ETIMEDOUT;
			}
			k_msleep(1);
			continue;
		}

		if (idx == 0 && b != ZE15CO_START_BYTE) {
			continue; /* esperar al inicio de trama */
		}
		frame[idx++] = b;
		if (idx < ZE15CO_FRAME_LEN) {
			continue;
		}

		if ((frame[1] == ZE15CO_CMD_READ || frame[1] == ZE15CO_GAS_CO) &&
		    frame[ZE15CO_FRAME_LEN - 1] == ze15co_checksum(frame)) {
			return 0; /* trama valida */
		}

		LOG_HEXDUMP_DBG(frame, ZE15CO_FRAME_LEN, "ZE15-CO descartada");
		do {
			idx--;
			memmove(frame, frame + 1, idx);
		} while (idx > 0 && frame[0] != ZE15CO_START_BYTE);
	}
}

int ze15co_read(const struct device *dev, struct ze15co_data *data)
{
	uint8_t frame[ZE15CO_FRAME_LEN];
	int ret = -ETIMEDOUT;

	if (dev == NULL || data == NULL) {
		return -EINVAL;
	}

	for (int attempt = 1; attempt <= ZE15CO_READ_ATTEMPTS; attempt++) {
		ret = ze15co_try_query(dev, frame);
		if (ret == 0) {
			break;
		}
		if (attempt < ZE15CO_READ_ATTEMPTS) {
			LOG_WRN("ZE15-CO reintento %d/%d", attempt + 1,
				ZE15CO_READ_ATTEMPTS);
		}
	}
	if (ret < 0) {
		return ret;
	}

	/* Concentracion: bit7 del high byte = fallo de sensor.
	   ppm = ((high & 0x1F) * 256 + low) * 0.1
	   En la respuesta Q&A va en bytes 2-3; en la trama de upload, en 4-5. */
	const uint8_t hi = (frame[1] == ZE15CO_CMD_READ) ? frame[2] : frame[4];
	const uint8_t lo = (frame[1] == ZE15CO_CMD_READ) ? frame[3] : frame[5];

	data->sensor_fault = (hi & 0x80U) != 0U;
	data->co_ppm = (double)(((hi & 0x1FU) << 8) | lo) * 0.1;

	LOG_INF("CO=%d.%01d ppm%s [%s]", (int)data->co_ppm,
		(int)(data->co_ppm * 10) % 10,
		data->sensor_fault ? " (FAULT)" : "",
		(frame[1] == ZE15CO_CMD_READ) ? "Q&A" : "upload");

	return 0;
}
