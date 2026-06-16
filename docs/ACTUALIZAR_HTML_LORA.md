# Actualizar la página del portal vía LoRa

La página HTML que sirve el portal cautivo **no está fija en el firmware**: vive
en memoria no volátil (NVS) y puede reemplazarse **por downlink LoRaWAN**, sin
reflashear el dispositivo. Esto es útil para nodos ya desplegados en campo.

La transferencia es **a prueba de fallos**: el HTML nuevo se reensambla en un
buffer aparte y solo se aplica si la longitud y la CRC cuadran. Si algo falla a
medias, la página viva **no se corrompe**.

## Resumen rápido

1. Edita tu `pagina.html` (≤ **4096 bytes**).
2. Genera las tramas:
   ```bash
   python3 tools/html_to_lora.py pagina.html
   ```
3. Encola en ChirpStack los downlinks que imprime, **en orden**, en el **FPort 10**.
4. El nodo los recibe uno por uplink (~180 s cada uno) y, al recibir el último
   (COMMIT), aplica y persiste la página nueva.

## El protocolo (FPort 10)

Cada downlink es una trama (little-endian en las longitudes):

| Trama | Bytes | Significado |
|-------|-------|-------------|
| **BEGIN** | `0x01` `len(LE16)` `crc(LE16)` | Reinicia la carga. `len` = tamaño total del HTML; `crc` = CRC-16/CCITT del HTML completo. |
| **DATA** | `0x02` `offset(LE16)` `bytes…` | Escribe `bytes` en `staging[offset]`. Admite cualquier tamaño y orden. |
| **COMMIT** | `0x03` | Si `len` y `crc` cuadran → guarda en NVS y pasa a vivo. |

- **CRC:** CRC-16/CCITT, semilla `0xFFFF` (la `crc16_ccitt` de Zephyr). El script
  la calcula por ti; coincide exactamente con la del firmware.
- **Tamaño máximo del HTML:** 4096 bytes (`PORTAL_HTML_MAX`).
- **Tamaño de trama DATA:** a DR0/SF12 el payload máximo es ~51 B, así que el
  script usa 40 B de datos por trama por defecto (`--chunk`). Con un DR mayor
  (ADR) puedes subirlo.

## El generador `tools/html_to_lora.py`

```bash
python3 tools/html_to_lora.py pagina.html [--chunk 40] [--fport 10]
```

Salida de ejemplo:

```
HTML: 36 bytes | CRC-16/CCITT: 0x20d7 | FPort: 10 | tramas: 5 (1 BEGIN + 3 DATA + 1 COMMIT)
En Class A (~180 s/uplink) tardará ~15 min en aplicarse.

Encola estos downlinks EN ORDEN en ChirpStack (FPort 10):

  [ 1/5] BEGIN   hex=012400d720  b64=ASQA1yA=
  [ 2/5] DATA    hex=0200003c68746d6c3e3c626f64793e486f6c61  b64=AgAAPGh0bWw+PGJvZHk+SG9sYQ==
  [ 3/5] DATA    hex=0210002054332d53333c2f626f64793e3c2f68  b64=AhAAIFQzLVMzPC9ib2R5PjwvaA==
  [ 4/5] DATA    hex=022000746d6c3e  b64=AiAAdG1sPg==
  [ 5/5] COMMIT  hex=03  b64=Aw==
```

Para cada trama te da el payload en **hex** y en **base64** (usa el que pida tu
interfaz de ChirpStack).

## Encolar en ChirpStack

1. Entra al **device** en ChirpStack → pestaña **Queue** (Enqueue downlink).
2. **FPort:** `10`.
3. **Payload:** pega el `b64=` (o cambia a hex si la UI lo permite) de la trama.
4. **Confirmed:** opcional. Sin confirmar es más rápido; confirmado te asegura
   que cada trama llegó.
5. Repite **en orden**: BEGIN, luego cada DATA, y por último COMMIT.

Puedes encolar todas de golpe: ChirpStack las va enviando **una por uplink** (el
nodo es Class A, solo recibe tras transmitir).

## Tiempos y consideraciones

- En Class A se envía **un downlink por uplink**. Con el envío cada 180 s, una
  página de N tramas tarda ~`N × 3` minutos en completarse.
- Para páginas grandes, sube el DR (vía ADR) y el `--chunk` para reducir el
  número de tramas.
- Si un COMMIT falla por CRC o longitud, el firmware lo registra y **descarta**
  la carga; la página anterior sigue intacta. Repite el proceso.
- El nodo loguea por serial: `OTA BEGIN`, `OTA DATA`, `OTA COMMIT OK` o el error.

## Volver al HTML por defecto

El HTML por defecto está empotrado en el firmware (`default_html` en
`src/portal/portal_html.c`). Para restaurarlo, borra la clave `portal/html` de
NVS (p. ej. `west flash --erase`) o envía por LoRa una página nueva que lo
reemplace.
