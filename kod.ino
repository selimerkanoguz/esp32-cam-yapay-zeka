#include "esp_camera.h"
#include <WiFi.h>
#include <FS.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <Preferences.h>
#include <time.h>

// ================= Wi-Fi Ayarları (varsayilan / ilk kurulum) =================
String sta_ssid     = "wifi";
String sta_password = "wifisifre";

const char* ap_ssid      = "ESP32-CAM-Kamera";
const char* ap_password  = "12345678";
const char* ota_password = "admin123!";

// ================= Dahili Flaş LED =================
#define FLASH_LED_PIN 4

// ================= 0.96 SSD1306 OLED =============
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1

#define I2C_SDA 14
#define I2C_SCL 15

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oled_aktif = false;
WebServer server(80);
bool psram_var = false;
Preferences ayarlarNvs;
Preferences wifiNvs;
Preferences geminiNvs;

String gemini_api_key = "xxxxxxx";
String gemini_prompt  = "Bu kamera karesi bir ekrandan veya belgeden cekildi. GÖREV: 1) Ekrandaki yaziyi veya soruyu dikkatle oku. 2) Eger bir soru varsa dogrudan cevabini ver. 3) Eger yazi veya bilgi varsa kisaca ne yazdigini soyle. 4) Eger goruntu cok karanlik, parlamis veya okunmuyorsa kesinlikle \"Okunamadi: Goruntu net degil\" yaz. FORMAT: OLED ekrana basilacak, en fazla 12-15 kelimelik TEK kisa cumle ile Turkce yanit ver.";

// ================= AI-Thinker Pinleri ==============
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ================= Tarih ve Saat (NTP) =================
const long gmtOffset_sec = 3 * 3600; // Türkiye: UTC+3
const int daylightOffset_sec = 0;

String tarihSaatOled() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 30)) {
    return "--:--:--";
  }
  char buf[32];
  strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M:%S", &timeinfo);
  return String(buf);
}

String tarihSaatKisa() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 30)) {
    return String(millis() / 1000) + "s";
  }
  char buf[24];
  strftime(buf, sizeof(buf), "%d.%m %H:%M", &timeinfo);
  return String(buf);
}

String sonAktifIp = "";
unsigned long sonSaatGuncelleme = 0;
unsigned long oledMesajZamani = 0;
bool oledMesajModunda = false;
String ekranMetni = "Sistem Hazir";

String htmlEscape(const String &text) {
  String sonuc = text;
  sonuc.replace("&", "&amp;");
  sonuc.replace("<", "&lt;");
  sonuc.replace(">", "&gt;");
  sonuc.replace("\"", "&quot;");
  sonuc.replace("'", "&#039;");
  return sonuc;
}

String jsKacir(const String &metin) {
  String s = metin;
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\r", "");
  s.replace("\n", "\\n");
  return s;
}

void geminiAyarlariYukle() {
  geminiNvs.begin("geminicfg", true);
  gemini_api_key = geminiNvs.getString("key", gemini_api_key);
  gemini_prompt  = geminiNvs.getString("prompt", gemini_prompt);
  geminiNvs.end();
  logEkle("Gemini ayarlari NVS'den yuklendi.");
}

void geminiAyarlariKaydet(const String &key, const String &prompt) {
  geminiNvs.begin("geminicfg", false);
  geminiNvs.putString("key", key);
  geminiNvs.putString("prompt", prompt);
  geminiNvs.end();
  gemini_api_key = key;
  gemini_prompt  = prompt;
  logEkle("Gemini ayarlari (API Key & Prompt) NVS'ye kaydedildi.");
}

// ================= HTML Log Sistemi =================
#define LOG_MAKS_SATIR 100
String logSatirlari[LOG_MAKS_SATIR];
int logSayaci = 0;
int logIndex = 0;

void logEkle(String mesaj) {
  unsigned long ms = millis();
  String zaman = "[" + String(ms / 1000.0, 1) + "s] ";
  logSatirlari[logIndex] = zaman + mesaj;
  logIndex = (logIndex + 1) % LOG_MAKS_SATIR;
  if (logSayaci < LOG_MAKS_SATIR) logSayaci++;
}

String logHtmlParcasi() {
  String html = "<div class='log-box' id='log'>";
  html += "<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:8px;'>";
  html += "<h3 style='margin:0;'>&#128221; ESP32-CAM Canlı Log</h3>";
  html += "<button type='button' class='btn-log-copy' id='btnCopyLogRoot' onclick='logKopyala()' title='Tüm sistem loglarını panoya kopyala'>&#128220; Log Kopyala</button>";
  html += "</div>";
  if (logSayaci == 0) {
    html += "<div class='line' style='color:#94a3b8;font-style:italic;'>Henüz log yok.</div>";
  } else {
    int baslangic = (logSayaci < LOG_MAKS_SATIR) ? 0 : logIndex;
    for (int i = 0; i < logSayaci; i++) {
      int idx = (baslangic + i) % LOG_MAKS_SATIR;
      html += "<div class='line'>" + logSatirlari[idx] + "</div>";
    }
  }
  html += "</div>";
  return html;
}

void handleLogMetin() {
  String metin = "=== ESP32-CAM SISTEM VE DONANIM RAPORU ===\n";
  metin += "Zaman / Uptime: " + String(millis() / 1000) + " sn (" + tarihSaatKisa() + ")\n";
  metin += "IP Adresi: " + WiFi.localIP().toString() + " | RSSI: " + String(WiFi.RSSI()) + " dBm\n";
  metin += "Dahili Heap: Bos=" + String(ESP.getFreeHeap() / 1024) + " KB (" + String(ESP.getFreeHeap()) + " B) / " + String(ESP.getHeapSize() / 1024) + " KB\n";
  if (psramFound()) {
    metin += "Harici PSRAM: Bos=" + String(ESP.getFreePsram() / 1024) + " KB (" + String(ESP.getFreePsram()) + " B) / " + String(ESP.getPsramSize() / 1024) + " KB\n";
  } else {
    metin += "Harici PSRAM: BULUNAMADI\n";
  }
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    metin += "Kamera: Cozunurluk=" + framesizeAdiYaz(s->status.framesize) + ", Kalite=" + String(s->status.quality);
    metin += ", Parlaklik=" + String(s->status.brightness) + ", Kontrast=" + String(s->status.contrast);
    metin += ", Doygunluk=" + String(s->status.saturation) + ", Efekt=" + String(s->status.special_effect) + "\n";
  }
  metin += "Gemini: API Key=" + (gemini_api_key.length() > 8 ? (gemini_api_key.substring(0, 6) + "..." + gemini_api_key.substring(gemini_api_key.length() - 4)) : "Yok") + " | Prompt Uzunluk=" + String(gemini_prompt.length()) + " karakter\n";
  metin += "================ CANLI LOGLAR ================\n";
  if (logSayaci == 0) {
    metin += "Henuz log yok.\n";
  } else {
    int baslangic = (logSayaci < LOG_MAKS_SATIR) ? 0 : logIndex;
    for (int i = 0; i < logSayaci; i++) {
      int idx = (baslangic + i) % LOG_MAKS_SATIR;
      metin += logSatirlari[idx] + "\n";
    }
  }
  metin += "==============================================\n";
  server.send(200, "text/plain; charset=utf-8", metin);
}

void handleLog() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta http-equiv='refresh' content='2'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>ESP32-CAM Log</title>";
  html += "<style>*{box-sizing:border-box;}body{font-family:'Courier New',monospace;background:#f0f4f8;color:#1e293b;margin:0;padding:20px;}";
  html += "h3{color:#1e293b;font-family:Arial,sans-serif;margin-top:0;font-size:1rem;font-weight:600;}";
  html += ".line{border-bottom:1px solid #e2e8f0;padding:5px 0;word-break:break-all;font-size:12.5px;color:#0f6c3a;}";
  html += ".log-box{background:#fff;border:1px solid #cbd5e1;border-radius:14px;padding:14px 16px;margin-top:16px;box-shadow:0 2px 12px rgba(15,30,60,0.08);}";
  html += "a{color:#2563eb;text-decoration:none;font-family:Arial,sans-serif;font-size:14px;font-weight:500;}a:hover{text-decoration:underline;}</style></head>";
  html += "<body><a href='/'>&larr; Ana sayfaya don</a>";
  html += logHtmlParcasi();
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void i2cTara() {
  logEkle("I2C tarama basliyor (SDA=" + String(I2C_SDA) + ", SCL=" + String(I2C_SCL) + ")");
  int bulunanSayi = 0;
  for (byte adres = 1; adres < 127; adres++) {
    Wire.beginTransmission(adres);
    byte hata = Wire.endTransmission();
    if (hata == 0) {
      char buf[32];
      snprintf(buf, sizeof(buf), "Cihaz bulundu: 0x%02X", adres);
      logEkle(String(buf));
      bulunanSayi++;
    }
  }
  if (bulunanSayi == 0) {
    logEkle("I2C tarama: HICBIR cihaz bulunamadi! Kablo/lehim/guc kontrol et.");
  } else {
    logEkle("I2C tarama bitti, toplam " + String(bulunanSayi) + " cihaz bulundu.");
  }
}

bool oledBulunduMu(byte adres) {
  Wire.beginTransmission(adres);
  return (Wire.endTransmission() == 0);
}

String turkceKarakterleriDuzelt(String s) {
  s.replace("ç", "c"); s.replace("Ç", "C");
  s.replace("ğ", "g"); s.replace("Ğ", "G");
  s.replace("ı", "i"); s.replace("İ", "I");
  s.replace("ö", "o"); s.replace("Ö", "O");
  s.replace("ş", "s"); s.replace("Ş", "S");
  s.replace("ü", "u"); s.replace("Ü", "U");
  return s;
}

String oledKisaYazi(String metin, int maxLen) {
  if (metin.length() <= maxLen) return metin;
  return metin.substring(0, maxLen - 1) + ".";
}

void ekranaYaz(String baslik, String metin) {
  if (!oled_aktif) return;

  baslik = turkceKarakterleriDuzelt(baslik);
  metin  = turkceKarakterleriDuzelt(metin);

  String baslikMetni = baslik;
  baslikMetni.trim();
  if (baslikMetni.length() > 21) {
    baslikMetni = baslikMetni.substring(0, 20) + ".";
  }

  String icerik = metin;
  icerik.replace("\r", "");
  icerik.trim();

  display.clearDisplay();

  // 1. ÜST BAŞLIK ALANI (0 - 12 px)
  if (baslikMetni.length() > 0) {
    display.fillRect(0, 0, 128, 12, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK, SSD1306_WHITE);
    display.setTextSize(1);
    display.setTextWrap(false);

    int bLen = baslikMetni.length();
    int bX = (128 - bLen * 6) / 2;
    if (bX < 2) bX = 2;
    display.setCursor(bX, 2);
    display.print(baslikMetni);
  }

  // 2. İÇERİK ALANI (15 - 63 px)
  display.setTextColor(SSD1306_WHITE);

  // Kontrol: Bu metin tek bir IP adresi mi? (Örn: 192.168.1.45)
  bool ipMi = false;
  int noktaSayisi = 0;
  bool sadeceRakamNokta = true;
  for (unsigned int i = 0; i < icerik.length(); i++) {
    char c = icerik.charAt(i);
    if (c == '.') {
      noktaSayisi++;
    } else if (!isDigit(c)) {
      sadeceRakamNokta = false;
      break;
    }
  }
  if (sadeceRakamNokta && noktaSayisi == 3 && icerik.length() >= 7 && icerik.length() <= 15) {
    ipMi = true;
  }

  if (ipMi) {
    sonAktifIp = icerik;
    // === IP ADRESİ İÇİN ÖZEL DÜZEN (Tarih ve Saat Dahil) ===
    display.setTextSize(1);
    display.setTextWrap(false);

    // Ağ adı satırı (y=16)
    display.setCursor(4, 16);
    if (WiFi.status() == WL_CONNECTED) {
      display.print("Ag: " + oledKisaYazi(sta_ssid, 17));
    } else {
      display.print("Ag: " + oledKisaYazi(ap_ssid, 17));
    }

    // IP Kutucuğu (y=26..44)
    display.drawRoundRect(2, 26, 124, 19, 3, SSD1306_WHITE);
    String ipYazisi = "IP: " + icerik;
    int ipX = (128 - ipYazisi.length() * 6) / 2;
    if (ipX < 4) ipX = 4;
    display.setCursor(ipX, 32);
    display.print(ipYazisi);

    // Tarih & Saat satırı (y=50) - Web: IP yerine doğrudan Tarih ve Saat!
    String zamanStr = tarihSaatOled();
    int zamanX = (128 - zamanStr.length() * 6) / 2;
    if (zamanX < 4) zamanX = 4;
    display.setCursor(zamanX, 50);
    display.print(zamanStr);
  }
  else if (baslik == "GELEN MESAJ") {
    // === GELEN MESAJ İÇİN TARİH VE SAATLİ ÖZEL DÜZEN ===
    display.setTextSize(1);
    display.setTextWrap(false);

    // Üstte Tarih ve Saat (y=16)
    String zamanStr = tarihSaatOled();
    int zamanX = (128 - zamanStr.length() * 6) / 2;
    if (zamanX < 4) zamanX = 4;
    display.setCursor(zamanX, 16);
    display.print(zamanStr);

    // İnce ayırıcı çizgi (y=26)
    display.drawFastHLine(4, 26, 120, SSD1306_WHITE);

    // Gelen mesaj metni (y=29..)
    display.setTextWrap(true);
    display.setCursor(4, 29);
    display.print(oledKisaYazi(icerik, 60));
  }
  else if (icerik.indexOf('\n') != -1) {
    // === ÇOK SATIRLI METİNLER (AP modu bilgileri vb.) ===
    display.setTextSize(1);
    display.setTextWrap(false);
    int y = 16;
    int startIdx = 0;
    while (startIdx < (int)icerik.length() && y <= 52) {
      int nextNewline = icerik.indexOf('\n', startIdx);
      String satir = (nextNewline == -1) ? icerik.substring(startIdx) : icerik.substring(startIdx, nextNewline);
      satir.trim();
      display.setCursor(4, y);
      display.print(oledKisaYazi(satir, 20));
      y += 12;
      if (nextNewline == -1) break;
      startIdx = nextNewline + 1;
    }
  }
  else if (icerik.length() <= 8) {
    // === ÇOK KISA KELİMELER (TEST, HATA vb. için büyük yazı) ===
    display.setTextSize(2);
    display.setTextWrap(false);
    int kX = (128 - icerik.length() * 12) / 2;
    if (kX < 0) kX = 0;
    display.setCursor(kX, 30);
    display.print(icerik);
  }
  else {
    // === GENEL METİNLER ===
    display.setTextSize(1);
    display.setTextWrap(true);
    display.setCursor(4, 16);
    display.print(oledKisaYazi(icerik, 80));
  }

  display.display();
}

String buildRootHtml() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>ESP32-CAM Panel</title>";
  html += "<style>";
  html += ":root{--bg:#f1f5f9;--surface:#ffffff;--surface-2:#f8fafc;--border:#e2e8f0;--border-2:#cbd5e1;--primary:#2563eb;--primary-h:#1d4ed8;--ok:#16a34a;--ok-bg:#f0fdf4;--ok-br:#bbf7d0;--warn:#b45309;--warn-bg:#fffbeb;--warn-br:#fde68a;--text:#0f172a;--text-2:#334155;--muted:#64748b;--shadow:0 1px 3px rgba(0,0,0,0.06),0 4px 16px rgba(0,0,0,0.04);}";
  html += "*{box-sizing:border-box;margin:0;padding:0;}";
  html += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Arial,sans-serif;background:var(--bg);color:var(--text);padding:14px 16px 32px;}";
  html += ".page-container{max-width:1200px;margin:0 auto;display:flex;flex-direction:column;gap:14px;}";
  html += ".card{background:var(--surface);border:1px solid var(--border);border-radius:14px;box-shadow:var(--shadow);padding:16px;}";
  html += ".topbar{display:flex;align-items:center;justify-content:space-between;gap:12px;}";
  html += ".brand{display:flex;align-items:center;gap:10px;}";
  html += ".brand-icon{width:36px;height:36px;background:linear-gradient(135deg,#2563eb,#3b82f6);border-radius:9px;display:flex;align-items:center;justify-content:center;font-size:18px;color:#fff;flex-shrink:0;}";
  html += ".title{font-size:1.15rem;font-weight:700;color:var(--text);}";
  html += ".subtitle{font-size:11.5px;color:var(--muted);}";
  html += ".badge{padding:4px 10px;border-radius:999px;font-size:11px;font-weight:600;background:var(--ok-bg);color:var(--ok);border:1px solid var(--ok-br);}";
  html += ".badge.offline{background:var(--warn-bg);color:var(--warn);border-color:var(--warn-br);}";
  html += ".btn-reboot{padding:5px 10px;border-radius:7px;border:1px solid #fca5a5;background:#fff1f2;color:#dc2626;font-size:11.5px;font-weight:700;cursor:pointer;display:inline-flex;align-items:center;gap:4px;transition:all .15s;}";
  html += ".btn-reboot:hover{background:#fee2e2;border-color:#f87171;transform:translateY(-1px);box-shadow:0 2px 6px rgba(220,38,38,0.12);}";
  html += ".btn-log-copy{padding:4px 9px;border-radius:6px;border:1.5px solid var(--border-2);background:var(--surface-2);color:var(--text);font-size:11px;font-weight:700;cursor:pointer;display:inline-flex;align-items:center;gap:4px;transition:all .15s;}";
  html += ".btn-log-copy:hover{background:#eff6ff;border-color:#93c5fd;color:#2563eb;transform:translateY(-1px);box-shadow:0 2px 6px rgba(37,99,235,0.12);}";
  html += ".layout{display:grid;grid-template-columns:1fr;gap:14px;align-items:start;}";
  html += "@media(min-width:880px){.layout{grid-template-columns:1fr 1fr;gap:16px;}.col-cam{position:sticky;top:14px;align-self:start;}}";
  html += ".col-cam,.col-controls{display:flex;flex-direction:column;gap:12px;}";
  html += ".sec-title{font-size:13px;font-weight:700;color:var(--text-2);margin-bottom:10px;display:flex;align-items:center;gap:6px;}";
  html += ".cam-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px;}";
  html += ".cam-badges{display:flex;gap:6px;align-items:center;}";
  html += ".live-dot{background:rgba(220,38,38,0.95);color:#fff;font-size:10px;font-weight:700;padding:2px 7px;border-radius:999px;letter-spacing:0.4px;}";
  html += ".badge-fps{background:#eff6ff;color:#2563eb;font-size:10.5px;font-weight:700;padding:2px 8px;border-radius:999px;border:1px solid #bfdbfe;}";
  html += ".camera-wrap{overflow:hidden;background:#0f172a;border-radius:12px;border:1px solid var(--border-2);min-height:260px;max-height:420px;display:flex;align-items:center;justify-content:center;position:relative;cursor:grab;user-select:none;}";
  html += ".camera-wrap:active{cursor:grabbing;}";
  html += "#camImg{display:block;width:100%;height:auto;max-height:400px;object-fit:contain;transition:transform .05s ease-out;transform-origin:center center;}";
  html += ".zoom-toolbar{display:flex;align-items:center;gap:6px;margin-top:10px;background:var(--surface-2);border:1px solid var(--border);border-radius:10px;padding:8px 10px;flex-wrap:wrap;}";
  html += ".zoom-label{font-size:11.5px;font-weight:600;color:var(--text-2);}";
  html += ".btn-zoom{width:26px;height:26px;border-radius:6px;border:1px solid var(--border-2);background:#fff;color:var(--text);font-weight:700;font-size:13px;cursor:pointer;display:flex;align-items:center;justify-content:center;transition:all .15s;}";
  html += ".btn-zoom:hover{background:#eff6ff;color:var(--primary);border-color:#bfdbfe;}";
  html += ".zoom-slider{flex:1;min-width:90px;accent-color:var(--primary);height:6px;}";
  html += ".zoom-val{font-size:11.5px;font-weight:700;color:var(--primary);min-width:32px;text-align:right;}";
  html += ".btn-zoom-reset{padding:4px 8px;border-radius:6px;border:1px solid var(--border-2);background:#fff;color:var(--muted);font-size:11px;font-weight:600;cursor:pointer;}";
  html += ".btn-zoom-reset:hover{color:var(--text);border-color:var(--text);}";
  html += ".cam-actions{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:8px;}";
  html += ".btn-act{padding:9px 12px;border-radius:9px;border:1.5px solid var(--border-2);background:#fff;color:var(--text-2);font-size:12.5px;font-weight:600;cursor:pointer;display:flex;align-items:center;justify-content:center;gap:6px;transition:all .15s;}";
  html += ".btn-act:hover{background:#eff6ff;border-color:#bfdbfe;color:var(--primary);}";
  html += ".btn-gemini{background:linear-gradient(135deg,#2563eb,#7c3aed)!important;color:#fff!important;border:none!important;font-weight:700!important;box-shadow:0 2px 8px rgba(124,58,237,0.35);}";
  html += ".btn-gemini:hover{opacity:0.92;transform:translateY(-1px);}";
  html += ".form-row{display:flex;gap:8px;align-items:stretch;}";
  html += "input[type='text']{flex:1;padding:11px 14px;border-radius:10px;border:1.5px solid var(--border-2);background:var(--surface-2);color:var(--text);font-size:14px;outline:none;transition:border-color .2s;}";
  html += "input[type='text']:focus{border-color:var(--primary);background:#fff;}";
  html += ".btn-send{padding:11px 20px;border:none;border-radius:10px;background:var(--primary);color:#fff;font-size:14px;font-weight:600;cursor:pointer;transition:background .18s;white-space:nowrap;}";
  html += ".btn-send:hover{background:var(--primary-h);}";
  html += ".help-text{font-size:11.5px;color:var(--muted);margin-top:6px;}";
  html += ".btn-grid{display:grid;grid-template-columns:repeat(2,1fr);gap:8px;}";
  html += "@media(min-width:640px){.btn-grid{grid-template-columns:repeat(4,1fr);}}";
  html += ".btn-tile{display:flex;align-items:center;justify-content:center;gap:6px;padding:9px 10px;border-radius:9px;text-decoration:none;border:1px solid var(--border-2);background:var(--surface-2);color:var(--text);font-size:12.5px;font-weight:600;transition:all .15s;white-space:nowrap;}";
  html += ".btn-tile:hover{transform:translateY(-1px);box-shadow:0 2px 8px rgba(0,0,0,0.06);}";
  html += ".btn-tile-primary:hover{background:#eff6ff;border-color:#bfdbfe;color:var(--primary);}";
  html += ".btn-tile-wifi:hover{background:#f0fdf4;border-color:#bbf7d0;color:#15803d;}";
  html += ".btn-tile-ota:hover{background:#fef2f2;border-color:#fecaca;color:#dc2626;}";
  html += ".btn-tile-log:hover{background:#faf5ff;border-color:#e9d5ff;color:#7e22ce;}";
  html += ".tile-icon{font-size:16px;flex-shrink:0;}";
  html += ".status-items{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:10px;text-align:center;}";
  html += ".status-item{padding:8px;background:var(--surface-2);border-radius:10px;border:1px solid var(--border);}";
  html += ".status-label{font-size:11px;color:var(--muted);display:block;margin-bottom:2px;}";
  html += ".status-val{font-size:12.5px;font-weight:700;color:var(--text);}";
  html += ".status-dot{display:inline-block;width:7px;height:7px;border-radius:50%;background:var(--ok);margin-right:4px;}";
  html += ".status-dot.ap{background:var(--warn);}";
  html += ".log-box{background:var(--surface-2);border:1px solid var(--border);border-radius:12px;padding:12px 14px;max-height:220px;overflow:auto;}";
  html += ".line{border-bottom:1px solid var(--border);padding:4px 0;word-break:break-word;font-size:11px;color:#166534;font-family:'Courier New',monospace;}";
  html += ".line:last-child{border-bottom:none;}";
  html += ".ram-group{display:flex;flex-direction:column;gap:5px;margin-bottom:10px;}";
  html += ".ram-header{display:flex;justify-content:space-between;font-size:12px;font-weight:600;color:var(--text-2);}";
  html += ".prog-bg{height:8px;background:#e2e8f0;border-radius:999px;overflow:hidden;}";
  html += ".prog-bar{height:100%;border-radius:999px;transition:width .4s ease;}";
  html += ".prog-heap{background:linear-gradient(90deg,#22c55e,#16a34a);}";
  html += ".prog-psram{background:linear-gradient(90deg,#8b5cf6,#6d28d9);}";
  html += ".hw-badges{display:flex;gap:6px;flex-wrap:wrap;margin-top:4px;}";
  html += ".hw-pill{font-size:11px;font-weight:600;color:var(--text-2);background:var(--surface-2);padding:3px 8px;border-radius:6px;border:1px solid var(--border);}";
  html += "</style></head><body><div class='page-container'>";

  // Top bar
  String badgeClass = oled_aktif ? "" : " offline";
  String badgeText  = oled_aktif ? "&#9679; OLED AKTİF" : "&#9679; OLED YOK";
  html += "<div class='card topbar'>";
  html += "<div class='brand'><div class='brand-icon'>&#128247;</div><div><div class='title'>ESP32-CAM Panel</div><div class='subtitle'>Kablosuz Kamera & Kontrol</div></div></div>";
  html += "<div style='display:flex;align-items:center;gap:8px;'>";
  html += "<button type='button' class='btn-reboot' onclick='cihazYenidenBaslat()' title='Cihazı Yeniden Başlat'>&#128259; Yeniden Başlat</button>";
  html += "<span class='badge" + badgeClass + "'>" + badgeText + "</span>";
  html += "</div>";
  html += "</div>";

  html += "<div class='layout'>";

  // SOL SÜTUN: KAMERA & FAREYLE BÜYÜTME (Sticky)
  html += "<div class='col-cam'>";
  html += "<div class='card'>";
  html += "<div class='cam-header'><span class='sec-title' style='margin:0;'>&#127909; Canlı Önizleme</span><div class='cam-badges'><span class='live-dot'>&#9679; CANLI</span><span class='badge-fps' id='fpsBadge'>2.0 sn</span></div></div>";
  html += "<div class='camera-wrap' id='camWrap' title='Fare tekerleği ile büyütebilir veya sürükleyebilirsiniz'>";
  html += "<img id='camImg' src='/capture' alt='Kamera bekleniyor...'>";
  html += "</div>";

  // Fare / Dokunmatik Yakınlaştırma Çubuğu
  html += "<div class='zoom-toolbar'>";
  html += "<span class='zoom-label'>&#128269; Yakınlaştır:</span>";
  html += "<button type='button' class='btn-zoom' onclick='stepZoom(25)' title='Büyüt'>+</button>";
  html += "<button type='button' class='btn-zoom' onclick='stepZoom(-25)' title='Küçült'>-</button>";
  html += "<input type='range' id='zoomRange' min='100' max='350' value='100' oninput='setZoom(this.value)' class='zoom-slider'>";
  html += "<span class='zoom-val' id='zoomVal'>1.0x</span>";
  html += "<button type='button' class='btn-zoom-reset' onclick='resetZoom()'>Sıfırla</button>";
  html += "</div>";

  // Hızlı Aksiyonlar: Şimdi Çek yerine Gemini'ye Sor!
  html += "<div class='cam-actions'>";
  html += "<button type='button' class='btn-act btn-gemini' id='btnGemini' onclick='geminiyeSor()'>&#129302; Gemini'ye Sor</button>";
  html += "<button type='button' class='btn-act' onclick='toggleFullScreen()'>&#9974; Tam Ekran</button>";
  html += "</div>";
  html += "</div>";
  html += "</div>"; // col-cam sonu

  // SAĞ SÜTUN: GEMINI YANITI, OLED MESAJ, BOOTSTRAP MENÜ VE DURUM
  html += "<div class='col-controls'>";

  // Gemini AI Yanıt Kartı
  html += "<div class='card' id='geminiKarti' style='border-left:4px solid #7c3aed;background:#faf5ff;'>";
  html += "<div class='sec-title' style='color:#7e22ce;margin-bottom:6px;'>&#129302; Gemini AI Yanıtı</div>";
  html += "<div id='geminiDurum' style='font-size:13px;color:#334155;line-height:1.5;'>Kameradaki görüntüyü analiz ettirmek için sol alttaki <b>Gemini'ye Sor</b> butonuna basın.</div>";
  html += "</div>";

  // OLED Mesaj Formu
  html += "<div class='card'>";
  html += "<div class='sec-title'>&#128172; OLED Ekrana Mesaj Gönder</div>";
  html += "<form action='/' method='POST'><div class='form-row'>";
  html += "<input type='text' name='metin' placeholder='Ekrana yazılacak mesajı girin...' required>";
  html += "<button type='submit' class='btn-send'>Gönder</button>";
  html += "</div></form>";
  html += "<div class='help-text'>Yazdığınız metin cihaz üzerindeki OLED ekrana anında iletilir.</div>";
  html += "</div>";

  // Hızlı Erişim Butonları (Kompakt Toolbar)
  html += "<div class='card' style='padding:12px 14px;'>";
  html += "<div class='sec-title' style='margin-bottom:8px;'>&#9881; Hızlı Menü & Ayarlar</div>";
  html += "<div class='btn-grid'>";
  html += "<a href='/ayarlar' class='btn-tile btn-tile-primary'><span class='tile-icon'>&#9881;&#65039;</span><span>Ayarlar</span></a>";
  html += "<a href='/wifi' class='btn-tile btn-tile-wifi'><span class='tile-icon'>&#128246;</span><span>WiFi Ayarları</span></a>";
  html += "<a href='/update' class='btn-tile btn-tile-ota'><span class='tile-icon'>&#128640;</span><span>OTA Yazılım</span></a>";
  html += "<a href='/log' class='btn-tile btn-tile-log'><span class='tile-icon'>&#128221;</span><span>Sistem Logu</span></a>";
  html += "</div></div>";

  // Canlı Sistem & Bellek (RAM) Kartı
  html += "<div class='card'>";
  html += "<div class='sec-title'>&#128202; Canlı Sistem & Bellek (RAM)</div>";
  html += "<div class='ram-group'>";
  html += "<div class='ram-header'><span>💾 Dahili RAM (Heap)</span><span id='ramHeapVal'>Hesaplanıyor...</span></div>";
  html += "<div class='prog-bg'><div class='prog-bar prog-heap' id='ramHeapBar' style='width:0%;'></div></div>";
  html += "</div>";
  html += "<div class='ram-group' style='margin-bottom:4px;'>";
  html += "<div class='ram-header'><span>⚡ Harici PSRAM (4 MB)</span><span id='ramPsramVal'>Hesaplanıyor...</span></div>";
  html += "<div class='prog-bg'><div class='prog-bar prog-psram' id='ramPsramBar' style='width:0%;'></div></div>";
  html += "</div>";
  html += "<div class='hw-badges'>";
  html += "<div class='hw-pill'>⏱️ Uptime: <b id='hwUptime'>--:--:--</b></div>";
  html += "<div class='hw-pill'>📶 Wi-Fi: <b id='hwRssi'>-- dBm</b></div>";
  html += "<div class='hw-pill'>🚀 CPU: <b>240 MHz</b></div>";
  html += "</div></div>";

  // Durum Çubuğu Kartı
  bool connected = (WiFi.status() == WL_CONNECTED);
  html += "<div class='card'><div class='status-items'>";
  html += "<div class='status-item'><span class='status-label'>IP Adresi</span><span class='status-val'>" + htmlEscape(WiFi.localIP().toString()) + "</span></div>";
  html += "<div class='status-item'><span class='status-label'>Bağlantı Durumu</span><span class='status-val'><span class='status-dot" + String(connected ? "" : " ap") + "'></span>" + String(connected ? "Wi-Fi Bağlı" : "AP Modu") + "</span></div>";
  html += "<div class='status-item'><span class='status-label'>Tarih & Saat (NTP)</span><span class='status-val' id='webSaat'>" + tarihSaatKisa() + "</span></div>";
  html += "</div></div>";

  // Canlı Log Kartı
  html += logHtmlParcasi();

  html += "</div>"; // col-controls sonu
  html += "</div>"; // layout sonu

  // JavaScript: Adaptif kare yükleme + Fare tekerleği/sürükleme ile Büyütme
  html += "</div><script>";
  html += "var img = document.getElementById('camImg');";
  html += "var wrap = document.getElementById('camWrap');";
  html += "var range = document.getElementById('zoomRange');";
  html += "var valLabel = document.getElementById('zoomVal');";
  html += "var currentZoom = 100;";
  html += "var panX = 0, panY = 0, isDragging = false, startX = 0, startY = 0;";
  html += "var yukleniyor = false;";

  html += "function applyTransform(){";
  html += "  img.style.transform = 'scale(' + (currentZoom / 100) + ') translate(' + panX + 'px, ' + panY + 'px)';";
  html += "  valLabel.innerText = (currentZoom / 100).toFixed(1) + 'x';";
  html += "  range.value = currentZoom;";
  html += "}";

  html += "function setZoom(v){ currentZoom = parseInt(v); if(currentZoom <= 100){ panX=0; panY=0; } applyTransform(); }";
  html += "function stepZoom(delta){ setZoom(Math.max(100, Math.min(350, currentZoom + delta))); }";
  html += "function resetZoom(){ setZoom(100); panX=0; panY=0; applyTransform(); }";

  // Fare tekerleğiyle yakınlaştırma / uzaklaştırma
  html += "wrap.addEventListener('wheel', function(e){";
  html += "  e.preventDefault();";
  html += "  var delta = e.deltaY < 0 ? 20 : -20;";
  html += "  stepZoom(delta);";
  html += "},{passive:false});";

  // Yakınlaşınca fare ile sürükleyerek kaydırma (Pan)
  html += "wrap.addEventListener('mousedown', function(e){";
  html += "  if(currentZoom > 100){ isDragging = true; startX = e.clientX - panX; startY = e.clientY - panY; }";
  html += "});";
  html += "window.addEventListener('mousemove', function(e){";
  html += "  if(isDragging){ panX = e.clientX - startX; panY = e.clientY - startY; applyTransform(); }";
  html += "});";
  html += "window.addEventListener('mouseup', function(){ isDragging = false; });";

  // Tam ekran yapma
  html += "function toggleFullScreen(){";
  html += "  var elem = document.getElementById('camWrap') || document.documentElement;";
  html += "  if(!document.fullscreenElement && !document.webkitFullscreenElement){";
  html += "    if(elem.requestFullscreen){ elem.requestFullscreen().catch(function(){}); }";
  html += "    else if(elem.webkitRequestFullscreen){ elem.webkitRequestFullscreen(); }";
  html += "  } else {";
  html += "    if(document.exitFullscreen){ document.exitFullscreen(); }";
  html += "    else if(document.webkitExitFullscreen){ document.webkitExitFullscreen(); }";
  html += "  }";
  html += "}";

  // Adaptif 2 saniye aralıklı kare yükleyici
  html += "function kareYukle(){";
  html += "  if(yukleniyor) return;";
  html += "  yukleniyor = true;";
  html += "  var n = new Image();";
  html += "  n.onload = function(){ img.src = n.src; yukleniyor = false; setTimeout(kareYukle, 2000); };";
  html += "  n.onerror = function(){ yukleniyor = false; setTimeout(kareYukle, 1500); };";
  html += "  n.src = '/capture?t=' + Date.now();";
  html += "}";
  html += "kareYukle();";

  html += "function manuelYenile(){ yukleniyor = false; kareYukle(); }";

  html += "var geminiKey = \"" + jsKacir(gemini_api_key) + "\";";
  html += "var geminiPrompt = \"" + jsKacir(gemini_prompt) + "\";";
  html += "async function geminiyeSor(){";
  html += "  var btn = document.getElementById('btnGemini');";
  html += "  var durum = document.getElementById('geminiDurum');";
  html += "  btn.disabled = true; btn.innerText = '⏳ İnceleniyor...';";
  html += "  durum.innerHTML = '<span style=\"color:#2563eb;font-weight:600;\">&#128065; Görüntü alınıyor...</span>';";
  html += "  try{";
  html += "    var capRes = await fetch('/capture?t=' + Date.now());";
  html += "    if(!capRes.ok) throw new Error('Kameradan kare alınamadı');";
  html += "    var blob = await capRes.blob();";
  html += "    var b64 = await new Promise(function(resolve, reject){";
  html += "      var r = new FileReader();";
  html += "      r.onloadend = function(){ resolve(r.result.split(',')[1]); };";
  html += "      r.onerror = reject;";
  html += "      r.readAsDataURL(blob);";
  html += "    });";
  html += "    durum.innerHTML = '<span style=\"color:#7c3aed;font-weight:600;\">&#129302; Gemini yapay zekası analiz ediyor...</span>';";
  html += "    var promptMetni = geminiPrompt;";
  html += "    var payload = {";
  html += "      contents: [{ parts: [ { text: promptMetni }, { inline_data: { mime_type: 'image/jpeg', data: b64 } } ] }],";
  html += "      generationConfig: { maxOutputTokens: 250, temperature: 0.1 }";
  html += "    };";
  html += "    var modeller = ['gemini-3.1-flash-lite', 'gemini-3.5-flash-lite', 'gemini-3.6-flash'];";
  html += "    var apiRes = null;";
  html += "    for(var mIdx = 0; mIdx < modeller.length; mIdx++){";
  html += "      try{";
  html += "        var url = 'https://generativelanguage.googleapis.com/v1beta/models/' + modeller[mIdx] + ':generateContent?key=' + geminiKey;";
  html += "        var r = await fetch(url, { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(payload) });";
  html += "        if(r.ok){ apiRes = r; break; }";
  html += "      }catch(e){}";
  html += "    }";
  html += "    if(!apiRes) throw new Error('Yapay zeka modelleri su an yogun, lutfen 5-10 sn sonra tekrar deneyin.');";
  html += "    var apiJson = await apiRes.json();";
  html += "    var cevap = '';";
  html += "    if(apiJson.candidates && apiJson.candidates[0] && apiJson.candidates[0].content && apiJson.candidates[0].content.parts){";
  html += "      cevap = apiJson.candidates[0].content.parts[0].text.trim();";
  html += "    }";
  html += "    if(!cevap) cevap = 'Cevap alınamadı.';";
  html += "    durum.innerHTML = '<div style=\"font-size:14px;font-weight:700;color:#0f172a;margin-bottom:6px;\">&#128161; ' + cevap + '</div><div style=\"font-size:11.5px;color:#16a34a;font-weight:600;\">&#10003; OLED ekrana yazıldı (' + new Date().toLocaleTimeString('tr-TR') + ')</div>';";
  html += "    var postData = new URLSearchParams({ metin: cevap });";
  html += "    await fetch('/', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:postData.toString() });";
  html += "  } catch(e){";
  html += "    durum.innerHTML = '<span style=\"color:#dc2626;font-weight:600;\">&#10060; Hata: ' + e.message + '</span>';";
  html += "  } finally{";
  html += "    btn.disabled = false; btn.innerHTML = '&#129302; Gemini\\'ye Sor';";
  html += "  }";
  html += "}";
  html += "function sistemGuncelle(){";
  html += "  fetch('/sistem/durum').then(function(r){return r.json();}).then(function(j){";
  html += "    var hKB = Math.round(j.heap_used/1024) + ' KB / ' + Math.round(j.heap_total/1024) + ' KB (%' + j.heap_pct + ')';";
  html += "    document.getElementById('ramHeapVal').innerText = hKB;";
  html += "    document.getElementById('ramHeapBar').style.width = j.heap_pct + '%';";
  html += "    if(j.psram_total > 0){";
  html += "      var pKB = Math.round(j.psram_used/1024) + ' KB / ' + Math.round(j.psram_total/1024) + ' KB (%' + j.psram_pct + ')';";
  html += "      document.getElementById('ramPsramVal').innerText = pKB;";
  html += "      document.getElementById('ramPsramBar').style.width = Math.max(j.psram_pct, 2) + '%';";
  html += "    } else { document.getElementById('ramPsramVal').innerText = 'PSRAM Yok'; }";
  html += "    document.getElementById('hwUptime').innerText = j.uptime;";
  html += "    document.getElementById('hwRssi').innerText = j.rssi + ' dBm';";
  html += "  }).catch(function(){});";
  html += "}";
  html += "function cihazYenidenBaslat(){";
  html += "  if(confirm('Cihazi yeniden baslatmak istediginize emin misiniz?')){";
  html += "    fetch('/restart', {method:'POST'}).then(function(){";
  html += "      alert('Cihaz yeniden baslatiliyor... Sayfa 6 saniye sonra otomatik yenilenecektir.');";
  html += "      setTimeout(function(){ window.location.reload(); }, 6000);";
  html += "    }).catch(function(){";
  html += "      setTimeout(function(){ window.location.reload(); }, 6000);";
  html += "    });";
  html += "  }";
  html += "}";
  html += "function logKopyala(){";
  html += "  var btn = document.getElementById('btnCopyLogRoot');";
  html += "  var eski = btn ? btn.innerHTML : '';";
  html += "  if(btn){ btn.disabled = true; btn.innerHTML = '&#9203; Aliniyor...'; }";
  html += "  fetch('/log/metin').then(function(r){ return r.text(); }).then(function(txt){";
  html += "    function bitti(){ if(btn){ btn.innerHTML = '&#10003; Kopyalandi!'; setTimeout(function(){ btn.innerHTML = eski; btn.disabled = false; }, 2000); } }";
  html += "    if(navigator.clipboard && window.isSecureContext){";
  html += "      navigator.clipboard.writeText(txt).then(bitti).catch(function(){ kopyalaFallback(txt, bitti); });";
  html += "    } else { kopyalaFallback(txt, bitti); }";
  html += "  }).catch(function(e){ alert('Log alinamadi: ' + e.message); if(btn){ btn.innerHTML = eski; btn.disabled = false; } });";
  html += "}";
  html += "function kopyalaFallback(text, cb){";
  html += "  var ta = document.createElement('textarea'); ta.value = text; ta.style.position = 'fixed'; ta.style.top = '-9999px';";
  html += "  document.body.appendChild(ta); ta.select(); document.execCommand('copy'); document.body.removeChild(ta); cb();";
  html += "}";
  html += "setInterval(sistemGuncelle, 2000); sistemGuncelle();";
  html += "</script></body></html>";
  return html;
}

void handleSistemDurum() {
  uint32_t hTotal = ESP.getHeapSize();
  uint32_t hFree  = ESP.getFreeHeap();
  uint32_t hUsed  = (hTotal > hFree) ? (hTotal - hFree) : 0;
  int hPct        = (hTotal > 0) ? (int)((hUsed * 100) / hTotal) : 0;

  uint32_t pTotal = psram_var ? ESP.getPsramSize() : 0;
  uint32_t pFree  = psram_var ? ESP.getFreePsram() : 0;
  uint32_t pUsed  = (pTotal > pFree) ? (pTotal - pFree) : 0;
  int pPct        = (pTotal > 0) ? (int)((pUsed * 100) / pTotal) : 0;

  uint32_t uptime = millis() / 1000;
  int upH = uptime / 3600;
  int upM = (uptime % 3600) / 60;
  int upS = uptime % 60;
  char upBuf[24];
  snprintf(upBuf, sizeof(upBuf), "%02d:%02d:%02d", upH, upM, upS);

  int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;

  String json = "{";
  json += "\"heap_total\":" + String(hTotal) + ",";
  json += "\"heap_free\":" + String(hFree) + ",";
  json += "\"heap_used\":" + String(hUsed) + ",";
  json += "\"heap_pct\":" + String(hPct) + ",";
  json += "\"psram_total\":" + String(pTotal) + ",";
  json += "\"psram_free\":" + String(pFree) + ",";
  json += "\"psram_used\":" + String(pUsed) + ",";
  json += "\"psram_pct\":" + String(pPct) + ",";
  json += "\"uptime\":\"" + String(upBuf) + "\",";
  json += "\"rssi\":" + String(rssi) + ",";
  json += "\"cpu\":" + String(ESP.getCpuFreqMHz());
  json += "}";

  server.send(200, "application/json", json);
}

void handleRestart() {
  logEkle("Web arayuzunden yeniden baslatma talep edildi.");
  ekranaYaz("YENIDEN BASLATILIYOR", "Lutfen bekleyin...");
  server.send(200, "text/plain", "OK");
  delay(500);
  ESP.restart();
}

void handleRoot() {
  server.send(200, "text/html", buildRootHtml());
}

void applyDisplayMessage(const String &mesaj) {
  String temiz = mesaj;
  temiz.trim();
  if (temiz.length() == 0) {
    return;
  }

  String zaman = tarihSaatKisa();
  String zamanli = "[" + zaman + "] " + temiz;
  ekranMetni = zamanli;
  logEkle("Web'den mesaj geldi: " + zamanli);

  oledMesajZamani = millis();
  oledMesajModunda = true;
  ekranaYaz("GELEN MESAJ", temiz);
}

void handleRootPost() {
  if (server.hasArg("metin")) {
    applyDisplayMessage(server.arg("metin"));
  }
  server.send(200, "text/html", buildRootHtml());
}

void handleGonder() {
  if (server.hasArg("metin")) {
    applyDisplayMessage(server.arg("metin"));
  }
  server.send(200, "text/html", buildRootHtml());
}

void handleCapture() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    delay(40);
    fb = esp_camera_fb_get();
  }
  if (!fb) {
    delay(80);
    fb = esp_camera_fb_get();
  }
  if (!fb) {
    sensor_t *s = esp_camera_sensor_get();
    String fsAd = s ? framesizeAdiYaz(s->status.framesize) : "Bilinmiyor";
    uint32_t hF = ESP.getFreeHeap() / 1024;
    uint32_t pF = psram_var ? (ESP.getFreePsram() / 1024) : 0;
    logEkle("handleCapture HATA: fb NULL (" + fsAd + " modunda, BosHeap=" + String(hF) + "KB, BosPSRAM=" + String(pF) + "KB)");
    server.send(503, "text/plain", "Kamera karesi alinamadi");
    return;
  }

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.setContentLength(fb->len);
  server.send(200, "image/jpeg", "");

  WiFiClient client = server.client();
  if (client.connected()) {
    uint8_t *ptr = fb->buf;
    size_t kalan = fb->len;
    while (kalan > 0 && client.connected()) {
      size_t parca = (kalan > 2048) ? 2048 : kalan;
      size_t gonderilen = client.write(ptr, parca);
      if (gonderilen == 0) break;
      ptr += gonderilen;
      kalan -= gonderilen;
    }
  }
  esp_camera_fb_return(fb);
}

void handleStream() {
  logEkle("Stream istemcisi baglandi: " + server.client().remoteIP().toString());
  WiFiClient client = server.client();
  String response = "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  server.sendContent(response);

  while (client.connected()) {
    ArduinoOTA.handle();
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
      delay(10);
      continue;
    }
    client.print("--frame\r\n");
    client.print("Content-Type: image/jpeg\r\n");
    client.printf("Content-Length: %u\r\n\r\n", fb->len);
    client.write(fb->buf, fb->len);
    client.print("\r\n");
    esp_camera_fb_return(fb);
    delay(20);
  }
  logEkle("Stream istemcisi ayrildi.");
}

void handleUpdate() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Firmware Güncelle</title>";
  html += "<style>*{box-sizing:border-box;margin:0;padding:0;}body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Arial,sans-serif;background:#f1f5f9;color:#0f172a;padding:24px 16px 40px;text-align:center;}";
  html += ".wrap{max-width:420px;margin:0 auto;}.card{background:#fff;border:1px solid #e2e8f0;border-radius:16px;box-shadow:0 1px 3px rgba(0,0,0,0.07),0 4px 16px rgba(0,0,0,0.06);padding:28px 24px;}";
  html += "h3{font-size:1.2rem;font-weight:700;color:#0f172a;margin-bottom:6px;}";
  html += ".sub{font-size:13px;color:#64748b;margin-bottom:22px;}";
  html += ".file-area{border:2px dashed #cbd5e1;border-radius:12px;padding:24px;background:#f8fafc;margin-bottom:16px;transition:border-color .2s;}";
  html += ".file-area:hover{border-color:#93c5fd;}";
  html += "input[type='file']{font-size:14px;color:#334155;width:100%;}";
  html += "input[type='submit']{width:100%;padding:13px;border:none;border-radius:10px;background:#2563eb;color:#fff;font-size:15px;font-weight:600;cursor:pointer;margin-top:4px;transition:background .18s;}";
  html += "input[type='submit']:hover{background:#1d4ed8;}";
  html += ".warn{background:#fffbeb;border:1px solid #fde68a;border-radius:10px;padding:10px 14px;font-size:12px;color:#92400e;margin-top:14px;line-height:1.5;text-align:left;}";
  html += "a.geri{color:#2563eb;text-decoration:none;font-size:13px;font-weight:500;display:inline-block;margin-bottom:14px;}a.geri:hover{text-decoration:underline;}";
  html += "</style></head><body><div class='wrap'>";
  html += "<a class='geri' href='/'>&#8592; Ana sayfaya dön</a>";
  html += "<div class='card'>";
  html += "<h3>&#128640; Firmware Güncelleme</h3>";
  html += "<div class='sub'>ESP32-CAM için .bin dosyası yükleyin</div>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  html += "<div class='file-area'><input type='file' name='update' accept='.bin'></div>";
  html += "<input type='submit' value='&#9650; Güncelle'>";
  html += "</form>";
  html += "<div class='warn'>&#9888; Güncelleme sırasında cihazın gücünü kesmeyin. Yükleme tamamlandıktan sonra cihaz otomatik olarak yeniden başlayacak.</div>";
  html += "</div></div></body></html>";
  server.send(200, "text/html", html);
}


// ================= KAMERA AYARLARI =================
framesize_t framesizeAdindanBul(int kod) {
  switch (kod) {
    case 0: return FRAMESIZE_QQVGA;
    case 1: return FRAMESIZE_QVGA;
    case 2: return FRAMESIZE_CIF;
    case 3: return FRAMESIZE_VGA;
    case 4: return FRAMESIZE_SVGA;
    case 5: return FRAMESIZE_XGA;
    case 6: return FRAMESIZE_SXGA;
    case 7: return FRAMESIZE_UXGA;
    default: return FRAMESIZE_SVGA;
  }
}

String framesizeAdiYaz(framesize_t fs) {
  switch (fs) {
    case FRAMESIZE_QQVGA: return "160x120 (QQVGA)";
    case FRAMESIZE_QVGA:  return "320x240 (QVGA)";
    case FRAMESIZE_CIF:   return "400x296 (CIF)";
    case FRAMESIZE_VGA:   return "640x480 (VGA)";
    case FRAMESIZE_SVGA:  return "800x600 (SVGA)";
    case FRAMESIZE_XGA:   return "1024x768 (XGA)";
    case FRAMESIZE_SXGA:  return "1280x1024 (SXGA)";
    case FRAMESIZE_UXGA:  return "1600x1200 (UXGA)";
    default: return "Bilinmiyor";
  }
}

int framesizeKoduBul(framesize_t fs) {
  switch (fs) {
    case FRAMESIZE_QQVGA: return 0;
    case FRAMESIZE_QVGA:  return 1;
    case FRAMESIZE_CIF:   return 2;
    case FRAMESIZE_VGA:   return 3;
    case FRAMESIZE_SVGA:  return 4;
    case FRAMESIZE_XGA:   return 5;
    case FRAMESIZE_SXGA:  return 6;
    case FRAMESIZE_UXGA:  return 7;
    default: return 4;
  }
}

// ONEMLI: Cozunurluk/kalite degisince ilk 1-2 kare cogu zaman eski
// tampon boyutuyla karisik/bozuk gelir. Bu fonksiyon yeni ayar sonrasi
// birkac kareyi cope atarak sensorun yeni ayara oturmasini bekler.
void kameraTamponunuTemizle(int kareSayisi) {
  for (int i = 0; i < kareSayisi; i++) {
    camera_fb_t * fb = esp_camera_fb_get();
    if (fb) {
      esp_camera_fb_return(fb);
    }
    delay(80);
  }
}

String sensorDurumJson() {
  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) return "{}";
  String json = "{";
  json += "\"framesize\":" + String(framesizeKoduBul(sensor->status.framesize)) + ",";
  json += "\"quality\":" + String(sensor->status.quality) + ",";
  json += "\"brightness\":" + String(sensor->status.brightness) + ",";
  json += "\"contrast\":" + String(sensor->status.contrast) + ",";
  json += "\"saturation\":" + String(sensor->status.saturation) + ",";
  json += "\"special_effect\":" + String(sensor->status.special_effect) + ",";
  json += "\"awb\":" + String(sensor->status.awb) + ",";
  json += "\"ae_level\":" + String(sensor->status.ae_level) + ",";
  json += "\"gainceiling\":" + String((int)sensor->status.gainceiling);
  json += "}";
  return json;
}

void handleAyarlarDurum() {
  server.send(200, "application/json", sensorDurumJson());
}

void ayarlariKaydet() {
  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) return;
  ayarlarNvs.begin("camcfg", false);
  ayarlarNvs.putInt("fsize", framesizeKoduBul(sensor->status.framesize));
  ayarlarNvs.putInt("quality", sensor->status.quality);
  ayarlarNvs.putInt("bright", sensor->status.brightness);
  ayarlarNvs.putInt("contr", sensor->status.contrast);
  ayarlarNvs.putInt("satur", sensor->status.saturation);
  ayarlarNvs.putInt("effect", sensor->status.special_effect);
  ayarlarNvs.putInt("awb", sensor->status.awb);
  ayarlarNvs.putInt("aelvl", sensor->status.ae_level);
  ayarlarNvs.putInt("gainc", (int)sensor->status.gainceiling);
  ayarlarNvs.end();
  logEkle("Kamera ayarlari kalici hafizaya (NVS) kaydedildi.");
}

void ayarlariYukle() {
  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) return;

  ayarlarNvs.begin("camcfg", true);
  bool kayitVarMi = ayarlarNvs.isKey("quality");

  // Varsayılan: 1024x768 (XGA, kod=5) veya NVS'teki en son seçilen çözünürlük
  int fsizeKod = psram_var ? ayarlarNvs.getInt("fsize", 5) : ayarlarNvs.getInt("fsize", 3);
  framesize_t hedefFs = framesizeAdindanBul(fsizeKod);
  sensor->set_framesize(sensor, hedefFs);

  if (!kayitVarMi) {
    ayarlarNvs.end();
    logEkle("Kamera baslatildi: " + framesizeAdiYaz(hedefFs) + " ile acildi (Varsayilan: 1024x768).");
    return;
  }
  int quality    = ayarlarNvs.getInt("quality", 12);
  int bright     = ayarlarNvs.getInt("bright", 0);
  int contr      = ayarlarNvs.getInt("contr", 0);
  int satur      = ayarlarNvs.getInt("satur", 0);
  int effect     = ayarlarNvs.getInt("effect", 0);
  int awb        = ayarlarNvs.getInt("awb", 1);
  int aelvl      = ayarlarNvs.getInt("aelvl", 0);
  int gainc      = ayarlarNvs.getInt("gainc", 0);
  ayarlarNvs.end();

  sensor->set_quality(sensor, quality);
  sensor->set_brightness(sensor, bright);
  sensor->set_contrast(sensor, contr);
  sensor->set_saturation(sensor, satur);
  sensor->set_special_effect(sensor, effect);
  sensor->set_whitebal(sensor, awb);
  sensor->set_ae_level(sensor, aelvl);
  sensor->set_gainceiling(sensor, (gainceiling_t)gainc);

  kameraTamponunuTemizle(2);

  logEkle("Kamera acilisi: " + framesizeAdiYaz(hedefFs) + " (son secilen) ile baslatildi, ayarlar NVS'den yuklendi.");
}

String buildAyarlarHtml(String durumMesaji) {
  sensor_t *sensor = esp_camera_sensor_get();

  int mevcutFramesizeKod = 5; // 5 = 1024x768 (XGA)
  int mevcutQuality = 12;
  int mevcutBrightness = 0, mevcutContrast = 0, mevcutSaturation = 0;
  int mevcutSpecialEffect = 0, mevcutAwb = 1, mevcutAecLevel = 0, mevcutGainceiling = 0;

  if (sensor) {
    mevcutFramesizeKod = framesizeKoduBul(sensor->status.framesize);
    mevcutQuality = sensor->status.quality;
    mevcutBrightness = sensor->status.brightness;
    mevcutContrast = sensor->status.contrast;
    mevcutSaturation = sensor->status.saturation;
    mevcutSpecialEffect = sensor->status.special_effect;
    mevcutAwb = sensor->status.awb;
    mevcutAecLevel = sensor->status.ae_level;
    mevcutGainceiling = sensor->status.gainceiling;
  }

  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>Ayarlar</title>";
  html += "<style>*{box-sizing:border-box;margin:0;padding:0;}";
  html += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Arial,sans-serif;background:#f1f5f9;color:#0f172a;padding:14px 16px 32px;}";
  html += ".page-container{max-width:1200px;margin:0 auto;display:flex;flex-direction:column;gap:14px;width:100%;}";
  html += ".topbar-row{display:flex;justify-content:space-between;align-items:center;}";
  html += "a.geri{color:#2563eb;text-decoration:none;font-size:13.5px;font-weight:600;display:inline-flex;align-items:center;gap:4px;}";
  html += "a.geri:hover{text-decoration:underline;}";
  html += ".page-title{font-size:1.15rem;font-weight:700;color:#0f172a;}";
  html += ".layout{display:grid;grid-template-columns:1fr;gap:14px;align-items:start;}";
  html += "@media(min-width:880px){.layout{grid-template-columns:1fr 1fr;gap:16px;}.col-cam{position:sticky;top:14px;align-self:start;}}";
  html += ".col-cam{display:flex;flex-direction:column;gap:12px;}";
  html += ".col-settings{display:flex;flex-direction:column;gap:8px;}";
  html += ".card{background:#fff;border:1px solid #e2e8f0;border-radius:14px;box-shadow:0 1px 3px rgba(0,0,0,0.06),0 4px 16px rgba(0,0,0,0.04);padding:14px;}";
  html += ".cam-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px;}";
  html += ".cam-title{font-size:13px;font-weight:700;color:#0f172a;display:flex;align-items:center;gap:6px;}";
  html += ".fps-badge{background:#eff6ff;color:#2563eb;font-size:10.5px;font-weight:700;padding:2px 7px;border-radius:999px;border:1px solid #bfdbfe;}";
  html += ".onizleme-kutu{overflow:hidden;border-radius:12px;border:1px solid #cbd5e1;background:#0f172a;display:flex;align-items:center;justify-content:center;min-height:260px;max-height:420px;position:relative;cursor:grab;user-select:none;}";
  html += ".onizleme-kutu:active{cursor:grabbing;}";
  html += "#onizleme{display:block;width:100%;height:auto;max-height:400px;object-fit:contain;transition:transform .05s ease-out;transform-origin:center center;}";
  html += ".live-dot{position:absolute;top:6px;left:6px;background:rgba(220,38,38,0.9);color:#fff;font-size:9.5px;font-weight:700;padding:2px 7px;border-radius:999px;letter-spacing:0.3px;z-index:2;}";
  html += ".durum-etiket{font-size:11px;color:#2563eb;text-align:center;margin:3px 0 1px;min-height:15px;font-weight:600;}";
  html += ".card-preset{background:linear-gradient(135deg,#eff6ff 0%,#f0fdf4 100%);border:1.5px solid #bfdbfe;border-radius:10px;padding:7px 11px;display:flex;align-items:center;justify-content:space-between;gap:8px;box-shadow:0 1px 4px rgba(37,99,235,0.06);}";
  html += ".preset-title{font-size:12px;font-weight:700;color:#1e3a8a;display:flex;align-items:center;gap:5px;}";
  html += ".preset-desc{font-size:10px;color:#64748b;margin-top:1px;}";
  html += ".btn-preset-act{padding:6px 12px;border-radius:7px;border:none;background:linear-gradient(135deg,#2563eb,#1d4ed8);color:#fff;font-size:11px;font-weight:700;cursor:pointer;display:flex;align-items:center;gap:5px;transition:all .18s;box-shadow:0 2px 5px rgba(37,99,235,0.25);white-space:nowrap;}";
  html += ".btn-preset-act:hover{transform:translateY(-1px);box-shadow:0 3px 8px rgba(37,99,235,0.35);opacity:0.95;}";
  html += "fieldset{border:none;padding:0;margin:0;}";
  html += "legend{font-size:11.5px;font-weight:700;color:#1e293b;margin-bottom:3px;display:flex;align-items:center;gap:5px;}";
  html += "label{display:block;font-size:11px;color:#475569;margin-top:3px;margin-bottom:2px;font-weight:500;}";
  html += "select{width:100%;padding:5px 8px;border-radius:6px;border:1.5px solid #cbd5e1;background:#fff;color:#0f172a;font-size:12px;outline:none;transition:border-color .2s;}";
  html += "select:focus{border-color:#2563eb;}";
  html += "input[type='range']{width:100%;accent-color:#2563eb;margin:2px 0;}";
  html += ".not{font-size:10.5px;color:#64748b;margin-top:2px;line-height:1.3;}";
  html += ".perf-box{background:#f0fdf4;border:1px solid #bbf7d0;color:#15803d;padding:8px 10px;border-radius:8px;font-size:11px;line-height:1.4;margin-top:4px;}";
  html += ".perf-box b{color:#166534;}";
  html += ".qval-row{display:flex;justify-content:space-between;align-items:center;}";
  html += ".qval-badge{background:#eff6ff;color:#2563eb;padding:1px 6px;border-radius:999px;font-size:10.5px;font-weight:700;border:1px solid #bfdbfe;}";
  html += ".grid-2{display:grid;grid-template-columns:1fr 1fr;gap:8px;}";
  html += ".interval-row{display:flex;gap:4px;margin:5px 0;align-items:center;}";
  html += ".btn-int{flex:1;padding:6px 2px;border-radius:7px;border:1px solid #cbd5e1;background:#f8fafc;color:#334155;font-size:11px;font-weight:600;cursor:pointer;transition:all .15s;text-align:center;}";
  html += ".btn-int:hover{border-color:#93c5fd;}";
  html += ".btn-int.active{background:#2563eb;color:#fff;border-color:#2563eb;}";
  html += ".btn-act{width:100%;padding:8px;border-radius:7px;border:1.5px solid #cbd5e1;background:#fff;color:#0f172a;font-size:12px;font-weight:600;cursor:pointer;display:flex;align-items:center;justify-content:center;gap:5px;transition:background .15s,border-color .15s;}";
  html += ".btn-act:hover{background:#eff6ff;border-color:#bfdbfe;color:#2563eb;}";
  html += ".zoom-toolbar{display:flex;align-items:center;gap:6px;margin-top:10px;background:#f8fafc;border:1px solid #e2e8f0;border-radius:10px;padding:8px 10px;flex-wrap:wrap;}";
  html += ".zoom-label{font-size:11.5px;font-weight:600;color:#334155;}";
  html += ".btn-zoom{width:26px;height:26px;border-radius:6px;border:1px solid #cbd5e1;background:#fff;color:#0f172a;font-weight:700;font-size:13px;cursor:pointer;display:flex;align-items:center;justify-content:center;transition:all .15s;}";
  html += ".btn-zoom:hover{background:#eff6ff;color:#2563eb;border-color:#bfdbfe;}";
  html += ".zoom-slider{flex:1;min-width:90px;accent-color:#2563eb;height:6px;}";
  html += ".zoom-val{font-size:11.5px;font-weight:700;color:#2563eb;min-width:32px;text-align:right;}";
  html += ".btn-zoom-reset{padding:4px 8px;border-radius:6px;border:1px solid #cbd5e1;background:#fff;color:#64748b;font-size:11px;font-weight:600;cursor:pointer;}";
  html += ".btn-zoom-reset:hover{color:#0f172a;border-color:#0f172a;}";
  html += ".cam-actions{display:grid;grid-template-columns:1fr 1fr;gap:8px;}";
  html += ".btn-gemini{background:linear-gradient(135deg,#2563eb,#7c3aed)!important;color:#fff!important;border:none!important;font-weight:700!important;}";
  html += ".btn-gemini:hover{opacity:0.92;}";
  html += ".onizleme-kutu{cursor:grab;user-select:none;}";
  html += ".onizleme-kutu:active{cursor:grabbing;}";
  html += "#onizleme{transition:transform .05s ease-out;transform-origin:center center;}";
  html += ".btn-reboot{padding:4px 9px;border-radius:6px;border:1px solid #fca5a5;background:#fff1f2;color:#dc2626;font-size:11px;font-weight:700;cursor:pointer;display:inline-flex;align-items:center;gap:4px;transition:all .15s;}";
  html += ".btn-reboot:hover{background:#fee2e2;border-color:#f87171;transform:translateY(-1px);box-shadow:0 2px 6px rgba(220,38,38,0.12);}";
  html += ".btn-log-copy{padding:4px 9px;border-radius:6px;border:1.5px solid #cbd5e1;background:#f8fafc;color:#0f172a;font-size:11px;font-weight:700;cursor:pointer;display:inline-flex;align-items:center;gap:4px;transition:all .15s;}";
  html += ".btn-log-copy:hover{background:#eff6ff;border-color:#93c5fd;color:#2563eb;transform:translateY(-1px);box-shadow:0 2px 6px rgba(37,99,235,0.12);}";
  html += "input[type='text']{width:100%;padding:7px 10px;border-radius:7px;border:1.5px solid #cbd5e1;background:#fff;color:#0f172a;font-size:12px;outline:none;font-family:monospace;transition:border-color .2s;}";
  html += "input[type='text']:focus,textarea:focus{border-color:#7c3aed;box-shadow:0 0 0 2px rgba(124,58,237,0.12);}";
  html += "textarea{width:100%;padding:7px 10px;border-radius:7px;border:1.5px solid #cbd5e1;background:#fff;color:#0f172a;font-size:11.5px;outline:none;resize:vertical;min-height:65px;font-family:inherit;line-height:1.4;}";
  html += ".btn-gemini-save{padding:7px 14px;border-radius:7px;border:none;background:linear-gradient(135deg,#7c3aed,#6d28d9);color:#fff;font-size:11.5px;font-weight:700;cursor:pointer;display:inline-flex;align-items:center;gap:5px;transition:all .18s;box-shadow:0 2px 6px rgba(124,58,237,0.25);}";
  html += ".btn-gemini-save:hover{transform:translateY(-1px);box-shadow:0 3px 9px rgba(124,58,237,0.35);opacity:0.95;}";
  html += "@media (max-width:880px){";
  html += "  .onizleme-kutu{min-height:180px;}";
  html += "  .onizleme-kutu img{max-height:220px;}";
  html += "}";
  html += "</style></head><body><div class='page-container'>";

  html += "<div class='topbar-row'>";
  html += "<a class='geri' href='/'>&#8592; Ana sayfaya dön</a>";
  html += "<div style='display:flex;align-items:center;gap:8px;'>";
  html += "<button type='button' class='btn-log-copy' id='btnCopyLog' onclick='logKopyala()' title='Tüm sistem loglarını panoya kopyala'>&#128220; Log Kopyala</button>";
  html += "<button type='button' class='btn-reboot' onclick='cihazYenidenBaslat()' title='Cihazı Yeniden Başlat'>&#128259; Yeniden Başlat</button>";
  html += "<div class='page-title'>&#9881;&#65039; Ayarlar</div>";
  html += "</div>";
  html += "</div>";

  html += "<div class='layout'>";

  // SOL KOLON (Kamera & Canlı İzleme - Sticky)
  html += "<div class='col-cam'>";
  html += "<div class='card'>";
  html += "<div class='cam-header'><span class='cam-title'>&#127909; Canlı Önizleme</span><span class='fps-badge' id='fpsGosterge'>2.0 sn</span></div>";
  html += "<div class='onizleme-kutu' id='kutuImg' title='Fare tekerleğiyle büyütebilir veya sürükleyebilirsiniz'><span class='live-dot'>&#9679; CANLI</span><img id='onizleme' src='/capture' alt='Kamera'></div>";

  // Fare / Dokunmatik Yakınlaştırma Çubuğu
  html += "<div class='zoom-toolbar'>";
  html += "<span class='zoom-label'>&#128269; Yakınlaştır:</span>";
  html += "<button type='button' class='btn-zoom' onclick='stepZoom(25)' title='Büyüt'>+</button>";
  html += "<button type='button' class='btn-zoom' onclick='stepZoom(-25)' title='Küçült'>-</button>";
  html += "<input type='range' id='zoomRange' min='100' max='350' value='100' oninput='setZoom(this.value)' class='zoom-slider'>";
  html += "<span class='zoom-val' id='zoomVal'>1.0x</span>";
  html += "<button type='button' class='btn-zoom-reset' onclick='resetZoom()'>Sıfırla</button>";
  html += "</div>";

  // Yenileme Hızı Seçici
  html += "<div style='margin-top:10px;'>";
  html += "<div style='font-size:11.5px;font-weight:600;color:#475569;margin-bottom:4px;'>⏱ Yenileme Aralığı (Yazı okuma için 2-3 sn idealdir):</div>";
  html += "<div class='interval-row'>";
  html += "<button type='button' class='btn-int' onclick='setAralik(1000,this)'>1 sn</button>";
  html += "<button type='button' class='btn-int active' onclick='setAralik(2000,this)'>2 sn</button>";
  html += "<button type='button' class='btn-int' onclick='setAralik(3000,this)'>3 sn</button>";
  html += "<button type='button' class='btn-int' onclick='setAralik(5000,this)'>5 sn</button>";
  html += "<button type='button' class='btn-int' onclick='setAralik(0,this)'>Dondur</button>";
  html += "</div></div>";

  // Hızlı Aksiyon Butonları
  html += "<div class='cam-actions' style='margin-top:6px;'>";
  html += "<button type='button' class='btn-act btn-gemini' id='btnGemini' onclick='geminiyeSor()'>&#129302; Gemini'ye Sor</button>";
  html += "<button type='button' class='btn-act' onclick='toggleFullScreen()'>&#9974; Tam Ekran</button>";
  html += "</div>";

  html += "<div class='durum-etiket' id='durumEtiket'>Hazır</div>";
  html += "</div>";
  html += "</div>";

  // SAĞ KOLON (Tüm Ayarlar)
  html += "<div class='col-settings'>";

  // Hızlı Ekrandan Yazı Okuma Modu Kartı (Kompakt ve Şık Buton)
  html += "<div class='card-preset'>";
  html += "<div><div class='preset-title'>&#128065; Ekrandan Yazı Okuma Modu</div>";
  html += "<div class='preset-desc'>SVGA (800x600) + Gri tonlama + Parlama kesme + Netlik</div></div>";
  html += "<button type='button' class='btn-preset-act' id='btnOku'>&#9889; Modu Uygula</button>";
  html += "</div>";

  html += "<form id='ayarForm'>";

  // Çözünürlük
  html += "<div class='card'><fieldset><legend>&#128250; Çözünürlük (Yazı için SVGA / XGA önerilir)</legend>";
  html += "<select name='framesize' id='f_framesize'>";
  const char* framesizeIsimleri[] = {"160x120 (QQVGA)", "320x240 (QVGA)", "400x296 (CIF)", "640x480 (VGA)", "800x600 (SVGA) - Yazı İçin İdeal", "1024x768 (XGA)* - Çok Net", "1280x1024 (SXGA)*", "1600x1200 (UXGA)*"};
  for (int i = 0; i <= 7; i++) {
    html += "<option value='" + String(i) + "'";
    if (i == mevcutFramesizeKod) html += " selected";
    html += ">" + String(framesizeIsimleri[i]) + "</option>";
  }
  html += "</select>";
  html += "</fieldset></div>";

  // JPEG Kalitesi
  html += "<div class='card'><fieldset>";
  html += "<div class='qval-row'><legend style='margin-bottom:0;'>&#127775; JPEG Netliği / Kalite</legend><span class='qval-badge' id='qval'>" + String(mevcutQuality) + "</span></div>";
  html += "<input type='range' name='quality' id='f_quality' min='10' max='63' value='" + String(mevcutQuality) + "' oninput=\"document.getElementById('qval').innerText=this.value\">";
  html += "</fieldset></div>";

  // Görüntü Ayarları (2'li Kompakt Izgara)
  html += "<div class='card'><fieldset><legend>&#127912; Parlaklık & Kontrast</legend>";
  html += "<div class='grid-2'>";
  html += "<div><label>Parlaklık (-2 .. +2)</label><input type='range' name='brightness' id='f_brightness' min='-2' max='2' value='" + String(mevcutBrightness) + "'></div>";
  html += "<div><label>Kontrast (-2 .. +2)</label><input type='range' name='contrast' id='f_contrast' min='-2' max='2' value='" + String(mevcutContrast) + "'></div>";
  html += "</div>";
  html += "<label>Doygunluk (saturation, -2 .. +2)</label>";
  html += "<input type='range' name='saturation' id='f_saturation' min='-2' max='2' value='" + String(mevcutSaturation) + "'>";
  html += "</fieldset></div>";

  // Renk & Efekt
  html += "<div class='card'><fieldset><legend>&#127752; Renk ve Efekt</legend>";
  html += "<div class='grid-2'>";
  html += "<div><label>Özel Efekt</label>";
  html += "<select name='special_effect' id='f_special_effect'>";
  const char* efektIsimleri[] = {"Yok", "Negatif", "Gri Tonlama (yazı için önerilir)", "Kırmızımsı", "Yeşilimsi", "Mavimsi", "Sepya"};
  for (int i = 0; i <= 6; i++) {
    html += "<option value='" + String(i) + "'";
    if (i == mevcutSpecialEffect) html += " selected";
    html += ">" + String(efektIsimleri[i]) + "</option>";
  }
  html += "</select></div>";
  html += "<div><label>Otomatik Beyaz Dengesi (AWB)</label>";
  html += "<select name='awb' id='f_awb'>";
  html += "<option value='1'" + String(mevcutAwb == 1 ? " selected" : "") + ">Açık</option>";
  html += "<option value='0'" + String(mevcutAwb == 0 ? " selected" : "") + ">Kapalı</option>";
  html += "</select></div>";
  html += "</div>";
  html += "</fieldset></div>";

  // Pozlama & Kazanç (2'li Kompakt Izgara)
  html += "<div class='card'><fieldset><legend>&#128161; Pozlama ve Kazanç (Parlama Önleme)</legend>";
  html += "<div class='grid-2'>";
  html += "<div><label>Pozlama (ae_level, -2..+2)</label><input type='range' name='ae_level' id='f_ae_level' min='-2' max='2' value='" + String(mevcutAecLevel) + "'></div>";
  html += "<div><label>Gain Ceiling (0=2x..6=128x)</label><input type='range' name='gainceiling' id='f_gainceiling' min='0' max='6' value='" + String(mevcutGainceiling) + "'></div>";
  html += "</div>";
  html += "</fieldset></div>";

  html += "</form>";

  // Gemini AI Ayarları Kartı
  html += "<div class='card' style='background:linear-gradient(180deg,#fff 0%,#faf5ff 100%);border:1.5px solid #ddd6fe;'>";
  html += "<fieldset>";
  html += "<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:6px;'>";
  html += "<legend style='color:#6d28d9;margin:0;'>&#129302; Gemini AI Ayarları</legend>";
  html += "<span style='font-size:10px;font-weight:700;color:#7c3aed;background:#ede9fe;padding:2px 7px;border-radius:999px;border:1px solid #ddd6fe;'>Kalıcı Hafıza</span>";
  html += "</div>";
  html += "<form id='geminiForm' onsubmit='geminiAyarlariKaydet(event)'>";
  html += "<label style='color:#5b21b6;font-weight:600;'>Gemini API Anahtarı (API Key)</label>";
  html += "<input type='text' id='f_gemini_key' value='" + htmlEscape(gemini_api_key) + "' placeholder='AIzaSy...' autocomplete='off' spellcheck='false'>";
  html += "<label style='color:#5b21b6;font-weight:600;margin-top:8px;'>Gemini Prompt (Talimat)</label>";
  html += "<textarea id='f_gemini_prompt' rows='3' placeholder='Kameradan gelen fotoğrafla ne yapması gerektiğini yazın...'>" + htmlEscape(gemini_prompt) + "</textarea>";
  html += "<div style='display:flex;justify-content:space-between;align-items:center;margin-top:8px;gap:8px;'>";
  html += "<button type='button' class='btn-zoom-reset' onclick='geminiPromptSifirla()' title='Varsayılan prompta dön'>Varsayılana Dön</button>";
  html += "<button type='submit' class='btn-gemini-save' id='btnGeminiKaydet'>&#128190; Gemini'yi Kaydet</button>";
  html += "</div>";
  html += "<div id='geminiKayitDurum' style='font-size:11px;color:#7c3aed;text-align:right;margin-top:4px;min-height:16px;font-weight:600;'></div>";
  html += "</form>";
  html += "</fieldset></div>";
  html += "</div>"; // col-settings sonu
  html += "</div>"; // layout sonu
  html += "</div>"; // page-container sonu

  // JavaScript
  html += "<script>";
  html += "var onizImg = document.getElementById('onizleme');";
  html += "var kutuImg = document.getElementById('kutuImg');";
  html += "var durumEtiket = document.getElementById('durumEtiket');";
  html += "var fpsGosterge = document.getElementById('fpsGosterge');";
  html += "var range = document.getElementById('zoomRange');";
  html += "var valLabel = document.getElementById('zoomVal');";
  html += "var currentZoom = 100;";
  html += "var panX = 0, panY = 0, isDragging = false, startX = 0, startY = 0;";
  html += "var yukleniyor = false;";
  html += "var beklemeSuresi = 2000;";
  html += "var zamanlayici = null;";

  html += "function applyTransform(){";
  html += "  onizImg.style.transform = 'scale(' + (currentZoom / 100) + ') translate(' + panX + 'px, ' + panY + 'px)';";
  html += "  valLabel.innerText = (currentZoom / 100).toFixed(1) + 'x';";
  html += "  range.value = currentZoom;";
  html += "}";

  html += "function setZoom(v){ currentZoom = parseInt(v); if(currentZoom <= 100){ panX=0; panY=0; } applyTransform(); }";
  html += "function stepZoom(delta){ setZoom(Math.max(100, Math.min(350, currentZoom + delta))); }";
  html += "function resetZoom(){ setZoom(100); panX=0; panY=0; applyTransform(); }";

  html += "function kareAl(){";
  html += "  if(yukleniyor) return;";
  html += "  yukleniyor = true;";
  html += "  var n = new Image();";
  html += "  n.onload = function(){";
  html += "    onizImg.src = n.src;";
  html += "    yukleniyor = false;";
  html += "    if(beklemeSuresi > 0){ zamanlayici = setTimeout(kareAl, beklemeSuresi); }";
  html += "  };";
  html += "  n.onerror = function(){ yukleniyor = false; if(beklemeSuresi > 0){ zamanlayici = setTimeout(kareAl, 1500); } };";
  html += "  n.src = '/capture?t=' + Date.now();";
  html += "}";
  html += "kareAl();";

  html += "function setAralik(ms, btn){";
  html += "  beklemeSuresi = ms;";
  html += "  clearTimeout(zamanlayici);";
  html += "  document.querySelectorAll('.btn-int').forEach(function(b){b.classList.remove('active');});";
  html += "  btn.classList.add('active');";
  html += "  fpsGosterge.innerText = (ms === 0 ? 'Donduruldu' : (ms / 1000) + ' sn');";
  html += "  if(ms > 0){ yukleniyor = false; kareAl(); }";
  html += "}";

  html += "function manuelYenile(){ clearTimeout(zamanlayici); yukleniyor = false; kareAl(); durumGoster('Kare yenilendi ✓'); }";

  html += "kutuImg.addEventListener('wheel', function(e){";
  html += "  e.preventDefault();";
  html += "  var delta = e.deltaY < 0 ? 20 : -20;";
  html += "  stepZoom(delta);";
  html += "},{passive:false});";

  html += "kutuImg.addEventListener('mousedown', function(e){";
  html += "  if(currentZoom > 100){ isDragging = true; startX = e.clientX - panX; startY = e.clientY - panY; }";
  html += "});";
  html += "window.addEventListener('mousemove', function(e){";
  html += "  if(isDragging){ panX = e.clientX - startX; panY = e.clientY - startY; applyTransform(); }";
  html += "});";
  html += "window.addEventListener('mouseup', function(){ isDragging = false; });";

  html += "function toggleFullScreen(){";
  html += "  if(!document.fullscreenElement){ kutuImg.requestFullscreen().catch(function(){}); }else{ document.exitFullscreen(); }";
  html += "}";

  html += "function durumGoster(msg){ durumEtiket.innerText = msg; }";

  html += "function ayarUygula(){";
  html += "  if(zamanlayici){ clearTimeout(zamanlayici); zamanlayici = null; }";
  html += "  var form = document.getElementById('ayarForm');";
  html += "  var data = new URLSearchParams(new FormData(form)).toString();";
  html += "  durumGoster('Uygulanıyor...');";
  html += "  fetch('/ayarlar', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:data})";
  html += "    .then(function(r){ return r.text(); })";
  html += "    .then(function(t){";
  html += "      durumGoster('Uygulandı ve kaydedildi \\u2713');";
  html += "      setTimeout(function(){ manuelYenile(); }, 500);";
  html += "    })";
  html += "    .catch(function(e){";
  html += "      durumGoster('HATA: ayar gönderilemedi');";
  html += "      if(beklemeSuresi > 0) kareAl();";
  html += "    });";
  html += "}";

  html += "function formaUygula(json){";
  html += "  document.getElementById('f_framesize').value = json.framesize;";
  html += "  document.getElementById('f_quality').value = json.quality;";
  html += "  document.getElementById('qval').innerText = json.quality;";
  html += "  document.getElementById('f_brightness').value = json.brightness;";
  html += "  document.getElementById('f_contrast').value = json.contrast;";
  html += "  document.getElementById('f_saturation').value = json.saturation;";
  html += "  document.getElementById('f_special_effect').value = json.special_effect;";
  html += "  document.getElementById('f_awb').value = json.awb;";
  html += "  document.getElementById('f_ae_level').value = json.ae_level;";
  html += "  document.getElementById('f_gainceiling').value = json.gainceiling;";
  html += "}";

  html += "['f_framesize','f_quality','f_brightness','f_contrast','f_saturation','f_special_effect','f_awb','f_ae_level','f_gainceiling'].forEach(function(id){";
  html += "  document.getElementById(id).addEventListener('change', ayarUygula);";
  html += "});";

  html += "document.getElementById('btnOku').addEventListener('click', function(){";
  html += "  if(zamanlayici){ clearTimeout(zamanlayici); zamanlayici = null; }";
  html += "  durumGoster('Yazı Okuma Modu uygulanıyor...');";
  html += "  fetch('/ayarlar/oku', {method:'POST'})";
  html += "    .then(function(r){ return r.json(); })";
  html += "    .then(function(json){";
  html += "      formaUygula(json); durumGoster('Yazı Okuma Modu uygulandı \\u2713');";
  html += "      setTimeout(function(){ manuelYenile(); }, 500);";
  html += "    })";
  html += "    .catch(function(e){";
  html += "      durumGoster('HATA: preset uygulanamadı');";
  html += "      if(beklemeSuresi > 0) kareAl();";
  html += "    });";
  html += "});";

  html += "var geminiKey = \"" + jsKacir(gemini_api_key) + "\";";
  html += "var geminiPrompt = \"" + jsKacir(gemini_prompt) + "\";";
  html += "async function geminiyeSor(){";
  html += "  var btn = document.getElementById('btnGemini');";
  html += "  btn.disabled = true; btn.innerText = '⏳ İnceleniyor...';";
  html += "  durumGoster('Görüntü alınıyor...');";
  html += "  try{";
  html += "    var capRes = await fetch('/capture?t=' + Date.now());";
  html += "    if(!capRes.ok) throw new Error('Kare alınamadı');";
  html += "    var blob = await capRes.blob();";
  html += "    var b64 = await new Promise(function(resolve, reject){ var r = new FileReader(); r.onloadend = function(){ resolve(r.result.split(',')[1]); }; r.onerror = reject; r.readAsDataURL(blob); });";
  html += "    durumGoster('Gemini analiz ediyor...');";
  html += "    var promptMetni = geminiPrompt;";
  html += "    var payload = { contents: [{ parts: [ { text: promptMetni }, { inline_data: { mime_type: 'image/jpeg', data: b64 } } ] }], generationConfig: { maxOutputTokens: 250, temperature: 0.1 } };";
  html += "    var modeller = ['gemini-3.1-flash-lite', 'gemini-3.5-flash-lite', 'gemini-3.6-flash'];";
  html += "    var apiRes = null;";
  html += "    for(var mIdx = 0; mIdx < modeller.length; mIdx++){";
  html += "      try{";
  html += "        var url = 'https://generativelanguage.googleapis.com/v1beta/models/' + modeller[mIdx] + ':generateContent?key=' + geminiKey;";
  html += "        var r = await fetch(url, { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(payload) });";
  html += "        if(r.ok){ apiRes = r; break; }";
  html += "      }catch(e){}";
  html += "    }";
  html += "    if(!apiRes) throw new Error('Yapay zeka modelleri su an yogun, lutfen 5-10 sn sonra tekrar deneyin.');";
  html += "    var apiJson = await apiRes.json();";
  html += "    var cevap = (apiJson.candidates && apiJson.candidates[0] && apiJson.candidates[0].content && apiJson.candidates[0].content.parts) ? apiJson.candidates[0].content.parts[0].text.trim() : 'Cevap alınamadı.';";
  html += "    durumGoster('Cevap: ' + cevap);";
  html += "    var postData = new URLSearchParams({ metin: cevap });";
  html += "    await fetch('/', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:postData.toString() });";
  html += "  }catch(e){ durumGoster('Hata: ' + e.message); }";
  html += "  finally{ btn.disabled = false; btn.innerHTML = '&#129302; Gemini\\'ye Sor'; }";
  html += "}";
  html += "function geminiAyarlariKaydet(e){";
  html += "  e.preventDefault();";
  html += "  var btn = document.getElementById('btnGeminiKaydet');";
  html += "  var durum = document.getElementById('geminiKayitDurum');";
  html += "  var key = document.getElementById('f_gemini_key').value.trim();";
  html += "  var prompt = document.getElementById('f_gemini_prompt').value.trim();";
  html += "  if(!key){ alert('Lutfen gecerli bir Gemini API anahtari girin.'); return; }";
  html += "  if(!prompt){ alert('Lutfen gecerli bir prompt talimati girin.'); return; }";
  html += "  btn.disabled = true; durum.innerText = 'Kaydediliyor...';";
  html += "  var data = new URLSearchParams({ gemini_key: key, gemini_prompt: prompt }).toString();";
  html += "  fetch('/ayarlar/gemini', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:data })";
  html += "    .then(function(r){ return r.text(); })";
  html += "    .then(function(t){";
  html += "      geminiKey = key; geminiPrompt = prompt;";
  html += "      durum.innerHTML = '&#10003; Gemini ayarlari kaydedildi!';";
  html += "      setTimeout(function(){ durum.innerText = ''; }, 3000);";
  html += "    })";
  html += "    .catch(function(err){ durum.innerText = 'HATA: ' + err.message; })";
  html += "    .finally(function(){ btn.disabled = false; });";
  html += "}";
  html += "function geminiPromptSifirla(){";
  html += "  var varsayilan = 'Bu kamera karesi bir ekrandan veya belgeden cekildi. G\\u00d6REV: 1) Ekrandaki yaziyi veya soruyu dikkatle oku. 2) Eger bir soru varsa dogrudan cevabini ver. 3) Eger yazi veya bilgi varsa kisaca ne yazdigini soyle. 4) Eger goruntu cok karanlik, parlamis veya okunmuyorsa kesinlikle \\\"Okunamadi: Goruntu net degil\\\" yaz. FORMAT: OLED ekrana basilacak, en fazla 12-15 kelimelik TEK kisa cumle ile Turkce yanit ver.';";
  html += "  document.getElementById('f_gemini_prompt').value = varsayilan;";
  html += "}";
  html += "function cihazYenidenBaslat(){";
  html += "  if(confirm('Cihazi yeniden baslatmak istediginize emin misiniz?')){";
  html += "    fetch('/restart', {method:'POST'}).then(function(){";
  html += "      alert('Cihaz yeniden baslatiliyor... Sayfa 6 saniye sonra otomatik yenilenecektir.');";
  html += "      setTimeout(function(){ window.location.reload(); }, 6000);";
  html += "    }).catch(function(){";
  html += "      setTimeout(function(){ window.location.reload(); }, 6000);";
  html += "    });";
  html += "  }";
  html += "}";
  html += "function logKopyala(){";
  html += "  var btn = document.getElementById('btnCopyLog');";
  html += "  var eski = btn.innerHTML;";
  html += "  btn.disabled = true; btn.innerHTML = '&#9203; Aliniyor...';";
  html += "  fetch('/log/metin').then(function(r){ return r.text(); }).then(function(txt){";
  html += "    function bitti(){ btn.innerHTML = '&#10003; Kopyalandi!'; setTimeout(function(){ btn.innerHTML = eski; btn.disabled = false; }, 2000); }";
  html += "    if(navigator.clipboard && window.isSecureContext){";
  html += "      navigator.clipboard.writeText(txt).then(bitti).catch(function(){ kopyalaFallback(txt, bitti); });";
  html += "    } else { kopyalaFallback(txt, bitti); }";
  html += "  }).catch(function(e){ alert('Log alinamadi: ' + e.message); btn.innerHTML = eski; btn.disabled = false; });";
  html += "}";
  html += "function kopyalaFallback(text, cb){";
  html += "  var ta = document.createElement('textarea'); ta.value = text; ta.style.position = 'fixed'; ta.style.top = '-9999px';";
  html += "  document.body.appendChild(ta); ta.select(); document.execCommand('copy'); document.body.removeChild(ta); cb();";
  html += "}";
  html += "</script>";

  html += "</body></html>";
  return html;
}

void handleAyarlar() {
  server.send(200, "text/html", buildAyarlarHtml(""));
}

void handleAyarlarPost() {
  sensor_t *sensor = esp_camera_sensor_get();
  bool cozunurlukDegisti = false;

  if (sensor) {
    if (server.hasArg("framesize")) {
      int kod = server.arg("framesize").toInt();
      framesize_t fs = framesizeAdindanBul(kod);
      if (fs != sensor->status.framesize) cozunurlukDegisti = true;
      sensor->set_framesize(sensor, fs);
      uint32_t pF = psram_var ? (ESP.getFreePsram() / 1024) : 0;
      uint32_t hF = ESP.getFreeHeap() / 1024;
      logEkle("Cozunurluk -> " + framesizeAdiYaz(fs) + " [BosHeap=" + String(hF) + "KB, BosPSRAM=" + String(pF) + "KB]");
    }
    if (server.hasArg("quality")) {
      int q = constrain(server.arg("quality").toInt(), 10, 63);
      sensor->set_quality(sensor, q);
    }
    if (server.hasArg("brightness")) {
      int v = constrain(server.arg("brightness").toInt(), -2, 2);
      sensor->set_brightness(sensor, v);
    }
    if (server.hasArg("contrast")) {
      int v = constrain(server.arg("contrast").toInt(), -2, 2);
      sensor->set_contrast(sensor, v);
    }
    if (server.hasArg("saturation")) {
      int v = constrain(server.arg("saturation").toInt(), -2, 2);
      sensor->set_saturation(sensor, v);
    }
    if (server.hasArg("special_effect")) {
      int v = constrain(server.arg("special_effect").toInt(), 0, 6);
      sensor->set_special_effect(sensor, v);
    }
    if (server.hasArg("awb")) {
      int v = server.arg("awb").toInt();
      sensor->set_whitebal(sensor, v);
    }
    if (server.hasArg("ae_level")) {
      int v = constrain(server.arg("ae_level").toInt(), -2, 2);
      sensor->set_ae_level(sensor, v);
    }
    if (server.hasArg("gainceiling")) {
      int v = constrain(server.arg("gainceiling").toInt(), 0, 6);
      sensor->set_gainceiling(sensor, (gainceiling_t)v);
    }
    logEkle("Kamera ayarlari kaydedildi [BosHeap=" + String(ESP.getFreeHeap()/1024) + "KB, BosPSRAM=" + String((psram_var ? ESP.getFreePsram() : 0)/1024) + "KB]");
    ayarlariKaydet();
  }

  server.send(200, "text/plain", "OK");
}

void handleAyarlarOku() {
  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor) {
    framesize_t hedefFs = psram_var ? FRAMESIZE_SVGA : FRAMESIZE_VGA;
    sensor->set_framesize(sensor, hedefFs);
    sensor->set_quality(sensor, 10); // 10: SVGA için maksimum netlik ve güvenli bellek sınırı
    sensor->set_brightness(sensor, -1); // Ekran arka aydınlatmasının patlamasını önler
    sensor->set_contrast(sensor, 2); // Maksimum kontrast: siyah harfler, beyaz zemin
    sensor->set_saturation(sensor, -2);
    sensor->set_special_effect(sensor, 2); // Gri tonlama: ekran alt-piksel renk bozulmalarını siler
    sensor->set_whitebal(sensor, 1);
    sensor->set_ae_level(sensor, -1); // Ekran parlamasını keser
    sensor->set_gainceiling(sensor, (gainceiling_t)2);
    sensor->set_bpc(sensor, 1);
    sensor->set_wpc(sensor, 1);
    sensor->set_lenc(sensor, 1); // Köşe netliği
    logEkle("Ekrandan Yazi Okuma Modu uygulandi: " + framesizeAdiYaz(hedefFs) + ", kalite=10, kontrast=2, gri tonlama, parlama kesme");
    ayarlariKaydet();
    server.send(200, "application/json", sensorDurumJson());
  } else {
    server.send(500, "application/json", "{}");
  }
}

void handleAyarlarGemini() {
  if (server.hasArg("gemini_key")) {
    gemini_api_key = server.arg("gemini_key");
    gemini_api_key.trim();
  }
  if (server.hasArg("gemini_prompt")) {
    gemini_prompt = server.arg("gemini_prompt");
    gemini_prompt.trim();
  }
  geminiAyarlariKaydet(gemini_api_key, gemini_prompt);
  server.send(200, "text/plain", "OK");
}

// ================= WIFI AYARLARI =================
void wifiAyarlariKaydet(String ssid, String pass) {
  wifiNvs.begin("wificfg", false);
  wifiNvs.putString("ssid", ssid);
  wifiNvs.putString("pass", pass);
  wifiNvs.end();
  logEkle("WiFi ayarlari kaydedildi. SSID: " + ssid);
}

void wifiAyarlariYukle() {
  wifiNvs.begin("wificfg", true);
  bool varMi = wifiNvs.isKey("ssid");
  if (varMi) {
    sta_ssid = wifiNvs.getString("ssid", sta_ssid);
    sta_password = wifiNvs.getString("pass", sta_password);
    logEkle("Kayitli WiFi ayari yuklendi. SSID: " + sta_ssid);
  } else {
    logEkle("NVS'te kayitli WiFi yok, varsayilan SSID kullaniliyor: " + sta_ssid);
  }
  wifiNvs.end();
}

String buildWifiHtml(String durumMesaji) {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>WiFi Ayarları</title>";
  html += "<style>*{box-sizing:border-box;margin:0;padding:0;}";
  html += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Arial,sans-serif;background:#f1f5f9;color:#0f172a;padding:20px 16px 32px;}";
  html += ".box{max-width:520px;margin:0 auto;display:flex;flex-direction:column;gap:12px;}";
  html += ".card{background:#fff;border:1px solid #e2e8f0;border-radius:16px;box-shadow:0 1px 3px rgba(0,0,0,0.07),0 4px 14px rgba(0,0,0,0.05);padding:18px;}";
  html += "a.geri{color:#2563eb;text-decoration:none;font-size:13px;font-weight:500;display:inline-flex;align-items:center;gap:4px;margin-bottom:4px;}";
  html += "a.geri:hover{text-decoration:underline;}";
  html += ".page-title{font-size:1.2rem;font-weight:700;color:#0f172a;margin-top:4px;}";
  html += "label{display:block;font-size:13px;color:#334155;font-weight:500;margin:14px 0 5px;}";
  html += "input[type='text'],input[type='password']{width:100%;padding:11px 12px;border-radius:10px;border:1.5px solid #cbd5e1;background:#f8fafc;color:#0f172a;font-size:14px;outline:none;transition:border-color .2s;}";
  html += "input[type='text']:focus,input[type='password']:focus{border-color:#2563eb;background:#fff;}";
  html += "input[type='submit']{width:100%;padding:12px 16px;border-radius:10px;border:none;font-size:14px;font-weight:600;cursor:pointer;margin-top:14px;background:#2563eb;color:#fff;transition:background .18s;}";
  html += "input[type='submit']:hover{background:#1d4ed8;}";
  html += "#btnTara{width:100%;padding:11px 16px;border-radius:10px;border:1.5px solid #cbd5e1;font-size:14px;font-weight:600;cursor:pointer;margin-top:4px;background:#f8fafc;color:#334155;transition:background .18s,border-color .18s;}";
  html += "#btnTara:hover{background:#eff6ff;border-color:#bfdbfe;color:#2563eb;}";
  html += ".ag-listesi{margin-top:10px;background:#f8fafc;border:1px solid #e2e8f0;border-radius:12px;max-height:220px;overflow:auto;}";
  html += ".ag-item{padding:11px 14px;border-bottom:1px solid #e2e8f0;cursor:pointer;font-size:14px;display:flex;justify-content:space-between;align-items:center;transition:background .15s;}";
  html += ".ag-item:last-child{border-bottom:none;}";
  html += ".ag-item:hover{background:#eff6ff;}";
  html += ".sinyal{color:#2563eb;font-size:12px;font-weight:600;background:#eff6ff;padding:2px 8px;border-radius:999px;border:1px solid #bfdbfe;}";
  html += ".durum-msg{background:#f0fdf4;border:1px solid #bbf7d0;color:#15803d;padding:10px 14px;border-radius:10px;font-size:13px;font-weight:500;}";
  html += ".not{font-size:12px;color:#64748b;margin-top:8px;line-height:1.55;}";
  html += ".ssid-pill{display:inline-block;background:#eff6ff;color:#2563eb;border:1px solid #bfdbfe;border-radius:999px;padding:2px 10px;font-size:12px;font-weight:600;}";
  html += "</style></head><body><div class='box'>";
  html += "<div class='card'><a class='geri' href='/'>&#8592; Ana sayfaya dön</a><div class='page-title'>&#128246; WiFi Ayarları</div></div>";
  html += "<div class='card'>";
  if (durumMesaji.length() > 0) {
    html += "<div class='durum-msg'>&#10003; " + htmlEscape(durumMesaji) + "</div>";
  }
  html += "<div class='not'>Şu anki kayıtlı ağ: <span class='ssid-pill'>" + htmlEscape(sta_ssid) + "</span></div>";
  html += "<button id='btnTara' type='button'>&#128268; Ağları Tara</button><div class='ag-listesi' id='agListesi'></div>";
  html += "<form id='wifiForm' method='POST' action='/wifi'>";
  html += "<label>Ağ Adı (SSID)</label><input type='text' name='ssid' id='f_ssid' value='" + htmlEscape(sta_ssid) + "' required>";
  html += "<label>Şifre</label><input type='password' name='pass' id='f_pass' placeholder='Yeni şifreyi gir'>";
  html += "<input type='submit' value='&#128190; Kaydet ve Yeniden Başlat'>";
  html += "</form><div class='not'>Kaydedince cihaz otomatik olarak yeniden başlar ve yeni ağa bağlanmayı dener. Bağlanamazsa eski AP moduna (192.168.4.1) döner.</div>";
  html += "</div><script>";
  html += "document.getElementById('btnTara').addEventListener('click', function(){var liste = document.getElementById('agListesi');liste.innerHTML='<div class=\"ag-item\">&#128268; Taranıyor, lütfen bekleyin...</div>';fetch('/wifi/tara').then(function(r){return r.json();}).then(function(agliste){liste.innerHTML='';if(agliste.length===0){liste.innerHTML='<div class=\"ag-item\">Hiçbir ağ bulunamadı.</div>';return;}agliste.forEach(function(ag){var div=document.createElement('div');div.className='ag-item';div.innerHTML='<span>'+ag.ssid+'</span><span class=\"sinyal\">'+ag.rssi+' dBm</span>';div.addEventListener('click', function(){document.getElementById('f_ssid').value=ag.ssid;document.getElementById('f_pass').focus();});liste.appendChild(div);});}).catch(function(){liste.innerHTML='<div class=\"ag-item\">Tarama başarısız.</div>';});});";
  html += "</script></div></body></html>";
  return html;
}


void handleWifi() {
  server.send(200, "text/html", buildWifiHtml(""));
}

void handleWifiTara() {
  logEkle("WiFi ag taramasi basladi...");
  int agSayisi = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < agSayisi; i++) {
    if (i > 0) json += ",";
    String ssidTemiz = WiFi.SSID(i);
    ssidTemiz.replace("\"", "'");
    json += "{\"ssid\":\"" + ssidTemiz + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  json += "]";
  WiFi.scanDelete();
  logEkle("WiFi taramasi bitti, " + String(agSayisi) + " ag bulundu.");
  server.send(200, "application/json", json);
}

void handleWifiPost() {
  if (server.hasArg("ssid")) {
    String yeniSsid = server.arg("ssid");
    String yeniPass = server.hasArg("pass") ? server.arg("pass") : "";
    yeniSsid.trim();

    if (yeniSsid.length() > 0) {
      if (yeniPass.length() == 0) {
        yeniPass = sta_password;
      }
      wifiAyarlariKaydet(yeniSsid, yeniPass);

      String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
      html += "<meta http-equiv='refresh' content='4;url=/'>";
      html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
      html += "<style>*{box-sizing:border-box;margin:0;padding:0;}body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Arial,sans-serif;background:#f1f5f9;color:#0f172a;padding:60px 20px;text-align:center;}";
      html += ".card{max-width:380px;margin:0 auto;background:#fff;border:1px solid #e2e8f0;border-radius:16px;padding:36px 28px;box-shadow:0 4px 20px rgba(0,0,0,0.07);}";
      html += ".icon{font-size:40px;margin-bottom:14px;}";
      html += "h3{font-size:1.2rem;font-weight:700;color:#0f172a;margin-bottom:10px;}";
      html += "p{font-size:13.5px;color:#64748b;line-height:1.6;margin-top:8px;}";
      html += ".ssid{font-weight:600;color:#2563eb;}";
      html += ".progress{margin-top:20px;height:4px;background:#e2e8f0;border-radius:999px;overflow:hidden;}";
      html += ".bar{height:100%;width:0;background:#2563eb;border-radius:999px;animation:fill 4s linear forwards;}";
      html += "@keyframes fill{to{width:100%}}</style></head>";
      html += "<body><div class='card'><div class='icon'>&#128260;</div>";
      html += "<h3>Ayarlar Kaydedildi!</h3>";
      html += "<p>Cihaz yeniden başlatılıyor ve <span class='ssid'>" + yeniSsid + "</span> ağına bağlanmayı deniyor...</p>";
      html += "<p>4 saniye sonra ana sayfaya yönlendirileceksiniz (IP değişebilir).</p>";
      html += "<div class='progress'><div class='bar'></div></div>";
      html += "</div></body></html>";
      server.send(200, "text/html", html);

      logEkle("Yeni WiFi ayarlariyla yeniden baslatiliyor...");
      delay(1500);
      ESP.restart();
      return;
    }
  }
  server.send(400, "text/plain", "SSID gerekli.");
}

void setup() {
  setCpuFrequencyMhz(240); // İşlemciyi maksimum 240 MHz hızına ayarla

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  logEkle("Sistem baslatiliyor (CPU: " + String(getCpuFrequencyMhz()) + " MHz)...");

  Wire.begin(I2C_SDA, I2C_SCL);
  logEkle("Wire.begin() cagrildi (SDA=" + String(I2C_SDA) + ", SCL=" + String(I2C_SCL) + ")");

  byte adresDeneme[] = {0x3C, 0x3D, 0x7A, 0x7B, 0x78, 0x79};
  bool bulundu = false;

  for (int i = 0; i < 6; i++) {
    byte adres = adresDeneme[i];
    if (oledBulunduMu(adres)) {
      logEkle("OLED I2C adresi bulundu: 0x" + String(adres, HEX));
      bool basladi = display.begin(SSD1306_SWITCHCAPVCC, adres);
      if (basladi) {
        oled_aktif = true;
        logEkle("OLED baslatildi (adres 0x" + String(adres, HEX) + ")");
        bulundu = true;
        break;
      } else {
        logEkle("OLED buldu ama baslatilamadi: 0x" + String(adres, HEX));
      }
    }
  }

  if (!bulundu) {
    logEkle("OLED HICBIR beklenen adreste bulunamadi! I2C taramasi yapiliyor...");
    i2cTara();
  }

  if (oled_aktif) {
    display.clearDisplay();
    display.fillScreen(SSD1306_WHITE);
    display.display();
    delay(300);

    display.clearDisplay();
    display.fillScreen(SSD1306_BLACK);
    display.display();
    delay(200);

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(10, 10);
    display.println("TEST");
    display.setTextSize(1);
    display.setCursor(0, 40);
    display.println("OLED HAZIR");
    display.display();
    delay(1000);

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Sistem Basliyor...");
    display.println("OLED TEST OK");
    display.display();
    delay(500);
  }

  // 3. Kamera Başlatma
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000; // Stabil 10 MHz saat hızı
  config.pixel_format = PIXFORMAT_JPEG; // Zorunlu JPEG formatı

  psram_var = psramFound();

  if (psram_var) {
    config.frame_size = FRAMESIZE_UXGA; // 1600x1200 maksimum donanim tamponu (SXGA, XGA, SVGA hepsi sigar, DMA tasmasi onlenir)
    config.jpeg_quality = 10;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
    logEkle("PSRAM bulundu (" + String(ESP.getPsramSize() / 1024) + " KB), UXGA tamponu tahsis edildi.");
  } else {
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    logEkle("PSRAM bulunamadi, fb_count=1 kullaniliyor.");
  }

  esp_err_t kamera_sonuc = esp_camera_init(&config);
  if (kamera_sonuc != ESP_OK) {
    char buf[48];
    snprintf(buf, sizeof(buf), "Kamera baslatma HATASI: 0x%x", kamera_sonuc);
    logEkle(String(buf));
    ekranaYaz("HATA!", "Kamera Basarisiz");
    delay(2000);
  } else {
    logEkle("Kamera basariyla baslatildi (XCLK=" + String(config.xclk_freq_hz / 1000000) + "MHz).");

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
      sensor->set_brightness(sensor, 0);
      sensor->set_contrast(sensor, 0);
      sensor->set_saturation(sensor, 0);
      sensor->set_gain_ctrl(sensor, 1);
      sensor->set_exposure_ctrl(sensor, 1);
      sensor->set_whitebal(sensor, 1);
      sensor->set_awb_gain(sensor, 1);
      sensor->set_aec2(sensor, 1);
      sensor->set_ae_level(sensor, 0);
      sensor->set_quality(sensor, 12);
      sensor->set_framesize(sensor, FRAMESIZE_QVGA);

      // ONEMLI: OV2640 klonlarinda yuksek kalitede gorulen nokta/gurultu/
      // bant seklindeki bozulmalari azaltir. Cogu ornekte kapali gelir.
      sensor->set_bpc(sensor, 1);     // black pixel correction
      sensor->set_wpc(sensor, 1);     // white pixel correction
      sensor->set_lenc(sensor, 1);    // lens correction
      sensor->set_dcw(sensor, 1);     // downsize enable (bazi cozunurluklerde stabilite icin)

      logEkle("Varsayilan kamera ayarlari + BPC/WPC/LENC duzeltmeleri uygulandi.");
    }

    ayarlariYukle();
  }

  wifiAyarlariYukle();
  geminiAyarlariYukle();

  WiFi.setSleep(false);
  logEkle("WiFi baglaniliyor: " + sta_ssid);
  ekranaYaz("WiFi Baglaniyor", sta_ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(sta_ssid.c_str(), sta_password.c_str());

  int deneme = 0;
  bool led_durum = false;
  while (WiFi.status() != WL_CONNECTED && deneme < 30) {
    led_durum = !led_durum;
    digitalWrite(FLASH_LED_PIN, led_durum ? HIGH : LOW);
    delay(200);
    deneme++;
  }

  digitalWrite(FLASH_LED_PIN, LOW);

  String aktif_ip = "";

  if (WiFi.status() == WL_CONNECTED) {
    aktif_ip = WiFi.localIP().toString();
    sonAktifIp = aktif_ip;
    logEkle("WiFi baglandi! IP: " + aktif_ip);
    configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org", "time.google.com");
    logEkle("NTP saat servisi baslatildi (UTC+3).");
    ekranaYaz("WIFI BAGLANDI", aktif_ip);
  } else {
    logEkle("WiFi baglanamadi, AP moduna geciliyor.");
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid, ap_password);
    aktif_ip = WiFi.softAPIP().toString();
    sonAktifIp = aktif_ip;
    logEkle("AP baslatildi. IP: " + aktif_ip);
    ekranaYaz("AP MODU AKTIF", "IP: " + aktif_ip + "\nSifre: " + String(ap_password) + "\nAg: " + String(ap_ssid));
  }

  ArduinoOTA.setHostname("esp32-cam-cihaz");
  ArduinoOTA.setPassword(ota_password);
  ArduinoOTA.onStart([]() {
    logEkle("OTA guncelleme basladi.");
    server.close();
    esp_camera_deinit();
    digitalWrite(FLASH_LED_PIN, LOW);
    ekranaYaz("OTA", "Yukleniyor...");
  });
  ArduinoOTA.begin();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/", HTTP_POST, handleRootPost);
  server.on("/capture", HTTP_GET, handleCapture);
  server.on("/stream", HTTP_GET, handleStream);
  server.on("/gonder", HTTP_POST, handleGonder);
  server.on("/ayarlar", HTTP_GET, handleAyarlar);
  server.on("/ayarlar", HTTP_POST, handleAyarlarPost);
  server.on("/ayarlar/oku", HTTP_POST, handleAyarlarOku);
  server.on("/ayarlar/gemini", HTTP_POST, handleAyarlarGemini);
  server.on("/ayarlar/durum", HTTP_GET, handleAyarlarDurum);
  server.on("/wifi", HTTP_GET, handleWifi);
  server.on("/wifi", HTTP_POST, handleWifiPost);
  server.on("/wifi/tara", HTTP_GET, handleWifiTara);
  server.on("/update", HTTP_GET, handleUpdate);
  server.on("/log", HTTP_GET, handleLog);
  server.on("/log/metin", HTTP_GET, handleLogMetin);
  server.on("/sistem/durum", HTTP_GET, handleSistemDurum);
  server.on("/restart", HTTP_POST, handleRestart);
  server.on("/update", HTTP_POST, []() {
    server.send(200, "text/plain", (Update.hasError()) ? "HATA" : "TAMAM");
    delay(1000);
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      logEkle("Web OTA yuklemesi basladi: " + upload.filename);
      server.close();
      esp_camera_deinit();
      digitalWrite(FLASH_LED_PIN, LOW);
      ekranaYaz("Web OTA", "Yukleniyor...");
      Update.begin(UPDATE_SIZE_UNKNOWN);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      Update.write(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
      logEkle("Web OTA yuklemesi bitti, " + String(upload.totalSize) + " byte.");
      Update.end(true);
    }
  });

  server.begin();
  logEkle("Web sunucu baslatildi, / , /capture , /stream , /ayarlar , /wifi , /log , /update hazir.");
}

void oledSaatGuncelle() {
  if (!oled_aktif) return;

  // Ekranda gelen mesaj varsa 20 saniye göster, sonra normal IP+Saat ekranına dön
  if (oledMesajModunda) {
    if (millis() - oledMesajZamani > 20000) {
      oledMesajModunda = false;
      if (sonAktifIp.length() > 0) {
        ekranaYaz(WiFi.status() == WL_CONNECTED ? "WIFI BAGLANDI" : "AP MODU AKTIF", sonAktifIp);
      }
    }
    return;
  }

  // Boştayken 1 saniyede bir IP ve canlı saati güncelle
  if (sonAktifIp.length() > 0 && (millis() - sonSaatGuncelleme >= 1000)) {
    sonSaatGuncelleme = millis();
    ekranaYaz(WiFi.status() == WL_CONNECTED ? "WIFI BAGLANDI" : "AP MODU AKTIF", sonAktifIp);
  }
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  oledSaatGuncelle();
}
