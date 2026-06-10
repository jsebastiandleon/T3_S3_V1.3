/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * BM688 application-layer wrapper sobre el driver bosch,bme680 de Zephyr.
 * Bus: I2C0  SDA=GPIO47  SCL=GPIO48 (header lateral JP1; pull-ups externos 4.7k)
 * Direccion: 0x77 (SD0 a VCC)
 */

#ifndef INCLUDE_SENSORS_BM688_H
#define INCLUDE_SENSORS_BM688_H

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Datos de una medicion completa del BM688.
 *
 * Las unidades siguen la convencion del driver bosch,bme680 de Zephyr:
 *  - temperature    : grados Celsius
 *  - pressure       : Pascales
 *  - humidity       : porcentaje de humedad relativa (%RH)
 *  - gas_resistance : Ohms (0 si el calentador aun no estabilizo)
 */
struct bm688_data {
	double temperature;
	double pressure;
	double humidity;
	double gas_resistance;
};

/**
 * bm688_init - Obtiene y valida el device binding desde el Device Tree.
 *
 * Usa el alias "bme688-sensor" definido en el overlay.
 *
 * @param[out] dev  *dev apunta al device de Zephyr en exito, NULL si falla.
 *
 * @retval 0       Exito.
 * @retval -ENODEV El device no esta en el DT o no paso device_is_ready().
 */
int bm688_init(const struct device **dev);

/**
 * bm688_read_data - Dispara una medicion y llena *data.
 *
 * Llama a sensor_sample_fetch() y luego a sensor_channel_get() para cada
 * canal. La resistencia de gas puede retornar 0 en las primeras lecturas
 * mientras el calentador alcanza temperatura de regimen.
 *
 * @param dev   Puntero obtenido con bm688_init().
 * @param data  Estructura de salida; se escribe completamente.
 *
 * @retval 0    Exito.
 * @retval <0   Codigo de error de Zephyr (sensor_sample_fetch o
 *              sensor_channel_get fallaron).
 */
int bm688_read_data(const struct device *dev, struct bm688_data *data);

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_SENSORS_BM688_H */
