## Bezpieczeństwo – Pico2W flasher (wersja 0.40)

### Nowe w 0.40
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
