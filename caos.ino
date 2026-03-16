// ╔══════════════════════════════════════════════════════════════════════╗
// ║           ESP32 CaosForge — Firmware v2.1 (DOIT ESP32 DEVKIT V1)   ║
// ║                                                                      ║
// ║  MELHORIAS vs original:                                              ║
// ║  [1] Chave HMAC gerada no boot e salva em NVS — nunca hardcoded     ║
// ║  [2] Bearer Token autentica o POST ao Node-RED                       ║
// ║  [3] WebServer em task FreeRTOS dedicada — não bloqueia o loop()    ║
// ║  [4] Entropia mínima: aguarda 10.000 iterações antes do 1º sorteio  ║
// ║  [5] Guard NaN/Inf no tan() do amplificador caótico                  ║
// ║  [6] Endpoint /info expõe chave, versão e stats                      ║
// ╚══════════════════════════════════════════════════════════════════════╝

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>    // [1] NVS — chave HMAC persistente
#include <time.h>
#include "mbedtls/md.h"
#include <cmath>             // [5] isfinite / isnan

// ─── STRUCTS (antes das funções — Arduino IDE exige isso) ──────────────

struct UltimoSorteio {
  int      numeros[6];
  uint32_t semente;
  char     hash[65];
  char     timestamp[32];
  int      total;
  float    tempCPU;
};

#define MAX_TASKS 20
struct CoreStats {
  float    uso0;
  float    uso1;
  uint32_t freq;
};

// ══════════════════════════════════════════════════════════════════════
//  CONFIGURAÇÕES — edite antes de fazer upload
// ══════════════════════════════════════════════════════════════════════

const char* SSID         = "SEU_WIFI";
const char* PASSWORD     = "SUA_SENHA";
const char* SERVER_URL   = "http://192.168.x.x:1880/caos";

// [2] Bearer Token — mesmo valor que TOKEN_ESPERADO no Node-RED
const char* BEARER_TOKEN = "TROQUE_POR_TOKEN_SECRETO_AQUI";

// ─── CONSTANTES ────────────────────────────────────────────────────────
const long     INTERVALO_MS    = 60000UL;
const long     GMT_OFFSET      = -3L * 3600L;
const int      DST_OFFSET      = 0;
const char*    NTP_SERVER      = "pool.ntp.org";
const char*    NVS_NAMESPACE   = "caosforge";
const char*    NVS_KEY_HMAC    = "hmac_key";
const uint32_t ENTROPIA_MINIMA = 10000UL;

// ─── VARIÁVEIS GLOBAIS ─────────────────────────────────────────────────

static uint8_t hmac_key[32];
static char    hmac_key_hex[65];

static portMUX_TYPE entropiaMux    = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t   entropia_viva  = 0;
volatile uint32_t   contadorEntrop = 0;

WebServer     server(80);
UltimoSorteio ultimo;
bool          temDados       = false;
unsigned long ultimaExecucao = 0;

static uint32_t     snap_runtime[MAX_TASKS];
static TaskHandle_t snap_handles[MAX_TASKS];
static UBaseType_t  snap_count     = 0;
static uint32_t     snap_lastTotal = 0;

// ─── SENSOR DE TEMPERATURA ─────────────────────────────────────────────
// "temprature_sens_read" — typo intencional, é o nome real no SDK Espressif
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

// ══════════════════════════════════════════════════════════════════════
//  [1] NVS — CHAVE HMAC PERSISTENTE
// ══════════════════════════════════════════════════════════════════════

void carregarOuGerarChaveHMAC() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);

  if (prefs.getBytesLength(NVS_KEY_HMAC) == 32) {
    prefs.getBytes(NVS_KEY_HMAC, hmac_key, 32);
    Serial.println("[NVS] Chave HMAC carregada.");
  } else {
    for (int i = 0; i < 32; i++) {
      hmac_key[i] = (uint8_t)((esp_random() ^ (uint32_t)micros()) & 0xFF);
    }
    prefs.putBytes(NVS_KEY_HMAC, hmac_key, 32);
    Serial.println("[NVS] Nova chave HMAC gerada e gravada na NVS.");
  }
  prefs.end();

  for (int i = 0; i < 32; i++) sprintf(hmac_key_hex + i * 2, "%02x", hmac_key[i]);
  hmac_key_hex[64] = '\0';
}

// Chame no setup() para forçar geração de nova chave, depois remova
void resetarChaveHMAC() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.remove(NVS_KEY_HMAC);
  prefs.end();
  Serial.println("[NVS] Chave removida. Sera regerada no proximo boot.");
}

// ══════════════════════════════════════════════════════════════════════
//  NTP
// ══════════════════════════════════════════════════════════════════════

String getTimestamp() {
  struct tm t;
  if (!getLocalTime(&t)) return "sem NTP";
  char buf[20];
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &t);
  return String(buf);
}

// ══════════════════════════════════════════════════════════════════════
//  CORE 0 — GERADOR DE ENTROPIA
// ══════════════════════════════════════════════════════════════════════

void taskGeradoraDeCaos(void* pvParameters) {
  for (;;) {
    uint32_t novo = esp_random() ^ (uint32_t)micros();

    // [5] Guard NaN/Inf: tan() diverge perto de k*pi/2
    double   tv          = tan((double)entropia_viva);
    uint32_t amplificado = (isfinite(tv) && !isnan(tv))
                           ? (uint32_t)(tv * 1e6)
                           : esp_random();

    portENTER_CRITICAL(&entropiaMux);
    entropia_viva  ^= novo ^ amplificado;
    contadorEntrop++;
    portEXIT_CRITICAL(&entropiaMux);

    vTaskDelay(1);  // obrigatorio: alimenta watchdog e cede CPU ao idle task
  }
}

uint32_t capturarEntropia() {
  uint32_t val;
  portENTER_CRITICAL(&entropiaMux);
  val = entropia_viva;
  portEXIT_CRITICAL(&entropiaMux);
  return val;
}

uint32_t capturarContador() {
  uint32_t val;
  portENTER_CRITICAL(&entropiaMux);
  val = contadorEntrop;
  portEXIT_CRITICAL(&entropiaMux);
  return val;
}

// ══════════════════════════════════════════════════════════════════════
//  [3] TASK DO WEBSERVER
// ══════════════════════════════════════════════════════════════════════

void taskWebServer(void* pvParameters) {
  for (;;) {
    server.handleClient();
    vTaskDelay(2);
  }
}

// ══════════════════════════════════════════════════════════════════════
//  CPU STATS
// ══════════════════════════════════════════════════════════════════════

CoreStats getCoreStats() {
  CoreStats cs = { 0.0f, 0.0f, getCpuFrequencyMhz() };
  TaskStatus_t tasks[MAX_TASKS];
  uint32_t totalRuntime = 0;
  UBaseType_t n = uxTaskGetSystemState(tasks, MAX_TASKS, &totalRuntime);
  if (n == 0 || totalRuntime == 0) return cs;

  uint32_t deltaTotal = totalRuntime - snap_lastTotal;
  snap_lastTotal = totalRuntime;
  if (deltaTotal == 0) return cs;

  float delta0 = 0, delta1 = 0;
  for (UBaseType_t i = 0; i < n; i++) {
    uint32_t prev = 0;
    for (UBaseType_t j = 0; j < snap_count; j++) {
      if (snap_handles[j] == tasks[i].xHandle) { prev = snap_runtime[j]; break; }
    }
    float d = (float)(tasks[i].ulRunTimeCounter - prev);
    if (tasks[i].xCoreID == 0) delta0 += d;
    else                        delta1 += d;
  }
  snap_count = (n < MAX_TASKS) ? n : MAX_TASKS;
  for (UBaseType_t i = 0; i < snap_count; i++) {
    snap_handles[i] = tasks[i].xHandle;
    snap_runtime[i] = tasks[i].ulRunTimeCounter;
  }
  float half = (float)deltaTotal / 2.0f;
  cs.uso0 = constrain((delta0 / half) * 100.0f, 0.0f, 100.0f);
  cs.uso1 = constrain((delta1 / half) * 100.0f, 0.0f, 100.0f);
  return cs;
}

// ══════════════════════════════════════════════════════════════════════
//  ENDPOINTS
// ══════════════════════════════════════════════════════════════════════

void handleRoot() {
  uint32_t iter   = capturarContador();
  bool     pronto = (iter >= ENTROPIA_MINIMA);

  // HTML montado em partes para evitar string unica muito grande na RAM
  String html = F("<!DOCTYPE html><html lang='pt-BR'><head>"
    "<meta charset='UTF-8'><meta http-equiv='refresh' content='65'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 CaosForge</title><style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{background:#0d0d0d;color:#e0e0e0;font-family:'Courier New',monospace;"
    "display:flex;flex-direction:column;align-items:center;padding:30px 16px}"
    "h1{color:#00e5ff;font-size:1.4rem;letter-spacing:4px;margin-bottom:4px;text-transform:uppercase}"
    ".sub{color:#555;font-size:.75rem;margin-bottom:32px;letter-spacing:2px}"
    ".card{background:#111;border:1px solid #1e1e1e;border-radius:8px;"
    "padding:24px 28px;width:100%;max-width:540px;margin-bottom:16px}"
    ".label{color:#555;font-size:.7rem;letter-spacing:2px;text-transform:uppercase;margin-bottom:8px}"
    ".numbers{display:flex;gap:10px;flex-wrap:wrap}"
    ".ball{background:#00e5ff;color:#000;font-weight:bold;font-size:1.1rem;"
    "width:46px;height:46px;border-radius:50%;display:flex;align-items:center;justify-content:center}"
    ".hash{color:#00e5ff;font-size:.72rem;word-break:break-all;line-height:1.7}"
    ".row{display:flex;justify-content:space-between;margin-bottom:10px}"
    ".val{color:#fff;font-size:.9rem}.muted{color:#444;font-size:.7rem}"
    ".dot{display:inline-block;width:8px;height:8px;border-radius:50%;"
    "background:#00e5ff;margin-right:6px;animation:blink 1.2s infinite}"
    "@keyframes blink{0%,100%{opacity:1}50%{opacity:.2}}"
    ".footer{color:#333;font-size:.65rem;margin-top:24px;text-align:center;line-height:1.8}"
    ".nodata{color:#444;text-align:center;padding:20px;font-size:.85rem}"
    ".bar-bg{background:#1e1e1e;border-radius:4px;height:6px;width:100%;margin-top:6px}"
    ".bar{height:6px;border-radius:4px;transition:width .5s}"
    ".tv{font-size:1.6rem;font-weight:bold}"
    ".cr{display:flex;align-items:center;gap:10px;margin-bottom:10px}"
    ".cl{color:#555;font-size:.7rem;width:56px;flex-shrink:0}"
    ".cb{flex:1;background:#1e1e1e;border-radius:4px;height:8px}"
    ".cbr{height:8px;border-radius:4px;transition:width .5s}"
    ".cp{color:#fff;font-size:.8rem;width:42px;text-align:right;flex-shrink:0}"
    "</style></head><body>"
    "<h1>&#x26A1; CaosForge</h1>"
    "<p class='sub'>ESP32 &middot; HMAC-SHA256 &middot; LIVE</p>");

  if (!pronto) {
    int pct = (int)((float)iter / ENTROPIA_MINIMA * 100.0f);
    html += "<div class='card'><div class='label'>Aquecendo gerador</div>";
    html += "<div class='nodata'>&#x23F3; " + String(iter) + " / " + String(ENTROPIA_MINIMA) + " iter. (" + String(pct) + "%)</div>";
    html += "<div class='bar-bg'><div class='bar' style='width:" + String(pct) + "%;background:#00e5ff'></div></div></div>";
  } else if (!temDados) {
    html += "<div class='card'><div class='nodata'>Aguardando primeiro sorteio...</div></div>";
  } else {
    html += "<div class='card'><div class='label'>&#x1F3B1; Numeros sorteados</div><div class='numbers'>";
    for (int i = 0; i < 6; i++) html += "<div class='ball'>" + String(ultimo.numeros[i]) + "</div>";
    html += "</div></div>";

    html += "<div class='card'>"
            "<div class='row'><span class='muted'>Sorteio</span><span class='val'>#" + String(ultimo.total) + "</span></div>"
            "<div class='row'><span class='muted'>Data/Hora</span><span class='val'>" + String(ultimo.timestamp) + "</span></div>"
            "<div class='row' style='margin-bottom:0'><span class='muted'>Semente</span><span class='val'>" + String(ultimo.semente) + "</span></div></div>";

    html += "<div class='card'><div class='label'>HMAC-SHA256</div><div class='hash'>" + String(ultimo.hash) + "</div></div>";

    float t = ultimo.tempCPU;
    int   tp = constrain((int)((t - 30) / 60.0f * 100), 0, 100);
    String ct = t < 55 ? "#00e5ff" : t < 70 ? "#ffb300" : "#f44336";
    html += "<div class='card'><div class='label'>Temperatura do Die</div>"
            "<div class='row' style='align-items:baseline'>"
            "<span class='tv' style='color:" + ct + "'>" + String(t, 1) + "&#xB0;C</span>"
            "<span class='muted' style='margin-left:10px'>die &middot; &plusmn;5&ndash;10&deg;C</span></div>"
            "<div class='bar-bg'><div class='bar' style='width:" + String(tp) + "%;background:" + ct + "'></div></div></div>";
  }

  CoreStats cs = getCoreStats();
  String c0 = cs.uso0 < 60 ? "#00e5ff" : cs.uso0 < 85 ? "#ffb300" : "#f44336";
  String c1 = cs.uso1 < 60 ? "#00e5ff" : cs.uso1 < 85 ? "#ffb300" : "#f44336";
  html += "<div class='card'><div class='label'>Nucleos &middot; " + String(cs.freq) + " MHz</div>"
          "<div class='cr'><span class='cl'>Core 0</span>"
          "<div class='cb'><div class='cbr' style='width:" + String((int)cs.uso0) + "%;background:" + c0 + "'></div></div>"
          "<span class='cp' style='color:" + c0 + "'>" + String(cs.uso0, 1) + "%</span></div>"
          "<div class='cr' style='margin-bottom:0'><span class='cl'>Core 1</span>"
          "<div class='cb'><div class='cbr' style='width:" + String((int)cs.uso1) + "%;background:" + c1 + "'></div></div>"
          "<span class='cp' style='color:" + c1 + "'>" + String(cs.uso1, 1) + "%</span></div>"
          "<div style='margin-top:10px;color:#333;font-size:.65rem'>Core 0 = Forja &nbsp;&middot;&nbsp; Core 1 = Oraculo + Web</div></div>";

  html += "<div class='card'><div class='label'>Sistema</div>"
          "<div class='row'><span class='muted'>IP</span><span class='val'>"            + WiFi.localIP().toString()          + "</span></div>"
          "<div class='row'><span class='muted'>Uptime</span><span class='val'>"        + String(millis() / 1000) + "s"      + "</span></div>"
          "<div class='row'><span class='muted'>RSSI</span><span class='val'>"          + String(WiFi.RSSI()) + " dBm"       + "</span></div>"
          "<div class='row'><span class='muted'>Heap</span><span class='val'>"          + String(ESP.getFreeHeap() / 1024) + " KB" + "</span></div>"
          "<div class='row'><span class='muted'>Entropia</span><span class='val'>"      + String(capturarContador()) + " iter." + "</span></div>"
          "<div class='row' style='margin-bottom:0'><span class='muted'>Status</span>"
          "<span class='val'><span class='dot'></span>Online</span></div></div>";

  html += "<p class='footer'>Atualiza a cada 65s &middot; /json &middot; /info</p></body></html>";
  server.send(200, "text/html", html);
}

void handleJson() {
  uint32_t iter = capturarContador();
  if (!temDados) {
    server.send(200, "application/json",
      "{\"status\":\"aguardando\",\"pronto\":" + String(iter >= ENTROPIA_MINIMA ? "true" : "false") +
      ",\"iteracoes\":" + String(iter) + ",\"minimo\":" + String(ENTROPIA_MINIMA) + "}");
    return;
  }
  CoreStats cs = getCoreStats();
  String json =
    "{\"semente\":"          + String(ultimo.semente)    +
    ",\"hash\":\""           + String(ultimo.hash)       + "\"" +
    ",\"timestamp\":\""      + String(ultimo.timestamp)  + "\"" +
    ",\"total\":"            + String(ultimo.total)      +
    ",\"temp_cpu\":"         + String(ultimo.tempCPU, 1) +
    ",\"cpu_freq_mhz\":"     + String(cs.freq)           +
    ",\"core0_pct\":"        + String(cs.uso0, 1)        +
    ",\"core1_pct\":"        + String(cs.uso1, 1)        +
    ",\"iteracoes_entropia\":" + String(iter)            +
    ",\"numeros\":["         +
    String(ultimo.numeros[0]) + "," + String(ultimo.numeros[1]) + "," +
    String(ultimo.numeros[2]) + "," + String(ultimo.numeros[3]) + "," +
    String(ultimo.numeros[4]) + "," + String(ultimo.numeros[5]) + "]}";
  server.send(200, "application/json", json);
}

void handleInfo() {
  String json =
    "{\"versao\":\"2.1\""
    ",\"hmac_key_hex\":\""    + String(hmac_key_hex)    + "\""
    ",\"hmac_fonte\":\"NVS\""
    ",\"bearer_token_set\":"  + String(strlen(BEARER_TOKEN) > 10 ? "true" : "false") +
    ",\"entropia_iteracoes\":" + String(capturarContador()) +
    ",\"entropia_minima\":"   + String(ENTROPIA_MINIMA)  +
    ",\"ip\":\""              + WiFi.localIP().toString() + "\""
    ",\"uptime_s\":"          + String(millis() / 1000)  +
    ",\"heap_livre_kb\":"     + String(ESP.getFreeHeap() / 1024) +
    "}";
  server.send(200, "application/json", json);
}

// ══════════════════════════════════════════════════════════════════════
//  SORTEIO + ENVIO
// ══════════════════════════════════════════════════════════════════════

void realizarSorteioEEnviar() {
  uint32_t iter = capturarContador();
  if (iter < ENTROPIA_MINIMA) {
    Serial.printf("[SORTEIO] Aguardando: %u / %u (%.0f%%)\n",
                  iter, ENTROPIA_MINIMA, (float)iter / ENTROPIA_MINIMA * 100.0f);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Desconectado. Reconectando...");
    WiFi.begin(SSID, PASSWORD);
    return;
  }

  uint32_t semente = capturarEntropia() ^ (uint32_t)micros() ^ esp_random();

  uint8_t hash_final[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, hmac_key, 32);
  mbedtls_md_hmac_update(&ctx, (const uint8_t*)&semente, sizeof(semente));
  mbedtls_md_hmac_finish(&ctx, hash_final);
  mbedtls_md_free(&ctx);

  int numeros[6], encontrados = 0, byte_idx = 0;
  while (encontrados < 6) {
    if (byte_idx >= 32) {
      mbedtls_md_init(&ctx);
      mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
      mbedtls_md_hmac_starts(&ctx, hmac_key, 32);
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
  ultimo.semente = semente;
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
  Serial.printf( "│ Numeros: %02d  %02d  %02d  %02d  %02d  %02d       │\n",
    numeros[0], numeros[1], numeros[2], numeros[3], numeros[4], numeros[5]);
  Serial.printf( "│ Semente:  %-26u │\n", semente);
  Serial.printf( "│ Entropia: %-26u │\n", iter);
  Serial.println("│ Hash:                               │");
  Serial.printf( "│  %.37s │\n", hashStr);
  Serial.printf( "│  %.37s │\n", hashStr + 37);
  Serial.printf( "│ Temp: %-30s │\n", (String(ultimo.tempCPU, 1) + " oC").c_str());
  Serial.printf( "│ Core0: %-29s │\n", (String(cs.uso0, 1) + "%").c_str());
  Serial.printf( "│ Core1: %-29s │\n", (String(cs.uso1, 1) + "%").c_str());
  Serial.println("└─────────────────────────────────────┘");

  String payload =
    "{\"origem\":\"ESP32_Chaos\",\"semente\":" + String(semente) +
    ",\"hash\":\""    + String(hashStr) + "\",\"numeros\":[" +
    String(numeros[0]) + "," + String(numeros[1]) + "," +
    String(numeros[2]) + "," + String(numeros[3]) + "," +
    String(numeros[4]) + "," + String(numeros[5]) + "]}";

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + BEARER_TOKEN);
  http.setTimeout(5000);
  int code = http.POST(payload);
  http.end();
  Serial.printf("-> Node-RED: HTTP %d\n\n", code);
}

// ══════════════════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(1500);  // CP2102 precisa estabilizar no DOIT V1

  memset(snap_handles, 0, sizeof(snap_handles));
  memset(snap_runtime, 0, sizeof(snap_runtime));
  memset(&ultimo,      0, sizeof(ultimo));

  // [1] Chave HMAC em NVS
  carregarOuGerarChaveHMAC();

  // WiFi
  WiFi.begin(SSID, PASSWORD);
  Serial.print("[WiFi] Conectando");
  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 40) {
    delay(500);
    Serial.print(".");
    t++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" OK — " + WiFi.localIP().toString());
  } else {
    Serial.println(" TIMEOUT (sem WiFi)");
  }

  // NTP
  if (WiFi.status() == WL_CONNECTED) {
    configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER);
    Serial.print("[NTP] Sincronizando");
    struct tm tm_info;
    int nt = 0;
    while (!getLocalTime(&tm_info) && nt < 20) { delay(500); Serial.print("."); nt++; }
    Serial.println(nt < 20 ? " OK — " + getTimestamp() : " TIMEOUT");
  }

  // WebServer
  server.on("/",     handleRoot);
  server.on("/json", handleJson);
  server.on("/info", handleInfo);
  server.begin();

  // [3] Task WebServer em Core 1 (10KB stack — necessário para HTML gerado)
  xTaskCreatePinnedToCore(taskWebServer,      "WebServer", 10240, NULL, 1, NULL, 1);

  // Task de entropia em Core 0
  xTaskCreatePinnedToCore(taskGeradoraDeCaos, "UsinaCaos",  4096, NULL, 1, NULL, 0);

  Serial.println();
  Serial.println("┌─────────────────────────────────────────────┐");
  Serial.println("│       ESP32 CaosForge v2.1 — Online         │");
  Serial.println("├─────────────────────────────────────────────┤");
  Serial.printf( "│ IP:     %-36s│\n", WiFi.localIP().toString().c_str());
  Serial.printf( "│ Web:    http://%-29s│\n", (WiFi.localIP().toString() + "/").c_str());
  Serial.printf( "│ JSON:   http://%-29s│\n", (WiFi.localIP().toString() + "/json").c_str());
  Serial.printf( "│ Info:   http://%-29s│\n", (WiFi.localIP().toString() + "/info").c_str());
  Serial.println("├─────────────────────────────────────────────┤");
  Serial.printf( "│ HMAC key (NVS):                             │\n");
  Serial.printf( "│  %.44s │\n", hmac_key_hex);
  Serial.printf( "│  %.44s │\n", hmac_key_hex + 44);
  Serial.println("├─────────────────────────────────────────────┤");
  Serial.printf( "│ Aguardando %u iter. de entropia...       │\n", ENTROPIA_MINIMA);
  Serial.println("└─────────────────────────────────────────────┘\n");
}

// ══════════════════════════════════════════════════════════════════════
//  LOOP — WebServer roda em task própria [3]; loop() só controla tempo
// ══════════════════════════════════════════════════════════════════════

void loop() {
  if (millis() - ultimaExecucao >= INTERVALO_MS) {
    ultimaExecucao = millis();
    realizarSorteioEEnviar();
  }
  delay(100);
}

