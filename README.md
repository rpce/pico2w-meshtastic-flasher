# Pico2W flasher – zdalny flasher i zbieracz logów dla węzła Meshtastic (ESP32-S3)

Pomysłodawca projektu: Rafał Cichosz (rpc)

**Aktualna wersja: 0.28**

Raspberry Pi Pico 2 W podłączone przez USB do węzła Meshtastic na ESP32-S3 (testowane na Heltec V4, XIAO ESP32-S3) pozwala zdalnie, bez zdejmowania węzła z masztu lub komina:

- wgrać firmware Meshtastic: pełna instalacja z konfiguracją, aktualizacja, samo OTA albo sama konfiguracja;
- zbierać logi węzła (w RAM i na flash Pico) i pobierać je przez stronę lub skrypt – także po komendach LoRa, dzięki dekodowaniu logów z API (opcja Debug log na węźle);
- sterować Pico komendami LoRa (HELP, WERSJE, TEMP_PICO, Wi-Fi, aktualizacja z internetu, POWMAN), tylko na prywatnym kanale z kluczem 256-bit;
- pobrać i wgrać aktualizację z własnego serwera WWW (`mtnod.txt` podpisany HMAC);
- zaktualizować samo Pico: przez stronę, skrypt albo internet;
- pilnować węzła: przy braku odpowiedzi kolejno restart hosta USB, reset węzła (RTS) i impuls na pinie resetu;
- oszczędzać energię w węzłach solarnych: tryb POWMAN, czyli głęboki sen Pico (ok. 0,3–1 mA) z pobudką timerem albo pinem GP14;
- pobrać skrypt `wyslij_firmware.py` prosto ze strony Pico – zawsze w wersji zgodnej z programem.

Szczegóły: [docs/Pico2W_flasher_instrukcja_0.28.pdf](docs/Pico2W_flasher_instrukcja_0.28.pdf)

## Pliki

| Plik | Opis |
|---|---|
| `pico2w_flasher_meshtastic/pico2w_flasher_meshtastic.ino` | program Pico 2 W (Arduino) |
| `pico2w_flasher_meshtastic/skrypt_py.h` | skrypt `wyslij_firmware.py` wkompilowany w program (do pobrania ze strony) |
| `wyslij_firmware.py` | skrypt na komputer: budowa pakietu, wysyłka, flashowanie, logi, ustawienia |
| `patches/tinyusb_rp2040_host_no_panic.patch` | poprawka TinyUSB host (RP2350) |
| `docs/` | instrukcja PDF, schematy i zdjęcia |

Gotowe pliki (`.uf2` do pierwszego wgrania, `.bin` do aktualizacji Pico) są w zakładce [Releases](https://github.com/rpce/pico2w-meshtastic-flasher/releases).

## Szybki start

1. Przytrzymaj BOOTSEL, podłącz Pico do PC i skopiuj plik `.uf2` na dysk RP2350.
2. Połącz się z siecią Wi-Fi Pico (AP `FlasherMeshtasticPico2W`) i wejdź na http://192.168.4.1. **Zmień domyślne hasło AP i token.**
3. Podłącz węzeł ESP32-S3 do USB Pico.
4. Na węźle ustaw kanał komend jako **prywatny z kluczem 256-bit** i włącz **Debug log** (`security.debug_log_api_enabled: true`; w aplikacji: Konfiguracja → Security → Debug log). Bez Debug log logi węzła znikają po pierwszej komendzie LoRa aż do restartu węzła.
5. Pobierz skrypt ze strony Pico (przycisk „Pobierz skrypt wyslij_firmware.py”) i uruchom: `python3 wyslij_firmware.py --help` (Windows: `py wyslij_firmware.py --help`). Skrypt sam tworzy środowisko `~/.pico_meshtastic_env` z biblioteką meshtastic.

Wersja programu Pico i skryptu musi być taka sama.

## Kompilacja

- Arduino IDE, rdzeń arduino-pico **6.3.2** (6.4.0 się nie linkuje), płytka Raspberry Pi Pico 2 W
- USB Stack: Adafruit TinyUSB Host
- Flash: 4 MB (Sketch: 1 MB, FS: 3 MB)
- poprawka z `patches/` (opis w instrukcji)
- `skrypt_py.h` musi leżeć w folderze szkicu obok `.ino` i mieć tę samą wersję – inaczej kompilacja zatrzyma się z komunikatem

## Sprzęt

- Zasilanie 5 V (lub 5,22 V) z zasilacza, przetwornicy albo splittera PoE, min. 2,4 A.
- Opcjonalnie elektroniczny wyłącznik zasilania węzła **TPS2552D** albo **TPS2553DBVR** (sterowany z GP15) – schematy w instrukcji.
- Opcjonalnie pobudka z POWMAN pinem **GP14** z wolnego GPIO węzła (Remote Hardware).

## Bezpieczeństwo

- Nie publikuj plików konfiguracji węzła (`.yaml`, `.cfg`) – zawierają klucze prywatne i klucze kanałów.
- Komendy LoRa tylko na prywatnym kanale z pełnym kluczem 256-bit – Pico odrzuca inne kanały.
- Zmień domyślny token i hasło AP. Nie wystawiaj strony Pico do internetu – do dostępu z zewnątrz użyj VPN.

## English summary

A Raspberry Pi Pico 2 W acts as a USB host for an ESP32-S3 Meshtastic node mounted in a hard-to-reach place. You can flash Meshtastic firmware and configuration remotely over Wi-Fi, collect node logs (also after LoRa commands, via API log records with the node's Debug log option), control the Pico with LoRa text commands on a private 256-bit channel, pull signed updates from your own web server, and put the Pico into POWMAN deep sleep for solar nodes. The user interface and documentation are in Polish. Precompiled files are in Releases.

License: MIT.
