# ⚡ ESP32 CaosForge

## 📈 Dados ao vivo

Os sorteios são registrados em tempo real nesta planilha pública:

🔗 **[Acessar Google Sheets](https://docs.google.com/spreadsheets/d/1qoPtb4fNSjBl3aQU2CqS8u9W14VDHrp8trGWmSIrbM0/edit?usp=sharing)**

---

> **Gerador de entropia física com sorteio criptográfico verificável, autenticação em toda a cadeia e monitoramento estatístico em tempo real via Node-RED e Google Sheets.**

[![Platform](https://img.shields.io/badge/platform-ESP32-blue?style=flat-square)](https://www.espressif.com/en/products/socs/esp32)
[![Framework](https://img.shields.io/badge/framework-Arduino-teal?style=flat-square)](https://www.arduino.cc/)
[![Node-RED](https://img.shields.io/badge/Node--RED-3.x-red?style=flat-square)](https://nodered.org/)
[![Version](https://img.shields.io/badge/versão-2.2-brightgreen?style=flat-square)](#)
[![License](https://img.shields.io/badge/license-Open%20Source-green?style=flat-square)](#)

---

## 📋 Índice

- [O que é](#-o-que-é)
- [O que mudou na v2.2](#-o-que-mudou-na-v22)
- [Como funciona](#-como-funciona)
- [Arquitetura do sistema](#-arquitetura-do-sistema)
- [Segurança criptográfica](#-segurança-criptográfica)
- [Autenticação em toda a cadeia](#-autenticação-em-toda-a-cadeia)
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

Cada sorteio é acompanhado de uma assinatura HMAC que permite verificação criptográfica independente. Toda a comunicação entre componentes é autenticada por token — sem autenticação, nenhum dado é processado. Os tokens **nunca aparecem no `flows.json`** exportado.

---

## 🆕 O que mudou na v2.2

### Segurança

| # | Problema | Solução |
|---|---|---|
| 1 | Chave HMAC hardcoded no `.ino` | Gerada no 1º boot e salva na **NVS**. Nunca aparece no código-fonte. |
| 2 | POST ao Node-RED sem autenticação | **Bearer Token** no header `Authorization`. |
| 3 | Token GAS hardcoded no `flows.json` | **Variáveis de ambiente** do Node-RED — tokens não aparecem no export do fluxo. |

### Robustez (correções do DOIT ESP32 DEVKIT V1)

| # | Problema | Solução |
|---|---|---|
| 4 | WebServer bloqueava o `loop()` | **Task FreeRTOS dedicada** no Core 1. |
| 5 | Primeiro sorteio com entropia imatura | Aguarda **10.000 iterações** antes do 1º sorteio. |
| 6 | `tan()` podia gerar NaN/Inf | **Guard `isfinite()`** com fallback para `esp_random()`. |
| 7 | SPIFFS causava reboot loop | **SPIFFS removido** — API de diretórios incompatível com a partição padrão do DOIT V1. |
| 8 | Reboot logo após NVS | **`delay(1500)`** para o CP2102 estabilizar no boot. |
| 9 | Task de entropia matava o watchdog | **`vTaskDelay(1)`** restaurado a cada iteração. |

### Correções de integração

| # | Problema | Solução |
|---|---|---|
| 10 | `msg.url` não funciona no Node-RED 3.x | URL configurada no **campo do nó** com sintaxe mustache `{{{env.VAR}}}`. |
| 11 | MONITOR com `outputs: 1` e `return [msg1, msg2]` | **`outputs: 2`** — saída 0 → debug, saída 1 → Google Sheets. |
| 12 | `SHEET_ID` duplicado no Apps Script | Script colado em arquivo único após apagar tudo. |

---

## ⚙️ Como funciona

### Core 0 — A Forja de Caos

Roda em loop com `vTaskDelay(1)` para alimentar o watchdog. A cada iteração, combina três fontes via XOR:

```cpp
uint32_t novo = esp_random() ^ (uint32_t)micros();

// Guard NaN/Inf: tan() diverge perto de k*pi/2
double   tv          = tan((double)entropia_viva);
uint32_t amplificado = (isfinite(tv) && !isnan(tv))
                       ? (uint32_t)(tv * 1e6)
                       : esp_random();  // fallback seguro

entropia_viva  ^= novo ^ amplificado;
contadorEntrop++;
```

### Core 1 — O Oráculo

A cada 60 segundos, após o pool atingir 10.000 iterações:

1. Captura `entropia_viva` com spinlock FreeRTOS
2. Combina com `micros()` e `esp_random()` para a semente final
3. Gera **HMAC-SHA256** usando a chave da NVS
4. Sorteia 6 números únicos de 1 a 60 sem viés estatístico
5. Envia via HTTP POST com **Bearer Token** no header

### WebServer — Task independente

```cpp
void taskWebServer(void* pvParameters) {
  for (;;) {
    server.handleClient();
    vTaskDelay(2);
  }
}
```

### Por que HMAC e não SHA-256 simples?

O HMAC (RFC 2104) é resistente a *length extension attacks*. Com a chave armazenada na NVS e nunca exposta no código, ninguém consegue forjar hashes mesmo capturando todos os sorteios transmitidos.

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
│  │  isfinite()    │      │  HTTP POST + Bearer Token  │  │
│  │  vTaskDelay(1) │      │  WebServer :80 (task)      │  │
│  └────────────────┘      └──────────────┬─────────────┘  │
│  ┌────────────────┐                     │                 │
│  │      NVS       │◀────────────────────┘                 │
│  │  hmac_key[32]  │  (gerada no 1º boot, persiste)        │
│  └────────────────┘                                       │
└──────────────────────────────────┬───────────────────────┘
                    ┌──────────────┴──────────────────┐
                    │ JSON POST                        │ HTTP GET
                    │ Authorization: Bearer <token>    │
                    ▼                                  ▼
     ┌──────────────────────────┐     ┌─────────────────────────┐
     │         Node-RED         │     │  Browser / rede local   │
     │                          │     │  http://[IP]/           │
     │  ✅ Verificar Bearer     │     │  http://[IP]/json       │
     │  ↓  (env.BEARER_TOKEN)   │     │  http://[IP]/info       │
     │  json parser             │     └─────────────────────────┘
     │  ↓ ↓ ↓ ↓ ↓              │
     │  [pipeline]              │
     │  ↓ IA → http request     │
     │  ↓ MONITOR (outputs: 2)  │
     │    ↓saída 0  ↓saída 1    │
     │   debug    http request  │
     └──────────────┬───────────┘
    URL: {{{env.GAS_URL}}}?token={{{env.GAS_TOKEN}}}
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

A chave persiste após reflash enquanto a NVS não for formatada. Para visualizá-la acesse `http://[IP]/info` ou leia o Serial Monitor.

### Sorteio sem viés de módulo

```
Bytes válidos: 0–239  →  240 valores
240 ÷ 60 = 4 grupos exatos  →  probabilidade uniforme ✅
Bytes descartados: 240–255  →  eliminam viés de módulo
```

### Proteção de race condition (dual-core)

```cpp
portENTER_CRITICAL(&entropiaMux);
val = entropia_viva;
portEXIT_CRITICAL(&entropiaMux);
```

### Resetar a chave HMAC

1. Adicione temporariamente no `setup()`: `resetarChaveHMAC();`
2. Faça upload, anote a nova chave no Serial Monitor
3. Remova a linha e faça upload novamente

> ⚠️ Resetar invalida todos os hashes históricos.

---

## 🔒 Autenticação em toda a cadeia

O projeto usa três camadas de autenticação. Nenhum token aparece no `flows.json` exportado.

### Camada 1 — ESP32 → Node-RED: Bearer Token

O ESP32 envia o token no header HTTP:
```
Authorization: Bearer SEU_TOKEN
```

O nó **"✅ Verificar Bearer Token"** lê o token de uma variável de ambiente:
```javascript
const TOKEN_ESPERADO = env.get('BEARER_TOKEN');
```

### Camada 2 — Node-RED → Google Sheets: Token na URL via variável de ambiente

O nó **http request** usa sintaxe mustache para compor a URL em tempo de execução:
```
{{{env.GAS_URL}}}?token={{{env.GAS_TOKEN}}}
```

O token e a URL ficam apenas nas configurações locais do Node-RED — nunca no `flows.json`.

### Camada 3 — Google Apps Script: Token no código do script

O token no Apps Script fica no código-fonte do projeto, que é **privado por padrão** — só quem tem acesso de editor à sua conta Google consegue ler. Não é necessário usar variáveis de ambiente aqui.

```javascript
const GAS_SECRET_TOKEN = "SEU_TOKEN_GAS";
```

### Por que variáveis de ambiente no Node-RED mas não no Apps Script?

| Local | Quem pode ver o código? | Risco de exposição |
|---|---|---|
| `flows.json` | Qualquer pessoa que receba o arquivo exportado | Alto — export inclui tudo |
| Apps Script | Apenas editores do seu Google Account | Baixo — código é privado |

Por isso as variáveis de ambiente são essenciais no Node-RED e desnecessárias no Apps Script.

---

## 📊 Monitor de Integridade

O Node-RED inclui um nó de **monitoramento estatístico contínuo** que analisa cada sorteio em busca de anomalias no hardware.

> **Importante:** O nó MONITOR DE INTEGRIDADE deve ter **`outputs: 2`** configurado. Com `outputs: 1` os alertas nunca chegam ao Google Sheets — este era um bug do fluxo original.

### O que é detectado

| Detecção | Método | O que indica |
|---|---|---|
| **Reboot do ESP32** | Semente = 0 | `micros()` zerou — chip reiniciou |
| **RNG travado** | Hash duplicado | Estado do gerador ciclando |
| **Entropia baixa** | Δ semente < 1.000 | Degradação física do gerador |
| **Correlação serial** | >15% repetição entre sorteios | Memória indesejada no sistema |
| **Viés estatístico** | Qui-quadrado (χ²) > 75 | Alguns números saindo mais que outros |
| **Faixa estreita** | Desvio padrão EWMA < 70% | Números concentrados numa região |
| **Sequências** | Análise de runs | Números consecutivos suspeitos |

### Score de saúde do RNG

```
🟢 SAUDÁVEL   Score ≥ 85   Tudo normal
🟡 ATENÇÃO    Score ≥ 60   Anomalias leves detectadas
🔴 CRÍTICO    Score < 60   Problema grave — verificar hardware
```

---

## 🌐 WebServer embutido

Servidor HTTP na porta 80, em task FreeRTOS dedicada no Core 1.

### Rotas disponíveis

| Rota | Descrição |
|---|---|
| `http://[IP]/` | Página visual com dados do último sorteio |
| `http://[IP]/json` | JSON com sorteio e CPU stats |
| `http://[IP]/info` | Chave HMAC, versão, entropia acumulada |

O IP é exibido no Serial Monitor na inicialização:

```
┌─────────────────────────────────────────────┐
│       ESP32 CaosForge v2.2 — Online         │
├─────────────────────────────────────────────┤
│ IP:     192.168.X.X                         │
│ Web:    http://192.168.X.X/                 │
│ JSON:   http://192.168.X.X/json             │
│ Info:   http://192.168.X.X/info             │
├─────────────────────────────────────────────┤
│ HMAC key (NVS): a3f2b9c1d4e5f6...          │
├─────────────────────────────────────────────┤
│ Aguardando 10000 iter. de entropia...       │
└─────────────────────────────────────────────┘
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
| `Preferences.h` | NVS — chave HMAC |
| `WiFi.h`, `HTTPClient.h`, `WebServer.h` | Rede e servidor |
| `<cmath>` | Guard NaN/Inf |

### Configuração no `caos.ino`

```cpp
const char* SSID         = "SEU_WIFI";
const char* PASSWORD     = "SUA_SENHA";
const char* SERVER_URL   = "http://192.168.X.X:1880/caos";

// Mesmo valor de BEARER_TOKEN nas variáveis de ambiente do Node-RED
const char* BEARER_TOKEN = "SEU_TOKEN_AQUI";
```

### Upload

1. Abra `caos.ino` na Arduino IDE
2. Placa: **ESP32 Dev Module**
3. Edite as configurações acima
4. Upload → Serial Monitor (115200 baud)

---

## 🔗 Configuração do Node-RED

### 1. Cadastrar as variáveis de ambiente

Vá em **≡ → Settings → Environment variables** e adicione:

| Nome | Valor | Descrição |
|---|---|---|
| `BEARER_TOKEN` | `SEU_TOKEN` | Token que o ESP32 envia no header |
| `GAS_TOKEN` | `SEU_TOKEN_GAS` | Token que o Apps Script valida |
| `GAS_URL` | `https://script.google.com/macros/s/SEU_ID/exec` | URL do Apps Script (sem `?token=`) |

Clique em **Save**.

> Os valores ficam armazenados localmente no servidor Node-RED e **não aparecem no `flows.json`** quando você exporta o fluxo.

### 2. Importar o fluxo

1. Menu **≡ → Import** → cole o conteúdo de `flows.json`
2. Clique em **Import → Deploy**

### 3. Configurar os nós

**Nó "✅ Verificar Bearer Token"** — dê duplo-clique e verifique:

```javascript
// Lê o token da variável de ambiente — não hardcode aqui
const TOKEN_ESPERADO = env.get('BEARER_TOKEN');

const auth = msg.req && msg.req.headers && msg.req.headers['authorization'];
if (!auth || auth !== 'Bearer ' + TOKEN_ESPERADO) {
    node.warn('[AUTH] Tentativa não autorizada — IP: ' + (msg.req && msg.req.ip || 'desconhecido'));
    node.status({fill:'red', shape:'dot', text:'Rejeitado ' + new Date().toLocaleTimeString()});
    return null;
}
node.status({fill:'green', shape:'dot', text:'OK ' + new Date().toLocaleTimeString()});
return msg;
```

**Nó "🔑 Adicionar Token GAS"** — dê duplo-clique e verifique:

```javascript
// Apenas serializa o payload — URL e token ficam no nó http request
msg.payload = JSON.stringify(msg.payload);
msg.headers = { 'Content-Type': 'application/json' };
return msg;
```

**Nó "http request"** (o que vai para o Google Sheets) — dê duplo-clique e configure:

| Campo | Valor |
|---|---|
| **Method** | POST |
| **URL** | `{{{env.GAS_URL}}}?token={{{env.GAS_TOKEN}}}` |
| **Return** | a UTF-8 string |

> Use **três chaves** `{{{ }}}` — com duas chaves `{{ }}` o Node-RED escapa os caracteres `/` e `?` da URL, quebrando a requisição.

**Nó "MONITOR DE INTEGRIDADE"** — verifique que tem **2 saídas configuradas**:
- Saída 0 → `monitor debug`
- Saída 1 → nó `http request` (Google Sheets alertas)

### 4. Deploy

Clique em **Deploy** após todas as configurações.

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

### Como verificar que os tokens estão protegidos

Após o Deploy, exporte o fluxo (**≡ → Export → Download**) e abra o JSON. Você verá:

```json
"url": "{{{env.GAS_URL}}}?token={{{env.GAS_TOKEN}}}"
```

Nenhum valor real aparece — o arquivo pode ser compartilhado ou commitado no GitHub com segurança.

---

## 📊 Configuração do Google Sheets

### Instalação do Apps Script

1. Abra a planilha → **Extensões → Apps Script**
2. No painel esquerdo, verifique se há mais de um arquivo — se houver, **exclua os extras** (três pontinhos → Excluir), deixando apenas um
3. No arquivo restante, **Ctrl+A → Delete** para limpar tudo
4. Cole o conteúdo de `google_apps_script.js`
5. Configure o token (pode ficar hardcoded aqui — o código do Apps Script é privado):
   ```javascript
   const GAS_SECRET_TOKEN = "SEU_TOKEN_GAS";  // mesmo valor de GAS_TOKEN no Node-RED
   ```
6. Salve (Ctrl+S)
7. **Implantar → Nova implantação**
   - Tipo: **Aplicativo da Web**
   - Executar como: **Eu**
   - Quem pode acessar: **Qualquer pessoa**
8. Autorize as permissões
9. Copie a URL gerada (termina em `/exec`) e cole no campo `GAS_URL` das variáveis de ambiente do Node-RED

### Testar o Apps Script

Abra no navegador (GET público, não exige token):
```
https://script.google.com/macros/s/SEU_ID/exec
```

Resposta esperada:
```json
{"status": "online", "planilha": "Nome da Planilha", "versao": "2.1-melhorado"}
```

Se retornar "Página não encontrada" → vá em **Implantar → Gerenciar implantações** e copie a URL correta de lá.

### Abas da planilha

**`Dados`** — sorteios normais:
| Data | Semente | Hash | N1 | N2 | N3 | N4 | N5 | N6 | IA_tentativa | IA_Acertos |

**`Alertas`** — anomalias detectadas pelo Monitor:
| Timestamp | Tipo | Severidade | Mensagem | Valor | Score_RNG | Status_Geral | Chi2 | Desvio_EWMA | Total_Sorteios |

---

## 🔧 Solução de problemas

### ESP32 em reboot loop

| Sintoma no Serial | Causa | Solução |
|---|---|---|
| Loop após `[NVS] Chave carregada` | Filesystem incompatível (SPIFFS/LittleFS) | Use o `caos.ino` v2.2 — sem filesystem |
| Trava antes do NVS | CP2102 sem tempo de estabilização | v2.2 usa `delay(1500)` no boot |
| Trava após conectar WiFi | Stack overflow na task WebServer | v2.2 usa stack de 10KB para o WebServer |

### Node-RED — erros comuns

| Mensagem | Causa | Solução |
|---|---|---|
| `msg properties can no longer override set node properties` | `msg.url` não funciona no Node-RED 3.x | Use `{{{env.GAS_URL}}}` no campo URL do nó |
| `non-http transport requested` | Campo URL vazio ou com valor inválido | Preencha com `{{{env.GAS_URL}}}?token={{{env.GAS_TOKEN}}}` |
| Token sempre rejeitado | Variável de ambiente não cadastrada | Verifique **Settings → Environment variables** |

### Google Apps Script — erros comuns

| Mensagem | Causa | Solução |
|---|---|---|
| `SHEET_ID has already been declared` | Dois arquivos no projeto com a mesma variável | Exclua os arquivos extras, deixe só um |
| `Página não encontrada` | URL da implantação desatualizada | Copie a URL em **Gerenciar implantações** |
| `Não autorizado` | `GAS_TOKEN` no Node-RED diferente do `GAS_SECRET_TOKEN` no script | Verifique os dois valores |

---

## 📁 Estrutura do repositório

```
ESP32CaosForge/
│
├── caos.ino                    # Firmware do ESP32 (v2.2)
│                               # → Chave HMAC persistente em NVS
│                               # → HMAC-SHA256 + sorteio sem viés
│                               # → Bearer Token no POST
│                               # → WebServer em task FreeRTOS (10KB stack)
│                               # → Entropia mínima (10k iter.) antes do 1º sorteio
│                               # → Guard NaN/Inf no tan()
│                               # → delay(1500) para estabilidade no DOIT V1
│                               # → Endpoints: /, /json, /info
│
├── flows.json                  # Fluxo Node-RED (v2.2)
│                               # → Sem tokens hardcoded — usa env vars
│                               # → Bearer Token via env.get('BEARER_TOKEN')
│                               # → URL do Sheets via {{{env.GAS_URL}}}
│                               # → MONITOR com outputs: 2 (bug corrigido)
│                               # → Pipeline completo: IA, Monitor, Sheets
│
├── google_apps_script.js       # Apps Script (v2.1)
│                               # → Token hardcoded (seguro — código privado)
│                               # → Roteamento automático Dados/Alertas
│                               # → Formatação condicional por severidade
│                               # → Health check via GET /exec
│
├── flow.jpg                    # Screenshot do fluxo Node-RED
│
└── README.md                   # Este arquivo
```

---

## 🧠 Fundamentos teóricos

**Por que hardware e não software?**
Geradores de números pseudo-aleatórios (PRNGs) como `Math.random()` ou `rand()` são algoritmos determinísticos — dada a mesma semente, produzem a mesma sequência. O ESP32 usa fontes físicas genuinamente não-determinísticas: variações de temperatura, interferências eletromagnéticas e instabilidades no oscilador de cristal.

**Por que aguardar entropia mínima?**
Logo após o boot, o pool de entropia está quase vazio. Aguardar 10.000 iterações garante que o pool foi misturado com milhares de amostras independentes antes do primeiro sorteio, eliminando qualquer previsibilidade na inicialização.

**Por que o guard no `tan()`?**
A função `tan(x)` diverge para ±∞ quando `x` se aproxima de π/2 + kπ. Em ponto flutuante, isso produz `Inf` ou `NaN`, e fazer `(uint32_t)(Inf * 1e6)` resulta em comportamento indefinido em C++. O guard com `isfinite()` detecta esses casos e usa `esp_random()` como substituto seguro.

**Por que o viés de módulo importa?**
Aplicar `% 60` sobre 256 valores (0–255) faz os números 0–15 terem 5 chances de aparecer enquanto os outros têm apenas 4. Com 10 mil sorteios, esse viés acumula ~4.000 ocorrências a mais para metade dos números — detectável estatisticamente. O CaosForge descarta os 16 bytes problemáticos (240–255), deixando 240 valores para 60 grupos perfeitos de 4.

**O que o Qui-quadrado mede?**
O teste χ² compara a frequência observada de cada número com a esperada para distribuição uniforme. Um χ² alto indica que alguns números aparecem sistematicamente mais — sinal de viés no gerador.

**Por que HMAC resiste a length extension attacks?**
SHA-256 puro permite que, conhecendo `SHA256(mensagem)`, se calcule `SHA256(mensagem ∥ extensão)` sem conhecer a mensagem. O HMAC aplica a chave em duas rodadas, quebrando essa propriedade. Com a chave na NVS e nunca exposta, ninguém consegue forjar um sorteio válido.

**Por que `vTaskDelay(1)` é obrigatório na task de entropia?**
O FreeRTOS monitora se o idle task está recebendo CPU. Uma task em loop infinito sem yield causa starvation do idle task, o watchdog dispara e o chip reseta. Com `vTaskDelay(1)`, a task cede 1ms a cada iteração — suficiente para o watchdog e ainda gera ~1.000 iterações de entropia por segundo.

**Por que variáveis de ambiente no Node-RED protegem os tokens?**
O `flows.json` exportado contém literalmente todo o código de todos os nós. Qualquer token hardcoded num nó de função aparece em texto puro no export — que pode ser compartilhado, versionado no Git ou enviado por engano. Variáveis de ambiente ficam armazenadas no servidor e são substituídas em tempo de execução, sem aparecer no arquivo exportado.

---

*Projeto open source para estudo de entropia, criptografia aplicada, sistemas embarcados e segurança em IoT.*
