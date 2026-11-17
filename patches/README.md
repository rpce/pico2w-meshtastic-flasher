# Poprawki TinyUSB host (arduino-pico 6.3.2)

Folder biblioteki:
- Linux:   ~/.arduino15/packages/rp2040/hardware/rp2040/6.3.2/libraries/Adafruit_TinyUSB_Arduino/
- Windows: %LOCALAPPDATA%\Arduino15\packages\rp2040\hardware\rp2040\6.3.2\libraries\Adafruit_TinyUSB_Arduino\

Aktualizacja pakietu plytek nadpisuje te pliki - po aktualizacji zrob zmiany ponownie.

Zmieniane sa 3 pliki:
- src/portable/raspberrypi/rp2040/hcd_rp2040.c   - usuniety panic hosta USB
- src/portable/raspberrypi/rp2040/rp2040_usb.c   - j.w.
- src/arduino/ports/rp2040/tusb_config_rp2040.h  - bufory CDC RX 4096 / TX 1024,
  wylaczone LINE_CONTROL i LINE_CODING przy podlaczeniu (DTR/RTS steruje
  program Pico, bo na ESP32-S3 to linie resetu i bootloadera)

Zgloszenie: hathach/tinyusb #4546, poprawka w TinyUSB: PR #4458
(arduino-pico 6.3.2 jej jeszcze nie ma).

## Sposob A (najprostszy, tez Windows)
Zrob kopie oryginalow, potem skopiuj pliki z `pliki/`:
- hcd_rp2040.c, rp2040_usb.c  -> src/portable/raspberrypi/rp2040/
- tusb_config_rp2040.h        -> src/arduino/ports/rp2040/

## Sposob B (Linux), w folderze biblioteki
    patch -p1 < tinyusb_rp2040_host_no_panic.patch
