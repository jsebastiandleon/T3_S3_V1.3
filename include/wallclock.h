#ifndef WALLCLOCK_H_
#define WALLCLOCK_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Reloj de pared del nodo: hora LOCAL derivada de la hora de red LoRaWAN.
 *
 * El nodo no tiene RTC con pila ni acceso a NTP (el SoftAP no da salida a
 * Internet). La unica fuente de hora es el comando MAC DeviceTimeReq de
 * LoRaWAN, que el network server contesta con la epoca GPS.
 *
 * Se usa para apagar el SoftAP en la franja nocturna (ver AP_OFF_* en main.c).
 * Precision necesaria: minutos. No sirve para sellar eventos con exactitud.
 */

/* Segundos entre la epoca Unix (1970-01-01) y la epoca GPS (1980-01-06).
   lorawan_device_time_get() devuelve epoca GPS; sumando esto se obtiene Unix.
   Se ignoran los 18 s de segundos intercalares GPS-UTC: son irrelevantes
   frente a una frontera horaria, y anadirlos exigiria mantener una tabla. */
#define WALLCLOCK_GPS_UNIX_OFFSET  315964800U

/* Hora local descompuesta. */
struct wallclock_tm {
	int      year;   /* p.ej. 2026        */
	uint8_t  month;  /* 1-12              */
	uint8_t  day;    /* 1-31              */
	uint8_t  hour;   /* 0-23, hora LOCAL  */
	uint8_t  min;    /* 0-59              */
	bool     dst;    /* true = horario de verano (UTC+2) */
};

/*
 * Convierte una epoca GPS (la que entrega lorawan_device_time_get()) a hora
 * local europea y la descompone en 'out'.
 *
 * Aplica las reglas de la UE: UTC+1 en invierno, UTC+2 desde el ultimo domingo
 * de marzo a las 01:00 UTC hasta el ultimo domingo de octubre a las 01:00 UTC.
 * Verificado contra la base de datos tz (Europe/Madrid) sobre 3 anios de
 * muestreo horario, incluidos ambos bordes de cambio.
 *
 * Devuelve false si gps_s es 0 (sin sincronizar).
 */
bool wallclock_from_gps(uint32_t gps_s, struct wallclock_tm *out);

#endif /* WALLCLOCK_H_ */
