/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Driver de aplicacion para el Sensirion SEN65 (familia SEN6x) sobre I2C.
 * Senales: PM1.0/2.5/4.0/10.0, RH, T, indice VOC, indice NOx.
 *
 * Protocolo (datasheet SEN6x, Farnell 4601635):
 *   - Direccion I2C 0x6B (7-bit), 100 kbit/s, sin clock-stretching.
 *   - Los comandos son IDs de 16 bits (MSB-first) y NO llevan CRC.
 *   - Los datos van en palabras de 16 bits (MSB-first), cada una seguida de un
 *     CRC-8 Dallas/Maxim (poly 0x31, init 0xFF, sin reflexion, XOR final 0x00).
 *   - Tras enviar un comando de lectura hay que esperar su tiempo de ejecucion
 *     antes de emitir el header de lectura (el sensor NACKea mientras procesa).
 */

#include "sensors/sen6x.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sen6x, LOG_LEVEL_INF);

/* ---- IDs de comando (subconjunto usado para el SEN65) -------------------- */
#define SEN6X_CMD_START_MEAS    0x0021U  /* Start Continuous Measurement SEN6x */
#define SEN6X_CMD_STOP_MEAS     0x0104U  /* Stop Measurement SEN6x             */
#define SEN6X_CMD_GET_READY     0x0202U  /* Get Data Ready SEN6x               */
#define SEN65_CMD_READ_MEAS     0x0446U  /* Read Measured Values SEN65         */
#define SEN6X_CMD_DEVICE_RESET  0xD304U  /* Device Reset SEN6x                 */

/* ---- Tiempos de ejecucion (ms) segun datasheet, con holgura -------------- */
#define SEN6X_EXEC_START_MS     50
#define SEN6X_EXEC_GET_READY_MS 20
#define SEN6X_EXEC_READ_MS      20
#define SEN6X_EXEC_RESET_MS     1200  /* reset + arranque hasta estado idle    */

/* Numero de palabras de "Read Measured Values SEN65" (24 bytes = 8 x [u16+CRC]) */
#define SEN65_MEAS_WORDS        8U

/* Valores centinela que el sensor devuelve cuando un canal es desconocido. */
#define SEN6X_UNKNOWN_U16       0xFFFFU
#define SEN6X_UNKNOWN_S16       0x7FFFU

/*
 * Direccion + bus resueltos del Device Tree (nodo "sen65" en &i2c0). Se guardan
 * a nivel de fichero porque el lazo principal solo maneja un puntero a device
 * como "handle de presencia"; la direccion la gestiona este driver.
 */
static const struct i2c_dt_spec sen65 = I2C_DT_SPEC_GET(DT_NODELABEL(sen65));

/*
 * CRC-8 Dallas/Maxim tal cual lo especifica el datasheet (Tabla 70):
 * poly 0x31, init 0xFF, sin reflexion de entrada/salida, XOR final 0x00.
 * Comprobacion: CRC(0xBE,0xEF) = 0x92.
 */
static uint8_t sen6x_crc8(const uint8_t *data, size_t len)
{
	uint8_t crc = 0xFFU;

	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++) {
			if (crc & 0x80U) {
				crc = (uint8_t)((crc << 1) ^ 0x31U);
			} else {
				crc = (uint8_t)(crc << 1);
			}
		}
	}
	return crc;
}

/* Envia un ID de comando de 16 bits (MSB-first, sin CRC). */
static int sen6x_send_command(uint16_t cmd)
{
	uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFFU) };

	return i2c_write_dt(&sen65, buf, sizeof(buf));
}

/*
 * Emite un comando de lectura y recupera 'n_words' palabras de 16 bits.
 * Espera 'exec_ms' entre la escritura del comando y el header de lectura,
 * valida el CRC de cada palabra y entrega los valores ya ensamblados en 'out'.
 */
static int sen6x_read_words(uint16_t cmd, uint16_t *out, size_t n_words,
			    int exec_ms)
{
	uint8_t rx[SEN65_MEAS_WORDS * 3U]; /* 3 bytes por palabra: MSB,LSB,CRC */
	int ret;

	if (n_words > SEN65_MEAS_WORDS) {
		return -EINVAL;
	}

	ret = sen6x_send_command(cmd);
	if (ret < 0) {
		LOG_ERR("SEN65: comando 0x%04x sin ACK: %d", cmd, ret);
		return ret;
	}

	/* El sensor NACKea mientras procesa: esperar el tiempo de ejecucion. */
	k_msleep(exec_ms);

	ret = i2c_read_dt(&sen65, rx, n_words * 3U);
	if (ret < 0) {
		LOG_ERR("SEN65: lectura de 0x%04x fallo: %d", cmd, ret);
		return ret;
	}

	for (size_t w = 0; w < n_words; w++) {
		const uint8_t *p = &rx[w * 3U];

		if (sen6x_crc8(p, 2U) != p[2]) {
			LOG_ERR("SEN65: CRC invalido en palabra %u de 0x%04x",
				(unsigned)w, cmd);
			return -EIO;
		}
		out[w] = (uint16_t)((p[0] << 8) | p[1]);
	}
	return 0;
}

/* uint16 -> double aplicando escala, mapeando el centinela "desconocido" a 0. */
static double sen6x_scale_u16(uint16_t raw, double scale)
{
	if (raw == SEN6X_UNKNOWN_U16) {
		return 0.0;
	}
	return (double)raw / scale;
}

/* int16 -> double aplicando escala, mapeando el centinela "desconocido" a 0. */
static double sen6x_scale_s16(uint16_t raw, double scale)
{
	if (raw == SEN6X_UNKNOWN_S16) {
		return 0.0;
	}
	return (double)((int16_t)raw) / scale;
}

int sen6x_init(const struct device **dev)
{
	if (dev == NULL) {
		return -EINVAL;
	}
	*dev = NULL;

	if (!device_is_ready(sen65.bus)) {
		LOG_ERR("SEN65: bus I2C (%s) no listo", sen65.bus->name);
		return -ENODEV;
	}

	/*
	 * Device Reset deja el sensor en un estado conocido (idle) tanto tras un
	 * power-on como tras un reset en caliente del MCU (donde el sensor podria
	 * seguir midiendo). Si no hay sensor en el bus, el write NACKea aqui y se
	 * retorna -EIO sin bloquear el arranque.
	 */
	int ret = sen6x_send_command(SEN6X_CMD_DEVICE_RESET);
	if (ret < 0) {
		LOG_ERR("SEN65: no responde en 0x%02x (revisa cableado/3V3): %d",
			sen65.addr, ret);
		return -EIO;
	}
	k_msleep(SEN6X_EXEC_RESET_MS);

	/* Arrancar la medicion continua: ~1.1s hasta el primer dato; los indices
	   VOC/NOx tardan ~10-11s extra en converger. */
	ret = sen6x_send_command(SEN6X_CMD_START_MEAS);
	if (ret < 0) {
		LOG_ERR("SEN65: Start Continuous Measurement fallo: %d", ret);
		return -EIO;
	}
	k_msleep(SEN6X_EXEC_START_MS);

	*dev = sen65.bus;
	LOG_INF("SEN65 listo en I2C 0x%02x (bus %s)", sen65.addr, sen65.bus->name);
	return 0;
}

int sen6x_read(const struct device *dev, struct sen6x_data *data)
{
	uint16_t w[SEN65_MEAS_WORDS];
	uint16_t ready[1];
	int ret;

	if (dev == NULL || data == NULL) {
		return -EINVAL;
	}

	/* Get Data Ready: byte1 == 0x01 si hay dato nuevo. Si no lo hay, el sensor
	   conserva la ultima lectura, asi que seguimos adelante igualmente (no es
	   un error) y devolvemos el ultimo valor disponible. */
	ret = sen6x_read_words(SEN6X_CMD_GET_READY, ready, 1U,
			       SEN6X_EXEC_GET_READY_MS);
	if (ret == 0 && (ready[0] & 0x00FFU) == 0U) {
		LOG_DBG("SEN65: aun sin dato nuevo; se devuelve el ultimo");
	}

	ret = sen6x_read_words(SEN65_CMD_READ_MEAS, w, SEN65_MEAS_WORDS,
			       SEN6X_EXEC_READ_MS);
	if (ret < 0) {
		return ret;
	}

	/* Orden y escalas segun "Read Measured Values SEN65" (Tabla 35):
	 *   0..3 PM1.0/2.5/4.0/10.0 uint16 /10  [ug/m3]
	 *   4    RH  int16 /100  [%]
	 *   5    T   int16 /200  [C]
	 *   6    VOC int16 /10   [indice]
	 *   7    NOx int16 /10   [indice] */
	data->pm1_0       = sen6x_scale_u16(w[0], 10.0);
	data->pm2_5       = sen6x_scale_u16(w[1], 10.0);
	data->pm4_0       = sen6x_scale_u16(w[2], 10.0);
	data->pm10_0      = sen6x_scale_u16(w[3], 10.0);
	data->humidity    = sen6x_scale_s16(w[4], 100.0);
	data->temperature = sen6x_scale_s16(w[5], 200.0);
	data->voc_index   = sen6x_scale_s16(w[6], 10.0);
	data->nox_index   = sen6x_scale_s16(w[7], 10.0);

	LOG_INF("SEN65 PM2.5=%d.%01d ug/m3 VOC=%d NOx=%d RH=%d%% T=%dC",
		(int)data->pm2_5, (int)(data->pm2_5 * 10) % 10,
		(int)data->voc_index, (int)data->nox_index,
		(int)data->humidity, (int)data->temperature);
	return 0;
}
