# T3_S3_V1.3

Board's link : https://lilygo.cc/en-us/products/t3-s3-v1-3?srsltid=AfmBOopbGDyCvH4L2d9RkC5nm8mq-K2uvjero0n-g52pj3jv
Repository: https://github.com/Xinyuan-LilyGO/LilyGo-LoRa-Series/blob/master/docs/en/t3_s3_sx1262/t3_s3_sx1262_hw.md

Descripcion de placa - pinout:
<img width="2362" height="1772" alt="image" src="https://github.com/user-attachments/assets/8ecf2e7c-f2f5-4586-b55e-28315bb94d65" />

Sensors:
https://es.farnell.com/sensirion/sen65-sin-t/m-dulo-de-sensor-digital-i2c/dp/4785272?gross_price=true&CMP=KNC-GES-GEN-SHOPPING-Pmax-High_ROAS&gad_source=1&gad_campaignid=18071281895&gclid=Cj0KCQjwiJvQBhCYARIsAMjts3J1ehyh159j6Vxk34-JErC3etC25RBRXYX2PenL7mrabHKXf34k8RwaAnBPEALw_wcB 

## Documentación

**Hardware / cableado**
- 🔌 [**Pinout — sensores ↔ ESP32-S3**](docs/PINOUT_SENSORES.md) — tabla de asignación de pines, buses, alimentación y ubicación física. **Empezar aquí para cablear.**

**Integración de sensores**
- [Integración BM688](docs/BM688_INTEGRATION.md) — ambiental (T/H/P/gas), I2C0.
- [Integración ZE15-CO](docs/ZE15_CO_INTEGRATION.md) — monóxido de carbono, UART1.
- [Integración SEN65](docs/SEN65_INTEGRATION.md) — calidad de aire (PM/VOC/NOx/RH&T), I2C1.

**Mensaje LoRa / ChirpStack**
- [Flujo de datos y mensaje LoRa](docs/FLUJO_DATOS_LORA.md) — visión end-to-end sensores → LoRa → ChirpStack.
- [Envío de sensores](docs/envio_sensores.md) — conformación del mensaje, decodificación, tiempos mínimos y qué pasa con los datos entre envíos.
- [Payload + decoder](docs/PAYLOAD_DECODER.md) — tabla del payload (29 B), decoder JS y alta en ChirpStack.

**Portal cautivo**
- [Portal cautivo WiFi](docs/PORTAL_CAUTIVO.md) — arquitectura, configuración y build del AP + dashboard web concurrente con LoRaWAN.
- [Manual de usuario del portal](docs/MANUAL_USUARIO_PORTAL.md) — cómo conectarse a la red y abrir la página (usuario final).
- [Actualizar el HTML por LoRa](docs/ACTUALIZAR_HTML_LORA.md) — cambiar la página servida vía downlink LoRaWAN (FPort 10).

**Diagnóstico**
- [Debug BM688 + LoRaWAN](docs/DEBUG_BM688_LORAWAN.md)
