# Portal cautivo WiFi — T3-S3

Portal cautivo sobre el WiFi del ESP32-S3, **concurrente** con LoRaWAN y los
sensores. Levanta un Access Point, sirve un dashboard web con las lecturas y
permite **actualizar el HTML servido vía downlink LoRa**.

> Manuales relacionados:
> - [MANUAL_USUARIO_PORTAL.md](MANUAL_USUARIO_PORTAL.md) — cómo conectarse (usuario final).
> - [ACTUALIZAR_HTML_LORA.md](ACTUALIZAR_HTML_LORA.md) — cómo cambiar la página por LoRa.

## Arquitectura

Todas las piezas son nativas de Zephyr; nada de stacks externos:

| Capa | Pieza | Función |
|------|-------|---------|
| Enlace | WiFi SoftAP (driver `esp32`, modo AP-only) | AP `T3S3-XXYY` |
| Red | IPv4 estática `192.168.4.1/24` + `NET_DHCPV4_SERVER` | reparte IPs `.10+` |
| Captura | Responder DNS UDP:53 catch-all | resuelve TODO a `192.168.4.1` → dispara el pop-up de portal del SO |
| HTTP | Subsistema `HTTP_SERVER` (:80) | sirve el dashboard, la imagen y `/api/sensors` |
| App | módulo `portal` | orquesta el arranque y el snapshot de sensores |

El portal arranca al boot (no necesita botones) y convive con el join/uplinks de
LoRaWAN y la lectura de BM688/ZE15-CO.

## Estructura de ficheros

```
include/portal/portal.h        API pública del módulo
src/portal/portal.c            orquestación (AP, IP, DHCP, DNS, HTTP) + snapshot
src/portal/captive_dns.c       responder DNS UDP:53
src/portal/http_routes.c       recursos HTTP (/, /api/sensors, /gorila.jpg)
src/portal/portal_html.c       HTML mutable (NVS) + actualización por LoRa
sections-rom.ld                sección iterable de recursos del servicio HTTP
gorila.jpg                     imagen embebida en flash, servida en /gorila.jpg
tools/html_to_lora.py          genera los downlinks de actualización de HTML
```

## Endpoints HTTP

| Ruta | Tipo | Descripción |
|------|------|-------------|
| `GET /` | dinámico | Dashboard HTML (mutable, desde NVS o el default empotrado) |
| `GET /api/sensors` | dinámico | JSON con el último snapshot de sensores |
| `GET /gorila.jpg` | estático | Imagen embebida (`image/jpeg`) |
| cualquier otra ruta | fallback | Sirve el dashboard (captura del portal) |

Ejemplo de `/api/sensors`:

```json
{"bm688":false,"temperature":0.00,"humidity":0.00,"pressure":0,
 "gas":0,"co":false,"co_ppm":0.0,"age_ms":1234}
```

`bm688`/`co` indican si cada sensor está presente y con lectura válida.

## Datos clave del AP

| Parámetro | Valor | Dónde se cambia |
|-----------|-------|-----------------|
| SSID | `T3S3-XXYY` (XXYY = 2 últimos bytes de la MAC) | `enable_softap()` en `portal.c` |
| Seguridad | WPA2-PSK | `portal.c` |
| Contraseña | `t3s3portal` | `AP_PSK` en `portal.c` |
| IP del portal | `192.168.4.1` | `AP_IP` en `portal.c` |
| Rango DHCP | `192.168.4.10+` | `setup_ap_ip()` en `portal.c` |

## Compilar y flashear

```bash
west build -b esp32s3_devkitc/esp32s3/procpu .
west flash
west espressif monitor      # opcional, para ver el log
```

Al arrancar verás:

```
I: SoftAP activo: SSID="T3S3-2940" PSK="t3s3portal"
I: AP IP 192.168.4.1, DHCPv4 desde 192.168.4.10
I: PORTAL READY (http://192.168.4.1)
I: DNS cautivo escuchando en :53
```

## Configuración crítica (`prj.conf`)

Integrar WiFi + LoRaWAN + HTTP en un solo firmware ESP32-S3 tiene varias
trampas reales. Estas opciones **no son opcionales**; cada una resuelve un fallo
concreto:

| Opción | Por qué |
|--------|---------|
| `CONFIG_WIFI_USAGE_MODE_AP=y` (AP-only) | La coexistencia STA/AP exige una 2ª instancia del nodo wifi en el devicetree. No usamos STA. |
| `CONFIG_ESP32_WIFI_NET_ALLOC_SPIRAM=y` | Heap de WiFi/NET a PSRAM, para no asfixiar la SRAM interna que necesita LoRaWAN. |
| `CONFIG_NET_IPV6=n` | Con IPv6 activo y `host=NULL`, el HTTP server enlaza en IPv6 (`::`) y rechaza IPv4 → el móvil recibe *connection refused*. |
| `CONFIG_ZVFS_EVENTFD_MAX=4` | `http_server_init()` crea un eventfd y **aborta antes de escuchar** si falla. Con el default (1), el net sockets service se lo queda y el HTTP server no arranca. |
| `CONFIG_NET_MAX_CONTEXTS=16`, `CONFIG_NET_MAX_CONN=16` | Holgura para listener + clientes + DNS + DHCP. |
| `CONFIG_ZVFS_OPEN_MAX=24`, `CONFIG_ZVFS_POLL_MAX=16` | Tabla de descriptores/poll suficiente para todos los sockets. |
| `CONFIG_HTTP_SERVER_MAX_CLIENTS=2` | Un móvil abre pocas conexiones; menos RAM. Debe ser ≥ el `_concurrent` del `HTTP_SERVICE_DEFINE`. |

### Coexistencia LoRaWAN ↔ WiFi (CMakeLists.txt)

El soft-SE de `loramac-node` y el supplicant WPA del WiFi definen **ambos** los
símbolos globales `aes_encrypt`/`aes_decrypt` (firmas distintas) → *multiple
definition* al enlazar. Se resuelve renombrando solo los de loramac (internos a
esa lib), sin activar el crypto mbedTLS (que arrastraría EC/PK inútiles):

```cmake
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL
    target_compile_definitions loramac-node PRIVATE
    aes_encrypt=loramac_aes_encrypt
    aes_decrypt=loramac_aes_decrypt)
```

## Cadencias (main.c)

- **Sensores + portal:** cada 5 s (dashboard "vivo", independiente de la radio).
- **Envío LoRa:** cada 180 s (EU868 a DR0/SF12 exige ~150 s entre uplinks por el
  1 % de duty-cycle).
- El ZE15-CO se **deshabilita tras 3 lecturas fallidas seguidas** (evita ~4.5 s
  por ciclo de timeouts si no hay sensor).

## Limitaciones conocidas

- HTML máximo: **4096 bytes** (`PORTAL_HTML_MAX`).
- HTTP plano (sin TLS); aceptable sobre un AP aislado de provisioning.
- El portal solo sirve IPv4.
- Footprint: ~16 % de flash; heap WiFi en PSRAM.
