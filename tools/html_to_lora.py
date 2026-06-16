#!/usr/bin/env python3
"""
html_to_lora.py — Genera los downlinks LoRaWAN para actualizar el HTML del
portal cautivo del T3-S3 (protocolo FPort 10: BEGIN / DATA / COMMIT).

Trocea un fichero HTML, calcula la CRC-16/CCITT (idéntica a la del firmware,
crc16_ccitt de Zephyr, semilla 0xFFFF) y emite las tramas listas para encolar
como downlinks en ChirpStack (en hex y en base64).

Uso:
    python3 html_to_lora.py pagina.html
    python3 html_to_lora.py pagina.html --chunk 40 --fport 10

Pega cada trama, EN ORDEN, como downlinks en el FPort indicado. En Class A se
envía un downlink por cada uplink del nodo (cada ~180 s), así que el nodo las
irá recibiendo una a una. El firmware valida la CRC en COMMIT antes de aplicar:
si algo falla, la página viva NO se corrompe.
"""
import argparse
import base64
import sys

PORTAL_HTML_MAX = 4096          # debe coincidir con PORTAL_HTML_MAX del firmware


def crc16_ccitt(data: bytes, seed: int = 0xFFFF) -> int:
    """Traducción exacta de crc16_ccitt() (software) de Zephyr."""
    crc = seed
    for byte in data:
        e = (crc ^ byte) & 0xFF
        f = (e ^ (e << 4)) & 0xFF
        crc = ((crc >> 8) ^ (f << 8) ^ (f << 3) ^ (f >> 4)) & 0xFFFF
    return crc


def frame_str(idx: int, total: int, payload: bytes, op: str) -> str:
    return (f"  [{idx:2d}/{total}] {op:6s}  hex={payload.hex()}  "
            f"b64={base64.b64encode(payload).decode()}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Genera downlinks de actualización "
                                             "de HTML del portal (FPort 10).")
    ap.add_argument("html", help="Fichero HTML a cargar (<= 4096 bytes).")
    ap.add_argument("--chunk", type=int, default=40,
                    help="Bytes de datos por trama DATA (def 40). A DR0/SF12 el "
                         "payload máximo es ~51 B; deja margen para los 3 B de "
                         "cabecera. Sube a DR mayor si tu ADR lo permite.")
    ap.add_argument("--fport", type=int, default=10, help="FPort (def 10).")
    args = ap.parse_args()

    with open(args.html, "rb") as fh:
        html = fh.read()

    if len(html) == 0:
        print("ERROR: el HTML está vacío.", file=sys.stderr)
        return 1
    if len(html) > PORTAL_HTML_MAX:
        print(f"ERROR: HTML de {len(html)} B > máximo {PORTAL_HTML_MAX} B.",
              file=sys.stderr)
        return 1
    if not (1 <= args.chunk <= 200):
        print("ERROR: --chunk fuera de rango (1..200).", file=sys.stderr)
        return 1

    crc = crc16_ccitt(html)

    frames = []
    # BEGIN: 0x01 | len(LE16) | crc(LE16)
    frames.append(("BEGIN", bytes([0x01]) +
                   len(html).to_bytes(2, "little") +
                   crc.to_bytes(2, "little")))
    # DATA: 0x02 | offset(LE16) | bytes...
    for off in range(0, len(html), args.chunk):
        chunk = html[off:off + args.chunk]
        frames.append(("DATA", bytes([0x02]) +
                       off.to_bytes(2, "little") + chunk))
    # COMMIT: 0x03
    frames.append(("COMMIT", bytes([0x03])))

    total = len(frames)
    print(f"HTML: {len(html)} bytes | CRC-16/CCITT: 0x{crc:04x} | "
          f"FPort: {args.fport} | tramas: {total} (1 BEGIN + "
          f"{total - 2} DATA + 1 COMMIT)")
    print(f"En Class A (~180 s/uplink) tardará ~{total * 3} min en aplicarse.\n")
    print("Encola estos downlinks EN ORDEN en ChirpStack (FPort "
          f"{args.fport}):\n")
    for i, (op, payload) in enumerate(frames, 1):
        print(frame_str(i, total, payload, op))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
