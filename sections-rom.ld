/*
 * Seccion iterable en ROM para los recursos del servicio HTTP del portal.
 * HTTP_RESOURCE_DEFINE coloca cada recurso en la seccion
 * "http_resource_desc_<servicio>"; HTTP_SERVICE_DEFINE referencia los
 * simbolos de frontera _..._list_start/_list_end que genera esta macro.
 * Hay que declarar una por cada servicio HTTP definido en la app.
 */
#include <zephyr/linker/iterable_sections.h>

ITERABLE_SECTION_ROM(http_resource_desc_portal_http_service, Z_LINK_ITERABLE_SUBALIGN)
