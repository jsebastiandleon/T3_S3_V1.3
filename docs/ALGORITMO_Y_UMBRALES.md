# Algoritmo de detección de incendio y umbrales

> Revisión crítica de la lógica de alarma del nodo T3-S3 (Gandía WildFire) y
> propuesta de algoritmo, apoyadas en normas de producto y en literatura
> revisada por pares. Estado del firmware analizado: **v2.10** (`FW_VERSION
> 0x020A`, commits `47c5ceb` + `2fff435`).
>
> Todas las referencias están al final, numeradas **[1]…[12]**, con una nota
> explícita sobre cuáles he verificado contra la fuente y cuáles no.

---

## 1. Qué mide el nodo

| Sensor | Magnitudes | Papel en la detección |
|---|---|---|
| **BME688** (I2C0) | temperatura, humedad, presión, resistencia de gas (MOX) | calor; humedad para corregir PM |
| **ZE15-CO** (UART1) | CO en ppm (0–500, resolución 0.1) | gas de combustión |
| **SEN65** (I2C1) | PM1.0 / PM2.5 / PM4 / PM10, humedad, temperatura, índice VOC, índice NOx | humo; calor de respaldo |

No hay CO₂, ni IAQ, ni b-VOC: eso exigiría la librería BSEC de Bosch, hoy no
integrada (ver `CONFORMIDAD_PLIEGO.md`). Lo señalo porque **la ausencia de CO₂
cierra una puerta concreta**: el cociente CO/CO₂ es el discriminante clásico
entre combustión con llama y combustión latente, y sin CO₂ no podemos usarlo.

---

## 2. El algoritmo actual (v2.10)

```
Por cada ciclo (5 s):
  1. Comparar cada variable con un umbral ABSOLUTO fijo.
  2. Flanco de subida -> marcar alerta; re-armar al bajar de umbral x 0.9.
  3. Rate-of-rise térmico: (T_ahora - T_hace_60s) >= 8 °C/min.
  4. FUEGO si coinciden >= 2 de 3 familias:
       HUMO = PM2.5 o PM10 ;  CO ;  CALOR = umbral fijo o rate-of-rise
```

Lo que está **bien planteado** y hay que conservar:

- **La confirmación multicriterio.** Es el principio de EN 54-30 y EN 54-31
  [2][3]: no declarar fuego por una sola familia de indicio. EN 54-31 exige
  explícitamente que *"al menos las señales de humo y de monóxido de carbono se
  evalúen de forma continua"* [3].
- **La histéresis y el cooldown**, que protegen el duty-cycle.
- **El canal de salud del nodo** (FPort 5) y, desde v2.9, la máscara de validez
  del FPort 4: un sensor caído apaga sus umbrales, y eso debe verse.

### 2.1 Las cinco debilidades

**D1 — Todos los umbrales son absolutos y fijos.** El nodo está *a la
intemperie*, no en una habitación. Su línea base cambia con la estación, la
hora y el viento. Un umbral fijo es a la vez demasiado sensible en un día de
calima y demasiado sordo en una noche limpia.

**D2 — El PM10 es criterio de alarma independiente, y no debería serlo.** El
PM10 es el **mejor indicador de polvo mineral y el peor de humo**. En la costa
mediterránea la calima sahariana supera 150 µg/m³ de PM10 con regularidad y sin
un solo incendio. Hoy ese episodio enciende el bit de HUMO él solo.

**D3 — El umbral de VOC está conceptualmente mal casado.** El índice VOC de
Sensirion **no es una medida absoluta**: es autoadaptativo, y 100 equivale a
*"la composición media de gas de las últimas 24 h"* [7]. Poner un umbral
absoluto de 150 sobre un índice que se recalibra solo tiene dos consecuencias
malas: un incendio de arranque lento se absorbe en la propia línea base, y tras
un día de humo la base queda desplazada hacia arriba.

**D4 — No se usa ninguna RELACIÓN entre variables, solo niveles.** Es la
carencia de fondo. La combustión no produce humo *o* CO *o* calor: los produce
**juntos y en proporciones características**. Esa proporción es información, y
hoy la tiramos.

**D5 — No hay persistencia.** Una sola muestra de 5 s puede disparar. Tres
muestras consecutivas cuestan 15 s de latencia y eliminan el ruido impulsivo.

---

## 3. Revisión de los umbrales actuales, uno a uno

| Variable | v2.10 | Veredicto | Fundamento |
|---|---|---|---|
| **Temperatura** | ≥ 58 °C | ✅ **Conservar** | EN 54-5 sitúa la respuesta estática de un detector térmico clase A1 entre **54 y 65 °C** [1]. 58 es el centro de esa banda. Bien elegido. |
| **Rate-of-rise** | 8 °C/min | ⚠️ **Conservar el valor, corregir la justificación** | El comentario del código lo atribuye a *"valor típico de detectores rate-of-rise EN 54-5"*. **No he podido verificar esa afirmación.** Lo que EN 54-5 sí define son envolventes de tiempo de respuesta a velocidades de calentamiento normalizadas (clase A1: responder entre 1 min 0 s y 4 min 20 s) [1], no un umbral de alarma en °C/min. Es una elección de ingeniería razonable; no hay que venderla como norma. |
| **CO** | ≥ 10 ppm | ✅ **Conservar como indicador fuerte** | El AQI de la EPA sitúa el inicio de *"insalubre para grupos sensibles"* en **9.5 ppm** (8 h) [4]. Y el límite ocupacional del NWCG para bomberos forestales **en la línea de fuego** es de **16 ppm**, que solo supera ~5 % de ellos [6]; en quemas prescritas se han medido medias geométricas del orden de **7 ppm** [6b]. Es decir: 10 ppm significa *"el humo ya está aquí"*, no *"hay un fuego a lo lejos"*. Correcto como indicador fuerte, insuficiente como detección temprana. |
| **PM2.5** | ≥ 35 µg/m³ | ⚠️ **Conservar, pero condicionado** | Coincide con el escalón *"USG"* del AQI de la EPA, **35.5 µg/m³** tras la revisión de mayo de 2024 [4]. Pero dos sesgos lo inflan: los sensores ópticos de bajo coste **sobreestiman el humo de incendio** y necesitan corrección específica [5], y por encima de ~40–50 % de humedad relativa el crecimiento higroscópico infla la lectura; con niebla, el PM10 de un sensor de esta gama llegó a leer **46 % de más** [8]. |
| **PM10** | ≥ 150 µg/m³ | ❌ **Degradar: que deje de ser criterio autónomo** | El escalón USG del AQI es 155 µg/m³ [4], así que el número no es el problema: el problema es *qué significa*. Es el canal donde la calima y el polen entran a placer. Su valor real está en el **cociente** con el PM2.5 (§4.2). |
| **Índice VOC** | ≥ 150 | ❌ **Cambiar de nivel a incremento** | Índice autoadaptativo con 100 = media de 24 h [7]. Ver D3. |
| **Resistencia de gas** | desactivado | ✅ **Correcto** | MOX sin calibrar y con deriva. Como *variación relativa* sí puede aportar (§4.1). |
| **Histéresis / cooldown** | 10 % / 60 s | ✅ **Conservar** | |
| **Persistencia** | — | ❌ **Falta** | Ver D5. |

---

## 4. Algoritmo propuesto

Cuatro capas. Las capas 0 y 2 son lo nuevo; la 3 generaliza lo que ya hay.

### 4.0 Capa 0 — Acondicionamiento

**Validez por canal.** Ya existe desde v2.10 (`sen6x_data.unknown`, `*_valid`,
`valid_mask` del FPort 4). Un canal sin medida real no entra en ningún cálculo.

**Líneas base adaptativas.** Para cada variable *x*, mantener una base lenta
`b_x` = **mediana móvil** sobre una ventana larga (propuesta: 6 h, decimando a
1 muestra/min → 360 muestras). Mediana y no media: así una columna de humo no
arrastra la propia referencia contra la que se la compara.

Se trabaja con **incrementos**, no con niveles:

```
Δx = max(0, x − b_x)
```

Esto no es un capricho: los discriminantes publicados (§4.2) están definidos
sobre *enhancement ratios*, incrementos sobre fondo, no sobre valores brutos
[10].

**Guarda de humedad sobre el PM.** Si HR > 75 %, penalizar la evidencia de PM
(propuesta: factor `G_rh = 0.5`); si HR > 90 % o hay niebla, `G_rh = 0.25`.
Justificación en [8].

### 4.1 Capa 1 — Evidencia por variable (puntuación 0…1)

Cada variable produce un escalar normalizado que combina **nivel** y **ritmo**,
y se queda con el mayor de los dos:

```
s_x = clamp( max( Δx / Θ_x ,  (dx/dt) / Ρ_x ), 0, 1 )
```

El término de ritmo es el que da detección temprana: un fuego que se acerca
mueve las variables **deprisa** mucho antes de moverlas **mucho**.

| Canal | Θ (incremento) | Ρ (ritmo) |
|---|---|---|
| PM2.5 | 25 µg/m³ | 10 µg/m³ por minuto |
| CO | 4 ppm | 1 ppm por minuto |
| Calor | 8 °C sobre base | 4 °C/min (pre-alarma) · 8 °C/min (alarma) |
| VOC | 40 puntos de índice sobre base de 10 min | 15 puntos/min |
| Gas MOX | caída del 30 % respecto a la base de 1 h | — |

### 4.2 Capa 2 — Discriminantes (lo que hoy falta)

Aquí es donde entran las **relaciones entre variables**, que es lo que pediste.

#### D-A · Fracción fina: humo frente a polvo

```
f = PM2.5 / PM10
```

Es el discriminante más valioso para Gandía, y lo podemos calcular ya porque
medimos las dos fracciones. El humo de incendio forestal está dominado por
partícula fina; el polvo mineral (calima sahariana), por gruesa [9][11].

A partir de las medianas de cociente grueso/fino publicadas para un episodio de
humo en zona urbana — 0.47 con humo y 1.37 sin humo [11] — y con
`f = 1 / (1 + grueso/fino)`:

| Situación | grueso/fino | **f = PM2.5/PM10** |
|---|---|---|
| Con humo de incendio | 0.47 | **0.68** |
| Sin humo | 1.37 | **0.42** |

**Regla:** la evidencia de PM solo cuenta como humo si `f ≥ 0.55`.

```
G_fine = 1        si f >= 0.55
G_fine = 0.25     si f <  0.55      (episodio dominado por gruesa: calima/polen)
```

#### D-B · Cociente PM2.5/CO: el trazador de humo de incendio

```
R = ΔPM2.5 / ΔCO        [µg·m⁻³ · ppm⁻¹]
```

Jaffe, Schnieder e Inouye [10] proponen exactamente este cociente como
indicador de humo de incendio y publican los valores:

| Origen del aerosol | R (µg·m⁻³·ppm⁻¹) |
|---|---|
| Humo de incendio, medido en superficie | **103 – 128** (mejor ajuste 140) |
| Penacho fresco en altura | 201 – 339 |
| Fondo urbano sin humo | 21 – 66 (media 37) |

Su propio criterio de segregación es **R ≥ 30**, con el que ~85 % de los días
marcados tienen el humo aportando más de la mitad del PM2.5 total [10].

```
G_ratio = 1      si 30 <= R <= 400
G_ratio = 0.3    si R < 30          (CO sin PM proporcional: escape, el propio sensor)
G_ratio = 0.5    si R > 400         (PM sin CO: polvo)
```

**Limitación honesta:** R solo es fiable si ΔCO sale del ruido. El ZE15-CO
resuelve 0.1 ppm, así que la regla práctica es **no evaluar R con ΔCO < 0.5
ppm**; por debajo, `G_ratio = 1` (neutro, no penaliza).

#### D-C · Coincidencia temporal: el producto de Gottuk

Gottuk, Peatross, Roby y Beyler [12] demostraron que combinar humo con CO
reduce las falsas alarmas **y a la vez** acorta el tiempo de detección, y su
criterio patentado es literalmente *"el producto de la oscurecimiento por humo
y la variación de la concentración de CO"*.

La forma de **producto** es la clave y merece entenderse: colapsa a cero si
falta cualquiera de los dos. Es matemáticamente lo contrario de un OR de
umbrales, y es lo que mata las molestias de fuente única.

```
P = s_pm25 · s_co
```

### 4.3 Capa 3 — Fusión, niveles y persistencia

```
S = 1.0·s_pm25·G_fine·G_rh
  + 0.8·s_co
  + 1.0·s_heat
  + 1.5·P·G_ratio            <- el término de coincidencia pesa más que cualquiera suelto
```

Familias, en el espíritu de EN 54-31 [3]: **HUMO** (PM), **GAS** (CO),
**CALOR** (T fija o ritmo).

| Nivel | Condición | Acción |
|---|---|---|
| **AVISO** (medio) | `S ≥ 1.0` durante **≥ 3 ciclos** consecutivos, o cualquier familia con `s ≥ 1.0` | FPort 4, respeta cooldown |
| **ALARMA · FUEGO** | **≥ 2 familias** con `s ≥ 0.7`, discriminantes aplicables superados, durante **≥ 3 ciclos** | FPort 4 inmediato, salta cooldown |
| **ANULACIÓN** | `T ≥ 58 °C` (banda A1 de EN 54-5 [1]) | ALARMA sin corroboración |

La anulación térmica es deliberada: si el nodo mismo está a 58 °C, exigirle
corroboración es absurdo.

**Persistencia N = 3** (15 s a 5 s de cadencia). Es la mejora más barata del
documento.

### 4.4 Por qué esto responde a los casos que reportó la plataforma

| Caso observado | v2.10 | Con el algoritmo propuesto |
|---|---|---|
| Alertas de VOC con índice 150 | dispara (umbral absoluto) | el índice se evalúa por incremento sobre su propia base: un 150 estable no dispara |
| Alertas de PM2.5 con 35–42 | dispara | necesita `f ≥ 0.55`, `G_rh`, y coincidencia con otra familia para pasar de AVISO |
| PM2.5 = 433 marca solo PM10 | artefacto del flanco | `active_mask` ya lo resuelve (v2.9); además el nivel se publica explícito |
| Calima con PM10 > 150 | **enciende HUMO** | `f < 0.55` → `G_fine = 0.25` → no alcanza AVISO por sí sola |

---

## 5. Lo que este nodo no puede hacer

Conviene dejarlo escrito para que nadie lo prometa:

1. **Sin CO₂ no hay cociente CO/CO₂**, el discriminante clásico entre llama y
   combustión latente. Requeriría un SEN66 (NDIR) o un SCD4x.
2. **EN 54 es norma de detección en edificios.** EN 54-26 [2] es explícito en
   que los detectores de CO se ensayan **solo frente a fuegos latentes**. Un
   nodo a la intemperie tiene dilución por viento, radiación solar y
   convección libre, condiciones que esas normas no contemplan. Usamos sus
   *principios* (multicriterio, banda térmica A1); **no podemos declarar
   conformidad** con ellas.
3. **Los sensores de PM de bajo coste necesitan corrección específica** frente
   a humo de incendio [5]. Sin una campaña de correlación contra un equipo de
   referencia, las cifras absolutas llevan un sesgo conocido y no cuantificado
   en nuestro montaje.

---

## 6. Plan de validación propuesto

1. **Línea base** — dejar un nodo dos semanas y registrar la distribución de
   PM2.5, PM10, `f`, CO y VOC. Sin esto, cualquier umbral adaptativo es un
   número inventado.
2. **Episodio de calima** — comprobar que `f` cae por debajo de 0.55 y que el
   algoritmo **no** genera alarma. Es el ensayo negativo más importante aquí.
3. **Humo controlado** — quema de biomasa vegetal a distancia conocida, con
   registro de todas las variables a 5 s. Extraer `R` y `f` reales de nuestro
   montaje y contrastarlos con [10] y [11].
4. **Ensayo térmico** — aire caliente controlado para verificar la banda de
   58 °C y el rate-of-rise.
5. **Fuente única** — motor de gasolina cerca (CO sin PM fino) y polvo agrícola
   (PM sin CO): ninguno debe pasar de AVISO.

---

## 7. Referencias

**Verificadas contra la fuente en esta revisión:**

- **[1]** EN 54-5:2017+A1:2018, *Fire detection and fire alarm systems — Part 5:
  Heat detectors — Point heat detectors*. Clase A1: temperatura de aplicación
  típica 25 °C, máxima 50 °C, respuesta estática **mínima 54 °C / máxima
  65 °C**; envolvente de respuesta 1 min 0 s – 4 min 20 s.
  <https://standards.iteh.ai/catalog/standards/cen/a79c17c6-294a-4732-b353-66981950e7bb/en-54-5-2017a1-2018>
- **[2]** EN 54-26:2015, *Part 26: Carbon monoxide detectors — Point detectors*.
  Los detectores de CO se ensayan **solo frente a fuegos latentes**.
  <https://standards.iteh.ai/catalog/standards/cen/8b75b891-26b2-4885-9a50-d3f1a5aab21f/en-54-26-2015>
- **[3]** EN 54-31:2014+A1:2016, *Part 31: Multi-sensor fire detectors —
  combination of smoke, CO and optionally heat*. Exige evaluación **continua**
  de al menos humo y CO. Complementa EN 54-30:2015 (CO + calor).
  <https://standards.iteh.ai/catalog/standards/cen/5c5e2dbf-b647-4da1-99b4-5d7d1770d6e6/en-54-31-2014a1-2016> ·
  <https://standards.globalspec.com/std/9911180/EN%2054-30>
- **[4]** US EPA, *Air Quality Index* — tabla de breakpoints, actualización de
  **mayo de 2024**. PM2.5 24 h: bueno 0–9.0, moderado 9.1–35.4, **USG
  35.5–55.4**. PM10 24 h: **USG 155–254**. CO 8 h: **USG 9.5–12.4 ppm**.
  <https://aqihub.info/indices/us> ·
  <https://www.federalregister.gov/documents/2024/03/06/2024-02637/reconsideration-of-the-national-ambient-air-quality-standards-for-particulate-matter>
- **[5]** Holder, A. L., Mebust, A. K., Maghran, L. A., McGown, M. R.,
  Stewart, K. E., Vallano, D. M., Elleman, R. A. y Baker, K. R., *Field
  Evaluation of Low-Cost Particulate Matter Sensors for Measuring Wildfire
  Smoke*, **Sensors** 20(17):4796, 2020.
  Los sensores de bajo coste **sobreestiman** el PM2.5 de humo; con ecuaciones
  de corrección específicas el error absoluto medio baja de 10 µg/m³.
  <https://doi.org/10.3390/s20174796>
- **[6]** Semmens, E. O., Leary, C. S., West, M. R., Noonan, C. W., Navarro,
  K. M. y Domitrovich, J. W., *Carbon monoxide exposures in wildland
  firefighters in the United States and targets for exposure reduction*,
  **J. Expo. Sci. Environ. Epidemiol.** 31(5):923–929, 2021. Límite ocupacional
  del NWCG en línea de fuego **16 ppm**, superado por ~5 % de los efectivos.
  *(Datos bibliográficos verificados en PubMed; la cifra procede del resumen,
  no del texto completo — la revista está tras muro de pago.)*
  <https://pubmed.ncbi.nlm.nih.gov/34285366/>
- **[6b]** *Characterization of occupational smoke exposure among wildland
  firefighters in the midwestern United States*, **Environmental Research**,
  2020. Media geométrica de CO en quemas prescritas **7.02 ± 0.69 ppm**.
  *(Cifra tomada de un resumen de búsqueda; no he leído el texto completo ni
  verificado la autoría.)*
  <https://www.sciencedirect.com/science/article/abs/pii/S0013935120314389>
- **[7]** Sensirion, *What is Sensirion's VOC Index?* y *What is Sensirion's NOx
  Index?* Índice 1–500; **VOC 100 = media de las últimas 24 h**; NOx: la
  condición media se mapea a 1. Normalización ganancia-offset adaptativa.
  <https://sensirion.com/media/documents/02232963/6294E043/Info_Note_VOC_Index.pdf> ·
  <https://sensirion.com/media/documents/9F289B95/6294DFFC/Info_Note_NOx_Index.pdf>
- **[8]** Jayaratne, R., Liu, X., Thai, P., Dunbabin, M. y Morawska, L., *The
  influence of humidity on the performance of a low-cost air particle mass
  sensor and the effect of atmospheric fog*, **Atmos. Meas. Tech.**
  11(8):4883–4890, 2018, doi:10.5194/amt-11-4883-2018. Con niebla, el PM10 del sensor leyó
  **46 % por encima** del monitor de referencia con secador.
  <https://amt.copernicus.org/articles/11/4883/2018/>
- **[9]** *A method for estimating the fraction of mineral dust in particulate
  matter using PM2.5-to-PM10 ratios* — aerosoles de modo fino frente a modo
  grueso. *(Título y URL confirmados; no he leído el texto completo.)* La
  afirmación que apoya —que el polvo mineral está dominado por fracción gruesa
  y el humo por fracción fina— es física de aerosoles bien establecida.
  <https://www.sciencedirect.com/science/article/abs/pii/S1674200115001935>
- **[10]** Jaffe, D. A., Schnieder, B. e Inouye, D., *Technical note: Use of
  PM2.5 to CO ratio as an indicator of wildfire smoke in urban areas*,
  **Atmos. Chem. Phys.** 22:12695–12704, 2022. NER en superficie con humo
  **103–128 µg·m⁻³·ppm⁻¹** (mejor ajuste 140); sin humo 21–66 (media 37);
  umbral de segregación **≥ 30**.
  <https://doi.org/10.5194/acp-22-12695-2022>
- **[12]** Gottuk, D., Peatross, M., Roby, R. y Beyler, C., *Advanced fire
  detection using multi-signature alarm algorithms*. Versión de congreso: Fire
  Suppression and Detection Research Application Symposium, 1999. Versión de
  revista: **Fire Safety Journal** 37(4):381–394, junio de 2002. Criterio
  patentado = **producto de la oscurecimiento por humo y la variación de CO**;
  reduce falsas alarmas y acorta el tiempo de detección simultáneamente.
  <https://www.nist.gov/publications/advanced-fire-detection-using-multi-signature-alarm-algorithms> ·
  <https://doi.org/10.1016/S0379-7112(01)00057-1>

**NO verificada contra el texto completo — comprobar antes de citarla fuera:**

- **[11]** *Composition of particulate matter during a wildfire smoke episode
  in an urban area*, **Aerosol Science and Technology**, 2021,
  doi:10.1080/02786826.2021.1895429. De aquí salen las medianas de cociente
  grueso/fino **0.47 con humo** y **1.37 sin humo**, que son la base de la
  regla `f ≥ 0.55` del §4.2. **Las tomé de un resumen de búsqueda; el artículo
  está tras muro de pago y no he podido leer el texto original.** La regla
  merece comprobarse contra nuestros propios datos de campo (§6, ensayo 3)
  antes de darla por buena.
  <https://www.tandfonline.com/doi/full/10.1080/02786826.2021.1895429>

---

## 8. Estado de implementación

Nada de este documento está implementado. El firmware v2.10 sigue ejecutando el
algoritmo de §2. Este texto es la propuesta a revisar antes de tocar código.
