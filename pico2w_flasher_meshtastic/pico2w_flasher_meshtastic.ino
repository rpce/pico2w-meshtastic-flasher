// =========================================================================
// Pomyslodawca projektu: Rafal Cichosz (rpc)
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
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <DNSServer.h>
#include <LEAmDNS.h>
#include <PicoOTA.h>
#include <HTTPClient.h>

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
struct Settings { String apSsid, apPass, token, staSsid, staPass, senders, url; bool sta; int ch; bool logOn; String cmd[5]; } cfg;
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
static LoraMsg loraQ[8]; static volatile uint8_t loraQn = 0;
auto_init_mutex(loraQMtx);
// Komenda odebrana na rdzeniu 1 (tekst calej wiadomosci), wykonywana na rdzeniu 0 (ustawienia, Wi-Fi)
static char cmdBuf[201]; static volatile uint32_t cmdFrom = 0; static volatile bool cmdPending = false;
static char nodeLong[41] = "", nodeShort[16] = "", nodeFw[40] = "";   // nazwa i wersja firmware wezla (API)  // nazwa wlasnego wezla z node_info (API)
static const uint32_t BOOT_REPORT_MS = 120000;     // raport o wlaczeniu zasilania: 2 min po polaczeniu z wezlem
static volatile bool bootReportWanted = false;     // start Pico z wlaczenia zasilania
static volatile bool resetNodeAfterUpdate = false; // po aktualizacji programu Pico: jednorazowy reset wezla (RTS)
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
// reqCmd: 1 reset RTS, 2 bootloader, 4 restart hosta, 5 test bootloadera, 6 flashowanie
static volatile uint8_t  reqCmd = 0;
static volatile uint32_t core1Beat = 0;

// Stan flashowania (zapisywany przez rdzeń 1, czytany przez stronę)
// fwState: 0 brak, 1 trwa, 2 OK, 3 błąd
static volatile uint8_t fwState = 0;
static volatile int     fwPct = 0;
static char             fwMsg[96] = "brak";
static bool             upActive = false;   // trwa przyjmowanie pakietu firmware
static uint32_t upTotal = 0, upTotalCrc = 0, upNext = 0;

// Tryb API Meshtastic (ramki 0x94 0xC3) - tylko na czas wysylania konfiguracji
static const uint32_t   CFG_OFFSET = 0xFFFFFFFF;   // wpis pakietu z konfiguracja (nie flashowany)
static const uint32_t   INFO_OFFSET = 0xFFFFFFFE;  // wpis z opisem pakietu (nie flashowany)
const char* PKG_INFO_PATH  = "/pkg.info";          // opis pakietu zapisanego na Pico
const char* NODE_INFO_PATH = "/node.info";         // co i kiedy wgrano do wezla
static char opTime[24] = "";
static String pkgMode = "";                        // tryb pakietu z opisu ("aktualizacja (sam program)", "pelna instalacja...")                       // czas zlecenia (z komputera / przegladarki)
static volatile uint32_t ackId = 0;                // potwierdzenie komendy (Routing.request_id)
static volatile uint8_t  ackErr = 0;
static const uint8_t API_DISC[] = { 0x94, 0xC3, 0x00, 0x02, 0x20, 0x01 };   // ToRadio.disconnect
static volatile bool     apiMode = false;
static volatile uint32_t apiNodeNum = 0;    // z my_info (FromRadio)
static volatile bool     apiConfigDone = false;
static volatile uint32_t apiFrames = 0;
static volatile uint32_t logNodeNum = 0;    // z linii logu "Use nodenum 0x..."
static volatile uint32_t lastRxMs = 0;
static volatile uint32_t lastApiUse = 0;           // ostatnie polaczenie API z wezlem
static volatile bool     fsReady = false;
const char* CFG_PENDING_PATH = "/cfg.pending";     // konfiguracja do wyslania po restarcie Pico
static const uint32_t CFG_BOOT_WAIT_MS = 60000;    // pierwszy start wezla: klucze, zapis plikow, GPS, BT
static const uint32_t CFG_RETRY_WAIT_MS = 30000;   // przed ponowieniem konfiguracji
static const uint32_t ACK_TIMEOUT_MS = 8000;       // np. zmiana regionu LoRa przestawia radio - trwa
#define PICO_VER "0.24"
const char PICO_VER_TAG[] = "PICOVER=" PICO_VER;    // znacznik w pliku .bin - skrypt odczytuje z niego wersje
const char* PICO_VERSION = PICO_VER_TAG + 8;
static volatile uint32_t picoRebootAt = 0;         // zaplanowany restart Pico (przycisk / skrypt)

// Slad ostatniego kroku w rejestrze watchdoga (przetrwa restart z watchdoga)
static const uint32_t CRUMB_MAGIC = 0x50494331;
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
auto_init_mutex(logMtx);

static volatile uint32_t logTotal = 0;      // ile bajtow trafilo do logu od startu (do przyrostowego podgladu)
void logAppend(const char* data, size_t len) {
  mutex_enter_blocking(&logMtx);
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
  char m[192];
  va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof(m), fmt, ap); va_end(ap);
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

void restartUsbHost() {
  logText("\n[PICO] Restart hosta USB...\n");
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
  ESP_FLASH_BEGIN = 0x02, ESP_FLASH_END = 0x04, ESP_SYNC = 0x08, ESP_READ_REG = 0x0A,
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
    if (wt == 2 && (fld == 2 || fld == 3 || fld == 4 || fld == 6 || fld == 13 || fld == 16)) {
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
            if (port == 1 && txt && txtLen && pFrom && (int)pCh == cfg.ch) {
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
      } else if (fld == 6) {                                   // log_record.message -> jak zwykly log tekstowy
        while (q < qe) {
          uint64_t t2; if (!pbVarint(q, qe, t2)) break;
          if ((t2 >> 3) == 1 && (t2 & 7) == 2) {
            uint64_t L2; if (!pbVarint(q, qe, L2) || L2 > (uint64_t)(qe - q)) break;
            heltecText(q, (int)L2);
            static const uint8_t nl = '\n'; heltecText(&nl, 1);
            break;
          }
          if (!pbSkip(q, qe, t2 & 7)) break;
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

// Wyslanie wiadomosci tekstowej na kanal (broadcast) przez API wezla
static size_t putVar(uint8_t* b, uint32_t v) { size_t n = 0; while (v >= 0x80) { b[n++] = (v & 0x7F) | 0x80; v >>= 7; } b[n++] = v; return n; }
bool apiSendText(int ch, const char* text, bool connected = false) {
  if (cdcIdx == 0xFF || fwState == 1) return false;
  if (!connected && !apiConnect()) { apiMode = false; logText("[PICO] Odpowiedz LoRa: wezel nie odpowiada\n"); return false; }
  uint8_t data[220], pkt[240], tr[250], fr[260];
  size_t tl = strlen(text); if (tl > 200) tl = 200;
  size_t d = 0; data[d++] = 0x08; data[d++] = 0x01; data[d++] = 0x12; d += putVar(data + d, tl); memcpy(data + d, text, tl); d += tl;
  size_t p = 0; pkt[p++] = 0x15; put32(pkt + p, 0xFFFFFFFF); p += 4;               // to = broadcast
  pkt[p++] = 0x18; pkt[p++] = (uint8_t)ch;                                          // channel
  pkt[p++] = 0x22; p += putVar(pkt + p, d); memcpy(pkt + p, data, d); p += d;       // decoded
  pkt[p++] = 0x35; put32(pkt + p, 0x50000000 | (millis() & 0x0FFFFFFF)); p += 4;    // id
  size_t t = 0; tr[t++] = 0x0A; t += putVar(tr + t, p); memcpy(tr + t, pkt, p); t += p;  // ToRadio.packet
  fr[0] = 0x94; fr[1] = 0xC3; fr[2] = t >> 8; fr[3] = t & 0xFF; memcpy(fr + 4, tr, t);
  bool ok = usbWriteAll(fr, t + 4);
  apiPump(1500);
  apiMode = false;          // bez "disconnect": wezel dalej przekazuje wiadomosci (komendy) jako pakiety
  logf("[PICO] Odpowiedz LoRa (kanal %d): %s\n", ch, strstr(text, "PWD") ? "(zmiana hasla - tresc ukryta)" : text);
  return ok;
}

bool cfgFail(const char* msg) {
  apiMode = false;
  fwSet(fwPct, "BLAD konfiguracji: %s", msg);
  fwState = 3;
  return false;
}


// Polaczenie API: want_config_id -> my_info z numerem wezla
bool apiConnect() {
  crumb(CR_API);
  lastApiUse = millis();
  fwSet(fwPct < 92 ? 92 : fwPct, "Polaczenie API z wezlem");
  apiNodeNum = 0; apiConfigDone = false; apiFrames = 0;
  apiMode = true;
  static const uint8_t wantCfg[] = { 0x94, 0xC3, 0x00, 0x03, 0x18, 0xB9, 0x60 };   // want_config_id = 12345
  for (int i = 0; i < 3 && !apiNodeNum; i++) {
    if (!usbWriteAll(wantCfg, sizeof(wantCfg))) return false;
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
  apiPump(3000);
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
  File f = LittleFS.open(PKG_PATH, "r");
  if (!f) return cfgFail("brak pakietu na Pico");
  PkgHdr h;
  if (f.read((uint8_t*)&h, sizeof(h)) != sizeof(h) || memcmp(h.magic, "MTPK", 4) != 0 || h.count > 8) {
    f.close(); return cfgFail("niepoprawny pakiet");
  }
  PkgEnt e; bool found = false;
  for (int i = 0; i < h.count && !found; i++)
    if (f.read((uint8_t*)&e, sizeof(e)) == sizeof(e) && e.offset == CFG_OFFSET) found = true;
  if (!found) { f.close(); return cfgFail("w pakiecie nie ma konfiguracji"); }
  bool ok = sendConfig(f, e, false);
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
bool flashPackage() {
  fwState = 1;
  fwSet(0, "Kontrola pakietu (CRC)");
  if (!pkgCrcOk()) {
    LittleFS.remove(PKG_OK_PATH);
    logText("[PICO] Pakiet USZKODZONY w pamieci Pico (CRC niezgodne) - wezel nietkniety\n");
    queueLora("PICO: BLAD - pakiet uszkodzony w pamieci Pico, wezel nietkniety. Pobierz/wyslij ponownie", 0);
    return flashFail("pakiet uszkodzony w pamieci Pico (CRC) - wyslij/pobierz ponownie");
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
    if (e[i].offset == CFG_OFFSET) { cfgIdx = i; logf("[FLASH] Konfiguracja: %lu B\n", (unsigned long)e[i].size); continue; }
    logf("[FLASH] Obraz %d: %s @0x%06lX, %lu B (skompresowany %lu B)\n", i + 1, e[i].name,
         (unsigned long)e[i].offset, (unsigned long)e[i].size, (unsigned long)e[i].csize);
    totalC += e[i].csize;
    nFlash++;
  }

  if (cdcIdx == 0xFF) { f.close(); return flashFail("brak ESP32-S3 na USB"); }

  if (nFlash == 0) {                                     // sama konfiguracja, bez flashowania
    bool ok = cfgIdx >= 0 && sendConfig(f, e[cfgIdx], false);
    f.close();
    if (cfgIdx >= 0) queueLoraReport(ok ? String("PICO: konfiguracja wgrana OK") : String("PICO: BLAD konfiguracji - ") + fwMsg, LORA_REPORT_CFG_MS);
    if (!ok) return cfgIdx >= 0 ? false : flashFail("pusty pakiet");
    noteNodeInfo(false, true, false);
    fwSet(100, "KONFIGURACJA WYSLANA");
    fwState = 2;
    return true;
  }

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
    if (e[i].offset >= INFO_OFFSET) continue;         // konfiguracja / opis - nie do flasha
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
    fwSet(100, "ZAKONCZONE POMYSLNIE");
    fwState = 2;
    return true;
  }
  logText("[FLASH] Firmware wgrany i zweryfikowany. Teraz konfiguracja.\n");
  { File pf = LittleFS.open(CFG_PENDING_PATH, "w"); if (pf) { pf.print(opTime); pf.close(); } }
  fwSet(91, "Czekam %lu s na pierwszy start wezla (klucze, zapis ustawien)", (unsigned long)(CFG_BOOT_WAIT_MS / 1000));
  apiPump(CFG_BOOT_WAIT_MS);
  bool ok = sendConfig(f, e[cfgIdx], false);
  if (!ok) {                                            // jedna powtorka: reset wezla + restart USB + dluzsza przerwa
    logText("[CFG] Konfiguracja nieudana - ponawiam: reset wezla, restart USB\n");
    fwState = 1;
    fwSet(91, "Ponowienie: reset wezla");
    runSeqBlocking(SEQ_RESET, 2);
    waitNodeBoot();
    fwSet(91, "Ponowienie: czekam %lu s na start wezla", (unsigned long)(CFG_RETRY_WAIT_MS / 1000));
    apiPump(CFG_RETRY_WAIT_MS);
    ok = sendConfig(f, e[cfgIdx], false);
  }
  LittleFS.remove(CFG_PENDING_PATH);
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
<title>Pico2W flasher ESP32-S3</title></head>
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
<div id="login" style="display:none;position:fixed;inset:0;background:#000d;z-index:20">
 <div style="max-width:320px;margin:15vh auto;background:#223;padding:20px;border:2px solid #58f">
  <b>Pico2W flasher ESP32-S3</b><p>Haslo:</p>
  <input id="pw" type="password" style="width:100%;padding:6px" onkeydown="if(event.key=='Enter')zaloguj()">
  <p><button onclick="zaloguj()" style="padding:6px 18px">Zaloguj</button> <span id="lerr" style="color:#f66"></span></p>
 </div></div>
<p>
<button onclick="logout()" style="padding:3px 10px">Wyloguj</button>
<button onclick="togSet()" id="setBtn" style="padding:3px 10px">Ustawienia: POKAZ</button>
<button onclick="togLog()" id="logBtn" style="padding:3px 10px">Zbieranie logow</button>
</p>
<div id="setp" style="display:none;background:#222;padding:10px;margin:8px 0;max-width:520px">
 <b>Wi-Fi i haslo</b> <span style="color:#aaa">(puste pole hasla = bez zmian; po zapisie Pico sie zrestartuje)</span>
 <table>
 <tr><td>Tryb</td><td><label><input type="radio" name="md" id="mAP" value="0"> AP (wlasna siec Pico)</label>
   <label><input type="radio" name="md" id="mSTA" value="1"> STA (klient Twojej sieci)</label></td></tr>
 <tr><td>Nazwa sieci AP</td><td><input id="sApS" maxlength="32"></td></tr>
 <tr><td>Haslo AP (8-63)</td><td><input id="sApP" type="password" maxlength="63"></td></tr>
 <tr><td>Siec STA (SSID)</td><td><input id="sStS" maxlength="32"></td></tr>
 <tr><td>Haslo sieci STA</td><td><input id="sStP" type="password" maxlength="63"></td></tr>
 <tr><td>Haslo / token</td><td><input id="sTok" type="password" maxlength="63"> <span style="color:#aaa">(logowanie do tej strony i --token w skrypcie)</span></td></tr>
 <tr><td>Kanal komend LoRa (0-7)</td><td><input id="sCh" type="number" min="0" max="7" style="width:50px"></td></tr>
 <tr><td>Komenda: wlacz Wi-Fi</td><td><input id="c0" maxlength="20"></td></tr>
 <tr><td>Komenda: uspij (Wi-Fi off)</td><td><input id="c1" maxlength="20"></td></tr>
 <tr><td>Komenda: staly AP wlacz</td><td><input id="c2" maxlength="20"></td></tr>
 <tr><td>Komenda: staly AP wylacz</td><td><input id="c3" maxlength="20"></td></tr>
 <tr><td>Komenda: temperatura</td><td><input id="c4" maxlength="20"></td></tr>
 <tr><td>Dozwoleni nadawcy</td><td><input id="sSnd" placeholder="np. ed819a48,0c838131 (puste = wszyscy)" style="width:260px"></td></tr>
 <tr><td>Adres pliku mtnod.txt</td><td><input id="sUrl" placeholder="https:&#47;&#47;serwer/komin/mtnod.txt (puste = brak)" style="width:260px"></td></tr>
 </table>
 <p style="color:#aaa;font-size:12px">Komendy wysyla sie jako wiadomosc tekstowa na kanale komend (wielkosc liter bez znaczenia; 3-20 znakow, bez spacji, kazda inna).
 Domyslne: WLACZ_PICO, USPIJ_PICO, WLACZ_TRYB_AP, WYLACZ_TRYB_AP, TEMP_PICO. Pico odpowiada na kanale krotka wiadomoscia.</p>
 <button onclick="zapiszUst()" style="padding:6px 14px">Zapisz i zrestartuj Pico</button>
 <button onclick="act('Przywrocic WSZYSTKIE ustawienia domyslne (AP, hasla, kanal) i zrestartowac Pico?','/settings_reset')" style="padding:6px 14px;background:#a33;color:#fff">Ustawienia domyslne</button>
 <p style="color:#aaa;font-size:12px">W trybie STA, gdy Pico nie polaczy sie z siecia w 30 s, uruchomi AP. Krotkie nacisniecie BOOTSEL restartuje wezel; przytrzymanie 30 s przywraca ustawienia domyslne Pico.</p>
</div>
<div style="display:flex;flex-wrap:wrap;gap:6px;margin:8px 0">
<button onclick="act('Zrestartowac host USB w Pico?','/reinit')" style="padding:6px 14px">Restart hosta USB</button>
<button onclick="act('Zresetowac wezel ESP32-S3?','/hreset')" style="padding:6px 14px">Reset ESP32-S3 (RTS)</button>
<button onclick="act('Przelaczyc ESP32-S3 w bootloader? Wezel przestanie dzialac do resetu.','/hboot')" style="padding:6px 14px">ESP32-S3 w bootloader</button>
<button onclick="act('Uruchomic test bootloadera? Wezel na chwile przestanie dzialac.','/chiptest')" style="padding:6px 14px;background:#ff0">Test bootloadera</button>
<button id="flBtn" onclick="flashBtn()" style="padding:6px 14px;background:#f80">Flashuj pakiet z Pico</button>
<button onclick="act('Wyslac do wezla konfiguracje z pakietu zapisanego na Pico? Wezel sie zrestartuje.','/cfg_send')" style="padding:6px 14px;background:#c8f">Wgraj konfiguracje</button>
<button onclick="act('Zrestartowac Pico? Strona wroci po ok. 10 s.','/pico_reboot')" style="padding:6px 14px;background:#a33;color:#fff">Restart Pico</button>
<button id="apOff" onclick="act('Wylaczyc tymczasowy tryb AP i wrocic do trybu z ustawien? Jesli w ustawieniach jest STA, ta strona przestanie dzialac pod 192.168.4.1.','/ap_off')" style="padding:6px 14px;background:#c60;color:#fff;display:none">Koniec trybu AP (wroc do ustawien)</button>
</div>
<div style="display:flex;flex-wrap:wrap;gap:6px;margin:8px 0">
<a id="dl1" href="#" style="padding:6px 14px;background:#0a0;color:#fff;text-decoration:none;border-radius:3px;font-size:13px;font-family:sans-serif">Pobierz log (.txt)</a>
<a id="dl3" href="#" style="padding:6px 14px;background:#555;color:#fff;text-decoration:none;border-radius:3px;font-size:13px;font-family:sans-serif">Pobierz logi z flash (.txt)</a>
<a id="dl2" href="#" style="padding:6px 14px;background:#06a;color:#fff;text-decoration:none;border-radius:3px;font-size:13px;font-family:sans-serif">Pobierz zdarzenia Pico (.txt)</a>
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
// Jedno haslo na sesje przegladarki: logowanie przy wejsciu, potem przyciski bez pytania
let TOK=''; try{ TOK=sessionStorage.getItem('tok')||''; }catch(e){}
function F(url, opt){ opt=opt||{}; opt.headers=Object.assign({'X-Auth-Token':TOK}, opt.headers||{}); return fetch(url,opt); }
function pokazLogin(){ document.getElementById('login').style.display='block'; document.getElementById('pw').focus(); }
async function zaloguj(){
  const t=document.getElementById('pw').value;
  const r=await fetch('/auth',{method:'POST',headers:{'X-Auth-Token':t}});
  if(r.status!=200){ document.getElementById('lerr').textContent='Zle haslo'; return; }
  TOK=t; try{ sessionStorage.setItem('tok',t); }catch(e){}
  document.getElementById('login').style.display='none'; document.getElementById('lerr').textContent='';
  linki(); tick();
}
function logout(){ TOK=''; try{ sessionStorage.removeItem('tok'); }catch(e){} location.reload(); }
function linki(){ document.getElementById('dl1').href='/download?t='+encodeURIComponent(TOK);
  document.getElementById('dl2').href='/download_pico?t='+encodeURIComponent(TOK);
  document.getElementById('dl3').href='/download_flash?t='+encodeURIComponent(TOK); }
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
let setOn=false;
async function togSet(){
  setOn=!setOn; document.getElementById('setp').style.display=setOn?'block':'none';
  document.getElementById('setBtn').textContent='Ustawienia: '+(setOn?'UKRYJ':'POKAZ');
  if(!setOn) return;
  const t=await (await F('/settings')).text(), v={};
  t.split('\n').forEach(l=>{const i=l.indexOf('='); if(i>0) v[l.substring(0,i)]=l.substring(i+1);});
  document.getElementById('sApS').value=v.ap_ssid||''; document.getElementById('sStS').value=v.sta_ssid||'';
  document.getElementById(v.sta=='1'?'mSTA':'mAP').checked=true;
  document.getElementById('sCh').value=v.ch||'1'; document.getElementById('sSnd').value=v.senders||''; document.getElementById('sUrl').value=v.url||'';
  ['cmd_on','cmd_sleep','cmd_ap_on','cmd_ap_off','cmd_temp'].forEach((k,i)=>document.getElementById('c'+i).value=v[k]||'');
  ['sApP','sStP','sTok'].forEach(id=>document.getElementById(id).value='');
}
async function zapiszUst(){
  const g=id=>document.getElementById(id).value;
  const sta=document.getElementById('mSTA').checked?'1':'0';
  if(!confirm('Zapisac ustawienia i zrestartowac Pico?'+(sta=='1'?'\nPico przejdzie w tryb STA - strona bedzie pod innym adresem.':''))) return;
  const b=new URLSearchParams({ap_ssid:g('sApS'),ap_pass:g('sApP'),token:g('sTok'),sta:sta,sta_ssid:g('sStS'),sta_pass:g('sStP'),ch:g('sCh'),senders:g('sSnd'),url:g('sUrl'),
    cmd_on:g('c0'),cmd_sleep:g('c1'),cmd_ap_on:g('c2'),cmd_ap_off:g('c3'),cmd_temp:g('c4')});
  const r=await F('/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});
  const txt=await r.text(); alert(txt);
  if(r.status==200 && g('sTok')){ TOK=g('sTok'); try{ sessionStorage.setItem('tok',TOK); }catch(e){} }
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
    if (ip) document.title = 'Pico2W flasher ESP32-S3, IP ' + ip[1] + (tp ? ', ' + tp[1] + '\u00B0C' : '');
    document.getElementById('logBtn').textContent = 'Zbieranie logow: ' + (st.indexOf('logi: zbierane')>=0 ? 'WL (kliknij by wylaczyc)' : 'WYL (kliknij by wlaczyc)');
    document.getElementById('st').textContent = st;
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
if (TOK) { linki(); tick(); } else pokazLogin();
</script></body></html>)HTML";

void handleRoot() {
  server.sendHeader("Connection", "close");
  if (upActive) { server.send(503, "text/html; charset=utf-8",
    "<meta charset='utf-8'><meta http-equiv='refresh' content='30'><body style='background:#111;color:#eee;font-family:sans-serif'>"
    "<h3>Trwa odbior pliku na Pico</h3>Strona odswiezy sie sama co 30 s. Nie klikaj nic - to spowalnia transmisje.</body>"); return; }
  server.send_P(200, "text/html", PAGE);
}

const char* fwStateName() {
  switch (fwState) { case 1: return "TRWA"; case 2: return "OK"; case 3: return "BLAD"; default: return "BRAK"; }
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
             " | temp Pico " + String(analogReadTemp(), 1) + " C" +
             " | logi: " + (cfg.logOn ? "zbierane" : "WYLACZONE") +
             (wifiState == 2 ? String(" | TRYB AP DLA WEZLOW (tymczasowo z LoRa, w ustawieniach ") + (cfg.sta ? "STA" : "AP") + ")" : String("")) +
             " | start Pico: " + bootReason +
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

// Haslo: naglowek X-Auth-Token (skrypt, przyciski) albo parametr ?t= (linki pobierania)
static bool tokenOk() {
  String t = server.header("X-Auth-Token");
  if (!t.length()) t = server.arg("t");
  return t.length() && t == cfg.token;
}
static bool needAuth() {
  if (upActive) {                                   // odbior pliku: nic innego nie obslugujemy (szybka odmowa)
    server.sendHeader("Connection", "close");
    server.send(503, "text/plain", "Trwa odbior pliku - odbior " + String((unsigned long)upNext) + "/" + String((unsigned long)upTotal));
    return false;
  }
  if (tokenOk()) return true;
  server.sendHeader("Connection", "close");
  server.send(401, "text/plain", "Wymagane haslo");
  return false;
}
static uint32_t hdrHex(const char* name) {
  String c = server.header(name);
  if (c.startsWith("0x") || c.startsWith("0X")) c = c.substring(2);
  return strtoul(c.c_str(), NULL, 16);
}

// POST /fw_begin  (X-Total-Size, X-Total-CRC)
void handleFwBegin() {
  rp2040.wdt_reset();
  if (!tokenOk())    { server.send(401, "text/plain", "Brak autoryzacji"); return; }
  if (fwState == 1)  { server.send(409, "text/plain", "Trwa flashowanie"); return; }
  upTotal    = strtoul(server.header("X-Total-Size").c_str(), NULL, 10);
  upTotalCrc = hdrHex("X-Total-CRC");
  bool picoUpd = server.header("X-Target") == "pico";
  // Przed aktualizacja Pico: usun pakiet ESP i pliki pomocnicze (zostaja ustawienia i historia)
  LittleFS.remove(PKG_OK_PATH);
  LittleFS.remove(PKG_PATH);
  LittleFS.remove(PKG_INFO_PATH);
  LittleFS.remove(DL_INFO_PATH);                     // pobranie z internetu nieaktualne
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
  upNext = 0; upActive = true;
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

void handleFwChunkDone() {
  rp2040.wdt_reset();
  if (!tokenOk() || !upActive)       chunkResult = 2;
  else if (chunkOverflow)            chunkResult = 6;
  else if (chunkOffset != upNext)    chunkResult = 3;
  else if ((~crc32Calc(0xFFFFFFFF, chunkBuf, chunkLen)) != chunkExpCrc) chunkResult = 4;
  else {
    uint32_t tw = millis();
    sumRxMs += tw - chunkT0;
    size_t w = upFile ? upFile.write(chunkBuf, chunkLen) : 0;
    sumWrMs += millis() - tw; sumChunks++;
    if (w != chunkLen) chunkResult = 5;
    else { upNext += chunkLen; chunkResult = 1; }
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

void handleFwEnd() {
  rp2040.wdt_reset();
  if (!tokenOk() || !upActive) { server.send(401, "text/plain", "Brak autoryzacji albo brak /fw_begin"); return; }
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
  if (!tokenOk()) { server.send(401, "text/plain", "Brak autoryzacji"); return; }
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
  if (!tokenOk()) { server.send(401, "text/plain", "Brak autoryzacji"); return; }
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
  cfg.apSsid = DEF_AP_SSID; cfg.apPass = DEF_AP_PASS; cfg.token = DEF_TOKEN;
  cfg.staSsid = ""; cfg.staPass = ""; cfg.sta = false; cfg.ch = 1; cfg.senders = ""; cfg.url = ""; cfg.logOn = true;
  for (int i = 0; i < 5; i++) cfg.cmd[i] = CMD_DEF[i];
  File f = LittleFS.open(SETTINGS_PATH, "r");
  if (!f) { logText("[PICO] Ustawienia: BRAK PLIKU - uzywam domyslnych\n"); return; }
  logText("[PICO] Ustawienia wczytane z pliku\n");
  while (f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    int e = l.indexOf('='); if (e < 1) continue;
    String k = l.substring(0, e), v = l.substring(e + 1);
    if (k == "ap_ssid" && v.length()) cfg.apSsid = v;
    else if (k == "ap_pass" && v.length() >= 8) cfg.apPass = v;
    else if (k == "token" && v.length() >= 4) cfg.token = v;
    else if (k == "sta_ssid") cfg.staSsid = v;
    else if (k == "sta_pass") cfg.staPass = v;
    else if (k == "sta") cfg.sta = (v == "1");
    else if (k == "ch") cfg.ch = constrain(v.toInt(), 0, 7);
    else if (k == "senders") cfg.senders = v;
    else if (k == "url") cfg.url = v;
    else if (k == "log") cfg.logOn = (v != "0");
    else for (int i = 0; i < 5; i++) if (k == CMD_KEYS[i] && v.length()) cfg.cmd[i] = v;
  }
  f.close();
}
bool saveSettings() {
  File f = LittleFS.open(SETTINGS_PATH, "w"); if (!f) return false;
  f.printf("ap_ssid=%s\nap_pass=%s\ntoken=%s\nsta=%d\nsta_ssid=%s\nsta_pass=%s\nch=%d\nsenders=%s\nlog=%d\n",
           cfg.apSsid.c_str(), cfg.apPass.c_str(), cfg.token.c_str(), cfg.sta ? 1 : 0, cfg.staSsid.c_str(),
           cfg.staPass.c_str(), cfg.ch, cfg.senders.c_str(), cfg.logOn ? 1 : 0);
  for (int i = 0; i < 5; i++) f.printf("%s=%s\n", CMD_KEYS[i], cfg.cmd[i].c_str());
  f.printf("url=%s\n", cfg.url.c_str());
  f.close(); return true;
}
void handleSettingsGet() {      // hasel nie odsylamy
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", String("ap_ssid=") + cfg.apSsid + "\nsta=" + (cfg.sta ? "1" : "0") +
              "\nsta_ssid=" + cfg.staSsid + "\nch=" + String(cfg.ch) + "\nsenders=" + cfg.senders + "\nurl=" + cfg.url + "\n" +
              CMD_KEYS[0] + "=" + cfg.cmd[0] + "\n" + CMD_KEYS[1] + "=" + cfg.cmd[1] + "\n" + CMD_KEYS[2] + "=" + cfg.cmd[2] +
              "\n" + CMD_KEYS[3] + "=" + cfg.cmd[3] + "\n" + CMD_KEYS[4] + "=" + cfg.cmd[4] + "\n");
}
bool urlOk(const String& u) {
  return (u.startsWith("http://") || u.startsWith("https://")) && u.length() > 10 && u.length() <= 190 && u.indexOf(' ') < 0;
}
void handleSettingsPost() {
  if (fwState == 1) { server.send(409, "text/plain", "Trwa flashowanie - zmiana ustawien zablokowana"); return; }
  Settings n = cfg;
  if (server.hasArg("ap_ssid") && server.arg("ap_ssid").length()) n.apSsid = server.arg("ap_ssid");
  if (server.hasArg("ap_pass") && server.arg("ap_pass").length()) n.apPass = server.arg("ap_pass");
  if (server.hasArg("token")   && server.arg("token").length())   n.token  = server.arg("token");
  if (server.hasArg("sta"))      n.sta = server.arg("sta") == "1";
  if (server.hasArg("sta_ssid")) n.staSsid = server.arg("sta_ssid");
  if (server.hasArg("sta_pass") && server.arg("sta_pass").length()) n.staPass = server.arg("sta_pass");
  if (server.hasArg("ch"))      n.ch = constrain(server.arg("ch").toInt(), 0, 7);
  if (server.hasArg("senders")) { n.senders = server.arg("senders"); n.senders.replace(" ", ""); n.senders.toLowerCase(); }
  if (server.hasArg("url"))     { n.url = server.arg("url"); n.url.trim(); }
  for (int i = 0; i < 5; i++)
    if (server.hasArg(CMD_KEYS[i]) && server.arg(CMD_KEYS[i]).length()) { n.cmd[i] = server.arg(CMD_KEYS[i]); n.cmd[i].trim(); n.cmd[i].toUpperCase(); }
  String err;
  if (n.apSsid.length() > 32 || n.staSsid.length() > 32) err = "SSID moze miec max 32 znaki";
  else if (n.apPass.length() < 8 || n.apPass.length() > 63) err = "Haslo AP: 8-63 znaki";
  else if (n.token.length() < 4) err = "Haslo strony: min. 4 znaki";
  else if (n.sta && !n.staSsid.length()) err = "Tryb STA wymaga nazwy sieci";
  else if (n.sta && n.staPass.length() && n.staPass.length() < 8) err = "Haslo sieci STA: min. 8 znakow";
  else if (n.senders.length() && n.senders.indexOf(',') < 0) err = "Filtr wezlow: puste (wszyscy) albo min. 2 wezly";
  else if (n.url.length() && !urlOk(n.url)) err = "Adres mtnod.txt: http://... albo https://..., max 190 znakow, bez spacji";
  for (const String* v : { &n.apSsid, &n.apPass, &n.token, &n.staSsid, &n.staPass, &n.senders, &n.url })
    if (v->indexOf('\n') >= 0 || v->indexOf('=') == 0) err = "Niedozwolony znak";
  for (int i = 0; i < 5 && !err.length(); i++) {
    if (n.cmd[i].length() < 3 || n.cmd[i].length() > 20 || n.cmd[i].indexOf(' ') >= 0 ||
        n.cmd[i].indexOf(':') >= 0 || n.cmd[i].indexOf('=') >= 0) err = "Komenda: 3-20 znakow, bez spacji, ':' i '='";
    if (n.cmd[i] == "HELP" || n.cmd[i] == "WERSJE" || n.cmd[i] == "AKTUALIZUJ_INTERNET") err = "Nazwy HELP, WERSJE i AKTUALIZUJ_INTERNET sa zarezerwowane";
    for (int j = 0; j < i; j++) if (n.cmd[i] == n.cmd[j]) err = "Komendy musza byc rozne";
  }
  if (err.length()) { server.send(400, "text/plain", err); return; }
  cfg = n;
  if (!saveSettings()) { server.send(500, "text/plain", "Blad zapisu"); return; }
  logText("[PICO] Zapisano nowe ustawienia - restart Pico\n");
  picoRebootAt = millis() + 1500;
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", String("Zapisano. Pico uruchomi sie ponownie za chwile.\n") +
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
void flushLogToFlash(const char* powod) {
  if (fwState == 1 || upActive) return;
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
void logFlashService() {
  if (!fsReady) return;
  if (millis() - flogLastMs > 3600000UL) flushLogToFlash("co godzine");
  else if (logTotal - flogSaved > 36 * 1024) flushLogToFlash("bufor RAM pelny");
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
    if (!held) { held = millis(); zgloszony = false; }
    else if (!zgloszony && millis() - held > 3000) { zgloszony = true; logText("[PICO] BOOTSEL trzymany - po 30 s ustawienia domyslne\n"); }
    if (millis() - held > 30000) {
      LittleFS.remove(SETTINGS_PATH);
      { File r = LittleFS.open("/reset.why", "w"); if (r) { r.print("przytrzymanie BOOTSEL 30 s"); r.close(); } }
      logText("[PICO] BOOTSEL 30 s - przywrocono ustawienia domyslne\n");
      delay(200);
      rp2040.reboot();
    }
  } else if (held) {
    uint32_t d = millis() - held;
    held = 0;
    if (d < 2000) { logText("[PICO] BOOTSEL krotko - restart wezla ESP32-S3\n"); reqCmd = 1; }
    else if (d < 30000) logText("[PICO] BOOTSEL puszczony przed 30 s - bez zmian\n");
  }
}

// Wi-Fi: STA (z powrotem do AP, gdy brak polaczenia) albo AP
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
      rp2040.wdt_reset();
      if (!widoczna) delay(500);
    }
    if (!widoczna) { logf("[PICO] Wi-Fi STA: sieci '%s' nie widac w skanie - krotka proba polaczenia\n", cfg.staSsid.c_str()); limit = 12000; }
    sprobuj = true;
  }
  if (sprobuj) {
    WiFi.setHostname(MDNS_NAME);
    // Kilka podejsc w oknie czasowym: routery z "band steering" (np. MikroTik) odrzucaja
    // pierwsze proby na 2,4 GHz, zeby urzadzenie przeszlo na 5 GHz - Pico ma tylko 2,4 GHz.
    logf("[PICO] Wi-Fi STA: lacze z '%s'...\n", cfg.staSsid.c_str());
    uint32_t t0 = millis();
    int podejscie = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < limit) {
      podejscie++;
      WiFi.begin(cfg.staSsid.c_str(), cfg.staPass.c_str());
      uint32_t t1 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t1 < 7000 && millis() - t0 < limit) { delay(200); rp2040.wdt_reset(); }
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

void setup() {
  switch (rp2040.getResetReason()) {
    case RP2040::PWRON_RESET:    bootReason = "wlaczenie zasilania"; break;
    case RP2040::RUN_PIN_RESET:  bootReason = "pin RUN"; break;
    case RP2040::SOFT_RESET:     bootReason = "programowy"; break;
    case RP2040::WDT_RESET:      bootReason = "WATCHDOG"; break;
    case RP2040::DEBUG_RESET:    bootReason = "debugger"; break;
    case RP2040::GLITCH_RESET:   bootReason = "GLITCH (zaklocenie zasilania)"; break;
    case RP2040::BROWNOUT_RESET: bootReason = "BROWNOUT (spadek napiecia!)"; break;
    default:                     bootReason = "nieznany"; break;
  }
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1); tzset();  // czas polski - takze dla czasu z sieci LoRa
  logf("[PICO] Wersja programu Pico: %s\n", PICO_VERSION);
  logf("[PICO] Przyczyna startu: %s\n", bootReason);
  if (rp2040.getResetReason() == RP2040::WDT_RESET) {   // restart programowy tez idzie przez watchdog - pomijamy go
    logText("[PICO] *** START PO RESTARCIE Z WATCHDOGA ***\n");
    if (watchdog_hw->scratch[1] == CRUMB_MAGIC) {
      uint32_t c = watchdog_hw->scratch[0] & 0xFF, x = watchdog_hw->scratch[0] >> 8;
      logf("[PICO] Ostatni krok przed zawieszeniem: %s%s\n",
           c < sizeof(CRUMB_NAMES) / sizeof(CRUMB_NAMES[0]) ? CRUMB_NAMES[c] : "?",
           x ? (String(", komenda ") + String(x)).c_str() : "");
    }
  }
  else logText("[PICO] Normalny start\n");
  if (rp2040.getResetReason() == RP2040::PWRON_RESET || rp2040.getResetReason() == RP2040::BROWNOUT_RESET)
    bootReportWanted = true;                                 // raport o wlaczeniu zasilania (bootReportService)
  crumb(CR_IDLE);

  if (!LittleFS.begin()) { logText("[PICO] LittleFS nie dal sie zamontowac - FORMATOWANIE (ustawienia utracone)\n");
                           LittleFS.format(); LittleFS.begin(); }
  if (LittleFS.exists("/reset.why")) { logf("[PICO] Ustawienia domyslne przywrocone wczesniej: %s\n", readFileStr("/reset.why").c_str());
                                       LittleFS.remove("/reset.why"); }
  if (LittleFS.exists(PICO_BIN)) {                   // po aktualizacji programu Pico: czysty LittleFS, ustawienia zostaja
    loadSettings();
    String hist = readFileStr(NODE_INFO_PATH), loraOld = readFileStr(PICO_LORA_FLAG);
    LittleFS.format(); LittleFS.begin();
    saveSettings();
    if (loraOld.length()) queueLora(String("PICO: zaktualizowane ") + loraOld + " -> " + PICO_VERSION + ", ustawienia zachowane", 120000);
    resetNodeAfterUpdate = true;                     // wezel pamieta tryb API sprzed aktualizacji - reset = swiezy start i logi
    if (hist.length()) { File h = LittleFS.open(NODE_INFO_PATH, "w"); if (h) { h.print(hist); h.close(); } }
    logf("[PICO] Program Pico zaktualizowany do wersji %s - LittleFS sformatowany od nowa\n", PICO_VERSION);
  }
  fsReady = true;
  loadPkgMode();
  pinMode(LED_BUILTIN, OUTPUT);

  loadSettings();
  rp2040.wdt_begin(8000);         // wczesniej: laczenie STA moze trwac do 30 s (karmione w petli)
  core1Beat = millis();
  startWifi();

  server.collectHeaders("X-Auth-Token", "X-Total-Size", "X-Total-CRC", "X-Offset", "X-Chunk-CRC", "X-Time", "X-Target");
  server.on("/", handleRoot);
  server.on("/status",        []() { if (needAuth()) handleStatus(); });
  server.on("/log",           []() { if (needAuth()) handleLog(); });
  server.on("/picolog",       []() { if (needAuth()) handlePicoLog(); });
  server.on("/download",      []() { if (needAuth()) handleDownload(); });
  server.on("/download_pico", []() { if (needAuth()) handleDownloadPico(); });
  server.on("/download_flash", []() { if (needAuth()) handleDownloadFlash(); });
  server.on("/settings",      HTTP_GET,  []() { if (needAuth()) handleSettingsGet(); });
  server.on("/settings",      HTTP_POST, []() { if (needAuth()) handleSettingsPost(); });
  // Akcje wymagaja hasla (X-Auth-Token) i nie dzialaja w trakcie flashowania
  auto akcja = [](uint8_t cmd, const char* opis) {
    if (!tokenOk()) { server.send(401, "text/plain", "Zle haslo"); return; }
    if (fwState == 1) { server.send(409, "text/plain", "Trwa flashowanie/konfiguracja - poczekaj"); return; }
    reqCmd = cmd; server.send(200, "text/plain", opis);
  };
  server.on("/reinit",   HTTP_POST, [akcja]() { akcja(4, "Restart hosta USB wykonany"); });
  server.on("/hreset",   HTTP_POST, [akcja]() { akcja(1, "Reset ESP32-S3 wykonany"); });
  server.on("/hboot",    HTTP_POST, [akcja]() { akcja(2, "ESP32-S3 przelaczony w bootloader"); });
  server.on("/chiptest", HTTP_POST, [akcja]() { akcja(5, "Test bootloadera uruchomiony - wynik w zdarzeniach Pico"); });
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
  server.on("/auth",     HTTP_POST, []() {
    bool ok = tokenOk();
    logf("[PICO] %s do strony z %s\n", ok ? "Logowanie" : "NIEUDANE logowanie", server.client().remoteIP().toString().c_str());
    server.send(ok ? 200 : 401, "text/plain", ok ? "OK" : "Zle haslo"); });
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
    if (!tokenOk()) { server.send(401, "text/plain", "Brak autoryzacji"); return; }
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

  core1Beat = millis();
  rp2040.wdt_begin(8000);
}

void loop() {
  if (millis() - core1Beat < 3000) rp2040.wdt_reset();
  if (wifiState != 1) {
    if (!staActive) dnsServer.processNextRequest();
    else MDNS.update();
    server.handleClient();
  }
  loraCmdService();
  netService();
  wifiService();
  checkBootselReset();
  ledService();
  logFlashService();
  if (picoRebootAt && (int32_t)(millis() - picoRebootAt) > 0) rp2040.reboot();
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

void loop1() {
  core1Beat = millis();
  USBHost.task();
  crumb(CR_IDLE);

  // Po starcie Pico (zwlaszcza po aktualizacji programu) wezel z zasilaniem bywa uspiony na USB i sie nie zglasza -
  // restart hosta USB po 6, 15 i 30 s, dopoki wezel sie nie pojawi
  static uint8_t bootUsbTries = 0;
  static const uint32_t BOOT_USB_AT[3] = { 6000, 15000, 30000 };
  if (bootUsbTries < 3 && !heltecMounted && fwState != 1 && millis() > BOOT_USB_AT[bootUsbTries]) {
    bootUsbTries++;
    logf("[PICO] Wezel nie widoczny na USB po starcie - restart hosta USB (%u/3)\n", bootUsbTries);
    restartUsbHost();
  } else if (bootUsbTries < 3 && heltecMounted) bootUsbTries = 3;   // wezel jest - koniec sprawdzania

  // Po aktualizacji programu Pico: reset wezla (jak przycisk "Reset ESP32-S3"), potem host USB restartuje sie sam
  if (resetNodeAfterUpdate && heltecMounted && !seq && fwState != 1 && millis() > 8000) {
    resetNodeAfterUpdate = false;
    logText("[PICO] Po aktualizacji Pico: reset wezla (RTS), zeby wystartowal od nowa z logami\n");
    resetNodeFull();
  }

  // Pico zrestartowalo sie w trakcie wysylania konfiguracji po flashowaniu - jedna proba wznowienia
  static bool pendingChecked = false;
  if (!pendingChecked && fsReady) {
    pendingChecked = true;
    if (LittleFS.exists(CFG_PENDING_PATH)) {
      String t = readFileStr(CFG_PENDING_PATH);
      LittleFS.remove(CFG_PENDING_PATH);                // tylko raz - bez petli restartow
      strncpy(opTime, t.c_str(), sizeof(opTime) - 1);
      if (LittleFS.exists(PKG_OK_PATH)) {
        logText("[PICO] Wznawiam wysylanie konfiguracji przerwane restartem Pico\n");
        fwState = 1; fwSet(0, "Wznowienie: czekam na wezel");
        uint32_t t0 = millis();
        lastRxMs = 0;
        while (millis() - t0 < 60000 && !(cdcIdx != 0xFF && millis() - lastRxMs < 3000)) apiPump(100);
        fwSet(1, "Wznowienie: czekam %lu s na pelny start wezla", (unsigned long)(CFG_BOOT_WAIT_MS / 1000));
        apiPump(CFG_BOOT_WAIT_MS);
        configFromPackage();
      }
    }
  }

  uint8_t cmd = reqCmd;
  if (cmd) {
    reqCmd = 0;
    if      (cmd == 1) resetNodeFull();
    else if (cmd == 2) startSeq(SEQ_BOOT, 5, "bootloader DTR/RTS");
    else if (cmd == 4) restartUsbHost();
    else if (cmd == 5) { seq = nullptr; bootloaderTest(); }
    else if (cmd == 6) { seq = nullptr; flashPackage(); }
    else if (cmd == 7) { seq = nullptr; configFromPackage(); }
  }

  runSeq();
  readHeltec();
  bootReportService();
  loraQService();

  // Straznik: ESP32-S3 podlaczony, a od 5 min zadnych danych (Meshtastic loguje co minute)
  static uint32_t lastFix = 0;
  if (heltecMounted && fwState != 1 && millis() - lastRxMs > 300000 && millis() - lastFix > 300000 &&
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
  static const char* const SET_KEYS[] = { "AP_SSID", "AP_PWD", "STA_SSID", "STA_PWD", "MODE_WIFI", "FILTR_NODE", "ADRES_URL" };
  int setIdx = -1, cmdIdx = -1;
  if (hasVal) for (int i = 0; i < 7; i++) if (key == SET_KEYS[i]) setIdx = i;
  if (!hasVal) for (int i = 0; i < 5; i++) if (cfg.cmd[i] == key) cmdIdx = i;
  bool help = !hasVal && key == "HELP", wersje = !hasVal && key == "WERSJE";
  bool dl = hasVal && key == "POBIERZ_INTERNET", upd = !hasVal && key == "AKTUALIZUJ_INTERNET";
  if (setIdx < 0 && cmdIdx < 0 && !help && !wersje && !dl && !upd) return;   // zwykla wiadomosc na kanale
  if (!senderAllowed(from)) { logf("[PICO] Komenda od niedozwolonego nadawcy !%08lx - pominieta\n", (unsigned long)from); return; }
  logf("[PICO] Komenda LoRa '%s' od !%08lx\n", key.c_str(), (unsigned long)from);
  char r[120] = "";
  if (help) {
    String l = "PICO komendy: HELP";
    for (int i = 0; i < 5; i++) l += ", " + cfg.cmd[i];
    queueLora(l, 0);                                         // 1/5: komendy
    queueLora("PICO ustawienia: WERSJE, AP_SSID:, AP_PWD:, STA_SSID:, STA_PWD:, MODE_WIFI:AP/STA, FILTR_NODE:, ADRES_URL:, "
              "POBIERZ_INTERNET:pelna/aktualizacja/pico, AKTUALIZUJ_INTERNET", 0);   // 2/5
    queueLora(statusLine(), 0);                              // 3/5: Wi-Fi
    queueLora(String("PICO: filtr wezlow: ") + (cfg.senders.length() ? cfg.senders : String("wszyscy")), 0);   // 4/5
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
    val.toLowerCase();
    int t = val == "pelna" ? 0 : val == "aktualizacja" ? 1 : val == "pico" ? 2 : -1;
    if (t < 0) { queueLora("PICO: BLAD POBIERZ_INTERNET - podaj pelna, aktualizacja albo pico", 0); return; }
    if (dlReq >= 0 || pendingFlashAt) { queueLora("PICO: BLAD - poprzednie pobieranie/wgrywanie jeszcze trwa", 0); return; }
    dlReq = t;
    return;
  }
  if (upd) { updReq = true; return; }
  if (cmdIdx == 0)      { wifiReq = 0; snprintf(r, sizeof(r), "PICO: Wi-Fi wlaczone (%s)", cfg.sta ? "STA" : "AP"); }
  else if (cmdIdx == 1) { wifiReq = 1; snprintf(r, sizeof(r), "PICO: usypiam (Wi-Fi wylaczone)"); }
  else if (cmdIdx == 2) { wifiReq = 2; snprintf(r, sizeof(r), "PICO: staly AP '%s' dla wezli", cfg.apSsid.c_str()); }
  else if (cmdIdx == 3) { wifiReq = 0; snprintf(r, sizeof(r), "PICO: koniec trybu AP, Wi-Fi wg ustawien"); }
  else if (cmdIdx == 4) { snprintf(r, sizeof(r), "PICO: temp %.1f C, uptime %lu min", analogReadTemp(), (unsigned long)(millis() / 60000)); }
  if (r[0]) { queueLora(r, 0); return; }

  // ---- zmiana ustawien ----
  String err, shown = val; bool restart = false;
  if (val.startsWith("=")) err = "wartosc nie moze zaczynac sie od '='";
  else if (setIdx == 0) {                                     // AP_SSID
    if (!val.length() || val.length() > 32) err = "nazwa 1-32 znaki";
    else { cfg.apSsid = val; restart = wifiState == 2 || (wifiState == 0 && !staActive); }
  } else if (setIdx == 1) {                                   // AP_PWD
    if (val.length() < 8 || val.length() > 63) err = "haslo 8-63 znaki";
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
void parseCmdLine(const char* l) {
  if (strstr(l, "handleReceived(REMOTE)") && strstr(l, "Portnum=1 ")) {
    const char* c = strstr(l, " Ch=");
    txtId = hexAfter(l, "id=0x"); txtFrom = hexAfter(l, "fr=0x"); txtCh = c ? atoi(c + 4) : -1;
    return;
  }
  const char* m = strstr(l, "Received text msg from=0x");
  if (!m) return;
  uint32_t id = hexAfter(m, "id=0x"), from = hexAfter(m, "from=0x");
  const char* t = strstr(m, "msg=");
  if (!t || id != txtId || txtCh != cfg.ch) return;           // inny kanal albo nie ta wiadomosc
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
    out[o++] = c;
    if (c == '\n') {
      line[lineLen] = 0;
      const char* p = strstr(line, "nodenum 0x");
      if (p) { uint32_t v = strtoul(p + 10, nullptr, 16); if (v) logNodeNum = v; }
      parseCmdLine(line);
      { const char* q = strstr(line, "| ");               // godzina z prefiksu logu wezla, gdy wezel zna czas
        if (q && q - line < 8 && isdigit(q[2]) && isdigit(q[3]) && q[4] == ':' && isdigit(q[5]) && q[7] == ':') {
          nodeTodSec = atoi(q + 2) * 3600UL + atoi(q + 5) * 60UL + atoi(q + 8); nodeTodMs = millis(); nodeTimeOk = true; } }
      lineLen = 0;
    } else if (lineLen < sizeof(line) - 1) line[lineLen++] = c;
  }
  if (o && (cfg.logOn || fwState == 1)) logAppend((const char*)out, o);   // w trakcie flashowania zawsze
}

// W trybie API: ramki 0x94 0xC3 len_hi len_lo + FromRadio; reszta to tekst
// Ramki 0x94 0xC3 len_hi len_lo + FromRadio; wszystko inne to tekst logu. Dziala zawsze:
// po uzyciu API wezel przez ok. 15 min wysyla logi jako LogRecord (gdy wlaczone debug_log_api_enabled)
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
    n = SerialHost.read(in, n);
    if (n <= 0) break;
    totalBytes += n;
    lastRxMs = millis();
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

// ---- Aktualizacja przez internet: plik mtnod.txt (podpisany tokenem) i pliki obok niego ----
// Linia: typ|wersja|plik|rozmiar|crc32 ; podpis=HMAC-SHA256(token, "typ|wersja|rozmiar|crc\n" ... ) w kolejnosci z pliku
static const char* const DL_TYPES[3] = { "pelna", "aktualizacja", "pico" };
static uint8_t dlBuf[2048];
static bool httpOpen(HTTPClient& http, const String& url, String& err) {
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (url.startsWith("https://")) http.setInsecure();      // szyfrowanie bez sprawdzania certyfikatu - chroni podpis
  if (!http.begin(url)) { err = "zly adres"; return false; }
  int code = http.GET();
  if (code != 200) { err = String("HTTP ") + code + " " + url.substring(url.lastIndexOf('/') + 1); http.end(); return false; }
  return true;
}
static bool netFail(const String& err) {
  logf("[PICO] Internet: BLAD - %s\n", err.c_str());
  queueLora(String("PICO: BLAD pobierania - ") + err, 0);
  return false;
}
bool netDownload(int typ) {
  if (!cfg.url.length()) return netFail("brak ADRES_URL");
  if (wifiState != 0 || !staActive) return netFail("Pico nie jest w sieci STA (wymagany internet)");
  if (fwState == 1 || upActive) return netFail("trwa inna operacja");
  logf("[PICO] Internet: pobieram %s\n", cfg.url.c_str());
  String man, err;
  { HTTPClient http; if (!httpOpen(http, cfg.url, err)) return netFail(err); man = http.getString(); http.end(); }
  rp2040.wdt_reset();
  // parsowanie i podpis
  String signedTxt, sig, plik, wer, skr; uint32_t size = 0, crc = 0; String crcTxt; bool found = false;
  int pos = 0;
  while (pos < (int)man.length()) {
    int e = man.indexOf('\n', pos); if (e < 0) e = man.length();
    String l = man.substring(pos, e); l.trim(); pos = e + 1;
    if (!l.length() || l[0] == '#') continue;
    if (l.startsWith("podpis=")) { sig = l.substring(7); sig.trim(); sig.toLowerCase(); continue; }
    String f[6]; int k = 0, st = 0;                         // typ|wersja|plik|rozmiar|crc[|wersja skryptu]
    for (; k < 6; k++) { int b = l.indexOf('|', st); if (k == 5) b = -1; f[k] = b < 0 ? l.substring(st) : l.substring(st, b); f[k].trim(); if (b < 0) { k++; break; } st = b + 1; }
    if (k != 5 && k != 6) continue;
    signedTxt += f[0] + "|" + f[1] + "|" + f[3] + "|" + f[4] + (k == 6 ? "|" + f[5] : String("")) + "\n";
    if (f[0] == DL_TYPES[typ]) { found = true; wer = f[1]; plik = f[2]; size = strtoul(f[3].c_str(), nullptr, 10); crcTxt = f[4]; crc = strtoul(f[4].c_str(), nullptr, 16); skr = k == 6 ? f[5] : String(""); }
  }
  uint8_t mac[32]; char hex[65];
  hmacSha256(cfg.token.c_str(), cfg.token.length(), signedTxt.c_str(), signedTxt.length(), mac);
  for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", mac[i]);
  if (sig != hex) return netFail("zly podpis mtnod.txt (inny token niz w Pico?)");
  if (!found) return netFail(String("brak wpisu '") + DL_TYPES[typ] + "' w mtnod.txt");
  if (typ != 2 && skr != PICO_VERSION)                      // pakiet wezla musi pochodzic ze skryptu w tej samej wersji
    return netFail(String("pakiet ze skryptu ") + (skr.length() ? skr : String("<0.24")) + ", Pico ma " + PICO_VERSION +
                   " - przygotuj pliki skryptem " + PICO_VERSION);
  if (size < 1024 || size > 3000000) return netFail("nietypowy rozmiar pliku");
  String url = plik.startsWith("http://") || plik.startsWith("https://") ? plik : cfg.url.substring(0, cfg.url.lastIndexOf('/') + 1) + plik;
  // miejsce: usun to, co zostanie zastapione; jesli dalej za malo - takze drugi rodzaj pliku
  const char* dest = typ == 2 ? PICO_DL_PATH : PKG_PATH;
  LittleFS.remove(DL_INFO_PATH);
  if (typ == 2) LittleFS.remove(PICO_DL_PATH);
  else { LittleFS.remove(PKG_OK_PATH); LittleFS.remove(PKG_PATH); LittleFS.remove(PKG_INFO_PATH); loadPkgMode(); }
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
      c = crc32Calc(c, dlBuf, n); got += n; t = millis();
      int pct = (int)((uint64_t)got * 10 / size);
      if (pct != lastPct) { lastPct = pct; logf("[PICO] Internet: %lu/%lu KB\n", (unsigned long)(got / 1024), (unsigned long)(size / 1024)); }
    }
    f.close(); http.end();
  }
  if (got != size || (~c) != crc) { LittleFS.remove(dest); return netFail(String("plik niezgodny (") + got + "/" + size + " B, CRC) - usuniety"); }
  // ponowny odczyt z pamieci Pico
  c = 0xFFFFFFFF; got = 0;
  { File f = LittleFS.open(dest, "r"); size_t n; while (f && (n = f.read(dlBuf, sizeof(dlBuf))) > 0) { c = crc32Calc(c, dlBuf, n); got += n; rp2040.wdt_reset(); } if (f) f.close(); }
  if (got != size || (~c) != crc) { LittleFS.remove(dest); return netFail("kontrola po zapisie niezgodna - plik usuniety"); }
  if (typ != 2) {
    File m = LittleFS.open(PKG_OK_PATH, "w"); if (m) { m.print(crc, HEX); m.close(); }
    savePkgInfo(); loadPkgMode();
  }
  { File m = LittleFS.open(DL_INFO_PATH, "w"); if (m) { m.printf("%s|%s|%lu|%s", DL_TYPES[typ], wer.c_str(), (unsigned long)size, crcTxt.c_str()); m.close(); } }
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
