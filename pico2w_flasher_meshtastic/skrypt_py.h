// skrypt_py.h - wyslij_firmware.py wkompilowany w program Pico (do pobrania ze strony).
// Wygenerowane automatycznie z wyslij_firmware.py - nie edytowac recznie.
// Plik musi lezec w folderze szkicu obok pico2w_flasher_meshtastic.ino.
#pragma once
#define SKRYPT_PY_WERSJA "0.28"
static const char SKRYPT_PY[] = R"PYSKRYPT(#!/usr/bin/env python3
"""
wyslij_firmware.py - pomyslodawca projektu: Rafal Cichosz (rpc)

Wysyla firmware Meshtastic (i opcjonalnie konfiguracje) do Pico 2 W, ktore wgrywa je do wezla ESP32-S3 przez USB.
Pliki pomocnicze (.mt.json, mt-<mcu>-ota.bin, littlefs-*.bin) sa brane z folderu podanego pliku firmware.

TRYBY (rodzaj wgrania wynika z podanego pliku):
  1) AKTUALIZACJA - sam program, ustawienia/klucze/NVS zostaja, konfiguracja niepotrzebna:
     python3 wyslij_firmware.py -f firmware-heltec-v4-2.8.3.xxx.bin
  2) PELNA INSTALACJA - kasowanie CALEGO flasha + factory + OTA + LittleFS, potem konfiguracja z pliku:
     python3 wyslij_firmware.py -f firmware-heltec-v4-2.8.3.xxx.factory.bin -c node.yaml
     (bez -c wezel zostanie z ustawieniami fabrycznymi)
  3) FACTORY BEZ KASOWANIA CALEGO FLASHA - nadpisuje bootloader, tablice partycji, program i czysci NVS,
     dlatego WYMAGA pliku konfiguracji:
     python3 wyslij_firmware.py -f firmware-...factory.bin --tylko-factory -c node.yaml
  4) SAMA KONFIGURACJA (bez wgrywania firmware):
     python3 wyslij_firmware.py --tylko-konfiguracja -c node.yaml
  Specjalne:
     -f firmware-....mt.json --tylko-ota [--ota PLIK] (tylko partycja OTA; .mt.json = adres partycji,
                                                      mozna tez podac factory.bin, obok ktorego lezy)
     -f firmware-...factory.bin --ota moj-ota.bin    (pelna instalacja z wlasnym plikiem OTA)
     -f firmware-...factory.bin --reset-ustawien -c node.yaml  (czysci NVS + otadata, swiezy LittleFS,
                                                     program zostaje; potem konfiguracja z pliku)

Przydatne opcje:
  --tylko-wyslij           tylko wyslij pakiet na Pico; wgranie pozniej przyciskiem na stronie
  --pokaz-konfiguracje     podglad komend z pliku konfiguracji (nic nie wysyla)
  --ip ADRES               adres Pico (AP: 192.168.4.1, STA: adres z routera albo pico-flasher.local)
  --token HASLO            haslo/token Pico (to samo co do strony)

Wysylanie w dwoch krokach (gdy komputer nie moze miec naraz internetu i polaczenia z Pico,
np. Pico w trybie AP - siec Pico nie ma internetu, a biblioteka meshtastic jest pobierana z internetu):
  1) z internetem - tylko zbuduj pakiet i zapisz do pliku (nic nie wysyla):
     python3 wyslij_firmware.py -f firmware-...factory.bin -c node.yaml --zapisz pakiet.bin
  2) polaczony z Wi-Fi Pico - wyslij gotowy plik (nie potrzebuje internetu ani biblioteki meshtastic):
     python3 wyslij_firmware.py --wyslij-pakiet pakiet.bin                  (wyslij i wgraj)
     python3 wyslij_firmware.py --wyslij-pakiet pakiet.bin --tylko-wyslij   (tylko wyslij, wgranie z przycisku)
  Gdy Pico jest w Twojej sieci (tryb STA), wystarczy jedno polecenie - te dwa kroki sa niepotrzebne.

Aktualizacja programu Pico przez Wi-Fi (plik .bin: Arduino IDE -> Szkic -> Eksportuj skompilowany plik binarny):
  python3 wyslij_firmware.py --pico-fw pico2w_flasher_meshtastic.ino.bin --ip 192.168.1.26
  (usuwa pakiet ESP32-S3 z Pico i formatuje LittleFS; ustawienia Pico zostaja)

WYMAGANE PLIKI (w jednym folderze, np. rozpakowane wydanie Meshtastic dla plytki):
  aktualizacja (-f firmware-XXX.bin):
     firmware-XXX.bin, firmware-XXX.mt.json
  pelna instalacja / --tylko-factory / --reset-ustawien (-f firmware-XXX.factory.bin):
     firmware-XXX.factory.bin   - bootloader + tablica partycji + program
     firmware-XXX.mt.json       - adresy partycji (bez niego skrypt nie wie, gdzie co wgrac)
     mt-esp32s3-ota.bin         - program OTA (albo wlasny: --ota PLIK)
     littlefs-XXX.bin           - system plikow (pelna instalacja i --reset-ustawien)
  konfiguracja (-c): node.yaml (meshtastic --export-config) albo kopia .cfg z aplikacji Android
  aktualizacja Pico (--pico-fw): plik .bin ze szkicu Pico (nie .uf2)
  Brak ktoregokolwiek pliku = skrypt konczy sie bledem, nic nie wysyla.

AKTUALIZACJA PRZEZ INTERNET (Pico w trybie STA pobiera pliki z Twojego serwera WWW):
  Opcja --przygotuj-serwer FOLDER tworzy (albo uzupelnia) podfolder z gotowymi plikami do wgrania na serwer:
     pelna.mtpk         - pelna instalacja: factory + OTA + LittleFS + konfiguracja z -c (kasuje caly flash wezla)
     aktualizacja.mtpk  - zwykla aktualizacja: sam firmware-*.bin (ustawienia i dane wezla zostaja)
     pico.bin           - nowy program Pico (ustawienia Pico zostaja, nowe opcje dostaja wartosci domyslne)
     mtnod.txt          - lista: typ|wersja|plik|rozmiar|CRC32 + podpis HMAC-SHA256 tokenem Pico
  Dlaczego osobny folder: Pico nie umie zamienic YAML na konfiguracje ani rozpakowac wydania Meshtastic -
  dostaje gotowe pakiety (te same, co przy wysylaniu skryptem), a podpis chroni przed podmiana plikow.
  Sciezki w mtnod.txt sa wzgledne (pliki leza obok mtnod.txt), wiec wystarczy wgrac CALY folder na serwer
  i wpisac w Pico adres samego mtnod.txt (strona: Ustawienia, albo LoRa: ADRES_URL:https://.../mtnod.txt).
  Mozna tez wpisac w mtnod.txt pelne adresy https://... w kolumnie 'plik' - podpis tego nie obejmuje.
  Token (--token) musi byc taki sam jak w Pico - inaczej Pico odrzuci mtnod.txt.
  Istniejacy folder: podmieniane sa tylko podane czesci, pozostale wpisy mtnod.txt zostaja.

  Pliki wezla - pelna instalacja + aktualizacja (aktualizacja znaleziona obok factory):
     python3 wyslij_firmware.py --przygotuj-serwer komin -f firmware-heltec-v4-2.8.3.xxx.factory.bin -c komin.yaml --token MojeHaslo
  Plik OTA (mt-esp32s3-ota.bin) i LittleFS sa brane automatycznie z folderu firmware, jak przy zwyklej
  pelnej instalacji; wlasny plik OTA: dodaj --ota moj-ota.bin
  Tylko aktualizacja wezla:
     python3 wyslij_firmware.py --przygotuj-serwer komin -f firmware-heltec-v4-2.8.3.xxx.bin --token MojeHaslo
  Program Pico - zawsze OSOBNYM poleceniem (rzadko), do tego samego folderu:
     python3 wyslij_firmware.py --przygotuj-serwer komin --pico-fw pico2w_flasher_meshtastic.ino.bin --token MojeHaslo
  (-f i --pico-fw razem sa odrzucane - wezel i Pico przygotowujesz i aktualizujesz oddzielnie)
  Potem przez LoRa:  POBIERZ_INTERNET:pelna (albo :aktualizacja / :pico), a po potwierdzeniu  AKTUALIZUJ_INTERNET

KOMENDA LoRa WERSJE (Pico 0.24+): wersja firmware odczytana z wezla, OTA i konfiguracja ostatnio wgrane przez Pico.

LOGI:
  python3 wyslij_firmware.py --logi                    (3 logi do nowego folderu logi_pico_RRRRMMDD_GGMM)
  python3 wyslij_firmware.py --logi moje_logi --ip pico-flasher.local --token MojeHaslo
  python3 wyslij_firmware.py -f firmware-...bin --logi  (wgranie, a po nim logi; przy bledzie logi tez sie pobiora)

TYLKO OTA (adres partycji z pliku metadanych .mt.json, firmware nie jest wgrywane):
  python3 wyslij_firmware.py --tylko-ota -f firmware-heltec-v4-2.8.3.xxx.mt.json --ota mt-esp32s3-ota.bin

USTAWIENIA PICO (od 0.26; wszystko oprocz tokenu - token zmieniasz tylko na stronie):
  python3 wyslij_firmware.py --pokaz-ustawienia --ip 192.168.1.26
  python3 wyslij_firmware.py --ustaw BRAK_ODPOWIEDZI=30 --ustaw PIN_RESETU=15 --ip 192.168.1.26
  python3 wyslij_firmware.py --ustaw "ADRES_URL=https://serwer/komin/mtnod.txt" --ustaw FILTR_NODE=04a9d917,6ecc4f80
  Klucze: AP_SSID, AP_PWD, STA_SSID, STA_PWD, MODE_WIFI (AP/STA), KANAL (0-7), FILTR_NODE (min. 2 wezly albo *),
  ADRES_URL (pusty = brak), BRAK_ODPOWIEDZI (minuty, 0 = wylaczone), PIN_RESETU (GP, -1 = brak),
  RESET_STAN (WYSOKI/NISKI), RESET_CZAS (1-60 s), POWMAN_TIME (0 / 12 / 6,18 / 5m), POWMAN_CZUWANIE (1-240), KOMENDA_WLACZ, KOMENDA_USPIJ, KOMENDA_AP_WLACZ,
  KOMENDA_AP_WYLACZ, KOMENDA_TEMP. Separator '=' albo ':' (pierwszy w tekscie, jak w komendach LoRa).
  Pico sprawdza wartosci tak samo jak w panelu. Od 0.27 restartuje sie tylko po zmianie Wi-Fi
  (AP_SSID, AP_PWD, STA_SSID, STA_PWD, MODE_WIFI); pozostale zmiany dzialaja od razu.
  Zmiana MODE_WIFI albo sieci zmienia adres Pico - kolejne polecenia z nowym --ip.

ZGODNOSC WERSJI: przed wysylaniem skrypt sprawdza wersje programu Pico. Inny numer niz skryptu = blad,
  nic nie jest wysylane. Dzialaja wtedy tylko --pico-fw (wyrownanie wersji) i --restart-pico.

Inne:
  python3 wyslij_firmware.py --restart-pico            (restart Pico)
  python3 wyslij_firmware.py --ip pico-flasher.local   (Pico w trybie STA)
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import time
import zlib

try:
    import requests
except ImportError:
    requests = None             # zostanie doinstalowane w srodowisku venv

WERSJA = "0.28"
DEFAULT_IP = "192.168.4.1"
DEFAULT_TOKEN = "MojeBezpieczneHaslo123"
# Wlasne srodowisko w podfolderze obok skryptu - niezalezne od starej wersji z pakietow Debiana
DEFAULT_VENV = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".pico_meshtastic_env")
ZNACZNIK_VENV = "WYSLIJ_FW_VENV_GOTOWE"


if os.name == "nt":
    os.system("")                                           # Windows: wlacza kody ANSI (czyszczenie ekranu) w konsoli
    try:
        sys.stdout.reconfigure(errors="replace")            # znaki spoza strony kodowej konsoli nie przerwa skryptu
    except Exception:
        pass


def jest_internet(host="pypi.org", port=443, czas=3):
    import socket
    try:
        socket.create_connection((host, port), timeout=czas).close()
        return True
    except OSError:
        return False


def sprawdz_wersje_pico(ip, token):
    """Skrypt i program Pico musza miec ten sam numer wersji - inaczej skrypt konczy sie bledem
    (format pakietu lub polecen mogl sie zmienic). Wyjatek: --pico-fw (tym sie wersje wyrownuje)."""
    try:
        r = requests.get(f"http://{ip}/status", headers={"X-Auth-Token": token}, timeout=10)
    except Exception as e:
        blad(f"Nie mozna odczytac wersji Pico ({e}) - nic nie wysylam.")
    if r.status_code == 401:
        blad("Pico odrzucilo token (--token) - nic nie wysylam.")
    import re
    m = re.search(r"wersja Pico ([^ |]+)", r.text)
    if not m:
        blad("Nie mozna ustalic wersji programu Pico (starsza wersja?) - nic nie wysylam.\n"
             "        Zaktualizuj Pico:  --pico-fw pico2w_flasher_meshtastic.ino.bin")
    if m.group(1) != WERSJA:
        blad(f"Wersja programu Pico {m.group(1)} rozni sie od wersji skryptu {WERSJA} - nic nie wysylam.\n"
             f"        Skrypt i Pico musza miec ten sam numer. Uzyj skryptu {m.group(1)} albo zaktualizuj Pico:\n"
             f"        --pico-fw pico2w_flasher_meshtastic.ino.bin  (program Pico w wersji {WERSJA})")
    print(f"Pico wersja {m.group(1)} - zgodna ze skryptem.")


def czekaj_na_pico(ip):
    """Sprawdza, czy Pico odpowiada; jesli nie - prosi o polaczenie z jego Wi-Fi."""
    import socket
    def jest():
        try:
            host, _, port = ip.partition(":")
            socket.create_connection((host, int(port or 80)), timeout=3).close()
            return True
        except OSError:
            return False
    if jest():
        return
    if not sys.stdin.isatty():
        blad(f"Pico ({ip}) nie odpowiada")
    print(f"\nPakiet gotowy. Pico ({ip}) nie odpowiada.")
    while not jest():
        if input("Polacz komputer z Wi-Fi Pico i nacisnij Enter (q = koniec): ").strip().lower() == "q":
            sys.exit(1)
    print("Pico odpowiada.")


def zapewnij_requests(a):
    """Brak 'requests' w biezacym Pythonie (np. swiezy Python na Windows): uruchom skrypt w jego venv
    (.pico_meshtastic_env) - tworzac go, jesli trzeba (wtedy potrzebny internet)."""
    if requests is not None:
        return
    py = os.path.join(a.venv, "Scripts", "python.exe") if os.name == "nt" else os.path.join(a.venv, "bin", "python3")
    print("Brak biblioteki 'requests' w tym Pythonie - uzywam srodowiska " + a.venv)
    przygotuj_venv(a.venv, aktualizuj=not os.path.isfile(py), pre=a.pre)
    blad("Brak biblioteki requests (instalacja: py -m pip install requests)")


PY_MIN, PY_MAX = (3, 9), (3, 14)      # wersje Pythona, dla ktorych meshtastic i pyyaml maja gotowe pakiety


def python_dla_venv():
    """Python do zbudowania venv: biezacy, jesli wersja jest wspierana; inaczej szuka 3.13 (potem 3.12, 3.14, 3.11)
    - na Windows przez 'py -3.13', na Linuksie 'python3.13'. Brak = komunikat, co zainstalowac."""
    import subprocess
    if PY_MIN <= sys.version_info[:2] <= PY_MAX:
        return [sys.executable]
    for w in ("3.13", "3.12", "3.14", "3.11"):
        kand = ["py", "-" + w] if os.name == "nt" else ["python" + w]
        try:
            if subprocess.run(kand + ["-c", "import venv"], capture_output=True, timeout=20).returncode == 0:
                print(f"Python {sys.version_info[0]}.{sys.version_info[1]} jest za "
                      f"{'nowy' if sys.version_info[:2] > PY_MAX else 'stary'} dla biblioteki meshtastic"
                      f" - srodowisko buduje Pythonem {w}.")
                return kand
        except Exception:
            pass
    blad(f"Python {sys.version_info[0]}.{sys.version_info[1]} nie jest jeszcze wspierany przez biblioteki meshtastic/pyyaml.\n"
         "        Zainstaluj Python 3.13 z python.org (moze byc obok obecnego) i uruchom skrypt ponownie -\n"
         "        skrypt sam go znajdzie.")


def przygotuj_venv(katalog, aktualizuj=True, pre=False):
    """Tworzy venv (jesli go nie ma), instaluje/aktualizuje NAJNOWSZE meshtastic i uruchamia
    skrypt ponownie w tym srodowisku - protobufy zawsze zgodne z aktualnym firmware.
    Bez internetu: uzywa tego, co jest w venv, albo biezacego Pythona, jesli ma meshtastic."""
    import subprocess
    if os.environ.get(ZNACZNIK_VENV) == "1":
        return                                              # juz dzialamy w przygotowanym venv
    py = os.path.join(katalog, "Scripts", "python.exe") if os.name == "nt" else os.path.join(katalog, "bin", "python3")

    def wersja_w(python):
        if not os.path.isfile(python):
            return ""
        return subprocess.run([python, "-c", "import importlib.metadata as m; import meshtastic.protobuf;"
                               "print(m.version('meshtastic'))"], capture_output=True, text=True).stdout.strip()

    if aktualizuj and not jest_internet() and sys.stdin.isatty():
        print("[UWAGA] Brak internetu - potrzebny do pobrania najnowszej biblioteki meshtastic.")
        while not jest_internet():
            odp = input("        Podlacz internet i nacisnij Enter (p = pomin aktualizacje, q = koniec): ").strip().lower()
            if odp == "q":
                sys.exit(1)
            if odp == "p":
                break
    if aktualizuj and not jest_internet():
        print("[UWAGA] Bez aktualizacji biblioteki meshtastic.")
        aktualizuj = False
        if not wersja_w(py):
            try:
                import importlib.metadata as md
                import meshtastic.protobuf  # noqa: F401
                print(f"        Uzywam meshtastic {md.version('meshtastic')} z biezacego Pythona ({sys.prefix}).\n")
                return
            except Exception:
                blad("Brak internetu i brak biblioteki meshtastic.\n"
                     "        Podlacz komputer do sieci z internetem (nie do Wi-Fi Pico) i uruchom skrypt ponownie.\n"
                     "        Mozna tez zbudowac pakiet z internetem:  --zapisz pakiet.bin\n"
                     "        a potem, polaczonym z Pico, wyslac go:    --wyslij-pakiet pakiet.bin")

    if os.path.isfile(py):                                  # venv zbudowany nieobslugiwanym Pythonem (np. 3.15) - od nowa
        w = subprocess.run([py, "-c", "import sys; print(sys.version_info[0], sys.version_info[1])"],
                           capture_output=True, text=True).stdout.split()
        if len(w) == 2 and not (PY_MIN <= (int(w[0]), int(w[1])) <= PY_MAX):
            import shutil
            print(f"Srodowisko {katalog} ma Pythona {w[0]}.{w[1]} (nieobslugiwany) - tworze je od nowa.")
            shutil.rmtree(katalog, ignore_errors=True)
    if not os.path.isfile(py):
        print(f"Tworze srodowisko Pythona: {katalog}")
        if subprocess.call(python_dla_venv() + ["-m", "venv", katalog]) != 0:
            blad("Nie udalo sie utworzyc venv. Na Debianie: sudo apt install python3-venv")
        aktualizuj = True
    if aktualizuj:
        print("Aktualizacja: pip, meshtastic[cli], pytap2, requests, pyyaml (najnowsze wersje"
              + (", z testowymi --pre" if pre else "") + ")...")
        pip = [py, "-m", "pip", "install", "--upgrade", "--quiet", "--disable-pip-version-check",
               "--retries", "1", "--timeout", "15"]
        if subprocess.call(pip + ["pip"]) != 0 or \
           subprocess.call(pip + (["--pre"] if pre else []) + ["pytap2", "meshtastic[cli]", "requests", "pyyaml"]) != 0:
            if not wersja_w(py):
                blad("Instalacja bibliotek nie powiodla sie. Podlacz internet i uruchom ponownie.")
            print("[UWAGA] Aktualizacja nie powiodla sie - uzywam wersji juz zainstalowanej.")
    wersja = wersja_w(py)
    if not wersja:
        blad(f"W {katalog} nie ma biblioteki meshtastic. Podlacz internet i uruchom ponownie.")
    print(f"meshtastic {wersja} w {katalog}{'' if aktualizuj else ' (bez aktualizacji)'}\n")
    sys.stdout.flush()
    env = dict(os.environ, **{ZNACZNIK_VENV: "1", "PYTHONNOUSERSITE": "1"})
    env.pop("PYTHONPATH", None)                             # zadnych bibliotek spoza venv
    if os.name == "nt":                                     # Windows: execve nie zastepuje procesu (konsola by sie rozjechala)
        try:
            sys.exit(subprocess.call([py, os.path.abspath(__file__)] + sys.argv[1:], env=env))
        except KeyboardInterrupt:
            sys.exit(130)
    os.execve(py, [py, os.path.abspath(__file__)] + sys.argv[1:], env)


def blad(msg):
    print(f"[BLAD] {msg}")
    sys.exit(1)


KAWALEK = 65536          # rozmiar kawalka = bufor Pico (64 KB)
PROBY = 10               # ile razy ponawiac jeden kawalek


def pasek(wyslano, razem, t0):
    proc = int(wyslano * 100 / razem) if razem else 100
    kbs = wyslano / 1024 / max(time.time() - t0, 0.001)
    p = "#" * (proc // 4) + "." * (25 - proc // 4)
    print(f"\r      [{p}] {proc:3d}%  {wyslano // 1024}/{razem // 1024} KB  {kbs:.0f} KB/s ", end="", flush=True)


def wyslij_kawalkami(baza, token, pakiet, crc, segmenty, cel=None):
    """Wysyla pakiet kawalkami po 32 KB; kazdy z wlasnym CRC, zly kawalek jest ponawiany."""
    naglowki = {"X-Auth-Token": token, "Connection": "close"}
    print("      (na czas wysylania wylacz podglad logow na stronie Pico albo zamknij strone)")
    try:
        r = requests.post(f"{baza}/fw_begin", timeout=(10, 30),
                          headers={**naglowki, "X-Total-Size": str(len(pakiet)), "X-Total-CRC": f"{crc:08X}",
                                   **({"X-Target": cel} if cel else {})})
    except requests.exceptions.RequestException as e:
        blad(f"Brak polaczenia z Pico: {e}\nSprawdz, czy komputer jest w sieci Pico.")
    if r.status_code != 200:
        blad(f"Pico odmowilo przyjecia pakietu ({r.status_code}): {r.text}")

    poz, t0, ponowien = 0, time.time(), 0
    biezacy = -1                                    # indeks aktualnie wysylanego obrazu
    linia = {"pre": "", "h": 0}                     # biezacy wiersz: opis + liczba '#'

    def rysuj(koniec=False):                        # procent biezacego obrazu (nie calego pakietu)
        _, a_, b_ = segmenty[biezacy]
        proc = 100 if koniec else max(0, min(100, (poz - a_) * 100 // max(1, b_ - a_)))
        print(f"\r{linia['pre']}{'#' * linia['h']}  {proc:3d}% ", end="", flush=True)

    def nowa_sekcja(idx, hashe=0):
        opis, a_, b_ = segmenty[idx]
        linia["pre"] = f"      Wysylam {opis} ({max(1, (b_ - a_ + 1023) // 1024)} KB): "
        linia["h"] = hashe

    def sekcja(p):
        for i, (_, a, b) in enumerate(segmenty):
            if a <= p < b:
                return i
        return len(segmenty) - 1

    while poz < len(pakiet):
        s_idx = sekcja(poz)
        if s_idx != biezacy:
            if biezacy >= 0:
                rysuj(True)
                print(" OK", flush=True)
            biezacy = s_idx
            nowa_sekcja(s_idx)
            rysuj()
        kaw = pakiet[poz:poz + KAWALEK]
        kcrc = zlib.crc32(kaw) & 0xFFFFFFFF
        granica = "----PicoFw" + os.urandom(8).hex()
        cialo = (f"--{granica}\r\n"
                 f'Content-Disposition: form-data; name="file"; filename="kawalek.bin"\r\n'
                 f"Content-Type: application/octet-stream\r\n\r\n").encode() + kaw + \
                f"\r\n--{granica}--\r\n".encode()
        ok = False
        for proba in range(1, PROBY + 1):
            try:
                r = requests.post(f"{baza}/fw_chunk", data=cialo, timeout=(10, 30),
                                  headers={**naglowki, "X-Offset": str(poz), "X-Chunk-CRC": f"{kcrc:08X}",
                                           "Content-Type": f"multipart/form-data; boundary={granica}"})
            except requests.exceptions.RequestException as e:
                czekaj = min(2 * proba, 10)
                print(f"\n        ! brak odpowiedzi ({e.__class__.__name__}), "
                      f"proba {proba}/{PROBY}, ponawiam za {czekaj} s", flush=True)
                ponowien += 1
                time.sleep(czekaj)
                continue
            if r.status_code == 200:                      # "OK <nastepny offset>"
                poz = int(r.text.split()[1]); ok = True; break
            if r.status_code == 409 and r.text.startswith("NEXT"):
                poz = int(r.text.split()[1]); ok = True; break   # Pico ma juz ten kawalek
            print(f"\n        ! {r.status_code} {r.text}, proba {proba}/{PROBY}", flush=True)
            ponowien += 1
            if r.status_code in (401, 413, 500, 507):
                blad("Blad, ktorego ponowienie nie naprawi - przerywam.")
            time.sleep(1)
        if not ok:
            print()
            blad(f"Kawalek @{poz} nie przeszedl po {PROBY} probach. Sprawdz 'Zdarzenia Pico'.")
        linia["h"] += 1
        # kawalek mogl objac kolejne, male obrazy (np. LittleFS po kompresji ma 2-4 KB)
        while biezacy < sekcja(poz - 1):
            rysuj(True)
            print(" OK", flush=True)
            biezacy += 1
            nowa_sekcja(biezacy, 1)
        rysuj()
    rysuj(True)
    print(" OK")
    dt = max(time.time() - t0, 0.001)
    print(f"      Przeslano {len(pakiet) // 1024} KB w {dt:.0f} s ({len(pakiet) / 1024 / dt:.0f} KB/s)")

    r = requests.post(f"{baza}/fw_end", headers=naglowki, timeout=(10, 60))
    if r.status_code != 200:
        blad(f"Kontrola calego pakietu na Pico nie przeszla ({r.status_code}): {r.text}")
    print(f"      {r.text}  ({time.time() - t0:.0f} s, ponowien: {ponowien})")


def offset_partycji(parts, subtype, domyslny):
    for p in parts:
        if p.get("subtype") == subtype:
            return int(p["offset"], 16)
    return domyslny



# =============================================================================
# KONFIGURACJA: YAML (meshtastic --export-config) lub .cfg (kopia z Androida)
#   -> ramki API Meshtastic (ToRadio) z komendami administracyjnymi
# =============================================================================
UWAGA_CZAS = ("Pelne wgrywanie trwa ok. 3-5 minut: kasowanie flasha, zapis firmware/OTA/LittleFS, start wezla,\n"
              "generowanie kluczy i konfiguracja (wezel startuje kilka razy). Nie odlaczaj zasilania w tym czasie.")
OPIS = []                           # opis pakietu pokazywany na stronie Pico
INFO_OFFSET = 0xFFFFFFFE            # wpis z opisem pakietu (nie flashowany)


def opis_aplikacji(dane, off):
    """Wersja z naglowka programu ESP32 (esp_app_desc_t), np. '2.8.3.f99f888, skompilowano 2026-...'."""
    d = dane[off + 0x20: off + 0x20 + 112]
    if len(d) < 112 or d[:4] != b"\x32\x54\xcd\xab":
        return "?"
    txt = lambda b: b.split(b"\0")[0].decode("ascii", "replace").strip()
    return f"{txt(d[16:48])} ({txt(d[48:80])}, kompilacja {txt(d[96:112])} {txt(d[80:96])})"


CFG_PLACEHOLDER = 0xDEADBEEF        # numer wezla podmieniany przez Pico
CFG_RESTART = 0x02                  # po tej ramce Pico czeka na restart wezla i laczy sie ponownie


def importuj_protobufy():
    from types import SimpleNamespace
    try:
        from meshtastic.protobuf import (admin_pb2, apponly_pb2, channel_pb2, config_pb2,
                                         localonly_pb2, mesh_pb2, module_config_pb2, portnums_pb2)
        try:
            from meshtastic.protobuf import clientonly_pb2
        except ImportError:
            clientonly_pb2 = None
    except ImportError:
        try:
            from meshtastic import (admin_pb2, apponly_pb2, channel_pb2, config_pb2,
                                    localonly_pb2, mesh_pb2, module_config_pb2, portnums_pb2)
            try:
                from meshtastic import clientonly_pb2
            except ImportError:
                clientonly_pb2 = None
        except ImportError:
            blad("Do konfiguracji potrzebna jest biblioteka meshtastic: pip install meshtastic")
    return SimpleNamespace(admin=admin_pb2, apponly=apponly_pb2, channel=channel_pb2, config=config_pb2,
                           local=localonly_pb2, mesh=mesh_pb2, module=module_config_pb2,
                           port=portnums_pb2, client=clientonly_pb2)


def _bez_base64(x):
    """YAML z CLI zapisuje bajty jako 'base64:...' - protobuf/json oczekuje samego base64."""
    if isinstance(x, dict):
        return {k: _bez_base64(v) for k, v in x.items()}
    if isinstance(x, list):
        return [_bez_base64(v) for v in x]
    if isinstance(x, str) and x.startswith("base64:"):
        return x[len("base64:"):]
    return x


def _parsuj_sekcje(slownik, wiadomosc, opis):
    from google.protobuf import json_format
    for sekcja, wartosc in (slownik or {}).items():
        try:
            json_format.ParseDict({sekcja: _bez_base64(wartosc)}, wiadomosc)
        except json_format.ParseError as e:
            print(f"  [UWAGA] {opis}.{sekcja}: {e} - pomijam nieznane pola")
            json_format.ParseDict({sekcja: _bez_base64(wartosc)}, wiadomosc, ignore_unknown_fields=True)


def wczytaj_profil(plik, pb):
    """Zwraca slownik: long, short, url, canned, ringtone, pozycja(lat,lon,alt), cfg(LocalConfig), mod(LocalModuleConfig)."""
    prof = dict(long=None, short=None, url=None, canned=None, ringtone=None, pozycja=None,
                cfg=pb.local.LocalConfig(), mod=pb.local.LocalModuleConfig())
    if plik.lower().endswith((".yaml", ".yml")):
        try:
            import yaml
        except ImportError:
            blad("Brak biblioteki pyyaml: pip install pyyaml")
        with open(plik, "r", encoding="utf-8") as fh:
            d = yaml.safe_load(fh) or {}
        prof.update(long=d.get("owner"), short=d.get("owner_short"), url=d.get("channel_url"),
                    canned=d.get("canned_messages"), ringtone=d.get("ringtone"))
        loc = d.get("location")
        if loc and "lat" in loc and "lon" in loc:
            prof["pozycja"] = (float(loc["lat"]), float(loc["lon"]), int(loc.get("alt", 0)))
        _parsuj_sekcje(d.get("config"), prof["cfg"], "config")
        _parsuj_sekcje(d.get("module_config"), prof["mod"], "module_config")
    else:
        if pb.client is None:
            blad("Ta wersja biblioteki meshtastic nie zna formatu .cfg (DeviceProfile) - zaktualizuj ja")
        dp = pb.client.DeviceProfile()
        with open(plik, "rb") as fh:
            dp.ParseFromString(fh.read())
        has = lambda n: dp.HasField(n) if n in dp.DESCRIPTOR.fields_by_name else False
        prof.update(long=dp.long_name if has("long_name") else None,
                    short=dp.short_name if has("short_name") else None,
                    url=dp.channel_url if has("channel_url") else None,
                    canned=dp.canned_messages if has("canned_messages") else None,
                    ringtone=dp.ringtone if has("ringtone") else None)
        if has("fixed_position"):
            fp = dp.fixed_position
            prof["pozycja"] = (fp.latitude_i / 1e7, fp.longitude_i / 1e7, fp.altitude)
        if has("config"):
            prof["cfg"].CopyFrom(dp.config)
        if has("module_config"):
            prof["mod"].CopyFrom(dp.module_config)
    return prof


def kanaly_z_url(url, pb):
    import base64
    frag = url.split("#", 1)[1] if "#" in url else url
    frag = frag.split("?", 1)[0]
    frag += "=" * (-len(frag) % 4)
    cs = pb.apponly.ChannelSet()
    cs.ParseFromString(base64.urlsafe_b64decode(frag))
    return cs


def lista_komend(prof, pb):
    """Kolejnosc: begin, wlasciciel, kanaly, config, moduly, pozycja, klucze (zmieniaja numer wezla) na koncu, commit.
    Wszystko w jednej transakcji - wezel zapisuje i restartuje sie dopiero po commit."""
    A, C = pb.admin.AdminMessage, pb.config.Config
    k = [(A(begin_edit_settings=True), 0, "begin_edit_settings")]

    if prof["long"] or prof["short"]:
        u = pb.mesh.User()
        if prof["long"]:
            u.long_name = prof["long"]
        if prof["short"]:
            u.short_name = prof["short"]
        k.append((A(set_owner=u), 0, f"owner: {prof['long']} / {prof['short']}"))

    cs = kanaly_z_url(prof["url"], pb) if prof["url"] else None
    if cs is not None:
        R = pb.channel.Channel.Role
        for i in range(8):
            ch = pb.channel.Channel(index=i)
            if i < len(cs.settings):
                ch.settings.CopyFrom(cs.settings[i])
                ch.role = R.PRIMARY if i == 0 else R.SECONDARY
                opis = f"kanal {i}: {cs.settings[i].name or '(domyslny)'}"
            else:
                ch.role = R.DISABLED
                opis = f"kanal {i}: wylaczony"
            k.append((A(set_channel=ch), 0, opis))

    # LoRa: z adresu kanalow, nadpisane sekcja config.lora
    lora = pb.config.Config.LoRaConfig()
    if cs is not None and cs.HasField("lora_config"):
        lora.CopyFrom(cs.lora_config)
    if prof["cfg"].HasField("lora"):
        lora.MergeFrom(prof["cfg"].lora)

    # device (rola) i lora (preset/region) ida na samym koncu - ich zmiana najbardziej obciaza swiezy wezel
    for f in prof["cfg"].DESCRIPTOR.fields:
        if not f.message_type or f.name in ("security", "lora", "device") or f.name not in C.DESCRIPTOR.fields_by_name:
            continue
        if prof["cfg"].HasField(f.name):
            k.append((A(set_config=C(**{f.name: getattr(prof["cfg"], f.name)})), 0, f"config.{f.name}"))
    M = pb.module.ModuleConfig
    for f in prof["mod"].DESCRIPTOR.fields:
        if f.message_type and prof["mod"].HasField(f.name) and f.name in M.DESCRIPTOR.fields_by_name:
            k.append((A(set_module_config=M(**{f.name: getattr(prof["mod"], f.name)})), 0, f"module_config.{f.name}"))

    pola = A.DESCRIPTOR.fields_by_name
    if prof["pozycja"] and "set_fixed_position" in pola:
        lat, lon, alt = prof["pozycja"]
        poz = pb.mesh.Position(latitude_i=int(round(lat * 1e7)), longitude_i=int(round(lon * 1e7)), altitude=int(alt))
        k.append((A(set_fixed_position=poz), 0, f"pozycja stala: {lat:.5f}, {lon:.5f}, {alt} m"))
    if prof["canned"] and "set_canned_message_module_messages" in pola:
        k.append((A(set_canned_message_module_messages=prof["canned"]), 0, "canned messages"))
    if prof["ringtone"] and "set_ringtone_message" in pola:
        k.append((A(set_ringtone_message=prof["ringtone"]), 0, "ringtone"))
    sec = prof["cfg"].security if prof["cfg"].HasField("security") else None
    zarzadzany = sec is not None and getattr(sec, "is_managed", False)
    if sec is not None:
        s1 = type(sec)()
        s1.CopyFrom(sec)
        if zarzadzany:
            s1.is_managed = False       # 'managed' blokuje lokalna administracje - wlaczany osobno na koncu
        k.append((A(set_config=C(security=s1)), 0, "config.security (klucze, klucze admina)"))
    if prof["cfg"].HasField("device"):
        k.append((A(set_config=C(device=prof["cfg"].device)), 0, "config.device (rola)"))
    if lora.ByteSize():
        k.append((A(set_config=C(lora=lora)), 0, "config.lora (region, preset)"))

    # CFG_RESTART: po tej ramce Pico czeka na restart wezla i laczy sie ponownie
    k.append((A(commit_edit_settings=True), CFG_RESTART if zarzadzany else 0,
              "commit_edit_settings (zapis + restart wezla)"))
    if zarzadzany:
        k.append((A(set_config=C(security=sec)), 0, "config.security is_managed=true (po restarcie, ostatnia)"))
    return k


def zbuduj_konfiguracje(plik, pokaz=False):
    """Buduje kontener MTCF: ramki ToRadio z miejscem na numer wezla (podmienia Pico)."""
    import random
    pb = importuj_protobufy()
    prof = wczytaj_profil(plik, pb)
    komendy = lista_komend(prof, pb)
    ramki = []
    for admin, flagi, opis in komendy:
        # want_response: wezel odpowiada potwierdzeniem (Routing) - Pico sprawdza kazda komende
        data = pb.mesh.Data(portnum=pb.port.PortNum.ADMIN_APP, payload=admin.SerializeToString(),
                            want_response=True)
        while True:
            pid = random.randint(0x01000000, 0x7FFFFFFF)
            pkt = pb.mesh.MeshPacket(to=CFG_PLACEHOLDER, id=pid, channel=0, decoded=data)
            b = pb.mesh.ToRadio(packet=pkt).SerializeToString()
            z_to = b"\x15" + struct.pack("<I", CFG_PLACEHOLDER)    # pole 'to' (fixed32, nr 2)
            z_id = b"\x35" + struct.pack("<I", pid)                # pole 'id' (fixed32, nr 6)
            if b.count(z_to) == 1 and b.count(z_id) == 1:
                break
        if len(b) > 510:
            blad(f"Ramka za duza ({len(b)} B): {opis}")
        ramki.append((flagi, b.index(z_to) + 1, b.index(z_id) + 1, struct.pack(">BBH", 0x94, 0xC3, len(b)) + b, opis))

    nazwa = (prof["long"] or "").encode("utf-8")[:39]
    out = struct.pack("<4sBBH40s", b"MTCF", 2, len(ramki), 0, nazwa)
    for flagi, to_off, id_off, r, opis in ramki:
        nazwa_k = opis.encode("utf-8")[:40]        # nazwa komendy do logu Pico
        out += struct.pack("<BBHHH", flagi, len(nazwa_k), to_off, id_off, len(r)) + r + nazwa_k
    print(f"Konfiguracja: {os.path.basename(plik)}  ({len(ramki)} komend, {len(out)} B)")
    OPIS.append(f"konfiguracja: {prof['long'] or '?'} / {prof['short'] or '?'}, plik {os.path.basename(plik)}")
    for i, (flagi, _, _, r, opis) in enumerate(ramki, 1):
        print(f"  {i:2d}. {opis}")
    if pokaz:
        print()
        for admin, _, opis in komendy:
            print(f"--- {opis}\n{admin}")
    return out


def zbuduj_pakiet(factory, tylko_factory=False, tylko_ota=False, reset_ustawien=False, ota_plik=None,
                  konfig=None, aktualizacja=False):
    if factory is None:                                         # tylko konfiguracja
        return spakuj([], 0, 0, konfig)
    nazwa = os.path.basename(factory)
    if not os.path.isfile(factory):
        blad(f"Nie ma pliku {factory}")
    if nazwa.endswith(".mt.json"):                             # same metadane - tylko dla --tylko-ota
        if not tylko_ota:
            blad("Sam plik .mt.json wystarczy tylko przy --tylko-ota; w innych trybach podaj firmware-*.bin")
        prog = nazwa[: -len(".mt.json")]
    elif nazwa.startswith("firmware-") and nazwa.endswith(".factory.bin"):
        prog = nazwa[: -len(".factory.bin")]                   # firmware-XXX
    elif nazwa.startswith("firmware-") and nazwa.endswith(".bin"):
        prog = nazwa[: -len(".bin")]                           # zwykly plik aktualizacji (sam program)
        aktualizacja = True
    else:
        blad("Plik musi miec nazwe firmware-*.factory.bin (albo firmware-*.bin do aktualizacji, albo *.mt.json przy --tylko-ota)")
    katalog = os.path.dirname(os.path.abspath(factory))
    meta_plik = os.path.join(katalog, prog + ".mt.json")
    if not os.path.isfile(meta_plik):
        blad(f"Brak pliku metadanych {meta_plik}")
    with open(meta_plik, "r", encoding="utf-8") as fh:
        meta = json.load(fh)

    mcu = meta.get("mcu")
    if mcu != "esp32s3":
        blad(f"Obslugiwany jest tylko ESP32-S3 (w metadanych: {mcu})")
    parts = meta.get("part") or []
    if not parts:
        blad("W .mt.json brak tablicy partycji ('part')")

    app_off = offset_partycji(parts, "ota_0", 0x10000)
    ota_off = offset_partycji(parts, "ota_1", 0x260000)
    fs_off = offset_partycji(parts, "spiffs", 0x300000)
    erase_end = max(int(p["offset"], 16) + int(p["size"], 16) for p in parts)
    flash_size = 1 << (erase_end - 1).bit_length()             # 8 MB / 16 MB

    obrazy = [
        ("factory", 0x0, os.path.join(katalog, nazwa)),
        ("ota", ota_off, ota_plik if ota_plik else os.path.join(katalog, f"mt-{mcu}-ota.bin")),
        ("littlefs", fs_off, os.path.join(katalog, "littlefs-" + prog[len("firmware-"):] + ".bin")),
    ]

    print(f"Metadane: {os.path.basename(meta_plik)}")
    print(f"Firmware: {meta.get('version')}  plytka: {meta.get('platformioTarget')}")
    if aktualizacja:
        # Zwykla aktualizacja: sam program w app0 + wyczyszczone otadata (start zawsze z app0).
        # Bez kasowania - ustawienia (LittleFS), klucze i NVS zostaja; plik konfiguracji niepotrzebny.
        od = [p for p in parts if p.get("name") == "otadata"]
        obrazy = [("app", app_off, os.path.join(katalog, prog + ".bin"))]
        if od:
            obrazy.append(("otadata-czysc", int(od[0]["offset"], 16), b"\xff" * int(od[0]["size"], 16)))
        print("Tryb: AKTUALIZACJA - sam program (firmware-*.bin) w app0, bez kasowania; ustawienia zostaja\n")
    elif tylko_factory:
        obrazy = obrazy[:1]
        print("Tryb: TYLKO factory.bin, bez pelnego kasowania (OTA i LittleFS z ustawieniami zostaja)\n")
    elif tylko_ota:
        obrazy = obrazy[1:2]
        print("Tryb: TYLKO OTA (partycja ota_1), bez kasowania - program i ustawienia zostaja\n")
    elif reset_ustawien:
        # Skasowany flash = same bajty 0xFF; po kompresji zajmuje kilkadziesiat bajtow
        pusto = []
        for p in parts:
            if p.get("subtype") in ("nvs", "ota"):
                pusto.append((p["name"] + "-czysc", int(p["offset"], 16), b"\xff" * int(p["size"], 16)))
        obrazy = pusto + [obrazy[2]]
        print("Tryb: RESET USTAWIEN - czyszczenie NVS i otadata + swiezy LittleFS (program i OTA zostaja)\n")
    else:
        print(f"Tryb: pelna instalacja, kasowanie 0x0 - 0x{erase_end:06X}, "
              f"flash {flash_size // (1024 * 1024)} MB\n")

    wpisy = []
    for nazwa_obr, off, plik in obrazy:
        if isinstance(plik, bytes):
            raw = plik
        else:
            if not os.path.isfile(plik):
                blad(f"Brak pliku {plik}")
            with open(plik, "rb") as fh:
                raw = fh.read()
        comp = zlib.compress(raw, 9)
        md5 = hashlib.md5(raw).digest()
        if off + len(raw) > erase_end:
            blad(f"{nazwa_obr} wychodzi poza flash (0x{off:X} + {len(raw)})")
        wpisy.append((nazwa_obr, off, raw, comp, md5))
        if isinstance(plik, bytes):
            OPIS.append(f"{nazwa_obr}: czyszczenie")
        elif nazwa_obr in ("factory", "app"):
            OPIS.append(f"firmware: {meta.get('version')} ({meta.get('platformioTarget')}), plik {os.path.basename(plik)}")
        elif nazwa_obr == "ota":
            OPIS.append(f"ota: {opis_aplikacji(raw, 0)}, plik {os.path.basename(plik)}")
        else:
            OPIS.append(f"{nazwa_obr}: plik {os.path.basename(plik)}")
        zrodlo = "(wypelnienie 0xFF - czyszczenie)" if isinstance(plik, bytes) else os.path.basename(plik)
        print(f"  {nazwa_obr:9s} @0x{off:06X}  {len(raw):>9} B -> {len(comp):>9} B  md5 {md5.hex()}")
        print(f"            plik: {zrodlo}")

    # Kontrola nakładania się obrazów
    zakresy = sorted((w[1], w[1] + len(w[2]), w[0]) for w in wpisy)
    for a, b in zip(zakresy, zakresy[1:]):
        if a[1] > b[0]:
            blad(f"Obrazy {a[2]} i {b[2]} nachodza na siebie")
    pelne = not (tylko_factory or tylko_ota or reset_ustawien or aktualizacja)
    OPIS.append("tryb: " + ("aktualizacja (sam program)" if aktualizacja else "tylko factory" if tylko_factory else "tylko OTA" if tylko_ota else
                            "reset ustawien" if reset_ustawien else "pelna instalacja (kasowanie calego flasha)"))
    return spakuj(wpisy, erase_end if pelne else 0, flash_size, konfig)


def spakuj(wpisy, erase_end, flash_size, konfig):
    """Format pakietu - musi odpowiadac PkgHdr / PkgEnt w szkicu Pico.
    eraseEnd = 0: bez pelnego kasowania. Wpis 'config' (offset 0xFFFFFFFF) nie jest flashowany."""
    wpisy = list(wpisy)
    if konfig is not None:
        wpisy.append(("config", 0xFFFFFFFF, konfig, konfig, hashlib.md5(konfig).digest()))
    if not wpisy:
        blad("Pakiet jest pusty")
    OPIS.append("pakiet utworzony: " + time.strftime("%Y-%m-%d %H:%M"))
    info = "\n".join(OPIS).encode("utf-8")[:1500]
    wpisy.append(("info", INFO_OFFSET, info, info, hashlib.md5(info).digest()))
    naglowek = struct.pack("<4sBBHII", b"MTPK", 1, len(wpisy), 0, erase_end, flash_size)
    pozycja = len(naglowek) + 64 * len(wpisy)
    tabela, dane = b"", b""
    for nazwa_obr, off, raw, comp, md5 in wpisy:
        tabela += struct.pack("<IIII16s32s", off, len(raw), len(comp), pozycja, md5,
                              nazwa_obr.encode()[:31])
        dane += comp
        pozycja += len(comp)
    pakiet = naglowek + tabela + dane
    print("\nOpis pakietu (widoczny na stronie Pico):")
    for l in OPIS:
        print(f"  {l}")
    roz = f"{len(pakiet) / 1024 / 1024:.2f} MB" if len(pakiet) >= 1024 * 1024 else f"{(len(pakiet) + 1023) // 1024} KB"
    print(f"\nPakiet: {len(pakiet)} B ({roz})")
    opisy = {"factory": "firmware (factory)", "app": "firmware (aktualizacja)", "ota": "OTA", "littlefs": "LittleFS",
             "config": "konfiguracje", "info": "opis pakietu"}
    segmenty, poz = [], len(naglowek) + 64 * len(wpisy)
    for nazwa_obr, off, raw, comp, md5 in wpisy:
        segmenty.append((opisy.get(nazwa_obr, nazwa_obr), poz, poz + len(comp)))
        poz += len(comp)
    segmenty[0] = (segmenty[0][0], 0, segmenty[0][2])        # naglowek liczony z pierwszym obrazem
    return pakiet, segmenty


def aktualizuj_pico(a):
    """Wysyla nowy program Pico (.bin). Pico usuwa pakiet ESP i pliki pomocnicze, sprawdza CRC,
    wgrywa program, restartuje sie i formatuje LittleFS od nowa (ustawienia zostaja)."""
    zapewnij_requests(a)
    if not os.path.isfile(a.pico_fw):
        blad(f"Nie ma pliku {a.pico_fw}")
    if not a.pico_fw.lower().endswith(".bin"):
        blad("Potrzebny plik .bin (nie .uf2). W Arduino IDE: Szkic -> Eksportuj skompilowany plik binarny.")
    with open(a.pico_fw, "rb") as fh:
        obraz = fh.read()
    if obraz[:4] == b"UF2\n":
        blad("To plik .uf2 (do wgrywania przez BOOTSEL) - do aktualizacji przez Wi-Fi potrzebny jest .bin (Szkic -> Eksportuj skompilowany plik binarny).")
    if not (64 * 1024 < len(obraz) < 1024 * 1024):
        blad(f"Nietypowy rozmiar programu: {len(obraz)} B (oczekiwane 64 KB - 1 MB)")
    mw = __import__("re").search(rb"PICOVER=([0-9A-Za-z.\-]{1,16})\x00", obraz)
    if not mw:
        blad("Ten plik to nie program Pico2W flasher (brak znacznika PICOVER) - Pico i tak by go odrzucilo. Sprawdz, czy to wlasciwy plik .bin.")
    print(f"Plik zawiera program Pico2W flasher w wersji {mw.group(1).decode()}")
    baza = f"http://{a.ip}"
    czekaj_na_pico(a.ip)
    try:
        st = requests.get(f"{baza}/status", headers={"X-Auth-Token": a.token}, timeout=10).text
        m = __import__("re").search(r"wersja Pico ([^ |]+)", st)
        print(f"Obecna wersja Pico: {m.group(1) if m else '?'}")
    except requests.exceptions.RequestException:
        pass
    print(f"Aktualizacja Pico: {os.path.basename(a.pico_fw)} ({len(obraz)} B)")
    print("UWAGA: pakiet ESP32-S3 zapisany na Pico zostanie usuniety, LittleFS sformatowany (ustawienia zostaja).")
    crc = zlib.crc32(obraz) & 0xFFFFFFFF
    wyslij_kawalkami(baza, a.token, obraz, crc, [("program Pico", 0, len(obraz))], cel="pico")
    print("Pico wgrywa nowy program i restartuje sie - czekam...")
    time.sleep(25)
    for _ in range(30):
        try:
            st = requests.get(f"{baza}/status", headers={"X-Auth-Token": a.token}, timeout=5).text
            m = __import__("re").search(r"wersja Pico ([^ |]+)", st)
            print(f"[SUKCES] Pico dziala, wersja {m.group(1) if m else '?'}")
            return
        except requests.exceptions.RequestException:
            time.sleep(3)
    blad("Pico nie odpowiada po aktualizacji. Sprawdz, czy jestes w jego sieci Wi-Fi (po restarcie moglo sie przelaczyc).")


LIMIT_PICO = 2900 * 1024          # ok. miejsca w LittleFS Pico na jeden pakiet (3 MB minus zapas)
ZNACZNIK_MTNOD = ("pelna", "aktualizacja", "pico")


def wersja_z_opisu():
    for l in OPIS:
        if l.startswith("firmware: "):
            return l.split()[1]
    return "?"


def podpis_mtnod(wpisy, token):
    import hmac
    tresc = "".join(f"{t}|{w[0]}|{w[2]}|{w[3]}" + (f"|{w[4]}" if len(w) > 4 and w[4] else "") + "\n"
                    for t in ZNACZNIK_MTNOD if t in wpisy for w in [wpisy[t]])
    return hmac.new(token.encode("utf-8"), tresc.encode("utf-8"), hashlib.sha256).hexdigest()


def przygotuj_serwer(a):
    """Buduje folder z plikami do aktualizacji przez internet i podpisany mtnod.txt."""
    folder = a.przygotuj_serwer
    if not a.firmware and not a.pico_fw:
        blad("--przygotuj-serwer wymaga -f firmware-*.bin / firmware-*.factory.bin albo --pico-fw PLIK.bin")
    if a.firmware and a.pico_fw:
        blad("Wezel i Pico przygotuj osobno: najpierw -f ... (wezel), a program Pico osobnym poleceniem --pico-fw ...")
    if a.firmware and not os.path.isfile(a.firmware):
        blad(f"Nie ma pliku firmware {a.firmware}")
    if a.config and not os.path.isfile(a.config):
        blad(f"Nie ma pliku konfiguracji {a.config}")
    if a.pico_fw and not os.path.isfile(a.pico_fw):
        blad(f"Nie ma pliku {a.pico_fw}")
    os.makedirs(folder, exist_ok=True)
    mt = os.path.join(folder, "mtnod.txt")
    wpisy = {}                                              # typ -> (wersja, plik, rozmiar, crc)
    if os.path.isfile(mt):
        with open(mt, "r", encoding="utf-8") as fh:
            for l in fh:
                l = l.strip()
                if not l or l.startswith("#") or l.startswith("podpis="):
                    continue
                p = l.split("|")
                if len(p) in (5, 6) and p[0] in ZNACZNIK_MTNOD:
                    wpisy[p[0]] = (p[1], p[2], p[3], p[4], p[5] if len(p) == 6 else "")
        print(f"Folder {folder} istnieje - podmieniam tylko podane czesci ({', '.join(wpisy) or 'brak wpisow'})\n")

    def zapisz(typ, nazwa, dane, wersja):
        with open(os.path.join(folder, nazwa), "wb") as fh:
            fh.write(dane)
        wpisy[typ] = (wersja, nazwa, str(len(dane)), f"{zlib.crc32(dane) & 0xFFFFFFFF:08X}", WERSJA)
        if typ != "pico" and len(dane) > LIMIT_PICO:
            print(f"[UWAGA] {nazwa}: {len(dane) // 1024} KB - wiecej niz miejsce na Pico (ok. {LIMIT_PICO // 1024} KB)!")
        elif typ != "pico" and len(dane) > LIMIT_PICO * 0.93:
            print(f"[UWAGA] {nazwa}: {len(dane) // 1024} KB - blisko limitu Pico ({LIMIT_PICO // 1024} KB)")

    if a.firmware:
        nazwa = os.path.basename(a.firmware)
        if nazwa.endswith(".factory.bin"):
            if not a.config:
                print("[UWAGA] Bez -c pelna instalacja zostawi wezel z ustawieniami fabrycznymi (bez regionu - nie nadaje!)")
            if a.config:
                przygotuj_venv(a.venv, aktualizuj=not a.bez_aktualizacji, pre=a.pre)
            OPIS.clear()
            konfig = zbuduj_konfiguracje(a.config) if a.config else None
            print("\n=== pelna.mtpk ===")
            pakiet, _ = zbuduj_pakiet(a.firmware, ota_plik=a.ota, konfig=konfig)
            zapisz("pelna", "pelna.mtpk", pakiet, wersja_z_opisu())
            upd = os.path.join(os.path.dirname(os.path.abspath(a.firmware)), nazwa[: -len(".factory.bin")] + ".bin")
            if os.path.isfile(upd):
                OPIS.clear()
                print("\n=== aktualizacja.mtpk ===")
                pakiet, _ = zbuduj_pakiet(upd)
                zapisz("aktualizacja", "aktualizacja.mtpk", pakiet, wersja_z_opisu())
            else:
                print(f"[UWAGA] Brak {os.path.basename(upd)} obok factory - aktualizacja.mtpk nie powstala")
        else:
            if a.config:
                print("[UWAGA] -c nie ma znaczenia dla zwyklej aktualizacji (ustawienia wezla zostaja)")
            OPIS.clear()
            print("\n=== aktualizacja.mtpk ===")
            pakiet, _ = zbuduj_pakiet(a.firmware)
            zapisz("aktualizacja", "aktualizacja.mtpk", pakiet, wersja_z_opisu())

    if a.pico_fw:
        if not a.pico_fw.lower().endswith(".bin"):
            blad("Potrzebny plik .bin (nie .uf2). W Arduino IDE: Szkic -> Eksportuj skompilowany plik binarny.")
        with open(a.pico_fw, "rb") as fh:
            obraz = fh.read()
        if obraz[:4] == b"UF2\n":
            blad("To plik .uf2 (do wgrywania przez BOOTSEL) - do aktualizacji przez Wi-Fi potrzebny jest .bin (Szkic -> Eksportuj skompilowany plik binarny).")
        if not (64 * 1024 < len(obraz) < 1024 * 1024):
            blad(f"Nietypowy rozmiar programu: {len(obraz)} B (oczekiwane 64 KB - 1 MB)")
        m = __import__("re").search(rb"PICOVER=([0-9A-Za-z.\-]{1,16})\x00", obraz)
        wer = m.group(1).decode() if m else "?"
        if not m:
            blad("Ten plik to nie program Pico2W flasher (brak znacznika PICOVER) - Pico odrzuci go po pobraniu.")
        zapisz("pico", "pico.bin", obraz, wer)

    with open(mt, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(f"# mtnod.txt - wygenerowany {time.strftime('%Y-%m-%d %H:%M')} przez wyslij_firmware.py {WERSJA}\n")
        fh.write("# typ|wersja|plik|rozmiar|crc32|wersja skryptu - pliki leza obok (mozna wpisac pelny adres https://...)\n")
        fh.write("# Podpis obejmuje wszystko poza nazwa pliku. Nie zmieniaj tych pol recznie.\n")
        fh.write("# Pico przyjmuje pakiet wezla tylko ze skryptu w tej samej wersji co program Pico.\n")
        for t in ZNACZNIK_MTNOD:
            if t in wpisy:
                w = wpisy[t]
                fh.write("|".join((t,) + tuple(w[:4]) + ((w[4],) if w[4] else ())) + "\n")
        fh.write(f"podpis={podpis_mtnod(wpisy, a.token)}\n")

    print(f"\nFolder: {os.path.abspath(folder)}")
    for t in ZNACZNIK_MTNOD:
        if t in wpisy:
            w, plik, roz, crc, skr = wpisy[t]
            print(f"  {plik:18s} {w:>12s} {int(roz) // 1024:>7} KB  CRC {crc}  skrypt {skr or '<0.24'}"
                  + ("  (Pico 0.24+ odrzuci - zbuduj ponownie)" if t != "pico" and skr != WERSJA else ""))
    print("  mtnod.txt          podpisany tokenem" + (" DOMYSLNYM - zmien token w Pico i tutaj!" if a.token == DEFAULT_TOKEN else ""))
    print("\nWgraj CALY folder na serwer WWW i wpisz w Pico adres mtnod.txt, np.:")
    print(f"  LoRa:  ADRES_URL:https://twoj-serwer/{os.path.basename(os.path.abspath(folder))}/mtnod.txt")
    print("Potem LoRa:  " + "  albo  ".join(f"POBIERZ_INTERNET:{t}" for t in ZNACZNIK_MTNOD if t in wpisy) +
          "   a po potwierdzeniu   AKTUALIZUJ_INTERNET")


def pobierz_logi(a):
    """Pobiera 3 logi z Pico (bez sprawdzania wersji - logi przydaja sie tez przy niezgodnosci)."""
    import urllib.request
    folder = a.logi or time.strftime("logi_pico_%Y%m%d_%H%M")
    os.makedirs(folder, exist_ok=True)
    pliki = (("/download", "pico_log.txt", "log wezla (RAM)"),
             ("/download_pico", "pico_zdarzenia.txt", "zdarzenia Pico"),
             ("/download_flash", "pico_log_flash.txt", "logi z flash"))
    bledy = 0
    for sciezka, nazwa, opis in pliki:
        cel = os.path.join(folder, nazwa)
        try:
            req = urllib.request.Request(f"http://{a.ip}{sciezka}", headers={"X-Auth-Token": a.token})
            with urllib.request.urlopen(req, timeout=60) as r, open(cel, "wb") as fh:
                n = 0
                while True:
                    blok = r.read(65536)
                    if not blok:
                        break
                    fh.write(blok); n += len(blok)
            print(f"  {opis:18s} {n // 1024:>6} KB  -> {cel}")
        except Exception as e:
            bledy += 1
            kod = getattr(e, "code", None)
            print(f"  {opis:18s} BLAD: {'zly token' if kod == 401 else 'Pico zajete (wgrywanie/odbior)' if kod == 503 else e}")
    if bledy == len(pliki):
        blad(f"Nie pobrano zadnego logu z {a.ip} (sprawdz polaczenie z Pico i --ip / --token)")
    print(f"\nLogi w folderze: {os.path.abspath(folder)}")


# Klucz w skrypcie -> pole formularza ustawien Pico (to samo co panel "Ustawienia")
USTAWIENIA = {
    "AP_SSID": ("ap_ssid", "nazwa sieci AP"), "AP_PWD": ("ap_pass", "haslo AP"),
    "STA_SSID": ("sta_ssid", "siec STA"), "STA_PWD": ("sta_pass", "haslo sieci STA"),
    "MODE_WIFI": ("sta", "tryb Wi-Fi AP/STA"), "KANAL": ("ch", "kanal komend LoRa (tylko prywatny, klucz 256-bit)"),
    "FILTR_NODE": ("senders", "dozwoleni nadawcy"), "ADRES_URL": ("url", "adres mtnod.txt"),
    "BRAK_ODPOWIEDZI": ("brak_odp", "minuty bez odpowiedzi wezla (0 = wylaczone)"),
    "PIN_RESETU": ("rst_pin", "GP pinu resetu wezla (-1 = brak)"), "RESET_STAN": ("rst_hi", "stan aktywny pinu resetu"),
    "RESET_CZAS": ("rst_sek", "czas impulsu resetu (s)"),
    "POWMAN_TIME": ("pm_time", "POWMAN pobudka: 0 / co h / godziny 6,18 / test 5m"),
    "POWMAN_CZUWANIE": ("pm_czuw", "POWMAN czuwanie po pobudce (min)"),
    "KOMENDA_WLACZ": ("cmd_on", "komenda: wlacz Wi-Fi"), "KOMENDA_USPIJ": ("cmd_sleep", "komenda: uspij (Wi-Fi off)"),
    "KOMENDA_AP_WLACZ": ("cmd_ap_on", "komenda: staly AP wlacz"), "KOMENDA_AP_WYLACZ": ("cmd_ap_off", "komenda: staly AP wylacz"),
    "KOMENDA_TEMP": ("cmd_temp", "komenda: temperatura"),
}


def ustawienie_z_tekstu(t):
    """'KLUCZ=wartosc' / 'KLUCZ:wartosc' -> (KLUCZ, pole_formularza, wartosc dla Pico)."""
    poz = [i for i in (t.find("="), t.find(":")) if i > 0]
    if not poz:
        blad(f"--ustaw {t}: brak '=' albo ':' (np. --ustaw BRAK_ODPOWIEDZI=30)")
    klucz, w = t[:min(poz)].strip().upper(), t[min(poz) + 1:].strip()
    if klucz in ("TOKEN", "HASLO"):
        blad("Token/haslo strony zmieniasz tylko w panelu Pico (Ustawienia) - skrypt go nie wysyla")
    if klucz not in USTAWIENIA:
        blad(f"--ustaw: nieznany klucz {klucz}. Dostepne: {', '.join(USTAWIENIA)}")
    pole = USTAWIENIA[klucz][0]
    if len(w) >= 2 and w[0] == w[-1] == '"':
        w = w[1:-1]                                       # "wartosc ze spacjami"
    if klucz in ("AP_PWD", "STA_PWD") and not w:
        blad(f"{klucz}: puste haslo nie zmienia ustawienia - podaj 8-63 znaki")
    if klucz == "MODE_WIFI":
        if w.upper() not in ("AP", "STA"):
            blad("MODE_WIFI: podaj AP albo STA")
        w = "1" if w.upper() == "STA" else "0"
    elif klucz == "RESET_STAN":
        m = {"WYSOKI": "1", "HIGH": "1", "1": "1", "NISKI": "0", "LOW": "0", "0": "0"}
        if w.upper() not in m:
            blad("RESET_STAN: podaj WYSOKI albo NISKI")
        w = m[w.upper()]
    elif klucz in ("KANAL", "BRAK_ODPOWIEDZI", "RESET_CZAS", "PIN_RESETU"):
        ww = w.upper().removeprefix("GP") if klucz == "PIN_RESETU" else w
        if not ww.lstrip("-").isdigit():
            blad(f"{klucz}: podaj liczbe")
        w = str(int(ww))
    elif klucz == "FILTR_NODE":
        if w.lower() in ("", "*", "wszyscy", "all"):
            w = ""
        else:
            ids = []
            for x in w.replace(";", ",").replace(" ", ",").split(","):
                x = x.strip().lower().removeprefix("!").removeprefix("0x")
                if not x:
                    continue
                if len(x) > 8 or any(c not in "0123456789abcdef" for c in x):
                    blad(f"FILTR_NODE: zly identyfikator '{x}' (hex, np. !ed819a48)")
                if x.zfill(8) not in ids:
                    ids.append(x.zfill(8))
            if len(ids) < 2 or len(ids) > 10:
                blad("FILTR_NODE: podaj 2-10 wezlow albo * = wszyscy")
            w = ",".join(ids)
    elif klucz.startswith("KOMENDA_"):
        w = w.upper()
    return klucz, pole, w


def ustawienia_pico(a):
    """--pokaz-ustawienia i --ustaw: odczyt / zmiana ustawien Pico przez /settings (jak panel strony)."""
    zapewnij_requests(a)
    sprawdz_wersje_pico(a.ip, a.token)
    baza, nagl = f"http://{a.ip}", {"X-Auth-Token": a.token}
    if a.ustaw:
        zmiany = [ustawienie_z_tekstu(t) for t in a.ustaw]
        dane = {pole: w for _, pole, w in zmiany}
        print("Zmiana ustawien Pico:")
        for k, pole, w in zmiany:
            pokaz = "(ukryte)" if k.endswith("_PWD") else (w if w != "" else "(puste)")
            print(f"  {k:18s} = {pokaz}")
        try:
            r = requests.post(f"{baza}/settings", headers=nagl, data=dane, timeout=15)
        except requests.exceptions.RequestException as e:
            blad(f"Pico nie odpowiada: {e}")
        if r.status_code != 200:
            blad(f"Pico odrzucilo zmiane ({r.status_code}): {r.text.strip()} - nic nie zmieniono")
        print("\nPico: " + r.text.strip().replace("\n", "\n      "))
        return
    try:
        r = requests.get(f"{baza}/settings", headers=nagl, timeout=10)
    except requests.exceptions.RequestException as e:
        blad(f"Pico nie odpowiada: {e}")
    v = dict(l.split("=", 1) for l in r.text.splitlines() if "=" in l)
    print(f"Ustawienia Pico {a.ip} (hasla i token nie sa pokazywane):")
    for k, (pole, opis) in USTAWIENIA.items():
        if pole not in v:
            continue
        w = v[pole]
        if k == "MODE_WIFI":
            w = "STA" if w == "1" else "AP"
        elif k == "RESET_STAN":
            w = "WYSOKI" if w == "1" else "NISKI"
        elif k == "FILTR_NODE" and not w:
            w = "* (wszyscy)"
        elif k == "BRAK_ODPOWIEDZI" and w == "0":
            w = "0 (wylaczone)"
        elif k == "PIN_RESETU":
            w = "-1 (brak)" if w == "-1" else f"GP{w}"
        print(f"  {k:18s} {w or '(puste)':30s} {opis}")
    print("\nZmiana: --ustaw KLUCZ=WARTOSC (mozna kilka razy w jednym poleceniu)")


def main():
    ap = argparse.ArgumentParser(
        description="Wysyla firmware Meshtastic (i opcjonalnie konfiguracje) do Pico 2 W, ktore wgrywa je do ESP32-S3.\nPomyslodawca projektu: Rafal Cichosz (rpc)",
        epilog="Przyklady uzycia, opis trybow i lista wymaganych plikow:  ./wyslij_firmware.py --przyklady",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-f", "--firmware",
                    help="firmware-*.bin = aktualizacja (sam program); firmware-*.factory.bin = pelna instalacja")
    ap.add_argument("-c", "--config", metavar="PLIK",
                    help="konfiguracja: .yaml (meshtastic --export-config) albo .cfg (kopia z Androida)")
    ap.add_argument("--tylko-konfiguracja", nargs="?", const=True, metavar="PLIK",
                    help="wyslij tylko konfiguracje do dzialajacego wezla (bez flashowania); plik mozna podac tu albo przez -c")
    ap.add_argument("--pokaz-konfiguracje", nargs="?", const=True, metavar="PLIK",
                    help="tylko pokaz, co zostanie wyslane (nic nie wysyla); plik mozna podac tu albo przez -c")
    ap.add_argument("--ip", default=DEFAULT_IP, help=f"adres Pico (domyslnie {DEFAULT_IP})")
    ap.add_argument("--token", default=DEFAULT_TOKEN, help="haslo/token Pico (to samo co do strony; domyslne MojeBezpieczneHaslo123)")
    ap.add_argument("--tylko-wyslij", action="store_true", help="tylko wyslij pakiet, bez flashowania")
    ap.add_argument("--tylko-factory", action="store_true",
                    help="factory.bin bez kasowania calego flasha (czysci NVS) - wymaga -c PLIK z konfiguracja")
    ap.add_argument("--tylko-ota", action="store_true",
                    help="tylko mt-<mcu>-ota.bin (albo --ota PLIK) w partycji ota_1, bez kasowania; "
                         "wymaga -f firmware-....mt.json (adres partycji) - firmware nie jest wgrywane")
    ap.add_argument("--ota", metavar="PLIK",
                    help="wlasny plik OTA zamiast domyslnego mt-<mcu>-ota.bin z folderu firmware")
    ap.add_argument("--reset-ustawien", action="store_true",
                    help="czysci NVS i otadata, wgrywa swiezy LittleFS, program zostaje - wymaga -c PLIK z konfiguracja")
    ap.add_argument("--przyklady", action="store_true", help="pokaz przyklady uzycia, tryby i wymagane pliki")
    ap.add_argument("--restart-pico", action="store_true", help="zrestartuj Pico (wymaga tokenu) i zakoncz")
    ap.add_argument("--pokaz-ustawienia", action="store_true", help="pokaz ustawienia Pico (bez hasel i tokenu)")
    ap.add_argument("--ustaw", action="append", metavar="KLUCZ=WARTOSC",
                    help="zmien ustawienie Pico (mozna kilka razy), np. --ustaw BRAK_ODPOWIEDZI=30; lista kluczy: --przyklady")
    ap.add_argument("--logi", nargs="?", const="", metavar="FOLDER",
                    help="pobierz z Pico 3 logi (log wezla z RAM, zdarzenia Pico, logi z flash) do FOLDER "
                         "(domyslnie nowy folder logi_pico_RRRRMMDD_GGMM) i zakoncz")
    ap.add_argument("--pico-fw", metavar="PLIK.bin",
                    help="aktualizacja programu Pico przez Wi-Fi (plik .bin z Arduino IDE: Szkic -> Eksportuj skompilowany plik)")
    ap.add_argument("--wymus", action="store_true", help="wyslij pakiet nawet jesli identyczny jest juz na Pico")
    ap.add_argument("--version", action="version", version=f"wyslij_firmware.py {WERSJA}")
    ap.add_argument("--zapisz", metavar="PLIK",
                    help="krok 1 z 2: tylko zbuduj pakiet i zapisz do pliku, nic nie wysylaj (np. gdy masz internet, "
                         "ale nie masz polaczenia z Pico) - przyklad na dole")
    ap.add_argument("--wyslij-pakiet", metavar="PLIK",
                    help="krok 2 z 2: wyslij pakiet zapisany wczesniej przez --zapisz (bez internetu i bez biblioteki "
                         "meshtastic, np. polaczony z Wi-Fi Pico) - przyklad na dole")
    ap.add_argument("--przygotuj-serwer", metavar="FOLDER",
                    help="zbuduj w FOLDER pliki do aktualizacji przez internet (pelna.mtpk, aktualizacja.mtpk, pico.bin) "
                         "i podpisany mtnod.txt - wgraj caly folder na swoj serwer WWW; nic nie wysyla do Pico (szczegoly: --przyklady)")
    ap.add_argument("--venv", default=DEFAULT_VENV,
                    help="srodowisko Pythona z biblioteka meshtastic (domyslnie ukryty podfolder .pico_meshtastic_env obok skryptu)")
    ap.add_argument("--pre", action="store_true",
                    help="instaluj takze wersje testowe meshtastic (alpha/beta) - dla firmware alpha/beta")
    ap.add_argument("--bez-aktualizacji", "--no-update", "--no_update", action="store_true",
                    help="nie aktualizuj meshtastic (np. bez internetu) - uzyj tego, co jest w venv")
    if len(sys.argv) == 1:
        ap.print_help()
        return
    a = ap.parse_args()
    for opcja in ("tylko_konfiguracja", "pokaz_konfiguracje"):   # --opcja PLIK = --opcja -c PLIK
        v = getattr(a, opcja)
        if isinstance(v, str):
            if a.config and a.config != v:
                ap.error(f"podano dwa rozne pliki konfiguracji (--{opcja.replace('_', '-')} PLIK i -c PLIK)")
            a.config = v
    if sys.stdout.isatty() and os.environ.get(ZNACZNIK_VENV) != "1":
        print("\033[2J\033[H", end="")                   # czysty ekran terminala
    print(f"wyslij_firmware.py wersja {WERSJA}  (pomyslodawca projektu: Rafal Cichosz, rpc)")

    if a.przyklady:
        print(__doc__.split("TRYBY", 1)[1].join(["TRYBY", ""]) if "TRYBY" in __doc__ else __doc__)
        return

    if a.logi is not None and (a.przygotuj_serwer or a.pico_fw or a.restart_pico or a.zapisz or a.tylko_wyslij
                               or a.pokaz_konfiguracje or a.ustaw or a.pokaz_ustawienia):
        blad("--logi laczy sie tylko z wgrywaniem wezla (-f, --tylko-konfiguracja, --wyslij-pakiet) albo dziala samo")

    if a.przygotuj_serwer:
        przygotuj_serwer(a)
        return

    if a.pico_fw:
        aktualizuj_pico(a)
        return

    if a.logi is not None and not (a.firmware or a.tylko_konfiguracja or a.wyslij_pakiet):
        pobierz_logi(a)                                   # same logi; z -f itd. - logi po wgraniu (na koncu)
        return

    if a.ustaw or a.pokaz_ustawienia:
        if a.firmware or a.pico_fw or a.przygotuj_serwer or a.wyslij_pakiet or a.tylko_konfiguracja or a.restart_pico:
            blad("--ustaw / --pokaz-ustawienia uzywaj osobno")
        ustawienia_pico(a)
        return

    if a.restart_pico:
        zapewnij_requests(a)
        try:
            r = requests.post(f"http://{a.ip}/pico_reboot", headers={"X-Auth-Token": a.token}, timeout=10)
        except requests.exceptions.RequestException as e:
            blad(f"Pico nie odpowiada: {e}")
        print(f"Pico: {r.status_code} {r.text}")
        return

    if a.wyslij_pakiet:
        if not os.path.isfile(a.wyslij_pakiet):
            blad(f"Nie ma pliku {a.wyslij_pakiet}")
        with open(a.wyslij_pakiet, "rb") as fh:
            pakiet = fh.read()
        if pakiet[:4] != b"MTPK":
            blad("To nie jest pakiet utworzony przez --zapisz")
        n = pakiet[5]
        segmenty, poz = [], 0
        print(f"Pakiet {a.wyslij_pakiet}: {len(pakiet)} B, {n} wpisow")
        opisy = {"factory": "firmware (factory)", "ota": "OTA", "littlefs": "LittleFS", "config": "konfiguracje",
                 "info": "opis pakietu"}
        for i in range(n):
            off, size, csize, dpos, md5, nm = struct.unpack_from("<IIII16s32s", pakiet, 16 + 64 * i)
            nm = nm.split(b"\0")[0].decode()
            print(f"  {nm:9s} {size:>9} B")
            segmenty.append((opisy.get(nm, nm), 0 if i == 0 else dpos, dpos + csize))
        tylko_cfg = all(sg[0] in ("konfiguracje", "opis pakietu") for sg in segmenty)
    else:
        pakiet = None
        tylko_cfg = a.tylko_konfiguracja

    if pakiet is None and sum([a.tylko_factory, a.tylko_ota, a.reset_ustawien]) > 1:
        blad("Opcje --tylko-factory, --tylko-ota i --reset-ustawien wykluczaja sie")
    aktualizacja = bool(a.firmware) and a.firmware.endswith(".bin") and not a.firmware.endswith(".factory.bin")
    if pakiet is None and a.firmware and a.firmware.endswith(".mt.json") and not a.tylko_ota:
        blad("Plik .mt.json (same metadane) mozna podac tylko z --tylko-ota")
    if pakiet is None and aktualizacja and (a.tylko_factory or a.tylko_ota or a.reset_ustawien or a.ota):
        blad("Plik firmware-*.bin to zwykla aktualizacja programu - opcje --tylko-*/--reset-ustawien/--ota\n"
             "        dotycza pliku firmware-*.factory.bin")
    if pakiet is None and (a.tylko_factory or a.reset_ustawien) and not a.config:
        blad(("--tylko-factory czysci NVS" if a.tylko_factory else "--reset-ustawien przywraca ustawienia fabryczne") +
             " - podaj plik konfiguracji: -c node.yaml (albo .cfg)")
    if pakiet is None:
        if a.ota and not os.path.isfile(a.ota):
            blad(f"Nie ma pliku OTA {a.ota}")
        if a.ota and (a.tylko_factory or a.reset_ustawien):
            print("[UWAGA] --ota nie ma znaczenia w tym trybie (OTA nie jest wgrywane)")
        if a.tylko_konfiguracja and not a.config:
            blad("--tylko-konfiguracja wymaga -c PLIK")
        if a.tylko_ota and not a.firmware:
            blad("--tylko-ota potrzebuje ukladu partycji plytki (adres OTA): podaj -f z plikiem metadanych\n"
                 "        firmware-....mt.json (albo firmware-...factory.bin, obok ktorego lezy). Firmware NIE jest wgrywane.\n"
                 "        Przyklad: --tylko-ota -f firmware-heltec-v4-2.8.3.xxx.mt.json --ota mt-esp32s3-ota.bin")
        if not a.tylko_konfiguracja and not a.firmware and not a.pokaz_konfiguracje:
            blad("Podaj -f firmware-*.factory.bin (albo --tylko-konfiguracja -c PLIK). Wszystkie opcje: -h")
        if a.config and not os.path.isfile(a.config):
            blad(f"Nie ma pliku konfiguracji {a.config}")
        if a.firmware and not a.tylko_konfiguracja and not os.path.isfile(a.firmware):
            blad(f"Nie ma pliku firmware {a.firmware}")

        # Konfiguracja wymaga protobufow meshtastic: zawsze najnowsza wersja z venv
        if a.config or requests is None:
            przygotuj_venv(a.venv, aktualizuj=not a.bez_aktualizacji, pre=a.pre)
        if requests is None and not (a.zapisz or a.pokaz_konfiguracje):
            zapewnij_requests(a)

        konfig = zbuduj_konfiguracje(a.config, pokaz=a.pokaz_konfiguracje) if a.config else None
        if a.pokaz_konfiguracje:
            return
        pakiet, segmenty = zbuduj_pakiet(None if a.tylko_konfiguracja else a.firmware, a.tylko_factory,
                                         a.tylko_ota, a.reset_ustawien, a.ota, konfig)
    elif requests is None:
        zapewnij_requests(a)

    if a.zapisz:
        with open(a.zapisz, "wb") as fh:
            fh.write(pakiet)
        print(f"Zapisano {a.zapisz}")
        return

    baza = f"http://{a.ip}"
    czekaj_na_pico(a.ip)
    sprawdz_wersje_pico(a.ip, a.token)
    crc_juz = ""
    try:
        crc_juz = requests.get(f"{baza}/pkg_crc", headers={"X-Auth-Token": a.token}, timeout=10).text.strip().upper()
    except requests.exceptions.RequestException:
        pass
    # chwila na ustabilizowanie Wi-Fi (np. po przelaczeniu sieci) - pierwsze polaczenie czesto sie nie udaje
    for i in range(10, 0, -1):
        print(f"\rStart wysylania za {i:2d} s (Ctrl+C = przerwij) ", end="", flush=True)
        time.sleep(1)
    print("\r" + " " * 45 + "\r", end="")
    crc = zlib.crc32(pakiet) & 0xFFFFFFFF
    if crc_juz.lstrip("0") == f"{crc:08X}".lstrip("0") and not a.wymus:
        print(f"\n[1/2] Identyczny pakiet (CRC32 {crc:08X}) jest juz na Pico - pomijam wysylanie (--wymus wysyla mimo to).")
    else:
        print(f"\n[1/2] Wysylanie pakietu do {baza} (CRC32 {crc:08X})...")
        wyslij_kawalkami(baza, a.token, pakiet, crc, segmenty)

    if tylko_cfg:
        czas_info = "Konfiguracja trwa ok. 1 minute (z restartem wezla)."
    elif all(sg[0] in ("OTA", "opis pakietu") for sg in segmenty):
        czas_info = "Wgranie samego OTA trwa ok. 30 s (zapis, sprawdzenie, restart wezla)."
    elif any(sg[0] == "firmware (aktualizacja)" for sg in segmenty):
        czas_info = "Aktualizacja programu trwa ok. 1 minute (zapis, sprawdzenie, restart wezla)."
    else:
        czas_info = UWAGA_CZAS
    if a.tylko_wyslij:
        print("Pakiet zapisany na Pico. Flashowanie mozna uruchomic przyciskiem na stronie.")
        print(czas_info)
        return

    print("\n[2/2] " + ("Wysylanie konfiguracji do wezla..." if tylko_cfg else "Flashowanie ESP32-S3..."))
    print("      " + czas_info.replace("\n", "\n      "))
    r = requests.post(f"{baza}/fw_flash", timeout=30,
                      headers={"X-Auth-Token": a.token, "X-Time": time.strftime("%Y-%m-%d %H:%M")})
    if r.status_code != 200:
        blad(f"Nie mozna uruchomic flashowania ({r.status_code}): {r.text}")

    ostatni = ""
    t0 = time.time()
    while time.time() - t0 < 20 * 60:
        time.sleep(2)
        try:
            stan = requests.get(f"{baza}/fw_status", headers={"X-Auth-Token": a.token}, timeout=10).text.strip()
        except requests.exceptions.RequestException:
            print("      (brak odpowiedzi Pico, ponawiam...)")
            continue
        if stan != ostatni:
            czesci = stan.split("|", 2)
            if len(czesci) == 3:
                print(f"      [{czesci[1]:>3}%] {czesci[2]}")
            ostatni = stan
        if stan.startswith("OK|"):
            print(f"\n[SUKCES] {czesci[2] if len(czesci) == 3 else 'Zakonczone'} ({time.time() - t0:.0f} s).")
            if a.logi is not None:
                print("\nCzekam 30 s na logi startu wezla, potem pobieram logi...")
                time.sleep(30)
                pobierz_logi(a)
            else:
                print(f"Logi startu: {baza}/  (przycisk 'Pobierz log' albo opcja --logi)")
            return
        if stan.startswith("BLAD|"):
            if a.logi is not None:
                print("\nPobieram logi do diagnozy...")
                pobierz_logi(a)
            if "konfiguracji" in stan:
                blad("Firmware wgrany poprawnie, ale KONFIGURACJA nie przeszla.\n"
                     "        Mozna ja powtorzyc przyciskiem 'Wgraj konfiguracje' na stronie albo:\n"
                     "        --wyslij-pakiet / --tylko-konfiguracja -c PLIK. Szczegoly w logu Pico.")
            blad("Flashowanie nie powiodlo sie. Szczegoly w logu Pico (Pobierz log).")
    if a.logi is not None:
        pobierz_logi(a)
    blad("Przekroczony czas oczekiwania na zakonczenie flashowania.")


if __name__ == "__main__":
    main()
)PYSKRYPT";
