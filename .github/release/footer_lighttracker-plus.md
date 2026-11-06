## Files
- `Bruce-lighttracker-plus-${VERSION}-full.bin` (flash at `0x0`)
- `Bruce-lighttracker-plus-${VERSION}-app.bin` (OTA)

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
