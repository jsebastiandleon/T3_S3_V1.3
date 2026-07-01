/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Anemometro + veleta "Carbon Integrated Anemometer" (velocidad + direccion).
 * Interfaz: RS485, protocolo Modbus RTU (funcion 0x03, direccion de esclavo 0x01
 * por defecto). El ESP32-S3 no tiene transceptor RS485: se usa un modulo
 * adaptador RS485<->TTL (MAX485 o similar) entre el sensor y el UART2.
 *
 * Bus: UART2  TX=GPIO39 (-> DI del adaptador)  RX=GPIO38 (<- RO del adaptador).
 * Direccion half-duplex: GPIO42 -> DE+RE del adaptador (OPCIONAL; si el modulo
 * es de conmutacion automatica, no se cablea y el driver lo detecta ausente).
 *
 * OJO HARDWARE: el sensor se alimenta a 5~24V DC (cable rojo +, negro GND) con
 * salida RS485. Las lineas RS485 son A (amarillo) y B (verde). La masa del
 * sensor debe unirse a la del ESP32 (masa comun). Cables marron/blanco =
 * calefaccion 12~24V (anti-hielo), OPCIONAL, no se conectan aqui.
 */

#ifndef INCLUDE_SENSORS_ANEMOMETER_H
#define INCLUDE_SENSORS_ANEMOMETER_H

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Datos de una medicion del anemometro.
 *
 *  - wind_speed_ms  : velocidad del viento en m/s (rango tipico 0-60)
 *  - wind_dir_deg   : direccion del viento en grados (0-359; 0 = Norte)
 */
struct anemometer_data {
	double   wind_speed_ms;
	uint16_t wind_dir_deg;
};

/**
 * anemometer_init - Obtiene y valida el device UART desde el Device Tree.
 *
 * Usa el alias "wind-uart" (&uart2) y, si esta declarado, el GPIO de direccion
 * DE/RE del nodo /zephyr,user (propiedad wind-de-gpios).
 *
 * @param[out] dev  *dev apunta al device UART en exito, NULL si falla.
 *
 * @retval 0       Exito.
 * @retval -EINVAL puntero nulo.
 * @retval -ENODEV el device no esta listo.
 */
int anemometer_init(const struct device **dev);

/**
 * anemometer_read - Lee velocidad y direccion via Modbus RTU (funcion 0x03).
 *
 * Emite una consulta de lectura de registros holding y valida la respuesta
 * (cabecera + CRC-16 Modbus). Si hay GPIO DE/RE, lo conmuta alrededor del TX.
 *
 * @param dev   Puntero obtenido con anemometer_init().
 * @param data  Estructura de salida.
 *
 * @retval 0          Exito.
 * @retval -EINVAL    puntero nulo.
 * @retval -ETIMEDOUT no llego respuesta completa a tiempo.
 * @retval -EIO       cabecera, longitud o CRC invalidos.
 */
int anemometer_read(const struct device *dev, struct anemometer_data *data);

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_SENSORS_ANEMOMETER_H */
