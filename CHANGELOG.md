# Dziennik zmian – Pico2W flasher

Program Pico i skrypt `wyslij_firmware.py` mają zawsze ten sam numer wersji.

Pomysłodawca projektu: Rafał Cichosz (rpc) · Kodowanie: Claude Opus 5.5

## [0.48] – 2026-11-23

**Nieprzetestowane na sprzęcie: Modbus RTU i Ethernet.** Pozostałe funkcje 0.48 sprawdzone na Heltec V4 (firmware 2.8.3).

### Dodane
- Skrypt `wyslij_firmware.py` wkompilowany w Pico jest skompresowany gzip (ok. 107 KB → 34 KB, program Pico mniejszy o ok. 73 KB). Przeglądarka rozpakowuje go przy pobieraniu sama.
- Osobny mały system plików (64 KB) na końcu obszaru programu Pico na ustawienia, zdarzenia Pico i małe pliki stanu (płytka, mail, historia wgrań). Nie zależy od pakietu, logów węzła ani formatowania dużego LittleFS (także przy aktualizacji programu Pico zdarzenia teraz zostają). Kopia ustawień zostaje też w dużym FS. Przy pierwszym starcie pliki przenoszą się same. Status pokazuje zajętość obu. „Informacja o wersjach” pokazuje rozmiar programu Pico wobec limitu i zapas. Program Pico musi mieć mniej niż 974 848 B (ok. 950 KB; Arduino podaje maksimum 1 040 384 B, minus 64 KB), inaczej wszystko zostaje w dużym FS (wpis w zdarzeniach).
- **Modbus RTU (RS485)** na UART0: GP0 TX (pin 1), GP1 RX (pin 2), GP2 DE/RE (pin 4). Do 8 odczytów: nazwa, adres, funkcja 03/04, rejestr, typ (u16, s16, u32, s32, f32, z „r” = odwrócone słowa), mnożnik, offset, pole telemetrii środowiskowej Meshtastic, kanał i *do sieci* (odznaczone = odczyt tylko na stronie, przez LoRa i dla termostatu). Odpytywanie bez blokowania pętli (co 5–3600 s), błędy w zdarzeniach `[MODBUS]` raz do powrotu odczytu, wartości na pasku stanu i komendą LoRa `MODBUS`. Telemetria środowiskowa (`TELEMETRY_APP`) na wybrany kanał co ustawioną liczbę minut, pola zgrupowane per kanał; 0 = nie wysyłaj. Skrypt: `MODBUS`, `MB_BAUD`, `MB_ODPYT`, `MB_WYSYL`, `MB1`–`MB8`.
- **Termostat z odczytu Modbus** (źródło 3, `TERM_MODBUS` = numer odczytu).
- **Usuwanie pakietu firmware z Pico** na żądanie: przycisk *Usuń pakiet* (drugi rząd), LoRa `USUN_PAKIET:TAK`, skrypt `--usun-pakiet`. Domyślnie pakiet zostaje na Pico jako awaryjne firmware; usunięcie zwalnia miejsce na logi węzła. Odmowa w trakcie wgrywania i po przerwanym wgraniu (pakiet potrzebny do dokończenia).
- **Termostat na GP15 (pin 20)**: wyjście sterowane temperaturą Pico (z poprawką) albo czujnika węzła (telemetria środowiskowa). Dwa progi dają histerezę: *załącz* niżej niż *wyłącz* = grzałka (np. 2 / 5 °C), *załącz* wyżej = wentylator (np. 35 / 30 °C). Stan awaryjny przy braku odczytu (wyłączone albo załączone, po ustawionej liczbie minut), min. 1 min między przełączeniami, wyjście aktywne stanem wysokim (MOSFET) albo niskim (moduł przekaźnika). Przełączenia w zdarzeniach `[TERMO]`, stan na pasku i komendą LoRa `TERMOSTAT`; skrypt `TERMOSTAT`, `TERM_ZALACZ`, `TERM_WYLACZ`, `TERM_BRAK_MIN`, `TERM_BRAK`, `TERM_NISKO`, `TERM_POPRAWKA`.
- **Kontrola płytki przed wgraniem firmware**: nazwa firmware z pakietu (z pliku `firmware-NAZWA-wersja`, np. `heltec-v4`) musi być tą, którą węzeł zgłasza przez API (`pio_env`). Inaczej Pico odmawia – komunikat na stronie, w zdarzeniach i przez LoRa. Gdy węzeł nie odpowiada, Pico porównuje z ostatnią zapamiętaną nazwą. Dodatkowo, jednocześnie z nazwą, musi się zgadzać model płytki (`hw_model` węzła z `hwModel` z `.mt.json` – podają go oficjalne płytki), więc ta sama nazwa builda dla innej płytki nie przejdzie. Gdy nic nie wiadomo – wgrywa z ostrzeżeniem w zdarzeniach. Zmiana egzemplarza węzła (`device_id`) jest odnotowywana w zdarzeniach.
- **Zgoda na inną płytkę** (świadoma zmiana firmware): komenda LoRa `INNA_PLYTKA:nazwa`, przycisk *Inna płytka (1 h)* albo skrypt `--inna-plytka nazwa` – jedno wgranie, najwyżej 1 h, wpis w zdarzeniach i potwierdzenie LoRa. `INNA_PLYTKA` bez nazwy podaje stan kontroli.
- **Ustawienie *Kontrola płytki*** (domyślnie tak; skrypt `KONTROLA_PLYTKI=TAK/NIE`) i przycisk **Zapomnij węzeł** – po przełożeniu Pico do innego węzła usuwa historię wgrań, ostatnią wersję firmware, nazwę firmware węzła, logi i starty węzła (zostają ustawienia, zdarzenia Pico i pakiet).
- Skrypt odczytuje płytkę z samej binarki (ścieżki `.pio/libdeps/<płytka>/` zapisane przy kompilacji) i przerywa, gdy nie zgadza się z nazwą pliku i `.mt.json` – ręczna zmiana nazwy pliku nie oszuka kontroli. Do pakietu trafia nazwa z binarki.
- **Logi mailem** (SMTP z TLS, port 465): po każdym restarcie węzła (zamierzonym lub nie; najwyżej raz na godzinę) i co tydzień albo codziennie Pico wysyła komplet – niewysłany log węzła (jeden plik `.gz`), zdarzenia Pico i starty węzła. Przed rotacją, czyli zanim najstarsze logi węzła zostaną usunięte, wysyła sam niewysłany log węzła. Na żądanie – także przy wyłączonej wysyłce automatycznej (`MAIL_KOMPLET=-1`) – komplet wysyła przycisk *Wyślij logi mailem*, komenda LoRa `WYSLIJ_LOGI` (wynik wraca przez LoRa) albo skrypt `--logi-mail`. Błąd: ponowna próba co 30 min, powiadomienie LoRa najwyżej raz na dobę.
- **Alarm logowania mailem** (domyślnie włączony, `MAIL_ALARM`): błędne hasło, blokada adresu albo dostęp spoza listy IP – mail z adresem IP, typem (strona/skrypt) i liczbą prób; próby zbierane 2 min, mail najwyżej co 15 min.
- **Czytelne tematy maili**: `[Pico <węzeł>] Logi na żądanie / Logi po restarcie węzła / Raport tygodniowy – logi / Log węzła przed rotacją / ALARM: nieudane logowanie z <IP> – <data>`.
- **Ustawienia maila**: serwer SMTP (`serwer` albo `serwer:port`), login (adres nadawcy), hasło (nie jest pokazywane), odbiorca (kilka po przecinku), komplet co tydzień / codziennie / tylko po restarcie; skrypt `MAIL_SERWER`, `MAIL_LOGIN`, `MAIL_HASLO`, `MAIL_DO`, `MAIL_KOMPLET`. Pusty serwer = bez maili.

### Poprawione
- Wpis o wysłanym alarmie logowania podaje liczbę różnych adresów IP i liczbę wpisów (wcześniej „2 adres/y” przy jednym adresie ze złym hasłem i blokadą).
- Kontrola płytki od razu przy rozpoczęciu wysyłania: skrypt podaje Pico płytkę i model z pakietu, a Pico odmawia przed usunięciem starego pakietu i logów (wcześniej odmawiało dopiero po wysłaniu całego pakietu, a stary pakiet i logi były już usunięte). Przy błędzie przed wejściem w bootloader zdarzenia piszą, że węzeł jest nietknięty (wcześniej mylnie „zostaje w trybie bootloadera”). Dłuższe komunikaty flashowania nie są już ucinane. Po „Usuń firmware z flash” status nie pokazuje już błędu ostatniego flashowania usuniętego pakietu.
- Komendy LoRa nie były rozpoznawane z firmware węzła 2.8.3: przy części wiadomości węzeł nie wypisuje do logu linii z treścią („Received text msg”). Pico zostaje teraz stale klientem API węzła (heartbeat co 5 min, ponowne połączenie po restarcie węzła, sesji 4403 albo wgrywaniu), więc węzeł przekazuje każdą wiadomość jako pakiet z treścią. Działa, gdy węzeł ma `debug_log_api_enabled` (logi płyną wtedy jako LogRecord). Przy okazji znika „ToPhone queue full” w logu węzła. Gdy węzeł nie ma `debug_log_api_enabled` i nie da się go włączyć (np. węzeł managed), Pico przechodzi w tryb awaryjny: co 30 s łączy się na chwilę, odbiera zaległe wiadomości (komendy) i rozłącza się. W logu węzła są wtedy krótkie przerwy.
- Ustawienia: sieć STA, hasło do strony (token), kanał komend i hop limit przeniesione pod ustawienia AP (wszystko o sieci i dostępie w jednym miejscu). Przycisk „Zapisz i zrestartuj Pico” przemianowany na „Zapisz”, bo ustawienia działają bez restartu.
- Miejsce na pakiet Pico sprawdza na samym początku, licząc to, co usunie (stary pakiet, logi i starty węzła). Gdy pakiet się nie zmieści, odmawia z dokładnymi liczbami i niczego nie usuwa, więc stary pakiet awaryjny i logi zostają. Skrypt pokazuje ten komunikat i kończy.
- Przed przyjęciem pakietu firmware węzła Pico usuwa wszystkie logi i starty węzła z flasha (zdarzenia Pico zostają). W trakcie odbioru i wgrywania logi węzła nie są zapisywane na flash (poza dziennikiem etapów flashowania), a po wgraniu zapis wraca sam. Po pakiecie zostaje co najmniej 96 KB wolnego flasha (wcześniej 32 KB). Ustawienia, zdarzenia i starty węzła mają zawsze miejsce, także w trakcie odbioru pakietu.
- Ustawienia zapisywane są do pliku tymczasowego i dopiero potem podmieniane (na małym FS i jako kopia w dużym). Wcześniej zapis przy zapełnionym flashu (np. w trakcie odbioru pakietu firmware) mógł uciąć plik ustawień i Pico po restarcie wracało do domyślnych (bez Wi-Fi STA, adresu URL itd.).
- „Starty węzła”: start zapisuje się tylko wtedy, gdy po linii ESP-ROM przyjdzie baner Meshtastic (`//\ E S H T /\ S T / C`). Urwany start bez baneru (np. węzeł zresetowany przy starcie Pico, gdy log łapie się dopiero od „Booted…”) albo sam ESP-ROM nie jest już zapisywany jako start węzła i nie wywołuje maila „po restarcie węzła”. Baner przychodzący kilkanaście sekund po ESP-ROM (np. po restarcie hosta USB) należy do tego samego startu.
- Log węzła w mailu jest teraz zwykłym plikiem `pico_log_wezla_<data>_<od>-<do>.txt` (data wysyłki i numery kawałków w nazwie, więc pliki z kolejnych maili sortują się po kolei i widać luki) (Pico rozpakowuje go przy wysyłce). Wcześniejszy załącznik .gz, sklejony z wielu kawałków, serwery pocztowe (np. o2/WP) odrzucały jako „wirus” (554).
- Zapis ustawień kończył się „Wymagane hasło”: po dodaniu pól Modbus, maila i termostatu formularz ma ok. 65 pól, a Pico sprawdzało podpis tylko dla pierwszych 48. Limit podniesiony do 96.
- Przyciski pobierania (log węzła, starty, zdarzenia, skrypt) są wyszarzone z napisem „Przygotowuję plik…” do końca pobierania – kolejne kliknięcia nie uruchamiają pobierania od nowa.
- Ustawienia: mniejsza czcionka opisów pozycji (nazwy pozycji bez zmian); „poprawka” czujnika Pico nazwana „offset”. Przycisk „Usuń firmware z flash”.
- Po zakończeniu sesji na porcie węzła 4403 Pico wysyła węzłowi `disconnect`. Aplikacja często nie kończy sesji API, a węzeł gromadził wtedy pakiety „dla telefonu” aż do przepełnienia kolejki (`ToPhone queue full, drop packet` setki razy na godzinę w logu).
- Ustawienia domyślne zgodne ze schematem: reset węzła GP22 (pin 29), wybudzanie POWMAN GP14 (pin 19), termostat GP15 (pin 20). Przy aktualizacji do 0.47 pin resetu ustawiony wcześniej na GP15 jest jednorazowo przenoszony na GP22 (wpis w zdarzeniach).
- Po wgraniu firmware z konfiguracją Pico zgłaszało „debug_log_api_enabled wyłączone – brak pełnej sekcji security”, choć konfiguracja je włącza: oceniało stan odczytany przed zapisem konfiguracji. Teraz sprawdza węzeł od nowa 20 s po zakończeniu.
- Pasek stanu w trzech wierszach (węzeł / Pico i sieć / zasoby i logi), pole „złącze USB: wykryte urządzenie / puste” i płytka węzła.

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
