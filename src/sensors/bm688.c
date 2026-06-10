/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sensors/bm688.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bm688, LOG_LEVEL_DBG);

int bm688_init(const struct device **dev)
{
	if (dev == NULL) {
		return -EINVAL;
	}

	*dev = DEVICE_DT_GET(DT_ALIAS(bme688_sensor));

	if (!device_is_ready(*dev)) {
		LOG_ERR("BM688 no disponible en bus I2C0 (SDA=GPIO47, SCL=GPIO48); revisa cableado y pull-ups 4.7k");
		*dev = NULL;
		return -ENODEV;
	}

	LOG_INF("BM688 listo: %s", (*dev)->name);
	return 0;
}

int bm688_read_data(const struct device *dev, struct bm688_data *data)
{
	struct sensor_value temp, press, hum, gas;
	int ret;

	if (dev == NULL || data == NULL) {
		return -EINVAL;
	}

	ret = sensor_sample_fetch(dev);
	if (ret < 0) {
		LOG_ERR("sensor_sample_fetch fallo: %d", ret);
		return ret;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	if (ret < 0) {
		LOG_ERR("No se pudo leer temperatura: %d", ret);
		return ret;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_PRESS, &press);
	if (ret < 0) {
		LOG_ERR("No se pudo leer presion: %d", ret);
		return ret;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_HUMIDITY, &hum);
	if (ret < 0) {
		LOG_ERR("No se pudo leer humedad: %d", ret);
		return ret;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_GAS_RES, &gas);
	if (ret < 0) {
		/*
		 * La resistencia de gas puede fallar en las primeras lecturas
		 * mientras el calentador de 320 °C alcanza regimen (aprox. 197 ms).
		 * No se trata como error fatal.
		 */
		LOG_WRN("Gas resistance no disponible aun (ret=%d)", ret);
		gas.val1 = 0;
		gas.val2 = 0;
	}

	data->temperature    = (double)temp.val1  + (double)temp.val2  / 1000000.0;
	/* El driver bosch,bme680 entrega SENSOR_CHAN_PRESS en kPa (val1=kPa,
	   val2=fraccion). El contrato de bm688_data->pressure son Pascales,
	   asi que escalamos x1000 (100.132 kPa -> 100132 Pa). */
	data->pressure       = ((double)press.val1 + (double)press.val2 / 1000000.0) * 1000.0;
	data->humidity       = (double)hum.val1   + (double)hum.val2   / 1000000.0;
	data->gas_resistance = (double)gas.val1   + (double)gas.val2   / 1000000.0;

	/* Formato entero para evitar dependencia de CONFIG_CBPRINTF_FP_SUPPORT */
	LOG_INF("T=%d.%02dC  P=%dPa  H=%d.%02d%%  Gas=%dOhm",
		temp.val1,  (int)(temp.val2  / 10000),
		(uint32_t)data->pressure,
		hum.val1,   (int)(hum.val2   / 10000),
		gas.val1);

	return 0;
}
