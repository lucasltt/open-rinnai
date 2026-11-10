/* Rinnai local dashboard: plain JS, no CDN, no control writes on startup. */
(() => {
  'use strict';
  if (window.__rinnaiDashboardBooted) return;
  window.__rinnaiDashboardBooted = true;

  // ESPHome's minimal HTML shell omits this. Set it before rendering so real
  // mobile browsers use the device width instead of a scaled 980px desktop.
  const viewport = document.head.querySelector('meta[name="viewport"]') || document.createElement('meta');
  viewport.name = 'viewport';
  viewport.content = 'width=device-width, initial-scale=1';
  if (!viewport.isConnected) document.head.append(viewport);
  for (const [name, content] of Object.entries({
    'theme-color': '#192a38', 'mobile-web-app-capable': 'yes',
    'apple-mobile-web-app-capable': 'yes', 'apple-mobile-web-app-title': 'Aquecedor',
    'apple-mobile-web-app-status-bar-style': 'default'
  })) {
    const meta = document.head.querySelector(`meta[name="${name}"]`) || document.createElement('meta');
    meta.name = name; meta.content = content;
    if (!meta.isConnected) document.head.append(meta);
  }
  if (!window.__RINNAI_PREVIEW__) {
    for (const [rel, href] of [['manifest', '/manifest.webmanifest'], ['apple-touch-icon', '/apple-touch-icon.png'], ['stylesheet', '/pwa.css']]) {
      if (document.head.querySelector(`link[rel="${rel}"][href="${href}"]`)) continue;
      const link = document.createElement('link'); link.rel = rel; link.href = href;
      if (rel === 'apple-touch-icon') link.sizes = '180x180';
      document.head.append(link);
    }
  }

  const paths = {
    waves: '<path d="M3 7c3-4 6 4 9 0s6 4 9 0M3 12c3-4 6 4 9 0s6 4 9 0M3 17c3-4 6 4 9 0s6 4 9 0"/>',
    settings: '<path d="m9.8 3-.7 2.1-2 .9-2-.5-1.6 2.8 1.4 1.6-.2 2.2L3 13.5l1.6 2.8 2.1-.4 1.8 1.2.5 2.1h3.3l.8-2 2-.9 2 .5 1.6-2.8-1.4-1.6.1-2.2L21 8.8 19.4 6l-2.1.4-1.8-1.2L15 3h-3.2Z"/><circle cx="12" cy="11.2" r="3"/>',
    plus: '<path d="M12 5v14M5 12h14"/>', minus: '<path d="M5 12h14"/>',
    arrow: '<path d="M4 12h16m-6-6 6 6-6 6"/>', check: '<path d="m5 12 4 4L19 6"/>',
    drop: '<path d="M12 3S5.5 10.1 5.5 14.2a6.5 6.5 0 0 0 13 0C18.5 10.1 12 3 12 3Z"/>',
    heat: '<path d="M12 3c1 4-3 5-2 9 2-1 3-3 3-4 4 3 5 5 5 8a6 6 0 0 1-12 0c0-4 3-7 6-13Z"/>',
    bolt: '<path d="m13 2-9 12h7l-1 8 10-12h-7l1-8Z"/>',
    clock: '<circle cx="12" cy="12" r="8"/><path d="M12 7v5l3 2"/>',
    coin: '<circle cx="12" cy="12" r="8"/><path d="M15 8h-4a2 2 0 0 0 0 4h2a2 2 0 0 1 0 4H9m3-10v12"/>',
    chart: '<path d="M4 4v16h16M7 15l4-5 4 3 5-7"/>',
    pulse: '<path d="M3 12h5l3-8 3 16 3-8h4"/>',
    chevron: '<path d="m6 9 6 6 6-6"/>', cross: '<path d="m6 6 12 12M6 18 18 6"/>',
    history: '<path d="M4 5v5h5M4.5 10A8 8 0 1 1 4 14m8-7v5l3 2"/>',
    down: '<path d="M12 4v12m-4-4 4 4 4-4M5 18v2h14v-2"/>',
    thermometer: '<path d="M9 14V5a3 3 0 0 1 6 0v9a5 5 0 1 1-6 0Z"/><path d="M12 8v9"/>'
  };
  const icon = (name, cls = '') => `<svg class="${cls}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.65" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${paths[name] || paths.pulse}</svg>`;
  const n = value => value === null || value === undefined || value === '' ? null : Number.isFinite(Number(value)) ? Number(value) : null;
  const nf = (value, decimals = 1) => n(value) === null ? '—' : Number(value).toLocaleString('pt-BR', { minimumFractionDigits: decimals, maximumFractionDigits: decimals });
  const money = value => n(value) === null ? '—' : Number(value).toLocaleString('pt-BR', { style: 'currency', currency: 'BRL' });
  const duration = value => {
    if (n(value) === null) return '—';
    const t = Math.max(0, Math.floor(value));
    const h = Math.floor(t / 3600), m = Math.floor(t / 60) % 60, s = t % 60;
    return h ? `${h}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}` : `${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
  };
  const uptime = value => {
    if (n(value) === null) return '—';
    const minutes = Math.floor(value / 60), hours = Math.floor(minutes / 60), days = Math.floor(hours / 24);
    return days ? `${days} d ${hours % 24} h` : hours ? `${hours} h ${minutes % 60} min` : `${minutes} min`;
  };
  const ago = seconds => {
    if (n(seconds) === null) return 'Sem horário';
    if (seconds < 60) return 'Agora há pouco';
    if (seconds < 3600) return `Há ${Math.floor(seconds / 60)} min`;
    if (seconds < 86400) return `Há ${Math.floor(seconds / 3600)} h`;
    return `Há ${Math.floor(seconds / 86400)} d`;
  };
  const sessionTime = item => {
    const epoch = n(item.ended_at_epoch_s);
    if (epoch === null) return n(item.age_s) === null ? 'Horário indisponível' : ago(item.age_s);
    const date = new Date(epoch * 1000);
    if (!Number.isFinite(date.getTime())) return 'Horário indisponível';
    const clock = date.toLocaleTimeString('pt-BR', { hour: '2-digit', minute: '2-digit' });
    return date.toDateString() === new Date().toDateString() ? `Hoje, ${clock}` :
      `${date.toLocaleDateString('pt-BR', { day: '2-digit', month: '2-digit' })}, ${clock}`;
  };

  let state = null, history = { points: [], sessions: [] }, receivedAt = 0, transportOk = false;
  let draft = 38, draftDirty = false, sending = false, sentTarget = null, sentAt = 0;
  let scope = 'totals', sessionScope = 'today', chartMode = 'temperature', chartWindow = 86400, chartPoints = [], chartCursor = null;
  let stateLoading = false, historyLoading = false, periodsLoading = false, diagnosticsLoading = false, toastTimer = null, chartFrame = null;
  let periods = { days: [], months: [] }, diagnostics = null;
  let lastAlertSignature = '', historyReceivedAt = 0;
  const openedAt = Date.now();
  const comparedSessions = new Set();
  const homeZone = 'America/Sao_Paulo';
  const localDayKey = milliseconds => new Intl.DateTimeFormat('sv-SE', { timeZone: homeZone, year: 'numeric', month: '2-digit', day: '2-digit' }).format(new Date(milliseconds));
  let root, el = {};

  const html = `
    <header class="topbar">
      <a class="brand" href="/" aria-label="Painel do aquecedor Rinnai">
        <span class="brand-mark">${icon('waves')}</span>
        <span><span class="brand-name">Rinnai</span><span class="brand-note" style="display:block">CONTROLE LOCAL · REU-E211</span></span>
      </a>
      <div class="top-actions"><span class="connection-pill waiting" id="connection-status">Conectando</span><button class="icon-button" id="open-settings" type="button" aria-label="Abrir tarifas e configurações">${icon('settings')}</button></div>
    </header>
    <div class="preview-banner" id="preview-banner" hidden>Prévia de interface com dados demonstrativos. Nenhum aquecedor é controlado nesta página.</div>
    <section class="page-intro" aria-labelledby="page-title"><div><div class="eyebrow">Seu aquecedor, por perto</div><h1 id="page-title">Água no seu ponto.</h1><p>Temperatura, uso e consumo em um só lugar.</p></div><span class="updated" id="updated-at">Aguardando a primeira leitura</span></section>
    <section class="hero-grid" aria-label="Controle e leituras do aquecedor">
      <article class="card control-card">
        <div class="card-heading"><h2 class="card-title">Temperatura desejada</h2><span class="state-pill" id="heater-status">Conectando</span></div>
        <div class="temperature-stepper"><button type="button" class="step-button" id="temperature-down" aria-label="Diminuir temperatura desejada em um grau" disabled>${icon('minus')}</button><div class="target-reading" aria-live="polite" aria-atomic="true"><span id="draft-temperature">—</span><span class="target-unit">°C</span></div><button type="button" class="step-button" id="temperature-up" aria-label="Aumentar temperatura desejada em um grau" disabled>${icon('plus')}</button></div>
        <p class="verified-setpoint" id="verified-setpoint">Aguardando a temperatura do aquecedor</p>
        <div class="presets" aria-label="Temperaturas favoritas"><button class="preset" type="button" data-preset="38" aria-pressed="false" disabled>38°</button><button class="preset" type="button" data-preset="39" aria-pressed="false" disabled>39°</button><button class="preset" type="button" data-preset="40" aria-pressed="false" disabled>40°</button></div>
        <button type="button" class="apply-button" id="apply-temperature" disabled><span id="apply-label">Aplicar temperatura</span>${icon('arrow')}</button>
        <p class="control-help" id="control-help">O ajuste só muda ao tocar em Aplicar.</p><p class="status-message" id="control-message" role="status" aria-live="polite"></p>
      </article>
      <article class="card live-card">
        <div class="card-heading"><h2 class="card-title">Água em tempo real</h2><span class="eyebrow" style="font-size:8px;letter-spacing:1.2px">AGORA</span></div>
        <div class="temperature-pair"><div class="water-temperature"><div class="metric-label">${icon('drop')}Entrada</div><div class="water-value"><span id="inlet-temperature">—</span><small>°C</small></div></div><div class="water-path">${icon('arrow')}</div><div class="water-temperature outlet"><div class="metric-label">Saída${icon('heat')}</div><div class="water-value"><span id="outlet-temperature">—</span><small>°C</small></div></div></div>
        <div class="live-divider"></div>
        <div class="live-grid"><div class="live-stat"><div class="metric-label">${icon('waves')}Vazão</div><div class="metric-number"><span id="live-flow">—</span><small>L/min</small></div><div class="metric-footnote">Água circulando</div></div><div class="live-stat"><div class="metric-label">${icon('bolt')}Potência</div><div class="metric-number"><span id="live-power">—</span><small>kW</small></div><div class="metric-footnote" id="power-original">— kcal/min</div></div><div class="live-stat"><div class="metric-label">${icon('clock')}Uso atual</div><div class="metric-number" id="live-duration">—</div><div class="metric-footnote" id="session-state">Aguardando água</div></div></div>
      </article>
    </section>
    <section aria-labelledby="consumption-title"><div class="section-heading"><div><h2 id="consumption-title">Cada uso conta.</h2><p>Estimativas a partir das leituras.</p></div><div class="segmented" id="scope-tabs" role="tablist" aria-label="Período do consumo"><button type="button" role="tab" aria-selected="true" data-scope="totals">Acumulado</button><button type="button" role="tab" aria-selected="false" tabindex="-1" data-scope="session">Uso atual</button></div></div>
      <div class="consumption-grid">
        <article class="card consumption-card"><div class="consumption-top"><span>Água estimada</span><span class="metric-icon">${icon('drop')}</span></div><div class="consumption-value"><span id="water-consumption">—</span><small>L</small></div><div class="consumption-note" id="water-note">Somatório das leituras de vazão</div></article>
        <article class="card consumption-card"><div class="consumption-top"><span>Energia estimada</span><span class="metric-icon energy">${icon('bolt')}</span></div><div class="consumption-value"><span id="energy-consumption">—</span><small>kWh</small></div><div class="consumption-note">Pela potência informada</div></article>
        <article class="card consumption-card"><div class="consumption-top"><span>Estimativa de gás</span><span class="metric-icon gas">${icon('heat')}</span></div><div class="consumption-value"><span id="gas-consumption">—</span><small id="gas-unit">m³</small></div><div class="consumption-note" id="gas-note">Gás natural · estimado</div></article>
        <article class="card consumption-card"><div class="consumption-top"><span>Estimativa de custo</span><span class="metric-icon cost">${icon('coin')}</span></div><div class="consumption-value" id="cost-consumption">—</div><div class="consumption-note" id="cost-note">Defina as tarifas nas configurações</div><dl class="cost-breakdown" aria-label="Custos de água e gás"><div><dt>Água</dt><dd id="water-cost">Tarifa não informada</dd></div><div><dt>Gás</dt><dd id="gas-cost">Tarifa não informada</dd></div></dl></article>
      </div>
    </section>
    <section class="wide-grid" aria-label="Histórico e usos recentes">
      <article class="card chart-card"><div class="panel-header"><div><h2>O ritmo da sua água</h2><p>Últimas 24 horas acompanhadas</p></div><select class="subtle-select" id="chart-window" aria-label="Período do gráfico"><option value="900">Últimos 15 min</option><option value="1800">Últimos 30 min</option><option value="7200">Últimas 2 horas</option><option value="21600">Últimas 6 horas</option><option value="43200">Últimas 12 horas</option><option value="86400" selected>Últimas 24 horas</option></select></div><div class="chart-tabs" id="chart-tabs" role="tablist" aria-label="Dados do gráfico"><button type="button" role="tab" aria-selected="true" data-chart="temperature">Temperatura</button><button type="button" role="tab" aria-selected="false" tabindex="-1" data-chart="flow">Vazão</button><button type="button" role="tab" aria-selected="false" tabindex="-1" data-chart="power">Potência</button></div><div class="chart-wrap"><canvas id="history-chart" tabindex="0" role="img" aria-label="Gráfico do histórico de temperatura. Use as setas para percorrer os pontos." aria-describedby="chart-description"></canvas><div class="chart-empty" id="chart-empty">${icon('chart')}<span>Os próximos pontos aparecem automaticamente.</span></div><div class="chart-tooltip" id="chart-tooltip" hidden></div></div><div class="chart-footer"><div class="chart-legend" id="chart-legend"><span class="legend-item" style="--legend-color:#ed753f">Saída</span><span class="legend-item" style="--legend-color:#537baf">Entrada</span></div><button type="button" class="text-button" id="export-history">Exportar CSV</button></div><p class="sr-only" id="chart-description">Sem dados de histórico disponíveis.</p></article>
      <article class="card activity-card"><div class="panel-header"><div><h2>Seu dia, uso por uso</h2><p>Compare os horários e a duração de cada acionamento.</p></div><span id="session-count" class="eyebrow" style="font-size:8px;letter-spacing:1px">—</span></div><div class="activity-list" id="activity-list"></div><div class="empty-sessions" id="empty-sessions">${icon('history')}<p>Os usos concluídos aparecerão aqui.</p></div><div class="activity-bottom"><span id="activity-footnote">Histórico disponível no dispositivo</span><button type="button" class="text-button" id="export-sessions">Exportar usos</button></div></article>
    </section>
    <details class="card diagnostics" id="diagnostics"><summary><span class="diagnostics-summary">${icon('pulse')}<span><strong>Diagnóstico do sistema</strong><small>Comunicação, Wi-Fi e memória</small></span></span><span class="summary-tail"><span class="health-tag" id="health-tag">Aguardando leitura</span>${icon('chevron','chevron')}</span></summary><div class="diagnostics-content"><dl class="diagnostic-grid"><div class="diagnostic-item"><dt>Estado do aquecedor</dt><dd id="diag-error">—</dd></div><div class="diagnostic-item"><dt>Sinal Wi-Fi</dt><dd id="diag-rssi">—</dd></div><div class="diagnostic-item"><dt>Tempo ligado</dt><dd id="diag-uptime">—</dd></div><div class="diagnostic-item"><dt>Memória livre</dt><dd id="diag-memory">—</dd></div><div class="diagnostic-item"><dt>Tempo de resposta</dt><dd id="diag-latency">—</dd></div><div class="diagnostic-item"><dt>Respostas válidas</dt><dd id="diag-valid">—</dd></div><div class="diagnostic-item"><dt>Consultas sem resposta</dt><dd id="diag-timeouts">—</dd></div><div class="diagnostic-item"><dt>Quadros rejeitados</dt><dd id="diag-bad">—</dd></div><div class="diagnostic-item"><dt>Comunicação com o aquecedor</dt><dd id="diag-link">—</dd></div><div class="diagnostic-item"><dt>Firmware</dt><dd id="diag-version">—</dd></div><div class="diagnostic-item"><dt>Interface serial validada</dt><dd>GPIO25 TX · GPIO26 RX</dd></div><div class="diagnostic-item"><dt>Histórico de usos</dt><dd id="diag-history">—</dd></div><div class="diagnostic-item"><dt>Tempo total aquecendo</dt><dd id="diag-heating-time">—</dd></div></dl><p class="diagnostic-note">Os dados chegam diretamente do aquecedor pela rede local. Gás e custos são estimativas; não substituem a medição da concessionária. Os usos concluídos são salvos no módulo. Os gráficos reiniciam após desligá-lo.</p></div></details>
    <footer class="dashboard-footer"><span>Controle local · sem depender do aplicativo Rinnai · <a href="/api">API e integrações</a></span><span id="footer-version">ESPHome · Rinnai REU-E211</span></footer>
    <div class="toast" id="toast" role="status" aria-live="polite" hidden></div>
    <dialog class="settings-dialog" id="settings-dialog" aria-labelledby="settings-title"><div class="dialog-heading"><div><h2 id="settings-title">Tarifas e estimativas</h2><p>Seus valores deixam a estimativa de custo mais útil.</p></div><button type="button" class="icon-button dialog-close" id="close-settings" aria-label="Fechar configurações">${icon('cross')}</button></div><form class="settings-form" id="settings-form"><div class="form-field"><label for="gas-type">Tipo de gás</label><select id="gas-type" name="gas_type"><option value="GN">GN · gás natural</option><option value="GLP">GLP · gás de botijão</option></select></div><div class="form-two"><div class="form-field"><label for="gas-tariff" id="gas-tariff-label">Gás · R$ por m³</label><input id="gas-tariff" name="gas_tariff" inputmode="decimal" type="number" min="0" max="100000" step="0.01" placeholder="Não informada"><small>Use a tarifa da sua conta.</small></div><div class="form-field"><label for="water-tariff">Água · R$ por m³</label><input id="water-tariff" name="water_tariff" inputmode="decimal" type="number" min="0" max="100000" step="0.01" placeholder="Não informada"><small>Inclua esgoto, se desejar.</small></div></div><p class="settings-explanation">O gás é estimado pela potência informada e pelo tipo de gás. As tarifas são opcionais: sem elas, mostramos o consumo e deixamos o custo em aberto. O acumulado soma o monitoramento disponível no módulo.</p><p class="form-error" id="settings-error" role="alert"></p><div class="dialog-footer"><button class="secondary-button" type="button" id="cancel-settings">Cancelar</button><button class="primary-button" type="submit" id="save-settings">Salvar tarifas</button></div></form></dialog>`;

  function text(key, value) {
    const node = el[key];
    if (node && node.textContent !== String(value)) node.textContent = String(value);
  }
  function showToast(message, error = false) {
    text('toast', message); el.toast.classList.toggle('error', error); el.toast.hidden = false;
    clearTimeout(toastTimer); toastTimer = setTimeout(() => { el.toast.hidden = true; }, error ? 6500 : 4200);
  }
  function live() { return navigator.onLine !== false && transportOk && !!state?.online && Date.now() - receivedAt < 10000; }
  function hasTariff() { return Number(state?.settings?.gas_tariff) > 0 || Number(state?.settings?.water_tariff) > 0; }
  function gasUnit() { return state?.settings?.gas_type === 'GLP' ? 'kg' : 'm³'; }

  function enhanceMarkup() {
    const activity = root.querySelector('.activity-card');
    activity.classList.add('daily-sessions');
    root.querySelector('.wide-grid').before(activity);
    root.querySelector('.wide-grid').classList.add('chart-only');
    activity.querySelector('.panel-header').insertAdjacentHTML('afterend', '<div class="session-filters segmented" id="session-filters" role="tablist" aria-label="Período dos acionamentos"><button type="button" role="tab" aria-selected="true" data-session-scope="today">Hoje</button><button type="button" role="tab" aria-selected="false" tabindex="-1" data-session-scope="yesterday">Ontem</button><button type="button" role="tab" aria-selected="false" tabindex="-1" data-session-scope="last24">Últimas 24 h</button><button type="button" role="tab" aria-selected="false" tabindex="-1" data-session-scope="all">Todos os 24</button></div><div class="session-summary" id="session-summary" aria-live="polite"></div><p class="session-context" id="session-context"></p><div class="session-comparison" id="session-comparison" aria-live="polite" hidden></div>');
    activity.querySelector('#activity-list').insertAdjacentHTML('afterend', '<div class="undated-sessions" id="undated-sessions" hidden><h3>Horário desconhecido</h3><p>Estes usos foram preservados sem uma data válida. Eles não entram no resumo do dia.</p><div id="undated-list"></div></div>');
    root.querySelector('.chart-footer').insertAdjacentHTML('afterend', '<p class="chart-coverage" id="chart-coverage">Pontos a cada 2 minutos; o gráfico é reconstruído desde a inicialização do módulo. Os horários dos usos ficam salvos separadamente.</p>');
    root.querySelector('.page-intro').insertAdjacentHTML('afterend', '<section class="alerts-panel" id="alerts-panel" aria-label="Avisos do aquecedor" aria-live="polite" hidden></section>');
    root.querySelector('#scope-tabs button:last-child').insertAdjacentHTML('beforebegin', '<button type="button" role="tab" aria-selected="false" tabindex="-1" data-scope="today">Hoje</button><button type="button" role="tab" aria-selected="false" tabindex="-1" data-scope="month">Este mês</button>');
    root.querySelector('.consumption-grid').insertAdjacentHTML('afterend', '<p class="period-note" id="period-note"></p><div class="period-meta" id="period-meta"></div>');
    root.querySelector('.wide-grid').insertAdjacentHTML('beforebegin', '<section class="card periods-card" aria-labelledby="periods-title"><div class="panel-header"><div><h2 id="periods-title">Seus últimos dias</h2><p>Resumo salvo no módulo · água e energia estimadas</p></div><div class="export-actions"><button class="text-button" type="button" id="export-days">CSV diário</button><button class="text-button" type="button" id="export-months">CSV mensal</button></div></div><div class="period-days" id="period-days"></div><p class="period-caption" id="period-caption">Aguardando os resumos.</p></section>');
    [['.water-temperature:not(.outlet)', 'age-inlet'], ['.water-temperature.outlet', 'age-outlet'], ['.live-stat:nth-child(1)', 'age-flow'], ['.live-stat:nth-child(2)', 'age-power']].forEach(([selector, id]) => root.querySelector(selector).insertAdjacentHTML('beforeend', `<span class="reading-age" id="${id}">Leitura ainda indisponível</span>`));
    root.querySelector('#verified-setpoint').insertAdjacentHTML('afterend', '<p class="reading-age setpoint-age" id="age-setpoint">Leitura ainda indisponível</p>');
    root.querySelector('.diagnostic-grid').insertAdjacentHTML('beforeend', '<div class="diagnostic-item"><dt>Inicializações registradas</dt><dd id="diag-boots">—</dd></div><div class="diagnostic-item"><dt>Reinícios por falha</dt><dd id="diag-crashes">—</dd></div><div class="diagnostic-item"><dt>Motivo do último reinício</dt><dd id="diag-reset-reason">—</dd></div><div class="diagnostic-item"><dt>Última inicialização</dt><dd id="diag-last-boot">—</dd></div><div class="diagnostic-item"><dt>Última leitura de chama</dt><dd id="age-flame">—</dd></div><div class="diagnostic-item"><dt>Última leitura de erro</dt><dd id="age-error">—</dd></div>');
    root.querySelector('.diagnostic-note').insertAdjacentHTML('beforebegin', '<div class="boot-history"><div class="panel-header"><h3>Reinícios recentes</h3><button class="text-button" id="export-diagnostics" type="button">Baixar diagnóstico</button></div><ol id="boot-list"></ol><details class="crash-details"><summary>Detalhes da última falha</summary><pre id="crash-log">Nenhum registro de falha disponível.</pre><p id="crash-firmware" class="period-caption"></p></details></div>');
    const favorites = [1, 2, 3].map(i => `<div class="favorite-field"><div class="form-field"><label for="favorite${i}-name">Favorito ${i}</label><input id="favorite${i}-name" type="text" maxlength="31" autocomplete="off" required></div><div class="form-field"><label for="favorite${i}-temp">Temperatura</label><div class="input-unit"><input id="favorite${i}-temp" type="number" min="35" max="50" step="1" required><span>°C</span></div></div></div>`).join('');
    const alerts = [['long-use', 'Banho longo', 'Avisa após o tempo escolhido, quando houve aquecimento.'], ['flow', 'Água circulando por muito tempo', 'Acompanha o uso contínuo, mesmo sem chama.'], ['error', 'Erro informado pelo aquecedor', 'Mostra o código recebido do aparelho.'], ['offline', 'Perda de comunicação', 'Avisa quando as leituras deixam de chegar.']].map(([id, title, note]) => `<label class="check-field"><input id="alert-${id}" type="checkbox"><span><strong>${title}</strong><small>${note}</small></span></label>`).join('');
    root.querySelector('#settings-form').insertAdjacentHTML('afterend', `<form class="settings-form preferences-form" id="preferences-form"><h3>Suas temperaturas favoritas</h3><p class="form-intro">O atalho seleciona a temperatura. O ajuste continua dependendo de Aplicar.</p>${favorites}<h3>Avisos no painel</h3><p class="form-intro">Os avisos aparecem com esta página aberta. Não enviam notificações quando o navegador está fechado.</p>${alerts}<div class="threshold-grid"><div class="form-field"><label for="long-use-min">Banho longo · minutos</label><input id="long-use-min" type="number" min="1" max="180" step="1" required></div><div class="form-field"><label for="continuous-flow-min">Água contínua · minutos</label><input id="continuous-flow-min" type="number" min="1" max="180" step="1" required></div><div class="form-field"><label for="offline-seconds">Sem resposta · segundos</label><input id="offline-seconds" type="number" min="10" max="600" step="1" required></div></div><p class="form-error" id="preferences-error" role="alert"></p><div class="dialog-footer"><button class="primary-button" type="submit" id="save-preferences">Salvar favoritos e avisos</button></div></form>`);
  }

  function favoriteSettings() {
    const supplied = state?.settings?.favorites;
    return [0, 1, 2].map(i => {
      const value = supplied?.[i], temperature = n(value?.temp_c);
      return { name: typeof value?.name === 'string' && value.name.trim() ? value.name : ['Banho', 'Conforto', 'Cozinha'][i], temp_c: temperature !== null && Number.isInteger(temperature) && temperature >= 35 && temperature <= 50 ? temperature : 38 + i };
    });
  }

  function readingAge(key, id) {
    const reading = state?.readings?.[key], base = n(reading?.age_s);
    const age = base === null ? null : Math.max(0, base + Math.floor((Date.now() - receivedAt) / 1000));
    const stale = !!reading?.stale || age !== null && age >= 30 || !live();
    text(id, age === null ? 'Leitura ainda indisponível' : `${stale ? 'Leitura antiga' : 'Lida'} há ${age < 60 ? `${Math.floor(age)} s` : `${Math.floor(age / 60)} min`}`);
    el[id]?.classList.toggle('stale', age !== null && stale);
  }

  function renderAges() {
    [['inlet', 'age-inlet'], ['outlet', 'age-outlet'], ['flow', 'age-flow'], ['power', 'age-power'], ['setpoint', 'age-setpoint'], ['flame', 'age-flame'], ['error', 'age-error']].forEach(([key, id]) => readingAge(key, id));
  }

  function renderAlerts() {
    const items = transportOk && Array.isArray(state?.alerts) ? state.alerts.slice(0, 8) : [];
    const settings = state?.settings?.alerts || {};
    const silentSeconds = (Date.now() - (receivedAt || openedAt)) / 1000;
    if (!transportOk && settings.offline_enabled !== false && silentSeconds >= (n(settings.offline_seconds) ?? 30)) items.push({ id: 'browser-offline', title: 'Sem conexão com o módulo', message: 'Não foi possível atualizar as leituras. Confira a rede e a alimentação do módulo.', severity: 'warning' });
    const signature = JSON.stringify(items);
    if (signature === lastAlertSignature) return;
    lastAlertSignature = signature;
    el['alerts-panel'].replaceChildren(); el['alerts-panel'].hidden = !items.length;
    items.forEach(item => {
      const article = document.createElement('article'); article.className = 'system-alert' + (item.severity === 'error' ? ' critical' : '');
      const glyph = document.createElement('span'); glyph.className = 'alert-glyph'; glyph.innerHTML = icon('pulse');
      const body = document.createElement('div'), title = document.createElement('strong'), message = document.createElement('p');
      title.textContent = String(item.title || 'Aviso do aquecedor'); message.textContent = String(item.message || ''); body.append(title, message); article.append(glyph, body); el['alerts-panel'].append(article);
    });
  }

  async function loadPeriods() {
    if (periodsLoading) return;
    periodsLoading = true;
    try { const data = await api('periods'); if (data && typeof data === 'object') { periods = { ...data, days: Array.isArray(data.days) ? data.days : [], months: Array.isArray(data.months) ? data.months : [] }; renderPeriods(); } }
    catch (_) { /* Existing period summaries remain available. */ }
    finally { periodsLoading = false; }
  }

  async function loadDiagnostics() {
    if (diagnosticsLoading) return;
    diagnosticsLoading = true;
    try { const data = await api('diagnostics'); if (data && typeof data === 'object') { diagnostics = data; renderDiagnostics(); } }
    catch (_) { /* Keep the last diagnostic rather than replacing it with zeros. */ }
    finally { diagnosticsLoading = false; }
  }

  const periodDate = (key, month = false) => {
    const value = String(key || '');
    if (!/^\d{6}(\d{2})?$/.test(value)) return 'Sem data';
    return month ? `${value.slice(4, 6)}/${value.slice(0, 4)}` : `${value.slice(6, 8)}/${value.slice(4, 6)}`;
  };

  function renderPeriods() {
    if (!el['period-days']) return;
    const days = periods.days.slice(-7);
    el['period-days'].replaceChildren();
    const max = Math.max(1, ...days.map(day => n(day.water_l) || 0));
    days.forEach(day => {
      const card = document.createElement('div'); card.className = 'period-day';
      const label = document.createElement('strong'); label.textContent = periodDate(day.key);
      const bar = document.createElement('div'); bar.className = 'period-bar'; const fill = document.createElement('span'); fill.style.height = `${Math.max(3, Math.min(100, (n(day.water_l) || 0) / max * 100))}%`; bar.append(fill);
      const amount = document.createElement('span'); amount.textContent = `${nf(day.water_l, 1)} L`;
      const detail = document.createElement('small'); detail.textContent = `${nf(day.energy_kwh, 3)} kWh${day.partial ? ' · parcial' : ''}`;
      card.append(label, bar, amount, detail); el['period-days'].append(card);
    });
    const unassigned = periods.unassigned || {}, undated = (n(unassigned.water_l) || 0) > 0 || (n(unassigned.energy_kwh) || 0) > 0;
    const caption = !periods.clock_valid ? 'Aguardando sincronização do relógio para organizar os dias e meses.' : days.length ? 'Até 31 dias e 12 meses ficam salvos no módulo. Um período parcial contém apenas o monitoramento disponível; consumos antigos sem data não são distribuídos retroativamente.' : 'Os novos consumos formarão os resumos a partir de agora.';
    text('period-caption', caption + (undated ? ` Sem data definida: ${nf(unassigned.water_l, 1)} L e ${nf(unassigned.energy_kwh, 3)} kWh.` : ''));
    el['export-days'].disabled = !periods.days.length; el['export-months'].disabled = !periods.months.length;
  }

  function epochLabel(value) {
    const epoch = n(value); if (epoch === null || epoch < 1577836800) return 'Horário indisponível';
    return new Date(epoch * 1000).toLocaleString('pt-BR', { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' });
  }

  function renderDiagnostics() {
    if (!diagnostics) return;
    text('diag-boots', nf(diagnostics.boot_count, 0)); text('diag-crashes', nf(diagnostics.crash_count, 0));
    text('diag-reset-reason', diagnostics.last_reset_reason || 'Não informado');
    const boots = Array.isArray(diagnostics.boots) ? diagnostics.boots.slice(0, 12) : [];
    text('diag-last-boot', boots.length ? epochLabel(boots[0].epoch_s) : 'Horário indisponível');
    el['boot-list'].replaceChildren();
    boots.forEach(boot => { const row = document.createElement('li'), date = document.createElement('span'), reason = document.createElement('strong'); date.textContent = epochLabel(boot.epoch_s); reason.textContent = `${boot.reason || 'Motivo não informado'}${boot.crash ? ' · falha' : ''}`; row.append(date, reason); el['boot-list'].append(row); });
    if (!boots.length) { const item = document.createElement('li'); item.textContent = 'O histórico de reinícios começa nesta versão.'; el['boot-list'].append(item); }
    text('crash-log', typeof diagnostics.crash_log === 'string' && diagnostics.crash_log.trim() ? diagnostics.crash_log : 'Nenhum registro detalhado de falha disponível.');
    const hasCrash = typeof diagnostics.crash_log === 'string' && diagnostics.crash_log.trim().length > 0;
    const firmwareHash = hasCrash ? diagnostics.crash_firmware_elf_sha256 : diagnostics.firmware_elf_sha256;
    text('crash-firmware', firmwareHash ? hasCrash ? `Firmware do registro da falha: ${diagnostics.crash_firmware_version ? `${diagnostics.crash_firmware_version} · ` : ''}${firmwareHash}` : `Firmware em execução: ${firmwareHash}` : 'A identificação do firmware da falha pode estar indisponível em registros antigos.');
  }

  async function api(path, form, timeoutMs) {
    if (form && navigator.onLine === false) throw new Error('Reconecte à rede antes de enviar um ajuste.');
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs ?? (form ? 10000 : 6000));
    try {
      const response = await fetch('/rinnai/api/' + path, {
        method: form ? 'POST' : 'GET', credentials: 'same-origin', cache: 'no-store', signal: controller.signal,
        ...(form ? { headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' }, body: new URLSearchParams(form).toString() } : {})
      });
      let data = null;
      const content = await response.text();
      if (content) { try { data = JSON.parse(content); } catch (_) { if (response.ok) throw Object.assign(new Error('O módulo não retornou uma leitura válida.'), { communicationFailure: true }); } }
      if (!response.ok) throw new Error(data?.error || data?.message || (response.status === 401 ? 'Entre novamente para continuar.' : 'Não foi possível concluir o pedido.'));
      return data;
    } catch (error) {
      if (error.name === 'AbortError') throw Object.assign(new Error('O módulo demorou a responder.'), { communicationFailure: true });
      if (error instanceof TypeError) error.communicationFailure = true;
      throw error;
    } finally { clearTimeout(timer); }
  }

  async function loadState() {
    if (stateLoading) return;
    stateLoading = true;
    try {
      const data = await api('state');
      if (!data || typeof data !== 'object') throw new Error('Leitura indisponível.');
      state = data; receivedAt = Date.now(); transportOk = true;
      const actual = n(state.setpoint_c), target = n(state.target_c);
      if (!draftDirty && !sending && sentTarget === null && actual !== null) draft = Math.max(35, Math.min(50, Math.round(state.control?.busy && target !== null ? target : actual)));
      if (sentTarget !== null && state.online && n(state.diag?.setpoint_age_s) !== null && state.diag.setpoint_age_s < 15 && actual === sentTarget && !state.control?.busy) {
        draft = actual; draftDirty = false; sentTarget = null;
        text('control-message', 'Temperatura confirmada pelo aquecedor.'); el['control-message'].classList.remove('error');
        showToast(`Temperatura confirmada em ${nf(actual, 0)} °C.`);
      } else if (sentTarget !== null && !state.control?.busy && Date.now() - sentAt > 8000) {
        const reason = 'A temperatura solicitada ainda não foi confirmada. ' + String(state.control?.status || 'Confira o estado antes de tentar novamente.');
        text('control-message', reason); el['control-message'].classList.add('error'); sentTarget = null;
      }
      render();
    } catch (_) { transportOk = false; render(); }
    finally { stateLoading = false; }
  }
  async function loadHistory() {
    if (historyLoading) return;
    historyLoading = true;
    try {
      const data = await api('history');
      if (data && typeof data === 'object') {
        const points = Array.isArray(data.samples) ? data.samples.filter(row => Array.isArray(row) && row.length >= 6).map(row => ({ age_s: n(row[0]), inlet_c: n(row[1]), outlet_c: n(row[2]), flow_l_min: n(row[3]), power_kw: n(row[4]), heating: row[5] === null ? null : !!row[5] })) : Array.isArray(data.points) ? data.points : [];
        history = { points, sessions: Array.isArray(data.sessions) ? data.sessions : [], sessions_persisted: data.sessions_persisted === true, total_sessions: n(data.total_sessions), session_capacity: n(data.session_capacity) || 24, window_s: n(data.window_s) || 86400, point_interval_s: n(data.point_interval_s) || 120 };
        historyReceivedAt = Date.now();
        chartCursor = null; renderSessions(); scheduleChart();
      }
    } catch (_) { /* Keep the last available historical readings. */ }
    finally { historyLoading = false; }
  }

  function render() {
    const connected = live(), diag = state?.diag || {}, busy = !!state?.control?.busy || sending || sentTarget !== null;
    const actual = n(state?.setpoint_c), heating = connected && !!state?.heating;
    const flow = n(state?.flow_l_min), error = n(diag.error_code);
    const status = !transportOk ? (receivedAt ? 'Sem conexão' : 'Conectando') : !state?.online ? 'Sem comunicação' : error > 0 ? 'Atenção no aquecedor' : heating ? 'Aquecendo' : flow > 0.1 ? 'Água em circulação' : 'Em repouso';
    root.classList.toggle('is-offline', !connected);
    text('connection-status', connected ? 'Conectado' : receivedAt ? 'Reconectando' : 'Conectando');
    el['connection-status'].className = 'connection-pill' + (connected ? '' : receivedAt ? ' offline' : ' waiting');
    text('heater-status', status); el['heater-status'].className = 'state-pill' + (heating ? ' heating' : !connected || error > 0 ? ' offline' : '');
    text('draft-temperature', actual === null && !draftDirty ? '—' : nf(draft, 0));
    text('verified-setpoint', actual === null ? 'Aguardando a temperatura do aquecedor' : `No aquecedor: ${nf(actual, 0)} °C${busy ? ' · ajuste em andamento' : ''}`);
    const controllerReady = state?.control?.ready !== false;
    const controlsEnabled = connected && !busy && actual !== null && controllerReady && error === 0;
    el['temperature-down'].disabled = !controlsEnabled || draft <= 35;
    el['temperature-up'].disabled = !controlsEnabled || draft >= 50;
    const favorites = favoriteSettings();
    root.querySelectorAll('[data-preset]').forEach((button, index) => {
      const favorite = favorites[index];
      button.dataset.preset = String(favorite.temp_c); button.textContent = `${favorite.name} · ${nf(favorite.temp_c, 0)}°`;
      button.title = `${favorite.name}, ${favorite.temp_c} graus`; button.disabled = !controlsEnabled;
      button.setAttribute('aria-pressed', String(favorite.temp_c === draft && actual !== null));
    });
    el['apply-temperature'].disabled = !controlsEnabled || draft === actual || !draftDirty;
    text('apply-label', busy ? 'Aguardando confirmação…' : 'Aplicar temperatura');
    text('control-help', !connected ? transportOk ? 'Aguardando comunicação com o aquecedor.' : 'Os controles voltam com a conexão.' : busy ? state?.control?.status || 'Consultando o aquecedor antes de confirmar.' : !controllerReady ? state?.control?.status || 'Aguardando leituras atuais para habilitar o ajuste.' : error === null ? 'Aguardando o diagnóstico do aquecedor.' : error > 0 ? `Ajuste indisponível enquanto o aquecedor informa o erro ${nf(error, 0)}.` : 'O ajuste só muda ao tocar em Aplicar. · 35–50 °C');
    text('inlet-temperature', nf(state?.inlet_c)); text('outlet-temperature', nf(state?.outlet_c));
    text('live-flow', nf(state?.flow_l_min)); text('live-power', nf(state?.power_kw));
    text('power-original', `${nf(state?.power_kcal_min)} kcal/min`);
    const session = state?.session || {};
    text('live-duration', session.active && connected ? duration(Number(session.duration_s || 0) + Math.floor((Date.now() - receivedAt) / 1000)) : '—');
    text('session-state', session.active ? 'Água em uso' : 'Aguardando água');
    const summary = state?.periods || {};
    const data = scope === 'session' ? session : scope === 'today' ? (summary.clock_valid ? summary.today || {} : {}) : scope === 'month' ? (summary.clock_valid ? summary.month || {} : {}) : state?.totals || {};
    text('water-consumption', nf(data.water_l, 1)); text('energy-consumption', nf(data.energy_kwh, 3));
    text('gas-consumption', nf(data.gas, 3)); text('gas-unit', gasUnit());
    text('gas-note', state?.settings?.gas_type === 'GLP' ? 'GLP · estimado' : 'Gás natural · estimado');
    text('cost-consumption', hasTariff() ? money(data.cost) : '—');
    const gasTariff = Number(state?.settings?.gas_tariff) > 0, waterTariff = Number(state?.settings?.water_tariff) > 0;
    text('water-cost', waterTariff ? money(data.water_cost) : 'Tarifa não informada');
    text('gas-cost', gasTariff ? money(data.gas_cost) : 'Tarifa não informada');
    el['water-cost'].classList.toggle('missing-tariff', !waterTariff);
    el['gas-cost'].classList.toggle('missing-tariff', !gasTariff);
    text('cost-note', gasTariff && waterTariff ? 'Água + gás · estimativa' : gasTariff ? 'Gás · tarifa de água não informada' : waterTariff ? 'Água · tarifa de gás não informada' : 'Defina as tarifas nas configurações');
    text('water-note', scope === 'session' ? session.active ? 'Água deste uso' : 'Sem uso em andamento' : scope === 'today' ? 'Água acompanhada hoje' : scope === 'month' ? 'Água acompanhada neste mês' : 'Acumulado do monitoramento');
    const periodSelected = scope === 'today' || scope === 'month';
    text('period-note', periodSelected ? !summary.clock_valid ? 'Aguardando o relógio do módulo. Os consumos sem data não são atribuídos a um dia ou mês.' : data.partial ? 'Período parcial: inclui somente o intervalo acompanhado pelo módulo.' : 'Inclui o consumo acompanhado pelo módulo neste período.' : '');
    el['period-note'].hidden = !periodSelected;
    text('period-meta', periodSelected && summary.clock_valid ? `${nf(data.sessions, 0)} usos concluídos · ${uptime(data.heating_s)} aquecendo${summary.tracked_since_epoch_s ? ` · monitoramento desde ${epochLabel(summary.tracked_since_epoch_s)}` : ''}` : '');
    el['period-meta'].hidden = !periodSelected;
    const healthy = connected && error === 0;
    text('health-tag', healthy ? 'Comunicação normal' : !connected ? 'Sem comunicação' : error > 0 ? `Erro ${nf(error, 0)}` : 'Aguardando leitura');
    el['health-tag'].classList.toggle('problem', !healthy);
    text('diag-error', error === null ? '—' : error === 0 ? 'Sem erro informado' : `Código ${nf(error, 0)}`);
    text('diag-rssi', n(diag.rssi_dbm) === null ? '—' : `${nf(diag.rssi_dbm, 0)} dBm · ${diag.rssi_dbm >= -60 ? 'ótimo' : diag.rssi_dbm >= -70 ? 'bom' : 'fraco'}`);
    text('diag-uptime', uptime(diag.uptime_s)); text('diag-memory', n(diag.heap_bytes) === null ? '—' : `${nf(diag.heap_bytes / 1024, 0)} KiB`);
    text('diag-latency', n(diag.latency_ms) === null ? '—' : `${nf(diag.latency_ms, 0)} ms`);
    text('diag-valid', nf(diag.valid_frames, 0)); text('diag-timeouts', nf(diag.timeouts, 0)); text('diag-bad', nf(diag.bad_frames, 0));
    text('diag-link', connected ? 'Respostas válidas recebidas' : 'Aguardando resposta válida');
    text('diag-version', state?.version || '—'); text('diag-heating-time', uptime(state?.totals?.heating_s));
    text('diag-history', diag.storage_ok === false ? 'Falha na gravação' : diag.session_history_persists ? 'Usos salvos na memória permanente' : 'Usos desta inicialização');
    text('footer-version', state?.version ? `Rinnai REU-E211 · versão ${state.version}` : 'ESPHome · Rinnai REU-E211');
    renderUpdated(); renderSessions(); renderAges(); renderAlerts(); renderDiagnostics();
  }

  function renderUpdated() {
    if (!receivedAt) { text('updated-at', 'Aguardando a primeira leitura'); return; }
    if (!transportOk) { text('updated-at', 'Reconectando ao módulo…'); return; }
    const diag = state?.diag || {};
    let age = n(diag.last_seen_age_s);
    if (age === null && n(diag.uptime_s) !== null && n(diag.last_seen_ms) !== null) age = Math.max(0, diag.uptime_s - diag.last_seen_ms / 1000);
    if (age !== null) age += Math.floor((Date.now() - receivedAt) / 1000);
    text('updated-at', !state?.online ? 'Aguardando resposta do aquecedor' : age !== null && age >= 2 ? `Última resposta há ${Math.floor(age)} s` : 'Leituras atualizadas pela rede local');
  }

  function changeDraft(value) {
    draft = Math.max(35, Math.min(50, Math.round(value))); draftDirty = draft !== n(state?.setpoint_c);
    text('control-message', ''); el['control-message'].classList.remove('error'); render();
  }
  async function applyTemperature() {
    if (el['apply-temperature'].disabled) return;
    sending = true; text('control-message', 'Enviando pedido ao aquecedor…'); el['control-message'].classList.remove('error'); render();
    const target = draft;
    try {
      await api('temperature', { target: String(target) });
      sentTarget = target; sentAt = Date.now();
      text('control-message', 'Pedido enviado. Aguardando confirmação do aquecedor…');
      await loadState();
    } catch (error) {
      if (error.communicationFailure) {
        // The heater may have acted even if the HTTP response was lost.
        // Observe its setpoint; never resend a relative step automatically.
        sentTarget = target; sentAt = Date.now();
        text('control-message', 'Conexão interrompida. Conferindo a temperatura no aquecedor…');
        await loadState();
      } else {
        text('control-message', error.message); el['control-message'].classList.add('error'); showToast(error.message, true);
      }
    }
    finally { sending = false; render(); }
  }

  const validEpoch = value => n(value) !== null && Number(value) >= 1577836800;
  const clockTime = value => validEpoch(value) ? new Date(Number(value) * 1000).toLocaleTimeString('pt-BR', { timeZone: homeZone, hour: '2-digit', minute: '2-digit' }) : '—';
  const dateName = value => {
    if (!validEpoch(value)) return 'Sem horário';
    const day = localDayKey(Number(value) * 1000), today = localDayKey(Date.now()), yesterday = localDayKey(Date.now() - 86400000);
    return day === today ? 'Hoje' : day === yesterday ? 'Ontem' : new Date(Number(value) * 1000).toLocaleDateString('pt-BR', { timeZone: homeZone, day: '2-digit', month: '2-digit' });
  };

  function sessionDuration(item) {
    const value = n(item.elapsed_s) ?? n(item.duration_s);
    return value === null ? null : Math.max(0, value + (item.running && live() ? Math.floor((Date.now() - receivedAt) / 1000) : 0));
  }
  function sessionBounds(item) {
    const start = validEpoch(item.started_at_epoch_s) ? Number(item.started_at_epoch_s) : null;
    const end = item.running ? Date.now() / 1000 : validEpoch(item.ended_at_epoch_s) ? Number(item.ended_at_epoch_s) : null;
    return { start, end, known: start !== null || end !== null };
  }
  function sessionInScope(item) {
    if (sessionScope === 'all') return true;
    const bounds = sessionBounds(item); if (!bounds.known) return false;
    const now = Date.now() / 1000;
    let lower = new Date(`${localDayKey(Date.now())}T00:00:00-03:00`).getTime() / 1000, upper = lower + 86400;
    if (sessionScope === 'yesterday') { upper = lower; lower -= 86400; }
    if (sessionScope === 'last24') { lower = now - 86400; upper = now + 1; }
    return (bounds.end ?? bounds.start) >= lower && (bounds.start ?? bounds.end) < upper;
  }
  function sessionEntry(item) {
    const row = document.createElement('article'); row.className = 'session-entry' + (item.running ? ' running' : item.complete === false ? ' incomplete' : '');
    if (n(item.sequence) !== null) row.dataset.sessionSequence = String(item.sequence);
    const bounds = sessionBounds(item), timeline = document.createElement('div'); timeline.className = 'session-hours';
    const start = document.createElement('strong'); start.textContent = bounds.start === null ? item.running ? 'Em uso' : '—' : clockTime(bounds.start);
    const end = document.createElement('span'); end.textContent = item.running ? 'em andamento' : bounds.end === null ? 'horário indisponível' : `até ${clockTime(bounds.end)}`;
    const day = document.createElement('small'); day.textContent = dateName(bounds.start ?? bounds.end);
    timeline.append(start, end, day);
    const body = document.createElement('div'); body.className = 'session-body';
    const heading = document.createElement('div'); heading.className = 'session-heading';
    const label = document.createElement('span'); label.textContent = item.running ? 'Uso em andamento' : n(item.sequence) === null ? 'Uso registrado' : `Uso #${item.sequence}`;
    const badge = document.createElement('small'); badge.className = 'session-badge'; badge.textContent = item.running ? 'AGORA' : item.complete === false ? 'Registro incompleto' : 'Concluído';
    heading.append(label, badge);
    const length = document.createElement('div'); length.className = 'session-length';
    const seconds = sessionDuration(item), strong = document.createElement('strong'); strong.textContent = duration(seconds);
    const lengthLabel = document.createElement('span'); lengthLabel.textContent = seconds === null ? 'duração indisponível' : seconds >= 3600 ? 'h:min:seg' : 'min:seg'; length.append(strong, lengthLabel);
    const usage = document.createElement('div'); usage.className = 'session-usage';
    [`${nf(item.water_l, 1)} L de água`, `${nf(item.energy_kwh, 3)} kWh`, `${nf(item.gas, 3)} ${gasUnit()} de gás estimado`, hasTariff() ? `${money(item.cost)} estimados` : 'Custo sem tarifa'].forEach(value => { const metric = document.createElement('span'); metric.textContent = value; usage.append(metric); });
    body.append(heading, length, usage);
    if (item.complete === false && !item.running) {
      const note = document.createElement('p'); note.className = 'session-warning'; note.textContent = 'A observação foi interrompida. A duração e o consumo incluem somente o trecho acompanhado.'; body.append(note);
    } else if (bounds.start === null && bounds.end !== null && !item.running) {
      const note = document.createElement('p'); note.className = 'session-warning'; note.textContent = 'O término foi salvo; o horário de início deste registro antigo não está disponível.'; body.append(note);
    }
    const pick = document.createElement('label'); pick.className = 'session-pick';
    const checkbox = document.createElement('input'); checkbox.type = 'checkbox'; checkbox.dataset.compareKey = sessionKey(item);
    checkbox.checked = comparedSessions.has(sessionKey(item)); checkbox.disabled = comparedSessions.size >= 2 && !checkbox.checked;
    checkbox.setAttribute('aria-label', `Comparar ${item.running ? 'uso em andamento' : `uso ${item.sequence || ''}`}`);
    checkbox.addEventListener('change', () => {
      if (checkbox.checked && comparedSessions.size < 2) comparedSessions.add(sessionKey(item)); else comparedSessions.delete(sessionKey(item));
      root.querySelectorAll('[data-compare-key]').forEach(input => { input.disabled = comparedSessions.size >= 2 && !comparedSessions.has(input.dataset.compareKey); });
      renderComparison();
    });
    const pickLabel = document.createElement('span'); pickLabel.textContent = 'Comparar'; pick.append(checkbox, pickLabel); body.append(pick);
    row.append(timeline, body); return row;
  }
  const sessionKey = item => item.running ? `active-${item.sequence || 'now'}` : `saved-${item.sequence || item.ended_at_epoch_s || 'unknown'}`;
  function renderComparison() {
    const all = state?.session?.active ? [{ ...state.session, running: true }, ...history.sessions] : history.sessions;
    const picked = all.filter(item => comparedSessions.has(sessionKey(item))).slice(0, 2);
    el['session-comparison'].replaceChildren(); el['session-comparison'].hidden = picked.length === 0;
    if (!picked.length) return;
    const heading = document.createElement('div'); heading.className = 'comparison-heading';
    const title = document.createElement('strong'); title.textContent = picked.length === 2 ? 'Dois usos, lado a lado' : 'Marque mais um uso para comparar';
    const clear = document.createElement('button'); clear.type = 'button'; clear.className = 'text-button'; clear.textContent = 'Limpar comparação';
    clear.addEventListener('click', () => { comparedSessions.clear(); renderSessions(); }); heading.append(title, clear); el['session-comparison'].append(heading);
    const grid = document.createElement('div'); grid.className = 'comparison-grid';
    picked.forEach(item => {
      const bounds = sessionBounds(item), card = document.createElement('div'), time = document.createElement('span'), elapsed = document.createElement('strong'), water = document.createElement('small');
      time.textContent = `${dateName(bounds.start ?? bounds.end)} · ${clockTime(bounds.start)}–${item.running ? 'agora' : clockTime(bounds.end)}`;
      elapsed.textContent = duration(sessionDuration(item)); water.textContent = `${nf(item.water_l, 1)} L de água${item.complete === false ? ' · registro incompleto' : ''}`;
      card.append(time, elapsed, water); grid.append(card);
    }); el['session-comparison'].append(grid);
    if (picked.length === 2) {
      const secondsA = sessionDuration(picked[0]), secondsB = sessionDuration(picked[1]), waterA = n(picked[0].water_l), waterB = n(picked[1].water_l);
      const difference = document.createElement('p'); difference.className = 'comparison-difference';
      difference.textContent = `Diferença: ${secondsA !== null && secondsB !== null ? duration(Math.abs(secondsA - secondsB)) + ' de duração' : 'duração indisponível'}${waterA !== null && waterB !== null ? ` · ${nf(Math.abs(waterA - waterB), 1)} L de água` : ''}.`;
      el['session-comparison'].append(difference);
    }
  }
  function renderSessions() {
    if (!el['activity-list']) return;
    const focusKey = document.activeElement?.dataset?.compareKey;
    const records = Array.isArray(history.sessions) ? history.sessions.slice(0, 24) : [];
    const current = state?.session;
    const all = current?.active ? [{ ...current, running: true }, ...records] : records;
    for (const key of comparedSessions) if (!all.some(item => sessionKey(item) === key)) comparedSessions.delete(key);
    const selected = all.filter(item => sessionBounds(item).known && sessionInScope(item));
    selected.sort((a, b) => (sessionBounds(b).start ?? sessionBounds(b).end ?? 0) - (sessionBounds(a).start ?? sessionBounds(a).end ?? 0));
    const undated = all.filter(item => !sessionBounds(item).known);
    el['activity-list'].replaceChildren(...selected.map(sessionEntry));
    el['undated-list'].replaceChildren(...undated.map(sessionEntry));
    el['undated-sessions'].hidden = undated.length === 0;
    el['empty-sessions'].hidden = selected.length > 0;
    text('empty-sessions', !all.length && Number(state?.totals?.sessions) > 0 ? 'Os usos anteriores estão no acumulado, mas seus detalhes não foram preservados. Os próximos acionamentos aparecerão aqui.' : 'Nenhum acionamento com horário disponível neste período.');
    text('session-count', `${records.length} / ${Math.max(24, n(history.session_capacity) || 24)} salvos${current?.active ? ' + em uso' : ''}`);
    text('activity-footnote', 'Até 24 usos concluídos ficam salvos, mesmo após reiniciar o módulo.');
    const summaryItems = sessionScope === 'all' ? [...selected, ...undated] : selected;
    const totalSeconds = summaryItems.reduce((sum, item) => sum + (sessionDuration(item) || 0), 0);
    const totalWater = summaryItems.reduce((sum, item) => sum + (n(item.water_l) || 0), 0);
    el['session-summary'].replaceChildren();
    [[`${summaryItems.length}`, 'acionamentos'], [duration(totalSeconds), 'tempo dos usos'], [`${nf(totalWater, 1)} L`, 'água estimada']].forEach(([value, label]) => { const card = document.createElement('div'), amount = document.createElement('strong'), caption = document.createElement('span'); amount.textContent = value; caption.textContent = label; card.append(amount, caption); el['session-summary'].append(card); });
    const scopeLabel = { today: 'Hoje', yesterday: 'Ontem', last24: 'Últimas 24 horas', all: 'Todos os registros salvos' }[sessionScope];
    text('session-context', `${scopeLabel} · horário de Brasília. Marque dois usos para comparar duração e água. A duração destaca cada uso completo para facilitar a comparação. Um uso que atravessa a meia-noite pode aparecer nos dois dias.${undated.length ? ` ${undated.length} registro(s) sem horário estão separados abaixo${sessionScope === 'all' ? ' e entram no resumo de todos os registros' : ' e não entram no resumo deste período'}.` : ''}`);
    renderComparison();
    if (focusKey) root.querySelectorAll('[data-compare-key]').forEach(input => { if (input.dataset.compareKey === focusKey) input.focus({ preventScroll: true }); });
  }

  function chartSpec() {
    if (chartMode === 'flow') return { keys: ['flow_l_min'], names: ['Vazão'], colors: ['#257e7b'], unit: 'L/min', fallback: 5 };
    if (chartMode === 'power') return { keys: ['power_kw'], names: ['Potência'], colors: ['#8274ad'], unit: 'kW', fallback: 10 };
    return { keys: ['outlet_c', 'inlet_c'], names: ['Saída', 'Entrada'], colors: ['#ed753f', '#537baf'], unit: '°C', fallback: 50 };
  }
  function scheduleChart() {
    if (chartFrame) cancelAnimationFrame(chartFrame);
    chartFrame = requestAnimationFrame(drawChart);
  }
  function drawChart() {
    chartFrame = null;
    const canvas = el['history-chart'], rect = canvas.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    const dpr = Math.min(2, window.devicePixelRatio || 1);
    canvas.width = Math.round(rect.width * dpr); canvas.height = Math.round(rect.height * dpr);
    const ctx = canvas.getContext('2d'); ctx.scale(dpr, dpr);
    const w = rect.width, h = rect.height, margin = { left: 36, right: 10, top: 15, bottom: 28 };
    const pw = w - margin.left - margin.right, ph = h - margin.top - margin.bottom;
    const spec = chartSpec();
    const historyAge = historyReceivedAt ? Math.max(0, (Date.now() - historyReceivedAt) / 1000) : 0;
    chartPoints = history.points.filter(point => n(point.age_s) !== null).map(point => ({ ...point, age_s: Number(point.age_s) + historyAge })).filter(point => point.age_s >= 0 && point.age_s <= chartWindow).sort((a, b) => b.age_s - a.age_s);
    const values = chartPoints.flatMap(point => spec.keys.map(key => n(point[key]))).filter(value => value !== null);
    el['chart-empty'].hidden = values.length > 0;
    let lo = chartMode === 'temperature' && values.length ? Math.floor((Math.min(...values) - 2) / 5) * 5 : 0;
    let hi = values.length ? Math.max(...values) : spec.fallback;
    hi = chartMode === 'temperature' ? Math.ceil((hi + 2) / 5) * 5 : Math.max(spec.fallback / 5, Math.ceil(hi * 1.2));
    if (hi <= lo) hi = lo + 5;
    const maxAge = chartWindow;
    const x = point => margin.left + pw * (1 - point.age_s / maxAge);
    const y = value => margin.top + ph * (1 - (value - lo) / (hi - lo));
    ctx.font = '9px -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif'; ctx.lineWidth = 1;
    for (let i = 0; i < 5; i++) {
      const value = lo + (hi - lo) * i / 4, yy = y(value);
      ctx.strokeStyle = '#e9ece8'; ctx.beginPath(); ctx.moveTo(margin.left, yy); ctx.lineTo(w - margin.right, yy); ctx.stroke();
      ctx.fillStyle = '#87938f'; ctx.textAlign = 'right'; ctx.fillText(nf(value, hi - lo < 5 ? 1 : 0), margin.left - 8, yy + 3);
    }
    ctx.fillStyle = '#87938f'; ctx.textAlign = 'left'; ctx.fillText(spec.unit, 0, 9);
    for (let i = 0; i < 4; i++) {
      const age = maxAge * (1 - i / 3), xx = margin.left + pw * i / 3;
      ctx.textAlign = i === 0 ? 'left' : i === 3 ? 'right' : 'center';
      const stamp = new Date(Date.now() - age * 1000);
      const label = age < 30 ? 'agora' : stamp.toLocaleTimeString('pt-BR', { timeZone: homeZone, hour: '2-digit', minute: '2-digit' });
      ctx.fillText(label, xx, h - 5);
    }
    spec.keys.forEach((key, series) => {
      const runs = []; let run = [];
      chartPoints.forEach(point => {
        const previous = run[run.length - 1];
        if (n(point[key]) === null || previous && previous.age_s - point.age_s > (history.point_interval_s || 120) * 2.5) {
          if (run.length) runs.push(run); run = [];
        }
        if (n(point[key]) !== null) run.push(point);
      });
      if (run.length) runs.push(run);
      if (!runs.length) return;
      runs.forEach(valid => {
        if (series === 0 && valid.length > 1) {
          const gradient = ctx.createLinearGradient(0, margin.top, 0, margin.top + ph);
          gradient.addColorStop(0, spec.colors[series] + '25'); gradient.addColorStop(1, spec.colors[series] + '03');
          ctx.beginPath(); ctx.moveTo(x(valid[0]), margin.top + ph);
          valid.forEach(point => ctx.lineTo(x(point), y(Number(point[key]))));
          ctx.lineTo(x(valid[valid.length - 1]), margin.top + ph); ctx.closePath(); ctx.fillStyle = gradient; ctx.fill();
        }
        ctx.beginPath(); ctx.strokeStyle = spec.colors[series]; ctx.lineWidth = series === 0 ? 2.2 : 1.7;
        valid.forEach((point, index) => index ? ctx.lineTo(x(point), y(Number(point[key]))) : ctx.moveTo(x(point), y(Number(point[key])))); ctx.stroke();
      });
      const last = runs[runs.length - 1].at(-1); ctx.fillStyle = spec.colors[series]; ctx.beginPath(); ctx.arc(x(last), y(Number(last[key])), 3, 0, Math.PI * 2); ctx.fill();
    });
    el['chart-legend'].replaceChildren();
    spec.names.forEach((name, index) => { const item = document.createElement('span'); item.className = 'legend-item'; item.style.setProperty('--legend-color', spec.colors[index]); item.textContent = name; el['chart-legend'].append(item); });
    const latest = chartPoints[chartPoints.length - 1];
    const oldest = chartPoints[0];
    text('chart-coverage', `Até 24 h de gráfico em memória · intervalos de ${nf((history.point_interval_s || 120) / 60, 0)} minutos. Vazão e potência mostram picos do intervalo; temperaturas, a última leitura.${oldest ? ` Cobertura disponível: ${uptime(oldest.age_s)}.` : ''} Os horários e a duração dos usos são salvos separadamente.`);
    text('chart-description', latest ? `Último ponto: ${spec.keys.map((key, index) => `${spec.names[index]} ${nf(latest[key])} ${spec.unit}`).join(', ')}. ${chartPoints.length} pontos disponíveis.` : 'Sem dados de histórico disponíveis.');
    canvas.setAttribute('aria-label', `Gráfico de ${chartMode === 'temperature' ? 'temperatura' : chartMode === 'flow' ? 'vazão' : 'potência'}. Use as setas para percorrer os pontos.`);
    if (chartCursor !== null && chartPoints.length) {
      chartCursor = Math.max(0, Math.min(chartPoints.length - 1, chartCursor));
      const point = chartPoints[chartCursor], xx = x(point);
      ctx.strokeStyle = '#9aa7a2'; ctx.lineWidth = 1; ctx.setLineDash([3, 3]); ctx.beginPath(); ctx.moveTo(xx, margin.top); ctx.lineTo(xx, margin.top + ph); ctx.stroke(); ctx.setLineDash([]);
      el['chart-tooltip'].textContent = `${new Date(Date.now() - point.age_s * 1000).toLocaleString('pt-BR', { timeZone: homeZone, day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' })}\n${spec.keys.map((key, index) => `${spec.names[index]}: ${nf(point[key])} ${spec.unit}`).join('\n')}`;
      el['chart-tooltip'].hidden = false; el['chart-tooltip'].style.left = `${Math.max(4, Math.min(w - 190, xx + 10))}px`; el['chart-tooltip'].style.top = '7px';
    } else el['chart-tooltip'].hidden = true;
  }

  function exportCSV(type) {
    const records = type === 'history' ? history.points : history.sessions;
    if (!records.length) { showToast('Ainda não há dados para exportar.'); return; }
    const quote = value => value === null || value === undefined ? '' : `"${(typeof value === 'number' ? String(value).replace('.', ',') : String(value)).replaceAll('"', '""')}"`;
    const localStamp = value => validEpoch(value) ? new Date(Number(value) * 1000).toLocaleString('pt-BR', { timeZone: homeZone }) : '';
    const utcStamp = value => validEpoch(value) ? new Date(Number(value) * 1000).toISOString() : '';
    let headings, rows;
    if (type === 'history') {
      headings = ['idade_s', 'entrada_C', 'saida_C', 'vazao_L_min', 'potencia_kW', 'chama'];
      const columns = ['age_s', 'inlet_c', 'outlet_c', 'flow_l_min', 'power_kw', 'heating'];
      rows = records.map(point => columns.map(key => typeof point[key] === 'boolean' ? Number(point[key]) : n(point[key]) === null ? '' : String(Number(point[key])).replace('.', ',')));
    } else {
      headings = ['numero', 'inicio_Brasilia', 'termino_Brasilia', 'inicio_UTC', 'termino_UTC', 'duracao_total_s', 'tempo_acompanhado_s', 'agua_L', 'energia_kWh', `gas_estimado_${gasUnit() === 'kg' ? 'kg' : 'm3'}`, 'custo_estimado_BRL', 'completo', 'motivo_termino'];
      rows = records.slice(0, 24).map(item => [item.sequence ?? '', localStamp(item.started_at_epoch_s), localStamp(item.ended_at_epoch_s), utcStamp(item.started_at_epoch_s), utcStamp(item.ended_at_epoch_s), n(item.elapsed_s) ?? '', n(item.duration_s) ?? '', n(item.water_l) ?? '', n(item.energy_kwh) ?? '', n(item.gas) ?? '', n(item.cost) ?? '', item.complete === true ? 1 : 0, item.end_reason || '']);
    }
    const content = '\uFEFF' + [headings, ...rows].map(row => row.map(quote).join(';')).join('\r\n');
    download(content, `rinnai-${type === 'history' ? 'leituras' : 'usos'}-${localDayKey(Date.now())}.csv`, 'text/csv;charset=utf-8');
  }

  function download(content, filename, type) {
    const url = URL.createObjectURL(new Blob([content], { type }));
    const link = document.createElement('a'); link.href = url; link.download = filename; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  function exportPeriods(kind) {
    const records = periods[kind];
    if (!Array.isArray(records) || !records.length) { showToast('Ainda não há períodos para exportar.'); return; }
    const columns = ['key', 'water_l', 'energy_kwh', 'gas', 'cost', 'heating_s', 'sessions', 'partial'];
    const rows = [['periodo', 'agua_L', 'energia_kWh', `gas_estimado_${gasUnit() === 'kg' ? 'kg' : 'm3'}`, 'custo_estimado_BRL', 'tempo_aquecendo_s', 'usos', 'parcial'].join(';'), ...records.map(row => columns.map(key => typeof row[key] === 'boolean' ? Number(row[key]) : n(row[key]) === null ? '' : String(Number(row[key])).replace('.', ',')).join(';'))];
    download('\uFEFF' + rows.join('\r\n'), `rinnai-${kind === 'days' ? 'diario' : 'mensal'}-${new Date().toISOString().slice(0, 10)}.csv`, 'text/csv;charset=utf-8');
  }

  function exportDiagnostics() {
    const content = { exported_at: new Date().toISOString(), firmware: state?.version || null, diagnostics, readings: state?.readings || null, communication: state?.diag || null, alerts: state?.alerts || [], preferences: { favorites: favoriteSettings(), alerts: state?.settings?.alerts || null } };
    download('Diagnóstico local do aquecedor Rinnai\r\n\r\n' + JSON.stringify(content, null, 2), `rinnai-diagnostico-${new Date().toISOString().slice(0, 10)}.txt`, 'text/plain;charset=utf-8');
  }

  function openSettings() {
    const settings = state?.settings || {};
    el['gas-type'].value = settings.gas_type === 'GLP' ? 'GLP' : 'GN';
    el['gas-tariff'].value = Number(settings.gas_tariff) > 0 ? settings.gas_tariff : '';
    el['water-tariff'].value = Number(settings.water_tariff) > 0 ? settings.water_tariff : '';
    favoriteSettings().forEach((favorite, index) => { el[`favorite${index + 1}-name`].value = favorite.name; el[`favorite${index + 1}-temp`].value = favorite.temp_c; });
    const alerts = settings.alerts || {};
    [['long-use', 'long_use_enabled'], ['flow', 'flow_enabled'], ['error', 'error_enabled'], ['offline', 'offline_enabled']].forEach(([id, key]) => { el[`alert-${id}`].checked = alerts[key] !== false; });
    el['long-use-min'].value = n(alerts.long_use_min) ?? 15; el['continuous-flow-min'].value = n(alerts.continuous_flow_min) ?? 20; el['offline-seconds'].value = n(alerts.offline_seconds) ?? 30;
    text('settings-error', ''); text('preferences-error', ''); tariffLabel();
    if (typeof el['settings-dialog'].showModal === 'function') el['settings-dialog'].showModal(); else el['settings-dialog'].setAttribute('open', '');
  }
  function closeSettings() {
    if (typeof el['settings-dialog'].close === 'function') el['settings-dialog'].close(); else el['settings-dialog'].removeAttribute('open');
    el['open-settings'].focus();
  }
  function tariffLabel() { text('gas-tariff-label', el['gas-type'].value === 'GLP' ? 'Gás · R$ por kg' : 'Gás · R$ por m³'); }
  async function saveSettings(event) {
    event.preventDefault();
    const gas = el['gas-tariff'].value.trim() === '' ? 0 : Number(el['gas-tariff'].value.replace(',', '.'));
    const water = el['water-tariff'].value.trim() === '' ? 0 : Number(el['water-tariff'].value.replace(',', '.'));
    if (!Number.isFinite(gas) || !Number.isFinite(water) || gas < 0 || water < 0 || gas > 100000 || water > 100000) { text('settings-error', 'Informe tarifas válidas, iguais ou maiores que zero.'); return; }
    el['save-settings'].disabled = true; text('settings-error', '');
    try { await api('settings', { gas_type: el['gas-type'].value, gas_tariff: String(gas), water_tariff: String(water) }); await loadState(); closeSettings(); showToast('Tarifas salvas no módulo.'); }
    catch (error) { text('settings-error', error.message); }
    finally { el['save-settings'].disabled = false; }
  }

  async function savePreferences(event) {
    event.preventDefault();
    const form = {};
    for (let i = 1; i <= 3; i++) {
      const name = el[`favorite${i}-name`].value.trim(), temperature = Number(el[`favorite${i}-temp`].value);
      if (!name || new TextEncoder().encode(name).length > 31 || /[\u0000-\u001f\u007f]/.test(name)) { text('preferences-error', `Use um nome curto e preenchido para o favorito ${i}.`); return; }
      if (!Number.isInteger(temperature) || temperature < 35 || temperature > 50) { text('preferences-error', 'Escolha temperaturas inteiras entre 35 e 50 °C.'); return; }
      form[`favorite${i}_name`] = name; form[`favorite${i}_temp`] = String(temperature);
    }
    for (const [id, key, min, max] of [['long-use-min', 'long_use_min', 1, 180], ['continuous-flow-min', 'continuous_flow_min', 1, 180], ['offline-seconds', 'offline_seconds', 10, 600]]) {
      const value = Number(el[id].value);
      if (!Number.isInteger(value) || value < min || value > max) { text('preferences-error', 'Confira os tempos dos avisos: 1–180 minutos e 10–600 segundos sem resposta.'); return; }
      form[key] = String(value);
    }
    [['long-use', 'long_use_enabled'], ['flow', 'flow_enabled'], ['error', 'error_enabled'], ['offline', 'offline_enabled']].forEach(([id, key]) => { form[key] = el[`alert-${id}`].checked ? '1' : '0'; });
    el['save-preferences'].disabled = true; text('preferences-error', '');
    try {
      await api('preferences', form);
      const deadline = Date.now() + 5000;
      let confirmed = false;
      while (Date.now() < deadline) {
        let data;
        try { data = await api('state', undefined, Math.max(50, Math.min(1200, deadline - Date.now()))); }
        catch (_) { /* Read again only; a preference request is never resent. */ }
        if (data?.diag?.storage_ok === false) throw new Error('O módulo não confirmou a gravação. Confira o diagnóstico antes de tentar novamente.');
        const saved = data?.settings, alerts = saved?.alerts || {};
        const favoriteMatch = [1, 2, 3].every(i => saved?.favorites?.[i - 1]?.name === form[`favorite${i}_name`] && Number(saved.favorites[i - 1].temp_c) === Number(form[`favorite${i}_temp`]));
        const flagsMatch = ['long_use_enabled', 'flow_enabled', 'error_enabled', 'offline_enabled'].every(key => typeof alerts[key] === 'boolean' && alerts[key] === (form[key] === '1'));
        const thresholdsMatch = ['long_use_min', 'continuous_flow_min', 'offline_seconds'].every(key => n(alerts[key]) === Number(form[key]));
        if (data && favoriteMatch && flagsMatch && thresholdsMatch) {
          state = data; receivedAt = Date.now(); transportOk = true; render(); confirmed = true; break;
        }
        await new Promise(resolve => setTimeout(resolve, Math.min(300, Math.max(0, deadline - Date.now()))));
      }
      if (!confirmed) throw new Error('O módulo não confirmou a gravação. As preferências não foram reenviadas automaticamente.');
      showToast('Favoritos e avisos salvos no módulo.'); closeSettings();
    }
    catch (error) { text('preferences-error', error.message); }
    finally { el['save-preferences'].disabled = false; }
  }

  function bindTabs(container, attribute, callback) {
    const buttons = [...container.querySelectorAll('button')];
    buttons.forEach(button => button.addEventListener('click', () => {
      buttons.forEach(item => { const selected = item === button; item.setAttribute('aria-selected', String(selected)); item.tabIndex = selected ? 0 : -1; });
      callback(button.dataset[attribute]);
    }));
    container.addEventListener('keydown', event => {
      const index = buttons.indexOf(document.activeElement); if (index < 0) return;
      let next = index;
      if (event.key === 'ArrowRight') next = (index + 1) % buttons.length;
      else if (event.key === 'ArrowLeft') next = (index - 1 + buttons.length) % buttons.length;
      else if (event.key === 'Home') next = 0; else if (event.key === 'End') next = buttons.length - 1; else return;
      event.preventDefault(); buttons[next].focus(); buttons[next].click();
    });
  }

  function boot() {
    document.documentElement.lang = 'pt-BR'; document.title = 'Rinnai · Água no seu ponto'; document.body.classList.add('rinnai-page');
    document.head.querySelectorAll('link[rel~="icon"]').forEach(node => node.remove());
    const favicon = document.createElement('link');
    favicon.rel = 'icon'; favicon.type = 'image/svg+xml'; favicon.href = window.__RINNAI_PREVIEW__ ? 'favicon.svg' : '/favicon.svg?v=110';
    document.head.append(favicon);
    root = document.createElement('main'); root.className = 'rinnai-app'; root.id = 'rinnai-dashboard'; root.innerHTML = html; document.body.replaceChildren(root);
    if (!window.__RINNAI_PREVIEW__) {
      if (!document.querySelector('script[src="/pwa.js"]')) {
        const installer = document.createElement('script'); installer.src = '/pwa.js'; installer.defer = true;
        document.head.append(installer);
      }
      if (window.isSecureContext && 'serviceWorker' in navigator) {
        navigator.serviceWorker.register('/sw.js', { scope: '/', updateViaCache: 'none' })
          .catch(error => console.warn('Não foi possível preparar a interface offline.', error));
      }
    }
    enhanceMarkup();
    root.querySelectorAll('[id]').forEach(node => { el[node.id] = node; });
    text('settings-title', 'Seu aquecedor, do seu jeito');
    el['settings-title'].nextElementSibling.textContent = 'Tarifas, temperaturas favoritas e avisos.';
    if (window.__RINNAI_PREVIEW__) el['preview-banner'].hidden = false;
    el['temperature-down'].addEventListener('click', () => changeDraft(draft - 1));
    el['temperature-up'].addEventListener('click', () => changeDraft(draft + 1));
    root.querySelectorAll('[data-preset]').forEach(button => button.addEventListener('click', () => changeDraft(Number(button.dataset.preset))));
    el['apply-temperature'].addEventListener('click', applyTemperature);
    el['open-settings'].addEventListener('click', openSettings); el['close-settings'].addEventListener('click', closeSettings); el['cancel-settings'].addEventListener('click', closeSettings);
    el['gas-type'].addEventListener('change', tariffLabel); el['settings-form'].addEventListener('submit', saveSettings);
    el['preferences-form'].addEventListener('submit', savePreferences);
    el['settings-dialog'].addEventListener('click', event => { if (event.target === el['settings-dialog']) { const r = el['settings-dialog'].getBoundingClientRect(); if (event.clientX < r.left || event.clientX > r.right || event.clientY < r.top || event.clientY > r.bottom) closeSettings(); } });
    bindTabs(el['scope-tabs'], 'scope', value => { scope = value; render(); });
    bindTabs(el['session-filters'], 'sessionScope', value => { sessionScope = value; comparedSessions.clear(); renderSessions(); });
    bindTabs(el['chart-tabs'], 'chart', value => { chartMode = value; chartCursor = null; scheduleChart(); });
    el['chart-window'].addEventListener('change', () => { chartWindow = Number(el['chart-window'].value); chartCursor = null; scheduleChart(); });
    el['export-history'].addEventListener('click', () => exportCSV('history')); el['export-sessions'].addEventListener('click', () => exportCSV('sessions'));
    el['export-days'].addEventListener('click', () => exportPeriods('days')); el['export-months'].addEventListener('click', () => exportPeriods('months')); el['export-diagnostics'].addEventListener('click', exportDiagnostics);
    const canvas = el['history-chart'];
    canvas.addEventListener('pointermove', event => {
      if (!chartPoints.length) return;
      const r = canvas.getBoundingClientRect(), px = event.clientX - r.left;
      const maxAge = chartWindow;
      const age = maxAge * (1 - Math.max(0, Math.min(1, (px - 36) / Math.max(1, r.width - 46))));
      chartCursor = chartPoints.reduce((best, point, index) => Math.abs(point.age_s - age) < Math.abs(chartPoints[best].age_s - age) ? index : best, 0); scheduleChart();
    });
    canvas.addEventListener('pointerleave', () => { chartCursor = null; scheduleChart(); });
    canvas.addEventListener('blur', () => { chartCursor = null; scheduleChart(); });
    canvas.addEventListener('keydown', event => { if (!chartPoints.length || !['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return; event.preventDefault(); if (chartCursor === null) chartCursor = chartPoints.length - 1; if (event.key === 'ArrowLeft') chartCursor--; if (event.key === 'ArrowRight') chartCursor++; if (event.key === 'Home') chartCursor = 0; if (event.key === 'End') chartCursor = chartPoints.length - 1; scheduleChart(); });
    if (window.ResizeObserver) new ResizeObserver(scheduleChart).observe(canvas.parentElement); else window.addEventListener('resize', scheduleChart);
    render(); scheduleChart(); loadState(); loadHistory(); loadPeriods(); loadDiagnostics();
    setInterval(loadState, 2000); setInterval(loadHistory, 10000); setInterval(loadPeriods, 30000); setInterval(loadDiagnostics, 15000);
    setInterval(() => { renderUpdated(); renderAges(); renderAlerts(); if (state?.session?.active && live()) text('live-duration', duration(Number(state.session.duration_s || 0) + Math.floor((Date.now() - receivedAt) / 1000))); }, 1000);
    document.addEventListener('visibilitychange', () => { if (!document.hidden) { loadState(); loadHistory(); loadPeriods(); loadDiagnostics(); } });
    window.addEventListener('offline', () => { transportOk = false; render(); });
    window.addEventListener('online', () => { loadState(); loadHistory(); loadPeriods(); loadDiagnostics(); });
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot, { once: true }); else boot();
})();
