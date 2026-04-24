// ╔══════════════════════════════════════════════════════════════════════╗
// ║           ESP32 CaosForge — Firmware v2.4 (DOIT ESP32 DEVKIT V1)   ║
// ║                                                                      ║
// ║  v2.4 — adição Lotofácil:                                            ║
// ║  [13] Geração de 15 números únicos de 1-25 (Lotofácil)              ║
// ║  [14] struct UltimoLotofacil + ultimoLotoMtx protege acesso         ║
// ║  [15] SERVER_URL_LOTOFACIL envia payload separado ao Node-RED        ║
// ║  [16] Web UI exibe cards Mega Sena e Lotofácil separados            ║
// ║  [17] /json retorna ambos os sorteios no mesmo objeto               ║
// ╚══════════════════════════════════════════════════════════════════════╝

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include "mbedtls/md.h"
#include <cmath>

// ─── STRUCTS ───────────────────────────────────────────────────────────

struct UltimoSorteio {
  int      numeros[6];
  uint32_t semente;
  char     hash[65];
  char     timestamp[32];
  int      total;
  float    tempCPU;
};

// [14] Struct Lotofácil — 15 números de 1-25
struct UltimoLotofacil {
  int      numeros[15];
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
//  CONFIGURAÇÕES
// ══════════════════════════════════════════════════════════════════════

const char* SSID                 = "SEU_SSID";
const char* PASSWORD             = "SUA_SENHA";
const char* SERVER_URL           = "http://192.168.xxx.xx:1880/caos";
const char* SERVER_URL_LOTOFACIL = "http://192.168.xxx.xx:1880/lotofacil"; // [15]
const char* BEARER_TOKEN         = "TROQUE_POR_TOKEN_FORTE_COM_32_CHARS_MINIMO_AQUI__";

const long     INTERVALO_MS    = 60000UL;
const long     GMT_OFFSET      = 0;
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

static SemaphoreHandle_t ultimoMtx     = NULL;
static SemaphoreHandle_t ultimoLotoMtx = NULL; // [14]
static SemaphoreHandle_t snapMtx       = NULL;

WebServer       server(80);
UltimoSorteio   ultimo;
UltimoLotofacil ultimoLoto;
bool            temDados     = false;
bool            temDadosLoto = false;
unsigned long   ultimaExecucao = 0;

static uint32_t     snap_runtime[MAX_TASKS];
static TaskHandle_t snap_handles[MAX_TASKS];
static UBaseType_t  snap_count     = 0;
static uint32_t     snap_lastTotal = 0;

// ─── SENSOR DE TEMPERATURA ─────────────────────────────────────────────
#ifdef __cplusplus
extern "C" {
#endif
uint8_t temprature_sens_read();
#ifdef __cplusplus
}
#endif

float getTempCPU() { return (temprature_sens_read() - 32) / 1.8f; }

// ══════════════════════════════════════════════════════════════════════
//  NVS — CHAVE HMAC PERSISTENTE
// ══════════════════════════════════════════════════════════════════════

void carregarOuGerarChaveHMAC() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  if (prefs.getBytesLength(NVS_KEY_HMAC) == 32) {
    prefs.getBytes(NVS_KEY_HMAC, hmac_key, 32);
    Serial.println("[NVS] Chave HMAC carregada.");
  } else {
    for (int i = 0; i < 32; i++)
      hmac_key[i] = (uint8_t)((esp_random() ^ (uint32_t)micros()) & 0xFF);
    prefs.putBytes(NVS_KEY_HMAC, hmac_key, 32);
    Serial.println("[NVS] Nova chave HMAC gerada e gravada na NVS.");
  }
  prefs.end();
  for (int i = 0; i < 32; i++) sprintf(hmac_key_hex + i * 2, "%02x", hmac_key[i]);
  hmac_key_hex[64] = '\0';
}

#ifdef RESET_HMAC_KEY
void resetarChaveHMAC() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  prefs.remove(NVS_KEY_HMAC);
  prefs.end();
  Serial.println("[NVS] Chave removida.");
}
#endif

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
    double   tv   = tan((double)entropia_viva);
    uint32_t amp  = (isfinite(tv) && !isnan(tv)) ? (uint32_t)(tv * 1e6) : esp_random();
    portENTER_CRITICAL(&entropiaMux);
    entropia_viva  ^= novo ^ amp;
    contadorEntrop++;
    portEXIT_CRITICAL(&entropiaMux);
    vTaskDelay(1);
  }
}

uint32_t capturarEntropia() {
  uint32_t v; portENTER_CRITICAL(&entropiaMux); v = entropia_viva; portEXIT_CRITICAL(&entropiaMux); return v;
}
uint32_t capturarContador() {
  uint32_t v; portENTER_CRITICAL(&entropiaMux); v = contadorEntrop; portEXIT_CRITICAL(&entropiaMux); return v;
}

// ══════════════════════════════════════════════════════════════════════
//  TASK WEBSERVER
// ══════════════════════════════════════════════════════════════════════

void taskWebServer(void* pvParameters) {
  for (;;) { server.handleClient(); vTaskDelay(2); }
}

// ══════════════════════════════════════════════════════════════════════
//  ESCAPE JSON
// ══════════════════════════════════════════════════════════════════════

String escapeJson(const char* s) {
  String out; out.reserve(strlen(s) + 4);
  for (const char* p = s; *p; p++) {
    switch (*p) {
      case '"':  out += "\\\""; break; case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break; case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break; default:   out += *p;     break;
    }
  }
  return out;
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
  if (xSemaphoreTake(snapMtx, pdMS_TO_TICKS(50)) != pdTRUE) return cs;
  uint32_t deltaTotal = totalRuntime - snap_lastTotal; snap_lastTotal = totalRuntime;
  if (deltaTotal == 0) { xSemaphoreGive(snapMtx); return cs; }
  float delta0 = 0, delta1 = 0;
  for (UBaseType_t i = 0; i < n; i++) {
    uint32_t prev = 0;
    for (UBaseType_t j = 0; j < snap_count; j++)
      if (snap_handles[j] == tasks[i].xHandle) { prev = snap_runtime[j]; break; }
    float d = (float)(tasks[i].ulRunTimeCounter - prev);
    if (tasks[i].xCoreID == 0) delta0 += d; else delta1 += d;
  }
  snap_count = (n < MAX_TASKS) ? n : MAX_TASKS;
  for (UBaseType_t i = 0; i < snap_count; i++) {
    snap_handles[i] = tasks[i].xHandle; snap_runtime[i] = tasks[i].ulRunTimeCounter;
  }
  xSemaphoreGive(snapMtx);
  float half = (float)deltaTotal / 2.0f;
  cs.uso0 = constrain((delta0 / half) * 100.0f, 0.0f, 100.0f);
  cs.uso1 = constrain((delta1 / half) * 100.0f, 0.0f, 100.0f);
  return cs;
}

// ══════════════════════════════════════════════════════════════════════
//  ENDPOINTS WEB
// ══════════════════════════════════════════════════════════════════════

void handleRoot() {
  uint32_t iter   = capturarContador();
  bool     pronto = (iter >= ENTROPIA_MINIMA);

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
    ".card-loto{background:#111;border:1px solid #2d0a1e;border-radius:8px;"
    "padding:24px 28px;width:100%;max-width:540px;margin-bottom:16px}"
    ".label{color:#555;font-size:.7rem;letter-spacing:2px;text-transform:uppercase;margin-bottom:8px}"
    ".label-loto{color:#e91e8c;font-size:.7rem;letter-spacing:2px;text-transform:uppercase;margin-bottom:8px}"
    ".numbers{display:flex;gap:10px;flex-wrap:wrap}"
    ".ball{background:#00e5ff;color:#000;font-weight:bold;font-size:1.1rem;"
    "width:46px;height:46px;border-radius:50%;display:flex;align-items:center;justify-content:center}"
    ".ball-loto{background:#e91e8c;color:#fff;font-weight:bold;font-size:.95rem;"
    "width:40px;height:40px;border-radius:50%;display:flex;align-items:center;justify-content:center}"
    ".hash{color:#00e5ff;font-size:.72rem;word-break:break-all;line-height:1.7}"
    ".hash-loto{color:#e91e8c;font-size:.72rem;word-break:break-all;line-height:1.7}"
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
    ".divider{border:none;border-top:1px solid #222;margin:12px 0}"
    "</style></head><body>"
    "<h1>&#x26A1; CaosForge</h1>"
    "<p class='sub'>ESP32 &middot; HMAC-SHA256 &middot; LIVE</p>");

  if (!pronto) {
    int pct = (int)((float)iter / ENTROPIA_MINIMA * 100.0f);
    html += "<div class='card'><div class='label'>Aquecendo gerador</div>";
    html += "<div class='nodata'>&#x23F3; " + String(iter) + " / " + String(ENTROPIA_MINIMA) + " iter. (" + String(pct) + "%)</div>";
    html += "<div class='bar-bg'><div class='bar' style='width:" + String(pct) + "%;background:#00e5ff'></div></div></div>";
  } else {
    // ── MEGA SENA ────────────────────────────────────────────────────────
    if (!temDados) {
      html += "<div class='card'><div class='label'>&#x1F3B1; Mega Sena</div>"
              "<div class='nodata'>Aguardando primeiro sorteio...</div></div>";
    } else {
      xSemaphoreTake(ultimoMtx, portMAX_DELAY);
      UltimoSorteio snap = ultimo;
      xSemaphoreGive(ultimoMtx);
      html += "<div class='card'><div class='label'>&#x1F3B1; Mega Sena &mdash; 6 de 60</div>"
              "<div class='numbers'>";
      for (int i = 0; i < 6; i++) html += "<div class='ball'>" + String(snap.numeros[i]) + "</div>";
      html += "</div><hr class='divider'>";
      html += "<div class='row'><span class='muted'>Sorteio</span><span class='val'>#" + String(snap.total) + "</span></div>";
      html += "<div class='row'><span class='muted'>Data/Hora</span><span class='val'>" + String(snap.timestamp) + "</span></div>";
      html += "<div class='row'><span class='muted'>Semente</span><span class='val'>" + String(snap.semente) + "</span></div>";
      html += "<div class='label' style='margin-top:8px'>HMAC-SHA256</div>"
              "<div class='hash'>" + String(snap.hash) + "</div></div>";
    }

    // ── LOTOFÁCIL [13][16] ───────────────────────────────────────────────
    if (!temDadosLoto) {
      html += "<div class='card-loto'><div class='label-loto'>&#x1F4CC; Lotofácil</div>"
              "<div class='nodata'>Aguardando primeiro sorteio...</div></div>";
    } else {
      xSemaphoreTake(ultimoLotoMtx, portMAX_DELAY);
      UltimoLotofacil snapLoto = ultimoLoto;
      xSemaphoreGive(ultimoLotoMtx);
      html += "<div class='card-loto'><div class='label-loto'>&#x1F4CC; Lotofácil &mdash; 15 de 25</div>"
              "<div class='numbers'>";
      for (int i = 0; i < 15; i++) html += "<div class='ball-loto'>" + String(snapLoto.numeros[i]) + "</div>";
      html += "</div><hr class='divider'>";
      html += "<div class='row'><span class='muted'>Sorteio</span><span class='val'>#" + String(snapLoto.total) + "</span></div>";
      html += "<div class='row'><span class='muted'>Data/Hora</span><span class='val'>" + String(snapLoto.timestamp) + "</span></div>";
      html += "<div class='row'><span class='muted'>Semente</span><span class='val'>" + String(snapLoto.semente) + "</span></div>";
      html += "<div class='label-loto' style='margin-top:8px'>HMAC-SHA256</div>"
              "<div class='hash-loto'>" + String(snapLoto.hash) + "</div></div>";
    }
  }

  // ── TEMPERATURA ────────────────────────────────────────────────────────
  if (temDados) {
    xSemaphoreTake(ultimoMtx, portMAX_DELAY); float t = ultimo.tempCPU; xSemaphoreGive(ultimoMtx);
    int tp = constrain((int)((t - 30) / 60.0f * 100), 0, 100);
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
  html += "<div class='card'><div class='label'>N&uacute;cleos &middot; " + String(cs.freq) + " MHz</div>"
          "<div class='cr'><span class='cl'>Core 0</span>"
          "<div class='cb'><div class='cbr' style='width:" + String((int)cs.uso0) + "%;background:" + c0 + "'></div></div>"
          "<span class='cp' style='color:" + c0 + "'>" + String(cs.uso0, 1) + "%</span></div>"
          "<div class='cr' style='margin-bottom:0'><span class='cl'>Core 1</span>"
          "<div class='cb'><div class='cbr' style='width:" + String((int)cs.uso1) + "%;background:" + c1 + "'></div></div>"
          "<span class='cp' style='color:" + c1 + "'>" + String(cs.uso1, 1) + "%</span></div>"
          "<div style='margin-top:10px;color:#333;font-size:.65rem'>Core 0 = Forja &nbsp;&middot;&nbsp; Core 1 = Or&aacute;culo + Web</div></div>";

  html += "<div class='card'><div class='label'>Sistema</div>"
          "<div class='row'><span class='muted'>IP</span><span class='val'>" + WiFi.localIP().toString() + "</span></div>"
          "<div class='row'><span class='muted'>Uptime</span><span class='val'>" + String(millis() / 1000) + "s</span></div>"
          "<div class='row'><span class='muted'>RSSI</span><span class='val'>" + String(WiFi.RSSI()) + " dBm</span></div>"
          "<div class='row'><span class='muted'>Heap</span><span class='val'>" + String(ESP.getFreeHeap() / 1024) + " KB</span></div>"
          "<div class='row'><span class='muted'>Entropia</span><span class='val'>" + String(capturarContador()) + " iter.</span></div>"
          "<div class='row' style='margin-bottom:0'><span class='muted'>Status</span>"
          "<span class='val'><span class='dot'></span>Online</span></div></div>";

  html += "<p class='footer'>Atualiza a cada 65s &middot; /json &middot; /info<br>"
          "<span style='color:#e91e8c'>&#x25CF;</span> Lotofácil 15/25 &nbsp;&nbsp;"
          "<span style='color:#00e5ff'>&#x25CF;</span> Mega Sena 6/60</p></body></html>";
  server.send(200, "text/html", html);
}

// [17] /json — retorna ambos os sorteios
void handleJson() {
  uint32_t iter = capturarContador();
  if (xSemaphoreTake(ultimoMtx, pdMS_TO_TICKS(50)) != pdTRUE) {
    server.send(503, "application/json", "{\"status\":\"busy\"}"); return;
  }
  bool dados = temDados; xSemaphoreGive(ultimoMtx);

  if (!dados) {
    server.send(200, "application/json",
      "{\"status\":\"aguardando\",\"pronto\":" + String(iter >= ENTROPIA_MINIMA ? "true" : "false") +
      ",\"iteracoes\":" + String(iter) + ",\"minimo\":" + String(ENTROPIA_MINIMA) + "}");
    return;
  }

  xSemaphoreTake(ultimoMtx, pdMS_TO_TICKS(50));
  UltimoSorteio snap = ultimo; xSemaphoreGive(ultimoMtx);

  xSemaphoreTake(ultimoLotoMtx, pdMS_TO_TICKS(50));
  UltimoLotofacil snapLoto = ultimoLoto; bool dadosLoto = temDadosLoto; xSemaphoreGive(ultimoLotoMtx);

  CoreStats cs = getCoreStats();
  String json = "{\"semente\":" + String(snap.semente) +
    ",\"hash\":\"" + String(snap.hash) + "\"" +
    ",\"timestamp\":\"" + String(snap.timestamp) + "\"" +
    ",\"total\":" + String(snap.total) +
    ",\"temp_cpu\":" + String(snap.tempCPU, 1) +
    ",\"cpu_freq_mhz\":" + String(cs.freq) +
    ",\"core0_pct\":" + String(cs.uso0, 1) +
    ",\"core1_pct\":" + String(cs.uso1, 1) +
    ",\"iteracoes_entropia\":" + String(iter) +
    ",\"mega_sena\":[" +
    String(snap.numeros[0]) + "," + String(snap.numeros[1]) + "," +
    String(snap.numeros[2]) + "," + String(snap.numeros[3]) + "," +
    String(snap.numeros[4]) + "," + String(snap.numeros[5]) + "]" +
    ",\"numeros\":[" +
    String(snap.numeros[0]) + "," + String(snap.numeros[1]) + "," +
    String(snap.numeros[2]) + "," + String(snap.numeros[3]) + "," +
    String(snap.numeros[4]) + "," + String(snap.numeros[5]) + "]";

  if (dadosLoto) {
    json += ",\"lotofacil\":[";
    for (int i = 0; i < 15; i++) { if (i > 0) json += ","; json += String(snapLoto.numeros[i]); }
    json += "]";
    json += ",\"lotofacil_semente\":" + String(snapLoto.semente);
    json += ",\"lotofacil_hash\":\"" + String(snapLoto.hash) + "\"";
    json += ",\"lotofacil_total\":" + String(snapLoto.total);
  }
  json += "}";
  server.send(200, "application/json", json);
}

void handleInfo() {
  bool ch = (hmac_key_hex[0] != '\0');
  bool tk = (strlen(BEARER_TOKEN) >= 32);
  String json = "{\"versao\":\"2.4\""
    ",\"hmac_key_carregada\":" + String(ch ? "true" : "false") +
    ",\"hmac_fonte\":\"NVS\""
    ",\"bearer_token_forte\":" + String(tk ? "true" : "false") +
    ",\"entropia_iteracoes\":" + String(capturarContador()) +
    ",\"entropia_minima\":" + String(ENTROPIA_MINIMA) +
    ",\"ip\":\"" + WiFi.localIP().toString() + "\"" +
    ",\"uptime_s\":" + String(millis() / 1000) +
    ",\"heap_livre_kb\":" + String(ESP.getFreeHeap() / 1024) +
    ",\"modalidades\":[\"mega_sena\",\"lotofacil\"]"
    "}";
  server.send(200, "application/json", json);
}

// ══════════════════════════════════════════════════════════════════════
//  SORTEIO MEGA SENA (6 de 60)
// ══════════════════════════════════════════════════════════════════════

void realizarSorteioEEnviar() {
  uint32_t iter = capturarContador();
  if (iter < ENTROPIA_MINIMA) {
    Serial.printf("[MEGA] Aguardando: %u / %u (%.0f%%)\n", iter, ENTROPIA_MINIMA, (float)iter/ENTROPIA_MINIMA*100.0f);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) { WiFi.begin(SSID, PASSWORD); return; }

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
      mbedtls_md_init(&ctx); mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
      mbedtls_md_hmac_starts(&ctx, hmac_key, 32); mbedtls_md_hmac_update(&ctx, hash_final, 32);
      mbedtls_md_hmac_finish(&ctx, hash_final); mbedtls_md_free(&ctx); byte_idx = 0;
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

  xSemaphoreTake(ultimoMtx, portMAX_DELAY);
  for (int i = 0; i < 6; i++) ultimo.numeros[i] = numeros[i];
  ultimo.semente = semente; ultimo.tempCPU = getTempCPU();
  memcpy(ultimo.hash, hashStr, 65); ultimo.total++;
  String ts = getTimestamp(); ts.toCharArray(ultimo.timestamp, sizeof(ultimo.timestamp));
  temDados = true; xSemaphoreGive(ultimoMtx);

  CoreStats cs = getCoreStats();
  Serial.println("┌─────────────────────────────────────────┐");
  Serial.printf( "│ MEGA SENA #%-29d │\n", ultimo.total);
  Serial.printf( "│ Nums: %02d  %02d  %02d  %02d  %02d  %02d              │\n",
    numeros[0], numeros[1], numeros[2], numeros[3], numeros[4], numeros[5]);
  Serial.printf( "│ Semente: %-30u │\n", semente);
  Serial.println("└─────────────────────────────────────────┘");

  String payload = "{\"origem\":\"ESP32_Chaos_Mega\",\"semente\":" + String(semente) +
    ",\"hash\":\"" + escapeJson(hashStr) + "\"" +
    ",\"timestamp\":\"" + escapeJson(ultimo.timestamp) + "\"" +
    ",\"numeros\":[" +
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
  Serial.printf("-> Node-RED [Mega Sena]: HTTP %d\n\n", code);
}

// ══════════════════════════════════════════════════════════════════════
//  [13] SORTEIO LOTOFÁCIL (15 de 25)
// ══════════════════════════════════════════════════════════════════════

void realizarSorteioLotofacilEEnviar() {
  uint32_t iter = capturarContador();
  if (iter < ENTROPIA_MINIMA) return;
  if (WiFi.status() != WL_CONNECTED) return;

  // Semente independente: XOR adicional para garantir diversidade
  uint32_t sementeLoto = capturarEntropia() ^ (uint32_t)micros() ^ esp_random() ^ 0xCAFEBABEUL;

  uint8_t hash_final[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, hmac_key, 32);
  mbedtls_md_hmac_update(&ctx, (const uint8_t*)&sementeLoto, sizeof(sementeLoto));
  mbedtls_md_hmac_finish(&ctx, hash_final);
  mbedtls_md_free(&ctx);

  // [13] 15 únicos de 1-25; skip b >= 225 (225 = 9×25, uniforme)
  int numerosLoto[15], encontrados = 0, byte_idx = 0;
  while (encontrados < 15) {
    if (byte_idx >= 32) {
      mbedtls_md_init(&ctx); mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
      mbedtls_md_hmac_starts(&ctx, hmac_key, 32); mbedtls_md_hmac_update(&ctx, hash_final, 32);
      mbedtls_md_hmac_finish(&ctx, hash_final); mbedtls_md_free(&ctx); byte_idx = 0;
    }
    uint8_t b = hash_final[byte_idx++];
    if (b >= 225) continue;     // descarta bytes que causariam viés
    int num = (b % 25) + 1;     // 1..25
    bool rep = false;
    for (int i = 0; i < encontrados; i++) if (numerosLoto[i] == num) { rep = true; break; }
    if (!rep) numerosLoto[encontrados++] = num;
  }

  char hashStr[65];
  for (int i = 0; i < 32; i++) sprintf(hashStr + i * 2, "%02x", hash_final[i]);
  hashStr[64] = '\0';

  // [14] Escrita atômica via mutex dedicado
  xSemaphoreTake(ultimoLotoMtx, portMAX_DELAY);
  for (int i = 0; i < 15; i++) ultimoLoto.numeros[i] = numerosLoto[i];
  ultimoLoto.semente = sementeLoto; ultimoLoto.tempCPU = getTempCPU();
  memcpy(ultimoLoto.hash, hashStr, 65); ultimoLoto.total++;
  String ts = getTimestamp(); ts.toCharArray(ultimoLoto.timestamp, sizeof(ultimoLoto.timestamp));
  temDadosLoto = true; xSemaphoreGive(ultimoLotoMtx);

  Serial.println("┌─────────────────────────────────────────┐");
  Serial.printf( "│ LOTOFÁCIL #%-29d │\n", ultimoLoto.total);
  Serial.printf( "│ %02d %02d %02d %02d %02d %02d %02d %02d %02d %02d %02d %02d %02d %02d %02d │\n",
    numerosLoto[0], numerosLoto[1], numerosLoto[2], numerosLoto[3], numerosLoto[4],
    numerosLoto[5], numerosLoto[6], numerosLoto[7], numerosLoto[8], numerosLoto[9],
    numerosLoto[10], numerosLoto[11], numerosLoto[12], numerosLoto[13], numerosLoto[14]);
  Serial.printf( "│ Semente: %-30u │\n", sementeLoto);
  Serial.println("└─────────────────────────────────────────┘");

  // Monta array de 15 números
  String nums = "";
  for (int i = 0; i < 15; i++) { if (i > 0) nums += ","; nums += String(numerosLoto[i]); }

  String payload = "{\"origem\":\"ESP32_Chaos_Loto\",\"semente\":" + String(sementeLoto) +
    ",\"hash\":\"" + escapeJson(hashStr) + "\"" +
    ",\"timestamp\":\"" + escapeJson(ultimoLoto.timestamp) + "\"" +
    ",\"numeros\":[" + nums + "]}";

  // [15] POST para endpoint separado /lotofacil
  HTTPClient http;
  http.begin(SERVER_URL_LOTOFACIL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + BEARER_TOKEN);
  http.setTimeout(5000);
  int code = http.POST(payload);
  http.end();
  Serial.printf("-> Node-RED [Lotofácil]: HTTP %d\n\n", code);
}

// ══════════════════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(1500);

  memset(snap_handles, 0, sizeof(snap_handles));
  memset(snap_runtime, 0, sizeof(snap_runtime));
  memset(&ultimo,     0, sizeof(ultimo));
  memset(&ultimoLoto, 0, sizeof(ultimoLoto)); // [14]

  carregarOuGerarChaveHMAC();

  WiFi.begin(SSID, PASSWORD);
  Serial.print("[WiFi] Conectando");
  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 40) { delay(500); Serial.print("."); t++; }
  if (WiFi.status() == WL_CONNECTED)
    Serial.println(" OK — " + WiFi.localIP().toString());
  else
    Serial.println(" TIMEOUT (sem WiFi)");

  if (WiFi.status() == WL_CONNECTED) {
    configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER);
    Serial.print("[NTP] Sincronizando");
    struct tm tm_info; int nt = 0;
    while (!getLocalTime(&tm_info) && nt < 20) { delay(500); Serial.print("."); nt++; }
    Serial.println(nt < 20 ? " OK — " + getTimestamp() : " TIMEOUT");
  }

  server.on("/",     handleRoot);
  server.on("/json", handleJson);
  server.on("/info", handleInfo);
  server.begin();

  // [8][9][14] Mutexes FreeRTOS — criados ANTES das tasks
  ultimoMtx     = xSemaphoreCreateMutex();
  ultimoLotoMtx = xSemaphoreCreateMutex();
  snapMtx       = xSemaphoreCreateMutex();
  if (!ultimoMtx || !ultimoLotoMtx || !snapMtx) {
    Serial.println("[ERRO] Falha ao criar mutexes — reiniciando.");
    ESP.restart();
  }

  xTaskCreatePinnedToCore(taskWebServer,      "WebServer",  10240, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(taskGeradoraDeCaos, "UsinaCaos",   4096, NULL, 1, NULL, 0);

  Serial.println();
  Serial.println("┌──────────────────────────────────────────────────┐");
  Serial.println("│        ESP32 CaosForge v2.4 — Online             │");
  Serial.println("├──────────────────────────────────────────────────┤");
  Serial.printf( "│ IP:       %-39s│\n", WiFi.localIP().toString().c_str());
  Serial.println("├──────────────────────────────────────────────────┤");
  Serial.println("│ Modalidades:                                     │");
  Serial.println("│   Mega Sena  — 6 numeros de 1-60  → POST /caos  │");
  Serial.println("│   Lotofacil  — 15 numeros de 1-25 → POST /lotofacil │");
  Serial.println("├──────────────────────────────────────────────────┤");
  Serial.printf( "│ Bearer token forte (>=32): %-21s │\n",
                 strlen(BEARER_TOKEN) >= 32 ? "SIM" : "NAO — TROQUE!");
  Serial.printf( "│ Aguardando %u iter. de entropia...            │\n", ENTROPIA_MINIMA);
  Serial.println("└──────────────────────────────────────────────────┘\n");
}

// ══════════════════════════════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════════════════════════════

void loop() {
  if (millis() - ultimaExecucao >= INTERVALO_MS) {
    ultimaExecucao = millis();
    realizarSorteioEEnviar();           // Mega Sena — 6 de 60
    realizarSorteioLotofacilEEnviar();  // [13] Lotofácil — 15 de 25
  }
  delay(100);
}
