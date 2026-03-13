#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <time.h>
#include "mbedtls/md.h"

// ─── STRUCTS GLOBAIS ───────────────────────────────────────────────
// Devem vir antes de qualquer função para que o Arduino IDE não
// quebre ao gerar os protótipos automáticos das funções que os usam.

struct UltimoSorteio {
  int      numeros[6];
  uint32_t semente;
  char     hash[65];      // 32 bytes hex + null
  char     timestamp[32];
  int      total;
  float    tempCPU;
};

#define MAX_TASKS 20

struct CoreStats {
  float    uso0;   // % Core 0
  float    uso1;   // % Core 1
  uint32_t freq;   // MHz
};

// ─── VARIÁVEIS GLOBAIS ─────────────────────────────────────────────

// --- CONFIGURAÇÕES DE REDE ---
const char* ssid      = "SSID";
const char* password  = "SENHA";
const char* serverUrl = "http://192.168.x.x:1880/caos";

// NTP — fuso America/Sao_Paulo = UTC-3
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset = -3 * 3600;
const int   dstOffset = 0;

unsigned long ultimaExecucao = 0;
const long    intervalo      = 60000; // 1 minuto

WebServer server(80);

static portMUX_TYPE entropiaMux   = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t   entropia_viva = 0;

UltimoSorteio ultimo;
bool          temDados = false;

// Snapshots para cálculo delta de uso de CPU
static uint32_t     snap_runtime[MAX_TASKS];
static TaskHandle_t snap_handles[MAX_TASKS];
static UBaseType_t  snap_count     = 0;
static uint32_t     snap_lastTotal = 0;

// ─── SENSOR DE TEMPERATURA ─────────────────────────────────────────
// "temprature_sens_read" — typo intencional, é o nome oficial do SDK
#ifdef __cplusplus
extern "C" {
#endif
uint8_t temprature_sens_read();
#ifdef __cplusplus
}
#endif

float getTempCPU() {
  return (temprature_sens_read() - 32) / 1.8f;
}

// ─── NTP ───────────────────────────────────────────────────────────
String getTimestamp() {
  struct tm t;
  if (!getLocalTime(&t)) return "sem hora NTP";
  char buf[20];
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &t);
  return String(buf);
}

// ─── ENTROPIA (CORE 0) ─────────────────────────────────────────────
void taskGeradoraDeCaos(void* pvParameters) {
  for (;;) {
    uint32_t novo = esp_random() ^ (uint32_t)micros();
    portENTER_CRITICAL(&entropiaMux);
    entropia_viva ^= novo ^ (uint32_t)(tan((double)entropia_viva) * 1e6);
    portEXIT_CRITICAL(&entropiaMux);
    vTaskDelay(1);
  }
}

uint32_t capturarEntropia() {
  uint32_t val;
  portENTER_CRITICAL(&entropiaMux);
  val = entropia_viva;
  portEXIT_CRITICAL(&entropiaMux);
  return val;
}

// ─── USO DOS NÚCLEOS via FreeRTOS runtime stats ────────────────────
CoreStats getCoreStats() {
  CoreStats cs;
  cs.uso0 = 0;
  cs.uso1 = 0;
  cs.freq = getCpuFrequencyMhz();

  TaskStatus_t tasks[MAX_TASKS];
  uint32_t     totalRuntime = 0;
  UBaseType_t  n = uxTaskGetSystemState(tasks, MAX_TASKS, &totalRuntime);

  if (n == 0 || totalRuntime == 0) return cs;

  uint32_t deltaTotal = totalRuntime - snap_lastTotal;
  snap_lastTotal = totalRuntime;
  if (deltaTotal == 0) return cs;

  float delta0 = 0, delta1 = 0;

  for (UBaseType_t i = 0; i < n; i++) {
    uint32_t prevRuntime = 0;
    for (UBaseType_t j = 0; j < snap_count; j++) {
      if (snap_handles[j] == tasks[i].xHandle) {
        prevRuntime = snap_runtime[j];
        break;
      }
    }
    float delta = (float)(tasks[i].ulRunTimeCounter - prevRuntime);
    if (tasks[i].xCoreID == 0) delta0 += delta;
    else                        delta1 += delta;
  }

  snap_count = (n < MAX_TASKS) ? n : MAX_TASKS;
  for (UBaseType_t i = 0; i < snap_count; i++) {
    snap_handles[i] = tasks[i].xHandle;
    snap_runtime[i] = tasks[i].ulRunTimeCounter;
  }

  float halfTotal = (float)deltaTotal / 2.0f;
  cs.uso0 = constrain((delta0 / halfTotal) * 100.0f, 0.0f, 100.0f);
  cs.uso1 = constrain((delta1 / halfTotal) * 100.0f, 0.0f, 100.0f);
  return cs;
}

// ─── PÁGINA HTML ───────────────────────────────────────────────────
void handleRoot() {
  String html = R"rawhtml(
<!DOCTYPE html><html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta http-equiv="refresh" content="65">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 CaosForge</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:#0d0d0d;color:#e0e0e0;font-family:'Courier New',monospace;
       display:flex;flex-direction:column;align-items:center;padding:30px 16px;min-height:100vh}
  h1{color:#00e5ff;font-size:1.4rem;letter-spacing:4px;margin-bottom:4px;text-transform:uppercase}
  .sub{color:#555;font-size:.75rem;margin-bottom:32px;letter-spacing:2px}
  .card{background:#111;border:1px solid #1e1e1e;border-radius:8px;
        padding:24px 28px;width:100%;max-width:540px;margin-bottom:16px}
  .label{color:#555;font-size:.7rem;letter-spacing:2px;text-transform:uppercase;margin-bottom:8px}
  .numbers{display:flex;gap:10px;flex-wrap:wrap}
  .ball{background:#00e5ff;color:#000;font-weight:bold;font-size:1.1rem;
        width:46px;height:46px;border-radius:50%;display:flex;align-items:center;
        justify-content:center;flex-shrink:0}
  .hash{color:#00e5ff;font-size:.72rem;word-break:break-all;line-height:1.7}
  .row{display:flex;justify-content:space-between;margin-bottom:10px}
  .val{color:#fff;font-size:.9rem}
  .muted{color:#444;font-size:.7rem}
  .dot{display:inline-block;width:8px;height:8px;border-radius:50%;
       background:#00e5ff;margin-right:6px;animation:blink 1.2s infinite}
  @keyframes blink{0%,100%{opacity:1}50%{opacity:.2}}
  .footer{color:#333;font-size:.65rem;margin-top:24px;text-align:center;line-height:1.8}
  .nodata{color:#444;text-align:center;padding:20px;font-size:.85rem}
  .temp-bar-bg{background:#1e1e1e;border-radius:4px;height:6px;width:100%;margin-top:6px}
  .temp-bar{height:6px;border-radius:4px;transition:width .5s}
  .temp-val{font-size:1.6rem;font-weight:bold}
  .cpu-row{display:flex;align-items:center;gap:10px;margin-bottom:10px}
  .cpu-label{color:#555;font-size:.7rem;width:56px;flex-shrink:0}
  .cpu-bar-bg{flex:1;background:#1e1e1e;border-radius:4px;height:8px}
  .cpu-bar{height:8px;border-radius:4px;transition:width .5s}
  .cpu-pct{color:#fff;font-size:.8rem;width:42px;text-align:right;flex-shrink:0}
</style>
</head>
<body>
<h1>&#x26A1; CaosForge</h1>
<p class="sub">ESP32 &#xB7; HMAC-SHA256 &#xB7; LIVE</p>
)rawhtml";

  if (!temDados) {
    html += "<div class='card'><p class='nodata'>Aguardando primeiro sorteio...</p></div>";
  } else {
    // Sorteio
    html += "<div class='card'><div class='label'>&#xDAltimo Sorteio</div><div class='numbers'>";
    for (int i = 0; i < 6; i++)
      html += "<div class='ball'>" + String(ultimo.numeros[i]) + "</div>";
    html += "</div></div>";

    // Metadados
    html += "<div class='card'>";
    html += "<div class='row'><span class='muted'>Timestamp</span><span class='val'>" + String(ultimo.timestamp) + "</span></div>";
    html += "<div class='row'><span class='muted'>Semente</span><span class='val'>"   + String(ultimo.semente)   + "</span></div>";
    html += "<div class='row'><span class='muted'>Sorteios</span><span class='val'>"  + String(ultimo.total)     + "</span></div>";
    html += "</div>";

    // Hash
    html += "<div class='card'><div class='label'>HMAC-SHA256</div>";
    html += "<div class='hash'>" + String(ultimo.hash) + "</div></div>";

    // Temperatura
    float  t   = ultimo.tempCPU;
    int    pct = constrain((int)((t - 30) / 60.0f * 100), 0, 100);
    String corT = t < 55 ? "#00e5ff" : t < 70 ? "#ffb300" : "#f44336";
    html += "<div class='card'><div class='label'>Temperatura do Die (CPU)</div>";
    html += "<div class='row' style='align-items:baseline'>";
    html += "<span class='temp-val' style='color:" + corT + "'>" + String(t, 1) + "&#xB0;C</span>";
    html += "<span class='muted' style='margin-left:10px'>die interno &#xB7; &#xB15;5&#x2013;10&#xB0;C</span>";
    html += "</div>";
    html += "<div class='temp-bar-bg'><div class='temp-bar' style='width:" + String(pct) + "%;background:" + corT + "'></div></div>";
    html += "</div>";
  }

  // Núcleos — sempre visível
  CoreStats cs  = getCoreStats();
  String    cor0 = cs.uso0 < 60 ? "#00e5ff" : cs.uso0 < 85 ? "#ffb300" : "#f44336";
  String    cor1 = cs.uso1 < 60 ? "#00e5ff" : cs.uso1 < 85 ? "#ffb300" : "#f44336";
  html += "<div class='card'><div class='label'>N&#xFA;cleos &#xB7; " + String(cs.freq) + " MHz</div>";
  html += "<div class='cpu-row'><span class='cpu-label'>Core 0</span>";
  html += "<div class='cpu-bar-bg'><div class='cpu-bar' style='width:" + String((int)cs.uso0) + "%;background:" + cor0 + "'></div></div>";
  html += "<span class='cpu-pct' style='color:" + cor0 + "'>" + String(cs.uso0, 1) + "%</span></div>";
  html += "<div class='cpu-row' style='margin-bottom:0'><span class='cpu-label'>Core 1</span>";
  html += "<div class='cpu-bar-bg'><div class='cpu-bar' style='width:" + String((int)cs.uso1) + "%;background:" + cor1 + "'></div></div>";
  html += "<span class='cpu-pct' style='color:" + cor1 + "'>" + String(cs.uso1, 1) + "%</span></div>";
  html += "<div style='margin-top:10px;color:#333;font-size:.65rem'>Core 0 = Forja de Caos &nbsp;&#xB7;&nbsp; Core 1 = Or&#xE1;culo + WebServer</div>";
  html += "</div>";

  // Sistema
  html += "<div class='card'><div class='label'>Sistema</div>";
  html += "<div class='row'><span class='muted'>IP</span><span class='val'>"         + WiFi.localIP().toString()         + "</span></div>";
  html += "<div class='row'><span class='muted'>Uptime</span><span class='val'>"     + String(millis() / 1000) + "s"     + "</span></div>";
  html += "<div class='row'><span class='muted'>RSSI WiFi</span><span class='val'>"  + String(WiFi.RSSI())  + " dBm"    + "</span></div>";
  html += "<div class='row'><span class='muted'>Heap livre</span><span class='val'>" + String(ESP.getFreeHeap()/1024) + " KB" + "</span></div>";
  html += "<div class='row' style='margin-bottom:0'><span class='muted'>Status</span><span class='val'><span class='dot'></span>Online</span></div>";
  html += "</div>";

  html += "<p class='footer'>Atualiza a cada 65s &#xB7; github.com/deletrr/Esp32CaosForge</p>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

// ─── ENDPOINT JSON ─────────────────────────────────────────────────
void handleJson() {
  if (!temDados) {
    server.send(200, "application/json", "{\"status\":\"aguardando\"}");
    return;
  }
  CoreStats cs = getCoreStats();
  String json = "{\"semente\":"      + String(ultimo.semente)   +
                ",\"hash\":\""       + String(ultimo.hash)       + "\"" +
                ",\"timestamp\":\""  + String(ultimo.timestamp)  + "\"" +
                ",\"total\":"        + String(ultimo.total)      +
                ",\"temp_cpu\":"     + String(ultimo.tempCPU, 1) +
                ",\"cpu_freq_mhz\":" + String(cs.freq)           +
                ",\"core0_pct\":"    + String(cs.uso0, 1)        +
                ",\"core1_pct\":"    + String(cs.uso1, 1)        +
                ",\"numeros\":["     +
                String(ultimo.numeros[0]) + "," + String(ultimo.numeros[1]) + "," +
                String(ultimo.numeros[2]) + "," + String(ultimo.numeros[3]) + "," +
                String(ultimo.numeros[4]) + "," + String(ultimo.numeros[5]) + "]}";
  server.send(200, "application/json", json);
}

// ─── SORTEIO + ENVIO ───────────────────────────────────────────────
void realizarSorteioEEnviar() {
  if (WiFi.status() != WL_CONNECTED) { WiFi.begin(ssid, password); return; }

  uint32_t semente_capturada = capturarEntropia() ^ (uint32_t)micros() ^ esp_random();
  const char* chave_secreta  = "SALT";

  uint8_t hash_final[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)chave_secreta, strlen(chave_secreta));
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)&semente_capturada, sizeof(semente_capturada));
  mbedtls_md_hmac_finish(&ctx, hash_final);
  mbedtls_md_free(&ctx);

  int numeros[6], encontrados = 0, byte_idx = 0;
  while (encontrados < 6) {
    if (byte_idx >= 32) {
      mbedtls_md_init(&ctx);
      mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
      mbedtls_md_hmac_starts(&ctx, (const unsigned char*)chave_secreta, strlen(chave_secreta));
      mbedtls_md_hmac_update(&ctx, hash_final, 32);
      mbedtls_md_hmac_finish(&ctx, hash_final);
      mbedtls_md_free(&ctx);
      byte_idx = 0;
    }
    uint8_t b = hash_final[byte_idx++];
    if (b >= 240) continue;
    int num = (b % 60) + 1;
    bool rep = false;
    for (int i = 0; i < encontrados; i++) if (numeros[i] == num) { rep = true; break; }
    if (!rep) numeros[encontrados++] = num;
  }

  char hashStr[65];
  for (int i = 0; i < 32; i++) sprintf(hashStr + i * 2, "%02x", hash_final[i]);
  hashStr[64] = '\0';

  for (int i = 0; i < 6; i++) ultimo.numeros[i] = numeros[i];
  ultimo.semente = semente_capturada;
  ultimo.tempCPU = getTempCPU();
  memcpy(ultimo.hash, hashStr, 65);
  ultimo.total++;
  String ts = getTimestamp();
  ts.toCharArray(ultimo.timestamp, sizeof(ultimo.timestamp));
  temDados = true;

  CoreStats cs = getCoreStats();

  Serial.println("┌─────────────────────────────────────┐");
  Serial.printf( "│ Sorteio #%-27d │\n", ultimo.total);
  Serial.println("├─────────────────────────────────────┤");
  Serial.printf( "│ Números: %02d  %02d  %02d  %02d  %02d  %02d       │\n",
    numeros[0], numeros[1], numeros[2], numeros[3], numeros[4], numeros[5]);
  Serial.printf( "│ Semente:  %-26u │\n", semente_capturada);
  Serial.println("│ Hash:                               │");
  Serial.printf( "│  %.37s │\n", hashStr);
  Serial.printf( "│  %.37s │\n", hashStr + 37);
  Serial.printf( "│ URL:  http://%-23s │\n", (WiFi.localIP().toString()+"/").c_str());
  Serial.printf( "│ Temp: %-30s │\n", (String(ultimo.tempCPU,1)+" oC").c_str());
  Serial.printf( "│ Core0: %-29s │\n", (String(cs.uso0,1)+"% Forja de Caos").c_str());
  Serial.printf( "│ Core1: %-29s │\n", (String(cs.uso1,1)+"% Oraculo + Web").c_str());
  Serial.printf( "│ Freq:  %-29s │\n", (String(cs.freq)+" MHz").c_str());
  Serial.println("└─────────────────────────────────────┘");

  String jsonPayload = "{\"origem\":\"ESP32_Chaos\",\"semente\":" + String(semente_capturada) +
                       ",\"hash\":\"" + String(hashStr) + "\",\"numeros\":[" +
                       String(numeros[0]) + "," + String(numeros[1]) + "," +
                       String(numeros[2]) + "," + String(numeros[3]) + "," +
                       String(numeros[4]) + "," + String(numeros[5]) + "]}";

  HTTPClient http;
  http.begin(serverUrl);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(jsonPayload);
  http.end();
  Serial.printf("-> Node-RED: HTTP %d\n\n", code);
}

// ─── SETUP ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  memset(snap_handles, 0, sizeof(snap_handles));
  memset(snap_runtime, 0, sizeof(snap_runtime));
  ultimo.total = 0;

  WiFi.begin(ssid, password);
  Serial.print("Conectando ao WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println();

  configTime(gmtOffset, dstOffset, ntpServer);
  Serial.print("Sincronizando NTP");
  struct tm t;
  while (!getLocalTime(&t)) { delay(500); Serial.print("."); }
  Serial.println(" OK - " + getTimestamp());

  server.on("/",     handleRoot);
  server.on("/json", handleJson);
  server.begin();

  Serial.println("┌─────────────────────────────────────┐");
  Serial.println("│        ESP32 CaosForge Online        │");
  Serial.println("├─────────────────────────────────────┤");
  Serial.printf( "│ IP:   %-30s│\n", WiFi.localIP().toString().c_str());
  Serial.printf( "│ Web:  http://%-23s│\n", (WiFi.localIP().toString()+"/").c_str());
  Serial.printf( "│ JSON: http://%-23s│\n", (WiFi.localIP().toString()+"/json").c_str());
  Serial.println("└─────────────────────────────────────┘\n");

  xTaskCreatePinnedToCore(taskGeradoraDeCaos, "UsinaCaos", 4096, NULL, 1, NULL, 0);
}

// ─── LOOP ──────────────────────────────────────────────────────────
void loop() {
  server.handleClient();

  if (millis() - ultimaExecucao >= intervalo) {
    ultimaExecucao = millis();
    realizarSorteioEEnviar();
  }
}
