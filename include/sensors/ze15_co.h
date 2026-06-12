/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ZE15-CO (Winsen) — modulo electroquimico de monoxido de carbono.
 * Interfaz: UART 9600 8N1, TTL 3V. Modo Q&A (pregunta/respuesta).
 * Bus: UART1  RX=GPIO40 (<- TXD/PIN8 del sensor)  TX=GPIO41 (-> RXD/PIN7).
 *
 * OJO HARDWARE: el ZE15-CO se alimenta con 5~12V DC (PIN15), NO con 3.3V.
 * El nivel logico del UART si es 3V -> compatible directo con el ESP32-S3
 * sin level shifter. Preheat ~30s antes de lecturas validas.
 */

#ifndef INCLUDE_SENSORS_ZE15_CO_H
#define INCLUDE_SENSORS_ZE15_CO_H

#include <zephyr/device.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Datos de una medicion del ZE15-CO.
 *
 *  - co_ppm        : concentracion de CO en ppm (resolucion 0.1, rango 0-500)
 *  - sensor_fault  : true si el sensor reporta fallo (bit 7 del high byte)
 */
struct ze15co_data {
	double co_ppm;
	bool   sensor_fault;
};

/**
 * ze15co_init - Obtiene y valida el device UART desde el Device Tree.
 *
 * Usa el alias "co-uart" definido en el overlay (&uart1).
 *
 * @param[out] dev  *dev apunta al device UART en exito, NULL si falla.
 *
 * @retval 0       Exito.
 * @retval -EINVAL puntero nulo.
 * @retval -ENODEV el device no esta listo.
 */
int ze15co_init(const struct device **dev);

/**
 * ze15co_read - Lee la concentracion de CO (Q&A o upload activo).
 *
 * Vacia el RX, envia el comando de consulta (FF 01 86 ...) y espera una
 * trama valida de 9 bytes: la respuesta Q&A (FF 86 ...) o, como respaldo,
 * la trama de upload activo (FF 04 ...) que el sensor emite cada 1s en su
 * modo por defecto. El modulo conmuta a Q&A al recibir la consulta y
 * revierte a upload tras 30s sin consultas, por lo que con periodos de
 * lectura >=30s el camino normal es la trama de upload.
 *
 * @param dev   Puntero obtenido con ze15co_init().
 * @param data  Estructura de salida.
 *
 * @retval 0         Exito.
 * @retval -EINVAL   puntero nulo.
 * @retval -ETIMEDOUT no llego respuesta completa a tiempo.
 * @retval -EIO      cabecera o checksum invalidos.
 */
int ze15co_read(const struct device *dev, struct ze15co_data *data);

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_SENSORS_ZE15_CO_H */
