/* Installation help only. This module never reads or changes heater settings. */
(() => {
  'use strict';
  if (window.__rinnaiPWABooted) return;
  window.__rinnaiPWABooted = true;

  const displayMode = window.matchMedia('(display-mode: standalone)');
  let installed = displayMode.matches || navigator.standalone === true;
  let installPrompt = null, promptBusy = false, promptMessage = '';
  let ui = null, observer = null;

  const node = (tag, className, text) => {
    const element = document.createElement(tag);
    if (className) element.className = className;
    if (text !== undefined) element.textContent = text;
    return element;
  };

  function glyph(close = false) {
    const ns = 'http://www.w3.org/2000/svg';
    const svg = document.createElementNS(ns, 'svg');
    svg.setAttribute('viewBox', '0 0 24 24');
    svg.setAttribute('fill', 'none');
    svg.setAttribute('stroke', 'currentColor');
    svg.setAttribute('stroke-width', '1.7');
    svg.setAttribute('stroke-linecap', 'round');
    svg.setAttribute('stroke-linejoin', 'round');
    svg.setAttribute('aria-hidden', 'true');
    const path = document.createElementNS(ns, 'path');
    path.setAttribute('d', close ? 'm6 6 12 12M6 18 18 6' : 'M9 3h6a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H9a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2ZM11 17h2');
    svg.append(path);
    return svg;
  }

  function instructions(title, steps, className = '') {
    const section = node('section', `rinnai-pwa-guide ${className}`.trim());
    section.append(node('h3', '', title));
    const list = node('ol');
    steps.forEach(step => list.append(node('li', '', step)));
    section.append(list);
    return section;
  }

  function installationAddress() {
    if (typeof window.__RINNAI_PWA_URL__ !== 'string') return null;
    try {
      const address = new URL(window.__RINNAI_PWA_URL__, location.href);
      return address.protocol === 'https:' && !address.username && !address.password ? address : null;
    } catch (_) {
      return null;
    }
  }

  function updateState() {
    if (!ui) return;
    ui.entry.hidden = installed;
    ui.install.hidden = installed || !installPrompt || !window.isSecureContext;
    ui.install.disabled = promptBusy;
    ui.install.textContent = promptBusy ? 'Aguardando o navegador…' : 'Instalar aplicativo';
    ui.status.textContent = installed ? 'Aplicativo instalado. Abra o Aquecedor pelo ícone da tela inicial.' :
      promptMessage || (window.isSecureContext ? 'Adicione o Aquecedor à tela inicial para abrir em uma janela de aplicativo.' :
        'No iPhone, adicione pelo Safari para abrir como aplicativo. Este endereço precisa de conexão com a rede para carregar o painel.');
    ui.status.classList.toggle('installed', installed);
    ui.offline.hidden = navigator.onLine !== false;
  }

  function closeDialog() {
    if (!ui) return;
    if (typeof ui.dialog.close === 'function') ui.dialog.close();
    else {
      ui.dialog.removeAttribute('open');
      ui.trigger.focus({ preventScroll: true });
    }
  }

  async function requestInstallation() {
    if (!installPrompt || promptBusy || installed || !window.isSecureContext) return;
    const prompt = installPrompt;
    installPrompt = null; // Each browser event can be used only once.
    promptBusy = true;
    promptMessage = 'Confirme a instalação na janela do navegador.';
    updateState();
    try {
      const result = await prompt.prompt();
      const choice = prompt.userChoice ? await prompt.userChoice : result;
      promptMessage = choice?.outcome === 'accepted' ? 'Instalação solicitada. Aguarde a confirmação do navegador.' :
        'Instalação cancelada. Você pode usar as instruções abaixo quando quiser.';
    } catch (_) {
      promptMessage = 'O navegador não concluiu a instalação. Use as instruções abaixo para adicionar à tela inicial.';
    } finally {
      promptBusy = false;
      updateState();
    }
  }

  function mount() {
    if (ui) return true;
    const dashboard = document.getElementById('rinnai-dashboard');
    const footer = dashboard?.querySelector('.dashboard-footer');
    if (!dashboard || !footer) return false;

    const entry = node('div', 'rinnai-pwa-entry');
    const trigger = node('button', 'rinnai-pwa-trigger', 'Instalar no celular');
    trigger.type = 'button';
    trigger.id = 'pwa-install-button';
    trigger.setAttribute('aria-haspopup', 'dialog');
    trigger.setAttribute('aria-controls', 'pwa-install-dialog');
    trigger.prepend(glyph());
    entry.append(trigger);
    footer.before(entry);

    const dialog = node('dialog', 'rinnai-pwa-dialog');
    dialog.id = 'pwa-install-dialog';
    dialog.setAttribute('aria-labelledby', 'pwa-install-title');
    dialog.setAttribute('aria-describedby', 'pwa-install-description');
    const header = node('div', 'rinnai-pwa-heading');
    const heading = node('div');
    const title = node('h2', '', 'Aquecedor no seu celular');
    title.id = 'pwa-install-title';
    const description = node('p', '', 'Seu painel, direto da tela inicial.');
    description.id = 'pwa-install-description';
    heading.append(title, description);
    const close = node('button', 'rinnai-pwa-close');
    close.type = 'button';
    close.setAttribute('aria-label', 'Fechar instruções de instalação');
    close.append(glyph(true));
    close.addEventListener('click', closeDialog);
    header.append(heading, close);

    const body = node('div', 'rinnai-pwa-body');
    const status = node('p', 'rinnai-pwa-status');
    status.id = 'pwa-install-status';
    status.setAttribute('role', 'status');
    status.setAttribute('aria-live', 'polite');
    const install = node('button', 'rinnai-pwa-native-install', 'Instalar aplicativo');
    install.type = 'button';
    install.id = 'pwa-native-install';
    install.hidden = true;
    install.addEventListener('click', requestInstallation);
    body.append(status, install);

    const address = installationAddress();
    if (!window.isSecureContext && address && address.origin !== location.origin) {
      const link = node('a', 'rinnai-pwa-address', 'Abrir o endereço de instalação');
      link.href = address.href;
      body.append(link);
    }

    body.append(instructions('iPhone · Safari', [
      'Abra este painel no Safari.',
      'Toque em Compartilhar. Dependendo do Safari, essa opção fica no menu da página.',
      'Escolha Adicionar à Tela de Início.',
      'Use o nome Aquecedor e mantenha Abrir como App ativado, se essa opção aparecer.',
      'Toque em Adicionar. Depois, abra pelo novo ícone.'
    ], 'rinnai-pwa-iphone'));

    const android = node('details', 'rinnai-pwa-android');
    android.append(node('summary', '', 'Instalar em Android'));
    android.append(instructions('Android · Chrome', [
      'Abra este endereço no Chrome.',
      'No menu de três pontos, escolha Instalar aplicativo ou Adicionar à tela inicial.',
      'Confirme o nome Aquecedor e a opção oferecida pelo navegador.'
    ]));
    if (!window.isSecureContext) android.append(node('p', 'rinnai-pwa-note', 'Neste endereço HTTP, o Android pode oferecer apenas um atalho. A instalação completa depende de um endereço HTTPS e do suporte do navegador.'));
    body.append(android);

    const connection = node('p', 'rinnai-pwa-note', 'Para consultar os dados e ajustar a temperatura, o celular precisa alcançar o aquecedor pela rede.');
    const offline = node('p', 'rinnai-pwa-offline', 'Celular sem conexão com a rede. Reconecte para obter leituras atuais e enviar ajustes.');
    offline.setAttribute('role', 'status');
    offline.hidden = true;
    body.append(connection, offline);
    dialog.append(header, body);
    dashboard.append(dialog);
    ui = { entry, trigger, dialog, status, install, offline };

    trigger.addEventListener('click', () => {
      updateState();
      if (dialog.open) return;
      if (typeof dialog.showModal === 'function') dialog.showModal();
      else {
        dialog.setAttribute('open', '');
        close.focus();
      }
    });
    dialog.addEventListener('close', () => { if (!entry.hidden) trigger.focus({ preventScroll: true }); });
    dialog.addEventListener('keydown', event => {
      if (event.key === 'Escape' && typeof dialog.close !== 'function') { event.preventDefault(); closeDialog(); }
    });
    dialog.addEventListener('click', event => {
      if (event.target !== dialog) return;
      const rect = dialog.getBoundingClientRect();
      if (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom) closeDialog();
    });
    updateState();
    observer?.disconnect();
    return true;
  }

  window.addEventListener('beforeinstallprompt', event => {
    if (installed || !window.isSecureContext || typeof event.prompt !== 'function') return;
    event.preventDefault();
    installPrompt = event;
    promptMessage = '';
    updateState();
  });
  window.addEventListener('appinstalled', () => {
    installed = true;
    installPrompt = null;
    promptMessage = '';
    updateState();
  });
  const updateDisplayMode = () => {
    installed = displayMode.matches || navigator.standalone === true;
    updateState();
  };
  if (typeof displayMode.addEventListener === 'function') displayMode.addEventListener('change', updateDisplayMode);
  else if (typeof displayMode.addListener === 'function') displayMode.addListener(updateDisplayMode);
  window.addEventListener('online', updateState);
  window.addEventListener('offline', updateState);

  function waitForDashboard() {
    if (mount()) return;
    observer = new MutationObserver(mount);
    observer.observe(document.documentElement, { childList: true, subtree: true });
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', waitForDashboard, { once: true });
  else waitForDashboard();
})();
