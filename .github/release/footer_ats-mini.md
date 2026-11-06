## Files
- `Bruce-ats-mini-${VERSION}-full.bin` (flash at `0x0`)
- `Bruce-ats-mini-${VERSION}-app.bin` (OTA)

## Flash
```
esptool.py --chip esp32s3 --port <PORT> --baud 460800 write_flash 0x0 <full image>
```
Or use https://bruce.computer/flasher / ESP Web Tools with the `-full.bin` image at offset 0.
`-app.bin` is the application only (OTA / Bruce updater, offset `0x10000`).

## ATS Mini
- ESP32-S3 + SI4732, 170x320 ST7789 (8-bit parallel), rotary encoder
- `ats-mini` = units with Octal (OPI) PSRAM. Quad PSRAM units need the `ats-mini-qspi` build.
- Encoder: one detent = one step. Short push = select, hold 0.6 s = back, BOOT = back.
- **FM → FM Radio (RDS)**: 87.5-108 MHz, seek, RDS station name and radio text.
  Short push cycles FREKANS / ARAMA / SES, rotate to change, hold to exit.
- Bruce's own beeps are silent on this device: the speaker is wired to the SI4732 only.
