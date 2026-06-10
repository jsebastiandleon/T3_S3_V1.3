# Depuración BM688 + LoRaWAN — T3-S3 V1.3 / Zephyr RTOS

Registro de la sesión de diagnóstico (2026-06-10) sobre la rama `feature/BM688`.
Se identificaron y resolvieron **3 fallos reales**, todos validados en hardware.

---

## Resumen

| # | Síntoma en el log | Causa raíz | Archivo del fix | Estado |
|---|---|---|---|---|
| 1 | `BM688 INIT ERR: -19` (`-ENODEV`) | Dirección I2C en el Device Tree (`0x76`) no coincide con el hardware (`0x77`) | `boards/esp32s3_devkitc_esp32s3_procpu.overlay` | ✅ Verificado |
| 2 | `MlmeConfirm failed : Rx 2 timeout` / `JOIN ERR: -116` (join inestable) | DevNonce se reiniciaba a 0 en cada arranque + contador del servidor desincronizado | `prj.conf` (+ reset device en ChirpStack) | ✅ Verificado |
| 3 | `P=100Pa` (presión 1000× baja) | El driver `bosch,bme680` entrega `SENSOR_CHAN_PRESS` en kPa, no en Pa | `src/sensors/bm688.c` | ✅ Verificado |

Eventos **benignos** descartados (no son fallos):
- `SHA-256 comparison failed ... Attempting to boot anyway` → flujo ESP *simple boot* sin secure boot. El `Expected: 0000...` es relleno no inicializado.
- `Gas=0Ohm` en las primeras lecturas → calentador de 320 °C estabilizándose (~197 ms). Manejado como warning no fatal.

---

## Fallo 1 — BM688 `-ENODEV` (dirección I2C)

**Evidencia.** El escáner I2C de arranque reportaba un único esclavo, y **no** en la dirección esperada:

```
I2C0 SCAN (100kHz, SDA=47 SCL=48):
  ACK 0x77
I2C0 SCAN done: 1 dispositivo(s)
E: BM688 no disponible en bus I2C0 ...
BM688 INIT ERR: -19 (continua sin sensor)
```

**Causa raíz.** El overlay declaraba `bme688@76` / `reg = <0x76>`. El driver `bosch,bme680`
lee el *chip-id* en `0x76` durante su init; al no recibir ACK, el init falla,
`device_is_ready()` devuelve `false` y el wrapper retorna `-ENODEV`. El BME680/688
solo tiene dos direcciones posibles según el pin SDO (GND = 0x76, VCC = 0x77); el
módulo real tiene **SDO a VCC**, por lo que responde en `0x77`. El escaneo es la fuente
de verdad.

**Fix.** Alinear el nodo del Device Tree con la dirección real:

```dts
bme688: bme688@77 {        /* antes @76 */
    compatible = "bosch,bme680";
    reg = <0x77>;          /* antes 0x76 */
};
```

**Validación.**
```
  ACK 0x77
I: BM688 listo: bme688@77
BM688 READY
```

---

## Fallo 2 — Join OTAA inestable (DevNonce)

**Evidencia.** `-116` = `-ETIMEDOUT`: el Join-Accept no llegó en RX1/RX2. El patrón era
determinista (varios fallos antes de unir, o todos los intentos fallando tras un
reflasheo):

```
JOINING... attempt 1
E: MlmeConfirm failed : Rx 2 timeout
JOIN ERR: -116 (retry in 10s)
...
```

**Causa raíz.** El firmware sembraba `dev_nonce = 0` en cada arranque del MCU. En
LoRaWAN 1.0.x el DevNonce debe ser monotónicamente creciente y el Network Server
(ChirpStack) descarta **silenciosamente** todo Join-Request con un DevNonce ya visto
(anti-replay). Desde el dispositivo eso se ve exactamente como un `Rx 2 timeout`.
Tras varios reflasheos, el contador del servidor quedó por delante del dispositivo.

**Fix (firmware).** Persistir el contexto MAC (DevNonce incluido) en flash. `prj.conf`:

```conf
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_NVS=y
CONFIG_SETTINGS=y
CONFIG_LORAWAN_NVM_SETTINGS=y
```

`lorawan_start()` llama internamente a `settings_subsys_init()` y restaura el
contexto; usa la `storage_partition` por defecto del board (192K @ `0x3b0000`),
**sin** cambios en el flash layout. Con NVM activo, `lorawan_join_otaa()` solo aplica
el `dev_nonce` del `join_cfg` cuando `CONFIG_LORAWAN_NVM_NONE` (ver
`zephyr/subsys/lorawan/loramac-node/lorawan.c`), por lo que el `dev_nonce = 0` del
`main.c` pasa a ser inocuo: el stack gestiona e incrementa el nonce por su cuenta.

**Fix (servidor, una sola vez).** Como el contador del servidor quedó adelantado, hubo
que resincronizar: en ChirpStack se **borró y recreó el device** (mismo DevEUI /
JoinEUI / AppKey), lo que limpia el set de DevNonce usados.

**Validación.** El NVM persiste y avanza entre arranques (`data wra` incrementa
`1,820` → `2,9cc` en reinicios sucesivos), y une de forma consistente:

```
I: 8 Sectors of 4096 bytes
I: data wra: 2, 9cc
...
JOINING... attempt 1
I: Joined network! DevAddr: 001633b6
I: Datarate changed: DR_0
JOINED!
```

> Nota: un `Rx 2 timeout` aislado antes de unir (une al 2º intento) ya **no** es el bug
> de DevNonce — es timing normal de LoRaWAN (ventana RX, duty-cycle del gateway).

> ⚠️ `west flash --erase` borra la `storage_partition` → reaparece el desfase y hay que
> volver a resetear el device en ChirpStack una vez. Un `west flash` normal **no** la borra.

---

## Fallo 3 — Presión 1000× baja (unidades kPa vs Pa)

**Evidencia.**
```
I: T=27.59C  P=100Pa  H=53.23%  Gas=13007335Ohm
```
100 Pa es físicamente imposible a nivel de mar (debe rondar ~100000 Pa / 1000 hPa).

**Causa raíz.** El driver `bosch,bme680` entrega `SENSOR_CHAN_PRESS` en **kPa**, no en
Pa (`zephyr/drivers/sensor/bosch/bme680/bme680.c`):

```c
/* data->calc_press has a resolution of 1 Pa. */
val->val1 = data->calc_press / 1000;          /* parte entera en kPa -> 100 */
val->val2 = (data->calc_press % 1000) * 1000; /* fracción */
```

`bm688.c` componía `val1 + val2/1e6` (≈100.1 kPa) y lo guardaba en
`bm688_data->pressure`, cuyo contrato documentado son Pascales. El campo `press_pa`
del payload LoRaWAN quedaba 1000× bajo.

**Fix.** Escalar a Pa en el wrapper (origen del dato), respetando el contrato del struct:

```c
/* SENSOR_CHAN_PRESS viene en kPa; el contrato de pressure son Pascales -> x1000 */
data->pressure = ((double)press.val1 + (double)press.val2 / 1000000.0) * 1000.0;
```

Temperatura (°C), humedad (%RH) y resistencia de gas (Ω) ya estaban en sus unidades
correctas.

**Validación.**
```
I: T=27.70C  P=100873Pa  H=53.35%  Gas=13007335Ohm
```
100873 Pa ≈ 1008.7 hPa. Correcto.

---

## Pendiente / a vigilar

- **Resistencia de gas constante.** `Gas=13007335Ohm` aparece idéntico al bit en lecturas
  consecutivas. En ambiente estable puede ser real, pero conviene verificar que **varíe**
  al cambiar el entorno cerca del sensor antes de usarlo como señal de calidad de aire.
