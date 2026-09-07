# Conformidad con el pliego — qué cumple el nodo y qué no

Requisito evaluado, literal:

> Anemómetro/veleta digital; temperatura y humedad relativa (±2 %); CO (0–500 ppm,
> ±5 %) y CO₂ equivalente (0–5.000 ppm, ±3 %); presión atmosférica y calidad del
> aire (IAQ).

**Todas las cifras de este documento están tomadas de las tres hojas de datos
oficiales**, no de memoria ni de fichas comerciales:

| Sensor | Documento | Versión |
|---|---|---|
| Bosch BME688 (BM688) | `BST-BME688-DS000-03` | Rev. 1.3, 02/2024 |
| Winsen ZE15-CO | *Carbon Monoxide Module ZE15-CO — Manual* | v1.1, 12-04-2018 |
| Sensirion SEN65 | *SEN6x – Datasheet* | v0.91, 08/2025 |

---

## 1. Veredicto

| # | Requisito | Estado | Resumen |
|---|---|---|---|
| 1 | Anemómetro / veleta digital | ✅ **Disponible, sin integrar en esta rama** | Implementado en la rama `feature/anemometro` (RS485/Modbus RTU, 0–60 m/s, 0–360°). No está fusionado en `feature/schedule_AP`. |
| 2 | Temperatura | ✅ **Cumple** | BM688 ±0.5 °C · SEN65 típ. 0.45 °C / máx. ±0.7 °C. |
| 3 | Humedad relativa **±2 %** | ❌ **No cumple** | BM688 **±3 %RH** · SEN65 típ. **4.5 %RH**, máx. **±6 %RH**. Ninguno alcanza ±2 %. |
| 4 | CO **0–500 ppm** | ✅ **Cumple exacto** | ZE15-CO: rango 0–500 ppm, resolución 0.1 ppm. |
| 4b | CO **±5 %** | ⚠️ **No acreditable** | La hoja del ZE15-CO **no publica ninguna cifra de exactitud**. |
| 5 | CO₂ equivalente **0–5.000 ppm ±3 %** | ❌ **No cumple** | Ningún sensor del nodo mide CO₂. Ver §4 — el ±3 % no lo cumple ni el sensor correcto del mercado. |
| 6 | Presión atmosférica | ✅ **Cumple** | BM688: 300–1100 hPa, **±0.6 hPa** absoluta. |
| 7 | Calidad del aire (IAQ) | ⚠️ **Parcial** | Se emite VOC/NOx/PM (Sensirion, con especificación). El **IAQ de Bosch** existe pero **solo con la librería BSEC**, hoy no integrada. |

**Marcador: 3 cumplen · 1 disponible sin fusionar · 2 parciales/no acreditables · 2 no cumplen.**

---

## 2. Corrección importante sobre el anemómetro

El requisito 1 **sí está resuelto en el proyecto**, en la rama
`feature/anemometro` (commit `974f548`), que aporta:

- `src/sensors/anemometer.c` + `include/sensors/anemometer.h` — Modbus RTU manual
  sobre UART2 (GPIO38 RX / GPIO39 TX, DE/RE en GPIO42).
- Overlay, payload ampliado, decoder de ChirpStack y snapshot al portal.
- Sensor: velocidad **0–60 m/s**, dirección **0–360°**, precisión de proveedor
  **±(0.3 + 0.03·V) m/s** y **±1°**, arranque ≤0.3 m/s, RS485 a 5–24 V (requiere
  fuente externa y adaptador MAX485; ver [ANEMOMETER_INTEGRATION.md](ANEMOMETER_INTEGRATION.md)
  en esa rama).

⚠️ **No está fusionado en `feature/schedule_AP`**, que es la rama de este
firmware. El nodo tal y como se compila aquí **no** mide viento. Para cumplir el
pliego hay que integrar esa rama y validar el conjunto.

> Nota: la precisión del anemómetro procede de la hoja del proveedor, no de una
> de las tres hojas de datos verificadas arriba.

---

## 3. Temperatura y humedad

### Temperatura ✅

| Sensor | Exactitud | Condiciones | Fuente |
|---|---|---|---|
| BM688 | **±0.5 °C** | 0–65 °C | Tabla 10 |
| SEN65 | típ. **0.45 °C** · máx. **±0.7 °C** | 15–30 °C, 50 %RH | Tabla 3 |

Ambas hojas advierten del autocalentamiento: Bosch (nota 19) dice que su lectura
*"is typically above ambient temperature"*, y Sensirion (nota 13) compensa el del
módulo de fábrica. El firmware incorpora compensación de offset por montaje
(`TEMP_OFFSET_*_C`, ver [CALIBRACION_TEMPERATURA.md](CALIBRACION_TEMPERATURA.md)):
**sin calibrar el montaje, la incertidumbre real en campo es de varios grados**,
no la de catálogo.

### Humedad relativa ❌ — el ±2 % no se alcanza

| Sensor | Exactitud | Condiciones | Fuente |
|---|---|---|---|
| BM688 | **±3 %RH** (incluye histéresis) | 20–80 %RH, 25 °C | Tabla 9 |
| BM688 | histéresis ±1.5 %RH · deriva 0.5 %RH/año | 25 °C | Tabla 9 |
| SEN65 | típ. **4.5 %RH** · máx. **±6 %RH** | 25 °C, 30–70 %RH | Tabla 3 |

El mejor de los dos se queda en **±3 %RH**, un 50 % por encima de lo exigido.
Para cumplir ±2 % haría falta un sensor de la clase **SHT4x** (±1.0–1.5 %RH).

---

## 4. CO y CO₂ equivalente

### CO: rango ✅, exactitud ⚠️

Tabla completa de *Technical Parameters* del ZE15-CO:

| Parámetro | Valor de hoja |
|---|---|
| Gas | Monóxido de carbono (CO) |
| **Rango de detección** | **0–500 ppm** ✅ |
| Resolución | 0.1 ppm |
| Gases interferentes | *"Alcohol &etc."* |
| Tiempo de precalentamiento | 30 s |
| Tiempo de respuesta / recuperación | ≤30 s / ≤30 s |
| Tensión de trabajo | 5–12 V DC |
| Temperatura de trabajo | **−10 … 55 °C** |
| Humedad de trabajo | 15–90 %RH (sin condensación) |
| Vida útil | 3–5 años (en aire) |

El rango exigido se cumple **exactamente**. Pero la hoja v1.1 **no contiene
ninguna cifra de exactitud, precisión ni error** — ni ±5 %, ni ±% de fondo de
escala, ni repetibilidad. No se puede acreditar el ±5 % con este documento; si el
pliego lo exige por escrito, hay que pedir a Winsen una declaración específica o
calibrar contra patrón.

> ⚠️ **Aviso operativo, no del pliego:** la temperatura máxima de trabajo del
> ZE15-CO es **55 °C**, por debajo del umbral térmico de incendio configurado en
> el firmware (`TH_TEMP_MAX` = 58 °C, EN 54-5 clase A1). En un fuego declarado el
> sensor de CO está fuera de su rango de operación antes de que salte el umbral
> de calor.

### CO₂ equivalente: ❌ no cumple, y el ±3 % es inalcanzable

**Ninguno de los tres sensores mide CO₂.** Punto por punto:

**a) El SEN65 no tiene canal de CO₂.** Tabla de variantes de la página 1 del
datasheet SEN6x, literal:

| Variante | Señales |
|---|---|
| SEN60 | PM |
| SEN63C | PM, RH & T, **CO2** |
| **SEN65** | **PM, RH & T, VOC, NOx** ← el nuestro, sin CO₂ |
| SEN66 | PM, RH & T, VOC, NOx, **CO2** |
| SEN68 | PM, RH & T, VOC, NOx, HCHO |

**b) El BM688 solo daría eCO₂ con BSEC, y sin especificar.** La Tabla 20
(salidas de BSEC) describe el canal así, literal:

> *"CO2 equivalents (ppm) — Estimation of the CO2 level in ppm. **The sensor does
> not directly measure CO2**, but derives this from the average correlation
> between VOCs and CO2 in **human's exhaled breath**."*

Tres problemas, todos bloqueantes:
1. **No está integrado**: requiere la librería **BSEC**, binario **cerrado bajo
   licencia** (§4.2 del datasheet). El driver `bosch,bme680` de Zephyr que usamos
   entrega resistencia de gas en ohmios, nada más.
2. **La hoja no publica ni rango ni exactitud para el eCO₂.** No hay ±3 % que
   invocar, ni 0–5.000 ppm.
3. **El modelo físico no aplica**: la correlación es con **aire exhalado humano**.
   En un nodo forestal de exterior no hay respiración humana que correlacionar; el
   número sería carente de significado.

**c) Ni siquiera el sensor correcto cumple el ±3 %.** Si se sustituyera el SEN65
por el **SEN66** (misma familia, con NDIR de CO₂), su Tabla 5 da:

| Rango | Exactitud de hoja | Traducido a % de lectura |
|---|---|---|
| 400–1.000 ppm | ±(50 ppm + 2.5 % m.v.) | a 1.000 ppm → ±75 ppm = **±7.5 %** |
| 1.001–2.000 ppm | ±(50 ppm + 3 % m.v.) | a 2.000 ppm → ±110 ppm = **±5.5 %** |
| 2.001–5.000 ppm | ±(40 ppm + 5 % m.v.) | a 5.000 ppm → ±290 ppm = **±5.8 %** |

Y el SEN63C es peor: ±(100 ppm + 10 % m.v.) en 400–5.000 ppm. Además ambos exigen
autocalibración (ASC) con exposición a aire fresco de 400 ppm **al menos una vez
por semana** para mantener esa especificación.

**Conclusión:** el **±3 % sobre 0–5.000 ppm no lo cumple ningún NDIR de esta
gama**. Es una tolerancia que conviene renegociar con quien redactó el pliego,
no un hueco que se tape comprando otro sensor.

---

## 5. Presión atmosférica ✅

| Parámetro | Valor | Condiciones | Fuente |
|---|---|---|---|
| Rango de operación | **300–1100 hPa** | — | Key features |
| **Exactitud absoluta** | **±0.6 hPa** | 300–1100 hPa, 0–65 °C | Tabla 9 |
| Exactitud relativa | ±0.12 hPa | 700–1100 hPa, 25–40 °C | Tabla 9 |
| Estabilidad a largo plazo | ±1.0 hPa/año | — | Tabla 9 |
| Ruido RMS | 0.12 Pa (≈1.7 cm) | — | Key features |

Cumple sin reservas. Es el requisito mejor cubierto del pliego.

---

## 6. Calidad del aire (IAQ) ⚠️

Hay que separar dos cosas que el pliego mezcla:

### Lo que sí emitimos hoy, con especificación

| Magnitud | Rango | Especificación de hoja | Fuente |
|---|---|---|---|
| Índice VOC | 1–500 | variación entre unidades <±15 puntos (o 15 % m.v.); repetibilidad <±5 | SEN6x Tabla 4 |
| Índice NOx | 1–500 | variación entre unidades <±50; repetibilidad <±10 | SEN6x Tabla 4 |
| PM1.0 / PM2.5 | 0–1.000 µg/m³ | (5 µg/m³ + 5 % m.v.) de 0 a 100; 10 % m.v. de 100 a 1.000 | SEN6x Tabla 2 |
| PM4 / PM10 | 0–1.000 µg/m³ | 25 µg/m³ de 0 a 100; 25 % m.v. de 100 a 1.000 | SEN6x Tabla 2 |
| Resistencia de gas | 1 kΩ–100 MΩ | ruido 1.5 % RMS | BME688 Tabla 2 |

El VOC tarda **<1 h** en cumplir especificación tras el encendido y el NOx
**<6 h** (SEN6x Tabla 4). Las PM están calibradas contra un **TSI DustTrak DRX
8533** en modo ambiente.

### Lo que el pliego llama "IAQ" y no emitimos

El **IAQ** con mayúsculas es un producto de Bosch y **solo existe con BSEC**
(nota 3 de la Tabla 3 del datasheet: *"IAQ parameters only apply for the
combination of BME688 together with the Bosch Software Environmental Cluster
(BSEC) solution"*). Su especificación:

| Parámetro | Valor |
|---|---|
| Rango IAQ | 0–500, resolución 1 |
| Estado de exactitud | 0–3 (0 durante la estabilización, 3 en régimen) |
| Desviación entre sensores | **±15 %** |
| Deriva por siloxanos | ±1 % / ±4 |

Obsérvese que **Bosch no publica una exactitud absoluta del IAQ**: es un índice
relativo autocalibrado (el propio datasheet explica que el algoritmo ajusta la
escala con el histórico de ~4 días para que IAQ≈50 sea "aire típicamente bueno" y
IAQ≈200 "aire típicamente contaminado"). No es una magnitud trazable a patrón.

**Si el pliego exige literalmente la etiqueta "IAQ"**, hay que integrar BSEC
(licencia + binario cerrado + ~4 días de calibración por nodo). **Si admite "un
índice de calidad del aire especificado"**, el índice VOC del SEN65 ya lo cubre,
y con mejor documentación de incertidumbre.

---

## 7. Qué haría falta para cumplir el pliego completo

| Hueco | Acción | Coste real |
|---|---|---|
| Viento | Fusionar `feature/anemometro` en la rama de producción y validar | Bajo — ya está escrito |
| HR ±2 % | Añadir SHT4x (±1.0–1.5 %RH) | Sensor nuevo, I2C, hueco en payload |
| CO ±5 % | Declaración de Winsen o calibración contra patrón | Documental / laboratorio |
| CO₂ | SEN65 → **SEN66** (o añadir SCD4x) | Cambio de sensor; **aun así no da ±3 %** |
| IAQ literal | Integrar BSEC (licencia Bosch) | Licencia + integración + calibración |
| ±3 % de CO₂ | **Renegociar la tolerancia** | Ningún producto de esta gama la cumple |
