/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SEN65 (Sensirion) — nodo de calidad de aire de la familia SEN6x.
 * Senales: PM1.0/2.5/4.0/10.0, humedad y temperatura ambiente, indice VOC e
 * indice NOx.
 *
 * Interfaz: I2C, 100 kbit/s (standard mode), sin clock-stretching.
 * Bus: I2C0  SDA=GPIO47  SCL=GPIO48 (mismo bus que el BME688; pull-ups 4.7k).
 * Direccion: 0x6B (7-bit) — definida en el overlay (nodo "sen65", alias
 * "sen65-sensor"). El BME688 esta en 0x77, asi que comparten bus sin colision.
 *
 * OJO HARDWARE: VDD = 3.3V (3.15-3.45V), NO 5V. Lleva ventilador y laser; tras
 * "Start Continuous Measurement" tarda ~1.1s en tener el primer dato y los
 * indices VOC/NOx tardan ~10-11s extra en converger (antes devuelven 0x7FFF).
 *
 * Protocolo (datasheet SEN6x, Farnell 4601635): palabras de 16 bits MSB-first,
 * cada una seguida de un CRC-8 (Dallas/Maxim, poly 0x31, init 0xFF). Los IDs de
 * comando son de 16 bits y NO llevan CRC.
 *
 * No existe driver nativo de la familia SEN6x en este arbol de Zephyr, por lo
 * que la integracion es a nivel de aplicacion (como el ZE15-CO), usando la API
 * i2c_* directamente sobre el controlador del bus.
 */

#ifndef INCLUDE_SENSORS_SEN6X_H
#define INCLUDE_SENSORS_SEN6X_H

#include <zephyr/device.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Datos de una medicion del SEN65 (ya convertidos a unidades fisicas).
 *
 *  - pm1_0, pm2_5, pm4_0, pm10_0 : masa de particulas en ug/m3
 *  - humidity                    : humedad relativa en %RH
 *  - temperature                 : temperatura ambiente en grados C
 *  - voc_index                   : indice VOC (1..500 tipico; 0 hasta converger)
 *  - nox_index                   : indice NOx (1..500 tipico; 0 hasta converger)
 *
 * Cualquier canal cuyo valor "desconocido" reporte el sensor (0xFFFF / 0x7FFF)
 * se entrega como 0.0 para no propagar valores centinela.
 */
struct sen6x_data {
	double pm1_0;
	double pm2_5;
	double pm4_0;
	double pm10_0;
	double humidity;
	double temperature;
	double voc_index;
	double nox_index;
};

/**
 * sen6x_init - Resuelve el bus/direccion del SEN65 desde el Device Tree y
 * arranca la medicion continua.
 *
 * Usa el alias "sen65-sensor" (nodo "sen65" en &i2c0). Verifica que el
 * controlador I2C este listo, opcionalmente lee el numero de serie como prueba
 * de presencia y envia "Start Continuous Measurement" para que haya datos en
 * los siguientes ciclos de lectura.
 *
 * @param[out] dev  En exito *dev apunta al controlador I2C del bus; NULL si
 *                  falla. (El puntero solo se usa como "handle de presencia"
 *                  para el lazo principal; la direccion se gestiona internamente.)
 *
 * @retval 0        Exito.
 * @retval -EINVAL  puntero nulo.
 * @retval -ENODEV  el bus I2C no esta listo.
 * @retval -EIO     el sensor no respondio (sin ACK / CRC invalido).
 */
int sen6x_init(const struct device **dev);

/**
 * sen6x_read - Lee la ultima medicion disponible del SEN65.
 *
 * Consulta "Get Data Ready"; si hay dato nuevo emite "Read Measured Values
 * SEN65" (0x0446, 8 palabras + CRC) y convierte cada canal a unidades fisicas.
 * Si aun no hay dato nuevo se devuelven las ultimas lecturas del sensor (este
 * mantiene el ultimo valor), por lo que la llamada no falla por ese motivo.
 *
 * @param dev   Puntero obtenido con sen6x_init() (no nulo).
 * @param data  Estructura de salida; se escribe completamente.
 *
 * @retval 0          Exito.
 * @retval -EINVAL    puntero nulo.
 * @retval -EIO       fallo de I2C o CRC invalido en la respuesta.
 */
int sen6x_read(const struct device *dev, struct sen6x_data *data);

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_SENSORS_SEN6X_H */
