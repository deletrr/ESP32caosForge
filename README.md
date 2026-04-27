# ⚡ ESP32 CaosForge

> Um gerador de números verdadeiramente aleatórios usando o comportamento físico do hardware — com segurança criptográfica e registro em tempo real no Google Sheets.

[![Platform](https://img.shields.io/badge/platform-ESP32-blue?style=flat-square)](https://www.espressif.com/en/products/socs/esp32)
[![Framework](https://img.shields.io/badge/framework-Arduino-teal?style=flat-square)](https://www.arduino.cc/)
[![Node-RED](https://img.shields.io/badge/Node--RED-3.x-red?style=flat-square)](https://nodered.org/)
[![Version](https://img.shields.io/badge/versão-2.3-brightgreen?style=flat-square)](#)
[![License](https://img.shields.io/badge/license-Open%20Source-green?style=flat-square)](#)

---

## 📋 Índice

- [O que é isso?](#-o-que-é-isso)
- [Como o sistema funciona?](#️-como-o-sistema-funciona)
- [Por que isso é seguro?](#-por-que-isso-é-seguro)
- [Monitor de saúde do gerador](#-monitor-de-saúde-do-gerador)
- [Servidor web dentro do chip](#-servidor-web-dentro-do-chip)
- [Como configurar?](#-como-configurar)
- [Algo deu errado?](#-algo-deu-errado)
- [Para os curiosos — como funciona por dentro?](#-para-os-curiosos--como-funciona-por-dentro)
- [O que tem em cada arquivo?](#-o-que-tem-em-cada-arquivo)
- [Dados ao vivo](#-dados-ao-vivo)

---

## 🤔 O que é isso?

Imagine jogar um dado. O resultado é imprevisível porque depende de forças físicas do mundo real — o ângulo do lançamento, a fricção, o ar. Agora imagine um dado digital que usa o próprio comportamento físico do chip para gerar seus números.

É exatamente isso que o **ESP32 CaosForge** faz.

O ESP32 é um microcontrolador (um computadorzinho minúsculo) com sensores físicos internos. Este projeto aproveita o **ruído térmico do chip**, variações minúsculas no relógio interno e outros fenômenos físicos reais para criar números genuinamente imprevisíveis — não uma simulação de aleatoriedade, mas a coisa real.

> 💡 **Por que isso importa?** Programas de computador normais geram números "pseudo-aleatórios" — calculados por fórmulas matemáticas e, tecnicamente, previsíveis. O CaosForge usa física real, como um dado de verdade.

---

## ⚙️ Como o sistema funciona?

O sistema é composto por três peças que trabalham juntas:

| Componente | O que faz | Onde fica |
|---|---|---|
| **ESP32 (chip)** | Gera os números aleatórios e envia pela internet | Fisicamente, na sua mesa |
| **Node-RED** | Recebe os dados e os distribui com segurança | No seu computador ou servidor |
| **Google Sheets** | Guarda um histórico público e verificável de tudo | Na nuvem (Google) |

### O chip ESP32 — "A Forja"

O ESP32 tem dois processadores internos (chamados Core 0 e Core 1) que trabalham ao mesmo tempo, cada um com uma tarefa:

- **Core 0 — "A Forja":** Fica em loop eterno, misturando três fontes de ruído físico milhares de vezes por segundo. É como um liquidificador de caos.
- **Core 1 — "O Oráculo":** A cada 60 segundos, pega o resultado dessa mistura, assina criptograficamente e envia pela internet.

> ✅ Antes de gerar qualquer número, o sistema aguarda pelo menos **10.000 rodadas** de mistura. Isso garante que o caos físico foi suficientemente acumulado — como esperar o dado parar de girar antes de olhar o resultado.

### A viagem dos dados

Após gerado no chip, cada resultado percorre este caminho com verificação de segurança em cada etapa:

```
[ESP32]  →  [Node-RED]  →  [Google Sheets]
```

---

## 🔐 Por que isso é seguro?

Existem quatro camadas de proteção, uma dentro da outra, como as cascas de uma cebola.

### Camada 1 — Assinatura digital (HMAC)

Cada resultado vem com uma "assinatura" matemática única, criada com uma chave secreta que fica guardada dentro do chip e **nunca sai de lá**. É como um selo de cera medieval — se alguém adulterar o número, a assinatura quebra e a fraude é detectada.

> 🔑 Essa chave é gerada uma única vez no primeiro boot e fica numa memória especial (NVS) que sobrevive até quando você atualiza o firmware. Ela nunca aparece em nenhuma tela ou arquivo.

### Camada 2 — Token de acesso (Bearer Token)

Para que o Node-RED aceite receber dados do ESP32, é necessária uma senha de **pelo menos 32 caracteres** gerada aleatoriamente. Sem ela, a conexão é recusada e um aviso é registrado.

### Camada 3 — Proteção anti-replay

Cada envio carrega um carimbo de data e hora. O sistema **rejeita qualquer dado com mais de 5 minutos de atraso** — isso impede que alguém grave um envio legítimo e o reenvie depois para tentar manipular o sistema.

### Camada 4 — Senhas no ambiente, não no código

As senhas e tokens ficam nas configurações do servidor, não escritas no código. Você pode publicar o código no GitHub sem vazar nenhuma credencial.

> ⚠️ **Atenção:** Resetar a chave HMAC apaga todo o histórico verificável. Resultados anteriores não poderão mais ser confirmados como legítimos. Faça isso só se realmente necessário.

---

## 📊 Monitor de Saúde do Gerador

O sistema acompanha continuamente a "saúde" do gerador. Se algo parecer errado — hardware com defeito, chip reiniciado, padrões suspeitos — ele detecta e registra um alerta automático no Google Sheets.

| O que é verificado | O que pode indicar |
|---|---|
| Semente igual a zero | O chip foi reiniciado inesperadamente |
| Hash repetido | O gerador travou e está ciclando os mesmos valores |
| Pouca variação nos números | Degradação física do hardware |
| Muita repetição entre resultados | O sistema está "lembrando" resultados anteriores |
| Teste estatístico (χ²) alto | Alguns valores saindo mais do que deveriam |
| Desvio padrão muito baixo | Resultados concentrados numa faixa estreita |

Cada análise gera um **Score de Saúde**:

| Score | Status |
|---|---|
| 85 ou mais | 🟢 Saudável — tudo normal |
| 60 a 84 | 🟡 Atenção — anomalias leves |
| Abaixo de 60 | 🔴 Crítico — verificar o hardware físico |

---

## 🌐 Servidor web dentro do chip

O próprio ESP32 roda um servidor web miniatura. Você pode acessá-lo pelo navegador na rede local digitando o IP do chip:

| Endereço | O que você vê |
|---|---|
| `http://[IP]/` | Página visual com o último resultado — fácil de ler |
| `http://[IP]/json` | Dados técnicos: semente, hash, temperatura do chip, uso dos processadores |
| `http://[IP]/info` | Status geral do sistema — sem expor nenhuma chave secreta |

> 🌡️ O chip também monitora sua própria temperatura interna. O valor não é preciso (margem de ±5–10°C), mas variações indicam quanto o processador está trabalhando.

---

## 💻 Como configurar?

### Parte 1 — Preparando o ESP32

Você vai precisar da **Arduino IDE** (programa gratuito para programar chips).

1. Abra o arquivo `caos.ino` na Arduino IDE
2. Preencha suas informações de Wi-Fi:
   ```cpp
   const char* SSID     = "NOME_DA_SUA_REDE";
   const char* PASSWORD = "SENHA_DO_WIFI";
   ```
3. Gere uma senha forte e cole no campo `BEARER_TOKEN`:
   ```bash
   openssl rand -hex 32
   ```
4. Selecione a placa **ESP32 Dev Module** e faça o upload
5. Abra o Monitor Serial (115200 baud) e procure a mensagem:
   ```
   Bearer token forte (>=32): SIM
   ```

### Parte 2 — Configurando o Node-RED

1. Vá em **Menu (☰) → Settings → Environment variables** e cadastre:

   | Nome | Valor |
   |---|---|
   | `BEARER_TOKEN` | A mesma senha usada no chip |
   | `GAS_TOKEN` | Uma nova senha para o Google Sheets (`openssl rand -hex 32`) |
   | `GAS_URL` | O endereço do seu Apps Script (gerado na Parte 3) |

2. Importe o fluxo: **Menu → Import** → cole o `flows.json` → **Deploy**

> 💡 As senhas ficam nas variáveis de ambiente e **não aparecem no `flows.json`** quando você exporta. Pode compartilhar o arquivo sem risco.

### Parte 3 — Configurando o Google Sheets

1. Abra sua planilha → **Extensões → Apps Script**
2. Apague arquivos extras no painel (deixe só um)
3. Cole o conteúdo de `google_apps_script.js` e configure o token:
   ```javascript
   const GAS_SECRET_TOKEN = "SEU_TOKEN_GAS"; // mesmo GAS_TOKEN do Node-RED
   ```
4. Salve e clique em **Implantar → Nova implantação**
   - Tipo: **Aplicativo da Web**
   - Executar como: **Eu**
   - Quem pode acessar: **Qualquer pessoa**
5. Copie a URL gerada e cole como `GAS_URL` nas variáveis do Node-RED

Para testar, acesse a URL diretamente no navegador. Você deve ver:
```json
{ "status": "online", "versao": "2.3" }
```

---

## 🔧 Algo deu errado?

### Problemas com o ESP32

| Sintoma | Causa provável | Solução |
|---|---|---|
| Chip reiniciando em loop | Versão antiga com problema de filesystem | Use o `caos.ino` v2.3 |
| `Bearer token forte: NAO` no serial | Senha muito curta | Gere com `openssl rand -hex 32` |
| `Falha ao criar mutexes` | Memória insuficiente | Verifique outras alocações grandes no `setup()` |

### Problemas com o Node-RED

| Mensagem de erro | Solução |
|---|---|
| `msg properties can no longer override set node properties` | Use `{{{env.GAS_URL}}}` no campo URL do nó http request |
| `non-http transport requested` | Verifique se o campo URL está preenchido corretamente |
| Token sempre rejeitado | Confirme que `BEARER_TOKEN` foi cadastrado em Settings |

### Problemas com o Google Apps Script

| Mensagem de erro | Solução |
|---|---|
| `SHEET_ID has already been declared` | Apague arquivos extras no projeto — deixe só um |
| `Página não encontrada` | Vá em **Implantar → Gerenciar implantações** e copie a URL atual |
| `Payload expirado (> 5 min)` | Verifique se o relógio do ESP32 ou do Node-RED está sincronizado |
| `Não autorizado` | Confirme que `GAS_TOKEN` no Node-RED é igual a `GAS_SECRET_TOKEN` no script |

---

## 🧠 Para os curiosos — como funciona por dentro?

### Por que hardware e não software?

Programas de computador são determinísticos — dada a mesma entrada, sempre produzem a mesma saída. Mesmo funções como `Math.random()` seguem fórmulas matemáticas previsíveis. O ESP32 usa fenômenos físicos reais que não podem ser reproduzidos: ruído térmico, instabilidades no oscilador de cristal e interferências eletromagnéticas microscópicas.

### Por que esperar 10.000 iterações?

Logo após ligar, o chip está "frio" — o reservatório de aleatoriedade ainda está quase vazio. As 10.000 iterações garantem tempo suficiente para acumular ruído físico real antes do primeiro resultado. É como agitar bem um coquetel antes de servir.

### O que é HMAC e por que resiste a ataques?

SHA-256 puro tem uma vulnerabilidade: conhecendo o hash de uma mensagem, dá para calcular o hash de uma mensagem maior sem conhecer a original. O HMAC aplica a chave secreta em duas etapas, quebrando essa propriedade. Com a chave guardada na NVS e nunca exposta, ninguém consegue forjar um resultado válido mesmo conhecendo todos os outros dados.

### Por que descartar alguns bytes ao calcular os números?

Suponha que você quer gerar números de 1 a 60 usando bytes aleatórios (valores de 0 a 255). Se aplicar `byte % 60` diretamente, os números 0 a 15 têm 5 chances de aparecer, enquanto os outros têm só 4. Com muitos resultados, isso cria um viés perceptível. A solução é descartar os bytes de 240 a 255 — sobram 240 valores que se dividem perfeitamente em 60 grupos iguais, sem viés.

### Por que dois tipos de proteção para partes diferentes do código?

`portENTER_CRITICAL` desativa interrupções e é ideal para proteger operações brevíssimas entre um sinal de hardware e uma tarefa no mesmo processador. Mas entre duas tarefas em processadores diferentes, pode causar deadlock (os dois ficam esperando um pelo outro para sempre). Para esses casos, o projeto usa `SemaphoreHandle_t` — o mecanismo correto do FreeRTOS, com timeout e sem risco de travamento.

---

## 📁 O que tem em cada arquivo?

```
ESP32CaosForge/
├── caos.ino                 # Firmware completo — código que roda dentro do ESP32
├── flows.json               # Fluxo do Node-RED — importe direto pela interface
├── google_apps_script.js    # Script que roda dentro do Google Sheets
├── flow.jpg                 # Imagem do fluxo Node-RED para referência visual
└── README.md                # Este guia
```

---

## 📈 Dados ao vivo

Os resultados são registrados em tempo real nesta planilha pública:

🔗 **[Acessar Google Sheets](https://docs.google.com/spreadsheets/d/1qoPtb4fNSjBl3aQU2CqS8u9W14VDHrp8trGWmSIrbM0/edit?usp=sharing)**

---

*Projeto open source para estudo de entropia, criptografia aplicada, sistemas embarcados e segurança em IoT.*
