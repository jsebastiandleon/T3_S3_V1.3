# Manual de usuario — Conectarse al portal del T3-S3

Guía rápida para acceder al portal web del dispositivo desde un móvil o un PC.
No necesitas conocimientos técnicos.

## 1. Conéctate a la red WiFi del dispositivo

En los ajustes WiFi de tu móvil/PC, busca y conéctate a la red:

| | |
|---|---|
| **Nombre (SSID)** | `T3S3-XXXX` (ej. `T3S3-2940`) |
| **Contraseña** | `t3s3portal` |

> `XXXX` cambia según el dispositivo (son los últimos dígitos de su dirección).

## 2. Abre el portal

En la mayoría de móviles aparecerá **automáticamente** una notificación tipo
**"Iniciar sesión en la red WiFi"** / *"Sign in to Wi-Fi network"*. Tócala y se
abrirá el portal.

Si no aparece, abre el navegador y entra a:

```
http://192.168.4.1
```

Verás la página con la imagen y las **lecturas de los sensores** (temperatura,
humedad, presión, gas y CO), que se actualizan solas cada pocos segundos.

## 3. ⚠️ Importante: "Esta red no tiene Internet"

Es **normal** que el móvil avise de que esta red **no tiene Internet**: el
dispositivo es un portal local, no da salida a Internet. No es un error.

Si usas **Android**, ten en cuenta este detalle:

> Android, al ver que la WiFi no tiene Internet, **manda el tráfico del navegador
> por los datos móviles**. Como `192.168.4.1` solo existe en esta WiFi, la página
> **no cargará** y verás *"tardó demasiado en responder"* / `ERR_CONNECTION_TIMED_OUT`.

**Solución:** desactiva temporalmente los **datos móviles** del teléfono (deja
solo la WiFi) y vuelve a abrir `http://192.168.4.1`. La página cargará al instante.

Cuando termines, puedes reactivar los datos móviles.

## Resolución de problemas

| Síntoma | Causa probable | Solución |
|---------|----------------|----------|
| "Tardó demasiado en responder" / `ERR_CONNECTION_TIMED_OUT` | Android usa datos móviles | Desactiva **datos móviles** y reintenta |
| "Conexión rechazada" / `ERR_CONNECTION_REFUSED` | El dispositivo aún arrancando | Espera ~10 s tras encenderlo y reintenta |
| No aparece la red `T3S3-XXXX` | Dispositivo apagado o sin energía | Verifica la alimentación (USB) |
| Pide contraseña y la rechaza | Contraseña mal escrita | Es `t3s3portal` (todo minúsculas) |
| La red pregunta "¿mantener conexión sin Internet?" | Comportamiento normal | Responde **Sí / Mantener** |
| La página carga pero los sensores marcan `--` | El sensor no está conectado | Es normal si no hay sensores; conéctalos |

## Notas

- Usa siempre `http://` (no `https://`).
- Si una página queda cacheada, prueba en una pestaña de **incógnito**.
- Varios dispositivos pueden conectarse a la vez.
