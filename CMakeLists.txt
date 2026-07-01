cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(t3s3_lora VERSION 1.0.0 LANGUAGES C)

target_include_directories(app PRIVATE include)

# Seccion iterable en ROM para los recursos del servicio HTTP del portal
# (necesaria para los simbolos _http_resource_desc_portal_http_service_list_*).
zephyr_linker_sources(SECTIONS sections-rom.ld)

target_sources(app PRIVATE
    src/main.c
    src/sensors/bm688.c
    src/sensors/ze15_co.c
    src/sensors/sen6x.c
    src/sensors/anemometer.c
    src/portal/portal.c
    src/portal/portal_html.c
    src/portal/captive_dns.c
    src/portal/http_routes.c
)

# ---------------------------------------------------------------------------
# Coexistencia LoRaWAN + WiFi: el soft-SE de loramac-node y el supplicant WPA
# interno del WiFi del ESP32 definen ambos los simbolos globales aes_encrypt y
# aes_decrypt (con firmas distintas) -> "multiple definition" al enlazar.
# Los de loramac son internos a esa libreria, asi que los renombramos SOLO en
# la libreria loramac-node (definicion + llamadas, todas en sus TUs). El
# supplicant conserva los suyos. Cero coste de tamano (a diferencia de activar
# el crypto mbedTLS, que ademas arrastraria EC/PK que no usa un AP WPA2-PSK).
#
# El target loramac-node lo crea Zephyr al procesar su arbol, DESPUES de este
# CMakeLists, por eso se difiere la llamada al final del directorio raiz.
# ---------------------------------------------------------------------------
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL
    target_compile_definitions loramac-node PRIVATE
    aes_encrypt=loramac_aes_encrypt
    aes_decrypt=loramac_aes_decrypt)
