/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Driver de aplicacion para el anemometro (velocidad + direccion) sobre
 * RS485 / Modbus RTU. Implementacion manual (uart_poll_in/out), igual estilo
 * que el ZE15-CO, para no arrastrar el subsistema CONFIG_MODBUS (que tomaria
 * el control del UART). Solo se necesita una lectura de registros holding.
 *
 * === MAPA MODBUS (POR DEFECTO, AJUSTAR TRAS PRUEBA EN BANCO) ===============
 * El proveedor no entrega la tabla de registros. Se usa el mapa mas habitual
 * de estos sensores integrados chinos; confirmar con el sensor real y tocar
 * SOLO estas constantes si algo no cuadra:
 *   - Baudrate:            4800 8N1  (algunos vienen a 9600; ver overlay)
 *   - Direccion esclavo:   0x01      (confirmado por la spec del proveedor)
 *   - Funcion:             0x03      (read holding registers)
 *   - Registro inicial:    0x0000
 *   - Nº de registros:     2         (0x0000 velocidad, 0x0001 direccion)
 *   - Velocidad:           reg[0] * 0.1  -> m/s
 *   - Direccion:           reg[1]        -> grados (0-359)
 * Variante habitual alternativa: direccion en "rumbo" 0-7 en reg[1] y grados
 * en reg[2] -> subir WIND_REG_COUNT a 3 y WIND_DIR_REG_IDX a 2.
 * ==========================================================================
 */

#include "sensors/anemometer.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(anemometer, LOG_LEVEL_DBG);

/* ---- Parametros Modbus (ver cabecera del fichero) ----------------------- */
#define WIND_SLAVE_ADDR   0x01U
#define WIND_FUNC_READ    0x03U
#define WIND_REG_START    0x0000U
#define WIND_REG_COUNT    2U
#define WIND_SPEED_REG_IDX 0U
#define WIND_DIR_REG_IDX   1U
#define WIND_SPEED_SCALE   0.1   /* reg -> m/s */

/* Baudrate: debe coincidir con current-speed del &uart2 en el overlay. */
#define WIND_BAUDRATE      4800

/* Trama de peticion: addr, func, reg_hi, reg_lo, cnt_hi, cnt_lo, crc_lo, crc_hi */
#define WIND_REQ_LEN       8U
/* Respuesta: addr, func, byte_count, data[2*N], crc_lo, crc_hi */
#define WIND_RESP_LEN      (5U + 2U * WIND_REG_COUNT)

/* Timeout de RX: a 4800 baud la respuesta (9 B) tarda ~19 ms; con 200 ms hay
   holgura de sobra incluso a baudrates bajos y para el turnaround del RS485. */
#define WIND_RX_TIMEOUT_MS 200
#define WIND_READ_ATTEMPTS 3

/* GPIO de direccion DE/RE del adaptador RS485 (OPCIONAL). Si el nodo
   /zephyr,user no declara wind-de-gpios, .port queda NULL y se asume que el
   adaptador conmuta solo (modo automatico). */
static const struct gpio_dt_spec rs485_de =
	GPIO_DT_SPEC_GET_OR(DT_PATH(zephyr_user), wind_de_gpios, {0});

/* CRC-16/Modbus (poly 0xA001, init 0xFFFF), devuelto en orden nativo; en la
   trama va little-endian (crc_lo primero). */
static uint16_t wind_crc16(const uint8_t *buf, size_t len)
{
	uint16_t crc = 0xFFFFU;

	for (size_t i = 0; i < len; i++) {
		crc ^= buf[i];
		for (int b = 0; b < 8; b++) {
			if (crc & 0x0001U) {
				crc = (crc >> 1) ^ 0xA001U;
			} else {
				crc >>= 1;
			}
		}
	}
	return crc;
}

int anemometer_init(const struct device **dev)
{
	if (dev == NULL) {
		return -EINVAL;
	}

	*dev = DEVICE_DT_GET(DT_ALIAS(wind_uart));

	if (!device_is_ready(*dev)) {
		LOG_ERR("Anemometro UART no listo (UART2 TX=GPIO39 RX=GPIO38); "
			"revisa cableado RS485 y alimentacion 5-24V del sensor");
		*dev = NULL;
		return -ENODEV;
	}

	if (rs485_de.port != NULL) {
		if (!gpio_is_ready_dt(&rs485_de)) {
			LOG_ERR("GPIO DE/RE del RS485 no listo");
			*dev = NULL;
			return -ENODEV;
		}
		/* Reposo en RX (DE/RE bajo: driver deshabilitado, receptor activo). */
		gpio_pin_configure_dt(&rs485_de, GPIO_OUTPUT_INACTIVE);
		LOG_INF("Anemometro: control DE/RE en GPIO (half-duplex manual)");
	} else {
		LOG_INF("Anemometro: sin GPIO DE/RE -> adaptador RS485 automatico");
	}

	LOG_INF("Anemometro listo: %s", (*dev)->name);
	return 0;
}

/* 8N1 al baudrate configurado: sirve para resetear los FIFOs via
   uart_configure() antes de cada transaccion (igual truco que el ZE15-CO). */
static const struct uart_config wind_uart_cfg = {
	.baudrate  = WIND_BAUDRATE,
	.parity    = UART_CFG_PARITY_NONE,
	.stop_bits = UART_CFG_STOP_BITS_1,
	.data_bits = UART_CFG_DATA_BITS_8,
	.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
};

/* Envia la peticion con el driver RS485 habilitado (DE alto) y devuelve a RX.
   Espera el airtime de la trama antes de bajar DE para no cortar el ultimo
   byte (uart_poll_out solo garantiza encolado, no fin de shift-out). */
static void wind_send_request(const struct device *dev, const uint8_t *req)
{
	if (rs485_de.port != NULL) {
		gpio_pin_set_dt(&rs485_de, 1); /* habilitar transmisor RS485 */
	}

	for (size_t i = 0; i < WIND_REQ_LEN; i++) {
		uart_poll_out(dev, req[i]);
	}

	if (rs485_de.port != NULL) {
		/* Tiempo de un caracter = 10 bits / baud. Espera WIND_REQ_LEN
		   caracteres + margen para vaciar el shift-register antes de RX. */
		uint32_t char_us = (10U * 1000000U) / WIND_BAUDRATE;
		k_busy_wait(char_us * (WIND_REQ_LEN + 2U));
		gpio_pin_set_dt(&rs485_de, 0); /* volver a recepcion */
	}
}

/* Un intento: resetea FIFO, envia la peticion y lee WIND_RESP_LEN bytes,
   resincronizando por la direccion de esclavo. Valida func, byte_count y CRC. */
static int wind_try_read(const struct device *dev, uint16_t *regs)
{
	uint8_t resp[WIND_RESP_LEN];
	uint8_t scratch;
	int err;

	err = uart_configure(dev, &wind_uart_cfg);
	if (err < 0) {
		LOG_WRN("uart_configure fallo: %d (sigo sin reset de FIFO)", err);
	}

	while (uart_poll_in(dev, &scratch) == 0) {
		/* vaciar RX */
	}

	/* Construir la peticion: addr, func, reg_start(be), count(be), crc(le). */
	uint8_t req[WIND_REQ_LEN] = {
		WIND_SLAVE_ADDR, WIND_FUNC_READ,
		(uint8_t)(WIND_REG_START >> 8), (uint8_t)(WIND_REG_START & 0xFF),
		(uint8_t)(WIND_REG_COUNT >> 8), (uint8_t)(WIND_REG_COUNT & 0xFF),
		0, 0,
	};
	uint16_t req_crc = wind_crc16(req, WIND_REQ_LEN - 2);
	req[6] = (uint8_t)(req_crc & 0xFF);
	req[7] = (uint8_t)(req_crc >> 8);

	wind_send_request(dev, req);

	size_t idx = 0;
	int64_t deadline = k_uptime_get() + WIND_RX_TIMEOUT_MS;

	while (true) {
		uint8_t b;

		if (uart_poll_in(dev, &b) != 0) {
			if (k_uptime_get() >= deadline) {
				LOG_WRN("Anemometro timeout: %u/%u bytes",
					(unsigned)idx, WIND_RESP_LEN);
				if (idx > 0) {
					LOG_HEXDUMP_WRN(resp, idx, "Anemometro parcial");
				}
				return -ETIMEDOUT;
			}
			k_msleep(1);
			continue;
		}

		if (idx == 0 && b != WIND_SLAVE_ADDR) {
			continue; /* esperar al inicio de trama (direccion de esclavo) */
		}
		resp[idx++] = b;
		if (idx < WIND_RESP_LEN) {
			continue;
		}

		/* Trama completa: validar func, byte_count y CRC. */
		uint16_t crc = wind_crc16(resp, WIND_RESP_LEN - 2);
		uint16_t crc_rx = resp[WIND_RESP_LEN - 2] |
				  ((uint16_t)resp[WIND_RESP_LEN - 1] << 8);

		if (resp[1] == WIND_FUNC_READ &&
		    resp[2] == (uint8_t)(2U * WIND_REG_COUNT) &&
		    crc == crc_rx) {
			for (unsigned r = 0; r < WIND_REG_COUNT; r++) {
				regs[r] = ((uint16_t)resp[3 + 2 * r] << 8) |
					  resp[4 + 2 * r];
			}
			return 0;
		}

		LOG_HEXDUMP_DBG(resp, WIND_RESP_LEN, "Anemometro descartada");
		/* Resincronizar: deslizar hasta el siguiente posible inicio. */
		do {
			idx--;
			memmove(resp, resp + 1, idx);
		} while (idx > 0 && resp[0] != WIND_SLAVE_ADDR);
	}
}

int anemometer_read(const struct device *dev, struct anemometer_data *data)
{
	uint16_t regs[WIND_REG_COUNT];
	int ret = -ETIMEDOUT;

	if (dev == NULL || data == NULL) {
		return -EINVAL;
	}

	for (int attempt = 1; attempt <= WIND_READ_ATTEMPTS; attempt++) {
		ret = wind_try_read(dev, regs);
		if (ret == 0) {
			break;
		}
		if (attempt < WIND_READ_ATTEMPTS) {
			LOG_WRN("Anemometro reintento %d/%d", attempt + 1,
				WIND_READ_ATTEMPTS);
		}
	}
	if (ret < 0) {
		return ret;
	}

	data->wind_speed_ms = (double)regs[WIND_SPEED_REG_IDX] * WIND_SPEED_SCALE;
	data->wind_dir_deg  = regs[WIND_DIR_REG_IDX];

	LOG_INF("Viento: %d.%01d m/s, dir %u deg",
		(int)data->wind_speed_ms,
		(int)(data->wind_speed_ms * 10) % 10,
		data->wind_dir_deg);

	return 0;
}
