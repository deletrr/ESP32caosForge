# ⚡ ESP32 CaosForge

> **Gerador de entropia física com sorteio criptográfico verificável, autenticação em toda a cadeia e monitoramento estatístico em tempo real via Node-RED e Google Sheets.**

[![Platform](https://img.shields.io/badge/platform-ESP32-blue?style=flat-square)](https://www.espressif.com/en/products/socs/esp32)
[![Framework](https://img.shields.io/badge/framework-Arduino-teal?style=flat-square)](https://www.arduino.cc/)
[![Node-RED](https://img.shields.io/badge/Node--RED-3.x-red?style=flat-square)](https://nodered.org/)
[![Version](https://img.shields.io/badge/versão-2.3-brightgreen?style=flat-square)](#)
[![License](https://img.shields.io/badge/license-Open%20Source-green?style=flat-square)](#)


## 📈 Dados ao vivo

Os sorteios são registrados em tempo real nesta planilha pública:

🔗 **[Acessar Google Sheets](https://docs.google.com/spreadsheets/d/1qoPtb4fNSjBl3aQU2CqS8u9W14VDHrp8trGWmSIrbM0/edit?usp=sharing)**


---

## 📋 Índice

- [O que é](#-o-que-é)
- [Histórico de versões](#-histórico-de-versões)
- [Como funciona](#-como-funciona)
- [Arquitetura do sistema](#-arquitetura-do-sistema)
- [Segurança criptográfica](#-segurança-criptográfica)
- [Autenticação em toda a cadeia](#-autenticação-em-toda-a-cadeia)
- [Concorrência entre tasks](#-concorrência-entre-tasks)
- [Monitor de Integridade](#-monitor-de-integridade)
- [WebServer embutido](#-webserver-embutido)
- [Temperatura do die (CPU)](#-temperatura-do-die-cpu)
- [Configuração do ESP32](#-configuração-do-esp32)
- [Configuração do Node-RED](#-configuração-do-node-red)
- [Configuração do Google Sheets](#-configuração-do-google-sheets)
- [Solução de problemas](#-solução-de-problemas)
- [Estrutura do repositório](#-estrutura-do-repositório)
- [Fundamentos teóricos](#-fundamentos-teóricos)


---

## 🌀 O que é

O **ESP32 CaosForge** é um sistema de geração de aleatoriedade baseada em entropia física real. Em vez de depender de algoritmos de software (que são pseudo-aleatórios e determinísticos), o projeto explora o comportamento caótico do hardware do ESP32 — ruído térmico do chip, jitter de clock, variações do oscilador interno — para produzir números genuinamente imprevisíveis.

O resultado é um pipeline completo de sorteio auditável e autenticado:

```
[Entropia Física] → [HMAC-SHA256 / chave NVS] → [Sorteio 1–60]
  → [Bearer Token] → [Node-RED] → [env vars + token] → [Google Sheets]
```

Cada sorteio é acompanhado de uma assinatura HMAC que permite verificação criptográfica independente. Toda a comunicação entre componentes é autenticada por token. Os tokens **nunca aparecem no `flows.json`** exportado.

---

## 🗂️ Histórico de versões

### v2.3 — Segurança e concorrência (atual)

| # | O que foi corrigido |
|---|---|
| 1 | `/info` não expõe mais a `hmac_key` — exibir a chave anularia a segurança do HMAC |
| 2 | Bearer Token obrigatório com **mínimo de 32 caracteres** — verificado no boot com aviso no Serial |
| 3 | `SemaphoreHandle_t ultimoMtx` protege a struct `UltimoSorteio` entre tasks (substituiu `portENTER_CRITICAL`, que causa deadlock entre tasks no mesmo core) |
| 4 | `SemaphoreHandle_t snapMtx` protege os arrays de CPU stats entre tasks pelo mesmo motivo |
| 5 | **Timestamp incluído no payload POST** — ativa a proteção anti-replay no Google Apps Script |
| 6 | `resetarChaveHMAC()` compilada somente com `-DRESET_HMAC_KEY` — elimina o risco de apagar a chave NVS acidentalmente |
| 7 | **Escape JSON** aplicado nos campos de string do payload — previne quebra de JSON por caracteres especiais no hash ou timestamp |

### v2.2 — Tokens via variáveis de ambiente no Node-RED

Tokens do Node-RED movidos para variáveis de ambiente (`env.get()`), de forma que não aparecem no `flows.json` exportado.

### v2.1 — Robustez e compatibilidade com DOIT ESP32 DEVKIT V1

- Chave HMAC persistente em NVS (nunca hardcoded)
- Bearer Token autentica o POST ao Node-RED
- WebServer em task FreeRTOS dedicada
- Entropia mínima de 10.000 iterações antes do 1º sorteio
- Guard NaN/Inf no `tan()` do amplificador caótico
- SPIFFS removido (causava reboot loop no DOIT V1)
- `delay(1500)` no boot para o CP2102 estabilizar
- `vTaskDelay(1)` restaurado para alimentar o watchdog
- Bug do MONITOR corrigido: `outputs: 2` com wires separados
- `{{{env.VAR}}}` no campo URL do nó http request

---

## ⚙️ Como funciona

### Core 0 — A Forja de Caos

Roda em loop com `vTaskDelay(1)`. A cada iteração combina três fontes via XOR:

```cpp
uint32_t novo = esp_random() ^ (uint32_t)micros();

// Guard NaN/Inf: tan() diverge perto de k*pi/2
double   tv          = tan((double)entropia_viva);
uint32_t amplificado = (isfinite(tv) && !isnan(tv))
                       ? (uint32_t)(tv * 1e6)
                       : esp_random();  // fallback seguro

entropia_viva  ^= novo ^ amplificado;
contadorEntrop++;
vTaskDelay(1);  // obrigatório: alimenta watchdog, cede CPU ao idle task
```

### Core 1 — O Oráculo

A cada 60 segundos, após 10.000 iterações acumuladas:

1. Captura `entropia_viva` com `portENTER_CRITICAL` (spinlock — correto para variável compartilhada entre cores)
2. Combina com `micros()` e `esp_random()` para a semente final
3. Gera **HMAC-SHA256** usando a chave da NVS
4. Sorteia 6 números únicos de 1 a 60 sem viés estatístico
5. Escreve o resultado na struct `UltimoSorteio` **protegida por mutex FreeRTOS**
6. Envia via HTTP POST com Bearer Token e timestamp

### WebServer — Task independente

```cpp
void taskWebServer(void* pvParameters) {
  for (;;) {
    server.handleClient();
    vTaskDelay(2);
  }
}
```

Roda no Core 1 com stack de 10KB. Lê a struct `UltimoSorteio` **sempre via mutex**, garantindo que nunca lê dados parcialmente escritos.

---

## 🏗️ Arquitetura do sistema

```
┌──────────────────────────────────────────────────────────┐
│                         ESP32                            │
│                                                          │
│  ┌────────────────┐      ┌───────────────────────────┐  │
│  │     Core 0     │      │          Core 1            │  │
│  │  "A Forja"     │─────▶│  "O Oráculo"              │  │
│  │                │      │                            │  │
│  │  esp_random()  │      │  Espera 10k iterações      │  │
│  │  micros()      │      │  HMAC-SHA256 / chave NVS   │  │
│  │  tan(caos)     │      │  Sorteio 1–60 sem viés     │  │
│  │  isfinite()    │      │  escapeJson() no payload   │  │
│  │  vTaskDelay(1) │      │  HTTP POST + Bearer Token  │  │
│  └────────────────┘      │  timestamp (anti-replay)   │  │
│                          │  WebServer :80 (task)      │  │
│  ┌────────────────┐      └──────────────┬─────────────┘  │
│  │      NVS       │◀────── hmac_key[32] │                 │
│  └────────────────┘                     │                 │
│  ┌─────────────────────────────────┐    │                 │
│  │  SemaphoreHandle_t ultimoMtx   │◀───┘                 │
│  │  SemaphoreHandle_t snapMtx     │  (FreeRTOS mutex)    │
│  └─────────────────────────────────┘                      │
└──────────────────────────────────┬───────────────────────┘
                    ┌──────────────┴──────────────────┐
                    │ POST /caos                       │ HTTP GET
                    │ Authorization: Bearer <token>    │
                    │ payload com timestamp            │
                    ▼                                  ▼
     ┌──────────────────────────┐     ┌─────────────────────────┐
     │         Node-RED         │     │  Browser / rede local   │
     │  ✅ env.get(BEARER_TOKEN)│     │  http://[IP]/           │
     │  ↓ json parser           │     │  http://[IP]/json       │
     │  ↓ pipeline              │     │  http://[IP]/info       │
     │  ↓ IA → http request     │     └─────────────────────────┘
     │  ↓ MONITOR (outputs: 2)  │
     │    ↓saída 0  ↓saída 1    │
     │   debug    http request  │
     └──────────────┬───────────┘
    {{{env.GAS_URL}}}?token={{{env.GAS_TOKEN}}}
                    │
         ┌──────────┴──────────┐
         ▼                     ▼
   ┌───────────┐    ┌──────────────────┐
   │   Dados   │    │    Alertas       │
   │ (Sheets)  │    │   (Sheets)       │
   └───────────┘    └──────────────────┘
```

---

## 🔐 Segurança criptográfica

### Chave HMAC persistente na NVS

No primeiro boot, o ESP32 gera 32 bytes aleatórios e salva na NVS:

```cpp
for (int i = 0; i < 32; i++) {
  hmac_key[i] = (uint8_t)((esp_random() ^ (uint32_t)micros()) & 0xFF);
}
prefs.putBytes("hmac_key", hmac_key, 32);
```

A chave persiste após reflash enquanto a NVS não for formatada.

### O endpoint `/info` não expõe a chave

Versões anteriores retornavam `hmac_key_hex` no endpoint `/info`. Isso foi removido na v2.3 — exibir a chave HMAC publicamente anularia toda a segurança do sistema, já que qualquer um poderia recriar os hashes:

```cpp
// [6] hmac_key_hex NUNCA é retornada — apenas indica se está carregada
bool chaveCarregada = (hmac_key_hex[0] != '\0');
```

A resposta do `/info` agora é:
```json
{
  "versao": "2.3",
  "hmac_key_carregada": true,
  "hmac_fonte": "NVS",
  "bearer_token_forte": true,
  "entropia_iteracoes": 183420,
  "entropia_minima": 10000,
  "ip": "192.168.1.42",
  "uptime_s": 3721,
  "heap_livre_kb": 214
}
```

### Sorteio sem viés de módulo

```
Bytes válidos: 0–239  →  240 valores
240 ÷ 60 = 4 grupos exatos  →  probabilidade uniforme ✅
Bytes descartados: 240–255  →  eliminam viés de módulo
```

### Escape JSON no payload

```cpp
String escapeJson(const char* s) {
  // escapa: " \ \n \r \t
}

// Aplicado nos campos string antes do POST
",\"hash\":\""      + escapeJson(hashStr)         + "\""
",\"timestamp\":\"" + escapeJson(ultimo.timestamp) + "\""
```

Previne que um hash com caracteres especiais quebre o JSON e cause comportamento inesperado no Node-RED ou no Apps Script.

### Resetar a chave HMAC de forma segura

Na v2.3, `resetarChaveHMAC()` só é compilada quando você define explicitamente a flag de build:

```
// No Arduino IDE: Sketch → Export Compiled Binary
// Build flags (platformio.ini ou flags extras):
-DRESET_HMAC_KEY
```

Isso elimina o risco de chamar a função acidentalmente no `setup()` e apagar a chave permanentemente. Após usar, remova a flag e compile novamente.

> ⚠️ Resetar a chave invalida todos os hashes históricos — sorteios anteriores não podem mais ser verificados.

---

## 🔒 Autenticação em toda a cadeia

### Camada 1 — ESP32 → Node-RED: Bearer Token (≥ 32 chars)

Na v2.3, o firmware verifica no boot se o token tem pelo menos 32 caracteres e exibe aviso no Serial:

```
│ Bearer token forte (>=32): SIM              │
```

Se o token tiver menos de 32 caracteres:
```
│ Bearer token forte (>=32): NAO — TROQUE!   │
```

Gere um token forte com:
```bash
openssl rand -hex 32
```

O token vai no header de cada POST:
```
Authorization: Bearer <seu_token_32_chars>
```

### Camada 2 — Node-RED: variáveis de ambiente

Os tokens ficam nas configurações locais do Node-RED e **não aparecem no `flows.json`** exportado:

```
≡ → Settings → Environment variables
```

| Nome | Valor |
|---|---|
| `BEARER_TOKEN` | token gerado com `openssl rand -hex 32` |
| `GAS_TOKEN` | token diferente para o Google Sheets |
| `GAS_URL` | URL do Apps Script (sem `?token=`) |

No nó **"✅ Verificar Bearer Token"**:
```javascript
const TOKEN_ESPERADO = env.get('BEARER_TOKEN');
```

No campo URL do nó **http request**:
```
{{{env.GAS_URL}}}?token={{{env.GAS_TOKEN}}}
```

> Use **três chaves** `{{{ }}}` — com duas o Node-RED escapa `/` e `?` da URL, quebrando a requisição.

### Camada 3 — Node-RED → Google Sheets: token + anti-replay

O Apps Script valida o token via `?token=` na URL e, a partir da v2.3, **rejeita payloads sem timestamp** (antes aceitava):

```javascript
function timestampRecente(tsString) {
  if (!tsString) return false;  // v2.3: sem timestamp = rejeitado
  // ...verifica se está dentro dos últimos 5 minutos
  const dataRecebida = new Date(Date.UTC(y, m-1, d, h, min, s));
  return Math.abs(new Date() - dataRecebida) < 5 * 60 * 1000;
}
```

O timestamp usa **UTC** para comparação correta independente do fuso horário do servidor Node-RED.

### Camada 4 — Apps Script: token no código-fonte (seguro)

O token do Apps Script pode ficar hardcoded no código sem problema — o código-fonte do Apps Script é **privado por padrão** e só acessível por editores da sua conta Google:

```javascript
const GAS_SECRET_TOKEN = "SEU_TOKEN_GAS";
```

| Local | Quem pode ver? | Risco |
|---|---|---|
| `flows.json` exportado | Qualquer pessoa que receba o arquivo | Alto |
| Variáveis de ambiente Node-RED | Apenas quem tem acesso ao servidor | Baixo |
| Código do Apps Script | Apenas editores do seu Google Account | Baixo |

---

## 🔄 Concorrência entre tasks

A v2.3 corrigiu um problema silencioso nas versões anteriores: o uso de `portENTER_CRITICAL` (spinlock) para proteger dados compartilhados entre tasks diferentes.

### Por que `portENTER_CRITICAL` era incorreto aqui

`portENTER_CRITICAL` desativa interrupções no core local. Ele é adequado para proteger variáveis compartilhadas **entre uma ISR e uma task no mesmo core** (como `entropia_viva`, que é escrita a cada tick). Mas entre duas tasks normais em cores diferentes (o loop do Oráculo e a task do WebServer), ele pode causar **deadlock** se ambas tentarem adquirir o lock ao mesmo tempo.

### Solução: `SemaphoreHandle_t` (mutex FreeRTOS)

```cpp
// Criados antes das tasks, no setup()
ultimoMtx = xSemaphoreCreateMutex();  // protege UltimoSorteio
snapMtx   = xSemaphoreCreateMutex();  // protege arrays de CPU stats

// Escrita (Oráculo)
xSemaphoreTake(ultimoMtx, portMAX_DELAY);
ultimo = novoSorteio;
xSemaphoreGive(ultimoMtx);

// Leitura (WebServer)
xSemaphoreTake(ultimoMtx, pdMS_TO_TICKS(50));
UltimoSorteio snap = ultimo;  // cópia local — libera rápido
xSemaphoreGive(ultimoMtx);
```

O WebServer usa `pdMS_TO_TICKS(50)` como timeout — se não conseguir o lock em 50ms, retorna HTTP 503 em vez de bloquear indefinidamente.

### Mapa de proteção de dados compartilhados

| Dado | Mecanismo | Por quê |
|---|---|---|
| `entropia_viva`, `contadorEntrop` | `portENTER_CRITICAL` (spinlock) | Compartilhado com ISR / operação atômica brevíssima |
| `UltimoSorteio ultimo` | `SemaphoreHandle_t ultimoMtx` | Entre duas tasks normais — mutex evita deadlock |
| Arrays `snap_runtime`, `snap_handles` | `SemaphoreHandle_t snapMtx` | Idem |

---

## 📊 Monitor de Integridade

O Node-RED inclui um nó de **monitoramento estatístico contínuo** que analisa cada sorteio em busca de anomalias no hardware.

> **Importante:** O nó MONITOR DE INTEGRIDADE deve ter **`outputs: 2`** configurado. Com `outputs: 1` os alertas nunca chegam ao Google Sheets.

### O que é detectado

| Detecção | Método | O que indica |
|---|---|---|
| **Reboot do ESP32** | Semente = 0 | `micros()` zerou — chip reiniciou |
| **RNG travado** | Hash duplicado | Estado do gerador ciclando |
| **Entropia baixa** | Δ semente < 1.000 | Degradação física do gerador |
| **Correlação serial** | >15% repetição entre sorteios | Memória indesejada no sistema |
| **Viés estatístico** | Qui-quadrado (χ²) > 75 | Alguns números saindo mais |
| **Faixa estreita** | Desvio padrão EWMA < 70% | Números concentrados numa região |
| **Sequências** | Análise de runs | Números consecutivos suspeitos |

### Score de saúde do RNG

```
🟢 SAUDÁVEL   Score ≥ 85   Tudo normal
🟡 ATENÇÃO    Score ≥ 60   Anomalias leves
🔴 CRÍTICO    Score < 60   Problema grave — verificar hardware
```

---

## 🌐 WebServer embutido

Servidor HTTP na porta 80, em task FreeRTOS dedicada no Core 1 com 10KB de stack. Todas as leituras de dados do sorteio passam pelo mutex antes de renderizar.

### Rotas disponíveis

| Rota | Descrição |
|---|---|
| `http://[IP]/` | Página visual com dados do último sorteio |
| `http://[IP]/json` | JSON com sorteio e CPU stats |
| `http://[IP]/info` | Status do sistema — **sem expor a chave HMAC** |

### Resposta `/info` (v2.3)

```json
{
  "versao": "2.3",
  "hmac_key_carregada": true,
  "hmac_fonte": "NVS",
  "bearer_token_forte": true,
  "entropia_iteracoes": 183420,
  "entropia_minima": 10000,
  "ip": "192.168.1.42",
  "uptime_s": 3721,
  "heap_livre_kb": 214
}
```

### Resposta `/json`

```json
{
  "semente": 2847361920,
  "hash": "a3f2...c91d",
  "timestamp": "16/03/2026 02:07:14",
  "total": 3,
  "temp_cpu": 53.4,
  "cpu_freq_mhz": 240,
  "core0_pct": 98.2,
  "core1_pct": 4.1,
  "iteracoes_entropia": 183420,
  "numeros": [7, 23, 41, 5, 58, 19]
}
```

### Serial Monitor no boot (v2.3)

```
┌─────────────────────────────────────────────┐
│       ESP32 CaosForge v2.3 — Online         │
├─────────────────────────────────────────────┤
│ IP:     192.168.X.X                         │
│ Web:    http://192.168.X.X/                 │
│ JSON:   http://192.168.X.X/json             │
│ Info:   http://192.168.X.X/info             │
├─────────────────────────────────────────────┤
│ HMAC key: carregada da NVS (nunca exposta)  │
├─────────────────────────────────────────────┤
│ Bearer token forte (>=32): SIM              │
├─────────────────────────────────────────────┤
│ Aguardando 10000 iter. de entropia...       │
└─────────────────────────────────────────────┘
```

---

## 🌡️ Temperatura do die (CPU)

```cpp
extern "C" uint8_t temprature_sens_read();  // typo intencional no SDK Espressif

float getTempCPU() {
  return (temprature_sens_read() - 32) / 1.8f;
}
```

Imprecisão de ±5–10°C. O valor absoluto não é confiável, mas a variação indica carga do sistema.

| Chip | Suporte |
|---|---|
| ESP32 clássico (DOIT DEVKIT V1) | ✅ `temprature_sens_read()` |
| ESP32-S2 / S3 / C3 | ⚠️ API diferente — requer adaptação |

---

## 💻 Configuração do ESP32

### Dependências

Todas incluídas no **ESP32 Arduino Core** — nenhuma instalação adicional:

| Biblioteca | Uso |
|---|---|
| `mbedtls/md.h` | HMAC-SHA256 |
| `Preferences.h` | NVS — chave HMAC persistente |
| `WiFi.h`, `HTTPClient.h`, `WebServer.h` | Rede e servidor |
| `<cmath>` | Guard NaN/Inf |

### Configuração no `caos.ino`

```cpp
const char* SSID         = "SEU_WIFI";
const char* PASSWORD     = "SUA_SENHA";
const char* SERVER_URL   = "http://192.168.X.X:1880/caos";

// Gere com: openssl rand -hex 32
// Mínimo obrigatório: 32 caracteres
const char* BEARER_TOKEN = "cole_aqui_saida_do_openssl_rand_hex_32";
```

### Upload

1. Abra `caos.ino` na Arduino IDE
2. Placa: **ESP32 Dev Module**
3. Edite as configurações acima
4. Upload → abra o Serial Monitor (115200 baud)
5. Verifique a linha `Bearer token forte (>=32): SIM`

---

## 🔗 Configuração do Node-RED

### 1. Cadastrar as variáveis de ambiente

**≡ → Settings → Environment variables**

| Nome | Valor |
|---|---|
| `BEARER_TOKEN` | mesmo valor de `BEARER_TOKEN` no `caos.ino` |
| `GAS_TOKEN` | token separado para o Google Sheets (`openssl rand -hex 32`) |
| `GAS_URL` | URL do Apps Script **sem** `?token=` |

Clique em **Save**. Os valores não aparecem no `flows.json` ao exportar.

### 2. Importar o fluxo

**≡ → Import** → cole `flows.json` → **Import → Deploy**

### 3. Configurar os três nós

**Nó "✅ Verificar Bearer Token":**
```javascript
const TOKEN_ESPERADO = env.get('BEARER_TOKEN');

const auth = msg.req && msg.req.headers && msg.req.headers['authorization'];
if (!auth || auth !== 'Bearer ' + TOKEN_ESPERADO) {
    node.warn('[AUTH] Tentativa não autorizada — IP: ' + (msg.req && msg.req.ip || '?'));
    node.status({fill:'red', shape:'dot', text:'Rejeitado ' + new Date().toLocaleTimeString()});
    return null;
}
node.status({fill:'green', shape:'dot', text:'OK ' + new Date().toLocaleTimeString()});
return msg;
```

**Nó "🔑 Adicionar Token GAS":**
```javascript
// Apenas serializa — URL e token ficam no nó http request
msg.payload = JSON.stringify(msg.payload);
msg.headers = { 'Content-Type': 'application/json' };
return msg;
```

**Nó "http request"** (Google Sheets):

| Campo | Valor |
|---|---|
| **Method** | POST |
| **URL** | `{{{env.GAS_URL}}}?token={{{env.GAS_TOKEN}}}` |
| **Return** | a UTF-8 string |

**Nó "MONITOR DE INTEGRIDADE"** — verifique que tem **2 saídas**:
- Saída 0 → `monitor debug`
- Saída 1 → nó `http request`

### 4. Verificar que os tokens estão protegidos

Exporte o fluxo (**≡ → Export → Download**) e abra o JSON. Você verá:

```json
"url": "{{{env.GAS_URL}}}?token={{{env.GAS_TOKEN}}}"
```

Nenhum valor real aparece — o arquivo pode ser commitado no GitHub.

### Estrutura do fluxo

```
[POST /caos]
     │
     ├──▶ [http response]              ← Responde 200 ao ESP32 imediatamente
     │
     └──▶ [✅ Verificar Bearer Token]  ← env.get('BEARER_TOKEN')
               │
               └──▶ [json parser]
                         │
                         ├──▶ [hash]      ──▶ Dashboard
                         ├──▶ [sementes]  ──▶ Dashboard
                         ├──▶ [numeros]   ──▶ Dashboard
                         ├──▶ [String]    ──▶ Debug
                         ├──▶ [Learning]  ──▶ Análise de tendências
                         ├──▶ [IA]
                         │      └──▶ [http request]  ← {{{env.GAS_URL}}}?token={{{env.GAS_TOKEN}}}
                         │                ↓ Sheets aba Dados
                         └──▶ [MONITOR DE INTEGRIDADE] (outputs: 2)
                                  ├── saída 0 ──▶ monitor debug
                                  └── saída 1 ──▶ [http request]
                                                       ↓ Sheets aba Alertas
```

---

## 📊 Configuração do Google Sheets

### Instalação do Apps Script

1. Abra a planilha → **Extensões → Apps Script**
2. Verifique se há mais de um arquivo no painel esquerdo — se houver, **exclua os extras** (⋮ → Excluir)
3. No arquivo restante, **Ctrl+A → Delete**
4. Cole o conteúdo de `google_apps_script.js`
5. Configure o token:
   ```javascript
   const GAS_SECRET_TOKEN = "SEU_TOKEN_GAS";  // mesmo GAS_TOKEN do Node-RED
   ```
6. Salve (Ctrl+S)
7. **Implantar → Nova implantação**
   - Tipo: **Aplicativo da Web**
   - Executar como: **Eu**
   - Quem pode acessar: **Qualquer pessoa**
8. Autorize as permissões
9. Copie a URL gerada e cole no campo `GAS_URL` das variáveis de ambiente do Node-RED

### Comportamento anti-replay (v2.3)

O Apps Script agora **rejeita payloads sem timestamp** e payloads com timestamp mais antigo que 5 minutos. O timestamp é enviado automaticamente pelo firmware v2.3 em cada POST.

A comparação usa UTC, o que garante funcionamento correto independente do fuso horário do servidor Node-RED.

### Testar o Apps Script

```
https://script.google.com/macros/s/SEU_ID/exec
```

Resposta esperada:
```json
{"status": "online", "planilha": "...", "versao": "2.3"}
```

Se retornar "Página não encontrada" → vá em **Implantar → Gerenciar implantações** e copie a URL atual.

### Abas da planilha

**`Dados`** — sorteios normais:
| Data | Semente | Hash | N1 | N2 | N3 | N4 | N5 | N6 | IA_tentativa | IA_Acertos |

**`Alertas`** — anomalias do Monitor:
| Timestamp | Tipo | Severidade | Mensagem | Valor | Score_RNG | Status_Geral | Chi2 | Desvio_EWMA | Total_Sorteios |

---

## 🔧 Solução de problemas

### ESP32

| Sintoma no Serial | Causa | Solução |
|---|---|---|
| Reboot loop após `[NVS] Chave carregada` | Filesystem incompatível | Use `caos.ino` v2.3 — sem filesystem |
| `Bearer token forte (>=32): NAO` | Token curto demais | Gere com `openssl rand -hex 32` |
| `[ERRO] Falha ao criar mutexes` | Heap insuficiente | Verifique se há outras alocações grandes no setup |

### Node-RED

| Mensagem | Causa | Solução |
|---|---|---|
| `msg properties can no longer override set node properties` | `msg.url` não funciona no Node-RED 3.x | Use `{{{env.GAS_URL}}}` no campo URL do nó |
| `non-http transport requested` | Campo URL com valor inválido | Preencha com `{{{env.GAS_URL}}}?token={{{env.GAS_TOKEN}}}` |
| Token sempre rejeitado | Variável de ambiente não cadastrada | Verifique **Settings → Environment variables** |

### Google Apps Script

| Mensagem | Causa | Solução |
|---|---|---|
| `SHEET_ID has already been declared` | Dois arquivos no projeto | Exclua os extras, deixe só um |
| `Página não encontrada` | URL desatualizada | Copie em **Gerenciar implantações** |
| `Payload expirado (> 5 min)` | Relógio do ESP32 ou Node-RED desincronizado | Verifique se o NTP sincronizou no boot |
| `Não autorizado` | Tokens incompatíveis | Confirme que `GAS_TOKEN` no Node-RED bate com `GAS_SECRET_TOKEN` no script |

---

## 📁 Estrutura do repositório

```
ESP32CaosForge/
│
├── caos.ino                    # Firmware (v2.3)
│                               # → Chave HMAC em NVS — nunca exposta no /info
│                               # → Bearer Token >= 32 chars obrigatório
│                               # → SemaphoreHandle_t para UltimoSorteio e CPU stats
│                               # → Timestamp no payload (ativa anti-replay GAS)
│                               # → escapeJson() nos campos string do payload
│                               # → resetarChaveHMAC() só com -DRESET_HMAC_KEY
│                               # → WebServer task 10KB stack, Core 1
│                               # → Guard NaN/Inf no tan()
│                               # → delay(1500) + vTaskDelay(1) para DOIT V1
│                               # → Endpoints: /, /json, /info
│
├── flows.json                  # Fluxo Node-RED (v2.2+)
│                               # → Tokens via env.get() — não aparecem no export
│                               # → URL: {{{env.GAS_URL}}}?token={{{env.GAS_TOKEN}}}
│                               # → MONITOR com outputs: 2 (bug corrigido)
│                               # → Pipeline: IA, Monitor, Sheets
│
├── google_apps_script.js       # Apps Script (v2.3)
│                               # → Validação de token via ?token= na URL
│                               # → Anti-replay: rejeita payloads sem timestamp
│                               # → Timestamp comparado em UTC (fuso neutro)
│                               # → Roteamento automático Dados/Alertas
│                               # → Health check via GET /exec
│
├── flow.jpg                    # Screenshot do fluxo Node-RED
│
└── README.md                   # Este arquivo
```

---

## 🧠 Fundamentos teóricos

**Por que hardware e não software?**
PRNGs como `Math.random()` ou `rand()` são determinísticos — dada a mesma semente, produzem a mesma sequência. O ESP32 usa fontes físicas genuinamente não-determinísticas: variações de temperatura, interferências eletromagnéticas e instabilidades no oscilador de cristal.

**Por que aguardar entropia mínima?**
Logo após o boot, o pool de entropia está quase vazio. 10.000 iterações garantem mistura suficiente com amostras independentes antes do primeiro sorteio.

**Por que `portENTER_CRITICAL` para `entropia_viva` mas mutex para `UltimoSorteio`?**
`portENTER_CRITICAL` desativa interrupções e é correto para proteger operações brevíssimas entre uma ISR e uma task no mesmo core. Para dados compartilhados entre duas tasks normais (o Oráculo e o WebServer), ele pode causar deadlock. `SemaphoreHandle_t` é o mecanismo correto do FreeRTOS para esse caso.

**Por que o guard no `tan()`?**
`tan(x)` diverge para ±∞ próximo de π/2 + kπ. Em ponto flutuante isso produz `Inf` ou `NaN`, e `(uint32_t)(Inf * 1e6)` é comportamento indefinido em C++. O guard com `isfinite()` usa `esp_random()` como fallback seguro.

**Por que o viés de módulo importa?**
Aplicar `% 60` sobre 256 valores faz os números 0–15 terem 5 chances de aparecer e os demais apenas 4. Com 10 mil sorteios isso acumula ~4.000 ocorrências a mais para metade dos números. Descartar 240–255 deixa 240 valores para 60 grupos perfeitos.

**Por que HMAC resiste a length extension attacks?**
SHA-256 puro permite calcular `SHA256(mensagem ∥ extensão)` conhecendo apenas `SHA256(mensagem)`. O HMAC aplica a chave em duas rodadas, quebrando essa propriedade. Com a chave na NVS e nunca exposta no `/info`, ninguém consegue forjar um sorteio válido.


---

## 📈 Dados ao vivo

Os sorteios são registrados em tempo real nesta planilha pública:

🔗 **[Acessar Google Sheets](https://docs.google.com/spreadsheets/d/1qoPtb4fNSjBl3aQU2CqS8u9W14VDHrp8trGWmSIrbM0/edit?usp=sharing)**

---

*Projeto open source para estudo de entropia, criptografia aplicada, sistemas embarcados e segurança em IoT.*
