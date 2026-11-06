## Files
| Device | Full image (`0x0`) | App only (OTA) |
|---|---|---|
| QRP Labs LightTracker Plus v1.0 | `Bruce-lighttracker-plus-${VERSION}-full.bin` | `Bruce-lighttracker-plus-${VERSION}-app.bin` |
| ATS Mini (OPI PSRAM) | `Bruce-ats-mini-${VERSION}-full.bin` | `Bruce-ats-mini-${VERSION}-app.bin` |

## Flash
```
esptool.py --chip esp32s3 --port <PORT> --baud 460800 write_flash 0x0 <full image>
```
Or use https://bruce.computer/flasher / ESP Web Tools with the `-full.bin` image at offset 0.
`-app.bin` is the application only (OTA / Bruce updater, offset `0x10000`).

## LightTracker Plus
- ESP32-S3 + SX1268 (EBYTE E22-400M30S, 433 MHz) + Quectel L80 GPS
- Display: 0.96" SSD1306 128x64 I2C OLED on the OLED header (SDA 3, SCL 4, 0x3C)
- BOOT button: short = next, double = previous, hold 0.5 s = select, hold 1.2 s = back
- LoRa Chat on the SX1262 driver (default radio), TCXO 1.8 V, E22 RF switch handled.
- GPS on UART RX 18 / TX 17 (9600). No CC1101 / Sub-GHz hardware.

## ATS Mini
- ESP32-S3 + SI4732, 170x320 ST7789 (8-bit parallel), rotary encoder
- `ats-mini` = units with Octal (OPI) PSRAM. Quad PSRAM units need the `ats-mini-qspi` build.
- Encoder: one detent = one step. Short push = select, hold 0.6 s = back, BOOT = back.
- **FM → FM Radio (RDS)**: 87.5-108 MHz, seek, RDS station name and radio text.
  Short push switches between frequency, seek and volume mode; rotate to change, hold to exit.
- Bruce's own beeps are silent on this device: the speaker is wired to the SI4732 only.
