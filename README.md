# Pico2W flasher – zdalny flasher i zbieracz logów dla węzła Meshtastic (ESP32-S3)

**Aktualna wersja: 0.44**

- **Pomysłodawca projektu:** Rafał Cichosz (rpc)
- **Kodowanie:** Claude Opus 5.5

Raspberry Pi Pico 2 W podłączone przez USB do węzła Meshtastic na ESP32-S3 pozwala zdalnie, przez Wi-Fi lub LoRa, wgrać firmware i konfigurację, zbierać logi i pilnować węzła – bez zdejmowania go z masztu albo komina.

**Obsługiwane węzły:** ESP32-S3 z wbudowanym USB (303A:1001), np. Heltec V3/V4, XIAO ESP32-S3 (+ E22P). Inne układy ESP32 i płytki z mostkiem USB-UART nie są jeszcze obsługiwane.

## Funkcje

- Wgrywanie firmware węzła skryptem przez sieć: pełna instalacja (factory + OTA + LittleFS), aktualizacja bez utraty ustawień, samo OTA, sama konfiguracja (z kopii YAML lub `.cfg`).
- Firmware zawsze trafia najpierw do pamięci Pico i jest sprawdzany, dopiero potem do węzła. Dziennik etapów dokańcza wgrywanie po zaniku zasilania lub restarcie Pico (do 3 prób na etap).
- Sieć: Wi-Fi AP, Wi-Fi STA (z zapasowym AP), Ethernet W5500-mini albo HAT W5100S (**nieprzetestowany**) albo bez sieci (tylko LoRa).
- Logi węzła: 96 KB w RAM i historia we flashu Pico **skompresowana (ok. 5×)**, zajmująca wolne miejsce (najstarsze kawałki usuwane, także przed przyjęciem pakietu firmware); opcjonalny filtr linii, zapis do flasha można wyłączyć. Jeden plik do pobrania ze strony lub skryptem. Osobno 2 ostatnie starty węzła i zdarzenia Pico (chronione, także sprzed restartów). Pico samo włącza na węźle opcję Debug log, więc logi płyną także po komendach LoRa.
- Kontrola zasobów Pico: wolny RAM, obciążenie rdzeni, wolny flash, wykrywanie wycieku pamięci; ostrzeżenia w zdarzeniach i (opcjonalnie) przez LoRa, stan komendą `ZASOBY`.
- Sterowanie komendami LoRa (lista: `HELP`) wyłącznie na prywatnym kanale z kluczem 256-bit, odpowiedzi z ustawianym hop limit.
- Pobieranie aktualizacji z własnego serwera (`mtnod.txt` podpisany HMAC, SHA-256 każdego pakietu, zaszyfrowana konfiguracja).
- Zdalna aktualizacja programu samego Pico z zachowaniem ustawień.
- Strażnik węzła (`BRAK_ODPOWIEDZI`): sprawdzenie przez API, restart hosta USB, reset RTS, impuls na pinie resetu (domyślnie **GP22, pin 29**); wynik zgłaszany przez LoRa.
- Raporty LoRa po włączeniu zasilania, wgraniu firmware i odzyskaniu węzła; dziennik zdarzeń z przyczyną każdego restartu Pico.
- Tryb POWMAN: głęboki sen Pico (ok. 0,3–1 mA) z pobudką timerem albo pinem GP14.
- Strona WWW z logowaniem (stronę można wyłączyć komendą `STRONA:WYL`, skrypt działa dalej); te same ustawienia ze strony, skryptu i LoRa.
- Skrypt `wyslij_firmware.py` (Linux, Windows) do pobrania ze strony Pico – zawsze w wersji zgodnej z programem.

Pełny opis: [docs/Pico2W_flasher_instrukcja_0.44.pdf](docs/Pico2W_flasher_instrukcja_0.44.pdf)

## Pliki

| Plik | Opis |
|---|---|
| `pico2w_flasher_meshtastic/pico2w_flasher_meshtastic.ino` | program Pico 2 W (Arduino) |
| `pico2w_flasher_meshtastic/skrypt_py.h` | skrypt `wyslij_firmware.py` wkompilowany w program (do pobrania ze strony) |
| `wyslij_firmware.py` | skrypt na komputer: pakiety, wysyłka, flashowanie, logi, ustawienia |
| `docs/` | instrukcja PDF i schematy |
| `CHANGELOG.md` | dziennik zmian |

Gotowe pliki (`.uf2` do pierwszego wgrania, `.bin` do aktualizacji Pico) są w zakładce [Releases](https://github.com/rpce/pico2w-meshtastic-flasher/releases).

Program Pico i skrypt mają zawsze ten sam numer wersji – używaj ich w parze.

## Szybki start

1. Przytrzymaj BOOTSEL, podłącz Pico do komputera i skopiuj plik `.uf2` na dysk RP2350.
2. Połącz się z siecią Wi-Fi **FlasherMeshtasticPico2W** (hasło `12345678`) i wejdź na `http://192.168.4.1`. Zaloguj się hasłem `MojeBezpieczneHaslo123` – Pico od razu wymusi nowe hasła strony i AP (min. 12 znaków).
3. Podłącz węzeł ESP32-S3 kablem USB do Pico.
4. Na węźle ustaw kanał komend jako **prywatny z kluczem 256-bit**.
5. Pobierz skrypt ze strony Pico i uruchom `python3 wyslij_firmware.py --help` (Windows: `py wyslij_firmware.py --help`). Skrypt sam tworzy środowisko `~/.pico_meshtastic_env` z biblioteką meshtastic.

## Kompilacja

- Arduino IDE, pakiet płytek Raspberry Pi Pico/RP2040/RP2350 (earlephilhower) **6.3.2** – 6.4.0 nie linkuje projektu.
- Płytka: Raspberry Pi Pico 2 W; USB Stack: **Adafruit TinyUSB Host (native)**; Flash Size: **4MB (Sketch: 1MB, FS: 3MB)**.
- Łatki biblioteki TinyUSB (`tusb_config_rp2040.h`, `hcd_rp2040.c`, `rp2040_usb.c`) – opis w instrukcji.
- `skrypt_py.h` musi leżeć w folderze szkicu obok `.ino` i mieć tę samą wersję, inaczej kompilacja zatrzyma się z komunikatem.

## Sprzęt

- Zasilanie 5 V (5,2 V przy diodzie Schottky'ego), min. 2,4 A, do VSYS (pin 39) i GND (pin 38); VSYS z VBUS (pin 40) przez diodę, mostek albo wyłącznik TPS2552D/TPS2553.
- Wyjście resetu węzła: **GP22 (pin 29)** – steruje modułem z tranzystorem/MOSFET albo wyłącznikiem **TPS2552D** / **TPS2553DBVR** (schematy w `docs/`).
- Opcjonalnie pobudka z POWMAN pinem **GP14** z wolnego GPIO węzła (Remote Hardware).
- Ethernet: W5500-mini (GP8–GP13) albo HAT W5100S (GP16–GP21) – opcje nieprzetestowane, mogą nie działać.

## English summary

A Raspberry Pi Pico 2 W acts as a USB host for an ESP32-S3 Meshtastic node mounted in a hard-to-reach place. You can flash Meshtastic firmware and configuration remotely over Wi-Fi, collect node logs (including boot logs), control the Pico with LoRa text commands on a private 256-bit channel, pull signed updates from your own web server and put the Pico into POWMAN deep sleep for solar nodes. Requests are signed (HMAC-SHA256), the node configuration on the server is encrypted. The user interface and documentation are in Polish. Precompiled files are in Releases.

## Jak zabezpieczony jest program

### Podpisy, szyfrowanie i nagłówki
- **Podpisane żądania.** Strona i skrypt podpisują żądania kluczem z hasła i soli (HMAC-SHA256), razem z nagłówkami (`X-Offset`, `X-Chunk-CRC`, `X-Chunk-SHA`, `X-Total-Size`, `X-Total-CRC`, `X-Target`, `X-Time`) i rosnącym licznikiem. Powtórzone (podsłuchane) żądanie jest odrzucane. Odrzucenie z powodu licznika strona ponawia raz sama, bez pytania o hasło.
- **Sól hasła jest jawna** (nagłówek strony, LoRa `SOL`, `--pokaz-ustawienia`) i zmienia się przy każdej zmianie hasła. Przeglądarka i skrypt zapamiętują sól dla danego Pico i ostrzegają, gdy się zmieni (podmienione Pico albo zmiana hasła).
- **Identyfikator Pico** (12 znaków hex) jest liczony z numeru seryjnego układu, więc nie da się go zmienić ustawieniami. Skrypt przypisuje sól do identyfikatora, a nie do adresu IP.
- **Konfiguracja węzła na serwerze jest zaszyfrowana.** Klucz powstaje z hasła strony i piasku, a znacznik HMAC wykrywa każdą zmianę. Pico odszyfrowuje konfigurację tylko na czas wysyłania do węzła i potem ją usuwa. Bez hasła i piasku pliku z serwera nie da się odczytać. Pakiety `.mtpk` robione lokalnie nie są szyfrowane – nie publikuj ich.
- **SHA-256 pakietów w podpisanym `mtnod.txt`.** Pico liczy SHA-256 w trakcie pobierania i odrzuca pakiet, który się nie zgadza. `mtnod.txt` większy niż 4 KB jest odrzucany.
- **Nagłówki strony:** `Content-Security-Policy`, `X-Frame-Options: DENY`, `X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer`, `Cache-Control: no-store`.
- **`STRONA:WYL`** wyłącza stronę w przeglądarce (odpowiedź 403), a skrypt działa dalej. Mniej rzeczy do zaatakowania, gdy strona nie jest potrzebna.
- **Odpowiedzi LoRa z ustawianym hop limit** (domyślnie 3) – trafiają tylko na prywatny kanał komend.
- **Ethernet (W5500-mini, HAT W5100S) jest nieprzetestowany.** Te same zabezpieczenia co Wi-Fi STA (`IP_DOZWOLONE`), ale działanie nie zostało sprawdzone na sprzęcie.

### Dostęp do strony i skryptu
- **Wymuszona zmiana haseł.** Dopóki hasło strony (token) albo hasło sieci AP jest domyślne lub krótsze niż 12 znaków, Pico pozwala tylko na zmianę haseł. Flashowanie, pobieranie i zmiany ustawień są zablokowane.
- **Sesje przeglądarki.** Ciasteczko `HttpOnly` i `SameSite=Strict` z losowym 128-bitowym identyfikatorem ze sprzętowego generatora Pico. Pico sprawdza przy każdym żądaniu zarówno identyfikator, jak i adres IP, z którego się zalogowano, więc przechwycone ciasteczko nie działa z innego adresu. Sesja wygasa po 30 min bezczynności, jest też przycisk Wyloguj. Zmiana hasła unieważnia wszystkie sesje.
- **Skrypt loguje się jak przeglądarka.** Hasło podajesz raz i nie jest ono nigdzie zapisywane. Skrypt pamięta tylko sesję, osobno dla każdego adresu Pico, w pliku dostępnym tylko dla właściciela. Sesja wygasa po 30 min bez użycia. Skrypt pobrany ze strony nie zawiera ani hasła, ani adresu Pico.
- **Porównanie hasła w stałym czasie**, więc czas odpowiedzi nie zdradza, ile znaków się zgadza.
- **Blokada zgadywania hasła.** Po 5 błędnych próbach z jednego adresu następuje blokada na 60 s. Każda kolejna blokada trwa dwa razy dłużej, maksymalnie 15 min. Blokada trafia do zdarzeń i jest zgłaszana przez LoRa. Trwające blokady nie są wypierane przez nowe adresy. Jeśli tablica blokad (32 adresy) zapełni się, każdy kolejny nieznany adres jest traktowany jak zablokowany.
- **Lista dozwolonych adresów (`IP_DOZWOLONE`)** w sieci domowej (Wi-Fi STA i Ethernet), np. `192.168.1.*`. Adres spoza listy dostaje odmowę bez wyświetlenia strony, a odmowa trafia do zdarzeń.
- **Sieć AP Pico** jest szyfrowana WPA2 i służy jako dostęp awaryjny przy urządzeniu. Filtr IP jej nie dotyczy.

### Integralność firmware i plików
- **Sumy kontrolne CRC32** dla każdego fragmentu i całego pakietu przy wysyłaniu. Przed wgraniem Pico jeszcze raz sprawdza pakiet zapisany w swojej pamięci. Uszkodzony pakiet zostaje odrzucony, a węzeł pozostaje nietknięty.
- **Ochrona przed cofnięciem wersji (anti-rollback).** Węzeł i Pico przyjmują tylko tę samą albo nowszą wersję. Starszą da się wgrać wyłącznie po świadomym włączeniu przełącznika downgrade (strona lub `DOWNGRADE:TAK` przez LoRa).
  - Przełącznik działa jednorazowo, przez 24 h, i zostaje zużyty przed rozpoczęciem wgrywania.
  - Wymaga czasu z NTP. Bez NTP, np. po restarcie lub zaniku zasilania, przełącznik jest traktowany jako wyłączony, czyli zabezpieczenie domyka się, a nie otwiera.
  - Gdy węzeł nie poda swojej wersji (np. zawieszony), Pico porównuje z ostatnią znaną wersją zapisaną w ustawieniach.
- **Odbiór pakietu jest przypisany do adresu**, który go rozpoczął. Każdy fragment ma własną sumę CRC i jest sprawdzany z hasłem lub sesją. Przerwany odbiór wygasa po 60 s bez nowego fragmentu, a niepełny plik zostaje usunięty.
- **Pobieranie z internetu** (`mtnod.txt`):
  - plik jest podpisany HMAC-SHA256 kluczem złożonym z hasła strony i jednorazowego „piasku”;
  - piasek to 6 znaków podawanych w komendzie LoRa, nie leży na serwerze;
  - znacznik czasu `czas=` blokuje ponowne podsunięcie starszego, wcześniej ważnego pliku;
  - plik bez podpisu albo ze złym podpisem jest odrzucany;
  - zestaw z tym samym lub starszym znacznikiem czasu niż ostatnio przyjęty jest odrzucany.
- **Firmware węzła wgrywane tylko przez USB** z pakietu sprawdzonego na Pico. LoRa nigdy nie przesyła firmware, może jedynie zlecić pobranie podpisanego pakietu.
- **Aktualizacja Pico:** przed wgraniem sprawdzana jest wersja zapisana w pliku `.bin` i suma kontrolna.

### Komendy LoRa
- Komendy są przyjmowane **tylko na kanale prywatnym z kluczem 256-bit** (Pico sprawdza to w konfiguracji węzła). Na innym kanale komendy są ignorowane, a raporty LoRa wyłączone.
- **`FILTR_NODE`** pozwala ograniczyć nadawców komend do listy węzłów: co najmniej 2, a nadawca jest dopisywany automatycznie.
- **Ochrona przed podrobionymi liniami logu.** Tekst wiadomości LoRa może zawierać znaki nowej linii, a w nich linie udające log węzła. Komenda jest przyjęta tylko wtedy, gdy spełnia wszystkie warunki:
  - zgadzają się identyfikator i nadawca pakietu;
  - pakiet przyszedł na kanale komend;
  - linie nie przyszły w tej samej chwili co treść innej wiadomości;
  - kanał komend jest prywatny.
- **Komendy ryzykowne wymagają potwierdzenia `:TAK`:** `USPIJ_POWMAN`, `RESTART_PICO`, `DOWNGRADE`.
- Hasła ustawiane przez LoRa nie są zapisywane w logach. **Hasła strony nie da się zmienić przez LoRa.**

### Port węzła TCP 4403 (aplikacja Meshtastic / CLI / VCOM)
- Domyślnie wyłączony. Włączany na określony czas, **maksymalnie 7 dni**, po czym sam się zamyka i zrywa połączenie. Po restarcie Pico jest zawsze wyłączony.
- Połączenia przyjmowane **tylko z adresów prywatnych** (192.168.x.x, 10.x.x.x, 172.16–31.x.x) i z listy `IP_DOZWOLONE`, jedno naraz. Każde połączenie, rozłączenie i odmowa trafia do zdarzeń.
- W trakcie sesji wgrywanie firmware jest zablokowane.

### Ślad zdarzeń (forensyka)
- Każda operacja trafia do dziennika zdarzeń z datą z NTP, w formacie `[AUDYT] adres_IP kto OPERACJA`, gdzie „kto” to `strona`, `skrypt` albo `LoRa !id_węzła`. Na przykład:
  ```
  [AUDYT] 192.168.1.50 strona LOGIN OK
  [AUDYT] 192.168.1.50 skrypt FLASH START
  [AUDYT] 192.168.1.50 skrypt OK - operacja na wezle - ...
  [AUDYT] 192.168.1.70 strona LOGIN BLAD - zle haslo (1/5)
  [AUDYT] LoRa !a312d782 KOMENDA PORT_WEZLA
  ```
- Rejestrowane są:
  - logowania (udane i nieudane) oraz wylogowania;
  - zmiany ustawień, z nazwami zmienionych pól (bez wartości haseł);
  - wysyłanie pakietu, flashowanie i wysyłanie konfiguracji, wraz z wynikiem;
  - resety i restarty, pobrania logów i skryptu, downgrade;
  - komendy LoRa z identyfikatorem nadawcy;
  - odmowy dostępu i blokady.
- Dziennik można pobrać ze strony lub skryptem.

### Ograniczenia – o czym trzeba pamiętać
- **Strona działa po HTTP, bez szyfrowania.** W sieci lokalnej ruch, w tym hasło przy logowaniu, da się podsłuchać. **Nie przekierowuj portów 80 ani 4403 na routerze.** Z zewnątrz łącz się wyłącznie przez VPN.
- **Pobieranie z internetu po HTTPS nie weryfikuje certyfikatu serwera**, bo Pico nie ma magazynu certyfikatów. O integralności plików decyduje podpis HMAC. Podsłuchujący może jednak zablokować pobieranie albo zobaczyć, co jest pobierane.
- **Port 4403 nie ma hasła**, bo nie obsługuje go aplikacja Meshtastic. Ochrona to czas włączenia, adres prywatny i lista IP.
- **Dostęp fizyczny.** Kto ma Pico w ręku (przycisk BOOTSEL), może wgrać dowolny program. Bezpieczny rozruch (secure boot) RP2350 nie jest włączony.
- Siła podpisu pobieranych plików zależy od hasła strony, dlatego używaj długiego, unikalnego hasła i nigdzie go nie wklejaj.
- **Nie publikuj plików konfiguracji węzła** (YAML / `.cfg`), bo zawierają klucze prywatne i klucze kanałów. Do repozytorium nie trafiają też firmware, pakiety `.mtpk` ani logi.

### Odrzucone
- Klucz sprzętowy YubiKey i inne metody MFA (TOTP, OTP, OAuth): wymagają domeny albo internetu i zegara, a Pico często pracuje bez internetu.

---

Licencja: MIT.
