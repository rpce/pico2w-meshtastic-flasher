// =========================================================================
// Pomyslodawca projektu: Rafal Cichosz (rpc)
// Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
// Pico 2 W jako host USB dla wezlow Meshtastic ESP32-S3 (np. Heltec V4, XIAO ESP32-S3)
//   - podgląd logów w przeglądarce (Wi-Fi AP)
//   - reset / bootloader przez DTR/RTS
//   - test bootloadera (SYNC, typ układu, MAC)
//   - FLASHOWANIE: pakiet (factory + ota + littlefs) wysłany skryptem
//     wyslij_firmware.py, zapis do LittleFS z CRC, pełne kasowanie,
//     zapis skompresowany (protokół esptool), weryfikacja MD5 w ESP32
//
// Arduino IDE (rdzeń Arduino-Pico / earlephilhower):
//   Płytka:     Raspberry Pi Pico 2W
//   USB Stack:  Adafruit TinyUSB Host (native)
//   Flash Size: 4MB (Sketch: 1MB, FS: 3MB)
//
// Wi-Fi "PicoTest" (hasło: test12345) -> http://192.168.4.1/
// =========================================================================
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Adafruit_TinyUSB.h>
#include <pico/mutex.h>
#include <hardware/watchdog.h>
#include <hardware/structs/watchdog.h>
#include <hardware/structs/usb.h>
#include <hardware/structs/powman.h>
#include <hardware/gpio.h>
#include <pico/unique_id.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <DNSServer.h>
#include <LEAmDNS.h>
#include <PicoOTA.h>
#include <HTTPClient.h>
#include <hardware/adc.h>
#include <pico/cyw43_arch.h>
#include <hardware/sync.h>
#include <W5500lwIP.h>                             // Ethernet W5500 (lwIP, wbudowane w arduino-pico)
#include <W5100lwIP.h>                             // Ethernet W5100S (WIZnet Ethernet HAT)
#ifndef CYW43_WL_GPIO_VBUS_PIN
#define CYW43_WL_GPIO_VBUS_PIN 2                      // Pico W / Pico 2 W: wykrywanie VBUS na GPIO2 ukladu CYW43
#endif

// ---- Typy zdefiniowane na górze (wymóg Arduino IDE) ----
struct Step { uint8_t lines; uint16_t delayAfter; };   // lines: bit0 = DTR, bit1 = RTS

// Pakiet firmware (format tworzony przez wyslij_firmware.py)
struct __attribute__((packed)) PkgHdr {
  char     magic[4];     // "MTPK"
  uint8_t  version;      // 1
  uint8_t  count;        // liczba obrazów
  uint16_t reserved;
  uint32_t eraseEnd;     // koniec obszaru do skasowania (z tablicy partycji)
  uint32_t flashSize;    // rozmiar flasha dla SPI_SET_PARAMS
};
struct __attribute__((packed)) PkgEnt {
  uint32_t offset;       // adres w flashu ESP32
  uint32_t size;         // rozmiar po rozpakowaniu
  uint32_t csize;        // rozmiar skompresowany (zlib)
  uint32_t dataPos;      // położenie danych w pliku pakietu
  uint8_t  md5[16];      // MD5 danych nieskompresowanych
  char     name[32];
};

// ---- Konfiguracja ----
// Ustawienia domyslne (przywracane przytrzymaniem BOOTSEL przez 30 s)
const char*  DEF_AP_SSID  = "FlasherMeshtasticPico2W";
const char*  DEF_AP_PASS  = "12345678";
const char*  DEF_TOKEN    = "MojeBezpieczneHaslo123";
const char*  SETTINGS_PATH = "/settings.txt";
const char*  MDNS_NAME    = "pico-flasher";         // w trybie STA: http://pico-flasher.local
// ch: numer kanalu Meshtastic z komendami; senders: dozwoleni nadawcy (hex, po przecinku; puste = wszyscy)
// cmd[]: nazwy komend LoRa (zmieniane na stronie): wlacz, uspij, AP wlacz, AP wylacz, temperatura
static const char* const CMD_KEYS[5] = { "cmd_on", "cmd_sleep", "cmd_ap_on", "cmd_ap_off", "cmd_temp" };
static const char* const CMD_DEF[5]  = { "WLACZ_PICO", "USPIJ_PICO", "WLACZ_TRYB_AP", "WYLACZ_TRYB_AP", "TEMP_PICO" };
// brakOdp: minuty ciszy wezla do odzyskiwania (0 = wylaczone); rstPin: GPIO impulsu resetu wezla (-1 = brak), rstHigh: aktywny stan wysoki
struct Settings { String picoId, apSsid, apPass, tokSol, tokHash, staSsid, staPass, senders, url; bool tokSlabe; bool sta; int ch; bool logOn; String cmd[5];
                  int brakOdp, rstPin, rstSek; bool rstHigh;
                  String pmTime; int pmCzuw;
                  float vKor, vProg;
                  String ipAllow;
                  uint32_t dgDo, mtCzas;
                  String netMode; int ethMod; bool logApiAuto; bool strona; int hop; bool zasLora;                  // tryb sieci: "wifi" (domyslnie) albo "brak" (Wi-Fi wylaczone; awaryjnie LoRa)
                  String staIp, staMask, staGw, staDns;
                  String ethIp, ethMask, ethGw, ethDns;
                  String fwOst; } cfg;                 // fwOst: ostatnia znana wersja firmware wezla (anti-rollback, gdy API nie odpowiada)   // staly adres w trybie STA (staIp puste = DHCP)     // dgDo: downgrade dozwolony do (unix); mtCzas: czas ostatniego przyjetego mtnod.txt              // ipAllow: dozwolone adresy klientow w trybie STA (puste = wszystkie)          // VSYS: korekta pomiaru (V) i prog ostrzezenia (V, 0 = wylaczony)   // POWMAN: "0" / "12" (co 12 h) / "6,18" (o godzinach) / "5m" (test); czuwanie min
struct Sesja { String id, ip, kto, k; uint32_t last; uint64_t lic, okno; };   // k: klucz sesji (hex); lic/okno: liczniki podpisanych zadan          // sesja logowania przegladarki (od 0.29)
struct Proba { String ip; uint8_t n, nr; uint32_t blokDo; };   // bledne hasla z danego IP
const char* PICO_BIN = "pico.bin";                 // aktualizacja programu Pico (przed restartem)
static const char* upPath = "/fw.pkg";             // dokad trafia odbierany plik (pakiet albo aktualizacja Pico)
// Stan Wi-Fi: 0 = wlaczone wg ustawien, 1 = uspione (Wi-Fi wylaczone), 2 = staly AP dla wezlow (UDP)
static volatile int  wifiState = 0;
static volatile int  wifiReq = -1;                 // zadanie zmiany stanu Wi-Fi (z komendy LoRa), wykonuje rdzen 0
static uint32_t lastStaTry = 0;
static uint8_t  staFails = 0;                      // nieudane proby STA od startu (szybkie ponowienia na poczatku)
static const uint32_t STA_RETRY_MS = 4UL * 3600UL * 1000UL;
// Komenda tekstowa z LoRa (kanal prywatny): wykrywana w logu, odpowiedz wysyla rdzen 1
static volatile uint32_t txtId = 0, txtFrom = 0; static volatile int txtCh = -1;
// Kolejka wiadomosci LoRa (odpowiedzi, raporty): wysyla rdzen 1, najwczesniejsza pierwsza, min. 5 s odstepu
struct LoraMsg { char txt[201]; uint32_t at, born; bool needName; };
struct Sha256 { uint32_t h[8]; uint8_t buf[64]; uint64_t len; size_t n; };   // podpis mtnod.txt (u gory - prototypy Arduino)
// POWMAN_TIME: "0" = bez timera (tylko pin), "12" = co 12 h (1-168), "6,18" = o tych godzinach (0-23, max 6;
// jedna godzina z przecinkiem, np. "6,"), "5m" = co 5 minut (1-1440, do testow)
static bool pmTimeOk(const String& t) {
  if (t == "0") return true;
  if (!t.length() || t.length() > 20) return false;
  if (t.endsWith("m")) { String n = t.substring(0, t.length() - 1); for (char c : n) if (!isdigit(c)) return false;
                         return n.length() && n.toInt() >= 5 && n.toInt() <= 1440; }   // min. 5 min
  if (t.indexOf(',') < 0) { for (char c : t) if (!isdigit(c)) return false; return t.toInt() >= 1 && t.toInt() <= 168; }
  int cnt = 0, i = 0;
  while (i < (int)t.length()) {
    int j = t.indexOf(',', i); if (j < 0) j = t.length();
    String h = t.substring(i, j);
    if (h.length()) { for (char c : h) if (!isdigit(c)) return false; if (h.toInt() > 23) return false; cnt++; }
    i = j + 1;
  }
  return cnt >= 1 && cnt <= 6;
}
static bool gpioOk(int p) { return p == -1 || (p >= 0 && p <= 22) || (p >= 26 && p <= 28); }   // GP23-25, 29: modul Wi-Fi
// Modul Ethernet (od 0.40 do wyboru w ustawieniach):
//  0 = W5500-mini na SPI1:              SCK GP10, MOSI GP11, MISO GP12, CS GP13, RST GP9,  INT GP8
//  1 = WIZnet Ethernet HAT (W5100S) SPI0: SCK GP18, MOSI GP19, MISO GP16, CS GP17, RST GP20, INT GP21
struct EthPiny { const char* nazwa; int sck, mosi, miso, cs, rst, irq, od, doo; };
static const EthPiny ETH_MOD[2] = { { "W5500-mini (SPI1: GP8-13)", 10, 11, 12, 13, 9, 8, 8, 13 },
                                    { "WIZnet Ethernet HAT W5100S (SPI0: GP16-21)", 18, 19, 16, 17, 20, 21, 16, 21 } };
static bool ethPinZajety(int p, int mod) { return p >= ETH_MOD[mod].od && p <= ETH_MOD[mod].doo; }
static Wiznet5500lwIP* e55 = nullptr;
static Wiznet5100lwIP* e51 = nullptr;
struct EthSel {                                       // jeden interfejs dla obu ukladow
  bool begin() { return e55 ? e55->begin() : e51 ? e51->begin() : false; }
  void config(IPAddress a, IPAddress b, IPAddress c, IPAddress d) { if (e55) e55->config(a, b, c, d); else if (e51) e51->config(a, b, c, d); }
  bool connected() { return e55 ? e55->connected() : e51 ? e51->connected() : false; }
  IPAddress localIP() { return e55 ? e55->localIP() : e51 ? e51->localIP() : IPAddress(); }
};
static EthSel eth;
#define ethP (e55 || e51)
static bool ethInit = false;                        // modul Ethernet wykryty i uruchomiony
// Port wezla TCP 4403: przezroczyste API Meshtastic (aplikacja / CLI --host / VCOM) przez USB wezla.
// Bufory miedzy rdzeniami: rdzen 0 (siec) <-> rdzen 1 (USB). Jeden producent i jeden odbiorca na bufor.
static const uint32_t RING_MASK = 16383;
struct Ring {                                       // metody zamiast funkcji - bez auto-prototypow Arduino
  uint8_t b[16384]; volatile uint32_t h = 0, t = 0;
  uint32_t wolne() { return RING_MASK - ((h - t) & RING_MASK); }
  uint32_t put(const uint8_t* d, uint32_t n) {
    uint32_t hh = h, f = wolne(); if (n > f) n = f;
    for (uint32_t i = 0; i < n; i++) b[(hh + i) & RING_MASK] = d[i];
    __dmb(); h = (hh + n) & RING_MASK; return n;
  }
  uint32_t get(uint8_t* d, uint32_t mx) {
    uint32_t hh = h; __dmb(); uint32_t tt = t, n = (hh - tt) & RING_MASK; if (n > mx) n = mx;
    for (uint32_t i = 0; i < n; i++) d[i] = b[(tt + i) & RING_MASK];
    __dmb(); t = (tt + n) & RING_MASK; return n;
  }
};
static Ring brUp, brDown;                           // brUp: klient -> wezel, brDown: wezel -> klient
static volatile bool brActive = false;              // klient polaczony (rdzen 1 przekazuje dane)
static uint64_t brUntil = 0;                        // port wlaczony do (sekundy od startu Pico), 0 = wylaczony
static volatile uint32_t brLost = 0;                // bajty utracone (klient nie nadazal)
WiFiServer brSrv(4403);
static LoraMsg loraQ[8]; static volatile uint8_t loraQn = 0;
auto_init_mutex(loraQMtx);
// Komenda odebrana na rdzeniu 1 (tekst calej wiadomosci), wykonywana na rdzeniu 0 (ustawienia, Wi-Fi)
static char cmdBuf[201]; static volatile uint32_t cmdFrom = 0; static volatile bool cmdPending = false;
static char nodeLong[41] = "", nodeShort[16] = "", nodeFw[40] = "";   // nazwa i wersja firmware wezla (API)  // nazwa wlasnego wezla z node_info (API)
static const uint32_t BOOT_REPORT_MS = 120000;     // raport o wlaczeniu zasilania: 2 min po polaczeniu z wezlem
static volatile bool bootReportWanted = false;     // start Pico z wlaczenia zasilania
static volatile bool resetNodeAfterUpdate = false; // po aktualizacji programu Pico: jednorazowy reset wezla (RTS)
static String dlPiasek;                            // piasek z komendy POBIERZ_INTERNET (czesc klucza podpisu mtnod.txt)
static int  dlReq = -1;                             // pobieranie z internetu: 0 pelna, 1 aktualizacja, 2 pico (rdzen 0)
static bool updReq = false;                         // AKTUALIZUJ_INTERNET (rdzen 0)
static uint32_t pendingFlashAt = 0;                 // start wgrywania po wyslaniu odpowiedzi LoRa
bool urlOk(const String& u);
void netService();
static uint32_t crc32Calc(uint32_t crc, const uint8_t* d, size_t n);
static const uint32_t LORA_REPORT_UPD_MS = 30000;  // raport po samej aktualizacji: wezel dopiero wystartowal
static const uint32_t LORA_REPORT_CFG_MS = 15000;  // raport po konfiguracji: wezel juz po restarcie i czekaniu
static bool staActive = false;                     // polaczony jako klient z siecia (STA)
String myIp();
bool ethUp();
void pbkdf2(const char* pw, size_t pl, const uint8_t* salt, size_t sl, uint8_t out[32]);
static void hmacSha256(const char* key, size_t kl, const char* msg, size_t ml, uint8_t out[32]);
// Jawne deklaracje (nie polegamy na automatycznych prototypach Arduino)
bool saveSettings();
bool apiProbe();
static void shaInit(Sha256& s);
static void shaUpd(Sha256& s, const uint8_t* d, size_t n);
static void shaFin(Sha256& s, uint8_t out[32]);
bool pmActive();
void rstPinIdle();
bool pmBootselOff();
void plannedReboot();
void pmEarlyBoot();
void pmLoadState();
static void pmSetFlag(bool on);
void pmExtend();
bool pmBusy();
void loraCmdService();
String nodeRecover(uint32_t cisza, const char* pref);
String hexStr(const uint8_t* b, size_t n);
bool hexBytes(const String& h, uint8_t* out, size_t n);
static bool ctEq(const String& a, const String& b);
static bool cfgxOdszyfruj(File& f, uint32_t pos, uint32_t len, String& err);
bool sendConfigAny(File& f, const PkgEnt& en, bool afterFlash);
void tokUstaw(const String& haslo);
void picoIdNowy();
static String obciazenieTxt();
bool saveSettings();
String ethTxt();
bool ethStart();
bool netEth();
bool netWifi();
void bridgeService();
String brTxt();
bool brCzas(String v, uint32_t& sek);
void brWlacz(uint32_t sek, const String& kto);
String vsysTxt();
bool domyslneHasla();
bool staIpOk(const String& ip, const String& mk, const String& gw, const String& dn);
bool ipLoraParse(String v, String& ip, String& mk, String& gw, String& dn);
String ipTxt(const String& ip, const String& mk, const String& gw, const String& dn);
void sesjeKasuj();
void vsysService(bool start = false);
bool apiConnect();
void parseCmdLine(const char* l);
void heltecText(const uint8_t* in, int n);
void loraCmd(const char* msg, uint32_t from, uint32_t id);
void queueLora(const String& txt, uint32_t delayMs, bool needName = false);
bool senderAllowed(uint32_t from);
const char*  PKG_PATH     = "/fw.pkg";
const char*  PKG_OK_PATH  = "/fw.ok";     // znacznik: pakiet kompletny i zweryfikowany
const char*  DL_INFO_PATH = "/dl.info";   // co pobrano z internetu: typ|wersja|rozmiar|crc
const char*  PICO_DL_PATH = "/pico_dl.bin";  // program Pico pobrany z internetu (czeka na AKTUALIZUJ_INTERNET)
const char*  PICO_LORA_FLAG = "/pico_lora.txt";  // aktualizacja Pico z LoRa: po restarcie potwierdz przez LoRa

// ---- Komunikacja między rdzeniami ----
// reqCmd: 1 reset RTS, 2 bootloader, 4 restart hosta, 5 test bootloadera, 6 flashowanie, 7 konfiguracja, 8 test pinu resetu
static volatile uint8_t  reqCmd = 0;
static volatile uint32_t core1Beat = 0;

// Stan flashowania (zapisywany przez rdzeń 1, czytany przez stronę)
// fwState: 0 brak, 1 trwa, 2 OK, 3 błąd
static volatile uint8_t fwState = 0;
static volatile int     fwPct = 0;
static char             fwMsg[96] = "brak";
static String           upIp;                // adres, ktory rozpoczal odbior (/fw_begin)
static bool             upActive = false;   // trwa przyjmowanie pakietu firmware
static uint32_t         upLastMs = 0;       // ostatni fragment (przerwany odbior wygasa po 60 s)
static uint32_t upTotal = 0, upTotalCrc = 0, upNext = 0;

// Tryb API Meshtastic (ramki 0x94 0xC3) - tylko na czas wysylania konfiguracji
static const uint32_t   CFG_OFFSET = 0xFFFFFFFF;   // wpis pakietu z konfiguracja (nie flashowany)
static const uint32_t   INFO_OFFSET = 0xFFFFFFFE;  // wpis z opisem pakietu (nie flashowany)
static const uint32_t   CFGX_OFFSET = 0xFFFFFFFD;  // konfiguracja ZASZYFROWANA (pakiet z serwera, od 0.40)
const char* CFGX_KEY_PATH = "/cfgx.key";            // klucz do niej (z hasla i piasku przy pobraniu)
const char* CFGX_DEC_PATH = "/cfg.dec";             // odszyfrowana konfiguracja - tylko na czas wysylania
const char* PKG_INFO_PATH  = "/pkg.info";          // opis pakietu zapisanego na Pico
const char* NODE_INFO_PATH = "/node.info";         // co i kiedy wgrano do wezla
static char opTime[24] = "";
static String pkgMode = "";                        // tryb pakietu z opisu ("aktualizacja (sam program)", "pelna instalacja...")                       // czas zlecenia (z komputera / przegladarki)
static volatile uint32_t ackId = 0;                // potwierdzenie komendy (Routing.request_id)
static volatile uint8_t  ackErr = 0;
static const uint8_t API_DISC[] = { 0x94, 0xC3, 0x00, 0x02, 0x20, 0x01 };   // ToRadio.disconnect
static const uint8_t API_WANTCFG[] = { 0x94, 0xC3, 0x00, 0x03, 0x18, 0xB9, 0x60 };   // want_config_id = 12345
static volatile bool     apiMode = false;
static volatile uint32_t apiNodeNum = 0;    // z my_info (FromRadio)
// security z FromRadio.config: 0 nieznane, 1 debug_log_api wlaczone, 2 wylaczone (kopia surowa - do ponownego wyslania z flaga)
static volatile uint8_t secStan = 0; static uint8_t secRaw[300]; static uint16_t secLen = 0; static bool secManaged = false, secKlucz = false;
static volatile int8_t chPskLen[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };   // dlugosc klucza kanalow wezla (-1 = nieznana)
static volatile bool     apiConfigDone = false;
static volatile uint32_t apiFrames = 0;
static volatile uint32_t logNodeNum = 0;    // z linii logu "Use nodenum 0x..."
static volatile uint32_t lastRxMs = 0;
static volatile uint32_t lastApiUse = 0;           // ostatnie polaczenie API z wezlem
static uint32_t pmApiAgoBoot = 0xFFFFFFFFUL;          // POWMAN: sekundy od ostatniego kontaktu API w chwili pobudki
// Sekundy od ostatniego kontaktu API z wezlem (takze sprzed snu POWMAN); 0xFFFFFFFF = nieznane
uint32_t apiAgoSec() {
  if (lastApiUse) return (millis() - lastApiUse) / 1000;
  if (pmApiAgoBoot != 0xFFFFFFFFUL) return pmApiAgoBoot + millis() / 1000;
  return 0xFFFFFFFFUL;
}
// ---- POWMAN (gleboki sen Pico) ----
#define PM_FLAG_PATH "/powman.on"                     // tryb POWMAN wlaczony (przezywa zanik zasilania)
static bool pmOn = false;                               // tryb POWMAN wlaczony (plik PM_FLAG_PATH)
static uint32_t pmDeadline = 0;                         // koniec czuwania (millis), 0 = brak
static uint32_t pmSleepAt = 0;
static bool pmBtnUsed = false;                          // BOOTSEL wylaczyl POWMAN - puszczenie bez restartu wezla
#define PM_WAKE_PIN  14                               // wejscie wybudzenia (z GPIO wezla, 4,7 kOhm do GND)
#define PM_MAGIC     0x504D                           // znacznik w rejestrze scratch POWMAN (16 bitow)
static volatile bool pmPark = false, core1Parked = false;    // rdzen 1 ma sie zatrzymac przed snem
static volatile bool pmCheckReq = false, pmCheckDone = false; // sprawdzenie wezla przed snem (rdzen 1)
static bool pmWoke = false, pmByPin = false, pmByTimer = false;
static uint32_t pmSleptMs = 0, pmPlanSec = 0, pmCycle = 0;
static uint32_t pmDiagRst = 0, pmDiagWhy = 0, pmDiagScr = 0;   // diagnostyka pobudki
#define PM_STATE_PATH "/powman.st"                     // dane snu (czas, plan, cykl) - zapas dla scratch POWMAN
static volatile bool     fsReady = false;
static volatile bool     setupKoniec = false;         // rdzen 0 skonczyl setup() (LittleFS nie jest bezpieczny przy jednoczesnym uzyciu z 2 rdzeni)
const char* CFG_PENDING_PATH = "/cfg.pending";     // (do 0.39) konfiguracja do wyslania po restarcie Pico
// Dziennik etapow flashowania (od 0.40): "etap|proba|czas" - po restarcie Pico (watchdog, zanik zasilania)
// niedokonczony etap jest ponawiany (max 3 proby): 1 = zapis obrazow (firmware, OTA, LittleFS - od poczatku), 2 = konfiguracja
const char* ETAP_PATH = "/flash.etap";
void logf(const char* fmt, ...);
String readFileStr(const char* path);
void flushLogToFlash(const char* powod, bool wymus = false);
static void etapZapisz(int etap, int proba) {
  bool ok = false;
  for (int i = 0; i < 3 && !ok; i++) {                 // zapis z kontrola odczytem; ponowienie przy chwilowym bledzie
    if (i) delay(50);
    File f = LittleFS.open(ETAP_PATH, "w");
    if (f) { f.printf("%d|%d|%s", etap, proba, opTime); f.close(); }
    String t = readFileStr(ETAP_PATH); ok = t.startsWith(String(etap) + "|" + String(proba) + "|");
  }
  const char* nazwa = etap == 1 ? "etap 1 - zapis obrazow (firmware, OTA, LittleFS)" : "etap 2 - konfiguracja";
  if (proba > 0)                                        // wznowienie po restarcie Pico: etap z dziennika, nie od poczatku
    logf("[FLASH] Dziennik: wznowienie - %s, ponowienie %d/3%s%s\n", nazwa, proba,
         etap == 2 ? " (obrazy zapisane i sprawdzone MD5 przed restartem Pico)" : "", ok ? "" : " - BLAD ZAPISU DZIENNIKA!");
  else
    logf("[FLASH] Dziennik: %s%s rozpoczety%s\n", etap == 2 ? "etap 1 potwierdzony (MD5 wszystkich obrazow OK); " : "",
         nazwa, ok ? "" : " - BLAD ZAPISU DZIENNIKA!");
  flushLogToFlash(etap == 1 ? "etap 1 flashowania" : "etap 2 flashowania", true);   // log przetrwa zanik zasilania w trakcie
}
static int etapProba = 0;                            // ktora proba po restarcie (0 = zwykle zlecenie)
// Dziennik odczytany w setup() na rdzeniu 0 (zanim rdzen 1 zacznie uzywac LittleFS - plik nie jest bezpieczny z 2 rdzeni naraz)
static volatile int etapStart = 0, etapStartProba = 0; static volatile bool etapPkgOk = false; static char etapStartCzas[24] = "";
static const uint32_t CFG_BOOT_WAIT_MS = 60000;    // pierwszy start wezla: klucze, zapis plikow, GPS, BT
static const uint32_t CFG_RETRY_WAIT_MS = 30000;   // przed ponowieniem konfiguracji
static const uint32_t ACK_TIMEOUT_MS = 8000;       // np. zmiana regionu LoRa przestawia radio - trwa
#define PICO_VER "0.41"
#include "skrypt_py.h"                               // wyslij_firmware.py do pobrania ze strony
constexpr bool verEq(const char* a, const char* b) { return *a == *b && (*a == 0 || verEq(a + 1, b + 1)); }
static_assert(verEq(SKRYPT_PY_WERSJA, PICO_VER),
              "skrypt_py.h ma inna wersje niz program Pico - uzyj skrypt_py.h z tej samej wersji co .ino");
const char PICO_VER_TAG[] = "PICOVER=" PICO_VER;    // znacznik w pliku .bin - skrypt odczytuje z niego wersje
const char* PICO_VERSION = PICO_VER_TAG + 8;
static volatile uint32_t picoRebootAt = 0;         // zaplanowany restart Pico (przycisk / skrypt)

// Slad ostatniego kroku w rejestrze watchdoga (przetrwa restart z watchdoga)
static const uint32_t CRUMB_MAGIC = 0x50494331;
#define PLANNED_MAGIC 0x52455354u   // watchdog scratch[2]: restart na zadanie (nie zawieszenie)
enum { CR_IDLE = 0, CR_USB_DEINIT, CR_USB_INIT, CR_BOOTWAIT, CR_API, CR_CFG, CR_FLASH, CR_REBOOTWAIT, CR_UPLOAD };
static const char* const CRUMB_NAMES[] = { "praca normalna", "restart hosta USB (deinit)", "restart hosta USB (init)",
  "czekanie na start wezla", "laczenie API", "wysylanie konfiguracji", "flashowanie ESP", "czekanie na restart wezla",
  "odbior pakietu" };
static inline void crumb(uint32_t c, uint32_t extra = 0) {
  watchdog_hw->scratch[0] = c | (extra << 8); watchdog_hw->scratch[1] = CRUMB_MAGIC;
}
void readHeltec();

Adafruit_USBH_Host USBHost;
Adafruit_USBH_CDC  SerialHost;
WebServer server(80);
DNSServer dnsServer;                                   // captive portal: kazda nazwa -> adres Pico

// =========================================================================
// Log w pamięci (ostatnie 16 KB)
// =========================================================================
static const size_t LOG_SIZE = 49152;
static char   logBuf[LOG_SIZE];
static size_t logHead = 0;
static bool   logWrapped = false;
static volatile bool     heltecMounted = false;
static volatile uint32_t totalBytes = 0;
static volatile uint32_t nodeLines = 0;          // linie logu wezla (tekst i log_record) - do wykrywania ciszy
auto_init_mutex(logMtx);

static volatile uint32_t logTotal = 0;      // ile bajtow trafilo do logu od startu (do przyrostowego podgladu)
// Starty wezla: od linii "ESP-ROM:", baneru "//\\ E S H T /\\ S T / C" (MESHTASTIC) albo "Booted, wake cause" przez 60 s, osobno od logu biezacego.
// Zapis na flash (rdzen 0): /boot_a.txt = ostatni start, /boot_b.txt = poprzedni.
static const size_t BOOT_SIZE = 24576;
static char bootBuf[BOOT_SIZE]; static volatile size_t bootLen = 0;
static volatile uint8_t bootStan = 0;            // 0 nic, 1 zbieranie, 2 gotowe do zapisu
static volatile uint32_t bootStartMs = 0;
static void bootAppend(const char* d, size_t n) {
  if (bootStan != 1) return;
  size_t k = BOOT_SIZE - bootLen; if (n > k) n = k;
  memcpy(bootBuf + bootLen, d, n); bootLen += n;
}
void logAppend(const char* data, size_t len) {
  mutex_enter_blocking(&logMtx);
  bootAppend(data, len);
  logTotal += len;
  for (size_t i = 0; i < len; i++) {
    logBuf[logHead++] = data[i];
    if (logHead >= LOG_SIZE) { logHead = 0; logWrapped = true; }
  }
  mutex_exit(&logMtx);
}
// Osobny, mały bufor tylko na komunikaty Pico ([PICO]/[ESP]/[FLASH])
static const size_t PLOG_SIZE = 12288;
static char   plogBuf[PLOG_SIZE];
static size_t plogHead = 0;
static bool   plogWrapped = false;
static const char* bootReason = "?";

// ---- Czas: NTP (ntp.task.gda.pl) w trybie STA; bez internetu - godzina z logu wezla (bez daty) ----
static volatile bool     nodeTimeOk = false;      // znana godzina z logu wezla (Meshtastic zna czas z sieci LoRa)
static volatile uint32_t nodeTodSec = 0, nodeTodMs = 0;
static bool timeFromNtp() { return time(nullptr) > 1700000000; }
// "RRRR-MM-DD GG:MM:SS"; nieznane elementy jako znaki zapytania
void nowStr(char* b, size_t n) {
  if (timeFromNtp()) {
    time_t t = time(nullptr); struct tm tmv; localtime_r(&t, &tmv);
    strftime(b, n, "%Y-%m-%d %H:%M:%S", &tmv);
  } else if (nodeTimeOk) {
    uint32_t sec = (nodeTodSec + (millis() - nodeTodMs) / 1000) % 86400;
    snprintf(b, n, "?\?\?\?-?\?-?\? %02lu:%02lu:%02lu", (unsigned long)(sec / 3600), (unsigned long)(sec / 60 % 60), (unsigned long)(sec % 60));
  } else snprintf(b, n, "?\?\?\?-?\?-?\? ?\?:?\?:?\?");
}

void logText(const char* s) {
  size_t n = strlen(s);
  logAppend(s, n);
  char now[24]; nowStr(now, sizeof(now));
  mutex_enter_blocking(&logMtx);
  char ts[48];
  int tn = snprintf(ts, sizeof(ts), "[%s | %lus] ", now, (unsigned long)(millis() / 1000));
  for (int k = 0; k < tn; k++) { plogBuf[plogHead++] = ts[k]; if (plogHead >= PLOG_SIZE) { plogHead = 0; plogWrapped = true; } }
  for (size_t i = 0; i < n; i++) {
    if (s[i] == '\n' && i == 0) continue;                  // pomiń pusty wiersz na początku
    plogBuf[plogHead++] = s[i];
    if (plogHead >= PLOG_SIZE) { plogHead = 0; plogWrapped = true; }
  }
  mutex_exit(&logMtx);
}
void logf(const char* fmt, ...) {
  char m[320];
  va_list ap; va_start(ap, fmt); int n = vsnprintf(m, sizeof(m), fmt, ap); va_end(ap);
  if (n >= (int)sizeof(m) && fmt[strlen(fmt) - 1] == '\n') { m[sizeof(m) - 2] = '\n'; m[sizeof(m) - 1] = 0; }   // obciete - zachowaj koniec linii
  logText(m);
}
void fwSet(int pct, const char* fmt, ...) {
  char m[96];
  va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof(m), fmt, ap); va_end(ap);
  strncpy(fwMsg, m, sizeof(fwMsg) - 1);
  fwPct = pct;
  logf("[FLASH] %s\n", m);
}

// =========================================================================
// DTR/RTS
// =========================================================================
static volatile uint8_t cdcIdx = 0xFF;
static const Step SEQ_RESET[] = { {0x02, 200}, {0x00, 0} };
static const Step SEQ_BOOT[]  = { {0x00, 100}, {0x01, 100}, {0x03, 0}, {0x02, 100}, {0x00, 0} };
static const Step* seq = nullptr;
static uint8_t seqLen = 0, seqPos = 0;
static volatile bool xferBusy = false;
static uint32_t nextAt = 0, sentAt = 0;

void xferDone(tuh_xfer_t* xfer) { (void)xfer; xferBusy = false; }

void startSeq(const Step* s, uint8_t n, const char* name) {
  if (cdcIdx == 0xFF) { logText("\n[PICO] Brak ESP32-S3 - polecenie pominiete\n"); return; }
  seq = s; seqLen = n; seqPos = 0; nextAt = millis(); xferBusy = false;
  logf("\n[PICO] Start sekwencji: %s\n", name);
}

void runSeq() {
  if (!seq) return;
  if (xferBusy) {
    if (millis() - sentAt > 1000) { xferBusy = false; logText("[PICO] Brak potwierdzenia polecenia\n"); }
    return;
  }
  if ((int32_t)(millis() - nextAt) < 0) return;
  if (seqPos >= seqLen || cdcIdx == 0xFF) { seq = nullptr; logText("[PICO] Sekwencja zakonczona\n"); return; }
  const Step& st = seq[seqPos++];
  xferBusy = true; sentAt = millis();
  if (!tuh_cdc_set_control_line_state(cdcIdx, st.lines, xferDone, 0)) {
    xferBusy = false; logText("[PICO] Nie udalo sie wyslac polecenia\n");
  }
  nextAt = millis() + st.delayAfter;
}

// =========================================================================
// Callbacki TinyUSB
// =========================================================================
extern "C" {
void tuh_mount_cb(uint8_t daddr) {
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(daddr, &vid, &pid);
  logf("\n[PICO] USB: urzadzenie addr=%u VID=%04X PID=%04X\n", daddr, vid, pid);
}
void tuh_umount_cb(uint8_t daddr) { logf("\n[PICO] USB: urzadzenie addr=%u odlaczone\n", daddr); }
void tuh_cdc_mount_cb(uint8_t idx) {
  SerialHost.mount(idx);
  cdcIdx = idx;
  heltecMounted = true;
  logText("\n[PICO] ESP32-S3 podlaczony (CDC)\n");
}
void tuh_cdc_umount_cb(uint8_t idx) {
  SerialHost.umount(idx);
  if (idx == cdcIdx) cdcIdx = 0xFF;
  seq = nullptr; xferBusy = false;
  heltecMounted = false;
  logText("\n[PICO] ESP32-S3 odlaczony\n");
}
bool hcd_port_connect_status(uint8_t rhport);
}

// Czy na linii USB jest fizycznie urzadzenie (podciagniete D+/D-), niezaleznie od logow i enumeracji.
// Rejestr SIE_STATUS.SPEED w trybie host: 0 = brak urzadzenia, 1 = LS, 2 = FS.
bool usbDevicePresent() { return (usb_hw->sie_status & USB_SIE_STATUS_SPEED_BITS) != 0; }
bool restartUsbHost(bool force = false);
static bool picoUpdBoot = false;                        // start zaraz po aktualizacji programu Pico
#define USBFIX_PATH "/usbfix.once"
#define NODE_RST_PATH "/node_rst.pend"                 // po aktualizacji Pico: reset wezla jeszcze nie zrobiony

// Restart hosta USB tylko gdy jest urzadzenie: wielokrotne restarty przy pustej linii zawieszaly
// TinyUSB (rdzen 1 stawal, watchdog restartowal Pico) - sprawdzone w testach 0.26.
bool restartUsbHost(bool force) {
  if (!usbDevicePresent() && !force) {
    logText("[PICO] Brak urzadzenia na linii USB - restart hosta pominiety\n");
    return false;
  }
  logText(force && !usbDevicePresent() ? "\n[PICO] Restart hosta USB (wymuszony przyciskiem, linia pusta)...\n" : "\n[PICO] Restart hosta USB...\n");
  crumb(CR_USB_DEINIT);
  tuh_deinit(0);
  cdcIdx = 0xFF; heltecMounted = false;
  delay(200);
  tusb_rhport_init_t rh = {};
  rh.role  = TUSB_ROLE_HOST;
  rh.speed = TUSB_SPEED_AUTO;
  crumb(CR_USB_INIT);
  tusb_init(0, &rh);
  logText("[PICO] Host USB uruchomiony ponownie\n");
  return true;
}

// =========================================================================
// Narzędzia blokujące dla rdzenia 1 (czekając, cały czas obsługują USB)
// =========================================================================
void usbWait(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) { core1Beat = millis(); USBHost.task(); }
}

bool setLinesBlocking(uint8_t lines) {
  if (cdcIdx == 0xFF) return false;
  xferBusy = true;
  if (!tuh_cdc_set_control_line_state(cdcIdx, lines, xferDone, 0)) { xferBusy = false; return false; }
  uint32_t t0 = millis();
  while (xferBusy && millis() - t0 < 500) { core1Beat = millis(); USBHost.task(); }
  return !xferBusy;
}

bool runSeqBlocking(const Step* s, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    if (!setLinesBlocking(s[i].lines)) return false;
    usbWait(s[i].delayAfter);
  }
  return true;
}

void drainUsb() {
  uint8_t b;
  while (SerialHost.available() > 0) SerialHost.read(&b, 1);
}

// =========================================================================
// Protokół bootloadera ESP32 (SLIP + komendy, jak esptool, tryb ROM)
// =========================================================================
enum : uint8_t {
  ESP_FLASH_BEGIN = 0x02, ESP_FLASH_END = 0x04, ESP_SYNC = 0x08, ESP_WRITE_REG = 0x09, ESP_READ_REG = 0x0A,
  ESP_SPI_SET_PARAMS = 0x0B, ESP_SPI_ATTACH = 0x0D,
  ESP_DEFL_BEGIN = 0x10, ESP_DEFL_DATA = 0x11, ESP_DEFL_END = 0x12, ESP_MD5 = 0x13
};
static const uint32_t ESP_BLOCK = 0x400;   // rozmiar bloku zapisu w trybie ROM

static inline void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

bool usbWriteAll(const uint8_t* d, size_t len) {
  uint32_t t0 = millis();
  while (len) {
    if (cdcIdx == 0xFF || millis() - t0 > 3000) return false;
    uint32_t w = tuh_cdc_write(cdcIdx, d, len);
    d += w; len -= w;
    tuh_cdc_write_flush(cdcIdx);
    core1Beat = millis();
    USBHost.task();
  }
  return true;
}

bool espSend(uint8_t cmd, const uint8_t* data, uint16_t len, uint32_t chk) {
  static uint8_t out[2 * (8 + 16 + ESP_BLOCK) + 4];
  uint8_t hdr[8] = { 0x00, cmd, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
  put32(hdr + 4, chk);
  size_t o = 0;
  out[o++] = 0xC0;
  auto put = [&](uint8_t b) {
    if (b == 0xC0)      { out[o++] = 0xDB; out[o++] = 0xDC; }
    else if (b == 0xDB) { out[o++] = 0xDB; out[o++] = 0xDD; }
    else                  out[o++] = b;
  };
  for (int i = 0; i < 8; i++) put(hdr[i]);
  for (uint16_t i = 0; i < len; i++) put(data[i]);
  out[o++] = 0xC0;
  return usbWriteAll(out, o);
}

// Parser odpowiedzi SLIP
static uint8_t rxFrame[256];
static size_t  rxLen = 0;
static bool    rxIn = false, rxEsc = false;
void espParserReset() { rxLen = 0; rxIn = false; rxEsc = false; }

// Zwraca: 1 = OK, 0 = brak odpowiedzi (timeout), -1 = bootloader zgłosił błąd
int espRecv(uint8_t cmd, uint32_t timeoutMs, uint32_t* value, uint8_t* dataOut, size_t* dataLen) {
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    core1Beat = millis();
    USBHost.task();
    if (cdcIdx == 0xFF) return 0;
    uint8_t b;
    while (SerialHost.available() > 0 && SerialHost.read(&b, 1) == 1) {
      if (b == 0xC0) {
        if (rxIn && rxLen >= 8 && rxFrame[0] == 0x01 && rxFrame[1] == cmd) {
          uint16_t size = rxFrame[2] | (rxFrame[3] << 8);
          if (size + 8 > rxLen) size = rxLen - 8;
          const uint8_t* d = rxFrame + 8;
          size_t sb = (size >= 4) ? 4 : 2;                     // bajty statusu na końcu danych
          size_t pos = (size >= sb) ? size - sb : 0;
          uint8_t status = d[pos], err = d[pos + 1];
          if (value) *value = rxFrame[4] | (rxFrame[5] << 8) | (rxFrame[6] << 16) | ((uint32_t)rxFrame[7] << 24);
          if (dataOut && dataLen) { size_t n = pos < *dataLen ? pos : *dataLen; memcpy(dataOut, d, n); *dataLen = n; }
          rxIn = true; rxLen = 0; rxEsc = false;
          if (status != 0) { logf("[ESP] Komenda 0x%02X: blad 0x%02X\n", cmd, err); return -1; }
          return 1;
        }
        rxIn = true; rxLen = 0; rxEsc = false;
        continue;
      }
      if (!rxIn) continue;
      if (rxEsc) { b = (b == 0xDC) ? 0xC0 : (b == 0xDD) ? 0xDB : b; rxEsc = false; }
      else if (b == 0xDB) { rxEsc = true; continue; }
      if (rxLen < sizeof(rxFrame)) rxFrame[rxLen++] = b;
    }
  }
  return 0;
}

int espCmd(uint8_t cmd, const uint8_t* data, uint16_t len, uint32_t chk, uint32_t timeoutMs,
           uint32_t* value = nullptr, uint8_t* dataOut = nullptr, size_t* dataLen = nullptr) {
  if (!espSend(cmd, data, len, chk)) return 0;
  return espRecv(cmd, timeoutMs, value, dataOut, dataLen);
}

bool espReadReg(uint32_t addr, uint32_t* val) {
  uint8_t d[4]; put32(d, addr);
  return espCmd(ESP_READ_REG, d, 4, 0, 1000, val) == 1;
}

bool espWriteReg(uint32_t addr, uint32_t val) {
  uint8_t d[16]; put32(d, addr); put32(d + 4, val); put32(d + 8, 0xFFFFFFFF); put32(d + 12, 0);
  return espCmd(ESP_WRITE_REG, d, 16, 0, 1000) == 1;
}
// ESP32-S3 przez USB-Serial/JTAG: watchdogi RTC (RWDT) i SWD nie sa kasowane przy wejsciu w bootloader.
// Gdy wezel nie ma dzialajacego programu (bootloader 2. stopnia uzbroil RWDT i wpadl w petle restartow),
// RWDT resetuje uklad po ~9 s flashowania. Jak esptool: wylacz RWDT, SWD na automatyczne karmienie.
void espWylaczWatchdogi() {
  const uint32_t B = 0x60008000;                    // RTC_CNTL (ESP32-S3)
  bool ok = espWriteReg(B + 0xB0, 0x50D83AA1) && espWriteReg(B + 0x98, 0) && espWriteReg(B + 0xB0, 0);
  uint32_t swd = 0;
  ok = ok && espWriteReg(B + 0xB8, 0x8F1D312A) && espReadReg(B + 0xB4, &swd) && espWriteReg(B + 0xB4, swd | (1UL << 31)) && espWriteReg(B + 0xB8, 0);
  logText(ok ? "[ESP] Watchdogi RTC wylaczone na czas flashowania\n" : "[ESP] UWAGA: nie udalo sie wylaczyc watchdogow RTC\n");
}

// Wejście w bootloader + SYNC
bool espEnterAndSync() {
  espParserReset();
  if (!runSeqBlocking(SEQ_BOOT, 5)) { logText("[ESP] BLAD: sekwencja DTR/RTS\n"); return false; }
  usbWait(300);
  drainUsb();
  uint8_t sync[36] = { 0x07, 0x07, 0x12, 0x20 };
  memset(sync + 4, 0x55, 32);
  for (int i = 1; i <= 10; i++) {
    logf("[ESP] SYNC proba %d...\n", i);
    if (espCmd(ESP_SYNC, sync, sizeof(sync), 0, 300) == 1) {
      usbWait(100);
      drainUsb();
      espParserReset();
      logText("[ESP] SYNC OK\n");
      espWylaczWatchdogi();
      return true;
    }
  }
  logText("[ESP] BLAD: brak odpowiedzi na SYNC\n");
  return false;
}

// =========================================================================
// Test bootloadera (bez zapisu)
// =========================================================================
void bootloaderTest() {
  logText("\n[PICO] ===== TEST BOOTLOADERA =====\n");
  if (cdcIdx == 0xFF) { logText("[PICO] Brak ESP32-S3\n"); return; }
  if (espEnterAndSync()) {
    uint32_t magic = 0;
    if (espReadReg(0x40001000, &magic))
      logf("[PICO] Uklad: 0x%08lX %s\n", (unsigned long)magic, magic == 0x9 ? "= ESP32-S3" : "(nieznany)");
    uint32_t mac0 = 0, mac1 = 0;
    if (espReadReg(0x60007044, &mac0) && espReadReg(0x60007048, &mac1))
      logf("[PICO] MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
           (unsigned)((mac1 >> 8) & 0xFF), (unsigned)(mac1 & 0xFF),
           (unsigned)(mac0 >> 24), (unsigned)((mac0 >> 16) & 0xFF),
           (unsigned)((mac0 >> 8) & 0xFF), (unsigned)(mac0 & 0xFF));
  }
  logText("[PICO] Restart ESP32-S3 do normalnej pracy (RTS)...\n");
  runSeqBlocking(SEQ_RESET, 2);
  logText("[PICO] ===== KONIEC TESTU =====\n\n");
}

// =========================================================================
// FLASHOWANIE pakietu z LittleFS
// =========================================================================
static uint32_t timeoutPerMb(uint32_t msPerMb, uint32_t bytes) {
  uint32_t t = (uint32_t)(((uint64_t)msPerMb * bytes) / (1024 * 1024));
  return t < 3000 ? 3000 : t;
}

bool flashFail(const char* msg) {
  LittleFS.remove(ETAP_PATH);
  fwSet(fwPct, "BLAD: %s", msg);
  fwState = 3;
  logText("[FLASH] ESP32-S3 zostaje w trybie bootloadera - mozna ponowic flashowanie.\n");
  return false;
}


// =========================================================================
// Konfiguracja: ramki ToRadio z pakietu (MTCF), numer wezla podmieniany na biezacy
// =========================================================================
static bool pbVarint(const uint8_t*& p, const uint8_t* e, uint64_t& v) {
  v = 0;
  for (int s = 0; s < 64 && p < e; s += 7) { uint8_t b = *p++; v |= (uint64_t)(b & 0x7F) << s; if (!(b & 0x80)) return true; }
  return false;
}
static bool pbSkip(const uint8_t*& p, const uint8_t* e, uint8_t wt) {
  uint64_t v;
  switch (wt) {
    case 0: return pbVarint(p, e, v);
    case 1: if (e - p < 8) return false; p += 8; return true;
    case 2: if (!pbVarint(p, e, v) || v > (uint64_t)(e - p)) return false; p += v; return true;
    case 5: if (e - p < 4) return false; p += 4; return true;
    default: return false;
  }
}
// Pole tekstowe nr 'fld' z podwiadomosci [p, e)
static void pbLogString(const uint8_t* p, const uint8_t* e, uint32_t fld, const char* prefix) {
  while (p < e) {
    uint64_t t; if (!pbVarint(p, e, t)) return;
    if ((t >> 3) == fld && (t & 7) == 2) {
      uint64_t L; if (!pbVarint(p, e, L) || L > (uint64_t)(e - p)) return;
      char m[200]; size_t n = L < sizeof(m) - 1 ? L : sizeof(m) - 1;
      memcpy(m, p, n); m[n] = 0;
      logf("%s%s\n", prefix, m);
      return;
    }
    if (!pbSkip(p, e, t & 7)) return;
  }
}
// Pole tekstowe nr 'fld' z podwiadomosci [p, e) do bufora
static void pbStr(const uint8_t* p, const uint8_t* e, uint32_t fld, char* out, size_t n) {
  while (p < e) {
    uint64_t t; if (!pbVarint(p, e, t)) return;
    if ((t >> 3) == fld && (t & 7) == 2) {
      uint64_t L; if (!pbVarint(p, e, L) || L > (uint64_t)(e - p)) return;
      size_t k = L < n - 1 ? L : n - 1; memcpy(out, p, k); out[k] = 0; return;
    }
    if (!pbSkip(p, e, t & 7)) return;
  }
}
// Odebrana ramka FromRadio
void apiFrame(const uint8_t* d, uint16_t n) {
  apiFrames++;
  const uint8_t *p = d, *e = d + n;
  while (p < e) {
    uint64_t tag; if (!pbVarint(p, e, tag)) return;
    uint32_t fld = tag >> 3; uint8_t wt = tag & 7;
    if (wt == 2 && (fld == 2 || fld == 3 || fld == 4 || fld == 5 || fld == 6 || fld == 10 || fld == 13 || fld == 16)) {
      uint64_t L; if (!pbVarint(p, e, L) || L > (uint64_t)(e - p)) return;
      const uint8_t *q = p, *qe = p + L; p += L;
      if (fld == 2) {                                   // packet: potwierdzenia (ROUTING_APP) i wiadomosci tekstowe
        uint32_t pFrom = 0, pId = 0; uint64_t pCh = 0;
        while (q < qe) {
          uint64_t t2; if (!pbVarint(q, qe, t2)) break;
          if ((t2 >> 3) == 1 && (t2 & 7) == 5 && qe - q >= 4) { pFrom = q[0] | (q[1] << 8) | (q[2] << 16) | ((uint32_t)q[3] << 24); q += 4; continue; }
          if ((t2 >> 3) == 6 && (t2 & 7) == 5 && qe - q >= 4) { pId = q[0] | (q[1] << 8) | (q[2] << 16) | ((uint32_t)q[3] << 24); q += 4; continue; }
          if ((t2 >> 3) == 7 && (t2 & 7) == 5 && qe - q >= 4) {          // rx_time: zegar wezla (z sieci LoRa / GPS)
            uint32_t rt = q[0] | (q[1] << 8) | (q[2] << 16) | ((uint32_t)q[3] << 24); q += 4;
            if (rt > 1700000000 && !timeFromNtp()) {
              struct timeval tv = { (time_t)rt, 0 }; settimeofday(&tv, nullptr);
              char b[24]; nowStr(b, sizeof(b)); logf("[PICO] Data i godzina z sieci LoRa: %s\n", b);
            }
            continue;
          }
          if ((t2 >> 3) == 3 && (t2 & 7) == 0) { if (!pbVarint(q, qe, pCh)) break; continue; }
          if ((t2 >> 3) == 4 && (t2 & 7) == 2) {
            uint64_t L2; if (!pbVarint(q, qe, L2) || L2 > (uint64_t)(qe - q)) break;
            const uint8_t *d2 = q, *de = q + L2;
            uint64_t port = 0; uint32_t req = 0; uint8_t err = 0;
            const uint8_t* txt = nullptr; size_t txtLen = 0;
            while (d2 < de) {
              uint64_t t3; if (!pbVarint(d2, de, t3)) break;
              uint32_t f3 = t3 >> 3; uint8_t w3 = t3 & 7;
              if (f3 == 1 && w3 == 0) { if (!pbVarint(d2, de, port)) break; }
              else if (f3 == 6 && w3 == 5 && de - d2 >= 4) { req = d2[0] | (d2[1] << 8) | (d2[2] << 16) | ((uint32_t)d2[3] << 24); d2 += 4; }
              else if (f3 == 2 && w3 == 2) {               // payload: Routing (error_reason 3) albo tekst
                uint64_t L3; if (!pbVarint(d2, de, L3) || L3 > (uint64_t)(de - d2)) break;
                const uint8_t *r = d2, *re = d2 + L3; d2 += L3;
                txt = r; txtLen = L3;
                while (r < re) {
                  uint64_t t4; if (!pbVarint(r, re, t4)) break;
                  if ((t4 >> 3) == 3 && (t4 & 7) == 0) { uint64_t v; if (pbVarint(r, re, v)) err = (uint8_t)v; break; }
                  if (!pbSkip(r, re, t4 & 7)) break;
                }
              } else if (!pbSkip(d2, de, w3)) break;
            }
            if (port == 5 && req) { ackErr = err; ackId = req; }
            // Wiadomosc tekstowa przekazana przez wezel (gdy Pico jest klientem API) -> komenda
            if (port == 1 && txt && txtLen && pFrom && (int)pCh == cfg.ch && chSecure(true)) {
              char m[201]; size_t k = 0;
              for (; k < txtLen && k < sizeof(m) - 1 && txt[k] != '\r' && txt[k] != '\n'; k++) m[k] = txt[k];
              m[k] = 0;
              loraCmd(m, pFrom, pId);
            }
            break;
          }
          if (!pbSkip(q, qe, t2 & 7)) break;
        }
      } else if (fld == 3) {                                   // my_info.my_node_num
        while (q < qe) {
          uint64_t t2; if (!pbVarint(q, qe, t2)) break;
          if ((t2 >> 3) == 1 && (t2 & 7) == 0) { uint64_t v; if (pbVarint(q, qe, v)) apiNodeNum = (uint32_t)v; break; }
          if (!pbSkip(q, qe, t2 & 7)) break;
        }
      } else if (fld == 10) {                                  // channel: indeks i dlugosc klucza (PSK) - bez zapisu klucza
        uint64_t idx = 0; int pskLen = 0; bool rola = false; uint64_t role = 0;
        while (q < qe) {
          uint64_t t2; if (!pbVarint(q, qe, t2)) break;
          if ((t2 >> 3) == 1 && (t2 & 7) == 0) { if (!pbVarint(q, qe, idx)) break; continue; }
          if ((t2 >> 3) == 3 && (t2 & 7) == 0) { if (!pbVarint(q, qe, role)) break; rola = true; continue; }
          if ((t2 >> 3) == 2 && (t2 & 7) == 2) {
            uint64_t L2; if (!pbVarint(q, qe, L2) || L2 > (uint64_t)(qe - q)) break;
            const uint8_t *s2 = q, *se = q + L2; q += L2;
            while (s2 < se) {
              uint64_t t3; if (!pbVarint(s2, se, t3)) break;
              if ((t3 >> 3) == 2 && (t3 & 7) == 2) { uint64_t L3; if (!pbVarint(s2, se, L3) || L3 > (uint64_t)(se - s2)) break; pskLen = (int)L3; s2 += L3; continue; }
              if (!pbSkip(s2, se, t3 & 7)) break;
            }
            continue;
          }
          if (!pbSkip(q, qe, t2 & 7)) break;
        }
        if (idx < 8) chPskLen[idx] = (rola && role == 0) ? 0 : (int8_t)pskLen;   // kanal wylaczony = brak klucza
      } else if (fld == 5) {                                   // config: tylko security (pole 8) - stan debug_log_api_enabled
        while (q < qe) {
          uint64_t t2; if (!pbVarint(q, qe, t2)) break;
          if ((t2 >> 3) == 8 && (t2 & 7) == 2) {
            uint64_t L2; if (!pbVarint(q, qe, L2) || L2 > (uint64_t)(qe - q)) break;
            const uint8_t *s2 = q, *se = q + L2; q += L2;
            bool dbg = false, man = false, kl = false;
            while (s2 < se) {
              uint64_t t3; if (!pbVarint(s2, se, t3)) break;
              uint32_t f3 = t3 >> 3; uint8_t w3 = t3 & 7;
              if (w3 == 0 && (f3 == 4 || f3 == 6)) { uint64_t v; if (!pbVarint(s2, se, v)) break; if (f3 == 4) man = v; else dbg = v; continue; }
              if (f3 == 2 && w3 == 2) { uint64_t L3; if (!pbVarint(s2, se, L3) || L3 > (uint64_t)(se - s2)) break; kl = (L3 == 32); s2 += L3; continue; }
              if (!pbSkip(s2, se, w3)) break;
            }
            secManaged = man; secKlucz = kl;
            if (L2 <= sizeof(secRaw)) { memcpy(secRaw, se - L2, L2); secLen = L2; } else secLen = 0;
            secStan = dbg ? 1 : 2;
            continue;
          }
          if (!pbSkip(q, qe, t2 & 7)) break;
        }
      } else if (fld == 13) {                                  // metadata: wersja firmware wezla
        pbStr(q, qe, 1, nodeFw, sizeof(nodeFw));
      } else if (fld == 4) {                                   // node_info: nazwa wlasnego wezla
        uint64_t num = 0; const uint8_t *u = nullptr, *ue = nullptr;
        while (q < qe) {
          uint64_t t2; if (!pbVarint(q, qe, t2)) break;
          if ((t2 >> 3) == 1 && (t2 & 7) == 0) { if (!pbVarint(q, qe, num)) break; continue; }
          if ((t2 >> 3) == 2 && (t2 & 7) == 2) {
            uint64_t L2; if (!pbVarint(q, qe, L2) || L2 > (uint64_t)(qe - q)) break;
            u = q; ue = q + L2; q += L2; continue;
          }
          if (!pbSkip(q, qe, t2 & 7)) break;
        }
        if (u && num && (uint32_t)num == apiNodeNum) { pbStr(u, ue, 2, nodeLong, sizeof(nodeLong)); pbStr(u, ue, 3, nodeShort, sizeof(nodeShort)); }
      } else if (fld == 6) {                                   // log_record -> linia jak w zwyklym logu tekstowym
        const uint8_t* msg = nullptr; uint32_t mlen = 0, tsec = 0, lvl = 0; char src[24] = "";
        while (q < qe) {
          uint64_t t2; if (!pbVarint(q, qe, t2)) break;
          uint32_t f2 = t2 >> 3, w2 = t2 & 7;
          if (f2 == 1 && w2 == 2) { uint64_t L2; if (!pbVarint(q, qe, L2) || L2 > (uint64_t)(qe - q)) break; msg = q; mlen = L2; q += L2; }
          else if (f2 == 2 && w2 == 5 && qe - q >= 4) { tsec = q[0] | (q[1] << 8) | (q[2] << 16) | ((uint32_t)q[3] << 24); q += 4; }
          else if (f2 == 4 && w2 == 0) { uint64_t v; if (!pbVarint(q, qe, v)) break; lvl = v; }
          else if (f2 == 3 && w2 == 2) { uint64_t L2; if (!pbVarint(q, qe, L2) || L2 > (uint64_t)(qe - q)) break;
                                         uint32_t k = L2 < sizeof(src) - 1 ? L2 : sizeof(src) - 1; memcpy(src, q, k); src[k] = 0; q += L2; }
          else if (!pbSkip(q, qe, w2)) break;
        }
        // szum po kazdym polaczeniu API (odtwarzanie kolejki pakietow dla klienta) - pomijamy
        static const char* const SZUM[] = { "replay packet to phone", "phone downloaded packet", "Tell client new packets" };
        bool szum = false;
        for (auto z : SZUM) { size_t zl = strlen(z); if (msg && mlen >= zl && !memcmp(msg, z, zl)) szum = true; }
        if (msg && !szum) {
          const char* lv = lvl >= 50 ? "CRIT " : lvl >= 40 ? "ERROR" : lvl >= 30 ? "WARN " : lvl >= 20 ? "INFO " : lvl >= 10 ? "DEBUG" : lvl ? "TRACE" : "";
          char pre[64]; int pl;
          if (tsec > 1700000000UL) { time_t tt = tsec; struct tm tmv; gmtime_r(&tt, &tmv);   // wezel podaje juz czas lokalny
            pl = snprintf(pre, sizeof(pre), "%s | %02d:%02d:%02d [%s] ", lv, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, src); }
          else pl = snprintf(pre, sizeof(pre), "%s | [%s] ", lv, src);
          if (pl > 0) heltecText((const uint8_t*)pre, pl < (int)sizeof(pre) ? pl : (int)sizeof(pre) - 1);
          heltecText(msg, (int)mlen);
          if (!mlen || msg[mlen - 1] != '\n') { static const uint8_t nl = '\n'; heltecText(&nl, 1); }
        }
      }
      else pbLogString(q, qe, 4, "[WEZEL] ");                     // client_notification.message
    } else if (fld == 7 && wt == 0) { uint64_t v; pbVarint(p, e, v); apiConfigDone = true; }
    else if (!pbSkip(p, e, wt)) return;
  }
}

void apiPump(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) { core1Beat = millis(); USBHost.task(); readHeltec(); }
}

// ---- Opis pakietu i historia wgran (pliki tekstowe na Pico) ----
String readFileStr(const char* path) {
  File f = LittleFS.open(path, "r"); if (!f) return "";
  String s = f.readString(); f.close(); return s;
}
String infoLine(const String& all, const char* prefix) {          // pierwsza linia zaczynajaca sie od prefix
  int p = all.startsWith(prefix) ? 0 : all.indexOf(String("\n") + prefix);
  if (p < 0) return "";
  if (p > 0) p++;
  int e = all.indexOf('\n', p);
  return e < 0 ? all.substring(p) : all.substring(p, e);
}
String opTimeStr() {
  return opTime[0] ? String(opTime) : String("czas nieznany (uptime Pico ") + String(millis() / 1000) + " s)";
}
// fw: wgrano firmware; cfg: wgrano konfiguracje; erased: pelne kasowanie (stara konfiguracja zniknela)
void noteNodeInfo(bool fw, bool cfg, bool erased) {
  String pkg = readFileStr(PKG_INFO_PATH), old = readFileStr(NODE_INFO_PATH);
  String fwPart = infoLine(old, "FW|"), cfgPart = infoLine(old, "CFG|"), otaPart = infoLine(old, "OTA|");
  bool tylkoOta = infoLine(pkg, "tryb:").indexOf("tylko OTA") >= 0;   // samo OTA nie zmienia wpisu o firmware
  if (fw && !tylkoOta) {
    fwPart = "FW|" + opTimeStr() + ": ";
    for (const char* k : { "firmware:", "ota:", "littlefs:", "tryb:" }) {
      String l = infoLine(pkg, k); if (l.length()) fwPart += l + " ; ";
    }
  }
  if (fw) {
    String o = infoLine(pkg, "ota:");
    if (o.length()) otaPart = "OTA|" + opTimeStr() + ": " + o;              // OTA pamietane osobno - aktualizacja go nie zmienia
    else if (erased) otaPart = "OTA|" + opTimeStr() + ": brak (flash skasowany)";
    if (erased && !cfg) cfgPart = "CFG|" + opTimeStr() + ": ustawienia fabryczne (flash skasowany)";
  }
  if (cfg) cfgPart = "CFG|" + opTimeStr() + ": " + infoLine(pkg, "konfiguracja:");
  File f = LittleFS.open(NODE_INFO_PATH, "w");
  if (f) { if (fwPart.length()) f.println(fwPart); if (otaPart.length()) f.println(otaPart); if (cfgPart.length()) f.println(cfgPart); f.close(); }
}

// Kanal komend musi byc prywatny z kluczem 256-bit (32 bajty). Publiczny / domyslny / krotki klucz = komendy
// ignorowane i nic nie jest wysylane (kazdy w zasiegu moglby sterowac Pico i czytac odpowiedzi).
bool chSecure(bool przyKomendzie) {
  static int8_t last = -2;
  int ch = cfg.ch; int8_t k = (ch >= 0 && ch < 8) ? chPskLen[ch] : -1;
  bool ok = (k == 32);
  if (k != last) {
    last = k;
    if (ok) logf("[PICO] Kanal komend %d: prywatny, klucz 256-bit - OK\n", ch);
    else logf("[PICO] *** Kanal komend %d NIE jest bezpieczny (%s) - komendy LoRa ignorowane, raporty LoRa wylaczone. Ustaw kanal prywatny z kluczem 256-bit ***\n",
              ch, k < 0 ? "brak informacji z wezla" : k == 0 ? "kanal wylaczony albo bez szyfrowania" :
                  k == 1 ? "klucz domyslny/prosty (kanal publiczny)" : k == 16 ? "klucz tylko 128-bit" : "nietypowy klucz");
  }
  if (!ok && przyKomendzie) logText("[PICO] Komenda LoRa odrzucona - kanal komend nie jest prywatny z kluczem 256-bit\n");
  return ok;
}

// Wyslanie wiadomosci tekstowej na kanal (broadcast) przez API wezla
static size_t putVar(uint8_t* b, uint32_t v) { size_t n = 0; while (v >= 0x80) { b[n++] = (v & 0x7F) | 0x80; v >>= 7; } b[n++] = v; return n; }
bool apiSendText(int ch, const char* text, bool connected = false) {
  if (cdcIdx == 0xFF || fwState == 1) return false;
  if (!connected && !apiConnect()) { apiMode = false; logText("[PICO] Odpowiedz LoRa: wezel nie odpowiada\n"); return false; }
  if (ch != cfg.ch || !chSecure(false)) { apiMode = false; logText("[PICO] Wiadomosc LoRa NIE wyslana - kanal nie jest prywatny z kluczem 256-bit\n"); return false; }
  uint8_t data[220], pkt[240], tr[250], fr[260];
  size_t tl = strlen(text); if (tl > 200) tl = 200;
  size_t d = 0; data[d++] = 0x08; data[d++] = 0x01; data[d++] = 0x12; d += putVar(data + d, tl); memcpy(data + d, text, tl); d += tl;
  size_t p = 0; pkt[p++] = 0x15; put32(pkt + p, 0xFFFFFFFF); p += 4;               // to = broadcast
  pkt[p++] = 0x18; pkt[p++] = (uint8_t)ch;                                          // channel
  pkt[p++] = 0x22; p += putVar(pkt + p, d); memcpy(pkt + p, data, d); p += d;       // decoded
  pkt[p++] = 0x35; put32(pkt + p, 0x50000000 | (millis() & 0x0FFFFFFF)); p += 4;    // id
  pkt[p++] = 0x48; pkt[p++] = (uint8_t)constrain(cfg.hop, 0, 7);                  // hop_limit z ustawien (domyslnie 3; bez pola = 0)
  size_t t = 0; tr[t++] = 0x0A; t += putVar(tr + t, p); memcpy(tr + t, pkt, p); t += p;  // ToRadio.packet
  fr[0] = 0x94; fr[1] = 0xC3; fr[2] = t >> 8; fr[3] = t & 0xFF; memcpy(fr + 4, tr, t);
  bool ok = usbWriteAll(fr, t + 4);
  apiPump(1500);
  apiMode = false;          // bez "disconnect": wezel dalej przekazuje wiadomosci (komendy) jako pakiety
  logf("[PICO] Odpowiedz LoRa (kanal %d): %s\n", ch, (strstr(text, "ustawiono") && strstr(text, "PWD")) ? "(zmiana hasla - tresc ukryta)" : text);
  return ok;
}

// security.debug_log_api_auto_enabled: po podlaczeniu wezla sprawdz security.debug_log_api_enabled; gdy wylaczone -
// wyslij te sama sekcje security (klucze bez zmian) z wlaczona flaga. Wezel zapisuje i restartuje sie (jednorazowo).
// Bez tego kazda odpowiedz LoRa Pico (tryb API) zatrzymuje logi tekstowe wezla do jego restartu.
static void logApiService() {
  static bool prevMnt = false; static uint32_t tMnt = 0; static uint8_t prob = 0, wlProb = 0; static bool zgloszone = false;
  if (heltecMounted != prevMnt) { prevMnt = heltecMounted; tMnt = millis(); if (heltecMounted) { secStan = 0; zgloszone = false; } }
  if (fwState == 1) { secStan = 0; zgloszone = false; tMnt = millis(); return; }   // flashowanie/konfiguracja: stan security nieaktualny - sprawdz 20 s po koncu
  if (!cfg.logApiAuto || !heltecMounted || fwState == 1 || brActive || millis() - tMnt < 20000) return;
  if (secStan == 0) {                                   // jeszcze nie znamy: jedno polaczenie API po starcie wezla
    if (prob >= 4) return;
    prob++;
    if (apiConnect()) { if (secStan != 2) { usbWriteAll(API_DISC, sizeof(API_DISC)); apiPump(300); apiMode = false; } }
    else apiMode = false;
    if (secStan == 0) { tMnt = millis(); return; }      // brak odpowiedzi - ponow za 20 s
  }
  if (secStan == 1) { if (!zgloszone) { logText("[PICO] Wezel: security.debug_log_api_enabled wlaczone - logi plyna takze w trybie API\n"); zgloszone = true; } return; }
  if (zgloszone) return;
  zgloszone = true;
  if (wlProb >= 2) { apiMode = false; logText("[PICO] Wezel nadal ma debug_log_api_enabled wylaczone po 2 probach - nie ponawiam do restartu Pico\n"); return; }
  if (secManaged) { logText("[PICO] Wezel: debug_log_api_enabled wylaczone, ale wezel jest 'managed' (is_managed) - Pico nie moze tego zmienic lokalnie\n"); return; }
  if (!secLen || !secKlucz || !apiNodeNum) { logText("[PICO] Wezel: debug_log_api_enabled wylaczone - brak pelnej sekcji security (klucza) z wezla, nie zmieniam. Wlacz recznie\n"); return; }
  // SecurityConfig bez pola 6 + "debug_log_api_enabled = true"
  apiMode = false;
  static uint8_t sec[310]; size_t sl = 0; const uint8_t *p = secRaw, *e = secRaw + secLen;
  while (p < e) {
    const uint8_t* st = p; uint64_t t; if (!pbVarint(p, e, t)) break;
    bool pomin = ((t >> 3) == 6);
    if (!pbSkip(p, e, t & 7)) break;
    if (!pomin) { memcpy(sec + sl, st, p - st); sl += p - st; }
  }
  sec[sl++] = 0x30; sec[sl++] = 0x01;
  static uint8_t adm[330], pkt[350], tr[360], fr[370]; size_t a = 0, d = 0, k = 0, t = 0;
  adm[a++] = 0x92; adm[a++] = 0x02;                     // AdminMessage.set_config (34)
  { static uint8_t c[320]; size_t cl = 0; c[cl++] = 0x42; cl += putVar(c + cl, sl); memcpy(c + cl, sec, sl); cl += sl;   // Config.security (8)
    a += putVar(adm + a, cl); memcpy(adm + a, c, cl); a += cl; }
  static uint8_t dat[340]; dat[d++] = 0x08; dat[d++] = 0x06; dat[d++] = 0x12; d += putVar(dat + d, a); memcpy(dat + d, adm, a); d += a;   // ADMIN_APP
  pkt[k++] = 0x15; put32(pkt + k, apiNodeNum); k += 4;                    // to = wlasny wezel
  pkt[k++] = 0x22; k += putVar(pkt + k, d); memcpy(pkt + k, dat, d); k += d;
  pkt[k++] = 0x35; put32(pkt + k, 0x51000000 | (millis() & 0x00FFFFFF)); k += 4;
  tr[t++] = 0x0A; t += putVar(tr + t, k); memcpy(tr + t, pkt, k); t += k;
  fr[0] = 0x94; fr[1] = 0xC3; fr[2] = t >> 8; fr[3] = t & 0xFF; memcpy(fr + 4, tr, t);
  logText("[PICO] Wezel: security.debug_log_api_enabled wylaczone - wlaczam (opcja debug_log_api_auto_enabled); wezel zrestartuje sie\n");
  if (!apiMode && !apiConnect()) { apiMode = false; logText("[PICO] Wlaczenie debug_log_api_enabled: wezel nie odpowiada\n"); zgloszone = false; return; }
  wlProb++;
  bool ok = usbWriteAll(fr, t + 4);
  apiPump(1500); usbWriteAll(API_DISC, sizeof(API_DISC)); apiPump(300); apiMode = false;
  memset(secRaw, 0, sizeof(secRaw)); memset(sec, 0, sizeof(sec)); memset(adm, 0, sizeof(adm)); memset(fr, 0, sizeof(fr)); memset(tr, 0, sizeof(tr)); memset(pkt, 0, sizeof(pkt)); memset(dat, 0, sizeof(dat)); secLen = 0;  // klucz prywatny nie zostaje w RAM
  logf("[PICO] Wlaczenie debug_log_api_enabled %s\n", ok ? "wyslane - czekam na restart wezla" : "- BLAD wysylania");
  if (!ok) zgloszone = false;
}

bool cfgFail(const char* msg) {
  apiMode = false;
  fwSet(fwPct, "BLAD konfiguracji: %s", msg);
  fwState = 3;
  return false;
}


// Polaczenie API: want_config_id -> my_info z numerem wezla
bool apiConnect() {
  if (brActive) return false;                          // port wezla 4403 zajety przez klienta
  crumb(CR_API);
  lastApiUse = millis();
  if (fwState == 1) fwSet(fwPct < 92 ? 92 : fwPct, "Polaczenie API z wezlem");   // poza operacja (np. odpowiedz LoRa) bez wpisu - nie nadpisuje wyniku
  apiNodeNum = 0; apiConfigDone = false; apiFrames = 0;
  apiMode = true;
  for (int i = 0; i < 3 && !apiNodeNum; i++) {
    if (!usbWriteAll(API_WANTCFG, sizeof(API_WANTCFG))) return false;
    uint32_t t0 = millis();
    while (millis() - t0 < 8000 && !apiNodeNum) apiPump(50);
  }
  if (!apiNodeNum) return false;
  uint32_t t0 = millis();
  while (millis() - t0 < 20000 && !apiConfigDone) apiPump(50);   // odbior biezacej konfiguracji
  apiPump(500);
  return true;
}

// Rozlaczenie API i czekanie na restart wezla (linia "Use nodenum" w logu)
bool apiWaitReboot(uint32_t prevNode) {
  crumb(CR_REBOOTWAIT);
  apiPump(1500);
  usbWriteAll(API_DISC, sizeof(API_DISC));
  apiPump(300);
  apiMode = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 15000 && !logNodeNum) apiPump(100);
  if (!logNodeNum) {                                   // logi stanely - odzyskanie USB i odczyt numeru przez API
    logText("[CFG] Brak logow po restarcie wezla\n");
    if (!waitNodeBoot()) return false;
    apiPump(5000);
    if (!logNodeNum) {
      if (!apiConnect()) { logText("[CFG] Wezel nie odpowiada po restarcie\n"); apiMode = false; return false; }
      logNodeNum = apiNodeNum;
      usbWriteAll(API_DISC, sizeof(API_DISC)); apiPump(300); apiMode = false;
    }
  }
  logf("[CFG] Wezel wystartowal ponownie: nodenum 0x%08lX%s\n", (unsigned long)logNodeNum,
       logNodeNum != prevNode ? " (inny niz przed restartem)" : "");
  return true;
}

// Czeka na start Meshtastic po resecie. Jesli USB milczy (zdarza sie po flashowaniu),
// restartuje host USB - jak odlaczenie i podlaczenie kabla.
// Po resecie wezla: chwila na start, potem restart hosta USB (jak odlaczenie kabla) i czekanie na logi.
// Bez tego odbior z ESP32-S3 czasem staje po starcie nowego programu.
bool waitNodeBoot() {
  crumb(CR_BOOTWAIT);
  logNodeNum = 0;
  lastRxMs = 0;
  // Najpierw bez restartu hosta: restart w trakcie wylaczania/wlaczania USB przez startujacy wezel
  // (pierwszy start po kasowaniu) potrafil zawiesic TinyUSB -> watchdog (test 0.40)
  // Start potwierdza linia "Use nodenum" (logNodeNum); same plynace logi tego nie dowodza (Debug log = logi zawsze)
  { uint32_t t0 = millis();
    while (millis() - t0 < 10000) {
      apiPump(100);
      if (millis() - t0 > 3000 && cdcIdx != 0xFF && lastRxMs && millis() - lastRxMs < 2000 && logNodeNum) {
        logf("[PICO] Wezel wystartowal od nowa (nodenum 0x%08lX), logi plyna - bez restartu hosta USB\n", (unsigned long)logNodeNum);
        return true;
      }
    }
    if (cdcIdx != 0xFF && lastRxMs && millis() - lastRxMs < 2000) {
      logText("[PICO] Logi z wezla plyna, ale nie widac nowego startu wezla (reset nie zadzialal?) - bez restartu hosta USB\n");
      return true;
    } }
  for (int k = 0; k < 2; k++) {
    logf("[PICO] Restart hosta USB po starcie wezla%s\n", k ? " (ponownie)" : "");
    restartUsbHost();
    crumb(CR_BOOTWAIT);
    lastRxMs = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < 40000) {
      apiPump(100);
      if (cdcIdx != 0xFF && lastRxMs && millis() - lastRxMs < 2000) {
        logText("[PICO] Logi z wezla plyna\n");
        return true;
      }
    }
  }
  logText("[PICO] Brak logow z wezla po 2 restartach hosta USB\n");
  return false;
}

bool sendConfig(File& f, const PkgEnt& en, bool afterFlash);
// Konfiguracja zaszyfrowana (pakiet z serwera): "MTX1" + nonce(16) + szyfrogram + znacznik(32).
// Strumien: blok i = HMAC(Ksz, nonce || i), znacznik = HMAC(Kmac, nonce || szyfrogram) - sprawdzany przed odszyfrowaniem.
static bool cfgxOdszyfruj(File& f, uint32_t pos, uint32_t len, String& err) {
  if (len < 4 + 16 + 32 + 8 || len > 16384) { err = "niepoprawny rozmiar zaszyfrowanej konfiguracji"; return false; }
  String kh = readFileStr(CFGX_KEY_PATH); kh.trim();
  uint8_t ks[32], km[32], tag[32], blk[32];
  if (kh.length() != 128 || !hexBytes(kh.substring(0, 64), ks, 32) || !hexBytes(kh.substring(64), km, 32)) { err = "brak klucza konfiguracji - pobierz pakiet ponownie"; return false; }
  uint8_t* b = (uint8_t*)malloc(len);
  if (!b) { err = "brak pamieci"; return false; }
  if (!f.seek(pos) || f.read(b, len) != len || memcmp(b, "MTX1", 4) != 0) { free(b); err = "uszkodzona zaszyfrowana konfiguracja"; return false; }
  uint32_t cl = len - 20 - 32;
  hmacSha256((const char*)km, 32, (const char*)b + 4, 16 + cl, tag);
  if (!ctEq(hexStr(tag, 32), hexStr(b + len - 32, 32))) { free(b); err = "konfiguracja nie przeszla kontroli (inny piasek / haslo?)"; return false; }
  uint8_t in[20]; memcpy(in, b + 4, 16);
  for (uint32_t o = 0, i = 0; o < cl; o += 32, i++) {
    in[16] = i >> 24; in[17] = i >> 16; in[18] = i >> 8; in[19] = i;
    hmacSha256((const char*)ks, 32, (const char*)in, 20, blk);
    for (uint32_t j = 0; j < 32 && o + j < cl; j++) b[20 + o + j] ^= blk[j];
  }
  File d = LittleFS.open(CFGX_DEC_PATH, "w");
  bool ok = d && d.write(b + 20, cl) == cl;
  if (d) d.close();
  memset(b, 0, len); free(b);
  if (!ok) { LittleFS.remove(CFGX_DEC_PATH); err = "blad zapisu"; return false; }
  return true;
}
bool sendConfigAny(File& f, const PkgEnt& en, bool afterFlash) {
  if (en.offset != CFGX_OFFSET) return sendConfig(f, en, afterFlash);
  String err;
  if (!cfgxOdszyfruj(f, en.dataPos, en.csize, err)) return cfgFail(err.c_str());
  File d = LittleFS.open(CFGX_DEC_PATH, "r");
  PkgEnt e2 = en; e2.offset = CFG_OFFSET; e2.dataPos = 0; e2.size = e2.csize = d ? d.size() : 0;
  bool ok = d && sendConfig(d, e2, afterFlash);
  if (d) d.close();
  LittleFS.remove(CFGX_DEC_PATH);                        // odszyfrowana konfiguracja nie zostaje na Pico
  return ok;
}
bool sendConfig(File& f, const PkgEnt& en, bool afterFlash) {
  struct __attribute__((packed)) { char magic[4]; uint8_t ver, count; uint16_t res; char name[40]; } ch;
  if (!f.seek(en.dataPos) || f.read((uint8_t*)&ch, sizeof(ch)) != sizeof(ch) ||
      memcmp(ch.magic, "MTCF", 4) != 0 || ch.ver != 2) return cfgFail("niepoprawny blok konfiguracji (zbuduj pakiet nowym skryptem)");
  ch.name[39] = 0;

  if (cdcIdx == 0xFF) return cfgFail("brak ESP32-S3 na USB");

  if (!apiConnect()) return cfgFail("wezel nie odpowiada w trybie API");
  logf("[CFG] Wezel 0x%08lX, odebrano %lu ramek. Konfiguracja '%s': %u komend\n",
       (unsigned long)apiNodeNum, (unsigned long)apiFrames, ch.name, ch.count);

  static uint8_t buf[520];
  int okCnt = 0, badCnt = 0;
  for (int i = 0; i < ch.count; i++) {
    struct __attribute__((packed)) { uint8_t flags, nameLen; uint16_t toOff, idOff, len; } fh;
    char nm[41] = {0};
    if (f.read((uint8_t*)&fh, sizeof(fh)) != sizeof(fh) || fh.len > sizeof(buf) || fh.len < 8 || fh.nameLen > 40 ||
        4u + fh.toOff + 4u > fh.len || 4u + fh.idOff + 4u > fh.len || f.read(buf, fh.len) != fh.len ||
        f.read((uint8_t*)nm, fh.nameLen) != fh.nameLen) return cfgFail("uszkodzona ramka");
    put32(buf + 4 + fh.toOff, apiNodeNum);
    uint32_t id0 = buf[4 + fh.idOff] | (buf[5 + fh.idOff] << 8) | (buf[6 + fh.idOff] << 16) | ((uint32_t)buf[7 + fh.idOff] << 24);
    bool last = (i == ch.count - 1);
    bool restart = (fh.flags & 0x02) && !last;
    if (last || restart) logNodeNum = 0;                // commit -> restart wezla
    fwSet(92 + 7 * (i + 1) / ch.count, "Konfiguracja %d/%d: %s", i + 1, ch.count, nm);
    crumb(CR_CFG, i + 1);

    // Wyslanie z potwierdzeniem; bez potwierdzenia - do 3 prob (z nowym id). Commit wysylany raz.
    bool acked = false;
    for (int proba = 0; proba < 3 && !acked; proba++) {
      uint32_t id = id0 ^ ((uint32_t)proba << 24);
      put32(buf + 4 + fh.idOff, id);
      ackId = 0;
      if (!usbWriteAll(buf, fh.len)) return cfgFail("zapis USB");
      uint32_t t1 = millis();
      while (millis() - t1 < ACK_TIMEOUT_MS && ackId != id) apiPump(20);
      if (ackId == id) {
        acked = true;
        if (ackErr && proba > 0) {     // pierwsza proba zwykle doszla, tylko potwierdzenie sie spoznilo
          okCnt++;
          logf("[CFG] %d. %s: ponowienie odrzucone (blad %u) - pierwsza proba najpewniej zastosowana\n", i + 1, nm, ackErr);
        }
        else if (ackErr == 8) {          // NO_RESPONSE: komenda przyjeta, wezel nie odeslal odpowiedzi (np. region LoRa)
          okCnt++; logf("[CFG] %d. %s: przyjete bez odpowiedzi (kod 8) - traktuje jako OK\n", i + 1, nm);
        }
        else if (ackErr) { badCnt++; logf("[CFG] %d. %s: ODRZUCONE przez wezel (blad %u)\n", i + 1, nm, ackErr); }
        else okCnt++;
      } else if (last || restart) break;
      else logf("[CFG] %d. %s: brak potwierdzenia, proba %d/3\n", i + 1, nm, proba + 1);
    }
    if (!acked) {
      if (last || restart) logf("[CFG] %d. %s: brak potwierdzenia (wezel mogl juz zaczac restart)\n", i + 1, nm);
      else { badCnt++; logf("[CFG] %d. %s: NIE DOSZLO\n", i + 1, nm); }
    }
    apiPump(150);
    if (restart) {                                       // dalsze komendy dopiero po restarcie
      fwSet(fwPct, "Czekam na restart wezla przed kolejnymi komendami");
      uint32_t prev = apiNodeNum;
      if (!apiWaitReboot(prev)) return cfgFail("wezel nie wystartowal po zapisie");
      apiPump(10000);
      if (!apiConnect()) return cfgFail("brak API po restarcie");
    }
  }
  logf("[CFG] Potwierdzone komendy: %d/%d, nieudane: %d\n", okCnt, ch.count, badCnt);
  fwSet(99, "Konfiguracja wyslana, czekam na restart wezla");
  apiWaitReboot(apiNodeNum);
  if (badCnt) { char m[48]; snprintf(m, sizeof(m), "%d z %d komend nie przeszlo", badCnt, ch.count); return cfgFail(m); }
  return true;
}

// Raport LoRa po wgraniu (na kanale komend), wysylany z opoznieniem przez rdzen 1
String fwVersionFromPkg() {
  String l = infoLine(readFileStr(PKG_INFO_PATH), "firmware:");
  if (l.length() < 11) return String("?");
  l = l.substring(10); int sp = l.indexOf(' '); return sp > 0 ? l.substring(0, sp) : l;
}
void queueLora(const String& txt, uint32_t delayMs, bool needName) {
  bool ok = false;
  mutex_enter_blocking(&loraQMtx);
  if (loraQn < sizeof(loraQ) / sizeof(loraQ[0])) {
    LoraMsg& m = loraQ[loraQn++];
    strncpy(m.txt, txt.c_str(), sizeof(m.txt) - 1); m.txt[sizeof(m.txt) - 1] = 0;
    m.born = millis(); m.at = m.born + delayMs; m.needName = needName; ok = true;
  }
  mutex_exit(&loraQMtx);
  if (!ok) logText("[PICO] Kolejka LoRa pelna - wiadomosc pominieta\n");
  else if (delayMs) logf("[PICO] Wiadomosc LoRa za %lu s: %s\n", (unsigned long)(delayMs / 1000), needName ? "start wezla" : txt.c_str());
}
void queueLoraReport(const String& txt, uint32_t delayMs) { queueLora(txt, delayMs); }
// Czas startu Pico (= wlaczenia zasilania wezla)
String bootTimeStr() {
  uint32_t up = millis() / 1000; char b[48];
  if (timeFromNtp()) { time_t t = time(nullptr) - up; struct tm tmv; localtime_r(&t, &tmv); strftime(b, sizeof(b), "%Y-%m-%d %H:%M", &tmv); return b; }
  if (nodeTimeOk) {
    uint32_t sec = (nodeTodSec + (millis() - nodeTodMs) / 1000 + 86400 - up % 86400) % 86400;
    snprintf(b, sizeof(b), "o %02lu:%02lu (data nieznana)", (unsigned long)(sec / 3600), (unsigned long)(sec / 60 % 60)); return b;
  }
  return String("(czas nieznany, ") + String(up / 60) + " min temu)";
}
// Wysylanie kolejki (rdzen 1): najwczesniej zaplanowana wiadomosc, co najmniej 5 s odstepu
void loraQService() {
  static uint32_t lastSent = 0;
  if (!loraQn || seq || fwState == 1 || millis() - lastSent < 5000) return;
  int best = -1; LoraMsg m;
  mutex_enter_blocking(&loraQMtx);
  for (int i = 0; i < loraQn; i++) if (best < 0 || (int32_t)(loraQ[i].at - loraQ[best].at) < 0) best = i;
  m = loraQ[best];
  bool due = (int32_t)(millis() - m.at) >= 0;
  bool drop = due && cdcIdx == 0xFF && millis() - m.at > 900000;     // 15 min bez wezla - porzuc
  bool go = due && (cdcIdx != 0xFF || drop);
  if (go) { for (int i = best + 1; i < loraQn; i++) loraQ[i - 1] = loraQ[i]; loraQn--; }
  mutex_exit(&loraQMtx);
  if (!go) return;
  if (drop) { logText("[PICO] Wiadomosc LoRa porzucona - brak wezla na USB\n"); return; }
  if (m.needName) {                                      // tresc z wersja firmware odczytana teraz z wezla
    if (!apiConnect()) { apiMode = false; logText("[PICO] WERSJE: wezel nie odpowiada\n"); lastSent = millis(); return; }
    String t = m.txt; t.replace("%FW%", nodeFw[0] ? nodeFw : "nieznana");
    apiSendText(cfg.ch, t.c_str(), true);
  } else apiSendText(cfg.ch, m.txt);
  lastSent = millis();
}
// Raport o wlaczeniu zasilania: odliczanie 2 min od polaczenia z wezlem, potem nazwa wezla (API)
// i czekanie do 8 min na date z NTP albo z sieci LoRa (rx_time pakietow); potem wysylka tak czy inaczej
void bootReportService() {
  static uint8_t st = 0, tries = 0; static uint32_t t0 = 0;
  if (!bootReportWanted || st == 3 || fwState == 1 || seq) return;
  if (st == 0) {
    if (!heltecMounted || cdcIdx == 0xFF) return;
    st = 1; t0 = millis(); logText("[PICO] Wezel podlaczony - raport o wlaczeniu zasilania za 2 min\n"); return;
  }
  if (st == 1) {
    if (millis() - t0 < BOOT_REPORT_MS) return;
    if (!apiConnect()) {
      apiMode = false;
      if (++tries >= 10) { st = 3; logText("[PICO] Raport o zasilaniu: wezel nie odpowiada - rezygnuje\n"); }
      else t0 = millis() - BOOT_REPORT_MS + 60000;            // ponow za minute
      return;
    }
    st = 2; t0 = millis();
    if (!timeFromNtp()) logText("[PICO] Raport o zasilaniu: czekam na date z sieci LoRa (max 8 min)\n");
  }
  if (st == 2) {
    if (!timeFromNtp() && millis() - t0 < 480000) return;
    char id[12]; snprintf(id, sizeof(id), "!%08lx", (unsigned long)apiNodeNum);
    String who = nodeLong[0] ? String("'") + nodeLong + "' (" + (nodeShort[0] ? String(nodeShort) + ", " : String("")) + id + ")" : String(id);
    queueLora(String("PICO: podano zasilanie - Pico i wezel ") + who + " wystartowaly " + bootTimeStr(), 0);
    st = 3;
  }
}

// Wysyla sama konfiguracje zapisana w pakiecie (przycisk na stronie / skrypt)
bool configFromPackage() {
  fwState = 1;
  fwSet(0, "Wysylanie konfiguracji z pakietu");
  if (brActive) return cfgFail("port wezla 4403 zajety - rozlacz klienta albo PORT_WEZLA:0");
  if (!pkgCrcOk()) {                                    // pamiec flash Pico mogla uszkodzic pakiet od czasu odbioru
    logText("[PICO] Pakiet USZKODZONY w pamieci Pico (CRC niezgodne) - konfiguracja nie wyslana\n");
    return cfgFail("pakiet uszkodzony w pamieci Pico (CRC) - wyslij/pobierz ponownie");
  }
  File f = LittleFS.open(PKG_PATH, "r");
  if (!f) return cfgFail("brak pakietu na Pico");
  PkgHdr h;
  if (f.read((uint8_t*)&h, sizeof(h)) != sizeof(h) || memcmp(h.magic, "MTPK", 4) != 0 || h.count > 8) {
    f.close(); return cfgFail("niepoprawny pakiet");
  }
  PkgEnt e; bool found = false;
  for (int i = 0; i < h.count && !found; i++)
    if (f.read((uint8_t*)&e, sizeof(e)) == sizeof(e) && (e.offset == CFG_OFFSET || e.offset == CFGX_OFFSET)) found = true;
  if (!found) { f.close(); return cfgFail("w pakiecie nie ma konfiguracji"); }
  bool ok = sendConfigAny(f, e, false);
  f.close();
  queueLoraReport(ok ? String("PICO: konfiguracja wgrana OK") : String("PICO: BLAD konfiguracji - ") + fwMsg, LORA_REPORT_CFG_MS);
  if (!ok) return false;
  noteNodeInfo(false, true, false);
  fwSet(100, "KONFIGURACJA WYSLANA");
  fwState = 2;
  return true;
}

// Ponowna kontrola CRC pakietu tuz przed wgraniem (pakiet mogl lezec na Pico kilka dni)
bool pkgCrcOk() {
  String exp = readFileStr(PKG_OK_PATH); exp.trim();
  if (!exp.length()) return false;
  static uint8_t vb[1024];
  File f = LittleFS.open(PKG_PATH, "r"); if (!f) return false;
  uint32_t crc = 0xFFFFFFFF; size_t n;
  while ((n = f.read(vb, sizeof(vb))) > 0) { crc = crc32Calc(crc, vb, n); core1Beat = millis(); }
  f.close();
  return (~crc) == strtoul(exp.c_str(), nullptr, 16);
}
// ---- Wersje (od 0.29): wgrywanie tylko tej samej albo nowszej wersji; starsza tylko z wlaczonym downgrade (24 h, jednorazowo).
// Porownanie liczbowe kolejnych czesci: 2.8.3.f99f888 -> 2, 8, 3 (skrot commita pomijany); 0.29 < 0.30 < 1.0.
int verCmp(const String& a, const String& b) {
  unsigned ia = 0, ib = 0;
  for (int k = 0; k < 5; k++) {
    long x = 0, y = 0; bool ha = false, hb = false;
    while (ia < a.length() && isdigit(a[ia])) { x = x * 10 + (a[ia] - '0'); ia++; ha = true; }
    while (ib < b.length() && isdigit(b[ib])) { y = y * 10 + (b[ib] - '0'); ib++; hb = true; }
    if (!ha && !hb) break;
    if (x != y) return x < y ? -1 : 1;
    bool da = ia < a.length() && a[ia] == '.', db = ib < b.length() && b[ib] == '.';
    if (da) ia++; else ia = a.length();
    if (db) ib++; else ib = b.length();
  }
  return 0;
}
bool downgradeOk() {
  time_t t = time(nullptr);
  return cfg.dgDo && t > 1700000000 && (uint32_t)t < cfg.dgDo;
}
void downgradeUstaw(bool wl) {
  time_t t = time(nullptr);
  cfg.dgDo = (wl && t > 1700000000) ? (uint32_t)t + 86400UL : 0;
  saveSettings();
}
String downgradeTxt() {
  if (!downgradeOk()) return "wylaczony";
  time_t t = cfg.dgDo; struct tm tmv; localtime_r(&t, &tmv);
  char b[40]; strftime(b, sizeof(b), "DOZWOLONY do %d.%m %H:%M", &tmv); return b;
}
// Poprawny format wersji przed zapamietaniem: "2.7.15" + opcjonalnie ".hash" / "-alpha" (max 40 znakow)
bool wersjaPoprawna(const String& v) {
  if (v.length() < 5 || v.length() > 40 || !isdigit(v[0])) return false;
  int grupy = 0, i = 0, n = v.length();
  while (grupy < 3) {
    int st = i; while (i < n && isdigit(v[i])) i++;
    if (i == st || i - st > 4) return false;           // pusta albo za dluga liczba
    grupy++;
    if (grupy < 3) { if (i >= n || v[i] != '.') return false; i++; }
  }
  for (; i < n; i++) if (!isalnum(v[i]) && v[i] != '.' && v[i] != '-' && v[i] != '_') return false;
  return true;
}
// Sprawdzenie przed wgraniem: starsza wersja -> odmowa, chyba ze downgrade wlaczony (wtedy zuzyty)
bool wersjaDozwolona(const String& nowa, const String& obecna, const char* co, String& err) {
  if (!nowa.length() || nowa == "?" || !obecna.length()) {
    logf("[PICO] %s: nie mozna porownac wersji (nowa '%s', obecna '%s') - przepuszczam\n", co, nowa.c_str(), obecna.c_str());
    return true;
  }
  if (verCmp(nowa, obecna) >= 0) return true;
  if (downgradeOk()) {
    logf("[PICO] %s: DOWNGRADE %s -> %s (przelacznik wlaczony - teraz zuzyty)\n", co, obecna.c_str(), nowa.c_str());
    downgradeUstaw(false);
    return true;
  }
  err = String(co) + " " + nowa + " jest starszy niz obecny " + obecna + " - aby wgrac: DOWNGRADE:TAK (LoRa) albo przycisk na stronie";
  logf("[PICO] Odrzucono: %s\n", err.c_str());
  return false;
}

bool flashPackage() {
  fwState = 1;
  fwSet(0, "Kontrola pakietu (CRC)");
  if (brActive) return flashFail("port wezla 4403 zajety - rozlacz klienta albo PORT_WEZLA:0");
  if (!pkgCrcOk()) {
    LittleFS.remove(PKG_OK_PATH);
    logText("[PICO] Pakiet USZKODZONY w pamieci Pico (CRC niezgodne) - wezel nietkniety\n");
    queueLora("PICO: BLAD - pakiet uszkodzony w pamieci Pico, wezel nietkniety. Pobierz/wyslij ponownie", 0);
    return flashFail("pakiet uszkodzony w pamieci Pico (CRC) - wyslij/pobierz ponownie");
  }
  {
    String nowa = fwVersionFromPkg(), err;
    if (nowa != "?" && !nodeFw[0] && heltecMounted) apiProbe();      // wersja dzialajaca na wezle (API)
    String obecna = nodeFw;
    if (obecna.length() && !wersjaPoprawna(obecna)) { logf("[PICO] Wersja z wezla w zlym formacie ('%s') - pomijam\n", obecna.c_str()); obecna = ""; }
    if (obecna.length() && obecna != cfg.fwOst) { cfg.fwOst = obecna; saveSettings(); }   // zapamietaj ostatnia znana
    if (!obecna.length() && wersjaPoprawna(cfg.fwOst)) {      // wezel nie odpowiada: porownanie z ostatnia znana wersja
      obecna = cfg.fwOst;
      logf("[PICO] Wezel nie podal wersji - porownuje z ostatnia znana: %s\n", obecna.c_str());
    }
    if (!wersjaDozwolona(nowa, obecna, "Firmware", err)) {
      queueLora("PICO: BLAD - " + err, 0);
      return flashFail("starsza wersja firmware - wlacz downgrade (24 h)");
    }
  }
  fwSet(0, "Start flashowania");

  File f = LittleFS.open(PKG_PATH, "r");
  if (!f) return flashFail("brak pakietu na Pico");

  PkgHdr h;
  if (f.read((uint8_t*)&h, sizeof(h)) != sizeof(h) || memcmp(h.magic, "MTPK", 4) != 0 || h.version != 1 ||
      h.count == 0 || h.count > 8) { f.close(); return flashFail("niepoprawny pakiet"); }
  PkgEnt e[8];
  for (int i = 0; i < h.count; i++)
    if (f.read((uint8_t*)&e[i], sizeof(PkgEnt)) != sizeof(PkgEnt)) { f.close(); return flashFail("uszkodzony pakiet"); }

  uint32_t totalC = 0;
  int cfgIdx = -1, nFlash = 0;
  for (int i = 0; i < h.count; i++) {
    e[i].name[31] = 0;
    if (e[i].offset == INFO_OFFSET) continue;
    if (e[i].offset == CFG_OFFSET || e[i].offset == CFGX_OFFSET) { cfgIdx = i; logf("[FLASH] Konfiguracja%s: %lu B\n", e[i].offset == CFGX_OFFSET ? " (zaszyfrowana)" : "", (unsigned long)e[i].size); continue; }
    logf("[FLASH] Obraz %d: %s @0x%06lX, %lu B (skompresowany %lu B)\n", i + 1, e[i].name,
         (unsigned long)e[i].offset, (unsigned long)e[i].size, (unsigned long)e[i].csize);
    totalC += e[i].csize;
    nFlash++;
  }

  if (cdcIdx == 0xFF) { f.close(); return flashFail("brak ESP32-S3 na USB"); }

  if (nFlash == 0) {                                     // sama konfiguracja, bez flashowania
    bool ok = cfgIdx >= 0 && sendConfigAny(f, e[cfgIdx], false);
    f.close();
    if (cfgIdx >= 0) queueLoraReport(ok ? String("PICO: konfiguracja wgrana OK") : String("PICO: BLAD konfiguracji - ") + fwMsg, LORA_REPORT_CFG_MS);
    if (!ok) return cfgIdx >= 0 ? false : flashFail("pusty pakiet");
    noteNodeInfo(false, true, false);
    fwSet(100, "KONFIGURACJA WYSLANA");
    fwState = 2;
    return true;
  }

  etapZapisz(1, etapProba);                             // od teraz wezel moze zostac bez programu - ponowimy po restarcie Pico
  fwSet(1, "Wejscie w bootloader");
  crumb(CR_FLASH);
  if (!espEnterAndSync()) { f.close(); return flashFail("bootloader nie odpowiada"); }

  // SPI_ATTACH (ROM: 8 bajtów zer)
  uint8_t p[24] = {0};
  if (espCmd(ESP_SPI_ATTACH, p, 8, 0, 3000) != 1) { f.close(); return flashFail("SPI_ATTACH"); }

  // SPI_SET_PARAMS: id, rozmiar, blok 64K, sektor 4K, strona 256, maska statusu
  put32(p, 0); put32(p + 4, h.flashSize); put32(p + 8, 0x10000); put32(p + 12, 0x1000);
  put32(p + 16, 0x100); put32(p + 20, 0xFFFF);
  if (espCmd(ESP_SPI_SET_PARAMS, p, 24, 0, 3000) != 1) { f.close(); return flashFail("SPI_SET_PARAMS"); }

  // Pełne kasowanie: FLASH_BEGIN na całym obszarze (ROM kasuje przy BEGIN).
  // eraseEnd == 0 -> aktualizacja bez pełnego kasowania (kasowane są tylko obszary zapisywanych obrazów).
  if (h.eraseEnd > 0) {
    fwSet(2, "Kasowanie flasha 0x0-0x%06lX (moze potrwac kilka minut)", (unsigned long)h.eraseEnd);
    put32(p, h.eraseEnd); put32(p + 4, 0); put32(p + 8, ESP_BLOCK); put32(p + 12, 0); put32(p + 16, 0);
    if (espCmd(ESP_FLASH_BEGIN, p, 20, 0, timeoutPerMb(30000, h.eraseEnd) + 5000) != 1) {
      f.close(); return flashFail("kasowanie flasha");
    }
    fwSet(10, "Flash skasowany");
  } else {
    fwSet(10, "Bez pelnego kasowania (aktualizacja)");
  }

  // Zapis obrazów
  static uint8_t pay[16 + ESP_BLOCK];
  uint32_t sentC = 0;
  for (int i = 0; i < h.count; i++) {
    if (e[i].offset >= CFGX_OFFSET) continue;         // konfiguracja / opis - nie do flasha
    uint32_t eraseBlocks = (e[i].size + ESP_BLOCK - 1) / ESP_BLOCK;
    uint32_t numBlocks   = (e[i].csize + ESP_BLOCK - 1) / ESP_BLOCK;
    put32(p, eraseBlocks * ESP_BLOCK); put32(p + 4, numBlocks); put32(p + 8, ESP_BLOCK);
    put32(p + 12, e[i].offset); put32(p + 16, 0);
    fwSet(fwPct, "Zapis %s: przygotowanie", e[i].name);
    if (espCmd(ESP_DEFL_BEGIN, p, 20, 0, timeoutPerMb(30000, eraseBlocks * ESP_BLOCK) + 3000) != 1) {
      f.close(); return flashFail("DEFL_BEGIN");
    }
    if (!f.seek(e[i].dataPos)) { f.close(); return flashFail("odczyt pakietu"); }

    int lastPct = -1;
    for (uint32_t seqNo = 0; seqNo < numBlocks; seqNo++) {
      uint32_t n = e[i].csize - seqNo * ESP_BLOCK;
      if (n > ESP_BLOCK) n = ESP_BLOCK;
      if (f.read(pay + 16, n) != (int)n) { f.close(); return flashFail("odczyt pakietu"); }
      put32(pay, n); put32(pay + 4, seqNo); put32(pay + 8, 0); put32(pay + 12, 0);
      uint32_t chk = 0xEF;
      for (uint32_t k = 0; k < n; k++) chk ^= pay[16 + k];
      int r = espCmd(ESP_DEFL_DATA, pay, 16 + n, chk, 10000);
      if (r != 1) {
        char m[64]; snprintf(m, sizeof(m), "zapis %s, blok %lu/%lu", e[i].name,
                             (unsigned long)seqNo + 1, (unsigned long)numBlocks);
        f.close(); return flashFail(m);
      }
      sentC += n;
      int pct = 10 + (int)((uint64_t)sentC * (cfgIdx >= 0 ? 78 : 85) / totalC);
      if (pct != lastPct && (pct % 5 == 0)) { fwSet(pct, "Zapis %s: %lu/%lu blokow", e[i].name,
                                                    (unsigned long)seqNo + 1, (unsigned long)numBlocks); }
      lastPct = pct;
    }

    // Weryfikacja MD5 liczona przez ESP32 z zawartości flasha
    fwSet(fwPct, "Weryfikacja MD5: %s", e[i].name);
    put32(p, e[i].offset); put32(p + 4, e[i].size); put32(p + 8, 0); put32(p + 12, 0);
    uint8_t md5r[40]; size_t md5len = sizeof(md5r);
    if (espCmd(ESP_MD5, p, 16, 0, timeoutPerMb(10000, e[i].size) + 3000, nullptr, md5r, &md5len) != 1) {
      f.close(); return flashFail("odczyt MD5");
    }
    char expect[33];
    for (int k = 0; k < 16; k++) snprintf(expect + 2 * k, 3, "%02x", e[i].md5[k]);
    char got[33] = {0};
    if (md5len >= 32) { memcpy(got, md5r, 32); for (int k = 0; k < 32; k++) got[k] = tolower(got[k]); }
    else { for (size_t k = 0; k < md5len && k < 16; k++) snprintf(got + 2 * k, 3, "%02x", md5r[k]); }
    if (strcmp(expect, got) != 0) {
      logf("[FLASH] MD5 oczekiwane %s, w ESP32 %s\n", expect, got);
      f.close(); return flashFail("niezgodne MD5");
    }
    logf("[FLASH] MD5 OK (%s)\n", got);
  }
  if (cfgIdx >= 0) etapZapisz(2, 0);                    // obrazy OK; gdyby Pico padlo przy starcie wezla - konfiguracja po restarcie
  else { LittleFS.remove(ETAP_PATH); logText("[FLASH] Dziennik: etap 1 potwierdzony - obrazy zweryfikowane, koniec\n"); }
  fwSet(cfgIdx >= 0 ? 89 : 98, "Restart do nowego firmware (RTS)");
  runSeqBlocking(SEQ_RESET, 2);
  fwSet(cfgIdx >= 0 ? 90 : 99, "Czekam na start wezla");
  bool up = waitNodeBoot();
  if (cfgIdx < 0) {
    f.close();
    if (!up) logText("[FLASH] UWAGA: brak logow z wezla po starcie (firmware wgrany poprawnie)\n");
    noteNodeInfo(true, false, h.eraseEnd > 0);
    // po pelnym kasowaniu bez konfiguracji wezel nie ma regionu ani kanalu - raportu nie da sie wyslac
    if (h.eraseEnd == 0)
    {
      String info = readFileStr(PKG_INFO_PATH), o = infoLine(info, "ota:");
      String co = infoLine(info, "tryb:").indexOf("tylko OTA") >= 0
                  ? String("PICO: OTA ") + (o.length() > 5 ? o.substring(5, o.indexOf(' ', 5) > 0 ? o.indexOf(' ', 5) : o.length()) : String("?")) + " wgrane"
                  : String("PICO: aktualizacja FW ") + fwVersionFromPkg();
      queueLoraReport(co + (up ? " OK" : ", brak logow wezla"), LORA_REPORT_UPD_MS);
    }
    { String v = fwVersionFromPkg(); if (wersjaPoprawna(v)) { cfg.fwOst = v; saveSettings(); } }
    fwSet(100, "ZAKONCZONE POMYSLNIE");
    fwState = 2;
    return true;
  }
  logText("[FLASH] Firmware wgrany i zweryfikowany. Teraz konfiguracja.\n");
  { String v = fwVersionFromPkg(); if (wersjaPoprawna(v)) { cfg.fwOst = v; saveSettings(); } }
  fwSet(91, "Czekam %lu s na pierwszy start wezla (klucze, zapis ustawien)", (unsigned long)(CFG_BOOT_WAIT_MS / 1000));
  apiPump(CFG_BOOT_WAIT_MS);
  bool ok = sendConfigAny(f, e[cfgIdx], false);
  if (!ok) {                                            // jedna powtorka: reset wezla + restart USB + dluzsza przerwa
    logText("[CFG] Konfiguracja nieudana - ponawiam: reset wezla, restart USB\n");
    fwState = 1;
    fwSet(91, "Ponowienie: reset wezla");
    runSeqBlocking(SEQ_RESET, 2);
    waitNodeBoot();
    fwSet(91, "Ponowienie: czekam %lu s na start wezla", (unsigned long)(CFG_RETRY_WAIT_MS / 1000));
    apiPump(CFG_RETRY_WAIT_MS);
    ok = sendConfigAny(f, e[cfgIdx], false);
  }
  LittleFS.remove(ETAP_PATH);
  logText(ok ? "[FLASH] Dziennik: etap 2 potwierdzony - konfiguracja wyslana, koniec\n" : "[FLASH] Dziennik: etap 2 nieudany - zamkniety (blad zgloszony)\n");
  f.close();
  noteNodeInfo(true, ok, h.eraseEnd > 0);
  queueLoraReport(String("PICO: FW ") + fwVersionFromPkg() + (ok ? " + konfiguracja OK" : " OK, BLAD konfiguracji"), LORA_REPORT_CFG_MS);
  if (!ok) return false;
  fwSet(100, "ZAKONCZONE POMYSLNIE (firmware + konfiguracja)");
  fwState = 2;
  return true;
}

// =========================================================================
// RDZEŃ 0: Wi-Fi + strona WWW + przyjmowanie pakietu
// =========================================================================
const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<title>Pico2W flasher</title></head>
<body style="font-family:sans-serif;background:#111;color:#eee;margin:15px">
<h3>Pico 2 W - host USB / flasher ESP32-S3 <span id="ver" style="font-weight:normal;font-size:13px;color:#888"></span></h3>
<div id="st">...</div>
<div style="position:fixed;bottom:4px;right:10px;font-size:11px;color:#777;z-index:8">Pomyslodawca projektu: Rafal Cichosz (rpc)</div>
<div id="prog" style="display:none;position:fixed;bottom:24px;right:10px;min-width:260px;max-width:420px;background:#223;
 border:2px solid #58f;padding:8px 12px;font-weight:bold;z-index:9;box-shadow:0 0 8px #000">
 <div id="progT">...</div>
 <div style="background:#000;height:10px;margin-top:6px"><div id="progB" style="background:#4c4;height:10px;width:0"></div></div>
 <div id="progM" style="font-weight:normal;font-size:12px;margin-top:4px;color:#aaa"></div></div>
<p><button onclick="togInfo()" id="infoBtn" style="padding:6px 14px">Informacja o wersjach: POKAZ</button></p>
<pre id="info" style="display:none;background:#222;color:#bdf;padding:8px;white-space:pre-wrap;margin:8px 0">...</pre>
<div id="busy" style="display:none;background:#a60;color:#fff;padding:8px;margin:8px 0;font-weight:bold">
TRWA ODBIOR PAKIETU FIRMWARE - strona nie odpytuje Pico, zeby nie przeszkadzac. Sprawdzi stan sama po szacowanym czasie odbioru: <span id="eta"></span>
<button onclick="tick()" style="margin-left:15px;padding:4px 12px">Sprawdz stan teraz</button></div>
<div id="zmien" style="display:none;position:fixed;inset:0;background:#000d;z-index:21">
 <div style="max-width:360px;margin:10vh auto;background:#223;padding:20px;border:2px solid #f80">
  <b>Ustaw nowe hasla</b><p style="color:#aaa;font-size:12px">Domyslne lub za krotkie hasla - flashowanie i pobieranie sa zablokowane. Kazde haslo min. 12 znakow, inne niz domyslne.</p>
  <p>Nowe haslo strony (token):<br><input id="zT1" type="password" maxlength="63" style="width:100%;padding:5px"></p>
  <p>Powtorz:<br><input id="zT2" type="password" maxlength="63" style="width:100%;padding:5px"></p>
  <p>Nowe haslo AP (Wi-Fi Pico):<br><input id="zA1" type="password" maxlength="63" style="width:100%;padding:5px"></p>
  <p>Powtorz:<br><input id="zA2" type="password" maxlength="63" style="width:100%;padding:5px"></p>
  <p><button onclick="zmienHasla()" style="padding:6px 18px">Zapisz</button> <button onclick="document.getElementById('zmien').style.display='none'" style="padding:6px 12px">Pozniej</button>
  <span id="zerr" style="color:#f66"></span></p>
 </div></div>
<div id="login" style="display:none;position:fixed;inset:0;background:#000d;z-index:20">
 <div style="max-width:320px;margin:15vh auto;background:#223;padding:20px;border:2px solid #58f">
  <b>Pico2W flasher</b><p>Haslo:</p>
  <input id="pw" type="password" style="width:100%;padding:6px" onkeydown="if(event.key=='Enter')zaloguj()">
  <p><button onclick="zaloguj()" style="padding:6px 18px">Zaloguj</button> <span id="lerr" style="color:#f66"></span></p>
 </div></div>
<p>
<button onclick="logout()" style="padding:3px 10px;background:#633;color:#fff">Wyloguj</button>
<button onclick="togSet()" id="setBtn" style="padding:3px 10px">Ustawienia: POKAZ</button>
<button onclick="togLog()" id="logBtn" style="padding:3px 10px">Zbieranie logow</button>
<span id="solTxt" style="margin-left:10px;color:#aaa;font-family:monospace" title="Sol hasla - jawna; potrzebna skryptowi do --przygotuj-serwer bez polaczenia z Pico (--sol). Zmienia sie przy zmianie hasla."></span>
</p>
<style>#setp td{padding:3px 6px;vertical-align:middle}#setp td:first-child{white-space:nowrap;width:1%}#setp input:not([type=radio]):not([type=number]),#setp select{min-width:220px}#setp input.ip:not([type=radio]):not([type=number]){min-width:0;width:125px}#setp td.ipw{white-space:nowrap}</style>
<div id="setp" style="display:none;background:#222;padding:10px;margin:8px 0;max-width:1100px">
 <b>Wi-Fi i haslo</b> <span style="color:#aaa">(puste pole hasla = bez zmian; po zapisie Pico sie zrestartuje)</span>
 <table>
 <tr><td>Tryb</td><td><label><input type="radio" name="md" id="mAP" value="0"> AP (wlasna siec Pico)</label>
   <label><input type="radio" name="md" id="mSTA" value="1"> STA (klient Twojej sieci)</label></td></tr>
 <tr><td>Nazwa sieci AP</td><td><input id="sApS" maxlength="32"></td></tr>
 <tr><td>Haslo AP (12-63)</td><td><input id="sApP" type="password" maxlength="63"></td></tr>
 <tr><td>Haslo AP - powtorz</td><td><input id="sApP2" type="password" maxlength="63"></td></tr>
 <tr><td>Tryb sieci</td><td><select id="sNet"><option value="wifi">Wi-Fi (wg trybu AP/STA)</option><option value="eth">Ethernet (Wi-Fi wylaczone; bez modulu - Wi-Fi awaryjnie)</option><option value="oba">oba - Wi-Fi i Ethernet</option><option value="brak">brak - Wi-Fi wylaczone (tylko LoRa)</option></select></td></tr>
 <tr><td>Ostrzezenia zasobow przez LoRa</td><td><select id="sZl"><option value="1">tak - alarm (malo RAM, wyciek, przeciazenie, malo flasha) idzie na kanal komend</option><option value="0">nie - tylko wpis w zdarzeniach (stan: komenda ZASOBY)</option></select></td></tr>
 <tr><td>security.debug_log_api_auto_enabled</td><td><select id="sLa"><option value="1">tak - Pico sam wlacza debug_log_api_enabled w wezle (logi plyna tez w trybie API; jednorazowy restart wezla)</option><option value="0">nie</option></select></td></tr>
 <tr><td>Modul Ethernet</td><td><select id="sEm"><option value="0">W5500-mini (SPI1: GP8-13)</option><option value="1">WIZnet Ethernet HAT W5100S (SPI0: GP16-21)</option></select></td></tr>
 <tr><td>Siec STA (SSID)</td><td><input id="sStS" maxlength="32"></td></tr>
 <tr><td>Haslo sieci STA</td><td><input id="sStP" type="password" maxlength="63"></td></tr>
 <tr><td>Staly adres Wi-Fi (STA)</td><td class="ipw">IP <input id="sSip" class="ip" placeholder="puste = DHCP"> maska <input id="sSmk" class="ip" placeholder="255.255.255.0">
   brama <input id="sSgw" class="ip"> DNS <input id="sSdn" class="ip" placeholder="= brama"></td></tr>
 <tr><td>Staly adres Ethernet</td><td class="ipw">IP <input id="sEip" class="ip" placeholder="puste = DHCP"> maska <input id="sEmk" class="ip" placeholder="255.255.255.0">
   brama <input id="sEgw" class="ip"> DNS <input id="sEdn" class="ip" placeholder="= brama"></td></tr>
 <tr><td>Haslo / token</td><td><input id="sTok" type="password" maxlength="63"> <span style="color:#aaa">(logowanie do strony i skryptu; min. 12 znakow; haslo nie jest wysylane siecia)</span></td></tr>
 <tr><td>Haslo / token - powtorz</td><td><input id="sTok2" type="password" maxlength="63"></td></tr>
 <tr><td>Kanal komend LoRa (0-7)</td><td><input id="sCh" type="number" min="0" max="7" style="width:50px"></td></tr>
 <tr><td>Komenda: wlacz Wi-Fi</td><td><input id="c0" maxlength="20"></td></tr>
 <tr><td>Komenda: uspij (Wi-Fi off)</td><td><input id="c1" maxlength="20"></td></tr>
 <tr><td>Komenda: staly AP wlacz</td><td><input id="c2" maxlength="20"></td></tr>
 <tr><td>Komenda: staly AP wylacz</td><td><input id="c3" maxlength="20"></td></tr>
 <tr><td>Komenda: temperatura</td><td><input id="c4" maxlength="20"></td></tr>
 <tr><td>Dozwoleni nadawcy</td><td><input id="sSnd" placeholder="np. ed819a48,0c838131 (puste = wszyscy)" style="width:260px"></td></tr>
 <tr><td>Adres pliku mtnod.txt</td><td><input id="sUrl" placeholder="https:&#47;&#47;serwer/komin/mtnod.txt (puste = brak)" style="width:260px"></td></tr>
 <tr><td>Strona WWW w przegladarce</td><td><select id="sStr"><option value="1">wlaczona</option><option value="0">wylaczona (po zapisie strona odpowie 403; wlaczysz LoRa STRONA:WL albo skryptem; skrypt dziala zawsze)</option></select></td></tr>
 <tr><td>Hop limit odpowiedzi LoRa</td><td><input id="sHop" type="number" min="0" max="7" style="width:50px"> <span style="color:#aaa">(0-7, domyslnie 3; wiecej = dalszy zasieg, wiekszy ruch w sieci)</span></td></tr>
 <tr><td>Brak odpowiedzi wezla (min)</td><td><input id="sOdp" type="number" min="0" max="1440" style="width:70px"> <span style="color:#aaa">(0 = wylaczone, 5-1440)</span></td></tr>
 <tr><td>POWMAN pobudka</td><td><input id="sPmT" style="width:90px"> <span style="color:#aaa">(0 = bez timera, 12 = co 12 h, 6,18 = o godzinach, 30m = co 30 min, min. 5m)</span></td></tr>
 <tr><td>POWMAN czuwanie (min)</td><td><input id="sPmC" type="number" min="1" max="240" style="width:60px">
   <span id="pmSt" style="color:#aaa"></span> <button onclick="act('Wylaczyc tryb POWMAN (Pico zostanie wlaczone na stale)?','/pm_off')">Wylacz POWMAN</button></td></tr>
 <tr><td>Dozwolone IP (STA)</td><td><input id="sIpA" placeholder="np. 192.168.1.98,192.168.1.* (puste = wszystkie)" style="width:260px"> <span style="color:#aaa">(tylko w trybie STA; w trybie AP dostep zawsze)</span></td></tr>
 <tr><td>Korekta napiecia (V)</td><td><input id="sVk" style="width:60px"> <span style="color:#aaa">(dodawana do pomiaru VSYS, -1.00 do 1.00; zmierz multimetrem)</span></td></tr>
 <tr><td>Prog niskiego napiecia (V)</td><td><input id="sVp" style="width:60px"> <span style="color:#aaa">(0 = wylaczony, np. 4.60 - ostrzezenie w zdarzeniach i LoRa)</span></td></tr>
 <tr><td>Pin resetu wezla (GP)</td><td><input id="sRp" type="number" min="-1" max="28" style="width:50px">
   <select id="sRh"><option value="1">aktywny stan wysoki</option><option value="0">aktywny stan niski</option></select>
   impuls <input id="sRs" type="number" min="1" max="60" style="width:45px"> s <span style="color:#aaa">(-1 = brak; przy Ethernecie zajete: W5500-mini GP8-13, HAT W5100S GP16-21)</span></td></tr>
 </table>
 <p style="color:#aaa;font-size:12px">Komendy wysyla sie jako wiadomosc tekstowa na kanale komend (wielkosc liter bez znaczenia; 3-20 znakow, bez spacji, kazda inna).
 Domyslne: WLACZ_PICO, USPIJ_PICO, WLACZ_TRYB_AP, WYLACZ_TRYB_AP, TEMP_PICO. Pico odpowiada na kanale krotka wiadomoscia.</p>
 <p style="color:#aaa;font-size:12px">Brak odpowiedzi wezla: gdy przez tyle minut z USB nie przyjda zadne dane, Pico pyta wezel przez API. Jesli nie odpowie:
 restart hosta USB, potem reset RTS, na koncu impuls na pinie resetu (max 3 na dobe). Pin steruje zewnetrznym ukladem (np. tranzystor odcinajacy zasilanie wezla).</p>
 <p style="color:#aaa;font-size:12px">POWMAN (gleboki sen Pico, ok. 0,3-1 mA): wlaczasz komenda LoRa USPIJ_POWMAN, potem USPIJ_POWMAN:TAK. Pico budzi sie timerem
 (POWMAN pobudka) na czas czuwania (Wi-Fi i LoRa dzialaja, sprawdza wezel, potem znow spi) albo stanem wysokim na GP14 (wtedy tryb sie wylacza).
 Wylaczenie: WLACZ_PICO w czasie czuwania, przycisk obok, BOOTSEL gdy Pico nie spi. Tryb przetrwa zanik zasilania.</p>
 <button onclick="zapiszUst()" style="padding:6px 14px">Zapisz i zrestartuj Pico</button>
 <button onclick="act('Przywrocic WSZYSTKIE ustawienia domyslne (AP, hasla, kanal) i zrestartowac Pico?','/settings_reset')" style="padding:6px 14px;background:#a33;color:#fff">Ustawienia domyslne</button>
 <p style="color:#aaa;font-size:12px">W trybie STA, gdy Pico nie polaczy sie z siecia w 30 s, uruchomi AP. Krotkie nacisniecie BOOTSEL restartuje wezel; przytrzymanie 30 s przywraca ustawienia domyslne Pico.</p>
</div>
<div style="display:flex;flex-wrap:wrap;gap:6px;margin:8px 0">
<button onclick="act('Zrestartowac host USB w Pico?','/reinit')" style="padding:6px 14px">Restart hosta USB</button>
<button onclick="act('Zresetowac wezel ESP32-S3?','/hreset')" style="padding:6px 14px">Reset ESP32-S3 (RTS)</button>
<button id="bRst" onclick="act('Wyslac impuls resetu na '+rstFiz+'? Uklad podlaczony do pinu zresetuje wezel (np. odetnie mu zasilanie na '+rstSek+' s).','/rst_test')" style="padding:6px 14px">Reset wezla</button>
<button onclick="act('Przelaczyc ESP32-S3 w bootloader? Wezel przestanie dzialac do resetu.','/hboot')" style="padding:6px 14px">ESP32-S3 w bootloader</button>
<button onclick="act('Uruchomic test bootloadera? Wezel na chwile przestanie dzialac.','/chiptest')" style="padding:6px 14px;background:#ff0">Test bootloadera</button>
<button id="flBtn" onclick="flashBtn()" style="padding:6px 14px;background:#f80">Flashuj pakiet z Pico</button>
<button onclick="act('Wyslac do wezla konfiguracje z pakietu zapisanego na Pico? Wezel sie zrestartuje.','/cfg_send')" style="padding:6px 14px;background:#c8f">Wgraj konfiguracje</button>
<button onclick="act('Zrestartowac Pico? Strona wroci po ok. 10 s.','/pico_reboot')" style="padding:6px 14px;background:#a33;color:#fff">Restart Pico</button>
<button onclick="act('Przelaczyc downgrade? Wlaczony pozwala RAZ wgrac starsza wersje (wezla albo Pico) przez 24 h; wlaczony - wylacza.','/downgrade')" style="padding:6px 14px;background:#655;color:#fff">Downgrade (24 h)</button>
<button onclick="portWezla()" style="padding:6px 14px;background:#456;color:#fff">Port wezla 4403</button>
<button id="apOff" onclick="act('Wylaczyc tymczasowy tryb AP i wrocic do trybu z ustawien? Jesli w ustawieniach jest STA, ta strona przestanie dzialac pod 192.168.4.1.','/ap_off')" style="padding:6px 14px;background:#c60;color:#fff;display:none">Koniec trybu AP (wroc do ustawien)</button>
</div>
<div style="display:flex;flex-wrap:wrap;gap:6px;margin:8px 0">
<a id="dl1" href="#" style="padding:6px 14px;background:#0a0;color:#fff;text-decoration:none;border-radius:3px;font-size:13px;font-family:sans-serif">Pobierz log (.txt)</a>
<a id="dl3" href="#" style="padding:6px 14px;background:#555;color:#fff;text-decoration:none;border-radius:3px;font-size:13px;font-family:sans-serif">Pobierz logi z flash (.txt)</a>
<a id="dl5" href="#" style="padding:6px 14px;background:#2a6f6f;color:#fff;text-decoration:none;border-radius:3px;font-size:13px;font-family:sans-serif">Pobierz starty wezla (.txt)</a>
<a id="dl2" href="#" style="padding:6px 14px;background:#06a;color:#fff;text-decoration:none;border-radius:3px;font-size:13px;font-family:sans-serif">Pobierz zdarzenia Pico (.txt)</a>
<a id="dl4" href="#" style="padding:6px 14px;background:#748;color:#fff;text-decoration:none;border-radius:3px;font-size:13px;font-family:sans-serif">Pobierz skrypt wyslij_firmware.py</a>
</div>
<p>
<button id="bp" onclick="tg('p')" style="padding:6px 14px">Zdarzenia Pico: START</button>
<button id="bl" onclick="tg('l')" style="padding:6px 14px">Log ESP32-S3: START</button>
<span style="color:#aaa">(podglad wylaczony = mniej obciazenia Pico; Pico i tak zbiera logi w pamieci)</span>
</p>
<b>Zdarzenia Pico:</b>
<pre id="plog" style="background:#001;color:#ff0;height:18vh;overflow-y:scroll;padding:10px;font-size:12px">(podglad zatrzymany)</pre>
<b>Log (ESP32-S3 + Pico):</b>
<pre id="log" style="background:#000;color:#0f0;height:55vh;overflow-y:scroll;padding:10px;font-size:12px">(podglad zatrzymany)</pre>
<script>
let on = {p:false, l:false};
function tg(k){
  on[k] = !on[k];
  document.getElementById(k=='p'?'bp':'bl').textContent =
    (k=='p'?'Zdarzenia Pico: ':'Log ESP32-S3: ') + (on[k]?'STOP':'START');
}
function czas(){ const d=new Date(), z=n=>String(n).padStart(2,'0');
  return d.getFullYear()+'-'+z(d.getMonth()+1)+'-'+z(d.getDate())+' '+z(d.getHours())+':'+z(d.getMinutes()); }
let infoN=0, lastFl='', infoOn=false, progHidden=false, progTimer=null;
function togInfo(){ infoOn=!infoOn; document.getElementById('info').style.display=infoOn?'block':'none';
  document.getElementById('infoBtn').textContent='Informacja o wersjach: '+(infoOn?'UKRYJ':'POKAZ'); if(infoOn) loadInfo(); }
// Czytelny opis etapu wgrywania (prawy gorny rog)
function etap(m){
  const t=[['Kasowanie','Formatuje ESP32-S3'],['Zapis factory','Wgrywam firmware (factory)'],['Zapis ota','Wgrywam OTA'],
    ['Zapis littlefs','Wgrywam LittleFS'],['Zapis ','Wgrywam'],['Weryfikacja MD5','Sprawdzam sume kontrolna'],['bootloader','Wchodze w bootloader ESP32-S3'],
    ['Restart do nowego','Uruchamiam nowy firmware'],['pierwszy start','Czekam na start wezla (klucze)'],['Czekam','Czekam na wezel'],
    ['Polaczenie API','Lacze sie z wezlem'],['Konfiguracja','Wgrywam konfiguracje'],['Wznowienie','Wznawiam konfiguracje']];
  for(const [k,v] of t) if(m.indexOf(k)>=0) return v;
  return m;
}
async function prog(){
  try{
    const [st,pct,msg] = (await (await F('/fw_status')).text()).split('|');
    const b=document.getElementById('prog');
    const ok=st==='OK', err=st==='BLAD';
    if(!ok && !err) progHidden=false;
    if(st==='BRAK' || ((ok||err) && progHidden)){ b.style.display='none'; return; }
    if((ok||err) && !progTimer) progTimer=setTimeout(()=>{ progHidden=true; progTimer=null; b.style.display='none'; }, 5000);
    b.style.display='block';
    b.style.borderColor = ok?'#4c4':err?'#f44':'#58f';
    document.getElementById('progT').textContent = ok?'Wszystko OK':err?'BLAD':etap(msg)+' ('+pct+'%)';
    document.getElementById('progB').style.width = pct+'%';
    document.getElementById('progB').style.background = err?'#f44':'#4c4';
    document.getElementById('progM').textContent = msg;
  }catch(e){}
}
async function loadInfo(){ try{ document.getElementById('info').textContent = await (await F('/info')).text(); }catch(e){} }
// Logowanie (od 0.29): haslo wysylane raz, potem sesja w ciasteczku (wygasa po 30 min bezczynnosci)
let LIC=Date.now();                                    // licznik podpisanych zadan (rosnacy)
const PN=['X-Offset','X-Chunk-CRC','X-Chunk-SHA','X-Total-Size','X-Total-CRC','X-Target','X-Time'];   // jak PODP_NAGL w Pico
function podpisz(met,url,body,hd){ const k=sessionStorage.getItem('pk'); if(!k) return {};
  const u=new URL(url,location.href), a=[]; LIC++;
  u.searchParams.forEach((v,n)=>a.push([n,v])); if(body instanceof URLSearchParams) body.forEach((v,n)=>a.push([n,v]));
  a.sort((x,y)=>x[0]<y[0]?-1:x[0]>y[0]?1:0);
  let m=met+'\n'+u.pathname+'\n'+LIC+'\n'; for(const [n,v] of a) m+=n+'='+v+'\n';
  for(const n of PN){ const v=hd&&hd[n]; if(v) m+=n+':'+v+'\n'; }
  return {'X-Licznik':String(LIC),'X-Podpis':HX(hmac(UH(k),TE(m)))}; }
async function F(url, opt, raz){ opt=opt||{}; opt.credentials='same-origin';
  const h0=opt.h0||opt.headers||{}; opt.h0=h0;
  opt.headers=Object.assign({},h0,podpisz((opt.method||'GET').toUpperCase(),url,opt.body,h0));
  const r=await fetch(url,opt);
  if(r.status==401&&!raz){ const t=await r.clone().text();       // licznik odrzucony (zadania rownolegle) - jedno ponowienie z nowym licznikiem
    if(t.indexOf('licznik')>=0){ LIC=Math.max(LIC,Date.now()); return F(url,opt,1); } }
  return r; }
async function pobierz(url,nazwa){ const r=await F(url); if(r.status==401){ pokazLogin(); return; }
  if(r.status!=200){ alert(await r.text()); return; }
  const b=await r.blob(), cd=r.headers.get('Content-Disposition')||'', m=cd.match(/filename=\x22?([^\x22;]+)/), a=document.createElement('a');
  a.href=URL.createObjectURL(b); a.download=m?m[1]:nazwa; document.body.appendChild(a); a.click();
  setTimeout(()=>{ URL.revokeObjectURL(a.href); a.remove(); },3000); }
function pokazLogin(){ document.getElementById('login').style.display='block'; document.getElementById('pw').focus(); }
// ---- Logowanie bez wysylania hasla: SHA-256 / HMAC / PBKDF2 w przegladarce (bez HTTPS nie ma crypto.subtle) ----
const SK=new Uint32Array([0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]);
function sha256(m){
  const l=m.length,n=((l+72)>>6)<<6,b=new Uint8Array(n);b.set(m);b[l]=0x80;
  const dv=new DataView(b.buffer);dv.setUint32(n-4,(l*8)>>>0);dv.setUint32(n-8,Math.floor(l/536870912));
  const h=new Uint32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]),w=new Uint32Array(64);
  for(let o=0;o<n;o+=64){
    for(let i=0;i<16;i++)w[i]=dv.getUint32(o+i*4);
    for(let i=16;i<64;i++){const a=w[i-15],c=w[i-2];
      w[i]=(w[i-16]+(((a>>>7)|(a<<25))^((a>>>18)|(a<<14))^(a>>>3))+w[i-7]+(((c>>>17)|(c<<15))^((c>>>19)|(c<<13))^(c>>>10)))|0;}
    let A=h[0],B=h[1],C=h[2],D=h[3],E=h[4],F_=h[5],G=h[6],H=h[7];
    for(let i=0;i<64;i++){
      const t1=(H+(((E>>>6)|(E<<26))^((E>>>11)|(E<<21))^((E>>>25)|(E<<7)))+((E&F_)^(~E&G))+SK[i]+w[i])|0;
      const t2=((((A>>>2)|(A<<30))^((A>>>13)|(A<<19))^((A>>>22)|(A<<10)))+((A&B)^(A&C)^(B&C)))|0;
      H=G;G=F_;F_=E;E=(D+t1)|0;D=C;C=B;B=A;A=(t1+t2)|0;}
    h[0]+=A;h[1]+=B;h[2]+=C;h[3]+=D;h[4]+=E;h[5]+=F_;h[6]+=G;h[7]+=H;}
  const out=new Uint8Array(32),ov=new DataView(out.buffer);for(let i=0;i<8;i++)ov.setUint32(i*4,h[i]);return out;}
function hmac(k,m){ if(k.length>64)k=sha256(k);
  const ip=new Uint8Array(64+m.length),op=new Uint8Array(96);
  for(let i=0;i<64;i++){const x=i<k.length?k[i]:0;ip[i]=x^0x36;op[i]=x^0x5c;}
  ip.set(m,64);op.set(sha256(ip),64);return sha256(op);}
function pbkdf(pw,salt){ const s=new Uint8Array(salt.length+4);s.set(salt);s[salt.length+3]=1;
  let u=hmac(pw,s);const t=u.slice();
  for(let i=1;i<10000;i++){u=hmac(pw,u);for(let j=0;j<32;j++)t[j]^=u[j];} return t;}
const TE=s=>new TextEncoder().encode(s), HX=b=>Array.from(b,x=>x.toString(16).padStart(2,'0')).join(''),
      UH=s=>new Uint8Array(s.match(/../g).map(x=>parseInt(x,16))), RND=n=>crypto.getRandomValues(new Uint8Array(n));
// nowe haslo: sol + skrot liczone tutaj, skrot zaszyfrowany kluczem sesji (haslo nie idzie siecia)
function nowyTok(haslo){
  const k=sessionStorage.getItem('pk'); if(!k) throw new Error('Zaloguj sie ponownie (brak klucza sesji)');
  const sol=HX(RND(16)), hs=pbkdf(TE(haslo),UH(sol)), m=hmac(UH(k),TE('zmiana:'+sol)), x=new Uint8Array(32);
  for(let i=0;i<32;i++)x[i]=hs[i]^m[i];
  try{ localStorage.setItem('solN_'+(sessionStorage.getItem('pid')||''),sol); }catch(e){}
  return {tok_sol:sol,tok_hash_x:HX(x),tok_len:String(haslo.length)};}
async function zaloguj(){
  const t=document.getElementById('pw').value, e=document.getElementById('lerr');
  e.textContent='Logowanie...';
  const r0=await fetch('/hello',{credentials:'same-origin'}); const h=await r0.text();
  if(r0.status!=200){ e.textContent=h; return; }
  const sol=(h.match(/sol=([0-9a-f]+)/)||[])[1], np=(h.match(/np=([0-9a-f]+)/)||[])[1];
  const pid=(h.match(/id=([0-9a-f]+)/)||[])[1]||''; sessionStorage.setItem('pid',pid);
  let zn=null; try{ zn=localStorage.getItem('sol_'+pid); }catch(x){}
  let zm=null; try{ zm=localStorage.getItem('solN_'+pid); }catch(x){}
  if(zn&&zn!=sol&&zm!=sol&&!confirm('UWAGA: Pico '+pid+' ma inna sol hasla niz ostatnio.\nJesli nie zmieniales hasla, to moze byc podstawione urzadzenie - nie loguj sie.\nKontynuowac?')){ e.textContent='Przerwano'; return; }
  const nc=HX(RND(16)), K=hmac(pbkdf(TE(t),UH(sol)),TE('K:'+np+':'+nc)), P=HX(hmac(K,TE('login')));
  const r=await fetch('/auth2',{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:new URLSearchParams({nc:nc,p:P,k:'strona'})});
  const odp=await r.text();
  if(r.status!=200){ e.textContent=odp||'Zle haslo'; return; }
  if((odp.match(/pico=([0-9a-f]+)/)||[])[1]!=HX(hmac(K,TE('pico')))){ e.textContent='Odpowiedz nie pochodzi od tego Pico!'; return; }
  sessionStorage.setItem('pk',HX(K)); sessionStorage.setItem('sol',sol); pokazSol();
  try{ localStorage.setItem('sol_'+pid,sol); }catch(x){}
  document.getElementById('pw').value='';
  document.getElementById('login').style.display='none'; e.textContent='';
  if(odp.indexOf('stan=ZMIEN')>=0) document.getElementById('zmien').style.display='block';
  linki(); tick();
}
async function zmienHasla(){
  const g=id=>document.getElementById(id).value, e=document.getElementById('zerr');
  if(g('zT1').length<12){ e.textContent='Haslo strony za krotkie ('+g('zT1').length+'/12 znakow)'; return; }
  if(g('zA1').length<12){ e.textContent='Haslo AP (Wi-Fi Pico) za krotkie ('+g('zA1').length+'/12 znakow)'; return; }
  if(g('zT1')!=g('zT2')){ e.textContent='Hasla strony sie roznia'; return; }
  if(g('zA1')!=g('zA2')){ e.textContent='Hasla AP sie roznia'; return; }
  let nt; try{ nt=nowyTok(g('zT1')); }catch(x){ e.textContent=x.message; return; }
  const r=await F('/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:new URLSearchParams(Object.assign({ap_pass:g('zA1')},nt))});
  const t=await r.text();
  if(r.status!=200){ e.textContent=t; return; }
  alert(t+'\n\nZaloguj sie nowym haslem strony. Skrypt sam zapyta o nowe haslo. Do sieci AP Pico laczysz sie odtad nowym haslem Wi-Fi.');
  location.reload();
}
function pokazSol(){ const s=sessionStorage.getItem('sol'), el=document.getElementById('solTxt'); if(el) el.textContent=s?('Pico '+(sessionStorage.getItem('pid')||'?')+', sol hasla: '+s):''; }
window.addEventListener('load',pokazSol);
async function logout(){ try{ await F('/logout',{method:'POST'}); }catch(e){} sessionStorage.removeItem('pk'); sessionStorage.removeItem('sol'); alert('Wylogowano.'); location.reload(); }
function linki(){ [['dl1','/download','pico_log.txt'],['dl2','/download_pico','pico_zdarzenia.txt'],
    ['dl3','/download_flash','pico_log_flash.txt'],['dl5','/download_boot','pico_starty_wezla.txt'],['dl4','/download_script','wyslij_firmware.py']].forEach(([id,u,n])=>{
    const e=document.getElementById(id); e.href='#'; e.onclick=ev=>{ ev.preventDefault(); pobierz(u,n); }; }); rstLabel(); }
async function portWezla(){
  const t=prompt('Port wezla TCP 4403 - aplikacja Meshtastic / meshtastic --host / VCOM, tylko siec lokalna.\nNa czas sesji Pico wstrzymuje wlasne API i straznika wezla.\nCzas: np. 30m, 2h, 1d (max 7d); 0 = wylacz','2h');
  if(t===null) return;
  try{
    const r=await F('/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({port_wezla:t})});
    if(r.status==401){ pokazLogin(); return; }
    alert(await r.text());
  }catch(e){ alert('Brak odpowiedzi Pico'); }
}
async function act(pytanie, url){
  if(!confirm(pytanie)) return;
  try{
    const r=await F(url,{method:'POST',headers:{'X-Time':czas()}});
    if(r.status==401){ pokazLogin(); return; }
    alert(await r.text()); progHidden=false;
  }catch(e){ alert('Brak odpowiedzi Pico'); }
}
async function togLog(){
  const on = document.getElementById('logBtn').textContent.indexOf('WYL')>=0 ? '1' : '0';
  const r = await F('/logcollect?on='+on,{method:'POST'}); alert(await r.text());
}
let pkgTryb='';
function flashBtn(){
  if (pkgTryb.indexOf('aktualizacja') >= 0)
    act('WGRAC AKTUALIZACJE FIRMWARE (sam program)?\nUstawienia, klucze i numer wezla zostaja. Trwa ok. 1 minuty.','/fw_flash');
  else if (pkgTryb.indexOf('pelna') >= 0)
    act('PELNA INSTALACJA FIRMWARE?\nCaly flash wezla zostanie SKASOWANY, potem firmware i konfiguracja z pakietu. Trwa 3-5 minut.','/fw_flash');
  else
    act('Wgrac pakiet z Pico (tryb: ' + (pkgTryb || 'nieznany') + ')?','/fw_flash');
}
let setOn=false, rstFiz='GP22 (pin 29)', rstSek=5;
async function rstLabel(){ try{
  const t=await (await F('/settings')).text(), v={};
  t.split('\n').forEach(l=>{const i=l.indexOf('='); if(i>0) v[l.substring(0,i)]=l.substring(i+1);});
  const gp=parseInt(v.rst_pin||'-1'), P=[1,2,4,5,6,7,9,10,11,12,14,15,16,17,19,20,21,22,24,25,26,27,29];
  rstFiz = 'GP'+gp+' (pin '+(gp>=0&&gp<=22 ? P[gp] : ({26:31,27:32,28:34})[gp]||'?')+')'; rstSek=v.rst_sek||'5';
  const b=document.getElementById('bRst'); b.textContent='Reset wezla '+rstFiz; b.style.display=gp<0?'none':'';
}catch(e){} }
async function togSet(){
  setOn=!setOn; document.getElementById('setp').style.display=setOn?'block':'none';
  document.getElementById('setBtn').textContent='Ustawienia: '+(setOn?'UKRYJ':'POKAZ');
  if(!setOn) return;
  const t=await (await F('/settings')).text(), v={};
  t.split('\n').forEach(l=>{const i=l.indexOf('='); if(i>0) v[l.substring(0,i)]=l.substring(i+1);});
  document.getElementById('sApS').value=v.ap_ssid||''; document.getElementById('sStS').value=v.sta_ssid||'';
  document.getElementById(v.sta=='1'?'mSTA':'mAP').checked=true;
  document.getElementById('sCh').value=v.ch||'1'; document.getElementById('sSnd').value=v.senders||''; document.getElementById('sUrl').value=v.url||'';
  document.getElementById('sOdp').value=v.brak_odp||'0'; document.getElementById('sHop').value=v.hop||'3'; document.getElementById('sStr').value=v.strona||'1'; document.getElementById('sRp').value=v.rst_pin||'-1';
  document.getElementById('sRs').value=v.rst_sek||'5'; document.getElementById('sRh').value=v.rst_hi||'1';
  document.getElementById('sPmT').value=v.pm_time||'0'; document.getElementById('sPmC').value=v.pm_czuw||'30';
  document.getElementById('sIpA').value=v.ip_allow||'';
  document.getElementById('sNet').value=v.net_mode||'wifi'; document.getElementById('sEm').value=v.eth_mod||'0'; document.getElementById('sLa').value=v.log_api_auto||'1'; document.getElementById('sZl').value=v.zas_lora||'1';
  document.getElementById('sEip').value=v.eth_ip||''; document.getElementById('sEmk').value=v.eth_mask||''; document.getElementById('sEgw').value=v.eth_gw||''; document.getElementById('sEdn').value=v.eth_dns||'';
  document.getElementById('sSip').value=v.sta_ip||''; document.getElementById('sSmk').value=v.sta_mask||'';
  document.getElementById('sSgw').value=v.sta_gw||''; document.getElementById('sSdn').value=v.sta_dns||'';
  document.getElementById('sVk').value=v.v_kor||'0.00'; document.getElementById('sVp').value=v.v_prog||'0.00';
  document.getElementById('pmSt').textContent=v.pm_on=='1'?'tryb POWMAN: WLACZONY':'tryb POWMAN: wylaczony';
  rstLabel();
  ['cmd_on','cmd_sleep','cmd_ap_on','cmd_ap_off','cmd_temp'].forEach((k,i)=>document.getElementById('c'+i).value=v[k]||'');
  ['sApP','sApP2','sStP','sTok','sTok2'].forEach(id=>document.getElementById(id).value='');
}
async function zapiszUst(){
  const g=id=>document.getElementById(id).value;
  const sta=document.getElementById('mSTA').checked?'1':'0';
  if(g('sNet')=='eth' && !confirm('Tryb sieci "eth" wylaczy Wi-Fi - strona bedzie dostepna tylko przez Ethernet (bez modulu Ethernet Pico wlaczy Wi-Fi awaryjnie). Kontynuowac?')) return;
  if(g('sNet')=='brak' && !confirm('Tryb sieci "brak" wylaczy Wi-Fi - strona przestanie dzialac. Powrot: komenda LoRa WLACZ_PICO (do restartu) i zmiana trybu. Kontynuowac?')) return;
  if(g('sTok')!=g('sTok2')){ alert('Haslo strony (token): oba pola musza byc takie same'); return; }
  if(g('sApP')!=g('sApP2')){ alert('Haslo AP: oba pola musza byc takie same'); return; }
  if(!confirm('Zapisac ustawienia i zrestartowac Pico?'+(sta=='1'?'\nPico przejdzie w tryb STA - strona bedzie pod innym adresem.':''))) return;
  const b=new URLSearchParams({ap_ssid:g('sApS'),ap_pass:g('sApP'),sta:sta,sta_ssid:g('sStS'),sta_pass:g('sStP'),ch:g('sCh'),senders:g('sSnd'),url:g('sUrl'),brak_odp:g('sOdp'),rst_pin:g('sRp'),rst_sek:g('sRs'),rst_hi:g('sRh'),pm_time:g('sPmT'),pm_czuw:g('sPmC'),v_kor:g('sVk'),v_prog:g('sVp'),ip_allow:g('sIpA'),net_mode:g('sNet'),eth_mod:g('sEm'),log_api_auto:g('sLa'),zas_lora:g('sZl'),hop:g('sHop'),strona:g('sStr'),sta_ip:g('sSip'),sta_mask:g('sSmk'),sta_gw:g('sSgw'),sta_dns:g('sSdn'),eth_ip:g('sEip'),eth_mask:g('sEmk'),eth_gw:g('sEgw'),eth_dns:g('sEdn'),
    cmd_on:g('c0'),cmd_sleep:g('c1'),cmd_ap_on:g('c2'),cmd_ap_off:g('c3'),cmd_temp:g('c4')});
  if(g('sTok')){ if(g('sTok').length<12){ alert('Haslo strony: min. 12 znakow'); return; }
    try{ const nt=nowyTok(g('sTok')); for(const k in nt) b.set(k,nt[k]); }catch(x){ alert(x.message); return; } }
  const r=await F('/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});
  const txt=await r.text(); alert(txt);
  if(r.status==200 && g('sTok')) location.reload();     // nowe haslo - sesje skasowane, logowanie od nowa
}
async function upd(id, url){
  const e = document.getElementById(id);
  const atBottom = e.scrollTop + e.clientHeight >= e.scrollHeight - 20;
  e.textContent = await (await F(url)).text();
  if (atBottom) e.scrollTop = e.scrollHeight;
}
let logPos = -1;
async function updLog(){                              // tylko nowe linie (przyrostowo) - szybciej i mniej obciaza Pico
  const e = document.getElementById('log');
  const atBottom = e.scrollTop + e.clientHeight >= e.scrollHeight - 20;
  const t = await (await F('/log' + (logPos >= 0 ? '?since=' + logPos : ''))).text();
  const nl = t.indexOf('\n'); if (nl < 1) return;
  const head = t.substring(0, nl), body = t.substring(nl + 1);
  logPos = parseInt(head.substring(1));
  if (head[0] == 'F' || e.textContent.startsWith('(podglad')) e.textContent = body;
  else if (body.length) { e.textContent += body; if (e.textContent.length > 65536) e.textContent = e.textContent.slice(-49152); }
  if (atBottom) e.scrollTop = e.scrollHeight;
}
async function tick(){
  try{
    const rs = await F('/status');
    if (rs.status == 401) { pokazLogin(); return; }
    if (rs.status == 503) {                             // Pico odbiera plik - nie przeszkadzamy, sprawdzamy co 30 s
      const m = (await rs.text()).match(/odbior (\d+)\/(\d+)/);
      const left = m ? (parseInt(m[2]) - parseInt(m[1])) : 1000000;
      const next = Math.min(Math.max(left / 15000 * 1000 + 10000, 15000), 300000);   // ok. 15 KB/s + zapas
      document.getElementById('busy').style.display='block';
      document.getElementById('eta').textContent='kolejne sprawdzenie za ok. ' + Math.round(next/1000) + ' s';
      setTimeout(tick, next); return;
    }
    const st = await rs.text();
    const pm = st.match(/pakiet: KOMPLETNY \(([^)]*)\)/); pkgTryb = pm ? pm[1] : '';
    const fb = document.getElementById('flBtn');
    fb.textContent = pkgTryb.indexOf('aktualizacja') >= 0 ? 'Aktualizuj firmware (sam program)'
                   : pkgTryb.indexOf('pelna') >= 0 ? 'Pelna instalacja firmware' : 'Flashuj pakiet z Pico';
    fb.style.display = st.indexOf('pakiet: KOMPLETNY') >= 0 ? '' : 'none';
    const ip = st.match(/\| IP ([0-9.]+)/), tp = st.match(/temp Pico ([0-9.-]+)/);
    if (ip) document.title = 'Pico2W flasher, IP ' + ip[1] + (tp ? ', ' + tp[1] + '\u00B0C' : '');
    document.getElementById('logBtn').textContent = 'Zbieranie logow: ' + (st.indexOf('logi: zbierane')>=0 ? 'WL (kliknij by wylaczyc)' : 'WYL (kliknij by wlaczyc)');
    document.getElementById('st').textContent = st.split(' | ').map(x=>x.replace(/ /g,'\u00a0')).join(' | ');   // bez lamania wewnatrz pola
    document.getElementById('apOff').style.display = st.indexOf('TRYB AP DLA WEZLOW') >= 0 ? '' : 'none';
    const busy = st.indexOf('odbior pakietu') >= 0;     // w trakcie odbioru pakietu nie obciazaj Pico
    document.getElementById('busy').style.display = busy ? 'block' : 'none';
    if (on.p && !busy) await upd('plog','/picolog');
    if (on.l && !busy) await updLog();
    // opis pakietu / historia wgran: po zmianie stanu FLASH i co ok. 30 s
    const fl = (st.split('FLASH: ')[1]||'').split(' ')[0] + (st.indexOf('KOMPLETNY')>=0);
    if (!busy) await prog();
    if (!busy && infoOn && (fl !== lastFl || ++infoN >= 15)) { lastFl = fl; infoN = 0; await loadInfo(); }
    const v = st.match(/wersja Pico ([^ |]+)/); if (v) document.getElementById('ver').textContent = 'v' + v[1];
    let next = 2000;
    if (busy) {                                         // odbior: jedno zapytanie po szacowanym czasie do konca
      const m = st.match(/odbior (\d+)\/(\d+)/);
      const left = m ? (parseInt(m[2]) - parseInt(m[1])) : 2000000;
      next = Math.min(Math.max(left / 15000 * 1000 + 10000, 15000), 180000);   // ok. 15 KB/s + zapas
      document.getElementById('eta').textContent = 'za ok. ' + Math.round(next / 1000) + ' s';
    }
    setTimeout(tick, next);
    return;
  }catch(e){}
  setTimeout(tick, 5000);
}
linki(); tick();                                         // bez sesji /status zwroci 401 -> okno logowania
</script></body></html>)HTML";

void handleRoot() {
  server.sendHeader("Connection", "close");
  if (upActive) { server.send(503, "text/html; charset=utf-8",
    "<meta charset='utf-8'><meta http-equiv='refresh' content='30'><body style='background:#111;color:#eee;font-family:sans-serif'>"
    "<h3>Trwa odbior pliku na Pico</h3>Strona odswiezy sie sama co 30 s. Nie klikaj nic - to spowalnia transmisje.</body>"); return; }
  server.sendHeader("Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; "
                    "connect-src 'self'; img-src 'self' data: blob:; frame-ancestors 'none'; form-action 'self'; base-uri 'none'");
  server.sendHeader("X-Frame-Options", "DENY");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("Referrer-Policy", "no-referrer");
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", PAGE);
}

const char* fwStateName() {
  switch (fwState) { case 1: return "TRWA"; case 2: return "OK"; case 3: return "BLAD"; default: return "BRAK"; }
}

// ---- Napiecie zasilania VSYS: ADC3 = GPIO29 (dzielnik 1:3). Pin wspoldzielony z ukladem Wi-Fi (CYW43),
// wiec pomiar pod blokada sterownika - tak jak w przykladzie pico-sdk (adc/read_vsys).
static float vsysStart = 0, vsysLast = 0;
static uint32_t vsysLastMs = 0, vsysWarnMs = 0;
float readVsys() {
  static bool adcOk = false;
  if (!adcOk) { adc_init(); adcOk = true; }
  cyw43_thread_enter();
  cyw43_arch_gpio_get(CYW43_WL_GPIO_VBUS_PIN);          // uklad Wi-Fi obudzony - linia wolna
  adc_gpio_init(29);
  adc_select_input(3);
  adc_fifo_setup(true, false, 0, false, false);
  adc_run(true);
  for (int i = 0; i < 8; i++) (void)adc_fifo_get_blocking();     // pierwsze probki bywaja zanizone
  uint32_t sum = 0;
  for (int i = 0; i < 32; i++) sum += adc_fifo_get_blocking();
  adc_run(false); adc_fifo_drain(); adc_fifo_setup(false, false, 0, false, false);
  cyw43_thread_exit();
  return (sum / 32.0f) * 3.0f * 3.3f / 4096.0f + cfg.vKor;
}
String vsysTxt() {
  if (vsysLast <= 0) return "?";
  char b[48];
  snprintf(b, sizeof(b), "%.2f V%s", vsysLast, (cfg.vProg > 0 && vsysLast < cfg.vProg) ? " NISKIE" : "");
  return b;
}
// start = pomiar przy starcie (przed Wi-Fi); potem co 10 min. Ostrzezenie najwyzej raz na godzine.
void vsysService(bool start) {
  if (!start && millis() - vsysLastMs < 600000UL) return;
  vsysLastMs = millis();
  vsysLast = readVsys();
  if (start) { vsysStart = vsysLast; logf("[PICO] Zasilanie VSYS przy starcie: %.2f V (korekta %+.2f V)\n", vsysLast, cfg.vKor); }
  if (cfg.vProg > 0 && vsysLast < cfg.vProg && (!vsysWarnMs || millis() - vsysWarnMs > 3600000UL)) {
    vsysWarnMs = millis();
    logf("[PICO] *** NISKIE NAPIECIE zasilania: %.2f V (prog %.2f V) - wezel moze sie nie uruchamiac albo resetowac ***\n", vsysLast, cfg.vProg);
    char m[80];
    snprintf(m, sizeof(m), "PICO: niskie napiecie zasilania %.2f V (prog %.2f V)", vsysLast, cfg.vProg);
    queueLora(m, 120000);
  }
}

void handleStatus() {
  FSInfo fi; LittleFS.info(fi);
  bool pkg = LittleFS.exists(PKG_PATH);
  bool pkgOk = LittleFS.exists(PKG_OK_PATH);
  String s = String("ESP32-S3: ") + (heltecMounted ? "PODLACZONY" : "brak") +
             " | odebrano: " + String(totalBytes) +
             " | zlacze: " + (hcd_port_connect_status(0) ? "COS" : "puste") +
             " | pakiet: " + (pkg ? (pkgOk ? "KOMPLETNY" + (pkgMode.length() ? " (" + pkgMode + ")" : String("")) : "NIEKOMPLETNY") : "brak") +
             " | FS: " + String((unsigned long)(fi.usedBytes / 1024)) + "/" + String((unsigned long)(fi.totalBytes / 1024)) + " KB" +
             " | FLASH: " + fwStateName() + " " + String(fwPct) + "% " + fwMsg +
             (upActive ? " | odbior " + String((unsigned long)upNext) + "/" + String((unsigned long)upTotal) : String("")) +
             (staActive ? " | Wi-Fi STA " + cfg.staSsid + ", RSSI " + String(WiFi.RSSI()) + " dBm"
                        : " | Wi-Fi AP " + cfg.apSsid + ", klienci: " + String(WiFi.softAPgetStationNum())) +
             " | IP " + myIp() +
             (netEth() ? " | Ethernet " + ethTxt() : String("")) +
             (brUntil ? " | port wezla 4403: " + brTxt() : String("")) +
             " | temp Pico " + String(analogReadTemp(), 1) + " C" +
             " | zasilanie " + vsysTxt() +
             (downgradeOk() ? " | downgrade " + downgradeTxt() : String("")) +
             (domyslneHasla() ? " | UWAGA: DOMYSLNE LUB ZA KROTKIE HASLA - zmien token i haslo AP w Ustawieniach" : "") +
             " | logi: " + (cfg.logOn ? "zbierane" : "WYLACZONE") +
             (wifiState == 2 ? String(" | TRYB AP DLA WEZLOW (tymczasowo z LoRa, w ustawieniach ") + (cfg.sta ? "STA" : "AP") + ")" : String("")) +
             " | start Pico: " + bootReason +
             obciazenieTxt() +
             " | uptime: " + String(millis() / 1000) + " s | wersja Pico " + PICO_VERSION;
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", s);
}

// Kopia fragmentu logu: od pozycji 'since' (liczonej od startu) do konca. Kopiowanie pod mutexem (memcpy),
// skladanie odpowiedzi juz poza nim - rdzen 1 (USB) nie czeka.
String logSince(uint32_t since, uint32_t* posOut) {
  mutex_enter_blocking(&logMtx);
  uint32_t total = logTotal;
  uint32_t avail = logWrapped ? LOG_SIZE : logHead;
  uint32_t n = (since <= total && total - since <= avail) ? total - since : avail;
  char* tmp = (char*)malloc(n + 1);
  if (tmp) {
    size_t start = (logHead + LOG_SIZE - n) % LOG_SIZE;
    size_t first = (start + n <= LOG_SIZE) ? n : LOG_SIZE - start;
    memcpy(tmp, logBuf + start, first);
    memcpy(tmp + first, logBuf, n - first);
    tmp[n] = 0;
  }
  mutex_exit(&logMtx);
  if (posOut) *posOut = total;
  if (!tmp) return String("");
  String out(tmp); free(tmp);
  return out;
}
String logSnapshot() { return logSince(0xFFFFFFFF, nullptr); }
void handleLog() {
  server.sendHeader("Connection", "close");
  if (upActive) { server.send(200, "text/plain", ""); return; }
  uint32_t since = server.hasArg("since") ? strtoul(server.arg("since").c_str(), nullptr, 10) : 0xFFFFFFFF;
  uint32_t pos;
  String body = logSince(since, &pos);
  // pierwsza linia: nowa pozycja; F = pelny log (klient czysci okno), P = przyrost
  bool full = !(since <= pos && pos - since == body.length());
  server.send(200, "text/plain; charset=utf-8", String(full ? "F" : "P") + String(pos) + "\n" + body);
}
String picoLogSnapshot() {
  String out;
  out.reserve(PLOG_SIZE + 1);
  mutex_enter_blocking(&logMtx);
  if (plogWrapped) for (size_t i = plogHead; i < PLOG_SIZE; i++) out += plogBuf[i];
  for (size_t i = 0; i < plogHead; i++) out += plogBuf[i];
  mutex_exit(&logMtx);
  return out;
}
void handlePicoLog() {
  server.sendHeader("Connection", "close");
  if (upActive) { server.send(200, "text/plain", "(trwa odbior pakietu - podglad wstrzymany)"); return; }
  server.send(200, "text/plain; charset=utf-8", picoLogSnapshot());
}

void handleDownloadPico() {
  server.sendHeader("Content-Disposition", "attachment; filename=\"pico_zdarzenia.txt\"");
  server.send(200, "text/plain; charset=utf-8",
              "===== ZDARZENIA PICO (start: " + String(bootReason) + ") =====\n" + picoLogSnapshot());
}

void handleDownload() {
  server.sendHeader("Content-Disposition", "attachment; filename=\"pico_log.txt\"");
  String all = "===== ZDARZENIA PICO (start: " + String(bootReason) + ") =====\n" + picoLogSnapshot() +
               "\n===== LOG (ESP32-S3 + Pico) =====\n" + logSnapshot();
  server.send(200, "text/plain; charset=utf-8", all);
}

// --- Odbiór pakietu firmware KAWAŁKAMI (każdy z własnym CRC, zły kawałek = ponowna wysyłka) ---
static const size_t CHUNK_MAX = 65536;
static uint8_t  chunkBuf[CHUNK_MAX];
static size_t   chunkLen = 0;
static bool     chunkOverflow = false;
static uint32_t chunkExpCrc = 0, chunkOffset = 0;
static int      chunkResult = 0;          // 0 brak, 1 OK, 2 zły token, 3 zły offset, 4 złe CRC, 5 błąd zapisu, 6 za duży
static File     upFile;                   // plik pakietu otwarty przez cały odbiór
static uint32_t chunkT0 = 0, sumRxMs = 0, sumWrMs = 0, sumChunks = 0;   // diagnostyka prędkości

static uint32_t crc32Calc(uint32_t crc, const uint8_t* d, size_t n) {   // crc startuje od 0xFFFFFFFF
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (int j = 0; j < 8; j++) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
  }
  return crc;
}

// ---- Logowanie (od 0.29): przegladarka - haslo raz, potem sesja (ciasteczko sid); skrypt - naglowek X-Auth-Token.
// Blokada po 5 blednych haslach z jednego IP (60 s, potem 2x dluzej, max 15 min), lista dozwolonych IP w trybie STA,
// wymuszona zmiana domyslnych / krotkich hasel (strona i AP).
static Sesja sesje[6];
struct Powitanie { String ip, np; uint32_t t; };
static Powitanie hello[8];               // jednorazowe liczby logowania (30 s)
static String audKto = "?", audK = "";  // audK: klucz biezacej sesji (zmiana hasla)
static String opKto = "";                           // kto zlecil biezaca operacje na firmware (wynik w dzienniku)
static const uint32_t SESJA_MS = 30UL * 60000UL;   // wygasa po 30 min bezczynnosci
static Proba proby[32];
static bool ctEq(const String& a, const String& b) {   // porownanie w stalym czasie (bez wczesnego wyjscia)
  size_t la = a.length(), lb = b.length(), m = la > lb ? la : lb;
  uint8_t d = (la != lb);
  for (size_t i = 0; i < m; i++) d |= (uint8_t)((i < la ? a[i] : 0) ^ (i < lb ? b[i] : 0));
  return d == 0;
}
void sesjeKasuj() { for (auto& x : sesje) x.id = ""; }
bool domyslneHasla() { return cfg.tokSlabe || !cfg.tokHash.length() || cfg.apPass == DEF_AP_PASS || cfg.apPass.length() < 12; }
static String klientIp() { return server.client().remoteIP().toString(); }
static bool ipDozwolone(const String& ip) {
  if ((!staActive && !ethUp()) || !cfg.ipAllow.length()) return true;   // tylko AP: bez filtra (dostep awaryjny przy Pico)
  bool apOn = wifiState == 2 || (wifiState == 0 && !staActive);
  if (apOn && ip.startsWith("192.168.4.")) return true;    // klient wlasnego AP Pico
  int st = 0;
  while (st <= (int)cfg.ipAllow.length()) {
    int k = cfg.ipAllow.indexOf(',', st); if (k < 0) k = cfg.ipAllow.length();
    String e = cfg.ipAllow.substring(st, k); st = k + 1;
    if (!e.length()) continue;
    if (e.endsWith("*")) { if (ip.startsWith(e.substring(0, e.length() - 1))) return true; }
    else if (ip == e) return true;
  }
  return false;
}
// Odmowa dla adresu spoza listy - wpis w zdarzeniach najwyzej raz na 10 min dla danego IP (strona odpytuje co kilka s)
static void ipOdmowa(const String& ip) {
  static String ost[4]; static uint32_t ostMs[4]; static uint8_t k = 0;
  for (int i = 0; i < 4; i++) if (ost[i] == ip && millis() - ostMs[i] < 600000UL) return;
  ost[k] = ip; ostMs[k] = millis(); k = (k + 1) & 3;
  logf("[PICO] Odmowa dostepu dla %s - adres spoza listy IP_DOZWOLONE\n", ip.c_str());
}
static int probaDla(const String& ip, bool utworz) {     // indeks w proby[] albo -1 (int, nie wskaznik - prototypy Arduino)
  int wolna = -1, najst = -1;
  for (int i = 0; i < 32; i++) {
    if (proby[i].ip == ip) return i;
    if (!proby[i].ip.length() && wolna < 0) wolna = i;
    bool aktywna = proby[i].blokDo && (int32_t)(millis() - proby[i].blokDo) < 0;   // trwajaca blokada - nie nadpisywac
    if (!aktywna && (najst < 0 || proby[i].blokDo < proby[najst].blokDo)) najst = i;
  }
  if (!utworz) return -1;
  int i = wolna >= 0 ? wolna : najst;
  if (i < 0) return -1;                                    // same aktywne blokady - nowy adres traktowany jak zablokowany
  proby[i].ip = ip; proby[i].n = 0; proby[i].nr = 0; proby[i].blokDo = 0;
  return i;
}
static uint32_t blokadaSek(const String& ip) {
  int i = probaDla(ip, false);
  if (i < 0) {                                             // nieznany adres: zablokowany tylko gdy tablica pelna aktywnych blokad
    for (auto& x : proby) if (!x.ip.length() || !x.blokDo || (int32_t)(millis() - x.blokDo) >= 0) return 0;
    return 60;
  }
  if (!proby[i].blokDo || (int32_t)(millis() - proby[i].blokDo) >= 0) return 0;
  return (proby[i].blokDo - millis()) / 1000 + 1;
}
static String klientTyp() { return server.header("X-Klient") == "skrypt" || (server.uri() != "/auth" && server.header("X-Auth-Token").length()) ? "skrypt" : "strona"; }
static void zlaProba(const String& ip) {
  int pi = probaDla(ip, true);
  if (pi < 0) { logf("[AUDYT] %s %s LOGIN BLAD - tablica blokad pelna, adres zablokowany\n", ip.c_str(), klientTyp().c_str()); return; }
  Proba& p = proby[pi];
  logf("[AUDYT] %s %s LOGIN BLAD - zle haslo (%u/5)\n", ip.c_str(), klientTyp().c_str(), (unsigned)(p.n + 1));
  if (++p.n < 5) return;
  p.n = 0; if (p.nr < 4) p.nr++;
  uint32_t sek = 60UL << (p.nr - 1); if (sek > 900) sek = 900;
  p.blokDo = millis() + sek * 1000UL;
  logf("[PICO] *** Blokada logowania z %s na %lu s (5 blednych hasel) ***\n", ip.c_str(), (unsigned long)sek);
  queueLora("PICO: blokada logowania z " + ip + " na " + String(sek) + " s - bledne hasla", 30000);
}
static void dobraProba(const String& ip) { int i = probaDla(ip, false); if (i >= 0) { proby[i].n = 0; proby[i].nr = 0; proby[i].blokDo = 0; } }
static String sidZCiastka() {
  String c = server.header("Cookie"); int p = c.indexOf("sid=");
  return p < 0 ? String("") : c.substring(p + 4, p + 36);
}
// Sesja z ciasteczka albo naglowek X-Auth-Token (skrypt). Bledny naglowek liczy sie do blokady.
// Podpis zadania (od 0.40): X-Licznik + X-Podpis = HMAC(K, metoda\nsciezka\nlicznik\n + argumenty posortowane
// "nazwa=wartosc\n" + wybrane naglowki "Nazwa:wartosc\n"). Licznik: okno 64 - bez powtorek, dopuszcza rownolegle zadania.
static bool licOk(int si, uint64_t n) {             // indeks sesji (nie referencja - prototypy Arduino)
  Sesja& x = sesje[si];
  if (n > x.lic) { uint64_t d = n - x.lic; x.okno = d >= 64 ? 0 : (x.okno << d); x.okno |= 1; x.lic = n; return true; }
  uint64_t d = x.lic - n;
  if (d >= 64 || (x.okno & (1ULL << d))) return false;
  x.okno |= 1ULL << d; return true;
}
static const char* const PODP_NAGL[] = { "X-Offset", "X-Chunk-CRC", "X-Chunk-SHA", "X-Total-Size", "X-Total-CRC", "X-Target", "X-Time" };
static int podpisPowod = 0;                          // 1 = zly podpis, 2 = licznik powtorzony/za stary (do diagnostyki)
static bool podpisOk(int si) {
  Sesja& x = sesje[si];
  podpisPowod = 1;
  String ns = server.header("X-Licznik"), ps = server.header("X-Podpis");
  uint8_t K[32], P[32];
  if (!ns.length() || ps.length() != 64 || !hexBytes(x.k, K, 32)) return false;
  for (char c : ns) if (!isdigit(c)) return false;
  String m = String(server.method() == HTTP_POST ? "POST" : "GET") + "\n" + server.uri() + "\n" + ns + "\n";
  int idx[48], k = 0;
  for (int i = 0; i < server.args() && k < 48; i++) if (server.argName(i) != "plain") idx[k++] = i;
  for (int i = 1; i < k; i++) { int v = idx[i], j = i - 1; while (j >= 0 && server.argName(idx[j]) > server.argName(v)) { idx[j + 1] = idx[j]; j--; } idx[j + 1] = v; }
  for (int i = 0; i < k; i++) m += server.argName(idx[i]) + "=" + server.arg(idx[i]) + "\n";
  for (const char* h : PODP_NAGL) { String v = server.header(h); if (v.length()) m += String(h) + ":" + v + "\n"; }
  hmacSha256((const char*)K, 32, m.c_str(), m.length(), P);
  if (!ctEq(hexStr(P, 32), ps)) return false;
  // to samo zadanie sprawdzane drugi raz (np. tokenOk + authOk w jednej obsludze) - tylko w tym samym polaczeniu
  static String ostPs; static uint32_t ostMs = 0; static uint16_t ostPort = 0;
  uint16_t port = server.client().remotePort();
  if (ps == ostPs && port == ostPort && millis() - ostMs < 2000) return true;
  if (!licOk(si, strtoull(ns.c_str(), nullptr, 10))) {
    podpisPowod = 2;
    logf("[AUDYT] %s licznik %s odrzucony (ostatni %llu) - strona ponowi zadanie\n", x.kto.c_str(), ns.c_str(), (unsigned long long)x.lic);
    return false;
  }
  podpisPowod = 0;
  ostPs = ps; ostMs = millis(); ostPort = port;
  return true;
}
static bool tokenOk() {
  String ip = klientIp();
  if (!ipDozwolone(ip) || blokadaSek(ip)) return false;
  String sid = sidZCiastka(), t = server.header("X-Auth-Token");
  bool tSid = t.length() == 32; for (char c : t) if (!isxdigit(c)) tSid = false;   // skrypt od 0.39: sesja w naglowku
  if (tSid) sid = t;
  if (sid.length() == 32)
    for (int si = 0; si < 6; si++) { Sesja& x = sesje[si];
      if (x.id.length() && millis() - x.last < SESJA_MS && x.ip == ip && ctEq(sid, x.id)) {
        if (!podpisOk(si)) {                              // sesja bez waznego podpisu: podsluchany identyfikator nic nie da
          static uint32_t lastLog = 0;
          if (podpisPowod == 1 && millis() - lastLog > 10000) { lastLog = millis(); logf("[AUDYT] %s %s ODRZUCONE - brak lub zly podpis zadania %s\n", ip.c_str(), x.kto.c_str(), server.uri().c_str()); }
          return false;
        }
        x.last = millis(); audKto = x.kto; audK = x.k; return true; } }
  if (tSid || !t.length()) return false;             // wygasla sesja - bez liczenia do blokady
  zlaProba(ip);
  return false;
}
// Dziennik operacji (forensyka): "[AUDYT] IP kto OPERACJA" - bez odpytywania stanu (status, logi na zywo)
static void audytOp(const String& ip) {
  static const char* const OP[][2] = {
    {"/download", "POBRANIE LOGU WEZLA"}, {"/download_pico", "POBRANIE ZDARZEN PICO"}, {"/download_flash", "POBRANIE LOGU FLASH"}, {"/download_boot", "POBRANIE STARTOW WEZLA"},
    {"/download_script", "POBRANIE SKRYPTU"}, {"/reinit", "RESTART HOSTA USB"}, {"/hreset", "RESET WEZLA"},
    {"/hboot", "BOOTLOADER WEZLA"}, {"/chiptest", "TEST BOOTLOADERA"}, {"/pm_off", "POWMAN WYLACZ"}, {"/rst_test", "TEST PINU RESETU"},
    {"/logcollect", "ZBIERANIE LOGOW"}, {"/settings_reset", "USTAWIENIA DOMYSLNE"}, {"/downgrade", "DOWNGRADE"},
    {"/fw_begin", "WYSYLANIE PAKIETU START"}, {"/fw_end", "WYSYLANIE PAKIETU KONIEC"}, {"/fw_flash", "FLASH START"},
    {"/cfg_send", "KONFIGURACJA WEZLA START"}, {"/ap_off", "KONIEC TRYBU AP"}, {"/pico_reboot", "RESTART PICO"} };
  String u = server.uri(), op;
  if (u == "/settings" && server.method() == HTTP_POST) {
    op = "ZMIANA USTAWIEN";
    if (server.args() <= 6) { op += ":"; for (int i = 0; i < server.args(); i++) if (server.argName(i) != "plain") op += " " + server.argName(i); }
  } else for (auto& o : OP) if (u == o[0]) { op = o[1]; break; }
  if (!op.length()) return;
  if (u == "/fw_flash" || u == "/cfg_send" || u == "/fw_end") opKto = ip + " " + audKto;
  logf("[AUDYT] %s %s %s\n", ip.c_str(), audKto.c_str(), op.c_str());
}
static bool uriBezZmianyHasel() {      // dostepne mimo domyslnych hasel - zeby dalo sie je zmienic
  String u = server.uri();
  return u == "/settings" || u == "/status" || u == "/logout";
}
// Pelna kontrola z odpowiedzia: 403 (IP), 429 (blokada), 401 (haslo), 403 (domyslne hasla)
static bool authOk() {
  String ip = klientIp();
  server.sendHeader("Connection", "close");
  if (!ipDozwolone(ip)) { ipOdmowa(ip); server.send(403, "text/plain", "Adres IP " + ip + " niedozwolony (lista w Ustawieniach)"); return false; }
  uint32_t b = blokadaSek(ip);
  if (b) { server.send(429, "text/plain", "Za duzo blednych hasel - sprobuj za " + String(b) + " s"); return false; }
  if (!tokenOk()) { server.send(401, "text/plain", podpisPowod == 2 ? "Wymagane haslo (licznik)" : "Wymagane haslo"); podpisPowod = 0; return false; }
  if (domyslneHasla() && !uriBezZmianyHasel()) {
    server.send(403, "text/plain", "Najpierw ustaw nowe haslo strony (token) i haslo AP: min. 12 znakow, inne niz domyslne (Ustawienia na stronie)");
    return false;
  }
  audytOp(ip);
  return true;
}
static bool needAuth() {
  if (upActive) {                                   // odbior pliku: nic innego nie obslugujemy (szybka odmowa)
    server.sendHeader("Connection", "close");
    server.send(503, "text/plain", "Trwa odbior pliku - odbior " + String((unsigned long)upNext) + "/" + String((unsigned long)upTotal));
    return false;
  }
  return authOk();
}
static uint32_t hdrHex(const char* name) {
  String c = server.header(name);
  if (c.startsWith("0x") || c.startsWith("0X")) c = c.substring(2);
  return strtoul(c.c_str(), NULL, 16);
}

// POST /fw_begin  (X-Total-Size, X-Total-CRC)
void handleFwBegin() {
  rp2040.wdt_reset();
  if (!authOk()) return;
  if (fwState == 1)  { server.send(409, "text/plain", "Trwa flashowanie"); return; }
  upTotal    = strtoul(server.header("X-Total-Size").c_str(), NULL, 10);
  upTotalCrc = hdrHex("X-Total-CRC");
  bool picoUpd = server.header("X-Target") == "pico";
  // Przed aktualizacja Pico: usun pakiet ESP i pliki pomocnicze (zostaja ustawienia i historia)
  LittleFS.remove(PKG_OK_PATH);
  LittleFS.remove(PKG_PATH);
  LittleFS.remove(PKG_INFO_PATH);
  LittleFS.remove(DL_INFO_PATH);                     // pobranie z internetu nieaktualne
  LittleFS.remove(CFGX_KEY_PATH); LittleFS.remove(ETAP_PATH);
  if (picoUpd) { LittleFS.remove(CFG_PENDING_PATH); LittleFS.remove(PICO_BIN); }
  upPath = picoUpd ? PICO_BIN : PKG_PATH;
  FSInfo fi; LittleFS.info(fi);
  uint32_t freeB = (uint32_t)(fi.totalBytes - fi.usedBytes);
  if (upTotal == 0 || upTotal + 32768 > freeB) {
    logf("[PICO] Pakiet %lu B nie zmiesci sie (wolne %lu B)\n", (unsigned long)upTotal, (unsigned long)freeB);
    server.send(507, "text/plain", "Za malo miejsca na Pico"); return;
  }
  if (upFile) upFile.close();
  upFile = LittleFS.open(upPath, "w");
  if (!upFile) { server.send(500, "text/plain", "Nie mozna utworzyc pliku"); return; }
  upNext = 0; upActive = true; upIp = klientIp(); upLastMs = millis();
  fwState = 0; fwPct = 0; strcpy(fwMsg, "odbior pakietu");
  logf("\n[PICO] Odbior %s: %lu B, CRC %08lX\n", picoUpd ? "AKTUALIZACJI PICO" : "pakietu",
       (unsigned long)upTotal, (unsigned long)upTotalCrc);
  server.send(200, "text/plain", "OK 0");
}

// POST /fw_chunk  (multipart, X-Offset, X-Chunk-CRC) - dane w RAM, zapis dopiero po zgodnym CRC
void handleFwChunkData() {
  HTTPUpload& up = server.upload();
  rp2040.wdt_reset();
  if (up.status == UPLOAD_FILE_START) {
    chunkLen = 0; chunkOverflow = false; chunkResult = 0;
    chunkT0 = millis();
    chunkOffset = strtoul(server.header("X-Offset").c_str(), NULL, 10);
    chunkExpCrc = hdrHex("X-Chunk-CRC");
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (chunkLen + up.currentSize > CHUNK_MAX) { chunkOverflow = true; return; }
    memcpy(chunkBuf + chunkLen, up.buf, up.currentSize);
    chunkLen += up.currentSize;
  }
}

static bool chunkShaOk() {
  uint8_t d[32]; Sha256 s; shaInit(s); shaUpd(s, chunkBuf, chunkLen); shaFin(s, d);
  return ctEq(hexStr(d, 32), server.header("X-Chunk-SHA"));
}
void handleFwChunkDone() {
  rp2040.wdt_reset();
  if (!tokenOk() || !upActive || klientIp() != upIp) chunkResult = 2;   // fragmenty tylko od tego, kto zaczal
  else if (chunkOverflow)            chunkResult = 6;
  else if (chunkOffset != upNext)    chunkResult = 3;
  else if ((~crc32Calc(0xFFFFFFFF, chunkBuf, chunkLen)) != chunkExpCrc) chunkResult = 4;
  else if (!chunkShaOk()) chunkResult = 4;                 // skrot SHA-256 z podpisanego naglowka (CRC da sie podrobic)
  else {
    uint32_t tw = millis();
    sumRxMs += tw - chunkT0;
    size_t w = upFile ? upFile.write(chunkBuf, chunkLen) : 0;
    sumWrMs += millis() - tw; sumChunks++;
    if (w != chunkLen) chunkResult = 5;
    else { upNext += chunkLen; chunkResult = 1; upLastMs = millis(); }
  }
  switch (chunkResult) {
    case 1: {
      int pct = upTotal ? (int)((uint64_t)upNext * 100 / upTotal) : 0;
      if (pct / 10 != (int)((uint64_t)(upNext - chunkLen) * 100 / (upTotal ? upTotal : 1)) / 10)
        { logf("[PICO] Odebrano %lu/%lu KB (%d%%) - sredni czas kawalka: odbior Wi-Fi %lu ms, zapis flash %lu ms\n",
               (unsigned long)(upNext / 1024), (unsigned long)(upTotal / 1024), pct,
               (unsigned long)(sumChunks ? sumRxMs / sumChunks : 0), (unsigned long)(sumChunks ? sumWrMs / sumChunks : 0));
          sumRxMs = sumWrMs = sumChunks = 0; }
      server.send(200, "text/plain", "OK " + String((unsigned long)upNext));
      break;
    }
    case 2: server.send(401, "text/plain", "Brak autoryzacji albo brak /fw_begin"); break;
    case 3: server.send(409, "text/plain", "NEXT " + String((unsigned long)upNext)); break;
    case 4: logf("[PICO] Zle CRC kawalka @%lu - ponowienie\n", (unsigned long)chunkOffset);
            server.send(400, "text/plain", "CRC kawalka niezgodne"); break;
    case 5: logText("[PICO] BLAD zapisu kawalka do LittleFS\n");
            server.send(500, "text/plain", "Blad zapisu"); break;
    default: server.send(413, "text/plain", "Kawalek za duzy"); break;
  }
}

// POST /fw_end - kontrola CRC całego pliku ODCZYTANEGO z pamięci Pico
// Wyciaga opis (wpis INFO) z pakietu do /pkg.info
void savePkgInfo() {
  File f = LittleFS.open(PKG_PATH, "r"); if (!f) return;
  PkgHdr h; PkgEnt e; bool found = false;
  if (f.read((uint8_t*)&h, sizeof(h)) == sizeof(h) && h.count <= 8)
    for (int i = 0; i < h.count && !found; i++)
      if (f.read((uint8_t*)&e, sizeof(e)) == sizeof(e) && e.offset == INFO_OFFSET && e.size < 2000) found = true;
  File o = LittleFS.open(PKG_INFO_PATH, "w");
  if (o) {
    if (found && f.seek(e.dataPos)) { for (uint32_t k = 0; k < e.size; k++) { int c = f.read(); if (c < 0) break; o.write((uint8_t)c); } }
    else o.print("(pakiet bez opisu - zbudowany starszym skryptem)");
    o.close();
  }
  f.close();
}

void loadPkgMode() {
  String l = LittleFS.exists(PKG_OK_PATH) ? infoLine(readFileStr(PKG_INFO_PATH), "tryb:") : String("");
  pkgMode = l.length() > 6 ? l.substring(6) : String("");
  pkgMode.trim();
}

void handleInfo() {
  String pkg = LittleFS.exists(PKG_OK_PATH) ? readFileStr(PKG_INFO_PATH) : String("brak kompletnego pakietu");
  String node = readFileStr(NODE_INFO_PATH);
  node.replace("FW|", "Firmware wgrany ");
  node.replace("CFG|", "Konfiguracja wgrana ");
  node.replace("OTA|", "OTA wgrane ");
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", "PAKIET NA PICO:\n" + pkg + "\n\nOSTATNIO WGRANE DO WEZLA:\n" + (node.length() ? node : String("brak danych")));
}

// Czy plik to program tego projektu: w .bin musi byc znacznik "PICOVER=" (PICO_VER_TAG, od 0.23).
// Chroni przed wgraniem przez pomylke innego programu (Pico stracilby Wi-Fi i strone).
String picoBinVer(const char* path) {              // wersja z pliku programu Pico (znacznik PICOVER=)
  File f = LittleFS.open(path, "r"); if (!f) return "";
  static const char TAG[] = "PICOVER="; const size_t TL = sizeof(TAG) - 1;
  uint8_t b[512 + 32]; size_t keep = 0, n; String v;
  while (!v.length() && (n = f.read(b + keep, 512)) > 0) {
    size_t tot = keep + n;
    for (size_t i = 0; i + TL + 16 <= tot || (i + TL <= tot && n < 512); i++)
      if (memcmp(b + i, TAG, TL) == 0) { for (size_t j = i + TL; j < tot && b[j] && j < i + TL + 16; j++) v += (char)b[j]; break; }
    keep = tot < TL + 16 ? tot : TL + 16;
    memmove(b, b + tot - keep, keep);
    rp2040.wdt_reset();
  }
  f.close();
  return v;
}
bool picoBinOk(const char* path) {
  static const char TAG[] = "PICOVER=";
  const size_t TL = sizeof(TAG) - 1;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  uint8_t b[512 + 16]; size_t keep = 0, n; bool ok = false;
  if (f.read(b, 4) == 4 && memcmp(b, "UF2\n", 4) == 0) {        // plik .uf2 (format dla BOOTSEL), nie .bin - wgrany zepsulby Pico
    f.close(); logText("[PICO] Plik aktualizacji to .uf2 - potrzebny .bin\n"); return false;
  }
  f.seek(0);
  while (!ok && (n = f.read(b + keep, 512)) > 0) {
    size_t tot = keep + n;
    for (size_t i = 0; i + TL <= tot; i++) if (memcmp(b + i, TAG, TL) == 0) { ok = true; break; }
    keep = tot < TL - 1 ? tot : TL - 1;
    memmove(b, b + tot - keep, keep);
    rp2040.wdt_reset();
  }
  f.close();
  return ok;
}

void handleFwEnd() {
  rp2040.wdt_reset();
  if (!authOk()) return;
  if (!upActive) { server.send(409, "text/plain", "Brak /fw_begin"); return; }
  if (klientIp() != upIp) { server.send(409, "text/plain", "Odbior rozpoczety z innego adresu"); return; }
  if (upNext != upTotal)       { server.send(409, "text/plain", "NEXT " + String((unsigned long)upNext)); return; }
  if (upFile) upFile.close();
  File f = LittleFS.open(upPath, "r");
  uint32_t crc = 0xFFFFFFFF; size_t n, tot = 0;
  while (f && (n = f.read(chunkBuf, 4096)) > 0) { crc = crc32Calc(crc, chunkBuf, n); tot += n; rp2040.wdt_reset(); }
  if (f) f.close();
  upActive = false;
  if (tot != upTotal || (~crc) != upTotalCrc) {
    LittleFS.remove(upPath);
    logf("[PICO] Kontrola calego pakietu NIEZGODNA (odczytano %u B) - usuniety\n", (unsigned)tot);
    server.send(400, "text/plain", "CRC calego pakietu niezgodne - pakiet usuniety");
    return;
  }
  if (upPath == PICO_BIN && !picoBinOk(PICO_BIN)) {
    LittleFS.remove(PICO_BIN);
    logText("[PICO] Odrzucono plik aktualizacji Pico - to nie jest program Pico2W flasher (brak znacznika PICOVER)\n");
    server.send(400, "text/plain", "To nie jest program Pico2W flasher (brak znacznika wersji) - odrzucony, Pico zostaje przy obecnym programie");
    return;
  }
  if (upPath == PICO_BIN) {
    String err;
    if (!wersjaDozwolona(picoBinVer(PICO_BIN), PICO_VERSION, "Program Pico", err)) {
      LittleFS.remove(PICO_BIN);
      server.send(400, "text/plain", err);
      return;
    }
  }
  if (upPath == PICO_BIN) {                          // aktualizacja Pico: zlec wgranie i restart
    { File m = LittleFS.open(PICO_LORA_FLAG, "w"); if (m) { m.print(PICO_VERSION); m.close(); } }   // potwierdzenie LoRa po starcie
    picoOTA.begin();
    picoOTA.addFile(PICO_BIN);
    picoOTA.commit();
    logf("[PICO] Aktualizacja Pico %u B zweryfikowana - restart i wgranie\n", (unsigned)tot);
    picoRebootAt = millis() + 1500;
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", "OK aktualizacja Pico zweryfikowana - Pico wgrywa ja i restartuje sie (ok. 30 s)");
    return;
  }
  { File m = LittleFS.open(PKG_OK_PATH, "w"); if (m) { m.print(upTotalCrc, HEX); m.close(); } }
  savePkgInfo();
  loadPkgMode();
  fwState = 0; fwPct = 0; strcpy(fwMsg, "pakiet gotowy");
  logf("[PICO] Pakiet kompletny i zweryfikowany z pamieci: %u B, CRC OK\n", (unsigned)tot);
  server.send(200, "text/plain", "OK pakiet " + String((unsigned long)tot) + " B, CRC zgodne");
}

void handleFwFlash() {
  if (!authOk()) return;
  if (fwState == 1)                 { server.send(409, "text/plain", "Flashowanie juz trwa"); return; }
  if (!LittleFS.exists(PKG_PATH))   { server.send(404, "text/plain", "Brak pakietu na Pico"); return; }
  if (!LittleFS.exists(PKG_OK_PATH)){ server.send(409, "text/plain", "Pakiet niekompletny lub niezweryfikowany - wyslij ponownie"); return; }
  if (!heltecMounted)               { server.send(409, "text/plain", "ESP32-S3 nie jest podlaczony"); return; }
  strncpy(opTime, server.header("X-Time").c_str(), sizeof(opTime) - 1);
  fwState = 1; fwPct = 0; strcpy(fwMsg, "zlecone");
  reqCmd = 6;
  server.send(200, "text/plain", "Flashowanie uruchomione");
}

void handleCfgSend() {
  if (!authOk()) return;
  if (fwState == 1)                 { server.send(409, "text/plain", "Trwa inna operacja"); return; }
  if (!LittleFS.exists(PKG_OK_PATH)){ server.send(409, "text/plain", "Brak kompletnego pakietu na Pico"); return; }
  if (!heltecMounted)               { server.send(409, "text/plain", "ESP32-S3 nie jest podlaczony"); return; }
  strncpy(opTime, server.header("X-Time").c_str(), sizeof(opTime) - 1);
  fwState = 1; fwPct = 0; strcpy(fwMsg, "zlecone");
  reqCmd = 7;
  server.send(200, "text/plain", "Wysylanie konfiguracji uruchomione");
}

void handleFwStatus() {
  // Format dla skryptu: STAN|procent|komunikat
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", String(fwStateName()) + "|" + String(fwPct) + "|" + fwMsg);
}

// ---- Ustawienia (plik tekstowy klucz=wartosc) ----
void loadSettings() {
  cfg.apSsid = DEF_AP_SSID; cfg.apPass = DEF_AP_PASS; cfg.tokSol = cfg.tokHash = ""; cfg.tokSlabe = true;
  String tokJawny;                                     // do 0.39 haslo bylo zapisane jawnie - zamiana na skrot
  cfg.staSsid = ""; cfg.staPass = ""; cfg.sta = false; cfg.ch = 1; cfg.senders = ""; cfg.url = ""; cfg.logOn = true;
  cfg.brakOdp = 0; cfg.rstPin = 22; cfg.rstSek = 5; cfg.rstHigh = true;
  cfg.pmTime = "0"; cfg.pmCzuw = 30; cfg.vKor = 0; cfg.vProg = 0; cfg.ipAllow = ""; cfg.dgDo = 0; cfg.mtCzas = 0; cfg.netMode = "wifi"; cfg.ethMod = 0; cfg.logApiAuto = true; cfg.zasLora = true; cfg.strona = true; cfg.hop = 3; cfg.staIp = cfg.staMask = cfg.staGw = cfg.staDns = "";
  cfg.ethIp = cfg.ethMask = cfg.ethGw = cfg.ethDns = ""; cfg.fwOst = ""; cfg.picoId = "";
  for (int i = 0; i < 5; i++) cfg.cmd[i] = CMD_DEF[i];
  File f = LittleFS.open(SETTINGS_PATH, "r");
  if (!f) { logText("[PICO] Ustawienia: BRAK PLIKU - uzywam domyslnych\n"); tokUstaw(DEF_TOKEN); picoIdNowy(); return; }
  logText("[PICO] Ustawienia wczytane z pliku\n");
  while (f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    int e = l.indexOf('='); if (e < 1) continue;
    String k = l.substring(0, e), v = l.substring(e + 1);
    if (k == "ap_ssid" && v.length()) cfg.apSsid = v;
    else if (k == "ap_pass" && v.length() >= 8) cfg.apPass = v;
    else if (k == "token" && v.length() >= 4) tokJawny = v;
    else if (k == "tok_sol" && v.length() == 32) cfg.tokSol = v;
    else if (k == "tok_hash" && v.length() == 64) cfg.tokHash = v;
    else if (k == "tok_slabe") cfg.tokSlabe = v == "1";
    else if (k == "sta_ssid") cfg.staSsid = v;
    else if (k == "sta_pass") cfg.staPass = v;
    else if (k == "sta") cfg.sta = (v == "1");
    else if (k == "ch") cfg.ch = constrain(v.toInt(), 0, 7);
    else if (k == "senders") cfg.senders = v;
    else if (k == "url") cfg.url = v;
    else if (k == "log") cfg.logOn = (v != "0");
    else if (k == "brak_odp") cfg.brakOdp = constrain(v.toInt(), 0, 1440);
    else if (k == "rst_pin" && gpioOk(v.toInt())) cfg.rstPin = v.toInt();
    else if (k == "rst_sek") cfg.rstSek = constrain(v.toInt(), 1, 60);
    else if (k == "rst_hi") cfg.rstHigh = (v != "0");
    else if (k == "pm_time" && pmTimeOk(v)) cfg.pmTime = v;
    else if (k == "pm_czuw") cfg.pmCzuw = constrain(v.toInt(), 1, 240);
    else if (k == "v_kor") cfg.vKor = constrain(v.toFloat(), -1.0f, 1.0f);
    else if (k == "v_prog") cfg.vProg = constrain(v.toFloat(), 0.0f, 5.5f);
    else if (k == "ip_allow") cfg.ipAllow = v;
    else if (k == "dg_do") cfg.dgDo = strtoul(v.c_str(), nullptr, 10);
    else if (k == "mt_czas") cfg.mtCzas = strtoul(v.c_str(), nullptr, 10);
    else if (k == "eth_mod") cfg.ethMod = v.toInt() == 1 ? 1 : 0;
    else if (k == "log_api_auto") cfg.logApiAuto = (v != "0");
    else if (k == "zas_lora") cfg.zasLora = (v != "0");
    else if (k == "strona") cfg.strona = (v != "0");
    else if (k == "hop") cfg.hop = constrain(v.toInt(), 0, 7);
    else if (k == "net_mode") cfg.netMode = (v == "brak" || v == "eth" || v == "oba") ? v : String("wifi");
    else if (k == "sta_ip") cfg.staIp = v;
    else if (k == "sta_mask") cfg.staMask = v;
    else if (k == "sta_gw") cfg.staGw = v;
    else if (k == "sta_dns") cfg.staDns = v;
    else if (k == "eth_ip") cfg.ethIp = v;
    else if (k == "fw_ost") cfg.fwOst = v;
    else if (k == "eth_mask") cfg.ethMask = v;
    else if (k == "eth_gw") cfg.ethGw = v;
    else if (k == "eth_dns") cfg.ethDns = v;
    else for (int i = 0; i < 5; i++) if (k == CMD_KEYS[i] && v.length()) cfg.cmd[i] = v;
  }
  f.close();
  if (!cfg.tokHash.length() || !cfg.tokSol.length()) {  // brak skrotu: z jawnego hasla (migracja) albo domyslne
    tokUstaw(tokJawny.length() ? tokJawny : String(DEF_TOKEN));
    if (tokJawny.length()) { saveSettings(); logText("[PICO] Haslo strony zapisane jako skrot (PBKDF2) - jawne haslo usuniete z pamieci\n"); }
  }
  picoIdNowy();
  logf("[PICO] Identyfikator Pico: %s\n", cfg.picoId.c_str());
}
bool saveSettings() {
  File f = LittleFS.open(SETTINGS_PATH, "w"); if (!f) return false;
  f.printf("ap_ssid=%s\nap_pass=%s\ntok_sol=%s\ntok_hash=%s\ntok_slabe=%d\nsta=%d\nsta_ssid=%s\nsta_pass=%s\nch=%d\nsenders=%s\nlog=%d\n",
           cfg.apSsid.c_str(), cfg.apPass.c_str(), cfg.tokSol.c_str(), cfg.tokHash.c_str(), cfg.tokSlabe ? 1 : 0, cfg.sta ? 1 : 0, cfg.staSsid.c_str(),
           cfg.staPass.c_str(), cfg.ch, cfg.senders.c_str(), cfg.logOn ? 1 : 0);
  for (int i = 0; i < 5; i++) f.printf("%s=%s\n", CMD_KEYS[i], cfg.cmd[i].c_str());
  f.printf("url=%s\n", cfg.url.c_str());
  f.printf("brak_odp=%d\nrst_pin=%d\nrst_sek=%d\nrst_hi=%d\n", cfg.brakOdp, cfg.rstPin, cfg.rstSek, cfg.rstHigh ? 1 : 0);
  f.printf("pm_time=%s\npm_czuw=%d\n", cfg.pmTime.c_str(), cfg.pmCzuw);
  f.printf("v_kor=%.2f\nv_prog=%.2f\n", cfg.vKor, cfg.vProg);
  f.printf("ip_allow=%s\n", cfg.ipAllow.c_str());
  f.printf("dg_do=%lu\nmt_czas=%lu\n", (unsigned long)cfg.dgDo, (unsigned long)cfg.mtCzas);
  f.printf("eth_mod=%d\nlog_api_auto=%d\nzas_lora=%d\nstrona=%d\nhop=%d\n", cfg.ethMod, cfg.logApiAuto ? 1 : 0, cfg.zasLora ? 1 : 0, cfg.strona ? 1 : 0, cfg.hop);
  f.printf("net_mode=%s\nsta_ip=%s\nsta_mask=%s\nsta_gw=%s\nsta_dns=%s\n", cfg.netMode.c_str(), cfg.staIp.c_str(),
           cfg.staMask.c_str(), cfg.staGw.c_str(), cfg.staDns.c_str());
  f.printf("eth_ip=%s\neth_mask=%s\neth_gw=%s\neth_dns=%s\nfw_ost=%s\n", cfg.ethIp.c_str(), cfg.ethMask.c_str(), cfg.ethGw.c_str(), cfg.ethDns.c_str(), cfg.fwOst.c_str());
  f.close(); return true;
}
void handleSettingsGet() {      // hasel nie odsylamy
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", String("ap_ssid=") + cfg.apSsid + "\nsta=" + (cfg.sta ? "1" : "0") +
              "\nsta_ssid=" + cfg.staSsid + "\nch=" + String(cfg.ch) + "\nsenders=" + cfg.senders + "\nurl=" + cfg.url + "\n" +
              CMD_KEYS[0] + "=" + cfg.cmd[0] + "\n" + CMD_KEYS[1] + "=" + cfg.cmd[1] + "\n" + CMD_KEYS[2] + "=" + cfg.cmd[2] +
              "\n" + CMD_KEYS[3] + "=" + cfg.cmd[3] + "\n" + CMD_KEYS[4] + "=" + cfg.cmd[4] + "\n" +
              "brak_odp=" + cfg.brakOdp + "\nrst_pin=" + cfg.rstPin + "\nrst_sek=" + cfg.rstSek + "\nrst_hi=" + (cfg.rstHigh ? "1" : "0") + "\n" +
              "pm_time=" + cfg.pmTime + "\npm_czuw=" + cfg.pmCzuw + "\npm_on=" + (pmActive() ? "1" : "0") + "\n" +
              "v_kor=" + String(cfg.vKor, 2) + "\nv_prog=" + String(cfg.vProg, 2) + "\nip_allow=" + cfg.ipAllow + "\nnet_mode=" + cfg.netMode + "\neth_mod=" + String(cfg.ethMod) + "\nlog_api_auto=" + (cfg.logApiAuto ? "1" : "0") + "\nzas_lora=" + (cfg.zasLora ? "1" : "0") + "\nhop=" + String(cfg.hop) + "\nstrona=" + (cfg.strona ? "1" : "0") +
              "\nsta_ip=" + cfg.staIp + "\nsta_mask=" + cfg.staMask + "\nsta_gw=" + cfg.staGw + "\nsta_dns=" + cfg.staDns +
              "\neth_ip=" + cfg.ethIp + "\neth_mask=" + cfg.ethMask + "\neth_gw=" + cfg.ethGw + "\neth_dns=" + cfg.ethDns +
              "\nipw=" + ipTxt(cfg.staIp, cfg.staMask, cfg.staGw, cfg.staDns) + "\nipe=" + ipTxt(cfg.ethIp, cfg.ethMask, cfg.ethGw, cfg.ethDns) + "\nport_wezla=" + brTxt() + "\n");
}
bool urlOk(const String& u) {
  return (u.startsWith("http://") || u.startsWith("https://")) && u.length() > 10 && u.length() <= 190 && u.indexOf(' ') < 0;
}
void handleSettingsPost() {
  if (fwState == 1) { server.send(409, "text/plain", "Trwa flashowanie - zmiana ustawien zablokowana"); return; }
  Settings n = cfg;
  if (server.hasArg("ap_ssid") && server.arg("ap_ssid").length()) n.apSsid = server.arg("ap_ssid");
  if (server.hasArg("ap_pass") && server.arg("ap_pass").length()) n.apPass = server.arg("ap_pass");
  String tokErr;                                       // nowe haslo: skrot liczony u klienta, zaszyfrowany kluczem sesji
  if (server.hasArg("token") && server.arg("token").length()) tokErr = "Odswiez strone / zaktualizuj skrypt (haslo nie jest juz wysylane jawnie)";
  if (server.hasArg("tok_hash_x") && server.arg("tok_hash_x").length()) {
    String sol = server.arg("tok_sol"), x = server.arg("tok_hash_x"); int dl = server.arg("tok_len").toInt();
    uint8_t K[32], xb[32], m[32], hb[32], hd[32], sb[16];
    if (!audK.length() || !hexBytes(audK, K, 32)) tokErr = "Zmiana hasla wymaga zalogowania (sesja)";
    else if (!hexBytes(sol, sb, 16) || !hexBytes(x, xb, 32)) tokErr = "Niepoprawne dane nowego hasla";
    else {
      String z = "zmiana:" + sol; hmacSha256((const char*)K, 32, z.c_str(), z.length(), m);
      for (int i = 0; i < 32; i++) hb[i] = xb[i] ^ m[i];
      pbkdf2(DEF_TOKEN, strlen(DEF_TOKEN), sb, 16, hd);
      if (dl < 12 || !memcmp(hb, hd, 32)) tokErr = "Haslo strony: min. 12 znakow i inne niz domyslne";
      else { n.tokSol = sol; n.tokHash = hexStr(hb, 32); n.tokSlabe = false; }
    }
  }
  if (server.hasArg("sta"))      n.sta = server.arg("sta") == "1";
  if (server.hasArg("sta_ssid")) n.staSsid = server.arg("sta_ssid");
  if (server.hasArg("sta_pass") && server.arg("sta_pass").length()) n.staPass = server.arg("sta_pass");
  if (server.hasArg("ch"))      n.ch = constrain(server.arg("ch").toInt(), 0, 7);
  if (server.hasArg("senders")) { n.senders = server.arg("senders"); n.senders.replace(" ", ""); n.senders.toLowerCase(); }
  if (server.hasArg("url"))     { n.url = server.arg("url"); n.url.trim(); }
  if (server.hasArg("brak_odp")) n.brakOdp = server.arg("brak_odp").toInt();
  if (server.hasArg("rst_pin"))  n.rstPin = server.arg("rst_pin").length() ? server.arg("rst_pin").toInt() : -1;
  if (server.hasArg("rst_sek"))  n.rstSek = server.arg("rst_sek").toInt();
  if (server.hasArg("rst_hi"))   n.rstHigh = server.arg("rst_hi") != "0";
  if (server.hasArg("pm_time"))  { n.pmTime = server.arg("pm_time"); n.pmTime.trim(); n.pmTime.replace(" ", ""); if (!n.pmTime.length()) n.pmTime = "0"; }
  if (server.hasArg("pm_czuw"))  n.pmCzuw = server.arg("pm_czuw").toInt();
  if (server.hasArg("v_kor"))    { String t = server.arg("v_kor"); t.replace(",", "."); n.vKor = t.toFloat(); }
  if (server.hasArg("net_mode")) n.netMode = server.arg("net_mode");
  if (server.hasArg("eth_mod"))  n.ethMod = server.arg("eth_mod") == "1" ? 1 : 0;
  if (server.hasArg("log_api_auto")) n.logApiAuto = server.arg("log_api_auto") != "0";
  if (server.hasArg("zas_lora")) n.zasLora = server.arg("zas_lora") != "0";
  if (server.hasArg("hop")) n.hop = constrain(server.arg("hop").toInt(), 0, 7);
  if (server.hasArg("strona")) n.strona = server.arg("strona") != "0";   // strona (pole Strona WWW) i skrypt (--ustaw STRONA=WYL)
  if (server.hasArg("sta_ip"))   { n.staIp = server.arg("sta_ip"); n.staIp.trim(); }
  if (server.hasArg("sta_mask")) { n.staMask = server.arg("sta_mask"); n.staMask.trim(); }
  if (server.hasArg("sta_gw"))   { n.staGw = server.arg("sta_gw"); n.staGw.trim(); }
  if (server.hasArg("sta_dns"))  { n.staDns = server.arg("sta_dns"); n.staDns.trim(); }
  if (server.hasArg("eth_ip"))   { n.ethIp = server.arg("eth_ip"); n.ethIp.trim(); }
  if (server.hasArg("eth_mask")) { n.ethMask = server.arg("eth_mask"); n.ethMask.trim(); }
  if (server.hasArg("eth_gw"))   { n.ethGw = server.arg("eth_gw"); n.ethGw.trim(); }
  if (server.hasArg("eth_dns"))  { n.ethDns = server.arg("eth_dns"); n.ethDns.trim(); }
  if (server.hasArg("ip_allow")) { n.ipAllow = server.arg("ip_allow"); n.ipAllow.replace(" ", ""); }
  if (server.hasArg("v_prog"))   { String t = server.arg("v_prog"); t.replace(",", "."); n.vProg = t.toFloat(); }
  bool pwSet = server.hasArg("port_wezla"), pwBad = false; uint32_t pwSek = 0;   // port wezla 4403 (bez zapisu do pliku)
  if (pwSet) pwBad = !brCzas(server.arg("port_wezla"), pwSek);
  String ipErr;                                        // IPW / IPE ze skryptu - ten sam format co w LoRa
  if (server.hasArg("ipw") && !ipLoraParse(server.arg("ipw"), n.staIp, n.staMask, n.staGw, n.staDns)) ipErr = "IPW";
  if (server.hasArg("ipe") && !ipLoraParse(server.arg("ipe"), n.ethIp, n.ethMask, n.ethGw, n.ethDns)) ipErr = "IPE";
  for (int i = 0; i < 5; i++)
    if (server.hasArg(CMD_KEYS[i]) && server.arg(CMD_KEYS[i]).length()) { n.cmd[i] = server.arg(CMD_KEYS[i]); n.cmd[i].trim(); n.cmd[i].toUpperCase(); }
  String err;
  if (pwBad) err = "PORT_WEZLA: 0 (wylaczony) albo czas np. 30m, 2h, 1d (max 7d)";
  else if (ipErr.length()) err = ipErr + ": podaj ip/maska,brama[,dns] np. 192.168.1.40/24,192.168.1.1 albo - (DHCP)";
  else if (n.apSsid.length() > 32 || n.staSsid.length() > 32) err = "SSID moze miec max 32 znaki";
  else if (n.apPass.length() < 8 || n.apPass.length() > 63) err = "Haslo AP: 8-63 znaki";
  else if (n.apPass == DEF_AP_PASS || n.apPass.length() < 12) err = "Haslo AP: min. 12 znakow i inne niz domyslne";
  else if (tokErr.length()) err = tokErr;
  else if (n.ipAllow.length() > 120) err = "Dozwolone IP: max 120 znakow";
  else if (n.netMode != "wifi" && n.netMode != "brak" && n.netMode != "eth" && n.netMode != "oba") err = "Tryb sieci: wifi, eth, oba albo brak";
  else if (!staIpOk(n.staIp, n.staMask, n.staGw, n.staDns)) err = "Staly adres Wi-Fi (STA): podaj poprawne IP, maske i brame (DNS opcjonalnie), albo zostaw IP puste (DHCP)";
  else if (!staIpOk(n.ethIp, n.ethMask, n.ethGw, n.ethDns)) err = "Staly adres Ethernet: podaj poprawne IP, maske i brame (DNS opcjonalnie), albo zostaw IP puste (DHCP)";
  else if (n.sta && !n.staSsid.length()) err = "Tryb STA wymaga nazwy sieci";
  else if (n.sta && n.staPass.length() && n.staPass.length() < 8) err = "Haslo sieci STA: min. 8 znakow";
  else if (n.senders.length() && n.senders.indexOf(',') < 0) err = "Filtr wezlow: puste (wszyscy) albo min. 2 wezly";
  else if (n.url.length() && !urlOk(n.url)) err = "Adres mtnod.txt: http://... albo https://..., max 190 znakow, bez spacji";
  else if (n.brakOdp != 0 && (n.brakOdp < 5 || n.brakOdp > 1440)) err = "Brak odpowiedzi wezla: 0 (wylaczone) albo 5-1440 min";
  else if (!gpioOk(n.rstPin)) err = "Pin resetu: -1 (brak), GP0-GP22 albo GP26-GP28";
  else if ((n.netMode == "eth" || n.netMode == "oba") && ethPinZajety(n.rstPin, n.ethMod))
    err = String("Pin resetu GP") + n.rstPin + " zajety przez modul Ethernet " + ETH_MOD[n.ethMod].nazwa + " - wybierz inny (np. GP15 albo GP22)";
  else if (n.rstSek < 1 || n.rstSek > 60) err = "Czas impulsu resetu: 1-60 s";
  else if (!pmTimeOk(n.pmTime)) err = "POWMAN pobudka: 0, co ile godzin (1-168), godziny np. 6,18 albo minuty np. 30m (min. 5m)";
  else if (n.pmCzuw < 1 || n.pmCzuw > 240) err = "POWMAN czuwanie: 1-240 min";
  else if (n.vKor < -1.0f || n.vKor > 1.0f) err = "Korekta napiecia: od -1.00 do 1.00 V";
  else if (n.vProg != 0 && (n.vProg < 3.0f || n.vProg > 5.5f)) err = "Prog napiecia: 0 (wylaczony) albo 3.00-5.50 V";
  for (char c : n.ipAllow) if (!isdigit(c) && c != '.' && c != ',' && c != '*') err = "Dozwolone IP: cyfry, kropki, przecinki i * (np. 192.168.1.98,192.168.1.*)";
  for (const String* v : { &n.apSsid, &n.apPass, &n.staSsid, &n.staPass, &n.senders, &n.url, &n.ipAllow,
                          &n.cmd[0], &n.cmd[1], &n.cmd[2], &n.cmd[3], &n.cmd[4] }) {
    if (v->indexOf('=') == 0) err = "Niedozwolony znak";
    for (char c : *v) if ((uint8_t)c < 32 || c == 127) err = "Niedozwolony znak sterujacy";   // m.in. \r \n NUL
  }
  for (int i = 0; i < 5 && !err.length(); i++) {
    if (n.cmd[i].length() < 3 || n.cmd[i].length() > 20 || n.cmd[i].indexOf(' ') >= 0 ||
        n.cmd[i].indexOf(':') >= 0 || n.cmd[i].indexOf('=') >= 0) err = "Komenda: 3-20 znakow, bez spacji, ':' i '='";
    if (n.cmd[i] == "HELP" || n.cmd[i] == "WERSJE" || n.cmd[i] == "AKTUALIZUJ_INTERNET" || n.cmd[i] == "USPIJ_POWMAN" ||
        n.cmd[i] == "RESTART_PICO" || n.cmd[i] == "DOWNGRADE" || n.cmd[i] == "PORT_WEZLA" || n.cmd[i] == "SOL" || n.cmd[i] == "STRONA" || n.cmd[i] == "ZASOBY")
      err = "Nazwy HELP, WERSJE, AKTUALIZUJ_INTERNET, USPIJ_POWMAN, RESTART_PICO, DOWNGRADE, PORT_WEZLA, SOL, STRONA i ZASOBY sa zarezerwowane";
    for (int j = 0; j < i; j++) if (n.cmd[i] == n.cmd[j]) err = "Komendy musza byc rozne";
  }
  if (err.length()) { server.send(400, "text/plain", err); return; }
  bool wifiZmiana = n.apSsid != cfg.apSsid || n.apPass != cfg.apPass || n.sta != cfg.sta ||
                    n.staSsid != cfg.staSsid || n.staPass != cfg.staPass || n.netMode != cfg.netMode || (netEth() && n.ethMod != cfg.ethMod) ||
                    n.staIp != cfg.staIp || n.staMask != cfg.staMask || n.staGw != cfg.staGw || n.staDns != cfg.staDns ||
                    (netEth() && (n.ethIp != cfg.ethIp || n.ethMask != cfg.ethMask || n.ethGw != cfg.ethGw || n.ethDns != cfg.ethDns));
  int staryPin = cfg.rstPin;
  bool hasloZm = n.tokHash != cfg.tokHash;
  if (hasloZm) { sesjeKasuj(); logf("[AUDYT] %s %s ZMIANA HASLA STRONY\n", klientIp().c_str(), audKto.c_str()); }
  String pref = hasloZm ? "HASLO STRONY ZMIENIONE - wszyscy zostali wylogowani, zaloguj sie NOWYM haslem.\n\n" : "";
  cfg = n;
  if (!saveSettings()) { server.send(500, "text/plain", "Blad zapisu"); return; }
  if (pwSet) brWlacz(pwSek, "strona/skrypt z " + klientIp());
  if (!wifiZmiana) {                                   // bez zmian Wi-Fi: zmiany dzialaja od razu, bez restartu Pico
    if (staryPin != cfg.rstPin) { if (staryPin >= 0) pinMode(staryPin, INPUT); rstPinIdle(); }
    else rstPinIdle();                                 // stan aktywny mogl sie zmienic
    logText("[PICO] Zapisano nowe ustawienia - dzialaja od razu (bez restartu)\n");
    server.send(200, "text/plain", pref + "Zapisano. Zmiany dzialaja od razu (Wi-Fi bez zmian - bez restartu Pico).");
    return;
  }
  logText("[PICO] Zapisano nowe ustawienia Wi-Fi - restart Pico\n");
  picoRebootAt = millis() + 1500;
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", pref + "Zapisano. Pico uruchomi sie ponownie za chwile.\n" +
              (n.sta ? String("Tryb STA: polacz sie z siecia '") + n.staSsid + "' i wejdz na http://" + MDNS_NAME +
                       ".local (albo adres z routera).\nJesli Pico nie polaczy sie w 30 s, wroci do trybu AP '" + n.apSsid + "'."
                     : String("Tryb AP: polacz sie z siecia '") + n.apSsid + "' i wejdz na http://192.168.4.1"));
}

static bool ledSt = false;                         // stan diody (wspolny z wifiOff)
// Dioda: Wi-Fi wlaczone = miga wolno (1 s / 1 s), odbior/flashowanie = miga szybko, uspienie = zgaszona
void ledService() {
  static uint32_t last = 0; bool& st = ledSt;
  if (millis() - last < 125) return;
  last = millis();
  if (wifiState == 1 && fwState != 1) return;      // uspione: radio wylaczone, dioda (przez CYW43) tez
  bool want = (fwState == 1 || upActive) ? ((millis() / 125) & 1) : ((millis() / 1000) & 1);
  if (want != st) { st = want; digitalWrite(LED_BUILTIN, st ? HIGH : LOW); }
}

// ---- Zapis logu na flash: co godzine (i gdy bufor RAM sie zapelnia), 2 pliki x 200 KB, starszy usuwany.
// Nigdy w trakcie odbioru pakietu / flashowania / konfiguracji (zapis na flash wstrzymuje oba rdzenie).
const char* FLOG_A = "/log_a.txt";                 // biezacy
const char* FLOG_B = "/log_b.txt";                 // poprzedni
static const size_t FLOG_MAX = 200 * 1024;
static uint32_t flogSaved = 0, flogLastMs = 0;
void flushLogToFlash(const char* powod, bool wymus) {
  if (!wymus && (fwState == 1 || upActive)) return;   // wymus: etapy flashowania (log musi przetrwac zanik zasilania)
  uint32_t pos; String body = logSince(flogSaved, &pos);
  flogSaved = pos; flogLastMs = millis();
  if (!body.length()) return;
  File a = LittleFS.open(FLOG_A, "a"); if (!a) return;
  char now[24]; nowStr(now, sizeof(now));
  a.printf("\n===== zapis %s (%s), uptime %lus =====\n", now, powod, (unsigned long)(millis() / 1000));
  a.print(body);
  size_t sz = a.size(); a.close();
  if (sz > FLOG_MAX) { LittleFS.remove(FLOG_B); LittleFS.rename(FLOG_A, FLOG_B); }
}
const char* BOOT_A = "/boot_a.txt";               // ostatni start wezla
const char* BOOT_B = "/boot_b.txt";               // poprzedni
void logFlashService() {
  if (!fsReady) return;
  if (bootStan == 1 && (millis() - bootStartMs > 60000 || bootLen >= BOOT_SIZE)) bootStan = 2;
  if (bootStan == 2 && fwState != 1 && !upActive) {   // zapis poza flashowaniem/odbiorem (LittleFS z rdzenia 1)
    LittleFS.remove(BOOT_B); LittleFS.rename(BOOT_A, BOOT_B);
    File f = LittleFS.open(BOOT_A, "w");
    if (f) { mutex_enter_blocking(&logMtx); f.write((const uint8_t*)bootBuf, bootLen); mutex_exit(&logMtx); f.close(); }
    bootStan = 0;
    logf("[PICO] Start wezla zapisany osobno (%u B) - przycisk Pobierz starty wezla\n", (unsigned)bootLen);
  }
  if (millis() - flogLastMs > 3600000UL) flushLogToFlash("co godzine");
  else if (logTotal - flogSaved > 36 * 1024) flushLogToFlash("bufor RAM pelny");
}
// skrypt z pamieci programu, w kawalkach (bez zmian - adres Pico i token podaje sie przy uruchomieniu)
void handleDownloadScript() {
  const size_t len = sizeof(SKRYPT_PY) - 1;
  server.sendHeader("Content-Disposition", "attachment; filename=\"wyslij_firmware.py\"");
  server.setContentLength(len);
  server.send(200, "text/x-python; charset=utf-8", "");
  for (size_t o = 0; o < len; o += 2048) { server.sendContent(SKRYPT_PY + o, len - o < 2048 ? len - o : 2048); rp2040.wdt_reset(); }
  logText("[PICO] Pobrano skrypt wyslij_firmware.py " SKRYPT_PY_WERSJA "\n");
}
void handleDownloadBoot() {                         // dwa ostatnie starty wezla: najpierw ostatni
  server.sendHeader("Content-Disposition", "attachment; filename=\"pico_starty_wezla.txt\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain; charset=utf-8", "");
  if (bootStan) server.sendContent("(trwa zbieranie albo zapis kolejnego startu wezla - pobierz ponownie za minute)\n\n");
  static char b[2048]; bool cos = false;
  for (const char* fn : { BOOT_A, BOOT_B }) {
    File f = LittleFS.open(fn, "r"); if (!f) continue; cos = true;
    int n; while ((n = f.read((uint8_t*)b, sizeof(b))) > 0) { server.sendContent(b, n); rp2040.wdt_reset(); }
    f.close(); server.sendContent("\n\n");
  }
  if (!cos) server.sendContent("Brak zapisanego startu wezla (zapis po kazdym starcie wezla, 60 s od ESP-ROM).\n");
  server.sendContent("");
}
void handleDownloadFlash() {                        // strumieniowo: oba pliki naraz nie zmieszcza sie w RAM
  flushLogToFlash("pobranie");
  server.sendHeader("Content-Disposition", "attachment; filename=\"pico_log_flash.txt\"");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain; charset=utf-8", "");
  static char b[2048];
  for (const char* fn : { FLOG_B, FLOG_A }) {
    File f = LittleFS.open(fn, "r"); if (!f) continue;
    int n; while ((n = f.read((uint8_t*)b, sizeof(b))) > 0) { server.sendContent(b, n); rp2040.wdt_reset(); }
    f.close();
  }
  server.sendContent("");
}

// BOOTSEL: krotkie nacisniecie (< 2 s) = restart wezla ESP32-S3 (RTS),
// przytrzymanie 30 s = ustawienia domyslne Pico + restart Pico. W trakcie flashowania nieaktywny.
void checkBootselReset() {
  static uint32_t held = 0, lastChk = 0;
  static bool zgloszony = false;
  if (millis() - lastChk < 100) return;
  lastChk = millis();
  if (fwState == 1) { held = 0; return; }
  if (BOOTSEL) {
    pmBootselOff();
    if (!held) { held = millis(); zgloszony = false; }
    else if (!zgloszony && millis() - held > 3000) { zgloszony = true; logText("[PICO] BOOTSEL trzymany - po 30 s ustawienia domyslne\n"); }
    if (millis() - held > 30000) {
      LittleFS.remove(SETTINGS_PATH);
      { File r = LittleFS.open("/reset.why", "w"); if (r) { r.print("przytrzymanie BOOTSEL 30 s"); r.close(); } }
      logText("[PICO] BOOTSEL 30 s - przywrocono ustawienia domyslne\n");
      flushLogToFlash("reset ustawien");
      delay(200);
      plannedReboot();
    }
  } else if (held) {
    uint32_t d = millis() - held;
    held = 0;
    if (pmBtnUsed) pmBtnUsed = false;                      // nacisniecie wylaczylo POWMAN - bez restartu wezla
    else if (d < 2000) { logText("[PICO] BOOTSEL krotko - restart wezla ESP32-S3\n"); reqCmd = 1; }
    else if (d < 30000) logText("[PICO] BOOTSEL puszczony przed 30 s - bez zmian\n");
  }
}

// Wi-Fi: STA (z powrotem do AP, gdy brak polaczenia) albo AP
// Staly adres STA: wszystkie pola puste albo IP + maska + brama poprawne (DNS opcjonalnie - domyslnie brama)
bool staIpOk(const String& ip, const String& mk, const String& gw, const String& dn) {
  if (!ip.length()) return true;
  IPAddress a;
  return a.fromString(ip) && a.fromString(mk) && a.fromString(gw) && (!dn.length() || a.fromString(dn));
}
// LoRa IPW/IPE: "ip/maska,brama[,dns]" (maska jako 24 albo 255.255.255.0), "-" = DHCP (puste pola)
bool ipLoraParse(String v, String& ip, String& mk, String& gw, String& dn) {
  v.replace(" ", ""); ip = mk = gw = dn = "";
  if (!v.length() || v == "-" || v == "dhcp" || v == "DHCP") return true;
  int sl = v.indexOf('/'), c1 = v.indexOf(','), c2 = c1 < 0 ? -1 : v.indexOf(',', c1 + 1);
  if (sl < 1 || c1 < 0 || sl > c1) return false;
  ip = v.substring(0, sl); mk = v.substring(sl + 1, c1);
  gw = c2 < 0 ? v.substring(c1 + 1) : v.substring(c1 + 1, c2);
  if (c2 >= 0) dn = v.substring(c2 + 1);
  if (mk.indexOf('.') < 0) {                           // prefiks /24 -> 255.255.255.0
    bool num = mk.length() > 0 && mk.length() <= 2; for (char ch : mk) if (!isdigit(ch)) num = false;
    int n = num ? mk.toInt() : -1;
    if (n < 1 || n > 30) return false;
    uint32_t m = 0xFFFFFFFFu << (32 - n);
    mk = String(m >> 24) + "." + String((m >> 16) & 255) + "." + String((m >> 8) & 255) + "." + String(m & 255);
  }
  return staIpOk(ip, mk, gw, dn);
}
// adres w formacie IPW/IPE: "ip/maska,brama[,dns]" albo "-" (DHCP)
String ipTxt(const String& ip, const String& mk, const String& gw, const String& dn) {
  if (!ip.length()) return "-";
  return ip + "/" + mk + "," + gw + (dn.length() ? "," + dn : String(""));
}
void startWifi() {
  lastStaTry = millis();
  WiFi.noLowPowerMode();          // bez oszczedzania energii radia: szybszy transfer, mniejsze opoznienia
  bool sprobuj = false;
  uint32_t limit = 30000;
  if (cfg.sta && cfg.staSsid.length()) {
    WiFi.mode(WIFI_STA);
    // Skan bywa pusty tuz po starcie radia - do 3 prob. Gdy sieci nie widac, i tak krotka proba polaczenia
    // (siec ukryta albo skan ja przeoczyl), dopiero potem AP.
    bool widoczna = false;
    for (int s = 0; s < 3 && !widoczna; s++) {
      int n = WiFi.scanNetworks();
      for (int i = 0; i < n && !widoczna; i++) widoczna = (WiFi.SSID(i) == cfg.staSsid);
      rp2040.wdt_reset(); pmBootselOff();
      if (!widoczna) delay(500);
    }
    if (!widoczna) { logf("[PICO] Wi-Fi STA: sieci '%s' nie widac w skanie - krotka proba polaczenia\n", cfg.staSsid.c_str()); limit = 12000; }
    sprobuj = true;
  }
  if (sprobuj) {
    WiFi.setHostname(MDNS_NAME);
    if (cfg.staIp.length()) {                          // staly adres zamiast DHCP
      IPAddress ip, mk, gw, dn;
      ip.fromString(cfg.staIp); mk.fromString(cfg.staMask); gw.fromString(cfg.staGw);
      if (!cfg.staDns.length() || !dn.fromString(cfg.staDns)) dn = gw;
      WiFi.config(ip, dn, gw, mk);
      logf("[PICO] Wi-Fi STA: staly adres %s, maska %s, brama %s, DNS %s\n", cfg.staIp.c_str(), cfg.staMask.c_str(), cfg.staGw.c_str(), dn.toString().c_str());
    }
    // Kilka podejsc w oknie czasowym: routery z "band steering" (np. MikroTik) odrzucaja
    // pierwsze proby na 2,4 GHz, zeby urzadzenie przeszlo na 5 GHz - Pico ma tylko 2,4 GHz.
    logf("[PICO] Wi-Fi STA: lacze z '%s'...\n", cfg.staSsid.c_str());
    uint32_t t0 = millis();
    int podejscie = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < limit) {
      podejscie++;
      WiFi.begin(cfg.staSsid.c_str(), cfg.staPass.c_str());
      uint32_t t1 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t1 < 7000 && millis() - t0 < limit) { delay(200); rp2040.wdt_reset(); pmBootselOff(); }
      if (WiFi.status() != WL_CONNECTED) {
        logf("[PICO] Wi-Fi STA: podejscie %d bez polaczenia (status %d) - ponawiam\n", podejscie, (int)WiFi.status());
        WiFi.disconnect(); delay(1000); rp2040.wdt_reset();
      }
    }
    if (WiFi.status() == WL_CONNECTED) {
      staActive = true; staFails = 0;
      logf("[PICO] Wi-Fi STA polaczone: IP %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      if (MDNS.begin(MDNS_NAME)) { MDNS.addService("http", "tcp", 80); logf("[PICO] mDNS: http://%s.local\n", MDNS_NAME); }
      setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); tzset();     // czas polski (z letnim)
      NTP.begin("ntp.task.gda.pl", "pool.ntp.org");
      NTP.waitSet([]() { rp2040.wdt_reset(); }, 5000);
      { char b[24]; nowStr(b, sizeof(b)); logf("[PICO] Czas z NTP: %s\n", timeFromNtp() ? b : "brak odpowiedzi serwera"); }
      return;
    }
    staFails++;
    logf("[PICO] Wi-Fi STA: brak polaczenia w %lu s (status %d) - uruchamiam AP; kolejna proba za %s, gdy nikt nie jest podlaczony\n",
         (unsigned long)(limit / 1000), (int)WiFi.status(), staFails <= 6 ? "5 min" : "4 h");
    WiFi.disconnect();
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(cfg.apSsid.c_str(), cfg.apPass.c_str());
  dnsServer.start(53, "*", WiFi.softAPIP());
  logf("[PICO] Wi-Fi AP '%s', IP %s\n", cfg.apSsid.c_str(), WiFi.softAPIP().toString().c_str());
}
void wifiOff() {
  digitalWrite(LED_BUILTIN, LOW); ledSt = false;     // po wybudzeniu ledService zapali ja ponownie
  if (staActive) MDNS.end();
  else dnsServer.stop();
  WiFi.disconnect(); WiFi.end();
  staActive = false;
  logText("[PICO] Wi-Fi wylaczone (uspienie)\n");
}
void startApOnly() {
  if (staActive) { MDNS.end(); WiFi.disconnect(); staActive = false; }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(cfg.apSsid.c_str(), cfg.apPass.c_str());
  dnsServer.start(53, "*", WiFi.softAPIP());
  logf("[PICO] Staly AP '%s' dla wezlow (UDP), IP %s\n", cfg.apSsid.c_str(), WiFi.softAPIP().toString().c_str());
}
// Wykonanie zadan Wi-Fi (rdzen 0) + ponowna proba STA co 4 h, gdy dziala zastepczy AP
void wifiService() {
  int r = wifiReq;
  if (r >= 0) {
    wifiReq = -1;
    if (r == 3) {                                            // zmiana ustawien Wi-Fi z LoRa - restart w biezacym stanie
      if (wifiState != 1) {
        logText("[PICO] Restart Wi-Fi po zmianie ustawien\n");
        wifiOff(); staFails = 0; lastStaTry = millis();
        if (wifiState == 2) startApOnly(); else startWifi();
      }
    } else if (r != wifiState) {
      if (wifiState != 1) wifiOff();
      wifiState = r;
      if (r == 0) { lastStaTry = millis(); startWifi(); }
      else if (r == 2) startApOnly();
    }
  }
  // Ponowne proby STA: pierwsze 6 co 5 min (chwilowy problem z siecia/routerem), potem co 4 h.
  // Tylko gdy nikt nie jest podlaczony do AP Pico.
  uint32_t odstep = staFails <= 6 ? 300000UL : STA_RETRY_MS;
  if (wifiState == 0 && cfg.sta && !staActive && millis() - lastStaTry > odstep && WiFi.softAPgetStationNum() == 0) {
    lastStaTry = millis();
    logf("[PICO] Ponowna proba polaczenia STA (proba %u)\n", staFails + 1);
    dnsServer.stop();
    startWifi();
  }
}

String myIp() { return staActive ? WiFi.localIP().toString() : WiFi.softAPIP().toString(); }
bool netEth()  { return cfg.netMode == "eth" || cfg.netMode == "oba"; }
bool netWifi() { return cfg.netMode == "wifi" || cfg.netMode == "oba"; }
bool ethUp() { return ethInit && ethP && eth.connected(); }
String ethTxt() {
  if (!netEth()) return "wylaczony";
  if (!ethInit || !ethP) return String("brak modulu ") + ETH_MOD[cfg.ethMod].nazwa;
  if (!eth.connected()) return "brak adresu (kabel / DHCP)";
  return "IP " + eth.localIP().toString() + (cfg.ethIp.length() ? " (staly)" : " (DHCP)");
}
// ---- Port wezla TCP 4403 ----
static uint64_t upSek() { return time_us_64() / 1000000ULL; }
// Czas wlaczenia: 0 = wylacz; liczba z m / h / d (bez jednostki = godziny), max 7 dni - potem sesja wygasa
bool brCzas(String v, uint32_t& sek) {
  v.trim(); v.toLowerCase(); v.replace(" ", "");
  if (v == "0") { sek = 0; return true; }
  if (!v.length()) return false;
  char u = v[v.length() - 1]; uint32_t mn = 3600;
  if (u == 'm') mn = 60; else if (u == 'h') mn = 3600; else if (u == 'd') mn = 86400; else if (!isdigit(u)) return false;
  String num = isdigit(u) ? v : v.substring(0, v.length() - 1);
  if (!num.length() || num.length() > 6) return false;
  for (char c : num) if (!isdigit(c)) return false;
  uint64_t t = (uint64_t)num.toInt() * mn;
  if (t == 0 || t > 7ULL * 86400) return false;
  sek = (uint32_t)t; return true;
}
String brTxt() {
  if (!brUntil) return "wylaczony";
  uint64_t now = upSek(); uint32_t zost = brUntil > now ? (uint32_t)(brUntil - now) : 0;
  String t = zost >= 86400 ? String(zost / 86400) + "d " + String((zost % 86400) / 3600) + "h"
           : zost >= 3600 ? String(zost / 3600) + "h " + String((zost % 3600) / 60) + "m" : String((zost + 59) / 60) + "m";
  if (timeFromNtp()) { time_t k = time(nullptr) + zost; struct tm tm; localtime_r(&k, &tm); char b[20];
    strftime(b, sizeof(b), zost >= 86400 ? "%Y-%m-%d %H:%M" : "%H:%M", &tm); t += String(" (do ") + b + ")"; }
  return "wlaczony, zostalo " + t + (brActive ? ", POLACZONY" : "");
}
static WiFiClient brCl;
static uint32_t brOd = 0;
static void brRozlacz(const char* why) {
  if (!brActive) return;
  brActive = false; delay(5);                          // rdzen 1 przestaje przekazywac dane
  brCl.stop();
  lastApiUse = millis();                               // wezel zostal w trybie API - straznik nie restartuje USB od razu
  logf("[PICO] Port wezla 4403: rozlaczono (%s), sesja %lu min%s - wracam do zwyklej pracy\n", why,
       (unsigned long)((millis() - brOd) / 60000), brLost ? (String(", utracono ") + String((unsigned long)brLost) + " B").c_str() : "");
}
void brWlacz(uint32_t sek, const String& kto) {
  if (!sek) {
    brRozlacz("port wylaczony");
    if (brUntil) { brUntil = 0; logf("[PICO] Port wezla 4403 wylaczony (%s)\n", kto.c_str()); }
    return;
  }
  brUntil = upSek() + sek;
  logf("[PICO] Port wezla 4403 wlaczony (%s): %s - tylko siec lokalna\n", kto.c_str(), brTxt().c_str());
}
static bool ipPrywatne(const String& ip) {
  IPAddress a; if (!a.fromString(ip)) return false;
  return a[0] == 10 || (a[0] == 192 && a[1] == 168) || (a[0] == 172 && a[1] >= 16 && a[1] <= 31);
}
void bridgeService() {
  static uint32_t lastOdm = 0;
  if (brUntil && upSek() >= brUntil) {                 // koniec czasu: rozlaczenie i powrot do zwyklej pracy
    brRozlacz("koniec czasu"); brUntil = 0;
    logText("[PICO] Port wezla 4403 wylaczony - uplynal czas\n");
    queueLora("PICO: port wezla 4403 zamkniety (uplynal czas)", 0);
  }
  WiFiClient nw = brSrv.accept();
  if (nw) {
    String ip = nw.remoteIP().toString(); const char* odm = nullptr;
    if (!brUntil) odm = "port wylaczony (PORT_WEZLA)";
    else if (!ipPrywatne(ip)) odm = "adres spoza sieci lokalnej";
    else if (!ipDozwolone(ip)) odm = "adres spoza listy IP_DOZWOLONE";
    else if (brActive) odm = "port zajety przez inne polaczenie";
    else if (fwState == 1 || seq || upActive) odm = "trwa operacja na firmware";
    if (odm) {
      if (millis() - lastOdm > 60000) { lastOdm = millis(); logf("[PICO] Port wezla 4403: odmowa dla %s - %s\n", ip.c_str(), odm); }
      nw.stop();
    } else {
      brCl = nw; brCl.setNoDelay(true);
      brUp.h = brUp.t = 0; brDown.h = brDown.t = 0; brLost = 0; __dmb();
      brOd = millis(); brActive = true;
      logf("[PICO] Port wezla 4403: polaczono z %s - wlasne API Pico, raporty LoRa i straznik wezla wstrzymane\n", ip.c_str());
    }
  }
  if (!brActive) return;
  if (!brCl.connected()) { brRozlacz("klient sie rozlaczyl"); return; }
  uint8_t b[512];
  for (int k = 0; k < 8; k++) {                        // klient -> wezel
    int av = brCl.available(); uint32_t f = brUp.wolne();
    if (av <= 0 || !f) break;
    int n = brCl.read(b, (size_t)min((uint32_t)av, min((uint32_t)sizeof(b), f)));
    if (n <= 0) break;
    brUp.put(b, n);
  }
  uint32_t n;                                          // wezel -> klient
  while ((n = brDown.get(b, sizeof(b))) > 0) {
    uint32_t off = 0, t0 = millis();
    while (off < n && brCl.connected() && millis() - t0 < 2000) { size_t w = brCl.write(b + off, n - off); if (w) off += w; else delay(1); }
    if (off < n) { brRozlacz("klient nie odbiera danych"); return; }
  }
}

// Start W5500: reset, SPI1, staly adres albo DHCP; wykrycie modulu przez begin() (rejestr wersji ukladu)
bool ethStart() {
  const EthPiny& m = ETH_MOD[cfg.ethMod];
  if (ethPinZajety(cfg.rstPin, cfg.ethMod)) {
    logf("[PICO] Ethernet: pin resetu wezla GP%d koliduje z modulem %s - Ethernet nie uruchomiony (zmien pin resetu)\n", cfg.rstPin, m.nazwa);
    return false;
  }
  pinMode(m.rst, OUTPUT); digitalWrite(m.rst, LOW); delay(5); digitalWrite(m.rst, HIGH); delay(150);
  if (!ethP) {
    if (cfg.ethMod == 1) { SPI.setSCK(m.sck); SPI.setTX(m.mosi); SPI.setRX(m.miso); e51 = new Wiznet5100lwIP(m.cs, SPI, m.irq); }
    else { SPI1.setSCK(m.sck); SPI1.setTX(m.mosi); SPI1.setRX(m.miso); e55 = new Wiznet5500lwIP(m.cs, SPI1, m.irq); }
  }
  if (cfg.ethIp.length()) {
    IPAddress ip, mk, gw, dn;
    ip.fromString(cfg.ethIp); mk.fromString(cfg.ethMask); gw.fromString(cfg.ethGw);
    if (!cfg.ethDns.length() || !dn.fromString(cfg.ethDns)) dn = gw;
    eth.config(ip, dn, gw, mk);
    logf("[PICO] Ethernet: staly adres %s, maska %s, brama %s, DNS %s\n", cfg.ethIp.c_str(), cfg.ethMask.c_str(), cfg.ethGw.c_str(), dn.toString().c_str());
  }
  if (!eth.begin()) { logf("[PICO] Ethernet: nie wykryto modulu %s\n", m.nazwa); return false; }
  ethInit = true;
  uint32_t t0 = millis();
  while (!eth.connected() && millis() - t0 < 15000) { delay(100); rp2040.wdt_reset(); }
  if (!eth.connected()) { logf("[PICO] Ethernet: modul %s jest, ale brak adresu w 15 s (kabel / DHCP) - dziala dalej w tle\n", m.nazwa); return false; }
  logf("[PICO] Ethernet polaczony: IP %s\n", eth.localIP().toString().c_str());
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); tzset();
  NTP.begin("ntp.task.gda.pl", "pool.ntp.org");
  NTP.waitSet([]() { rp2040.wdt_reset(); }, 5000);
  { char b[24]; nowStr(b, sizeof(b)); logf("[PICO] Czas z NTP: %s\n", timeFromNtp() ? b : "brak odpowiedzi serwera"); }
  return true;
}

void setup() {
  pmEarlyBoot();                  // POWMAN: czy to pobudka ze snu (przed wszystkim, co dotyka rejestrow POWMAN)
  if (!LittleFS.begin()) { logText("[PICO] LittleFS nie dal sie zamontowac - FORMATOWANIE (ustawienia utracone)\n");
                           LittleFS.format(); LittleFS.begin(); }
  // Znacznik sprzetowy "switched core power-down" w chip_reset NIE kasuje sie przy zwyklym restarcie - po jednej pobudce
  // kazdy kolejny restart (aktualizacja, przycisk) wygladalby jak pobudka. Pobudka = znacznik + plik stanu zapisany tuz przed snem.
  if (pmWoke && !LittleFS.exists(PM_STATE_PATH)) { pmWoke = false; pmByPin = pmByTimer = false; }
  switch (rp2040.getResetReason()) {
    case RP2040::PWRON_RESET:    bootReason = "wlaczenie zasilania"; break;
    case RP2040::RUN_PIN_RESET:  bootReason = "pin RUN"; break;
    case RP2040::SOFT_RESET:     bootReason = "programowy"; break;
    case RP2040::WDT_RESET:      bootReason = watchdog_hw->scratch[2] == PLANNED_MAGIC ? "restart na zadanie (strona/skrypt/LoRa/BOOTSEL)" : "WATCHDOG"; break;
    case RP2040::DEBUG_RESET:    bootReason = "debugger"; break;
    case RP2040::GLITCH_RESET:   bootReason = "GLITCH (zaklocenie zasilania)"; break;
    case RP2040::BROWNOUT_RESET: bootReason = "BROWNOUT (spadek napiecia!)"; break;
    default:                     bootReason = "nieznany"; break;
  }
  if (pmWoke) bootReason = pmByPin ? "wybudzenie z POWMAN (pin GP14)" : pmByTimer ? "wybudzenie z POWMAN (timer)" : "wybudzenie z POWMAN";
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); tzset();  // czas polski - takze dla czasu z sieci LoRa
  logf("[PICO] Wersja programu Pico: %s\n", PICO_VERSION);
  logf("[PICO] Przyczyna startu: %s\n", bootReason);
  bool naZadanie = rp2040.getResetReason() == RP2040::WDT_RESET && watchdog_hw->scratch[2] == PLANNED_MAGIC;
  watchdog_hw->scratch[2] = 0;
  if (rp2040.getResetReason() == RP2040::WDT_RESET && !naZadanie && !pmWoke) {
    logText("[PICO] *** START PO RESTARCIE Z WATCHDOGA ***\n");
    if (watchdog_hw->scratch[1] == CRUMB_MAGIC) {
      uint32_t c = watchdog_hw->scratch[0] & 0xFF, x = watchdog_hw->scratch[0] >> 8;
      logf("[PICO] Ostatni krok przed zawieszeniem: %s%s\n",
           c < sizeof(CRUMB_NAMES) / sizeof(CRUMB_NAMES[0]) ? CRUMB_NAMES[c] : "?",
           x ? (String(", komenda ") + String(x)).c_str() : "");
    }
  }
  else logText("[PICO] Normalny start\n");
  if (pmWoke) logText("[PICO] Start po glebokim snie POWMAN\n");
  else if (rp2040.getResetReason() == RP2040::PWRON_RESET || rp2040.getResetReason() == RP2040::BROWNOUT_RESET)
    bootReportWanted = true;                                 // raport o wlaczeniu zasilania (bootReportService)
  crumb(CR_IDLE);

  if (LittleFS.exists("/reset.why")) { logf("[PICO] Ustawienia domyslne przywrocone wczesniej: %s\n", readFileStr("/reset.why").c_str());
                                       LittleFS.remove("/reset.why"); }
  if (LittleFS.exists(PICO_BIN)) {                   // po aktualizacji programu Pico: czysty LittleFS, ustawienia zostaja
    loadSettings();
    String hist = readFileStr(NODE_INFO_PATH), loraOld = readFileStr(PICO_LORA_FLAG);
    LittleFS.format(); LittleFS.begin();
    saveSettings();
    if (loraOld.length()) queueLora(String("PICO: zaktualizowane ") + loraOld + " -> " + PICO_VERSION + ", ustawienia zachowane", 120000);
    resetNodeAfterUpdate = true; picoUpdBoot = true;
    { File r = LittleFS.open(NODE_RST_PATH, "w"); if (r) { r.print("1"); r.close(); } }   // przetrwa restart Pico przed resetem wezla                     // wezel pamieta tryb API sprzed aktualizacji - reset = swiezy start i logi
    if (hist.length()) { File h = LittleFS.open(NODE_INFO_PATH, "w"); if (h) { h.print(hist); h.close(); } }
    logf("[PICO] Program Pico zaktualizowany do wersji %s - LittleFS sformatowany od nowa\n", PICO_VERSION);
  }
  { String t = readFileStr(ETAP_PATH); t.trim();          // dziennik etapow flashowania - odczyt jeszcze bez rdzenia 1
    int p1 = t.indexOf('|'), p2 = t.indexOf('|', p1 + 1);
    if (p1 > 0 && p2 > p1) {
      etapStart = t.substring(0, p1).toInt(); etapStartProba = t.substring(p1 + 1, p2).toInt();
      strncpy(etapStartCzas, t.substring(p2 + 1).c_str(), sizeof(etapStartCzas) - 1);
      etapPkgOk = LittleFS.exists(PKG_OK_PATH);
      logf("[PICO] Dziennik flashowania: przerwany etap %d (%s), dotychczasowe ponowienia %d, pakiet na Pico: %s\n", etapStart,
           etapStart == 1 ? "zapis obrazow" : "konfiguracja", etapStartProba, etapPkgOk ? "jest" : "BRAK");
    } else if (t.length()) logText("[PICO] Dziennik flashowania nieczytelny - pomijam\n");
  }
  fsReady = true;
  if (LittleFS.exists(NODE_RST_PATH)) resetNodeAfterUpdate = true;   // reset wezla po aktualizacji Pico - obowiazkowy, do skutku
  pmOn = LittleFS.exists(PM_FLAG_PATH);
  pmLoadState();
  loadPkgMode();
  pinMode(LED_BUILTIN, OUTPUT);

  loadSettings();
  rstPinIdle();                   // pin resetu wezla w stanie nieaktywnym od startu
  rp2040.wdt_begin(8000);         // wczesniej: laczenie STA moze trwac do 30 s (karmione w petli)
  core1Beat = millis();
  vsysService(true);              // napiecie zasilania przed wlaczeniem Wi-Fi
  bool ethOk = netEth() ? ethStart() : false;     // Ethernet tylko w trybie eth / oba (domyslnie wylaczony)
  if (netWifi()) startWifi();
  else if (cfg.netMode == "eth" && !ethOk) {       // Ethernet nie dziala - awaryjnie Wi-Fi wg ustawien AP/STA
    logText("[PICO] Tryb sieci: eth, ale Ethernet niedostepny - awaryjnie wlaczam Wi-Fi\n");
    startWifi();
  } else {                        // "brak" albo "eth" z dzialajacym Ethernetem: Wi-Fi wylaczone
    wifiState = 1;
    logf("[PICO] Tryb sieci: %s - Wi-Fi wylaczone (awaryjnie: komenda LoRa WLACZ_PICO albo WLACZ_TRYB_AP)\n", cfg.netMode.c_str());
  }

  server.collectHeaders("Cookie", "X-Auth-Token", "X-Klient", "X-Licznik", "X-Podpis", "X-Chunk-SHA", "X-Total-Size", "X-Total-CRC", "X-Offset", "X-Chunk-CRC", "X-Time", "X-Target");
  server.on("/", []() {                              // strona glowna: adres spoza listy dostaje tylko odmowe
    String ip = klientIp();
    if (!ipDozwolone(ip)) { ipOdmowa(ip); server.sendHeader("Connection", "close"); server.send(403, "text/plain", "Adres IP " + ip + " niedozwolony"); return; }
    if (!cfg.strona) { server.sendHeader("Connection", "close"); server.send(403, "text/plain; charset=utf-8", "Strona Pico wylaczona (LoRa STRONA:WL wlacza). Skrypt dziala."); return; }
    handleRoot(); });
  server.on("/status",        []() { if (needAuth()) handleStatus(); });
  server.on("/log",           []() { if (needAuth()) handleLog(); });
  server.on("/picolog",       []() { if (needAuth()) handlePicoLog(); });
  server.on("/download",      []() { if (needAuth()) handleDownload(); });
  server.on("/download_pico", []() { if (needAuth()) handleDownloadPico(); });
  server.on("/download_flash", []() { if (needAuth()) handleDownloadFlash(); });
  server.on("/download_boot",  []() { if (needAuth()) handleDownloadBoot(); });
  server.on("/download_script", []() { if (needAuth()) handleDownloadScript(); });
  server.on("/settings",      HTTP_GET,  []() { if (needAuth()) handleSettingsGet(); });
  server.on("/settings",      HTTP_POST, []() { if (needAuth()) handleSettingsPost(); });
  // Akcje wymagaja hasla (X-Auth-Token) i nie dzialaja w trakcie flashowania
  auto akcja = [](uint8_t cmd, const char* opis) {
    if (!authOk()) return;
    if (fwState == 1) { server.send(409, "text/plain", "Trwa flashowanie/konfiguracja - poczekaj"); return; }
    reqCmd = cmd; server.send(200, "text/plain", opis);
  };
  server.on("/reinit",   HTTP_POST, [akcja]() { akcja(4, "Restart hosta USB wykonany"); });
  server.on("/hreset",   HTTP_POST, [akcja]() { akcja(1, "Reset ESP32-S3 wykonany"); });
  server.on("/hboot",    HTTP_POST, [akcja]() { akcja(2, "ESP32-S3 przelaczony w bootloader"); });
  server.on("/chiptest", HTTP_POST, [akcja]() { akcja(5, "Test bootloadera uruchomiony - wynik w zdarzeniach Pico"); });
  server.on("/pm_off", HTTP_POST, []() { if (!authOk()) return;
    bool byl = pmOn; pmSetFlag(false); pmSleepAt = pmDeadline = 0;
    if (byl) logText("[POWMAN] Tryb POWMAN wylaczony ze strony\n");
    server.send(200, "text/plain", byl ? "Tryb POWMAN wylaczony - Pico pracuje normalnie" : "Tryb POWMAN nie byl wlaczony"); });
  server.on("/rst_test", HTTP_POST, [akcja]() { if (cfg.rstPin < 0) { server.send(400, "text/plain", "Pin resetu wylaczony (-1) - ustaw go w ustawieniach"); return; }
    akcja(8, "Impuls na pinie resetu wyslany - opis w zdarzeniach Pico"); });
  server.on("/pkg_crc",  []() { if (!needAuth()) return;
    server.send(200, "text/plain", LittleFS.exists(PKG_OK_PATH) ? readFileStr(PKG_OK_PATH) : String("")); });
  server.on("/logcollect", HTTP_POST, []() { if (!needAuth()) return;
    cfg.logOn = server.arg("on") == "1"; saveSettings();
    logf("[PICO] Zbieranie logow ESP32-S3: %s\n", cfg.logOn ? "WLACZONE" : "WYLACZONE");
    server.send(200, "text/plain", cfg.logOn ? "Zbieranie logow wlaczone" : "Zbieranie logow wylaczone (Pico dalej nasluchuje komend)"); });
  server.on("/settings_reset", HTTP_POST, []() { if (!needAuth()) return;
    if (fwState == 1) { server.send(409, "text/plain", "Trwa flashowanie"); return; }
    LittleFS.remove(SETTINGS_PATH);
    { File r = LittleFS.open("/reset.why", "w"); if (r) { r.print("przycisk 'Ustawienia domyslne' na stronie"); r.close(); } }
    logText("[PICO] Przywrocono ustawienia domyslne (ze strony) - restart\n");
    picoRebootAt = millis() + 1500;
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", String("Ustawienia domyslne. Pico uruchomi sie ponownie jako AP '") + DEF_AP_SSID +
                "', haslo " + DEF_AP_PASS + ", adres http://192.168.4.1"); });
  server.on("/auth",     HTTP_POST, []() {           // stare logowanie haslem - wycofane od 0.40
    server.sendHeader("Connection", "close");
    server.send(410, "text/plain", "Logowanie haslem wycofane - odswiez strone albo zaktualizuj skrypt"); });
  // Logowanie bez wysylania hasla (od 0.40): 1) GET /hello -> sol + jednorazowe np (30 s, przypisane do IP)
  // 2) POST /auth2: nc + p = HMAC(K, "login"), gdzie K = HMAC(skrot_hasla, "K:" + np + ":" + nc); skrot = PBKDF2(haslo, sol)
  server.on("/hello",    HTTP_GET, []() {
    String ip = klientIp();
    server.sendHeader("Connection", "close");
    if (!ipDozwolone(ip)) { ipOdmowa(ip); server.send(403, "text/plain", "Adres IP " + ip + " niedozwolony"); return; }
    uint32_t b = blokadaSek(ip);
    if (b) { server.send(429, "text/plain", "Za duzo blednych hasel - sprobuj za " + String(b) + " s"); return; }
    uint8_t r[16]; for (int i = 0; i < 16; i += 4) { uint32_t x = rp2040.hwrand32(); memcpy(r + i, &x, 4); }
    String np = hexStr(r, 16);
    int w = 0; for (int i = 0; i < 8; i++) { if (hello[i].ip == ip) { w = i; break; } if (hello[i].t < hello[w].t) w = i; }
    hello[w].ip = ip; hello[w].np = np; hello[w].t = millis(); if (!hello[w].t) hello[w].t = 1;
    server.send(200, "text/plain", "sol=" + cfg.tokSol + "\nnp=" + np + "\nwer=" + PICO_VERSION + "\nid=" + cfg.picoId + "\n"); });
  server.on("/auth2",    HTTP_POST, []() {
    String ip = klientIp(), kto = server.arg("k") == "skrypt" ? "skrypt" : "strona";
    server.sendHeader("Connection", "close");
    if (!ipDozwolone(ip)) { ipOdmowa(ip); server.send(403, "text/plain", "Adres IP " + ip + " niedozwolony"); return; }
    uint32_t b = blokadaSek(ip);
    if (b) { server.send(429, "text/plain", "Za duzo blednych hasel - sprobuj za " + String(b) + " s"); return; }
    String np;
    for (auto& h : hello) if (h.t && h.ip == ip) { if (millis() - h.t < 30000) np = h.np; h.ip = ""; h.np = ""; h.t = 0; }
    String nc = server.arg("nc"), pr = server.arg("p"); uint8_t tmp[32], hb[32];
    if (!np.length() || !hexBytes(nc, tmp, 16) || !hexBytes(pr, tmp, 32)) { server.send(401, "text/plain", "Brak lub wygasle powitanie - zaloguj sie ponownie"); return; }
    if (!hexBytes(cfg.tokHash, hb, 32)) { server.send(500, "text/plain", "Brak skrotu hasla"); return; }
    String m = "K:" + np + ":" + nc; uint8_t K[32], P[32], Q[32];
    hmacSha256((const char*)hb, 32, m.c_str(), m.length(), K);
    hmacSha256((const char*)K, 32, "login", 5, P);
    if (!ctEq(hexStr(P, 32), pr)) { zlaProba(ip); server.send(401, "text/plain", "Zle haslo"); return; }
    dobraProba(ip);
    Sesja* w = &sesje[0];
    for (auto& x : sesje) if (!x.id.length() || millis() - x.last >= SESJA_MS) { w = &x; break; } else if (x.last < w->last) w = &x;
    char id[33];
    for (int i = 0; i < 4; i++) snprintf(id + i * 8, 9, "%08lx", (unsigned long)rp2040.hwrand32());
    w->id = id; w->ip = ip; w->last = millis(); w->kto = kto; w->k = hexStr(K, 32); w->lic = 0; w->okno = 0;
    pmExtend();
    logf("[AUDYT] %s %s LOGIN OK\n", ip.c_str(), kto.c_str());
    hmacSha256((const char*)K, 32, "pico", 4, Q);           // dowod dla klienta, ze to prawdziwe Pico
    server.sendHeader("Set-Cookie", String("sid=") + id + "; Path=/; HttpOnly; SameSite=Strict");
    server.send(200, "text/plain", String("sid=") + id + "\npico=" + hexStr(Q, 32) + "\nstan=" + (domyslneHasla() ? "ZMIEN" : "OK") + "\n"); });
  server.on("/downgrade", HTTP_POST, []() {         // przelacznik: wlacza na 24 h albo wylacza, gdy wlaczony
    if (!authOk()) return;
    if (downgradeOk()) { downgradeUstaw(false); logText("[PICO] Downgrade wylaczony ze strony\n"); server.send(200, "text/plain", "Downgrade wylaczony"); return; }
    if (time(nullptr) < 1700000000) { server.send(409, "text/plain", "Pico nie ma czasu z NTP - nie mozna wlaczyc"); return; }
    downgradeUstaw(true); logText("[PICO] Downgrade wlaczony ze strony (24 h, jednorazowo)\n");
    server.send(200, "text/plain", "Downgrade " + downgradeTxt() + " - jedno wgranie starszej wersji (wezla albo Pico)"); });
  server.on("/logout",   HTTP_POST, []() {
    String sid = sidZCiastka();
    for (auto& x : sesje) if (x.id.length() && sid.length() == 32 && ctEq(sid, x.id)) {
      x.id = ""; logf("[AUDYT] %s %s WYLOGOWANIE\n", klientIp().c_str(), x.kto.c_str()); }
    server.sendHeader("Set-Cookie", "sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", "Wylogowano"); });
  server.on("/fw_begin", HTTP_POST, handleFwBegin);
  server.on("/fw_chunk", HTTP_POST, handleFwChunkDone, handleFwChunkData);
  server.on("/fw_end",   HTTP_POST, handleFwEnd);
  server.on("/fw_flash",  HTTP_POST, handleFwFlash);
  server.on("/fw_status", []() { if (needAuth()) handleFwStatus(); });
  server.on("/cfg_send",  HTTP_POST, handleCfgSend);
  server.on("/info",      []() { if (needAuth()) handleInfo(); });
  server.on("/ap_off", HTTP_POST, []() {             // koniec tymczasowego AP (jak WYLACZ_TRYB_AP z LoRa)
    if (!needAuth()) return;
    logText("[PICO] Strona: koniec tymczasowego trybu AP\n");
    server.send(200, "text/plain", "OK - powrot do trybu z ustawien");
    wifiReq = 0;
  });
  server.on("/pico_reboot", HTTP_POST, []() {
    if (!authOk()) return;
    if (fwState == 1) { server.send(409, "text/plain", "Trwa flashowanie/konfiguracja - restart zablokowany"); return; }
    logText("[PICO] Restart Pico na zadanie\n");
    flushLogToFlash("restart Pico");
    picoRebootAt = millis() + 700;
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", "Restart Pico za chwile");
  });
  // Captive portal: telefony/Windows sprawdzaja internet pod roznymi adresami - przekieruj na strone Pico
  server.onNotFound([]() {
    server.sendHeader("Location", String("http://") + myIp() + "/", true);
    server.sendHeader("Connection", "close");
    server.send(302, "text/plain", "");
  });
  server.begin();
  if (!cfg.strona) logText("[PICO] Strona WWW wylaczona - dziala tylko skrypt (LoRa STRONA:WL wlacza)\n");
  brSrv.begin();                  // port wezla 4403 (przyjmuje tylko gdy wlaczony)

  core1Beat = millis();
  rp2040.wdt_begin(8000);
}

// =========================================================================
// POWMAN - gleboki sen Pico (stan P1.7: rdzenie, SRAM i XIP bez zasilania). Wybudzenie = pelny start Pico.
// Wzorowane na pico-sdk hardware_powman (BSD-3). Zrodla wybudzenia: timer AON (LPOSC) i poziom wysoki na GP14.
// =========================================================================
bool pmActive() { return pmOn; }
static inline void pmW(volatile uint32_t* r, uint32_t v) { *r = POWMAN_PASSWORD_BITS | (v & 0xffff); }
static inline void pmSet(volatile uint32_t* r, uint32_t b) { hw_set_bits(r, POWMAN_PASSWORD_BITS | b); }
static inline void pmClr(volatile uint32_t* r, uint32_t b) { hw_clear_bits(r, POWMAN_PASSWORD_BITS | b); }
static void pmScr(int i, uint32_t v16) { pmW(&powman_hw->scratch[i], v16 & 0xffff); }
static uint32_t pmScrGet(int i) { return powman_hw->scratch[i] & 0xffff; }
static uint64_t pmGetMs() {
  uint32_t hi = powman_hw->read_time_upper, lo;
  while (true) { lo = powman_hw->read_time_lower; uint32_t h2 = powman_hw->read_time_upper; if (h2 == hi) break; hi = h2; }
  return ((uint64_t)hi << 32) | lo;
}
static void pmSetFlag(bool on) {
  pmOn = on;
  if (on) { File f = LittleFS.open(PM_FLAG_PATH, "w"); if (f) { f.print("1"); f.close(); } pmScr(7, 0); }
  else LittleFS.remove(PM_FLAG_PATH);
}
// Pierwsza rzecz w setup(): czy to pobudka z POWMAN i czym; wylaczenie zrodel wybudzenia
void pmEarlyBoot() {
  pmDiagRst = powman_hw->chip_reset; pmDiagWhy = powman_hw->last_swcore_pwrup; pmDiagScr = powman_hw->scratch[0];
  if (pmScrGet(0) == PM_MAGIC || (pmDiagRst & POWMAN_CHIP_RESET_HAD_SWCORE_PD_BITS)) {
    pmWoke = true;
    uint32_t why = powman_hw->last_swcore_pwrup;
    pinMode(PM_WAKE_PIN, INPUT_PULLDOWN);
    pmByTimer = why & 0x40;
    pmByPin = (why & 0x1E) || (!pmByTimer && digitalRead(PM_WAKE_PIN));
    if (pmScrGet(0) == PM_MAGIC) {
    uint32_t t0 = pmScrGet(3) | (pmScrGet(4) << 16);
    pmSleptMs = (uint32_t)pmGetMs() - t0;
    pmPlanSec = pmScrGet(5) | (pmScrGet(6) << 16);
    pmCycle = pmScrGet(7);
    uint32_t unix0 = pmScrGet(1) | (pmScrGet(2) << 16);
    if (unix0 > 1700000000UL) { struct timeval tv = { (time_t)(unix0 + pmSleptMs / 1000), 0 }; settimeofday(&tv, nullptr); }
    }
    pmScr(0, 0);
  }
  pmClr(&powman_hw->timer, POWMAN_TIMER_PWRUP_ON_ALARM_BITS | POWMAN_TIMER_ALARM_ENAB_BITS);
  pmClr(&powman_hw->inte, POWMAN_INTE_TIMER_BITS);
  pmClr(&powman_hw->pwrup[0], POWMAN_PWRUP0_ENABLE_BITS);
}
// BOOTSEL wcisniety przy wlaczonym trybie POWMAN (przed snem, w czuwaniu, przy laczeniu Wi-Fi) = tryb wylaczony
bool pmBootselOff() {
  if (!pmOn || !fsReady || !BOOTSEL) return false;
  pmSetFlag(false); pmSleepAt = pmDeadline = 0; pmBtnUsed = true;
  logText("[POWMAN] BOOTSEL - tryb POWMAN wylaczony, Pico pracuje normalnie\n");
  return true;
}
// Po zamontowaniu LittleFS: dane snu z pliku (gdy scratch POWMAN nie przetrwal) + diagnostyka
void pmLoadState() {
  if (pmOn || pmWoke) logf("[POWMAN] diag: chip_reset=0x%08lx pwrup=0x%lx scratch0=0x%08lx\n",
                          (unsigned long)pmDiagRst, (unsigned long)pmDiagWhy, (unsigned long)pmDiagScr);
  if (pmOn && !pmWoke && (pmDiagRst & POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS)) {      // reset przyciskiem/pinem RUN = czlowiek
    pmSetFlag(false); logText("[POWMAN] Start z pinu RUN (przycisk) - tryb POWMAN wylaczony\n");
  }
  pmBootselOff();
  if (!LittleFS.exists(PM_STATE_PATH)) return;
  String st = readFileStr(PM_STATE_PATH); LittleFS.remove(PM_STATE_PATH);
  unsigned long unix0 = 0, t0 = 0, plan = 0, cyk = 0, apiAgo = 0xFFFFFFFFUL;
  int n = sscanf(st.c_str(), "%lu %lu %lu %lu %lu", &unix0, &t0, &plan, &cyk, &apiAgo);
  if (!pmWoke || n < 4) return;
  uint32_t spal = (uint32_t)pmGetMs() - (uint32_t)t0;
  if (n == 5 && apiAgo != 0xFFFFFFFFUL) pmApiAgoBoot = apiAgo + spal / 1000;
  if (pmCycle) return;                                          // dane snu juz ze scratch
  pmSleptMs = spal; pmPlanSec = plan; pmCycle = cyk;
  if (unix0 > 1700000000UL) { struct timeval tv = { (time_t)(unix0 + pmSleptMs / 1000), 0 }; settimeofday(&tv, nullptr); }
}
// Rzeczywista czestotliwosc LPOSC: timer liczy z LPOSC przy zalozeniu 32768 Hz, porownanie z micros() przez 2 s
static void pmLposcHz(uint32_t hz) {
  pmClr(&powman_hw->timer, POWMAN_TIMER_RUN_BITS);
  pmW(&powman_hw->lposc_freq_khz_int, hz / 1000);
  pmW(&powman_hw->lposc_freq_khz_frac, (hz % 1000) * 65536 / 1000);
  pmSet(&powman_hw->timer, POWMAN_TIMER_USE_LPOSC_BITS);
  pmSet(&powman_hw->timer, POWMAN_TIMER_RUN_BITS);
  uint32_t t0 = millis(); while (!(powman_hw->timer & POWMAN_TIMER_USING_LPOSC_BITS) && millis() - t0 < 100) {}
}
static uint32_t pmMeasureLposc() {
  pmLposcHz(32768);
  uint64_t a = pmGetMs(); uint32_t u0 = micros();
  while (micros() - u0 < 2000000UL) rp2040.wdt_reset();
  uint64_t b = pmGetMs(); uint32_t u1 = micros();
  uint32_t hz = (uint32_t)(32768.0 * (double)(b - a) * 1000.0 / (double)(u1 - u0) + 0.5);
  if (hz < 25000 || hz > 40000) hz = 32768;
  pmLposcHz(hz);
  return hz;
}
// Ile sekund spac i opis nastepnej pobudki
static uint32_t pmNextSleep(String& opis) {
  String t = cfg.pmTime;
  if (t == "0") { opis = "tylko pinem GP14"; return 0; }
  if (t.endsWith("m")) { uint32_t m = t.toInt(); opis = String("za ") + m + " min (test)"; return m * 60; }
  if (t.indexOf(',') < 0) { uint32_t h = t.toInt(); opis = String("za ") + h + " h"; return h * 3600; }
  int godz[6], n = 0, i = 0;
  while (i < (int)t.length() && n < 6) { int j = t.indexOf(',', i); if (j < 0) j = t.length();
    String h = t.substring(i, j); if (h.length()) godz[n++] = h.toInt(); i = j + 1; }
  time_t now = time(nullptr);
  if (now < 1700000000) { uint32_t h = 24 / (n ? n : 1); opis = String("za ") + h + " h (brak godziny - odstep zastepczy)"; return h * 3600; }
  uint32_t best = 0xFFFFFFFF;
  for (int k = 0; k < n; k++) {
    struct tm lt; localtime_r(&now, &lt); lt.tm_hour = godz[k]; lt.tm_min = 0; lt.tm_sec = 0;
    time_t cel = mktime(&lt);
    while (cel < now + 600) { lt.tm_mday++; cel = mktime(&lt); }
    if ((uint32_t)(cel - now) < best) best = cel - now;
  }
  time_t cel = now + best; struct tm c; localtime_r(&cel, &c);
  char b[40]; snprintf(b, sizeof(b), "ok. %02d:%02d", c.tm_hour, c.tm_min); opis = b;
  return best;
}
// Zasypianie: zapis logu, kalibracja LPOSC, zatrzymanie rdzenia 1, Wi-Fi off, zrodla wybudzenia, stan P1.7
bool pmGoSleep() {
  if (pmBusy()) { logText("[POWMAN] Trwa operacja na firmware - nie zasypiam\n"); return false; }   // ostatnia kontrola
  pinMode(PM_WAKE_PIN, INPUT_PULLDOWN); delay(5);
  if (digitalRead(PM_WAKE_PIN)) {
    logText("[POWMAN] GP14 w stanie wysokim - nie zasypiam (pin obudzilby Pico od razu). Ustaw pin wezla na 0.\n");
    return false;
  }
  String opis; uint32_t sek = pmNextSleep(opis);
  uint32_t hz = pmMeasureLposc();
  pmCycle++;
  logf("[POWMAN] Zasypiam (cykl %lu), pobudka %s; LPOSC %lu Hz; wybudzenie pinem GP14\n",
       (unsigned long)pmCycle, opis.c_str(), (unsigned long)hz);
  { uint64_t tf = pmGetMs(); uint32_t uf = time(nullptr) > 1700000000 ? (uint32_t)time(nullptr) : 0;
    File f = LittleFS.open(PM_STATE_PATH, "w");
    if (f) { f.printf("%lu %lu %lu %lu %lu", (unsigned long)uf, (unsigned long)(uint32_t)tf, (unsigned long)sek, (unsigned long)pmCycle,
                     (unsigned long)apiAgoSec()); f.close(); } }
  flushLogToFlash("POWMAN zasypiam");
  hw_clear_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS);   // watchdog wylaczony - rdzen 1 zaraz stanie
  pmPark = true;
  uint32_t t0 = millis(); while (!core1Parked && millis() - t0 < 3000) delay(1);
  wifiOff();
  gpio_init(23); gpio_set_dir(23, GPIO_OUT); gpio_put(23, 0); gpio_pull_down(23);   // CYW43 (Wi-Fi) bez zasilania
  hw_set_bits(&usb_hw->main_ctrl, USB_MAIN_CTRL_PHY_ISO_BITS);
  // dane dla nastepnego startu (scratch POWMAN przezywa sen, 16 bitow na rejestr)
  uint64_t teraz = pmGetMs(); uint32_t unix0 = time(nullptr) > 1700000000 ? (uint32_t)time(nullptr) : 0;
  pmScr(1, unix0); pmScr(2, unix0 >> 16); pmScr(3, (uint32_t)teraz); pmScr(4, (uint32_t)teraz >> 16);
  pmScr(5, sek); pmScr(6, sek >> 16); pmScr(7, pmCycle); pmScr(0, PM_MAGIC);
  for (int i = 0; i < 4; i++) powman_hw->boot[i] = 0;               // bez "wznowienia" - zawsze czysty start
  pmSet(&powman_hw->dbg_pwrcfg, POWMAN_DBG_PWRCFG_IGNORE_BITS);
  pmClr(&powman_hw->seq_cfg, POWMAN_SEQ_CFG_HW_PWRUP_SRAM0_BITS | POWMAN_SEQ_CFG_HW_PWRUP_SRAM1_BITS);
  // wybudzenie pinem: poziom wysoki na GP14
  gpio_set_input_enabled(PM_WAKE_PIN, true); gpio_pull_down(PM_WAKE_PIN);
  pmW(&powman_hw->pwrup[0], (POWMAN_PWRUP0_MODE_VALUE_LEVEL << POWMAN_PWRUP0_MODE_LSB) | POWMAN_PWRUP0_DIRECTION_BITS |
                            (PM_WAKE_PIN << POWMAN_PWRUP0_SOURCE_LSB));
  pmClr(&powman_hw->pwrup[0], POWMAN_PWRUP0_STATUS_BITS);
  pmSet(&powman_hw->pwrup[0], POWMAN_PWRUP0_ENABLE_BITS);
  // wybudzenie timerem
  if (sek) {
    uint64_t al = pmGetMs() + (uint64_t)sek * 1000;
    pmClr(&powman_hw->timer, POWMAN_TIMER_ALARM_ENAB_BITS);
    pmW(&powman_hw->alarm_time_15to0, al); pmW(&powman_hw->alarm_time_31to16, al >> 16);
    pmW(&powman_hw->alarm_time_47to32, al >> 32); pmW(&powman_hw->alarm_time_63to48, al >> 48);
    pmSet(&powman_hw->timer, POWMAN_TIMER_ALARM_BITS);                // skasowanie starego alarmu (bit kasowany zapisem 1)
    pmSet(&powman_hw->timer, POWMAN_TIMER_ALARM_ENAB_BITS | POWMAN_TIMER_PWRUP_ON_ALARM_BITS);
  }
  // zadanie stanu P1.7 (wszystkie przelaczane domeny wylaczone)
  pmClr(&powman_hw->state, POWMAN_STATE_REQ_IGNORED_BITS);
  pmW(&powman_hw->state, (0xFu << POWMAN_STATE_REQ_LSB) & POWMAN_STATE_REQ_BITS);
  bool ok = !(powman_hw->state & (POWMAN_STATE_REQ_IGNORED_BITS | POWMAN_STATE_BAD_SW_REQ_BITS));
  if (ok) { ok = false; for (int i = 0; i < 500; i++) if (powman_hw->state & POWMAN_STATE_WAITING_BITS) { ok = true; break; } }
  if (!ok) { pmScr(0, 0); plannedReboot(); }                          // nie wyszlo - zwykly restart zamiast snu
  noInterrupts();
  while (true) __wfi();
  return true;
}
// Rdzen 1: zatrzymanie przed snem (wolane z loop1)
void pmCore1Park() {
  tuh_deinit(0);
  core1Parked = true;
  noInterrupts();
  while (true) __wfi();
}
// Rdzen 0: logika trybu POWMAN (wolane z loop)
// Cokolwiek zwiazanego z firmware w toku = zakaz snu: flashowanie/konfiguracja wezla, przyjmowanie pakietu lub programu Pico,
// zlecone wgranie z LoRa/internetu, pobieranie z internetu, zaplanowany restart Pico (aktualizacja), sekwencja RTS/bootloader.
bool pmFwOp() {   // operacja na firmware (wylacza tryb POWMAN)
  return fwState == 1 || upActive || pendingFlashAt || dlReq >= 0 || picoRebootAt;
}
bool pmBusy() {   // zakaz snu: operacja na firmware, sekwencja RTS/bootloader albo wlaczony port wezla
  return pmFwOp() || seq != nullptr || brUntil;
}

void pmService() {
  static bool startDone = false;
  if (!startDone && millis() > 3000) {
    startDone = true;
    if (pmWoke) {
      char b[160];
      snprintf(b, sizeof(b), "[POWMAN] Wybudzenie %s po %lu min (plan %lu min), cykl %lu\n",
               pmByPin ? "PINEM GP14" : pmByTimer ? "timerem" : "(przyczyna nieznana)",
               (unsigned long)(pmSleptMs / 60000), (unsigned long)(pmPlanSec / 60), (unsigned long)pmCycle);
      logText(b);
    }
    if (pmOn && pmWoke && pmByPin) {
      pmSetFlag(false);
      logText("[POWMAN] Wybudzenie pinem - tryb POWMAN wylaczony, Pico pracuje normalnie\n");
      queueLora("PICO: wybudzone pinem GP14 - tryb POWMAN wylaczony", 120000);
    } else if (pmOn && pmWoke) {
      pmDeadline = millis() + (uint32_t)cfg.pmCzuw * 60000UL;
      logf("[POWMAN] Czuwanie %d min (Wi-Fi i komendy LoRa dzialaja)\n", cfg.pmCzuw);
    } else if (pmOn) {
      logText("[POWMAN] Start Pico w trybie POWMAN (zasilanie/restart) - zasypiam za 30 s (BOOTSEL = anuluj)\n");
      pmSleepAt = millis() + 30000;
    }
  }
  if (!pmOn) { pmDeadline = pmSleepAt = 0; return; }
  if (pmBusy()) {
    if (!pmFwOp()) { if (pmDeadline) pmDeadline = millis() + 60000; if (pmSleepAt) pmSleepAt = millis() + 60000; return; }
    if (pmOn) {                                     // operacja na firmware: tryb POWMAN wylaczany automatycznie                                     // ponowne wlaczenie tylko swiadomie (USPIJ_POWMAN:TAK)
      pmSetFlag(false); pmSleepAt = pmDeadline = 0;
      logText("[POWMAN] Operacja na firmware - tryb POWMAN wylaczony automatycznie (wlacz ponownie: USPIJ_POWMAN)\n");
      queueLora("PICO: tryb POWMAN wylaczony (operacja na firmware) - wlacz ponownie USPIJ_POWMAN", 0);
    }
    return;
  }
  if (pmDeadline && (int32_t)(millis() - pmDeadline) >= 0) {
    pmDeadline = 0;
    logText("[POWMAN] Koniec czuwania - sprawdzam wezel przed snem\n");
    pmCheckDone = false; pmCheckReq = true;
    uint32_t t0 = millis(); while (!pmCheckDone && millis() - t0 < 600000UL) { rp2040.wdt_reset(); server.handleClient(); delay(20); }
    uint32_t t1 = millis(); while (loraQn && millis() - t1 < 180000UL) { rp2040.wdt_reset(); delay(50); }   // raport LoRa wyslany
    pmSleepAt = millis() + 1000;
  }
  if (pmSleepAt && (int32_t)(millis() - pmSleepAt) >= 0) {
    pmSleepAt = 0;
    if (!pmGoSleep()) pmDeadline = millis() + (uint32_t)cfg.pmCzuw * 60000UL;   // nie wyszlo - kolejne czuwanie
  }
}
// Aktywnosc uzytkownika w czasie czuwania (komenda LoRa, logowanie na strone) przedluza czuwanie
void pmExtend() {
  if (pmOn && pmDeadline) { uint32_t d = millis() + (uint32_t)cfg.pmCzuw * 60000UL; if ((int32_t)(d - pmDeadline) > 0) pmDeadline = d; }
}
String pmStatus() {
  if (!pmOn) return "POWMAN wylaczony";
  String s = "POWMAN wlaczony (pobudka " + cfg.pmTime + ", czuwanie " + cfg.pmCzuw + " min";
  if (pmDeadline) s += String(", do snu ") + ((pmDeadline - millis()) / 60000 + 1) + " min";
  return s + ")";
}

// Restart na zadanie: znacznik w rejestrze watchdoga (przezywa restart), zeby po starcie nie pokazywac "WATCHDOG"
void plannedReboot() { watchdog_hw->scratch[2] = PLANNED_MAGIC; rp2040.reboot(); }

// Rdzen 1 (USB) nie odpowiada: zapis do zdarzen i logu we flash, zanim watchdog zrestartuje Pico
void core1Check() {
  static uint8_t zgloszen = 0;
  uint32_t beat = core1Beat;                   // najpierw odczyt bicia rdzenia 1, potem zegar (inaczej "ujemna" roznica)
  int32_t ile = (int32_t)(millis() - beat);
  if (ile < 4000) return;
  if (zgloszen >= 1 || fwState == 1) return;   // raz na start Pico (oszczedzamy flash)
  zgloszen++;
  uint32_t c = watchdog_hw->scratch[0] & 0xFF;
  logf("[PICO] *** Rdzen 1 (USB) nie odpowiada od %lu s, krok: %s, urzadzenie na USB: %s - zapis logu przed restartem ***\n",
       (unsigned long)(ile / 1000), c < sizeof(CRUMB_NAMES) / sizeof(CRUMB_NAMES[0]) ? CRUMB_NAMES[c] : "?",
       usbDevicePresent() ? "tak" : "nie");
  flushLogToFlash("zawieszenie rdzenia 1");
}

// Po starcie Pico (aktualizacja, pobudka z POWMAN, zasilanie) host USB bywa martwy: wezel pracuje, a linia
// wyglada na pusta - pomaga zwykly restart Pico. Robimy go sami RAZ na epizod: znacznik w LittleFS blokuje
// kolejne restarty, dopoki wezel sie nie pojawi (wezel naprawde bez zasilania = brak petli restartow).
void usbBootFixService() {
  static bool done = false, flag = false;
  if (!fsReady) return;
  bool jest = heltecMounted || usbDevicePresent();
  if (done) {                                            // znacznik zostal - usun, gdy wezel w koncu sie pojawi
    if (flag && jest) { LittleFS.remove(USBFIX_PATH); flag = false; }
    return;
  }
  if (jest) { if (LittleFS.exists(USBFIX_PATH)) LittleFS.remove(USBFIX_PATH); done = true; return; }
  if (millis() < 25000 || fwState == 1) return;
  done = true;
  if (pmWoke || pmOn) {                                  // tryb POWMAN: osobna procedura - bez restartu Pico,
    logText("[POWMAN] Po pobudce nie widac wezla na USB - bez restartu Pico, wezel sprawdzi koniec czuwania\n");
    return;                                              // wezel sprawdza pmNodeCheck przed snem (odzyskiwanie jak straznik)
  }
  if (LittleFS.exists(USBFIX_PATH)) {
    flag = true;
    logText("[PICO] Nadal nie widac wezla na USB po restarcie Pico - bez kolejnych restartow (straznik zajmie sie wezlem)\n");
    return;
  }
  File f = LittleFS.open(USBFIX_PATH, "w"); if (f) { f.print("1"); f.close(); }
  logText(picoUpdBoot ? "[PICO] Po aktualizacji Pico nie widac wezla na USB - jednorazowy restart Pico\n"
                      : "[PICO] Po starcie nie widac wezla na USB - jednorazowy restart Pico\n");
  flushLogToFlash("restart Pico (brak wezla na USB)");
  delay(200);
  plannedReboot();
}

// Obciazenie: petle nie czekaja (ciagle odpytywanie), wiec "% CPU" nic nie mowi - liczymy obiegi/s i najdluzszy obieg (5 s)
struct PetlaStat { uint32_t tp, n, mx, okno, hz, maxUs; };
static PetlaStat petla[2];
static uint32_t ramMinWolne = 0xFFFFFFFF;
static void petlaTik(int c) {
  PetlaStat& p = petla[c]; uint32_t t = micros();
  if (p.tp) { uint32_t d = t - p.tp; if (d > p.mx) p.mx = d; } else p.okno = t;
  p.tp = t; p.n++;
  if (t - p.okno >= 5000000) { p.hz = (uint32_t)((uint64_t)p.n * 1000000 / (t - p.okno)); p.maxUs = p.mx; p.n = 0; p.mx = 0; p.okno = t;
    if (c == 0) { uint32_t w = rp2040.getFreeHeap(); if (w < ramMinWolne) ramMinWolne = w; } }
}
// Kontrola zasobow (od 0.41): rekordy od startu (poza operacjami na firmware), proba RAM co godzine w spoczynku,
// ostrzezenia w zdarzeniach i przez LoRa (kazdy rodzaj najwyzej raz na 24 h). Progi stale - bez dodatkowych ustawien.
static const uint32_t ZAS_RAM_PROG = 30 * 1024;         // wolny RAM ponizej - ostrzezenie
static const uint32_t ZAS_WYCIEK = 16 * 1024;           // spadek wolnego RAM w spoczynku przez 6 h (co godzine coraz mniej)
static const uint32_t ZAS_PETLA0_MS = 5000, ZAS_PETLA1_MS = 1000;   // najdluzszy obieg petli: rdzen 0 (siec, strona), rdzen 1 (USB)
static const uint32_t ZAS_FLASH_PROG = 48 * 1024;       // wolne miejsce LittleFS ponizej - ostrzezenie
static uint32_t zasRek[2] = {0, 0};                     // najdluzszy obieg petli od startu [us]
static uint32_t zasRam10 = 0;                           // wolny RAM 10 min po starcie
static uint32_t zasProbki[7]; static uint8_t zasNp = 0; // wolny RAM w spoczynku, co godzine (ostatnie 7)
static uint32_t zasOstrz[4] = {0, 0, 0, 0};             // czas ostatniego ostrzezenia: RAM, wyciek, petla, flash
static uint16_t zasIle = 0;                             // liczba ostrzezen od startu
static uint32_t zasFlashWolne() { FSInfo fi; LittleFS.info(fi); return (uint32_t)(fi.totalBytes - fi.usedBytes); }
static String obciazenieTxt() {
  uint32_t tot = rp2040.getTotalHeap(), uz = rp2040.getUsedHeap();
  return " | RAM: " + String(uz / 1024) + "/" + String(tot / 1024) + " KB zajete (min. wolne " +
         String(ramMinWolne == 0xFFFFFFFF ? (tot - uz) / 1024 : ramMinWolne / 1024) + " KB) | petle: rdzen0 " + String(petla[0].hz) +
         "/s, max " + String(petla[0].maxUs / 1000) + " ms (rekord " + String(zasRek[0] / 1000) + "); rdzen1 " + String(petla[1].hz) +
         "/s, max " + String(petla[1].maxUs / 1000) + " ms (rekord " + String(zasRek[1] / 1000) + ") | ostrzezenia zasobow: " + String(zasIle);
}
String zasobyTxt() {                                    // komenda LoRa ZASOBY i wpis co 3 h (zapas: wolne / calosc)
  uint32_t wol = rp2040.getFreeHeap(), tot = rp2040.getTotalHeap();
  uint32_t mn = ramMinWolne == 0xFFFFFFFF ? wol : ramMinWolne;
  FSInfo fi; LittleFS.info(fi);
  String t = "PICO zasoby: RAM wolne " + String(wol / 1024) + "/" + String(tot / 1024) + " KB (" + String(tot ? wol * 100 / tot : 0) +
             "%), min " + String(mn / 1024) + " KB (" + String(tot ? mn * 100 / tot : 0) + "%)";
  if (zasRam10) t += ", po 10 min " + String(zasRam10 / 1024);
  t += "; rdzen0 " + String(petla[0].hz) + "/s max " + String(zasRek[0] / 1000) + " ms; rdzen1 " + String(petla[1].hz) + "/s max " +
       String(zasRek[1] / 1000) + " ms; flash wolne " + String((uint32_t)((fi.totalBytes - fi.usedBytes) / 1024)) + "/" +
       String((uint32_t)(fi.totalBytes / 1024)) + " KB; " + String(millis() / 3600000UL) + " h; ostrz. " + String(zasIle) + (zasIle ? "" : " OK");
  return t;
}
static void zasOstrzez(int rodzaj, const String& txt) {
  zasIle++;
  logf("[ZASOBY] UWAGA: %s\n", txt.c_str());
  if (zasOstrz[rodzaj] && millis() - zasOstrz[rodzaj] < 86400000UL) return;   // LoRa najwyzej raz na dobe dla rodzaju
  if (!cfg.zasLora) return;                             // opcja: ostrzezenia tylko w zdarzeniach
  zasOstrz[rodzaj] = millis() | 1;
  queueLora("PICO: UWAGA " + txt, 0);
}
void zasobyService() {                                  // rdzen 0, co 5 s
  static uint32_t t5 = 0, tFw = 0, tGodz = 0, t3h = 0;
  static uint32_t logRam = 0, logPetla = 0, logFlash = 0;   // wpis w zdarzeniach najwyzej co godzine
  uint32_t now = millis();
  if (now - t5 < 5000) return;
  t5 = now;
  if (pmFwOp() || seq) { tFw = now; return; }           // flashowanie, odbior, pobieranie, RTS: dlugie obiegi sa normalne
  if (now < 60000 || now - tFw < 15000) return;          // start Pico i 15 s po operacji
  for (int c = 0; c < 2; c++) {
    uint32_t m = petla[c].maxUs; if (m > zasRek[c]) zasRek[c] = m;
    uint32_t prog = (c == 0 ? ZAS_PETLA0_MS : ZAS_PETLA1_MS) * 1000;
    if (m > prog && (!logPetla || now - logPetla > 3600000UL)) {
      logPetla = now;
      zasOstrzez(2, "rdzen " + String(c) + (c ? " (USB)" : " (siec)") + " zajety " + String(m / 1000) + " ms (prog " + String(prog / 1000000) + " s) - " +
                  "Pico przeciazone, sprawdz zdarzenia");
    }
  }
  uint32_t wol = rp2040.getFreeHeap();
  if (!zasRam10 && now > 600000UL) zasRam10 = wol;
  if (wol < ZAS_RAM_PROG && (!logRam || now - logRam > 3600000UL)) {
    logRam = now;
    zasOstrzez(0, "malo RAM - wolne " + String(wol / 1024) + " KB (prog " + String(ZAS_RAM_PROG / 1024) + " KB)");
  }
  uint32_t fl = zasFlashWolne();
  if (fl < ZAS_FLASH_PROG && (!logFlash || now - logFlash > 3600000UL)) {
    logFlash = now;
    zasOstrzez(3, "malo miejsca we flashu - wolne " + String(fl / 1024) + " KB");
  }
  if (now - tGodz >= 3600000UL && !brActive && !upActive) {   // proba RAM w spoczynku co godzine
    tGodz = now;
    if (zasNp == 7) { memmove(zasProbki, zasProbki + 1, 6 * sizeof(uint32_t)); zasNp = 6; }
    zasProbki[zasNp++] = wol;
    if (zasNp == 7) {
      bool spada = true;
      for (int i = 1; i < 7; i++) if (zasProbki[i] + 1024 > zasProbki[i - 1]) spada = false;   // co godzine min. 1 KB mniej
      if (spada && zasProbki[0] - zasProbki[6] >= ZAS_WYCIEK)
        zasOstrzez(1, "mozliwy wyciek pamieci - wolny RAM spada od 6 h: " + String(zasProbki[0] / 1024) + " -> " + String(zasProbki[6] / 1024) + " KB");
    }
  }
  if (now - t3h >= 3 * 3600000UL) { t3h = now; logf("[ZASOBY] %s\n", zasobyTxt().c_str() + 5); }   // slad w zdarzeniach co 3 h
}
void loop() {
  setupKoniec = true;
  petlaTik(0);
  zasobyService();
  usbBootFixService();
  vsysService();
  { uint32_t beat = core1Beat; if ((int32_t)(millis() - beat) < 3000) rp2040.wdt_reset(); }
  if (wifiState != 1) {
    if (!staActive) dnsServer.processNextRequest();
    else MDNS.update();
  }
  if (wifiState != 1 || ethUp()) server.handleClient();   // strona przez Wi-Fi i/lub Ethernet
  bridgeService();
  loraCmdService();
  if (upActive && millis() - upLastMs > 60000) {           // przerwany odbior: zwolnij Pico po 60 s ciszy
    upActive = false; if (upFile) upFile.close(); LittleFS.remove(upPath);
    strcpy(fwMsg, "odbior przerwany");
    logf("[AUDYT] %s ODBIOR PRZERWANY - 60 s bez fragmentu, niepelny plik usuniety\n", upIp.c_str());
  }
  { static uint8_t prevFw = 0; uint8_t f = fwState;          // wynik operacji na firmware do dziennika
    if (prevFw == 1 && f != 1) logf("[AUDYT] %s %s %s - %s\n", opKto.length() ? opKto.c_str() : "?", f == 2 ? "OK" : "BLAD", "operacja na wezle", fwMsg);
    prevFw = f; }
  netService();
  wifiService();
  checkBootselReset();
  pmService();
  ledService();
  logFlashService();
  if (picoRebootAt && (int32_t)(millis() - picoRebootAt) > 0) { flushLogToFlash("restart Pico"); plannedReboot(); }
  core1Check();
}

// =========================================================================
// RDZEŃ 1: host USB, logi, sekwencje, test bootloadera, flashowanie
// =========================================================================
void setup1() {
  USBHost.begin(0);
  SerialHost.begin(115200);
}

// Pelny reset wezla: RTS, 5 s na start, restart hosta USB i czekanie na logi (sprawdzone recznie: sam RTS
// nie zawsze wystarcza - wezel po restarcie trzeba "podlaczyc" od nowa po stronie USB)
void resetNodeFull() {
  if (cdcIdx == 0xFF) { logText("\n[PICO] Brak ESP32-S3 - reset pominiety\n"); return; }
  logText("\n[PICO] Reset wezla (RTS) + restart hosta USB po 5 s\n");
  runSeqBlocking(SEQ_RESET, 2);
  apiPump(2000);                                     // + 3 s w waitNodeBoot = ok. 5 s na start wezla
  waitNodeBoot();
}

// ---- Straznik wezla: BRAK_ODPOWIEDZI ----
// Cisza na USB przez cfg.brakOdp minut -> pytanie przez API (wezel po uzyciu API do 15 min nie wysyla logow).
// Brak odpowiedzi -> restart hosta USB -> reset RTS -> impuls na pinie resetu (max 3 na dobe).
static uint32_t rstPulseAt[3] = { 0, 0, 0 };
void rstPinIdle() {
  if (cfg.rstPin < 0) return;
  digitalWrite(cfg.rstPin, cfg.rstHigh ? LOW : HIGH);
  pinMode(cfg.rstPin, OUTPUT);
  digitalWrite(cfg.rstPin, cfg.rstHigh ? LOW : HIGH);
}
void rstPulse() {
  if (cfg.rstPin < 0) { logText("[PICO] Pin resetu wylaczony (-1) - impuls pominiety\n"); return; }
  logf("\n[PICO] Impuls resetu wezla: GP%d w stanie %s przez %d s\n", cfg.rstPin, cfg.rstHigh ? "wysokim" : "niskim", cfg.rstSek);
  digitalWrite(cfg.rstPin, cfg.rstHigh ? HIGH : LOW);
  uint32_t t0 = millis();
  while (millis() - t0 < (uint32_t)cfg.rstSek * 1000UL) apiPump(100);
  digitalWrite(cfg.rstPin, cfg.rstHigh ? LOW : HIGH);
  logf("[PICO] Impuls resetu zakonczony (GP%d nieaktywny)\n", cfg.rstPin);
}
// Krotkie zapytanie API: czy wezel zyje (odpowiada my_info), potem rozlaczenie
bool apiProbe() {
  if (cdcIdx == 0xFF) return false;
  lastApiUse = millis();
  apiNodeNum = 0; apiConfigDone = false; apiFrames = 0;
  apiMode = true;
  bool ok = false;
  for (int i = 0; i < 2 && !ok; i++) {
    if (!usbWriteAll(API_WANTCFG, sizeof(API_WANTCFG))) break;
    uint32_t t0 = millis();
    while (millis() - t0 < 6000 && !apiNodeNum) apiPump(50);
    ok = apiNodeNum != 0;
  }
  if (ok) { uint32_t t0 = millis(); while (millis() - t0 < 15000 && !apiConfigDone) apiPump(50); }
  usbWriteAll(API_DISC, sizeof(API_DISC)); apiPump(300);
  apiMode = false;
  return ok;
}
// Czy wezel zyje po kroku odzyskiwania: nowe dane z USB albo odpowiedz na API
bool nodeAliveAfter(uint32_t since, uint32_t waitMs) {
  uint32_t t0 = millis();
  while (millis() - t0 < waitMs) {
    uint32_t r = lastRxMs;
    if (cdcIdx != 0xFF && r && (int32_t)(r - since) > 0) return true;
    apiPump(200);
  }
  return apiProbe();
}
void nodeWatchService() {
  static uint32_t lastCheck = 0;
  if (!cfg.brakOdp || fwState == 1 || seq || millis() < 180000) return;
  uint32_t now = millis(), lim = (uint32_t)cfg.brakOdp * 60000UL, rx = lastRxMs, api = lastApiUse;
  // po uzyciu API wezel do 15 min nie wysyla logow - inaczej sprawdzanie co kilka minut samo podtrzymywaloby cisze
  uint32_t apiQuiet = lim > 1200000UL ? lim : 1200000UL;
  if (now - rx < lim || now - lastCheck < lim || (api && now - api < apiQuiet)) return;
  lastCheck = now;
  uint32_t cisza = rx ? (now - rx) / 60000UL : now / 60000UL;
  if (heltecMounted && apiProbe()) {
    logf("[PICO] Straznik: %lu min bez logow, wezel odpowiada na API - wszystko w porzadku\n", (unsigned long)cisza);
    lastCheck = millis(); return;
  }
  if (!nodeRecover(cisza, "").length()) logf("[PICO] Straznik: wezel nadal nie odpowiada - kolejna proba za %d min\n", cfg.brakOdp);
  lastCheck = millis();
}
// Odzyskiwanie wezla: restart USB -> RTS -> impuls na pinie (bez USB przy pustej linii). Zwraca co pomoglo ("" = nic).
String nodeRecover(uint32_t cisza, const char* pref) {
  logf("\n[PICO] Straznik: wezel nie odpowiada od %lu min%s - odzyskiwanie\n", (unsigned long)cisza,
       cdcIdx == 0xFF ? " (nie widac go na USB)" : "");
  String pomoglo;
  uint32_t t = millis();
  if (usbDevicePresent()) {
    logText("[PICO] Straznik krok 1/3: restart hosta USB\n");
    if (restartUsbHost() && nodeAliveAfter(t, 60000)) pomoglo = "restart USB";
  } else logText("[PICO] Straznik: brak urzadzenia na linii USB (wezel bez zasilania?) - kroki 1-2 pominiete\n");
  if (!pomoglo.length() && cdcIdx != 0xFF) {
    logText("[PICO] Straznik krok 2/3: reset wezla RTS\n");
    t = millis(); resetNodeFull();
    if (nodeAliveAfter(t, 90000)) pomoglo = "reset RTS";
  }
  if (!pomoglo.length()) {
    int wolny = -1, uzyte = 0;
    for (int i = 0; i < 3; i++) { if (rstPulseAt[i] && millis() - rstPulseAt[i] < 86400000UL) uzyte++; else if (wolny < 0) wolny = i; }
    if (cfg.rstPin < 0) logText("[PICO] Straznik krok 3/3: pin resetu wylaczony - pomijam\n");
    else if (wolny < 0) logf("[PICO] Straznik krok 3/3: wykorzystany limit %d impulsow resetu na dobe - pomijam\n", uzyte);
    else {
      logText("[PICO] Straznik krok 3/3: impuls na pinie resetu\n");
      rstPulseAt[wolny] = millis();
      t = millis(); rstPulse(); waitNodeBoot();
      if (nodeAliveAfter(t, 120000)) pomoglo = String("impuls GP") + cfg.rstPin;
    }
  }
  if (pomoglo.length()) {
    logf("[PICO] Straznik: wezel znow odpowiada - pomogl %s\n", pomoglo.c_str());
    queueLora(String("PICO: ") + pref + "wezel nie odpowiadal " + cisza + " min - pomogl " + pomoglo, 60000);
  }
  return pomoglo;
}


void pmNodeCheck() {
  uint32_t rx = lastRxMs;
  if (cdcIdx != 0xFF && rx && millis() - rx < 300000UL) { logText("[POWMAN] Wezel aktywny (dane z USB)\n"); return; }
  uint32_t ago = apiAgoSec();
  if (heltecMounted && ago < 1200) {                 // kazde sprawdzenie API przedluza wezlowi tryb API (cisza w logach)
    logf("[POWMAN] Wezel na USB, kontakt API %lu min temu - bez sprawdzania (nie przedluzam trybu API)\n", (unsigned long)(ago / 60));
    return;
  }
  if (heltecMounted && apiProbe()) { logText("[POWMAN] Wezel odpowiada na API\n"); return; }
  if (!nodeRecover(rx ? (millis() - rx) / 60000UL : millis() / 60000UL, "(POWMAN) ").length())
    logText("[POWMAN] Wezel nadal nie odpowiada - zasypiam, sprobuje przy nastepnej pobudce\n");
}

void loop1() {
  petlaTik(1);
  core1Beat = millis();
  if (pmPark) pmCore1Park();
  if (pmCheckReq) { pmCheckReq = false; pmNodeCheck(); pmCheckDone = true; }
  USBHost.task();
  crumb(CR_IDLE);

  // Po starcie Pico (zwlaszcza po aktualizacji programu) wezel z zasilaniem bywa uspiony na USB i sie nie zglasza -
  // restart hosta USB po 6, 15 i 30 s, dopoki wezel sie nie pojawi
  static uint8_t bootUsbTries = 0;
  static const uint32_t BOOT_USB_AT[3] = { 6000, 15000, 30000 };
  static uint32_t presentSince = 0;
  if (usbDevicePresent()) { if (!presentSince) presentSince = millis(); } else presentSince = 0;
  if (bootUsbTries < 3 && !heltecMounted && fwState != 1 && millis() > BOOT_USB_AT[bootUsbTries] &&
      presentSince && millis() - presentSince > 3000) {
    bootUsbTries++;
    logf("[PICO] Wezel nie widoczny na USB po starcie - restart hosta USB (%u/3)\n", bootUsbTries);
    restartUsbHost();
  } else if (bootUsbTries < 3 && heltecMounted) bootUsbTries = 3;   // wezel jest - koniec sprawdzania
  static bool pustaZgloszona = false;
  if (!pustaZgloszona && !heltecMounted && !presentSince && millis() > 6000) {
    pustaZgloszona = true;
    logText("[PICO] Brak urzadzenia na linii USB - czekam na wezel (bez restartow hosta)\n");
  }

  // Po aktualizacji programu Pico: reset wezla (jak przycisk "Reset ESP32-S3"), potem host USB restartuje sie sam
  if (resetNodeAfterUpdate && heltecMounted && !seq && fwState != 1 && millis() > 8000) {
    resetNodeAfterUpdate = false;
    logText("[PICO] Po aktualizacji Pico: reset wezla (RTS), zeby wystartowal od nowa z logami\n");
    resetNodeFull();
    if (LittleFS.exists(NODE_RST_PATH)) LittleFS.remove(NODE_RST_PATH);
  }

  logApiService();

  // Po restarcie samego Pico wezel bywa w trybie API sprzed restartu i nie wysyla logow tekstowych (do 15 min).
  // Raz po starcie: 20 s ciszy -> polaczenie API i "disconnect" (wezel wraca do logow); dalej cisza -> reset RTS.
  static uint8_t logFix = 0; static uint32_t logFixT = 0, logFixB = 0;
  if (logFix < 3 && heltecMounted && fwState != 1 && !seq && !resetNodeAfterUpdate && !brActive) {   // po aktualizacji: reset wyzej
    if (!logFixT) { logFixT = millis(); logFixB = nodeLines; }
    else if (millis() - logFixT > 30000) {
      bool cisza = nodeLines == logFixB;                // ani jednej linii logu przez 30 s
      if (logFix == 0) {
        if (cisza && (pmWoke || pmOn)) {
          // POWMAN: bez polaczenia API i bez resetu. Kazdy kontakt API (tez "disconnect") odnawia na wezle
          // 15-minutowy tryb API, wiec logi nigdy by nie wrocily. Bez kontaktu wroca same.
          logText("[POWMAN] Wezel bez logow tekstowych (po komendzie/odpowiedzi LoRa przelaczyl port w tryb API do restartu) - "
                  "bez resetu wezla. Aby logi plynely: w aplikacji Meshtastic Konfiguracja > Security > Debug log (security.debug_log_api_enabled = true), "
                  "teraz: przycisk Reset wezla\n");
          logFix = 3;
        } else if (cisza) {
          logText("[PICO] Wezel podlaczony, ale nie wysyla logow (tryb API sprzed restartu Pico) - zamykam sesje API\n");
          if (apiConnect()) { usbWriteAll(API_DISC, sizeof(API_DISC)); apiPump(300); }
          apiMode = false;
          logFix = 1; logFixT = millis(); logFixB = nodeLines;
        } else logFix = 3;
      } else if (logFix == 1) {
        if (cisza) { logText("[PICO] Wezel dalej bez logow - reset wezla (RTS)\n"); resetNodeFull(); }
        else logText("[PICO] Logi wezla wrocily\n");
        logFix = 3;
      }
    }
  }

  // Pico zrestartowalo sie w trakcie wysylania konfiguracji po flashowaniu - jedna proba wznowienia
  static bool pendingChecked = false;
  if (!pendingChecked && fsReady && setupKoniec && millis() > 3000) {
    pendingChecked = true;
    if (etapStart) {
      int etap = etapStart, proba = etapStartProba + 1;
      strncpy(opTime, etapStartCzas, sizeof(opTime) - 1);
      int maks = 3;                                     // "do 3 razy sztuka" - zapis obrazow i konfiguracja
      if (!etapPkgOk || proba > maks || (etap != 1 && etap != 2)) {
        LittleFS.remove(ETAP_PATH);
        logf("[PICO] Przerwane flashowanie (etap %d) - nie ponawiam (%s)\n", etap, etapPkgOk ? "limit prob" : "brak pakietu");
        queueLoraReport(etap == 1 ? String("PICO: BLAD - wgrywanie wezla przerwane restartem Pico, ponowienia nieudane. Wezel moze byc bez programu!")
                                  : String("PICO: BLAD - konfiguracja przerwana restartem Pico, ponowienia nieudane. Wyslij konfiguracje ponownie"), LORA_REPORT_CFG_MS);
      } else {
        etapProba = proba;
        opKto = "wznowienie po restarcie Pico";
        logf("[PICO] Wznawiam przerwane flashowanie po restarcie Pico: %s, proba %d/%d\n", etap == 1 ? "zapis obrazow" : "konfiguracja", proba, maks);
        etapZapisz(etap, proba);                        // licznik prob zapisany PRZED ponowieniem (kolejny restart = kolejna proba)
        fwState = 1; fwSet(0, "Wznowienie: czekam na wezel");
        uint32_t t0 = millis();
        lastRxMs = 0;
        if (etap == 1) {                                 // wezel po kasowaniu moze byc tylko w bootloaderze - wystarczy, ze jest na USB
          while (millis() - t0 < 60000 && cdcIdx == 0xFF) apiPump(100);
          if (!flashPackage()) queueLoraReport(String("PICO: BLAD ponowionego wgrywania wezla - ") + fwMsg, LORA_REPORT_CFG_MS);
        } else {
          while (millis() - t0 < 60000 && !(cdcIdx != 0xFF && millis() - lastRxMs < 3000)) apiPump(100);
          fwSet(1, "Wznowienie: czekam %lu s na pelny start wezla", (unsigned long)(CFG_BOOT_WAIT_MS / 1000));
          apiPump(CFG_BOOT_WAIT_MS);
          bool ok = configFromPackage();
          LittleFS.remove(ETAP_PATH);
          queueLoraReport(ok ? String("PICO: konfiguracja wznowiona po restarcie Pico - OK") : String("PICO: BLAD konfiguracji po wznowieniu - ") + fwMsg, LORA_REPORT_CFG_MS);
        }
        etapProba = 0;
      }
    }
  }

  uint8_t cmd = reqCmd;
  if (cmd) {
    reqCmd = 0;
    if      (cmd == 1) resetNodeFull();
    else if (cmd == 2) startSeq(SEQ_BOOT, 5, "bootloader DTR/RTS");
    else if (cmd == 4) restartUsbHost(true);           // przycisk na stronie: restart nawet przy pustej linii
    else if (cmd == 5) { seq = nullptr; bootloaderTest(); }
    else if (cmd == 6) { seq = nullptr; flashPackage(); }
    else if (cmd == 7) { seq = nullptr; configFromPackage(); }
    else if (cmd == 8) rstPulse();
  }

  runSeq();
  readHeltec();
  if (brActive) {                                     // port wezla: dane od klienta do wezla
    uint8_t b[256]; uint32_t n;
    while ((n = brUp.get(b, sizeof(b))) > 0) usbWriteAll(b, n);
    return;                                           // w czasie sesji: bez wlasnego API, raportow i straznika wezla
  }
  bootReportService();
  loraQService();

  nodeWatchService();     // BRAK_ODPOWIEDZI > 0: wykrywanie zawieszenia wezla i odzyskiwanie

  // Prosty straznik (BRAK_ODPOWIEDZI = 0): ESP32-S3 podlaczony, a od 5 min zadnych danych (Meshtastic loguje co minute)
  static uint32_t lastFix = 0;
  if (!cfg.brakOdp && heltecMounted && fwState != 1 && millis() - lastRxMs > 300000 && millis() - lastFix > 300000 &&
      millis() - lastApiUse > 1200000) {               // po uzyciu API wezel do 15 min nie wysyla tekstu - to nie awaria USB
    logText("[PICO] 5 min bez danych z ESP32-S3 - restart hosta USB\n");
    lastFix = millis();
    restartUsbHost();
    lastRxMs = millis();
  }
}

// ---- Komendy z LoRa (kanal prywatny) ----
// Z logu debug wezla: "handleReceived(REMOTE) (id=0x.. fr=0x.. ... Ch=N Portnum=1" i potem
// "Received text msg from=0x.., id=0x.., msg=TEKST". Wykonywane tylko gdy kanal i nadawca sie zgadzaja.
static uint32_t hexAfter(const char* s, const char* key) { const char* p = strstr(s, key); return p ? strtoul(p + strlen(key), nullptr, 16) : 0; }
bool senderAllowed(uint32_t from) {
  if (!cfg.senders.length()) return true;
  char h[12]; snprintf(h, sizeof(h), "%08lx", (unsigned long)from);
  String list = "," + cfg.senders + ",";
  return list.indexOf(String(",") + h + ",") >= 0 || list.indexOf(String(",!") + h + ",") >= 0;
}
// Rdzen 1: odsiewa duplikaty (ta sama wiadomosc z logu i z pakietu) i przekazuje tekst do rdzenia 0
void loraCmd(const char* msg, uint32_t from, uint32_t id) {
  if (fwState == 1 || !msg[0]) return;
  static uint32_t lastId = 0, lastFrom = 0, lastMs = 0; static char lastMsg[201] = "";
  if (from == lastFrom && millis() - lastMs < 10000 && ((id && id == lastId) || !strcmp(msg, lastMsg))) return;
  lastId = id; lastFrom = from; lastMs = millis(); strncpy(lastMsg, msg, sizeof(lastMsg) - 1);
  if (cmdPending) return;                                   // poprzednia jeszcze nieobsluzona
  strncpy(cmdBuf, msg, sizeof(cmdBuf) - 1); cmdBuf[sizeof(cmdBuf) - 1] = 0; cmdFrom = from; cmdPending = true;
}
static bool isHexStr(const String& v) { for (size_t i = 0; i < v.length(); i++) if (!isxdigit(v[i])) return false; return v.length() > 0; }
static String statusLine() {
  String s = String("PICO Wi-Fi: wybrany tryb ") + (cfg.sta ? "STA" : "AP");
  if (wifiState == 1) s += " (Wi-Fi uspione)";
  else if (wifiState == 2) s += " (teraz staly AP)";
  else if (cfg.sta && !staActive) s += " (brak sieci, AP zastepczy)";
  s += ", AP '" + cfg.apSsid + "', STA " + (cfg.staSsid.length() ? "'" + cfg.staSsid + "'" : String("brak"));
  s += ", IP " + (wifiState == 1 ? String("-") : myIp());
  if (staActive && wifiState == 0) s += ", RSSI " + String(WiFi.RSSI()) + " dBm";
  s += "; tryb sieci " + cfg.netMode + (netEth() ? ", Ethernet " + ethTxt() : String(""));
  if (brUntil) s += "; port wezla 4403 " + brTxt();
  return s;
}
// Rdzen 0: wykonanie komendy (ustawienia, Wi-Fi) i odpowiedz do kolejki
void loraCmdService() {
  if (!cmdPending) return;
  String msg = cmdBuf; uint32_t from = cmdFrom; cmdPending = false;
  msg.trim();
  // Komenda ustawiajaca: KLUCZ:wartosc albo KLUCZ=wartosc (wartosc bez separatora i spacji z brzegow)
  int c1 = msg.indexOf(':'), c2 = msg.indexOf('=');
  int sep = c1 < 0 ? c2 : (c2 < 0 ? c1 : min(c1, c2));
  String key, val; bool hasVal = false;
  if (sep > 0) { key = msg.substring(0, sep); key.trim(); if (key.indexOf(' ') < 0) { val = msg.substring(sep + 1); val.trim(); hasVal = true; } }
  if (!hasVal) { int sp = msg.indexOf(' '); key = sp > 0 ? msg.substring(0, sp) : msg; }
  key.toUpperCase();
  // Wartosc w cudzyslowie: "..." albo z klawiatury telefonu „...” / “...” - wtedy spacje w srodku i na brzegach zostaja
  if (hasVal) {
    static const char* const QO[] = { "\"", "\xE2\x80\x9E", "\xE2\x80\x9C", "\xE2\x80\x9D" };
    for (const char* a : QO) {
      if (!val.startsWith(a)) continue;
      String rest = val.substring(strlen(a));
      for (const char* b : QO) if (rest.endsWith(b) && rest.length() >= strlen(b)) { val = rest.substring(0, rest.length() - strlen(b)); break; }
      break;
    }
  }
  static const char* const SET_KEYS[] = { "AP_SSID", "AP_PWD", "STA_SSID", "STA_PWD", "MODE_WIFI", "FILTR_NODE", "ADRES_URL", "BRAK_ODPOWIEDZI",
                                          "POWMAN_TIME", "POWMAN_CZUWANIE", "IP_DOZWOLONE", "IPW", "IPE", "TRYB_SIECI", "HOP" };
  int setIdx = -1, cmdIdx = -1;
  if (hasVal) for (int i = 0; i < 15; i++) if (key == SET_KEYS[i]) setIdx = i;
  if (!hasVal) for (int i = 0; i < 5; i++) if (cfg.cmd[i] == key) cmdIdx = i;
  bool help = !hasVal && key == "HELP", wersje = !hasVal && key == "WERSJE";
  bool dl = hasVal && key == "POBIERZ_INTERNET", upd = !hasVal && key == "AKTUALIZUJ_INTERNET";
  bool spij = key == "USPIJ_POWMAN", dgr = key == "DOWNGRADE", rst = key == "RESTART_PICO", pw = key == "PORT_WEZLA";
  bool sol = !hasVal && key == "SOL", str = key == "STRONA", zas = !hasVal && key == "ZASOBY";
  if (setIdx < 0 && cmdIdx < 0 && !help && !wersje && !dl && !upd && !spij && !dgr && !rst && !pw && !sol && !str && !zas) return;   // zwykla wiadomosc na kanale
  if (!senderAllowed(from)) { logf("[PICO] Komenda od niedozwolonego nadawcy !%08lx - pominieta\n", (unsigned long)from); return; }
  logf("[AUDYT] LoRa !%08lx KOMENDA %s\n", (unsigned long)from, key.c_str());
  { char k[20]; snprintf(k, sizeof(k), "LoRa !%08lx", (unsigned long)from); if (dl || upd) opKto = k; }
  pmExtend();                                                // komenda w czasie czuwania POWMAN przedluza czuwanie
  if (zas) { queueLora(zasobyTxt(), 0); return; }
  if (sol) { queueLora("PICO " + cfg.picoId + ": sol hasla strony " + cfg.tokSol + " (dla skryptu: --sol " + cfg.tokSol + ")", 0); return; }
  if (str) {                                                 // strona WWW Pico (port 80: przegladarka i skrypt)
    String v = val; v.toUpperCase();
    if (!hasVal) { queueLora(String("PICO: strona WWW ") + (cfg.strona ? "wlaczona" : "wylaczona") + ". STRONA:WL wlacza, STRONA:WYL wylacza (skrypt dziala zawsze)", 0); return; }
    if (v != "WL" && v != "WYL") { queueLora("PICO: BLAD STRONA - podaj STRONA:WL albo STRONA:WYL", 0); return; }
    bool on = (v == "WL");
    if (on == cfg.strona) { queueLora(String("PICO: strona WWW juz ") + (on ? "wlaczona" : "wylaczona"), 0); return; }
    if (!on && pmFwOp()) { queueLora("PICO: BLAD STRONA - trwa operacja na firmware, sprobuj pozniej", 0); return; }
    cfg.strona = on; saveSettings();
    logf("[AUDYT] LoRa !%08lx STRONA WWW %s\n", (unsigned long)from, on ? "WLACZONA" : "WYLACZONA");
    queueLora(String("PICO: strona WWW ") + (on ? "wlaczona - " + myIp() : String("wylaczona (skrypt dziala); STRONA:WL wlacza")), 0);
    return;
  }
  if (pw) {                                                  // port wezla TCP 4403
    if (!hasVal) { queueLora("PICO: port wezla 4403 " + brTxt() + ". PORT_WEZLA:2h wlacza (m/h/d), PORT_WEZLA:0 wylacza", 0); return; }
    uint32_t sek;
    if (!brCzas(val, sek)) { queueLora("PICO: BLAD PORT_WEZLA - podaj 0 albo czas np. 30m, 2h, 1d (max 7d)", 0); return; }
    char kto[24]; snprintf(kto, sizeof(kto), "LoRa !%08lx", (unsigned long)from);
    brWlacz(sek, kto);
    queueLora(sek ? "PICO: port wezla 4403 " + brTxt() + ", tylko siec lokalna" : String("PICO: port wezla 4403 wylaczony"), 0);
    return;
  }
  if (rst) {                                                 // restart samego Pico (wezel bez zmian)
    String v = val; v.toUpperCase();
    if (!hasVal) { queueLora("PICO: potwierdz RESTART_PICO:TAK - restart Pico (wezel bez zmian)", 0); return; }
    if (v != "TAK") { queueLora("PICO: BLAD RESTART_PICO - potwierdz wpisujac RESTART_PICO:TAK", 0); return; }
    if (pmFwOp()) { queueLora("PICO: BLAD RESTART_PICO - trwa operacja na firmware, sprobuj pozniej", 0); return; }
    logf("[PICO] Restart Pico komenda LoRa od !%08lx - za 30 s\n", (unsigned long)from);
    queueLora("PICO: restart Pico za 30 s", 0);
    picoRebootAt = millis() + 30000;                       // czas na wyslanie odpowiedzi LoRa
    return;
  }
  if (dgr) {
    String v = val; v.toUpperCase();
    if (!hasVal) { queueLora(String("PICO: downgrade ") + downgradeTxt() + ". DOWNGRADE:TAK pozwala raz wgrac starsza wersje (wezla albo Pico) przez 24 h, DOWNGRADE:NIE wylacza", 0); return; }
    if (v == "TAK") {
      if (time(nullptr) < 1700000000) { queueLora("PICO: BLAD DOWNGRADE - Pico nie ma czasu z NTP", 0); return; }
      downgradeUstaw(true); logText("[PICO] Downgrade wlaczony komenda LoRa (24 h, jednorazowo)\n");
      queueLora("PICO: downgrade " + downgradeTxt() + " - jedno wgranie starszej wersji", 0); return;
    }
    if (v == "NIE") { downgradeUstaw(false); logText("[PICO] Downgrade wylaczony komenda LoRa\n"); queueLora("PICO: downgrade wylaczony", 0); return; }
    queueLora("PICO: BLAD DOWNGRADE - podaj DOWNGRADE:TAK albo DOWNGRADE:NIE", 0); return;
  }
  if (spij) {
    String v = val; v.toUpperCase();
    String opis; pmNextSleep(opis);
    if (!hasVal) { queueLora(String("PICO: potwierdz USPIJ_POWMAN:TAK - sen POWMAN, wybudzenie pinem GP14") +
                             (cfg.pmTime != "0" ? String(" + timer (") + cfg.pmTime + "), czuwanie " + cfg.pmCzuw + " min" : String(", bez timera")), 0); return; }
    if (v != "TAK") { queueLora("PICO: BLAD USPIJ_POWMAN - potwierdz wpisujac USPIJ_POWMAN:TAK", 0); return; }
    pinMode(PM_WAKE_PIN, INPUT_PULLDOWN); delay(2);
    if (digitalRead(PM_WAKE_PIN)) { queueLora("PICO: BLAD USPIJ_POWMAN - GP14 w stanie wysokim, ustaw pin wezla na 0", 0); return; }
    pmSetFlag(true); pmDeadline = 0; pmSleepAt = millis() + 60000;
    logText("[POWMAN] Tryb POWMAN wlaczony komenda LoRa - sen za 60 s\n");
    queueLora(String("PICO: zasypiam za 1 min (POWMAN), pobudka ") + opis + ", wybudzenie pinem GP14", 0);
    return;
  }
  char r[120] = "";
  if (help) {
    String l = "PICO komendy: HELP";
    for (int i = 0; i < 5; i++) l += ", " + cfg.cmd[i];
    l += ", WERSJE, ZASOBY, USPIJ_POWMAN, RESTART_PICO, DOWNGRADE, AKTUALIZUJ_INTERNET, SOL, STRONA:WL/WYL";
    queueLora(l, 0);                                         // 1/5: komendy
    queueLora("PICO ustawienia: AP_SSID:, AP_PWD:, STA_SSID:, STA_PWD:, MODE_WIFI:, TRYB_SIECI:, IPW:, IPE:, FILTR_NODE:, ADRES_URL:, BRAK_ODPOWIEDZI:, "
              "IP_DOZWOLONE:, HOP:, POBIERZ_INTERNET:typ:PIASEK", 0);   // 2/5
    queueLora(statusLine(), 0);                              // 3/5: Wi-Fi
    queueLora(String("PICO: filtr wezlow: ") + (cfg.senders.length() ? cfg.senders : String("wszyscy")) + "; BRAK_ODPOWIEDZI: " +
              (cfg.brakOdp ? String(cfg.brakOdp) + " min, pin resetu " + (cfg.rstPin >= 0 ? "GP" + String(cfg.rstPin) : String("brak"))
                           : String("wylaczone")) + "; " + pmStatus() + " - ust. POWMAN_TIME:, POWMAN_CZUWANIE:; PORT_WEZLA:", 0);   // 4/5
    String di = readFileStr(DL_INFO_PATH); di.trim();
    int p1 = di.indexOf('|'), p2 = di.indexOf('|', p1 + 1);
    String co = (p1 > 0 && p2 > p1) ? di.substring(0, p1) + " " + di.substring(p1 + 1, p2) : String("nic");
    queueLora(String("PICO internet: pobrane ") + co + ", ADRES_URL " + (cfg.url.length() ? cfg.url : String("brak")), 0);   // 5/5
    return;
  }
  if (wersje) {
    String h = readFileStr(NODE_INFO_PATH);
    auto pole = [](const String& l, const char* k) -> String {   // wartosc po "k" do spacji / ' ;'
      int p = l.indexOf(k); if (p < 0) return "";
      p += strlen(k); while (p < (int)l.length() && l[p] == ' ') p++;
      int e = p; while (e < (int)l.length() && l[e] != ' ' && l[e] != ',' && l[e] != ';') e++;
      return l.substring(p, e);
    };
    auto czas = [](const String& l) -> String {                   // "FW|2026-11-17 18:05: ..." -> data i godzina
      int p = l.indexOf('|'), e = l.indexOf(": ", p); return (p >= 0 && e > p) ? l.substring(p + 1, e) : String("?");
    };
    String fw = infoLine(h, "FW|"), ota = infoLine(h, "OTA|"), cf = infoLine(h, "CFG|");
    String tryb; { int p = fw.indexOf("tryb: "); if (p >= 0) { int e = fw.indexOf(" ;", p); tryb = fw.substring(p + 6, e > 0 ? e : fw.length()); } }
    String m1 = "PICO wersje: FW %FW% (z wezla)";
    if (fw.length()) m1 += ", wgrane przez Pico " + czas(fw) + (tryb.length() ? " (" + tryb + ")" : String(""));
    if (ota.length()) m1 += "; OTA " + (ota.indexOf("brak") >= 0 ? String("brak") : pole(ota, "ota:")) + " (" + czas(ota) + ")";
    m1 += String("; Pico ") + PICO_VERSION;
    queueLora(m1, 0, true);
    String m2 = "PICO konfiguracja: ";
    if (!cf.length()) m2 += "brak wpisu (Pico nie wgrywalo konfiguracji)";
    else if (cf.indexOf("plik ") >= 0) { String pl = cf.substring(cf.indexOf("plik ") + 5); pl.trim(); m2 += pl + ", wgrana " + czas(cf); }
    else m2 += cf.substring(cf.indexOf(": ") + 2) + " (" + czas(cf) + ")";
    m2 += " - zmiany z aplikacji nie sa tu widoczne";
    queueLora(m2, 0);
    return;
  }
  if (dl) {
    int c = val.indexOf(':');
    String typTxt = c < 0 ? val : val.substring(0, c), pias = c < 0 ? String("") : val.substring(c + 1);
    typTxt.toLowerCase(); pias.trim(); pias.toUpperCase();
    int t = typTxt == "pelna" ? 0 : typTxt == "aktualizacja" ? 1 : typTxt == "pico" ? 2 : -1;
    if (t < 0) { queueLora("PICO: BLAD POBIERZ_INTERNET - podaj pelna, aktualizacja albo pico", 0); return; }
    if (pias.length() < 4 || pias.length() > 8) { queueLora("PICO: BLAD - podaj piasek ze skryptu: POBIERZ_INTERNET:" + typTxt + ":PIASEK", 0); return; }
    dlPiasek = pias;
    queueLora("PICO: przystepuje do pobierania " + typTxt + " z internetu, prosze czekac (1-3 min) - potem raport", 0);
    if (dlReq >= 0 || pendingFlashAt) { queueLora("PICO: BLAD - poprzednie pobieranie/wgrywanie jeszcze trwa", 0); return; }
    dlReq = t;
    return;
  }
  if (upd) { updReq = true; return; }
  if (cmdIdx == 0 && pmOn) { pmSetFlag(false); pmSleepAt = pmDeadline = 0; logText("[POWMAN] Tryb POWMAN wylaczony komenda LoRa\n"); }
  if (cmdIdx == 0)      { wifiReq = 0; snprintf(r, sizeof(r), "PICO: Wi-Fi wlaczone (%s)%s", cfg.sta ? "STA" : "AP", pmOn ? "" : ", POWMAN wylaczony"); }
  else if (cmdIdx == 1) { wifiReq = 1; snprintf(r, sizeof(r), "PICO: usypiam (Wi-Fi wylaczone)"); }
  else if (cmdIdx == 2) { wifiReq = 2; snprintf(r, sizeof(r), "PICO: staly AP '%s' dla wezli", cfg.apSsid.c_str()); }
  else if (cmdIdx == 3) { wifiReq = 0; snprintf(r, sizeof(r), "PICO: koniec trybu AP, Wi-Fi wg ustawien"); }
  else if (cmdIdx == 4) { snprintf(r, sizeof(r), "PICO: temp %.1f C, zasilanie %s, uptime %lu min", analogReadTemp(), vsysTxt().c_str(), (unsigned long)(millis() / 60000)); }
  if (r[0]) { queueLora(r, 0); return; }

  // ---- zmiana ustawien ----
  String err, shown = val; bool restart = false;
  if (val.startsWith("=")) err = "wartosc nie moze zaczynac sie od '='";
  else if (setIdx == 0) {                                     // AP_SSID
    if (!val.length() || val.length() > 32) err = "nazwa 1-32 znaki";
    else { cfg.apSsid = val; restart = wifiState == 2 || (wifiState == 0 && !staActive); }
  } else if (setIdx == 1) {                                   // AP_PWD
    if (val.length() < 12 || val.length() > 63 || val == DEF_AP_PASS) err = "haslo 12-63 znaki, inne niz domyslne";
    else { cfg.apPass = val; restart = wifiState == 2 || (wifiState == 0 && !staActive); }
  } else if (setIdx == 2) {                                   // STA_SSID
    if (!val.length() || val.length() > 32) err = "nazwa 1-32 znaki";
    else { cfg.staSsid = val; staFails = 0; restart = wifiState == 0 && cfg.sta; }
  } else if (setIdx == 3) {                                   // STA_PWD (puste = siec otwarta)
    if (val.length() && (val.length() < 8 || val.length() > 63)) err = "haslo puste albo 8-63 znaki";
    else { cfg.staPass = val; staFails = 0; restart = wifiState == 0 && cfg.sta; if (!val.length()) shown = "(puste - siec otwarta)"; }
  } else if (setIdx == 4) {                                   // MODE_WIFI
    val.toUpperCase(); shown = val;
    if (val == "AP") cfg.sta = false;
    else if (val == "STA") { if (!cfg.staSsid.length()) err = "najpierw ustaw STA_SSID"; else { cfg.sta = true; staFails = 0; } }
    else err = "podaj AP albo STA";
    if (!err.length()) restart = wifiState == 0;
  } else if (setIdx == 6) {                                   // ADRES_URL
    if (!urlOk(val)) err = "podaj http://... albo https://... (max 190 znakow, bez spacji)";
    else cfg.url = val;
  } else if (setIdx == 7) {                                   // BRAK_ODPOWIEDZI (minuty, 0 = wylaczone)
    bool num = val.length() > 0; for (size_t i = 0; i < val.length(); i++) if (!isdigit(val[i])) num = false;
    int m = num ? val.toInt() : -1;
    if (m != 0 && (m < 5 || m > 1440)) err = "podaj 0 (wylaczone) albo 5-1440 minut";
    else { cfg.brakOdp = m; shown = m ? String(m) + " min" : String("0 (wylaczone)"); }
  } else if (setIdx == 8) {                                   // POWMAN_TIME
    String v = val; v.replace(" ", ""); v.toLowerCase();
    if (!pmTimeOk(v)) err = "podaj 0, co ile godzin (1-168), godziny np. 6,18 albo test np. 5m";
    else { cfg.pmTime = v; shown = v == "0" ? String("0 (bez timera)") : v; }
  } else if (setIdx == 9) {                                   // POWMAN_CZUWANIE
    bool num = val.length() > 0; for (size_t i = 0; i < val.length(); i++) if (!isdigit(val[i])) num = false;
    int m = num ? val.toInt() : -1;
    if (m < 1 || m > 240) err = "podaj 1-240 minut";
    else { cfg.pmCzuw = m; shown = String(m) + " min"; }
  } else if (setIdx == 10) {                                  // IP_DOZWOLONE (* albo - = wszystkie)
    String v = val; v.replace(" ", "");
    if (v == "*" || v == "-") v = "";
    for (char c : v) if (!isdigit(c) && c != '.' && c != ',' && c != '*') err = "cyfry, kropki, przecinki i * (np. 192.168.1.98,192.168.1.*)";
    if (v.length() > 120) err = "max 120 znakow";
    if (!err.length()) { cfg.ipAllow = v; shown = v.length() ? v : String("wszystkie"); }
  } else if (setIdx == 11 || setIdx == 12) {                 // IPW / IPE: ip/maska,brama[,dns] albo - (DHCP)
    String ip, mk, gw, dn;
    if (!ipLoraParse(val, ip, mk, gw, dn)) err = "podaj ip/maska,brama[,dns] np. 192.168.1.40/24,192.168.1.1 albo - (DHCP)";
    else {
      if (setIdx == 11) { cfg.staIp = ip; cfg.staMask = mk; cfg.staGw = gw; cfg.staDns = dn; staFails = 0; restart = wifiState == 0 && cfg.sta; }
      else { cfg.ethIp = ip; cfg.ethMask = mk; cfg.ethGw = gw; cfg.ethDns = dn; }
      shown = ip.length() ? ip + " maska " + mk + " brama " + gw + " DNS " + (dn.length() ? dn : gw) : String("DHCP");
      if (setIdx == 12 && netEth() && !pmFwOp()) { shown += ", restart Pico za 30 s"; picoRebootAt = millis() + 30000; }
      else if (setIdx == 12) shown += " (dla Ethernetu)";
    }
  } else if (setIdx == 13) {                                  // TRYB_SIECI: wifi / brak
    String v = val; v.toLowerCase(); v.trim();
    if (v != "wifi" && v != "brak" && v != "eth" && v != "oba") err = "podaj wifi, eth, oba albo brak";
    else if (v != cfg.netMode && (v == "eth" || v == "oba" || netEth())) {   // zmiana z/na Ethernet: restart Pico
      if (pmFwOp()) err = "trwa operacja na firmware, sprobuj pozniej";
      else { cfg.netMode = v; shown = v + ", restart Pico za 30 s"; picoRebootAt = millis() + 30000;
             logf("[PICO] Tryb sieci %s z LoRa - restart Pico za 30 s\n", v.c_str()); }
    } else {
      cfg.netMode = v; shown = v;
      if (v == "brak" && wifiState != 1) { wifiReq = 1; shown += ", Wi-Fi wylaczam (awaryjnie WLACZ_PICO)"; }
      if (v == "wifi" && wifiState == 1) { wifiReq = 0; shown += ", Wi-Fi wlaczam"; }
    }
  } else if (setIdx == 14) {                                  // HOP: hop limit odpowiedzi LoRa Pico
    String v = val; v.trim();
    if (v.length() != 1 || v[0] < '0' || v[0] > '7') err = "podaj 0-7";
    else { cfg.hop = v.toInt(); shown = v + " (od tej odpowiedzi)"; }
  } else if (setIdx == 5) {                                   // FILTR_NODE
    String v = val; v.toLowerCase(); v.replace(" ", ","); v.replace(";", ",");
    String out; int cnt = 0;
    if (!(v == "" || v == "*" || v == "wszyscy" || v == "all")) {
      int i = 0;
      while (i <= (int)v.length() && !err.length()) {
        int j = v.indexOf(',', i); if (j < 0) j = v.length();
        String id = v.substring(i, j); id.trim();
        if (id.startsWith("!")) id.remove(0, 1);
        if (id.startsWith("0x")) id.remove(0, 2);
        if (id.length()) {
          if (id.length() > 8 || !isHexStr(id)) err = "zly identyfikator '" + id + "' (hex, np. !ed819a48)";
          else { while (id.length() < 8) id = "0" + id; if (out.indexOf(id) < 0) { if (out.length()) out += ","; out += id; cnt++; } }
        }
        i = j + 1;
      }
      if (!err.length() && cnt > 10) err = "max 10 wezlow";
      if (!err.length() && cnt < 2) err = "podaj min. 2 wezly (zapas, gdyby id sie zmienilo) albo * = wszyscy";
    }
    if (!err.length()) {
      char me[9]; snprintf(me, sizeof(me), "%08lx", (unsigned long)from);
      String note;
      if (out.length() && out.indexOf(me) < 0) { out += String(",") + me; note = String(" (dodano nadawce !") + me + ")"; }
      cfg.senders = out; shown = (out.length() ? out : String("wszyscy")) + note;
    }
  }
  if (err.length()) { queueLora(String("PICO: BLAD ") + key + " - " + err + ", bez zmian", 0); return; }
  if (!saveSettings()) { queueLora("PICO: BLAD zapisu ustawien, bez zmian", 0); loadSettings(); return; }
  if ((setIdx <= 3 || setIdx == 6) && val.length()) shown = "\"" + val + "\"";       // w cudzyslowie - widac spacje na brzegach
  queueLora(String("PICO: ustawiono ") + key + " = " + shown + (restart ? ", restart Wi-Fi" : ""), 0);
  if (restart) wifiReq = 3;
}
// Ochrona przed wstrzyknieciem: tekst wiadomosci LoRa (z dowolnego kanalu) moze zawierac znaki nowej linii
// i podrobione linie logu. Prawdziwy kolejny pakiet przychodzi najwczesniej po czasie nadawania LoRa (>= 30 ms),
// a podrobione linie - w tej samej chwili co wiadomosc, w ktorej siedza. Takie linie sa ignorowane.
static uint32_t txtMsgMs = 0;
void parseCmdLine(const char* l) {
  // naglowek pakietu: "handleReceived(REMOTE)" albo "decoded message (" (wezel nie zawsze wypisuje obie linie)
  if ((strstr(l, "handleReceived(REMOTE)") || strstr(l, "] decoded message (")) && strstr(l, "Portnum=1 ")) {
    if (txtMsgMs && millis() - txtMsgMs < 30) { txtId = 0; txtCh = -1; return; }   // linia zaraz po tresci wiadomosci
    const char* c = strstr(l, " Ch=");
    txtId = hexAfter(l, "id=0x"); txtFrom = hexAfter(l, "fr=0x"); txtCh = c ? atoi(c + 4) : -1;
    return;
  }
  const char* m = strstr(l, "Received text msg from=0x");
  if (!m) return;
  bool zaSzybko = txtMsgMs && millis() - txtMsgMs < 30;
  txtMsgMs = millis(); if (!txtMsgMs) txtMsgMs = 1;
  uint32_t id = hexAfter(m, "id=0x"), from = hexAfter(m, "from=0x");
  const char* t = strstr(m, "msg=");
  if (!t || zaSzybko || !id || id != txtId || from != txtFrom || txtCh != cfg.ch) return;   // inny kanal, nie ta wiadomosc albo podrobka
  txtId = 0;                                                    // jedna linia na pakiet
  if (!chSecure(true)) return;                                  // kanal komend musi byc prywatny, klucz 256-bit
  char m2[201]; size_t n = 0; t += 4;
  while (*t && *t != '\r' && *t != '\n' && n < sizeof(m2) - 1) m2[n++] = *t++;
  m2[n] = 0;
  loraCmd(m2, from, id);
}

// Tekst z ESP32-S3 do logu (bez kodow kolorow ANSI i znakow \r) + wykrywanie "Use nodenum 0x"
void heltecText(const uint8_t* in, int n) {
  static uint8_t escState = 0;
  static char line[320]; static uint16_t lineLen = 0;
  uint8_t out[256]; int o = 0;
  for (int i = 0; i < n; i++) {
    uint8_t c = in[i];
    if (escState == 1) { escState = (c == '[') ? 2 : 0; continue; }
    if (escState == 2) { if (c >= 0x40 && c <= 0x7E) escState = 0; continue; }
    if (c == 0x1B) { escState = 1; continue; }
    if (c == '\r') continue;
    if (c < 0x20 && c != '\n' && c != '\t') continue;   // binarne smieci (np. ramki API) - bez NUL w logu
    out[o++] = c;
    if (c == '\n') {
      nodeLines++;
      line[lineLen] = 0;
      const char* p = strstr(line, "nodenum 0x");
      if (p) { uint32_t v = strtoul(p + 10, nullptr, 16); if (v) logNodeNum = v; }
      parseCmdLine(line);
      if (!strncmp(line, "ESP-ROM:", 8) || strstr(line, "E S H T") || (strstr(line, "Booted, wake cause") && !(bootStan == 1 && millis() - bootStartMs < 10000))) {
        if (bootStan != 1 || millis() - bootStartMs > 3000) {   // nowy start wezla (ESP-ROM i Booted to ten sam start)
          mutex_enter_blocking(&logMtx);
          char t[24]; nowStr(t, sizeof(t));
          bootLen = snprintf(bootBuf, BOOT_SIZE, "===== start wezla %s | Pico uptime %lus =====\n", t, (unsigned long)(millis() / 1000));
          bootStan = 1; bootStartMs = millis();
          memcpy(bootBuf + bootLen, line, lineLen); bootLen += lineLen; bootBuf[bootLen++] = '\n';
          mutex_exit(&logMtx);
        }
      }
      { const char* q = strstr(line, "| ");               // godzina z prefiksu logu wezla, gdy wezel zna czas
        if (q && q - line < 8 && isdigit(q[2]) && isdigit(q[3]) && q[4] == ':' && isdigit(q[5]) && q[7] == ':') {
          nodeTodSec = atoi(q + 2) * 3600UL + atoi(q + 5) * 60UL + atoi(q + 8); nodeTodMs = millis(); nodeTimeOk = true; } }
      lineLen = 0;
    } else if (lineLen < sizeof(line) - 1) line[lineLen++] = c;
  }
  if (o && (cfg.logOn || fwState == 1)) logAppend((const char*)out, o);   // w trakcie flashowania zawsze
  else if (o && bootStan == 1) { mutex_enter_blocking(&logMtx); bootAppend((const char*)out, o); mutex_exit(&logMtx); }
}

// W trybie API: ramki 0x94 0xC3 len_hi len_lo + FromRadio; reszta to tekst
// Ramki 0x94 0xC3 len_hi len_lo + FromRadio; wszystko inne to tekst logu. Dziala zawsze:
// po pierwszym uzyciu API wezel do restartu wysyla logi tylko jako LogRecord (gdy wlaczone debug_log_api_enabled)
// i przekazuje odebrane wiadomosci jako pakiety - z nich tez czytamy komendy.
void apiFeed(const uint8_t* in, int n) {
  static uint8_t st = 0; static uint16_t need = 0, got = 0;
  static uint8_t fr[520];
  uint8_t txt[256]; int tn = 0;
  auto flush = [&]() { if (tn) { heltecText(txt, tn); tn = 0; } };
  for (int i = 0; i < n; i++) {
    uint8_t b = in[i];
    switch (st) {
      case 0: if (b == 0x94) st = 1; else { txt[tn++] = b; if (tn == sizeof(txt)) flush(); } break;
      case 1: if (b == 0xC3) { flush(); st = 2; }
              else { txt[tn++] = 0x94; if (tn == sizeof(txt)) flush(); txt[tn++] = b; if (tn == sizeof(txt)) flush(); st = 0; } break;
      case 2: need = (uint16_t)b << 8; st = 3; break;
      case 3: need |= b; got = 0; st = (need == 0 || need > sizeof(fr)) ? 0 : 4; break;
      case 4: fr[got++] = b; if (got >= need) { apiFrame(fr, need); st = 0; } break;
    }
  }
  flush();
}

void readHeltec() {
  uint8_t in[256];
  int n;
  while ((n = SerialHost.available()) > 0) {
    if (n > (int)sizeof(in)) n = sizeof(in);
    if (brActive) {                                   // port wezla: nie czytaj wiecej niz zmiesci bufor (reszta czeka w USB)
      uint32_t f = brDown.wolne(); if (!f) break; if ((uint32_t)n > f) n = f;
    }
    n = SerialHost.read(in, n);
    if (n <= 0) break;
    totalBytes += n;
    lastRxMs = millis();
    if (brActive) { uint32_t w = brDown.put(in, n); if (w < (uint32_t)n) brLost += n - w; }
    apiFeed(in, n);
  }
}

// ---- SHA-256 i HMAC-SHA256 (podpis pliku mtnod.txt) ----
static const uint32_t SHA_K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
  0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
  0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
  0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
static inline uint32_t shaRor(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void shaBlock(Sha256& s, const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = shaRor(w[i - 15], 7) ^ shaRor(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = shaRor(w[i - 2], 17) ^ shaRor(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3], e = s.h[4], f = s.h[5], g = s.h[6], h = s.h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = h + (shaRor(e, 6) ^ shaRor(e, 11) ^ shaRor(e, 25)) + ((e & f) ^ (~e & g)) + SHA_K[i] + w[i];
    uint32_t t2 = (shaRor(a, 2) ^ shaRor(a, 13) ^ shaRor(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  s.h[0] += a; s.h[1] += b; s.h[2] += c; s.h[3] += d; s.h[4] += e; s.h[5] += f; s.h[6] += g; s.h[7] += h;
}
static void shaInit(Sha256& s) {
  static const uint32_t H0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
  memcpy(s.h, H0, sizeof(H0)); s.len = 0; s.n = 0;
}
static void shaUpd(Sha256& s, const uint8_t* d, size_t n) {
  while (n--) { s.buf[s.n++] = *d++; s.len++; if (s.n == 64) { shaBlock(s, s.buf); s.n = 0; } }
}
static void shaFin(Sha256& s, uint8_t out[32]) {
  uint64_t bits = s.len * 8; uint8_t b = 0x80; shaUpd(s, &b, 1);
  b = 0; while (s.n != 56) shaUpd(s, &b, 1);
  uint8_t L[8]; for (int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - 8 * i));
  shaUpd(s, L, 8);
  for (int i = 0; i < 8; i++) { out[4 * i] = s.h[i] >> 24; out[4 * i + 1] = s.h[i] >> 16; out[4 * i + 2] = s.h[i] >> 8; out[4 * i + 3] = s.h[i]; }
}
static void hmacSha256(const char* key, size_t kl, const char* msg, size_t ml, uint8_t out[32]) {
  uint8_t k[64] = { 0 }, ip[64], op[64], ih[32]; Sha256 s;
  if (kl > 64) { shaInit(s); shaUpd(s, (const uint8_t*)key, kl); shaFin(s, k); } else memcpy(k, key, kl);
  for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
  shaInit(s); shaUpd(s, ip, 64); shaUpd(s, (const uint8_t*)msg, ml); shaFin(s, ih);
  shaInit(s); shaUpd(s, op, 64); shaUpd(s, ih, 32); shaFin(s, out);
}

String hexStr(const uint8_t* b, size_t n) {
  static const char* H = "0123456789abcdef"; String s; s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) { s += H[b[i] >> 4]; s += H[b[i] & 15]; }
  return s;
}
bool hexBytes(const String& h, uint8_t* out, size_t n) {
  if (h.length() != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    char a = h[2 * i], c = h[2 * i + 1];
    if (!isxdigit(a) || !isxdigit(c)) return false;
    auto v = [](char x) { return (uint8_t)(isdigit(x) ? x - '0' : (tolower(x) - 'a' + 10)); };
    out[i] = (v(a) << 4) | v(c);
  }
  return true;
}
// PBKDF2-HMAC-SHA256, 10000 powtorzen, 32 bajty - skrot hasla strony (Pico nie przechowuje hasla)
void pbkdf2(const char* pw, size_t pl, const uint8_t* salt, size_t sl, uint8_t out[32]) {
  uint8_t msg[36], u[32], t[32];
  if (sl > 32) sl = 32;
  memcpy(msg, salt, sl); msg[sl] = 0; msg[sl + 1] = 0; msg[sl + 2] = 0; msg[sl + 3] = 1;
  hmacSha256(pw, pl, (const char*)msg, sl + 4, u); memcpy(t, u, 32);
  for (uint32_t i = 1; i < 10000; i++) {
    hmacSha256(pw, pl, (const char*)u, 32, u);
    for (int j = 0; j < 32; j++) t[j] ^= u[j];
    if ((i & 511) == 0) rp2040.wdt_reset();
  }
  memcpy(out, t, 32);
}
void picoIdNowy() {                                  // identyfikator Pico: skrot sprzetowego numeru seryjnego (staly, takze po resecie)
  pico_unique_board_id_t u; pico_get_unique_board_id(&u);
  uint8_t d[32]; Sha256 s; shaInit(s); shaUpd(s, (const uint8_t*)"PicoID:", 7); shaUpd(s, u.id, sizeof(u.id)); shaFin(s, d);
  cfg.picoId = hexStr(d, 6);
}
void tokUstaw(const String& haslo) {                 // nowa sol + skrot (przy pierwszym starcie / migracji)
  uint8_t sb[16], hb[32];
  for (int i = 0; i < 16; i += 4) { uint32_t x = rp2040.hwrand32(); memcpy(sb + i, &x, 4); }
  pbkdf2(haslo.c_str(), haslo.length(), sb, 16, hb);
  cfg.tokSol = hexStr(sb, 16); cfg.tokHash = hexStr(hb, 32);
  cfg.tokSlabe = haslo.length() < 12 || haslo == DEF_TOKEN;
}

// ---- Aktualizacja przez internet: plik mtnod.txt (podpisany tokenem) i pliki obok niego ----
// Linia: typ|wersja|plik|rozmiar|crc32 ; podpis=HMAC-SHA256(token, "typ|wersja|rozmiar|crc\n" ... ) w kolejnosci z pliku
static const char* const DL_TYPES[3] = { "pelna", "aktualizacja", "pico" };
static uint8_t dlBuf[2048];
static String urlHost(const String& u) {
  int b = u.indexOf("://"); b = b < 0 ? 0 : b + 3;
  int e = b; while (e < (int)u.length() && u[e] != '/' && u[e] != ':') e++;
  return u.substring(b, e);
}
static bool httpOpen(HTTPClient& http, const String& url, String& err) {
  String host = urlHost(url), plik = url.substring(url.lastIndexOf('/') + 1);
  IPAddress ip;
  if (!host.length() || !WiFi.hostByName(host.c_str(), ip)) { err = "DNS: nie znaleziono serwera " + host + " (adres albo brak internetu)"; return false; }
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (url.startsWith("https://")) http.setInsecure();      // szyfrowanie bez sprawdzania certyfikatu - chroni podpis
  if (!http.begin(url)) { err = "zly adres"; return false; }
  int code = http.GET();
  if (code == HTTPC_ERROR_CONNECTION_FAILED)
    err = url.startsWith("https://") ? "brak polaczenia z " + host + " (port 443) albo serwer nie obsluguje TLS 1.2 - sprobuj http://"
                                     : "serwer " + host + " nie odpowiada (port 80)";
  else if (code == HTTPC_ERROR_READ_TIMEOUT) err = "serwer " + host + " nie odpowiedzial w 20 s";
  else if (code < 0) err = String("polaczenie z ") + host + " przerwane (blad " + code + ")";
  else if (code == 404) err = "brak pliku " + plik + " na serwerze (HTTP 404)";
  else if (code == 401 || code == 403) err = "brak dostepu do " + plik + " (HTTP " + code + ")";
  else if (code != 200) err = String("HTTP ") + code + " " + plik;
  if (code != 200) { http.end(); return false; }
  return true;
}
static bool netFail(const String& err) {
  logf("[PICO] Internet: BLAD - %s\n", err.c_str());
  queueLora(String("PICO: BLAD pobierania - ") + err, 0);
  return false;
}
bool netDownload(int typ) {
  if (!cfg.url.length()) return netFail("brak ADRES_URL");
  if (domyslneHasla()) return netFail("domyslne lub za krotkie haslo strony (token) - podpis mtnod.txt bylby do podrobienia; zmien je na stronie");
  if ((wifiState != 0 || !staActive) && !ethUp()) return netFail("Pico nie jest w sieci STA ani Ethernet (wymagany internet)");
  if (fwState == 1 || upActive) return netFail("trwa inna operacja");
  logf("[PICO] Internet: pobieram %s\n", cfg.url.c_str());
  String man, err;
  { HTTPClient http; if (!httpOpen(http, cfg.url, err)) return netFail(err);
    int sz = http.getSize();
    if (sz > 4096) { http.end(); return netFail("mtnod.txt za duzy (" + String(sz) + " B) - to nie plik ze skryptu"); }
    WiFiClient* st = http.getStreamPtr(); uint32_t t0 = millis();
    while ((sz < 0 || (int)man.length() < sz) && man.length() < 4096 && millis() - t0 < 15000) {
      if (st->available()) man += (char)st->read(); else if (!http.connected()) break; else delay(2);
    }
    http.end();
    if (man.length() >= 4096) return netFail("mtnod.txt za duzy - to nie plik ze skryptu"); }
  rp2040.wdt_reset();
  // parsowanie i podpis
  String signedTxt, sig, plik, wer, skr, czasTxt, shaTxt; uint32_t size = 0, crc = 0; String crcTxt; bool found = false;
  int pos = 0;
  while (pos < (int)man.length()) {
    int e = man.indexOf('\n', pos); if (e < 0) e = man.length();
    String l = man.substring(pos, e); l.trim(); pos = e + 1;
    if (!l.length() || l[0] == '#') continue;
    if (l.startsWith("podpis=")) { sig = l.substring(7); sig.trim(); sig.toLowerCase(); continue; }
    if (l.startsWith("czas="))   { czasTxt = l.substring(5); czasTxt.trim(); continue; }
    String f[7]; int k = 0, st = 0;                         // typ|wersja|plik|rozmiar|crc[|wersja skryptu[|sha256]]
    for (; k < 7; k++) { int b = l.indexOf('|', st); if (k == 6) b = -1; f[k] = b < 0 ? l.substring(st) : l.substring(st, b); f[k].trim(); if (b < 0) { k++; break; } st = b + 1; }
    if (k < 5 || k > 7) continue;
    signedTxt += f[0] + "|" + f[1] + "|" + f[3] + "|" + f[4] + (k >= 6 ? "|" + f[5] : String("")) + (k == 7 ? "|" + f[6] : String("")) + "\n";
    if (f[0] == DL_TYPES[typ]) { found = true; wer = f[1]; plik = f[2]; size = strtoul(f[3].c_str(), nullptr, 10); crcTxt = f[4]; crc = strtoul(f[4].c_str(), nullptr, 16); skr = k >= 6 ? f[5] : String(""); shaTxt = k == 7 ? f[6] : String(""); shaTxt.toLowerCase(); }
  }
  // od 0.29: podpis kluczem token + ":" + piasek, obejmuje tez znacznik czasu przygotowania plikow
  if (!czasTxt.length()) return netFail("mtnod.txt bez znacznika czasu - przygotuj pliki skryptem " + String(PICO_VERSION));
  uint32_t czas = strtoul(czasTxt.c_str(), nullptr, 10);
  signedTxt += "czas=" + czasTxt + "\n";
  String klucz = cfg.tokHash + ":" + dlPiasek;          // od 0.40: skrot hasla (PBKDF2) zamiast hasla
  uint8_t mac[32]; char hex[65];
  hmacSha256(klucz.c_str(), klucz.length(), signedTxt.c_str(), signedTxt.length(), mac);
  for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", mac[i]);
  if (!ctEq(sig, String(hex))) return netFail("zly podpis mtnod.txt - zly piasek albo inny token niz w Pico");
  if (czas <= cfg.mtCzas) return netFail("zestaw plikow nie nowszy niz ostatnio przyjety (powtorka?) - odrzucony; przygotuj serwer ponownie");
  if (!found) return netFail(String("brak wpisu '") + DL_TYPES[typ] + "' w mtnod.txt");
  if (typ != 2 && skr != PICO_VERSION)                      // pakiet wezla musi pochodzic ze skryptu w tej samej wersji
    return netFail(String("pakiet ze skryptu ") + (skr.length() ? skr : String("<0.24")) + ", Pico ma " + PICO_VERSION +
                   " - przygotuj pliki skryptem " + PICO_VERSION);
  if (shaTxt.length() != 64) return netFail("mtnod.txt bez sumy SHA-256 - przygotuj pliki skryptem " + String(PICO_VERSION));
  if (size < 1024 || size > 3000000) return netFail("nietypowy rozmiar pliku");
  String url = plik.startsWith("http://") || plik.startsWith("https://") ? plik : cfg.url.substring(0, cfg.url.lastIndexOf('/') + 1) + plik;
  // miejsce: usun to, co zostanie zastapione; jesli dalej za malo - takze drugi rodzaj pliku
  const char* dest = typ == 2 ? PICO_DL_PATH : PKG_PATH;
  LittleFS.remove(DL_INFO_PATH);
  if (typ == 2) LittleFS.remove(PICO_DL_PATH);
  else { LittleFS.remove(PKG_OK_PATH); LittleFS.remove(PKG_PATH); LittleFS.remove(PKG_INFO_PATH); LittleFS.remove(ETAP_PATH); loadPkgMode(); }
  FSInfo fi; LittleFS.info(fi);
  if (size + 65536 > fi.totalBytes - fi.usedBytes) {
    if (typ == 2) { LittleFS.remove(PKG_OK_PATH); LittleFS.remove(PKG_PATH); LittleFS.remove(PKG_INFO_PATH); loadPkgMode(); }
    else LittleFS.remove(PICO_DL_PATH);
    LittleFS.info(fi);
    if (size + 65536 > fi.totalBytes - fi.usedBytes)
      return netFail(String("brak miejsca: ") + size / 1024 + " KB, wolne " + (uint32_t)((fi.totalBytes - fi.usedBytes) / 1024) + " KB");
  }
  // pobieranie z liczeniem CRC
  logf("[PICO] Internet: %s %s, %lu KB\n", DL_TYPES[typ], wer.c_str(), (unsigned long)(size / 1024));
  uint32_t got = 0, c = 0xFFFFFFFF, t = millis(), t0 = millis();
  Sha256 dsha; shaInit(dsha);                              // SHA-256 pobieranego pliku (CRC32 da sie podrobic)
  LittleFS.remove(CFGX_KEY_PATH);
  {
    HTTPClient http; if (!httpOpen(http, url, err)) return netFail(err);
    WiFiClient* st = http.getStreamPtr();
    File f = LittleFS.open(dest, "w");
    if (!f) { http.end(); return netFail("nie mozna utworzyc pliku"); }
    int lastPct = -1;
    while (got < size && millis() - t < 20000) {
      rp2040.wdt_reset();
      size_t av = st->available();
      if (!av) { if (!http.connected()) break; delay(2); continue; }
      size_t n = st->readBytes(dlBuf, av < sizeof(dlBuf) ? av : sizeof(dlBuf));
      if (!n) continue;
      if (got + n > size) { got = size + 1; break; }
      if (f.write(dlBuf, n) != n) { f.close(); http.end(); LittleFS.remove(dest); return netFail("blad zapisu do pamieci Pico"); }
      c = crc32Calc(c, dlBuf, n); shaUpd(dsha, dlBuf, n); got += n; t = millis();
      int pct = (int)((uint64_t)got * 10 / size);
      if (pct != lastPct) { lastPct = pct; logf("[PICO] Internet: %lu/%lu KB\n", (unsigned long)(got / 1024), (unsigned long)(size / 1024)); }
    }
    f.close(); http.end();
  }
  if (got != size || (~c) != crc) { LittleFS.remove(dest); return netFail(String("plik niezgodny (") + got + "/" + size + " B, CRC) - usuniety"); }
  { uint8_t d[32]; shaFin(dsha, d);
    if (!ctEq(hexStr(d, 32), shaTxt)) { LittleFS.remove(dest); return netFail("suma SHA-256 pliku niezgodna z podpisanym mtnod.txt - plik usuniety"); } }
  // ponowny odczyt z pamieci Pico
  c = 0xFFFFFFFF; got = 0;
  { File f = LittleFS.open(dest, "r"); size_t n; while (f && (n = f.read(dlBuf, sizeof(dlBuf))) > 0) { c = crc32Calc(c, dlBuf, n); got += n; rp2040.wdt_reset(); } if (f) f.close(); }
  if (got != size || (~c) != crc) { LittleFS.remove(dest); return netFail("kontrola po zapisie niezgodna - plik usuniety"); }
  if (typ != 2) {                                       // klucz do zaszyfrowanej konfiguracji (jesli jest) + kontrola
    uint8_t hb[32], ks[32], km[32];
    if (hexBytes(cfg.tokHash, hb, 32)) {
      String a = "szyfr:" + dlPiasek, b2 = "mac:" + dlPiasek;
      hmacSha256((const char*)hb, 32, a.c_str(), a.length(), ks); hmacSha256((const char*)hb, 32, b2.c_str(), b2.length(), km);
      File kf = LittleFS.open(CFGX_KEY_PATH, "w"); if (kf) { kf.print(hexStr(ks, 32) + hexStr(km, 32)); kf.close(); }
    }
    File pf = LittleFS.open(dest, "r"); PkgHdr ph; bool zasz = false; uint32_t zp = 0, zl = 0;
    if (pf && pf.read((uint8_t*)&ph, sizeof(ph)) == sizeof(ph) && ph.count <= 8)
      for (int i = 0; i < ph.count; i++) { PkgEnt pe; if (pf.read((uint8_t*)&pe, sizeof(pe)) == sizeof(pe) && pe.offset == CFGX_OFFSET) { zasz = true; zp = pe.dataPos; zl = pe.csize; } }
    if (zasz) {
      String e2;
      bool ok = cfgxOdszyfruj(pf, zp, zl, e2);
      LittleFS.remove(CFGX_DEC_PATH);
      if (!ok) { pf.close(); LittleFS.remove(dest); LittleFS.remove(CFGX_KEY_PATH); return netFail("konfiguracja w pakiecie: " + e2 + " - plik usuniety"); }
      logText("[PICO] Internet: konfiguracja w pakiecie zaszyfrowana - klucz z hasla i piasku OK\n");
    } else LittleFS.remove(CFGX_KEY_PATH);
    if (pf) pf.close();
    File m = LittleFS.open(PKG_OK_PATH, "w"); if (m) { m.print(crc, HEX); m.close(); }
    savePkgInfo(); loadPkgMode();
  }
  { File m = LittleFS.open(DL_INFO_PATH, "w"); if (m) { m.printf("%s|%s|%lu|%s", DL_TYPES[typ], wer.c_str(), (unsigned long)size, crcTxt.c_str()); m.close(); } }
  if (czas > cfg.mtCzas) { cfg.mtCzas = czas; saveSettings(); }   // starsze zestawy beda odrzucane
  logf("[PICO] Internet: pobrano %s %s (%lu B, %lu s), suma OK\n", DL_TYPES[typ], wer.c_str(), (unsigned long)size, (unsigned long)((millis() - t0) / 1000));
  queueLora(String("PICO: pobrano ") + DL_TYPES[typ] + " " + wer + " (" + size / 1024 + " KB), suma OK. Wgranie: AKTUALIZUJ_INTERNET", 0);
  return true;
}
void netUpdate() {
  String di = readFileStr(DL_INFO_PATH); di.trim();
  int p1 = di.indexOf('|'), p2 = di.indexOf('|', p1 + 1), p3 = di.indexOf('|', p2 + 1);
  if (p1 < 0 || p2 < 0 || p3 < 0) { queueLora("PICO: BLAD - nic nie pobrano (najpierw POBIERZ_INTERNET:...)", 0); return; }
  String typ = di.substring(0, p1), wer = di.substring(p1 + 1, p2);
  uint32_t size = strtoul(di.substring(p2 + 1, p3).c_str(), nullptr, 10), crc = strtoul(di.substring(p3 + 1).c_str(), nullptr, 16);
  if (fwState == 1 || pendingFlashAt) { queueLora("PICO: BLAD - trwa inna operacja", 0); return; }
  if (typ == "pico") {
    uint32_t c = 0xFFFFFFFF, got = 0;
    { File f = LittleFS.open(PICO_DL_PATH, "r"); size_t n; while (f && (n = f.read(dlBuf, sizeof(dlBuf))) > 0) { c = crc32Calc(c, dlBuf, n); got += n; rp2040.wdt_reset(); } if (f) f.close(); }
    if (got != size || (~c) != crc) { LittleFS.remove(PICO_DL_PATH); LittleFS.remove(DL_INFO_PATH); queueLora("PICO: BLAD - program Pico uszkodzony w pamieci, usuniety", 0); return; }
    if (!picoBinOk(PICO_DL_PATH)) { LittleFS.remove(PICO_DL_PATH); LittleFS.remove(DL_INFO_PATH);
      logText("[PICO] Odrzucono pobrany program Pico - brak znacznika PICOVER (to nie Pico2W flasher)\n");
      queueLora("PICO: BLAD - pobrany plik to nie program Pico2W flasher, odrzucony", 0); return; }
    { String err;
      if (!wersjaDozwolona(picoBinVer(PICO_DL_PATH), PICO_VERSION, "Program Pico", err)) {
        LittleFS.remove(PICO_DL_PATH); LittleFS.remove(DL_INFO_PATH); queueLora("PICO: BLAD - " + err, 0); return; } }
    LittleFS.remove(PICO_BIN); LittleFS.remove(CFG_PENDING_PATH);
    if (!LittleFS.rename(PICO_DL_PATH, PICO_BIN)) { queueLora("PICO: BLAD - nie mozna przygotowac aktualizacji Pico", 0); return; }
    LittleFS.remove(DL_INFO_PATH);
    { File m = LittleFS.open(PICO_LORA_FLAG, "w"); if (m) { m.print(PICO_VERSION); m.close(); } }
    picoOTA.begin(); picoOTA.addFile(PICO_BIN); picoOTA.commit();
    logf("[PICO] Aktualizacja Pico %s z internetu - restart za 25 s\n", wer.c_str());
    queueLora(String("PICO: wgrywam Pico ") + wer + " i restartuje sie, potwierdzenie ok. 1-2 min po starcie", 0);
    picoRebootAt = millis() + 25000;                       // czas na wyslanie odpowiedzi LoRa
    return;
  }
  if (!LittleFS.exists(PKG_OK_PATH)) { queueLora("PICO: BLAD - brak pobranego pakietu", 0); return; }
  { String ok = readFileStr(PKG_OK_PATH); ok.trim();
    if (strtoul(ok.c_str(), nullptr, 16) != crc) { queueLora("PICO: BLAD - pakiet na Pico jest inny niz pobrany, pobierz ponownie", 0); return; } }
  if (!heltecMounted) { queueLora("PICO: BLAD - wezel nie jest podlaczony do USB", 0); return; }
  queueLora(String("PICO: wgrywam ") + typ + " " + wer + " do wezla, raport po zakonczeniu" +
            (typ == "pelna" ? " (ok. 3-5 min, wezel w tym czasie milczy)" : " (ok. 1 min)"), 0);
  pendingFlashAt = millis() + 8000;
}
// Rdzen 0: obsluga komend internetowych i opoznionego startu wgrywania
void netService() {
  if (dlReq >= 0) { int t = dlReq; netDownload(t); dlReq = -1; }
  if (updReq) { updReq = false; netUpdate(); }
  if (pendingFlashAt && (int32_t)(millis() - pendingFlashAt) >= 0 && (loraQn == 0 || millis() - pendingFlashAt > 30000)) {
    pendingFlashAt = 0;
    if (fwState == 1 || !heltecMounted) { queueLora("PICO: BLAD - wgrywanie nie ruszylo (wezel odlaczony albo inna operacja)", 0); return; }
    char now[24]; nowStr(now, sizeof(now));
    strncpy(opTime, now, sizeof(opTime) - 1);
    fwState = 1; fwPct = 0; strcpy(fwMsg, "zlecone (z internetu)");
    reqCmd = 6;
    logText("[PICO] Internet: start wgrywania pobranego pakietu\n");
  }
}
