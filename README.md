# ⚡ ESP32 CaosForge

> **Gerador de entropia física com sorteio criptográfico verificável, autenticação em toda a cadeia e monitoramento estatístico em tempo real via Node-RED e Google Sheets.**

[![Platform](https://img.shields.io/badge/platform-ESP32-blue?style=flat-square)](https://www.espressif.com/en/products/socs/esp32)
[![Framework](https://img.shields.io/badge/framework-Arduino-teal?style=flat-square)](https://www.arduino.cc/)
[![Node-RED](https://img.shields.io/badge/Node--RED-3.x-red?style=flat-square)](https://nodered.org/)
[![License](https://img.shields.io/badge/license-Open%20Source-green?style=flat-square)](#)

---

## 📋 Índice

- [O que é](#-o-que-é)
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
- [Dados ao vivo](#-dados-ao-vivo)

---

## 🌀 O que é

O **ESP32 CaosForge** é um sistema de geração de aleatoriedade baseada em entropia física real. Em vez de depender de algoritmos de software (que são pseudo-aleatórios e determinísticos), o projeto explora o comportamento caótico do hardware do ESP32 — ruído térmico do chip, jitter de clock, variações do oscilador interno — para produzir números genuinamente imprevisíveis.

O resultado é um pipeline completo de sorteio auditável e autenticado:

```
[Entropia Física] → [HMAC-SHA256 / chave NVS] → [Sorteio 1–60]
      → [Bearer Token] → [Node-RED] → [URL fixa + token] → [Google Sheets]
```

Cada sorteio é acompanhado de uma assinatura HMAC que permite verificação criptográfica independente. Toda a comunicação entre componentes é autenticada por token — sem autenticação, nenhum dado é processado.

---


## ⚙️ Como funciona

O ESP32 possui dois núcleos de processamento independentes, e o CaosForge os usa de forma deliberada:

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

O resultado é uma variável que muda ~1.000 vezes por segundo, impossível de prever ou reproduzir.

### Core 1 — O Oráculo

A cada 60 segundos, executa o pipeline criptográfico — mas somente após o pool atingir 10.000 iterações:

1. Verifica `contadorEntrop ≥ 10.000`
2. Captura `entropia_viva` de forma thread-safe (spinlock FreeRTOS)
3. Combina com `micros()` e `esp_random()` para a semente final
4. Gera **HMAC-SHA256** usando a chave da NVS
5. Sorteia 6 números únicos de 1 a 60 sem viés estatístico
6. Envia via HTTP POST com **Bearer Token** no header

### WebServer — Task independente

O WebServer roda em sua própria task FreeRTOS no Core 1. Isso elimina o bloqueio que existia no `loop()` original:

```cpp
void taskWebServer(void* pvParameters) {
  for (;;) {
    server.handleClient();
    vTaskDelay(2);
  }
}
```

### Por que HMAC e não SHA-256 simples?

O HMAC (RFC 2104) mistura a chave secreta com os dados de forma padronizada e resistente a *length extension attacks*. Com a chave armazenada na NVS, mesmo quem lê o código-fonte não consegue calcular ou forjar hashes.

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
│  │  isfinite()    │      │  Temp. do die              │  │
│  │  vTaskDelay(1) │      │  HTTP POST + Bearer Token  │  │
│  └────────────────┘      │  WebServer :80 (task)      │  │
│                          └──────────────┬─────────────┘  │
│  ┌────────────────┐                     │                 │
│  │      NVS       │◀────────────────────┘                 │
│  │  hmac_key[32]  │  (gerada no 1º boot, persiste)        │
│  └────────────────┘                                       │
└──────────────────────────────────┬───────────────────────┘
                    ┌──────────────┴───────────────────┐
                    │ JSON POST                        │ HTTP GET
                    │ Authorization: Bearer <token>    │
                    ▼                                  ▼
     ┌──────────────────────────┐     ┌───────────────────────────┐
     │         Node-RED         │     │  Browser / rede local     │
     │                          │     │  http://[IP]/             │
     │  ✅ Verificar Bearer     │     │  http://[IP]/json         │
     │  ↓                       │     │  http://[IP]/info         │
     │  json parser             │     └───────────────────────────┘
     │  ↓ ↓ ↓ ↓ ↓              │
     │  [pipeline]              │
     │  ↓ IA                    │
     │  ↓ MONITOR (outputs: 2)  │
     │    ↓saída 0  ↓saída 1    │
     │   debug    http request  │
     └──────────────┬───────────┘
                    │ POST URL?token=<gas_token>
         ┌──────────┴──────────┐
         ▼                     ▼
   ┌───────────┐    ┌─────────────────┐
   │   Dados   │    │    Alertas      │
   │ (Sheets)  │    │   (Sheets)      │
   └───────────┘    └─────────────────┘
```

---

## 🔐 Segurança criptográfica

### Chave HMAC persistente na NVS
A chave HMAC **não existe no código-fonte**. No primeiro boot, o ESP32 gera 32 bytes aleatórios e salva na NVS:

```cpp
// Ocorre apenas uma vez — primeiro boot
for (int i = 0; i < 32; i++) {
  hmac_key[i] = (uint8_t)((esp_random() ^ (uint32_t)micros()) & 0xFF);
}
prefs.putBytes("hmac_key", hmac_key, 32);
```

A chave persiste após reflash enquanto a NVS não for formatada. Para visualizá-la, acesse `http://[IP]/info` ou leia o Serial Monitor na inicialização.

### Geração da semente

```cpp
uint32_t semente = capturarEntropia()   // Pool acumulado: 10k+ iterações
                 ^ (uint32_t)micros()   // Jitter de clock
                 ^ esp_random();        // RNG de hardware
```

### Sorteio sem viés de módulo

```
Bytes válidos: 0–239  →  240 valores
240 ÷ 60 = 4 grupos exatos  →  probabilidade uniforme ✅
Bytes descartados: 240–255  →  eliminam o viés de módulo
```

### Proteção de race condition (dual-core)

```cpp
portENTER_CRITICAL(&entropiaMux);
val = entropia_viva;
portEXIT_CRITICAL(&entropiaMux);
```

`volatile` sozinho não é suficiente no ESP32 dual-core — o spinlock do FreeRTOS garante consistência real.

### Resetar a chave HMAC

Caso queira forçar geração de uma nova chave:

1. Adicione temporariamente no `setup()`:
   ```cpp
   resetarChaveHMAC();
   ```
2. Faça upload, anote a nova chave no Serial Monitor
3. **Remova a linha** e faça upload novamente

> ⚠️ Resetar a chave invalida todos os hashes históricos.

---

## 🔒 Autenticação em toda a cadeia

### ESP32 → Node-RED: Bearer Token

Cada POST enviado pelo ESP32 inclui o header:

```
Authorization: Bearer SEU_TOKEN_AQUI
```

O nó **"✅ Verificar Bearer Token"** bloqueia qualquer requisição sem o token correto antes de qualquer processamento.

### Node-RED → Google Sheets: Token na URL

O token é configurado **diretamente no campo URL do nó http request**, junto com a URL do Apps Script:

```
https://script.google.com/macros/s/SEU_ID/exec?token=SEU_TOKEN_GAS
```

Isso contorna a limitação do Node-RED 3.x que não permite sobrescrever o campo URL via `msg.url`.

### Diagrama

```
ESP32 ──[Bearer Token no header]──▶ Node-RED ──[URL fixa com ?token=]──▶ Google Sheets
```

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
| **Viés estatístico** | Qui-quadrado (χ²) > 75 | Alguns números saindo mais que outros |
| **Faixa estreita** | Desvio padrão EWMA < 70% | Números concentrados numa região |
| **Sequências** | Análise de runs | Números consecutivos suspeitos |

### Score de saúde do RNG

```
🟢 SAUDÁVEL   Score ≥ 85   Tudo normal
🟡 ATENÇÃO    Score ≥ 60   Anomalias leves detectadas
🔴 CRÍTICO    Score < 60   Problema grave — verificar hardware
```

Alertas são gravados automaticamente na aba **"Alertas"** do Google Sheets com timestamp, tipo, severidade e métricas contextuais.

---

## 🌐 WebServer embutido

Servidor HTTP na porta 80, acessível por qualquer dispositivo na rede local. Roda em task FreeRTOS dedicada no Core 1 — não interfere no timing do sorteio.

### Rotas disponíveis

| Rota | Descrição |
|---|---|
| `http://[IP]/` | Página visual com dados do último sorteio |
| `http://[IP]/json` | JSON com sorteio e CPU stats |
| `http://[IP]/info` | Chave HMAC, versão, entropia acumulada |

O IP é exibido no Serial Monitor na inicialização:

```
┌─────────────────────────────────────────────┐
│       ESP32 CaosForge v2.1 — Online         │
├─────────────────────────────────────────────┤
│ IP:     192.168.X.X                         │
│ Web:    http://192.168.X.X/                 │
│ JSON:   http://192.168.X.X/json             │
│ Info:   http://192.168.X.X/info             │
├─────────────────────────────────────────────┤
│ HMAC key (NVS):                             │
│  a3f2b9c1d4e5f6...                          │
├─────────────────────────────────────────────┤
│ Aguardando 10000 iter. de entropia...       │
└─────────────────────────────────────────────┘
```

### O que a página exibe

- **Barra de progresso** durante o aquecimento do gerador (antes do 1º sorteio)
- **Bolinhas dos 6 números** do último sorteio
- Semente, data/hora e número do sorteio
- Hash HMAC-SHA256 completo
- Temperatura do die do chip com barra colorida
- Uso de CPU por núcleo (barras animadas)
- IP, uptime, RSSI, heap livre e iterações de entropia
- **Auto-refresh** a cada 65 segundos

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

### Resposta `/info`

```json
{
  "versao": "2.1",
  "hmac_key_hex": "a3f2b9c1d4e5f6...",
  "hmac_fonte": "NVS",
  "bearer_token_set": true,
  "entropia_iteracoes": 183420,
  "entropia_minima": 10000,
  "ip": "192.168.1.42",
  "uptime_s": 3721,
  "heap_livre_kb": 214
}
```

---

## 🌡️ Temperatura do die (CPU)

O ESP32 possui um sensor de temperatura interno que mede o **die do chip** — o silício em si, não o ambiente ao redor.

```cpp
extern "C" uint8_t temprature_sens_read();  // typo intencional no SDK Espressif

float getTempCPU() {
  return (temprature_sens_read() - 32) / 1.8f;  // Fahrenheit → Celsius
}
```

O sensor tem imprecisão de **±5–10°C**. O valor absoluto não é confiável como termômetro, mas a variação é relevante:

| Temperatura | O que indica |
|---|---|
| Subindo gradualmente | Carga crescente do Core 0 gerando entropia |
| Pico após sorteio | HMAC-SHA256 aquecendo o processador |
| Queda para valor fixo | Possível reboot do chip |

```
🔵 Azul     < 55°C   Normal
🟠 Laranja  55–70°C  Carga moderada
🔴 Vermelho  > 70°C  Carga alta
```

### Compatibilidade

| Chip | Suporte |
|---|---|
| ESP32 clássico (DOIT DEVKIT V1, dual-core Xtensa LX6) | ✅ `temprature_sens_read()` |
| ESP32-S2 / ESP32-C3 / ESP32-S3 | ⚠️ API diferente — requer adaptação |

---

## 💻 Configuração do ESP32

### Dependências

Todas incluídas no **ESP32 Arduino Core** — nenhuma instalação adicional:

| Biblioteca | Uso |
|---|---|
| `mbedtls/md.h` | HMAC-SHA256 |
| `Preferences.h` | NVS — chave HMAC persistente |
| `WiFi.h`, `HTTPClient.h`, `WebServer.h` | Rede e servidor |
| `<cmath>` | Guard NaN/Inf no `tan()` |

### Configuração no código (`caos.ino`)

Edite apenas a seção de configurações no topo:

```cpp
const char* SSID         = "SEU_WIFI";
const char* PASSWORD     = "SUA_SENHA";
const char* SERVER_URL   = "http://192.168.X.X:1880/caos";  // IP do PC com Node-RED

// Bearer Token — mesmo valor que TOKEN_ESPERADO no Node-RED
const char* BEARER_TOKEN = "SEU_TOKEN_AQUI";
```

Para encontrar o IP do seu computador:
- **Windows:** `ipconfig` no terminal
- **Linux/Mac:** `ip addr` ou `ifconfig`

### Sobre a chave HMAC

Não é necessário configurar nada. No primeiro boot o ESP32 gera e salva automaticamente. A chave aparece no Serial Monitor e em `http://[IP]/info`.

### Upload

1. Abra `caos.ino` na Arduino IDE
2. Selecione a placa: **ESP32 Dev Module**
3. Edite as configurações acima
4. Faça o upload
5. Abra o Serial Monitor (115200 baud)

---

## 🔗 Configuração do Node-RED

### Importando o fluxo

1. Abra o Node-RED (`http://localhost:1880`)
2. Menu **≡ → Import** → cole o conteúdo de `flows.json`
3. Clique em **Import → Deploy**

### Configurações obrigatórias após importar

**1. Nó "✅ Verificar Bearer Token"** — dê duplo-clique e edite:

```javascript
const TOKEN_ESPERADO = 'SEU_TOKEN_AQUI';  // mesmo BEARER_TOKEN do caos.ino
```

**2. Nó "http request"** (o que vai para o Google Sheets) — dê duplo-clique e configure:

| Campo | Valor |
|---|---|
| **Method** | POST |
| **URL** | `https://script.google.com/macros/s/SEU_ID/exec?token=SEU_TOKEN_GAS` |
| **Return** | a UTF-8 string |

> A URL já inclui o token como parâmetro fixo. Não use `msg.url` — no Node-RED 3.x `msg.url` não sobrescreve o campo do nó e causa o erro `msg properties can no longer override`.

**3. Nó "MONITOR DE INTEGRIDADE"** — verifique que está com **2 saídas**:
- Saída 0 → `monitor debug`
- Saída 1 → nó `http request` (Google Sheets)

Após configurar tudo: **Deploy**.

### Estrutura do fluxo

```
[POST /caos]
     │
     ├──▶ [http response]              ← Responde 200 ao ESP32 imediatamente
     │
     └──▶ [✅ Verificar Bearer Token]  ← Bloqueia se token inválido
               │
               └──▶ [json parser]
                         │
                         ├──▶ [hash]      ──▶ Dashboard
                         ├──▶ [sementes]  ──▶ Dashboard
                         ├──▶ [numeros]   ──▶ Dashboard
                         ├──▶ [String]    ──▶ Debug
                         ├──▶ [Learning]  ──▶ Análise de tendências
                         ├──▶ [IA]        ──▶ [http request → Sheets Dados]
                         └──▶ [MONITOR DE INTEGRIDADE] (outputs: 2)
                                  ├── saída 0 ──▶ monitor debug
                                  └── saída 1 ──▶ [http request → Sheets Alertas]
```

---

## 📊 Configuração do Google Sheets

### Apps Script

O script suporta duas abas automaticamente e valida o token em todas as requisições.

**Aba `Dados`** — sorteios normais:
| Data | Semente | Hash | N1 | N2 | N3 | N4 | N5 | N6 | IA_tentativa | IA_Acertos |

**Aba `Alertas`** — anomalias do Monitor:
| Timestamp | Tipo | Severidade | Mensagem | Valor | Score_RNG | Status_Geral | Chi2 | Desvio_EWMA | Total_Sorteios |

### Instalação

1. Abra a planilha → **Extensões → Apps Script**
2. No painel esquerdo, verifique se existe mais de um arquivo. Se houver, **exclua todos exceto o principal** (três pontinhos → Excluir)
3. No arquivo restante, **selecione tudo (Ctrl+A) e delete**
4. Cole o conteúdo de `google_apps_script.js`
5. No topo do script, configure o token:
   ```javascript
   const GAS_SECRET_TOKEN = "SEU_TOKEN_GAS";  // mesmo token da URL no Node-RED
   ```
6. Salve (Ctrl+S)
7. **Implantar → Nova implantação**
   - Tipo: **Aplicativo da Web**
   - Executar como: **Eu**
   - Quem pode acessar: **Qualquer pessoa**
8. Autorize as permissões
9. **Copie a URL gerada** (termina em `/exec`)
10. Cole no campo URL do nó **http request** do Node-RED: `[URL]/exec?token=SEU_TOKEN_GAS`

### Testar antes de esperar o ESP32

Abra no navegador:
```
https://script.google.com/macros/s/SEU_ID/exec
```

Resposta esperada:
```json
{"status": "online", "planilha": "Nome da Planilha", ...}
```

Se retornar "Página não encontrada" → a URL está errada. Vá em **Implantar → Gerenciar implantações** e copie a URL correta de lá.

---

## 🔧 Solução de problemas

### ESP32 em reboot loop

| Sintoma no Serial | Causa | Solução |
|---|---|---|
| Trava após `[NVS] Chave carregada` | Filesystem (SPIFFS/LittleFS) incompatível | Use o `caos.ino` v2.1 que não usa filesystem |
| Trava antes do NVS | `delay()` insuficiente no boot | Versão v2.1 usa `delay(1500)` |
| Trava após WiFi conectar | Stack overflow na task | Versão v2.1 usa 10KB de stack para o WebServer |

### Node-RED — erros comuns

| Mensagem | Causa | Solução |
|---|---|---|
| `msg properties can no longer override set node properties` | `msg.url` no Node-RED 3.x não funciona | Coloque a URL diretamente no campo do nó http request |
| `non-http transport requested` | Campo URL do nó estava vazio ou com `{{{url}}}` | Preencha com a URL completa incluindo `?token=` |

### Google Apps Script — erros comuns

| Mensagem | Causa | Solução |
|---|---|---|
| `SHEET_ID has already been declared` | Há dois arquivos no projeto com a mesma variável | Apague todos os arquivos extras, deixe só um com o conteúdo do `google_apps_script.js` |
| `Página não encontrada` (HTML do Google Drive) | URL da implantação desatualizada | Vá em **Gerenciar implantações** e copie a URL atual |
| Token inválido / `Não autorizado` | Token no Node-RED diferente do Apps Script | Verifique se `?token=` na URL do Node-RED bate com `GAS_SECRET_TOKEN` no script |

---

## 📁 Estrutura do repositório

```
ESP32CaosForge/
│
├── caos.ino                    #→ Chave HMAC persistente em NVS
│                               # → HMAC-SHA256 + sorteio sem viés
│                               # → Bearer Token no POST
│                               # → WebServer em task FreeRTOS
│                               # → Entropia mínima (10k iter.) antes do 1º sorteio
│                               # → Guard NaN/Inf no tan()
│                               # → delay(1500) para estabilidade no DOIT V1
│                               # → Endpoints: /, /json, /info
│
├── flows.json                  
│                               # → Bearer Token antes do json parser
│                               # → MONITOR com outputs: 2 (bug corrigido)
│                               # → URL do http request com token fixo na URL
│                               # → Pipeline completo: IA, Monitor, Sheets
│
├── google_apps_script.js       
│                               # → Validação de token via ?token= na URL
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
Logo após o boot, o pool de entropia está quase vazio — `micros()` vale apenas alguns milhares e `esp_random()` acabou de inicializar. Aguardar 10.000 iterações garante que o pool foi misturado com milhares de amostras independentes antes do primeiro sorteio.

**Por que o guard no `tan()`?**
A função `tan(x)` diverge para ±∞ quando `x` se aproxima de π/2 + kπ. Em ponto flutuante, isso produz `Inf` ou `NaN`. Fazer `(uint32_t)(Inf * 1e6)` resulta em comportamento indefinido em C++. O guard detecta esses casos com `isfinite()` e usa `esp_random()` como substituto.

**Por que o viés de módulo importa?**
Se você aplica `% 60` sobre 256 valores (0–255), os números 0–15 têm 5 chances de aparecer enquanto os outros têm apenas 4. Com 10 mil sorteios, esse viés acumula ~4.000 ocorrências a mais para metade dos números — detectável estatisticamente. O CaosForge descarta os 16 bytes problemáticos (240–255), deixando exatamente 240 valores para 60 grupos perfeitos.

**O que o Qui-quadrado mede?**
O teste χ² compara a frequência observada de cada número com a frequência esperada para uma distribuição uniforme. Um χ² alto indica que alguns números aparecem sistematicamente mais que outros — sinal de viés no gerador.

**Por que HMAC resiste a length extension attacks?**
SHA-256 puro tem uma propriedade chamada *length extension*: conhecendo `SHA256(mensagem)`, é possível calcular `SHA256(mensagem ∥ extensão)` sem conhecer a mensagem original. O HMAC (RFC 2104) aplica a chave em duas rodadas, quebrando essa propriedade. Com a chave na NVS e nunca exposta, ninguém consegue forjar um sorteio válido mesmo capturando todos os hashes transmitidos.

**Por que `vTaskDelay(1)` é obrigatório na task de entropia?**
O FreeRTOS do ESP32 usa um watchdog timer que monitora se o idle task está recebendo tempo de CPU. Uma task em loop infinito sem nenhum yield causa starvation do idle task, o watchdog dispara e o chip reseta. Com `vTaskDelay(1)`, a task cede 1ms a cada iteração — suficiente para o watchdog e ainda gera ~1.000 iterações de entropia por segundo.

---

## 📈 Dados ao vivo

Os sorteios são registrados em tempo real nesta planilha pública:

🔗 **[Acessar Google Sheets](https://docs.google.com/spreadsheets/d/1qoPtb4fNSjBl3aQU2CqS8u9W14VDHrp8trGWmSIrbM0/edit?usp=sharing)**

---

*Projeto open source para estudo de entropia, criptografia aplicada, sistemas embarcados e segurança em IoT.*
