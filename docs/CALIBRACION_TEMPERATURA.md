# Calibración — offset de temperatura

Dónde se configura: bloque **CALIBRACION — OFFSET DE TEMPERATURA** al principio
de [`src/main.c`](../src/main.c) (`TEMP_OFFSET_BM688_C`, `TEMP_OFFSET_SEN65_C`).

---

## 1. Por qué hace falta

Los dos sensores de temperatura del nodo miden **su propio encapsulado**, no el
aire, y los dos se autocalientan:

| Fuente de calor | Efecto |
|---|---|
| BM688: placa MOX del canal de gas a ~320 °C en cada ciclo | sesgo **positivo**, en el mismo chip que el termómetro |
| SEN65: ventilador + láser dentro del módulo | sesgo **positivo** |
| ESP32-S3 como SoftAP + SX1262 transmitiendo, en caja cerrada | calienta el aire interior |
| Radiación solar sobre el alojamiento (exterior) | varios °C en un montaje sin abrigo ventilado |

La exactitud de catálogo describe el chip en banco. Cifras exactas de las hojas
de datos:

| Sensor | Exactitud de temperatura | Fuente |
|---|---|---|
| BM688 | **±0.5 °C** (0–65 °C) | BST-BME688-DS000-03 Rev 1.3 (02/2024), Tabla 10 |
| SEN65 | típ. **0.45 °C**, máx. **±0.7 °C** (15–30 °C, 50 %RH) | SEN6x DS v0.91 (08/2025), Tabla 3 |

Los dos fabricantes avisan del problema en la propia hoja:

- Bosch, nota 19 de la Tabla 10: el valor *"depends on the PCB temperature,
  sensor element self-heating and ambient temperature and **is typically above
  ambient temperature**"*.
- Sensirion, nota 13 de la Tabla 3: el autocalentamiento **del módulo** ya viene
  compensado de fábrica según su nota de aplicación, y el sensor admite además
  sus propios parámetros de offset por I2C (comandos *Set Temperature Offset
  Parameters* §4.8.16 y *Set Temperature Acceleration Parameters* §4.8.17, que
  este firmware **no** usa).

De ahí que `TEMP_OFFSET_SEN65_C` suela necesitar bastante menos ajuste que el del
BM688: lo que corrige es el calor de **nuestra caja**, no el del módulo. En un
nodo desplegado el error lo domina el **montaje**, y es de varios grados; por eso
la compensación es **por montaje**, no por sensor suelto.

## 2. Convención del signo

El valor se **resta** a la lectura cruda, así que la constante son los **grados
que el sensor lee de más**:

```
offset = T_leída_por_el_sensor − T_real_del_aire
```

- Sensor marca 27.4 °C con el aire a 25.0 °C → `offset = +2.4`
- Si leyera de menos, el offset es **negativo**.
- `0.0` = sin compensar (valor de fábrica del repo).

## 3. Procedimiento de medida

1. **Nodo en su caja definitiva, cerrada y en régimen**: WiFi AP encendido y
   LoRa con su ciclo normal de envíos. Déjalo **≥ 30 min**; el
   autocalentamiento tarda en estabilizarse y medir en frío da un offset corto.
2. **A la sombra y sin corrientes**, con un termómetro de referencia pegado a la
   caja — misma masa de aire, nunca al sol.
3. Anota varias parejas *(lectura del log, referencia)* durante **≥ 15 min** y
   promedia la diferencia.
   > El log del driver (`bm688.c`, línea `T=..C`) imprime el valor **crudo**,
   > sin compensar: es exactamente el que necesitas aquí. Los valores del
   > payload y del portal ya salen corregidos.
4. Escribe el promedio en `TEMP_OFFSET_*_C`, **sube `FW_VERSION`** y reflashea.

Repite si cambias la caja, la ubicación o el ciclo de transmisión.

## 4. Qué corrige y qué no

**Corrige el sesgo, no la incertidumbre.** Tras calibrar sigues teniendo la
dispersión del sensor (~±1 °C) más lo que varíe el montaje con el sol y el
viento. No prometas ±0.5 °C en campo.

| Consumidor | ¿Le afecta? |
|---|---|
| Payload de datos, FPort 2 (bytes 1–2 y 23–24) | **Sí** |
| Payload de alerta, FPort 4 (bytes 1–2) | **Sí** |
| Umbral fijo `TH_TEMP_MAX` (58 °C, EN 54-5 clase A1) | **Sí** — es el que más lo necesitaba |
| Portal cautivo (dashboard web) | **Sí** |
| Rate-of-rise `TH_ROR_CPMIN` (8 °C/min, EN 54-5) | **No** — es una *diferencia* en una ventana de 60 s: un sesgo constante se cancela solo |
| Log crudo del driver `bm688.c` | **No** (intencionadamente: es la referencia para calibrar) |

Esa última fila explica por qué el disparo por subida rápida ya era fiable sin
calibrar y el umbral fijo no: con +3 °C de sesgo, `TH_TEMP_MAX 58.0` dispara a
55 °C reales.

## 5. Implementación

La resta se aplica **nada más leer**, antes de acumular la media y antes de la
snapshot del portal (`src/main.c`, dentro del lazo principal), de modo que todo
aguas abajo ve el mismo valor corregido y coherente:

```c
sd.temperature -= TEMP_OFFSET_BM688_C;   /* BM688 */
ad.temperature -= TEMP_OFFSET_SEN65_C;   /* SEN65 */
```
