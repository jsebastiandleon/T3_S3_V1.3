/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hora local del nodo a partir de la hora de red LoRaWAN (epoca GPS).
 *
 * Los algoritmos de calendario son los de Howard Hinnant ("chrono-Compatible
 * Low-Level Date Algorithms"), de dominio publico: aritmetica entera pura, sin
 * bucles ni tablas y validos para cualquier anio. Se prefieren a time.h porque
 * la libc de Zephyr no trae zonas horarias y porque asi el calculo es
 * verificable byte a byte contra la base de datos tz.
 */

#include "wallclock.h"

/* Dias desde 1970-01-01 para una fecha civil (proleptic Gregorian). */
static int64_t days_from_civil(int y, unsigned int m, unsigned int d)
{
	y -= (m <= 2);

	const int64_t      era = (y >= 0 ? y : y - 399) / 400;
	const unsigned int yoe = (unsigned int)(y - era * 400);          /* 0..399   */
	const unsigned int doy = (153U * (m + (m > 2 ? -3U : 9U)) + 2U) / 5U + d - 1U;
	const unsigned int doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;

	return era * 146097 + (int64_t)doe - 719468;
}

/* Inversa: fecha civil a partir de los dias desde 1970-01-01. */
static void civil_from_days(int64_t z, int *y, unsigned int *m, unsigned int *d)
{
	z += 719468;

	const int64_t      era = (z >= 0 ? z : z - 146096) / 146097;
	const unsigned int doe = (unsigned int)(z - era * 146097);       /* 0..146096 */
	const unsigned int yoe = (doe - doe / 1460U + doe / 36524U -
				  doe / 146096U) / 365U;                 /* 0..399    */
	const int64_t      yy  = (int64_t)yoe + era * 400;
	const unsigned int doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
	const unsigned int mp  = (5U * doy + 2U) / 153U;                 /* 0..11     */
	const unsigned int dd  = doy - (153U * mp + 2U) / 5U + 1U;       /* 1..31     */
	const unsigned int mm  = mp + (mp < 10U ? 3U : -9U);             /* 1..12     */

	*y = (int)(yy + (mm <= 2U));
	*m = mm;
	*d = dd;
}

/* Dia del mes del ultimo domingo. Solo se llama para marzo y octubre, que
   tienen 31 dias, asi que el ultimo dia es siempre el 31. */
static unsigned int last_sunday_of_31day_month(int y, unsigned int m)
{
	const int64_t days = days_from_civil(y, m, 31);
	/* 1970-01-01 fue jueves; con dias >= 0, (dias + 4) % 7 da 0 = domingo. */
	const int     dow  = (int)(((days % 7) + 11) % 7);

	return 31U - (unsigned int)dow;
}

/*
 * Regla de la UE (Directiva 2000/84/CE): el horario de verano va del ultimo
 * domingo de marzo a las 01:00 UTC al ultimo domingo de octubre a las 01:00
 * UTC. El cambio se define en UTC, igual en todos los husos de la UE, lo que
 * evita tener que razonar sobre la hora local durante la propia transicion.
 */
static bool eu_dst_active(int64_t utc)
{
	int          y;
	unsigned int m, d;

	/* Division con redondeo hacia abajo: utc siempre es > 0 aqui (posterior
	   a 1980), asi que la division entera basta. */
	civil_from_days(utc / 86400, &y, &m, &d);

	if (m < 3U || m > 10U) {
		return false;
	}
	if (m > 3U && m < 10U) {
		return true;
	}

	/* Marzo u octubre: hay que comparar con el instante exacto del cambio. */
	const int64_t change = days_from_civil(y, m, last_sunday_of_31day_month(y, m))
			       * 86400 + 3600;

	return (m == 3U) ? (utc >= change) : (utc < change);
}

bool wallclock_from_gps(uint32_t gps_s, struct wallclock_tm *out)
{
	if (gps_s == 0U || out == NULL) {
		return false;
	}

	const int64_t utc   = (int64_t)gps_s + WALLCLOCK_GPS_UNIX_OFFSET;
	const bool    dst   = eu_dst_active(utc);
	const int64_t local = utc + (dst ? 7200 : 3600);

	int          y;
	unsigned int m, d;

	civil_from_days(local / 86400, &y, &m, &d);

	const int64_t sod = local % 86400;   /* segundos del dia */

	out->year  = y;
	out->month = (uint8_t)m;
	out->day   = (uint8_t)d;
	out->hour  = (uint8_t)(sod / 3600);
	out->min   = (uint8_t)((sod % 3600) / 60);
	out->dst   = dst;

	return true;
}
