// ╔══════════════════════════════════════════════════════════════════════╗
// ║       ESP32 CaosForge — Google Apps Script Melhorado                ║
// ║                                                                      ║
// ║  MELHORIAS APLICADAS:                                                ║
// ║  [1] Validação de token secreto via parâmetro na URL                 ║
// ║      (Google Apps Script não expõe headers HTTP diretamente)         ║
// ║  [2] Proteção contra replay: rejeita payloads com timestamp          ║
// ║      mais antigo que 5 minutos                                       ║
// ║  [3] Endpoint GET /exec retorna status de saúde real                 ║
// ║  [4] Limite de linhas por aba para evitar planilha infinita          ║
// ║                                                                      ║
// ║  COMO USAR:                                                          ║
// ║  Ao implantar, adicione ?token=SEU_TOKEN_SECRETO_AQUI na URL.       ║
// ║  Configure o mesmo token no nó "requisição http" do Node-RED.        ║
// ╚══════════════════════════════════════════════════════════════════════╝

// ─── [1] TOKEN DE AUTENTICAÇÃO ─────────────────────────────────────────
// IMPORTANTE: troque pelo mesmo valor usado no Node-RED (campo URL do nó
// "requisição http"). Ex: ?token=a7f3d9e2c1b4...
const GAS_SECRET_TOKEN = "SUA_TOKEN_AQUI";

// ─── [4] LIMITE DE LINHAS POR ABA ──────────────────────────────────────
// Quando a aba atingir este limite, as linhas mais antigas são removidas
const MAX_LINHAS_DADOS   = 5000;
const MAX_LINHAS_ALERTAS = 2000;

const SHEET_ID     = SpreadsheetApp.getActiveSpreadsheet().getId();
const ABA_DADOS    = "Dados";
const ABA_ALERTAS  = "Alertas";

const HEADER_DADOS = [
  "Data", "Semente", "Hash",
  "N1", "N2", "N3", "N4", "N5", "N6",
  "IA_tentativa", "IA_Acertos"
];

const HEADER_ALERTAS = [
  "Timestamp", "Tipo", "Severidade", "Mensagem",
  "Valor", "Score_RNG", "Status_Geral",
  "Chi2", "Desvio_EWMA", "Total_Sorteios"
];

// ─── HELPERS ───────────────────────────────────────────────────────────

function garantirAba(nome, cabecalho) {
  const ss  = SpreadsheetApp.getActiveSpreadsheet();
  let sheet = ss.getSheetByName(nome);
  if (!sheet) {
    sheet = ss.insertSheet(nome);
    const headerRange = sheet.getRange(1, 1, 1, cabecalho.length);
    headerRange.setValues([cabecalho]);
    headerRange.setFontWeight("bold");
    headerRange.setBackground(nome === ABA_ALERTAS ? "#b71c1c" : "#1a237e");
    headerRange.setFontColor("#ffffff");
    headerRange.setHorizontalAlignment("center");
    sheet.setFrozenRows(1);
    cabecalho.forEach((_, i) => sheet.setColumnWidth(i + 1, 160));
  }
  return sheet;
}

function formatarData(d) {
  const p = n => String(n).padStart(2, '0');
  return `${p(d.getDate())}/${p(d.getMonth()+1)}/${d.getFullYear()} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

function corSeveridade(severidade) {
  switch ((severidade || "").toUpperCase()) {
    case "CRITICO": return "#ffcdd2";
    case "AVISO":   return "#fff9c4";
    default:        return "#f1f8e9";
  }
}

// [4] Remove linhas antigas quando o limite é atingido
function limitarLinhas(sheet, maxLinhas) {
  const total = sheet.getLastRow();
  if (total > maxLinhas + 1) {  // +1 para o cabeçalho
    const excesso = total - maxLinhas - 1;
    sheet.deleteRows(2, excesso);  // deleta logo após o cabeçalho (linha 2)
  }
}

// ─── [1] VALIDAÇÃO DE TOKEN ────────────────────────────────────────────

function validarToken(e) {
  const token = e.parameter && e.parameter.token;
  if (!token || token !== GAS_SECRET_TOKEN) {
    return false;
  }
  return true;
}

// ─── [2] VALIDAÇÃO DE TIMESTAMP (anti-replay) ──────────────────────────
// Aceita payloads com até 5 minutos de atraso
function timestampRecente(tsString) {
  try {
    if (!tsString) return true;  // sem timestamp: aceita (compatibilidade)
    // Formato esperado: DD/MM/YYYY HH:MM:SS
    const partes = tsString.match(/(\d+)\/(\d+)\/(\d+) (\d+):(\d+):(\d+)/);
    if (!partes) return true;
    const [,d, m, y, h, min, s] = partes;
    const data = new Date(y, m-1, d, h, min, s);
    const agora = new Date();
    const diffMs = Math.abs(agora - data);
    return diffMs < 5 * 60 * 1000;  // 5 minutos em ms
  } catch (e) {
    return true;  // em caso de erro de parse: aceita
  }
}

// ─── HANDLERS PRINCIPAIS ───────────────────────────────────────────────

function doPost(e) {
  try {
    // [1] Valida token antes de qualquer processamento
    if (!validarToken(e)) {
      return ContentService
        .createTextOutput(JSON.stringify({ status: "erro", mensagem: "Não autorizado" }))
        .setMimeType(ContentService.MimeType.JSON);
    }

    const data = JSON.parse(e.postData.contents);

    // [2] Verifica se o payload é recente (anti-replay)
    const ts = data.timestamp || data.timestamp_nodered;
    if (!timestampRecente(ts)) {
      return ContentService
        .createTextOutput(JSON.stringify({ status: "erro", mensagem: "Payload expirado (> 5 min)" }))
        .setMimeType(ContentService.MimeType.JSON);
    }

    if (data.alertas !== undefined || data.saude !== undefined) {
      return salvarAlertas(data);
    } else {
      return salvarDadosNormais(data);
    }
  } catch (err) {
    return ContentService
      .createTextOutput(JSON.stringify({ status: "erro", mensagem: err.message }))
      .setMimeType(ContentService.MimeType.JSON);
  }
}

function salvarDadosNormais(data) {
  const sheet    = garantirAba(ABA_DADOS, HEADER_DADOS);
  const timestamp = formatarData(new Date());
  const numeros   = data.numeros || [];

  const linha = [
    timestamp,
    data.semente      || "",
    data.hash         || "",
    numeros[0] || "", numeros[1] || "", numeros[2] || "",
    numeros[3] || "", numeros[4] || "", numeros[5] || "",
    data.ia_tentativa || "",
    data.ia_acertos   !== undefined ? data.ia_acertos : ""
  ];

  sheet.appendRow(linha);
  const ultimaLinha = sheet.getLastRow();
  sheet.getRange(ultimaLinha, 1, 1, linha.length)
       .setBackground("#e8f5e9")
       .setBorder(false, false, true, false, false, false, "#c8e6c9", SpreadsheetApp.BorderStyle.SOLID);

  // [4] Limita o tamanho da aba
  limitarLinhas(sheet, MAX_LINHAS_DADOS);

  return ContentService
    .createTextOutput(JSON.stringify({ status: "ok", tipo: "sorteio", linha: ultimaLinha }))
    .setMimeType(ContentService.MimeType.JSON);
}

function salvarAlertas(data) {
  const sheet         = garantirAba(ABA_ALERTAS, HEADER_ALERTAS);
  const alertas       = data.alertas        || [];
  const saude         = data.saude          || {};
  const metricas      = data.metricas       || {};
  const timestamp     = data.timestamp      || formatarData(new Date());
  const score         = saude.score         !== undefined ? saude.score : "";
  const label         = saude.label         || "";
  const chi2          = (metricas.chi2      || {}).valor || "";
  const desvioEwma    = (metricas.desvio_padrao || {}).ewma || "";
  const totalSorteios = data.total_analisados || "";

  if (alertas.length === 0) {
    const linhaOK = [
      timestamp, "STATUS_OK", "OK", "Nenhuma anomalia detectada neste ciclo.",
      "", score, label, chi2, desvioEwma, totalSorteios
    ];
    sheet.appendRow(linhaOK);
    const ultima = sheet.getLastRow();
    sheet.getRange(ultima, 1, 1, HEADER_ALERTAS.length).setBackground("#f1f8e9");
    limitarLinhas(sheet, MAX_LINHAS_ALERTAS);  // [4]
    return ContentService
      .createTextOutput(JSON.stringify({ status: "ok", tipo: "monitor_ok", alertas_gravados: 0 }))
      .setMimeType(ContentService.MimeType.JSON);
  }

  let linhasGravadas = 0;
  alertas.forEach(alerta => {
    const linha = [
      alerta.timestamp  || timestamp,
      alerta.tipo       || "DESCONHECIDO",
      alerta.severidade || "INFO",
      alerta.mensagem   || "",
      alerta.valor      !== undefined ? String(alerta.valor) : "",
      score, label, chi2, desvioEwma, totalSorteios
    ];
    sheet.appendRow(linha);
    const ultima = sheet.getLastRow();
    sheet.getRange(ultima, 1, 1, HEADER_ALERTAS.length).setBackground(corSeveridade(alerta.severidade));
    if ((alerta.severidade || "").toUpperCase() === "CRITICO") {
      sheet.getRange(ultima, 2).setFontWeight("bold").setFontColor("#b71c1c");
      sheet.getRange(ultima, 3).setFontWeight("bold").setFontColor("#b71c1c");
    }
    linhasGravadas++;
  });

  // [4] Limita o tamanho da aba de alertas
  limitarLinhas(sheet, MAX_LINHAS_ALERTAS);

  return ContentService
    .createTextOutput(JSON.stringify({ status: "ok", tipo: "monitor_alertas", alertas_gravados: linhasGravadas }))
    .setMimeType(ContentService.MimeType.JSON);
}

// [3] GET /exec — health check aprimorado
function doGet(e) {
  // GET público (não exige token) para health check de monitoramento
  const ss     = SpreadsheetApp.getActiveSpreadsheet();
  const sheets = ss.getSheets().map(s => ({
    nome:     s.getName(),
    linhas:   s.getLastRow(),
    colunas:  s.getLastColumn()
  }));

  return ContentService
    .createTextOutput(JSON.stringify({
      status:    "online",
      planilha:  ss.getName(),
      abas:      sheets,
      timestamp: formatarData(new Date()),
      versao:    "2.0-melhorado"
    }))
    .setMimeType(ContentService.MimeType.JSON);
}
