#ifndef NODEWDT_H_
#define NODEWDT_H_

/*
 * Perro guardian del nodo.
 *
 * Hasta ahora no habia NINGUNO: si el lazo principal se quedaba bloqueado —en
 * un mutex, en un semaforo sin liberar, en un bucle— el nodo se quedaba mudo
 * indefinidamente sirviendo aun el portal WiFi, y nadie se enteraba. En los
 * logs del 2026-07-29 hubo 10 h de silencio compatibles con ese escenario.
 *
 * Diseño en dos niveles, porque un solo umbral no sirve:
 *
 *   1. El HILO SUPERVISOR alimenta el WDT hardware cada NODEWDT_FEED_S. Si el
 *      sistema entero se cuelga (kernel muerto, interrupciones desactivadas,
 *      inversion de prioridad que mata a todo el mundo), el supervisor deja de
 *      correr, nadie alimenta y el hardware reinicia en segundos.
 *
 *   2. El supervisor vigila ADEMAS que el lazo principal AVANCE. Si el
 *      contador de progreso no cambia en NODEWDT_STALL_S, deja de alimentar
 *      a proposito y fuerza el reinicio.
 *
 * ¿Por que el umbral de estancamiento es tan largo (30 min)? Porque el lazo
 * principal se bloquea de forma LEGITIMA durante mucho tiempo: lorawan_send()
 * hace k_sem_take(..., K_FOREVER) esperando a que el MAC termine, y un uplink
 * CONFIRMED a SF12 puede reintentar varias veces respetando el duty-cycle del
 * 1% — minutos. Un watchdog agresivo reiniciaria el nodo en mitad de un envio
 * valido y provocaria bucles de reinicio, que es PEOR que no tener watchdog.
 * (La solucion de fondo es sacar la radio a su propio hilo — Fase 3.)
 *
 * Un reinicio por watchdog NO es silencioso: hwinfo lo reporta como
 * RESET_WATCHDOG y el uplink de arranque (FPORT_DIAG) lo manda al servidor.
 */

/* Cada cuantos segundos alimenta el supervisor al WDT hardware. */
#define NODEWDT_FEED_S    2

/*
 * Timeout del WDT hardware, en ms segun la API de Zephyr.
 *
 * OJO: el driver del ESP32 pasa window.max DIRECTAMENTE como cuenta de ticks
 * del MWDT (wdt_esp32.c:119 -> wdt_hal_config_stage, documentado en el HAL
 * como "Number of WDT ticks"), y con su prescaler (40000) sobre el APB de
 * 80 MHz un tick son 500 us. Pero configura DOS etapas con el mismo valor
 * (interrupcion en la 0, reset en la 1) y las etapas del MWDT son
 * acumulativas, asi que los dos factores de 2 se cancelan y el reset acaba
 * cayendo cerca de window.max milisegundos.
 *
 * Ese razonamiento no lo he podido medir en hardware, asi que el valor se
 * elige a proposito de forma que un error de x2 en cualquier sentido de igual:
 * 20 s nominales quedan entre 10 y 40 s reales, y CUALQUIERA de esos valores
 * es enorme frente a los 2 s de alimentacion y minusculo frente a los 30 min
 * del umbral de estancamiento. La funcion no depende de acertar el numero.
 */
#define NODEWDT_HW_TIMEOUT_MS  20000

/*
 * Tiempo sin que el lazo principal avance antes de forzar el reinicio.
 *
 * 45 min sale de acotar el peor bloqueo LEGITIMO. Un uplink CONFIRMED sin ACK
 * reintenta hasta 8 veces respetando el duty-cycle del 1%: con LoRaMac bajando
 * el DR cada 2 intentos son ~10 min, y si se quedara clavado en SF12, ~22 min.
 * El bucle de rejoin encadena mas envios bloqueantes, pero marca vida en cada
 * vuelta (esta progresando, no colgado), asi que no cuenta.
 *
 * Un FALSO reinicio no seria solo una molestia: falsearia el diagnostico que
 * monta el FPORT_DIAG. Ver "RESET CAUSE: WATCHDOG" cuando en realidad hubo un
 * envio largo y valido mandaria a buscar un cuelgue inexistente. De ahi el
 * margen de 2x sobre el peor caso pesimista.
 *
 * Sigue siendo util: el silencio que hay que cazar duro 10 HORAS.
 */
#define NODEWDT_STALL_S   (45 * 60)

/*
 * Arranca el WDT hardware y el hilo supervisor. Llamar UNA vez, antes de
 * entrar en el lazo principal.
 *
 * Devuelve 0 si el perro guardian queda activo, o <0 si no se pudo armar. Un
 * fallo aqui NO debe abortar el arranque: es mejor un nodo sin watchdog que un
 * nodo que no arranca; pero hay que reportarlo.
 */
int nodewdt_start(void);

/*
 * "Sigo vivo": la llama el lazo principal en cada vuelta. Es lo unico que
 * impide que el supervisor deje de alimentar al perro.
 */
void nodewdt_alive(void);

#endif /* NODEWDT_H_ */
