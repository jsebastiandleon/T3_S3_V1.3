/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Perro guardian del nodo: WDT hardware + supervision del avance del lazo
 * principal. Ver include/nodewdt.h para el razonamiento del diseño.
 */

#include "nodewdt.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#define NODEWDT_STACK  1536
/*
 * Prioridad COOPERATIVA (negativa): el supervisor tiene que poder expulsar al
 * hilo main (prioridad 0, expulsable) aunque este girando sin ceder la CPU.
 * Con una prioridad expulsable mas baja nunca llegaria a ejecutarse en ese
 * caso, que es justo uno de los que debe detectar.
 */
#define NODEWDT_PRIO   -1

static const struct device *wdt_dev;
static int                  wdt_channel;
static atomic_t             progress;

void nodewdt_alive(void)
{
	atomic_inc(&progress);
}

static void nodewdt_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	atomic_val_t last_seen   = atomic_get(&progress);
	int64_t      last_change = k_uptime_get();

	while (1) {
		const atomic_val_t now_seen = atomic_get(&progress);

		if (now_seen != last_seen) {
			last_seen   = now_seen;
			last_change = k_uptime_get();
		}

		const int64_t stalled_s = (k_uptime_get() - last_change) / 1000;

		if (stalled_s >= NODEWDT_STALL_S) {
			/*
			 * Se deja de alimentar A PROPOSITO. No se llama a
			 * sys_reboot() porque el estancamiento puede venir de un
			 * bloqueo que tambien afecte al camino de reinicio por
			 * software; dejar que muerda el hardware es la via que no
			 * depende de que el kernel siga sano. Ademas asi la causa
			 * queda registrada como RESET_WATCHDOG y sale por
			 * FPORT_DIAG en el siguiente arranque.
			 */
			printk("WDT: el lazo principal lleva %lld s sin avanzar "
			       "-> se deja de alimentar, reset en breve\n",
			       stalled_s);
			return;
		}

		(void)wdt_feed(wdt_dev, wdt_channel);
		k_sleep(K_SECONDS(NODEWDT_FEED_S));
	}
}

K_THREAD_STACK_DEFINE(nodewdt_stack, NODEWDT_STACK);
static struct k_thread nodewdt_tcb;

int nodewdt_start(void)
{
	wdt_dev = DEVICE_DT_GET(DT_ALIAS(watchdog0));
	if (!device_is_ready(wdt_dev)) {
		printk("WDT: dispositivo no listo\n");
		return -ENODEV;
	}

	/* El driver del ESP32 exige window.min == 0 y window.max != 0. */
	const struct wdt_timeout_cfg cfg = {
		.window   = { .min = 0U, .max = NODEWDT_HW_TIMEOUT_MS },
		.callback = NULL,   /* nada de printk desde ISR con el sistema colgado */
		.flags    = WDT_FLAG_RESET_SOC,
	};

	wdt_channel = wdt_install_timeout(wdt_dev, &cfg);
	if (wdt_channel < 0) {
		printk("WDT: install_timeout err %d\n", wdt_channel);
		return wdt_channel;
	}

	int ret = wdt_setup(wdt_dev, 0);

	if (ret < 0) {
		printk("WDT: setup err %d\n", ret);
		return ret;
	}

	k_thread_create(&nodewdt_tcb, nodewdt_stack, NODEWDT_STACK,
			nodewdt_thread, NULL, NULL, NULL,
			NODEWDT_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&nodewdt_tcb, "nodewdt");

	printk("WDT armado: hw %d ms, alimentado cada %d s, "
	       "estancamiento del lazo %d s\n",
	       NODEWDT_HW_TIMEOUT_MS, NODEWDT_FEED_S, NODEWDT_STALL_S);
	return 0;
}
