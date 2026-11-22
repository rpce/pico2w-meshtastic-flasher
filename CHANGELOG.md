# Dziennik zmian – Pico2W flasher

Program Pico i skrypt `wyslij_firmware.py` mają zawsze ten sam numer wersji.

Pomysłodawca projektu: Rafał Cichosz (rpc) · Kodowanie: Claude Opus 5.5

## [0.44] – 2026-11-22

### Dodane
- **Kompresja logów węzła we flashu** (gzip, ok. 5×): kawałki po 32 KB tekstu, każdy jako osobny plik. *Pobierz logi z flash* na stronie daje rozpakowany .txt (rozpakowuje Pico), skrypt `--logi` pobiera .gz i rozpakowuje sam.
- **Dynamiczne miejsce na logi węzła**: zajmują całe wolne miejsce we flashu (zapas 256 KB), najstarsze kawałki znikają, gdy brakuje miejsca – także przed przyjęciem pakietu firmware (skrypt, strona, internet). Pasek stanu pokazuje, ile logu jest we flashu, na ile godzin starcza i stopień kompresji.
- **Ustawienie *Zapis logów węzła do flasha*** (domyślnie tak; skrypt `LOGI_FLASH=TAK/NIE`) – nie = logi węzła tylko w RAM.
- **Ustawienie *Filtr logów*** (skrypt `FILTR_LOGOW`): fragmenty oddzielone `;`, linie z nimi nie trafiają do flasha. Domyślnie puste = pełne logi.

### Zmienione
- Log węzła w RAM: 96 KB (było 48 KB); zapis do flasha po 32 KB albo co godzinę, zawsze do końca pełnej linii.
- Zdarzenia Pico zapisywane we flashu (2 × 32 KB, chronione – nigdy nie usuwane na pakiet); *Pobierz zdarzenia Pico* pokazuje też zdarzenia sprzed restartów.
- Próg przeciążenia rdzenia 1 (USB): 5 s (wysłanie wiadomości LoRa przez API trwa normalnie do ok. 2,5 s).
- *Pobierz log* wysyłany strumieniowo, podgląd na żywo pokazuje ostatnie 24 KB.
- Stare pliki logu `/log_a.txt`, `/log_b.txt` są usuwane przy pierwszym starcie.

## [0.41] – 2026-11-21

### Dodane
- **Kontrola zasobów Pico**: rekordy od startu (najmniej wolnego RAM, najdłuższy obieg pętli rdzenia 0 i 1), wolny RAM 10 min po starcie, wolne miejsce we flashu.
- **Ostrzeżenia w zdarzeniach i przez LoRa** (każdy rodzaj najwyżej raz na dobę): mało RAM (< 30 KB), możliwy wyciek pamięci (wolny RAM w spoczynku spada co godzinę przez 6 h, łącznie ≥ 16 KB), przeciążony rdzeń (obieg pętli > 5 s na każdym rdzeniu), mało miejsca we flashu (< 48 KB). Bez ostrzeżeń w trakcie operacji na firmware.
- **Ustawienie *Ostrzeżenia zasobów przez LoRa*** (domyślnie tak, skrypt `ZASOBY_LORA=TAK/NIE`) – przez LoRa idą tylko alarmy; wyłączone = sam wpis w zdarzeniach.
- **Komenda LoRa `ZASOBY`** – stan zasobów w jednej wiadomości; wpis `[ZASOBY]` w zdarzeniach co 3 h; rekordy i liczba ostrzeżeń w pasku stanu strony.
- Odpowiedź `ZASOBY` pokazuje zapas: wolne z całości i procent (RAM, flash), obiegi pętli na sekundę obu rdzeni.

### Pozostałe
- Instrukcja: rozdział o kontroli zasobów (progi, jak odczytać zapas) oraz całkowite czyszczenie Pico pakietem [Pico Universal Flash Nuke](https://github.com/Gadgetoid/pico-universal-flash-nuke) (sprawdzone na Pico 2 W).
- Źródła: linia `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` w `pico2w_flasher_meshtastic.ino` i `wyslij_firmware.py`.

## [0.40] – 2026-11-21

Zmiany od wydania 0.28 (wersje robocze 0.29–0.39 nie były publikowane).

### Dodane – bezpieczeństwo
- **Logowanie na stronie z sesją** (ciasteczko `HttpOnly`, `SameSite=Strict`, 128-bit, przypisane do adresu IP, wygasa po 30 min), przycisk *Wyloguj*. Zmiana hasła unieważnia wszystkie sesje.
- **Wymuszona zmiana domyślnych haseł** strony i sieci AP (min. 12 znaków) – do tego czasu flashowanie, pobieranie i zmiany ustawień są zablokowane.
- **Blokada zgadywania hasła**: 5 błędnych prób = blokada 60 s, każda kolejna dwa razy dłuższa (maks. 15 min), zgłoszenie przez LoRa.
- **Lista dozwolonych adresów `IP_DOZWOLONE`** w sieci domowej (Wi-Fi STA i Ethernet).
- **Podpisane żądania** strony i skryptu (HMAC-SHA256 z klucza z hasła i soli, rosnący licznik). Hasło nie jest wysyłane siecią, powtórzone żądanie jest odrzucane.
- **Sól hasła i identyfikator Pico** (12 znaków hex z numeru seryjnego układu). Skrypt: `--pico-id` i `--sol`, LoRa: `SOL`. Przeglądarka i skrypt ostrzegają, gdy sól się zmieni.
- **Skrypt loguje się jak przeglądarka**: hasło podajesz raz, skrypt pamięta tylko sesję. Skrypt pobrany ze strony nie zawiera hasła ani adresu Pico.
- **Ochrona przed cofnięciem wersji**: tylko ta sama albo nowsza wersja; starsza po jednorazowym przełączniku downgrade (strona, `DOWNGRADE:TAK`, ważny 24 h, wymaga NTP).
- **`mtnod.txt` podpisany hasłem i jednorazowym piaskiem** (6 znaków podawanych w komendzie LoRa), ze znacznikiem czasu i SHA-256 każdego pakietu. Zestaw nie nowszy niż ostatnio przyjęty jest odrzucany.
- **Zaszyfrowana konfiguracja węzła na serwerze** (klucz z hasła i piasku, znacznik HMAC).
- **Dziennik `[AUDYT]`**: logowania, zmiany ustawień, flashowanie, resety, pobrania, komendy LoRa, odmowy i blokady – z adresem IP lub nadawcą LoRa.
- Nagłówki bezpieczeństwa strony (`Content-Security-Policy`, `X-Frame-Options`, `nosniff`, `no-referrer`, `no-store`).

### Dodane – sieć i dostęp do węzła
- **Ethernet**: W5500-mini (SPI1, GP8–GP13) albo HAT W5100S (SPI0, GP16–GP21), wybór modułu i trybu sieci (`TRYB_SIECI`) w ustawieniach, stałe IP. **Nieprzetestowane – może nie działać.**
- **Port węzła TCP 4403** (`PORT_WEZLA:2h`, maks. 7 dni) dla aplikacji Meshtastic, CLI i VCOM – tylko adresy prywatne, samo się zamyka.
- **`STRONA` / `STRONA:WL` / `STRONA:WYL`** – wyłączenie strony w przeglądarce (403), skrypt i LoRa działają dalej.
- **Hop limit odpowiedzi LoRa** (`HOP:0–7`, domyślnie 3).

### Dodane – węzeł, flashowanie i logi
- **Strażnik węzła `BRAK_ODPOWIEDZI`**: sprawdzenie przez API, restart hosta USB, reset RTS, impuls na pinie resetu (stan aktywny i czas impulsu w ustawieniach); wynik zgłaszany przez LoRa. Przycisk *Reset węzła* na stronie.
- **Dziennik etapów flashowania** (`/flash.etap`): po zaniku zasilania albo restarcie Pico wgrywanie jest dokończone samo, do 3 prób na etap.
- **Automatyczne włączanie Debug log na węźle** (ustawienie *debug_log_api_auto*, domyślnie włączone). Skrypt sam dodaje `debug_log_api_enabled: true` do pakietu konfiguracji.
- **Log startów węzła**: 2 ostatnie starty, każdy do 24 KB (60 s od ESP-ROM, baneru MESHTASTIC albo „Booted”). Przycisk *Pobierz starty węzła*, skrypt `--logi` pobiera też `pico_starty_wezla.txt`.
- **Skrypt ustawia wszystko to, co strona** (`--ustaw`, `--pokaz-ustawienia`), oprócz hasła strony.
- Instrukcja: listy poleceń LoRa i skryptu, przykłady, schematy wyłączników TPS2552D i TPS2553DBVR.

### Zmienione
- **Domyślny pin resetu węzła: GP22 (pin 29)** – z dala od pinów modułów Ethernet. Po aktualizacji zapisany wcześniej pin zostaje – zmień go w *Pin resetu węzła* albo `--ustaw PIN_RESETU=22` i przełóż przewód.
- Opcja skryptu `--pico` ma teraz nazwę `--pico-id`.
- Zestaw plików z serwera z tym samym lub starszym znacznikiem czasu niż ostatnio przyjęty jest odrzucany.
- Restart węzła jest potwierdzany dopiero po linii „Use nodenum” z jego logu.

### Poprawione
- Komendy LoRa rozpoznawane także po nagłówku „decoded message” (węzły, które nie wypisują `handleReceived(REMOTE)`).
- `STRONA:WYL` nie zatrzymuje już całego serwera, tylko stronę.
- Zapis dziennika etapów z kontrolą odczytu i wymuszonym zapisem logu do flasha.
- Komunikat o braku pliku OTA w skrypcie.
- Skrypt nie pyta dwa razy o hasło (instalacja `requests` przed pytaniem o hasło).

## [0.28]

- Tryb POWMAN – głęboki sen Pico (ok. 0,3–1 mA), pobudka timerem (`POWMAN_TIME`) albo pinem GP14, okno czuwania `POWMAN_CZUWANIE`.
- Każda operacja na firmware wyłącza POWMAN automatycznie, z wiadomością LoRa.
- Logi węzła po komendach LoRa i po pobudce POWMAN (dekodowanie LogRecord, wymaga opcji Debug log).
- Kanał komend tylko prywatny z kluczem 256-bit.
- Bezpieczniejsza aktualizacja Pico (odrzuca UF2 i pliki bez `PICOVER=`), obowiązkowy reset węzła po aktualizacji Pico.
- Skrypt `wyslij_firmware.py` do pobrania ze strony Pico (`skrypt_py.h`).
- Odporność USB: jednorazowy restart Pico, gdy po starcie nie widać węzła; przycisk „Restart hosta USB” działa zawsze.
- Poprawki logów: długie wpisy, znaki sterujące, godzina w liniach z LogRecord.
