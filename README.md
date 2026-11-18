# Pico 2 W – zdalny flasher i zbieracz logów dla węzła Meshtastic (ESP32-S3)

Pomysłodawca projektu: Rafał Cichosz (rpc)

Raspberry Pi Pico 2 W podłączone przez USB do węzła Meshtastic na ESP32-S3 (Heltec V4, XIAO ESP32-S3 + E22P) pozwala zdalnie, bez zdejmowania węzła z masztu lub komina:

- wgrać firmware Meshtastic (pełna instalacja z konfiguracją, aktualizacja, samo OTA, sama konfiguracja),
- zbierać logi węzła (RAM i flash Pico) i pobierać je przez stronę lub skrypt,
- sterować Pico komendami LoRa (HELP, WERSJE, TEMP_PICO, Wi-Fi, aktualizacja z internetu itp.),
- pobrać i wgrać aktualizację z własnego serwera WWW (`mtnod.txt` podpisany HMAC),
- zaktualizować samo Pico (przez stronę, skrypt albo internet).

Szczegóły: **[docs/Pico2W_flasher_instrukcja.pdf](docs/Pico2W_flasher_instrukcja.pdf)**

## Pliki

| Plik | Opis |
|---|---|
| `pico2w_flasher_meshtastic/pico2w_flasher_meshtastic.ino` | program Pico 2 W (Arduino) |
| `wyslij_firmware.py` | skrypt PC: budowa pakietu, wysyłanie, logi, serwer aktualizacji |
| `patches/tinyusb_rp2040_host_no_panic.patch` | poprawka TinyUSB host (RP2350) |
| `docs/` | instrukcja PDF i zdjęcia |

Gotowe pliki (`.uf2` do pierwszego wgrania, `.bin` do aktualizacji Pico) są w zakładce **[Releases](../../releases)**.

## Szybki start

1. Przytrzymaj BOOTSEL, podłącz Pico do PC i skopiuj plik `.uf2` na dysk RP2350.
2. Połącz się z siecią Wi-Fi Pico (AP) i wejdź na `http://192.168.4.1`. **Zmień domyślne hasło/token.**
3. Podłącz węzeł ESP32-S3 do USB Pico.
4. Na PC: `python3 wyslij_firmware.py --help` (Windows: `py wyslij_firmware.py --help`).
   Skrypt sam tworzy środowisko `~/.pico_meshtastic_env` z biblioteką meshtastic.

Wersja programu Pico i skryptu musi być taka sama.

## Kompilacja

- Arduino IDE, rdzeń **arduino-pico 6.3.2**, płytka Raspberry Pi Pico 2 W
- USB Stack: **Adafruit TinyUSB Host**
- Flash: z miejscem na system plików (opis w instrukcji)
- poprawka z `patches/` (opis w instrukcji)

## Bezpieczeństwo

Nie publikuj plików konfiguracji węzła (`.yaml`, `.cfg`) – zawierają klucze prywatne i klucze kanałów.

---

## English summary

Raspberry Pi Pico 2 W acting as a USB host for an ESP32-S3 Meshtastic node mounted in a hard-to-reach place. It lets you flash Meshtastic firmware and configuration remotely over Wi-Fi, collect node logs, control the Pico with LoRa text commands, and pull signed updates from your own web server. The user interface and documentation are in Polish. Precompiled files are in Releases.

License: MIT.
