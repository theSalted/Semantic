const $ = (id) => document.getElementById(id);
let busy = false;
let noticeTimer;
let bankRevision;
let configuration;
let methods = [];
let conversations = [];
let conversation;
let round = 0;

function beginRecord() {
  conversation = { id: crypto.randomUUID(), startedAt: new Date().toISOString(),
    type: 'user-driven comparison', bankRevision, configuration, methods: {} };
  for (const method of methods) conversation.methods[method.id] = [];
  conversations.push(conversation);
}
function ensureEditable() {
  if (conversation && Object.values(conversation.methods).some(turns => turns.length))
    throw new Error('Start a new conversation before changing the phrase bank.');
}
function methodCard(method) {
  const card = document.createElement('section');
  card.className = 'method-card';
  const label = document.createElement('p'); label.textContent = method.name;
  const status = document.createElement('p'); status.className = 'method-status'; status.textContent = method.note;
  const log = document.createElement('div'); log.className = 'method-log';
  log.setAttribute('role', 'log'); log.setAttribute('aria-label', method.name + ' conversation');
  card.append(label, status, log); method.status = status; method.log = log;
  return card;
}
async function initializeMethods() {
  configuration = await api('/api/methods', undefined, 'GET');
  methods = configuration.methods.filter(x => x.available).map(x => ({...x, sessionId: null, queue: Promise.resolve(), pending: 0}));
  $('comparisons').replaceChildren(...methods.map(methodCard));
  $('unavailable').textContent = configuration.methods.filter(x => !x.available)
    .map(x => x.name + ': ' + x.note).join('\n');
  beginRecord();
}


function clearMessages() {
  clearTimeout(noticeTimer);
  $('notice').hidden = true;
  $('error').hidden = true;
}

function showError(error) {
  $('error').textContent = error instanceof TypeError ? 'Cannot reach the local server.' : error.message;
  $('error').hidden = false;
}

async function api(route, body, method = 'POST', signal) {
  const response = await fetch(route, {
    method, signal, headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
  });
  const result = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(result.error || 'Could not complete the request. Try again.');
  return result;
}

async function run(button, label, action) {
  if (busy) return;
  busy = true;
  clearMessages();
  const originalLabel = button?.textContent;
  if (button) button.textContent = label;
  document.querySelectorAll('input, button, textarea, select').forEach(control => control.disabled = true);
  try { await action(); }
  catch (error) { showError(error); }
  finally {
    busy = false;
    if (button) button.textContent = originalLabel;
    document.querySelectorAll('input, button, textarea, select').forEach(control => control.disabled = false);
  }
}

function notice(message) {
  $('response').hidden = true;
  $('notice').textContent = message;
  $('notice').hidden = false;
  noticeTimer = setTimeout(() => { $('notice').hidden = true; }, 5000);
}

async function saved(result) {
  await refreshBank();
  notice(`${result.added} added.${result.skipped ? ` ${result.skipped} already exist.` : ''}`);
}

function fit(field) {
  field.style.height = 'auto';
  field.style.height = `${field.scrollHeight + 2}px`;
}

function renderBank(snapshot) {
  if (bankRevision && bankRevision !== snapshot.revision) {
    for (const method of methods) { method.sessionId = null; method.log.replaceChildren(); }
    if (configuration) beginRecord();
  }
  bankRevision = snapshot.revision;
  if (conversation && !Object.values(conversation.methods).some(x => x.length)) conversation.bankRevision = bankRevision;
  $('add-form').hidden = Boolean(snapshot.readOnly);
  $('bank-title').textContent = snapshot.catalog ? 'Response catalog' : 'Phrase bank';
  if (snapshot.catalog) {
    const catalog = snapshot.catalog;
    const summary = document.createElement('p');
    summary.textContent = `${catalog.responseCount.toLocaleString()} responses · ${catalog.keyCount.toLocaleString()} keys · ${catalog.keyedResponseCount.toLocaleString()} responses with keys · ${catalog.status}`;
    const choice = document.createElement('select');
    choice.setAttribute('aria-label', 'Catalog response');
    for (const response of catalog.responses) {
      const option = document.createElement('option');
      option.value = response.responseId; option.textContent = response.responseId;
      choice.append(option);
    }
    const text = document.createElement('textarea'); text.readOnly = true; text.rows = 4;
    text.setAttribute('aria-label', 'Exact authored response');
    const keys = document.createElement('p'); keys.style.whiteSpace = 'pre-wrap';
    const show = () => {
      text.value = catalog.responses[choice.selectedIndex].text;
      const matched = catalog.keys.filter(k => k.responseId === choice.value);
      keys.textContent = matched.length ? matched.map(k => `${k.keyId}: ${k.text}`).join('\n') : 'No indexed keys. This response remains in the catalog.';
      fit(text);
    };
    choice.addEventListener('change', show);
    $('bank').replaceChildren(summary, choice, text, keys); show();
    return;
  }
  // Reuse unchanged rows so saving one pair does not erase edits in another.
  const existing = new Map();
  for (const row of $('bank').children) {
    if (!existing.has(row.bankKey)) existing.set(row.bankKey, []);
    existing.get(row.bankKey).push(row);
  }
  const rows = snapshot.phrases.map((phrase, index) => {
    const key = JSON.stringify(phrase);
    const row = existing.get(key)?.shift() || makeRow(phrase);
    row.bankKey = key;
    row.dataset.index = index;
    row.setAttribute('aria-label', `Phrase ${index + 1}`);
    row.querySelector('[name=prompt]').setAttribute('aria-label', `Prompt ${index + 1}`);
    row.querySelector('[name=response]').setAttribute('aria-label', `Response ${index + 1}`);
    return row;
  });
  $('bank').replaceChildren(...rows);
  $('bank').querySelectorAll('textarea').forEach(fit);
}

function makeRow(phrase) {
  const row = document.createElement('form');
  row.className = 'bank-row';
  for (const [name, maximum] of [['prompt', 1000], ['response', 8000]]) {
    const field = document.createElement('textarea');
    field.name = name;
    field.value = phrase[name];
    field.rows = 1;
    field.maxLength = maximum;
    field.required = true;
    field.addEventListener('input', () => fit(field));
    row.append(field);
  }
  const actions = document.createElement('div');
  actions.className = 'actions';
  const save = document.createElement('button');
  save.type = 'submit';
  save.textContent = 'Save';
  const remove = document.createElement('button');
  remove.type = 'button';
  remove.textContent = 'Delete';
  actions.append(save, remove);
  row.append(actions);
  row.addEventListener('submit', async event => {
    event.preventDefault();
    await run(save, 'Saving…', async () => {
      ensureEditable();
      const result = await api(`/api/phrases/${row.dataset.index}`, {
        revision: bankRevision,
        prompt: row.querySelector('[name=prompt]').value,
        response: row.querySelector('[name=response]').value,
      }, 'PUT');
      row.remove();
      renderBank(result);
      notice('Saved.');
    });
  });
  remove.addEventListener('click', async () => {
    await run(remove, 'Deleting…', async () => {
      ensureEditable();
      const result = await api(`/api/phrases/${row.dataset.index}`, { revision: bankRevision }, 'DELETE');
      row.remove();
      renderBank(result);
      notice('Deleted.');
    });
  });
  return row;
}

async function refreshBank() {
  renderBank(await api('/api/phrases', undefined, 'GET'));
}

async function sendShared() {
  if (!$('input').reportValidity()) return;
  const input = $('input').value.trim();
  if (!input) throw new Error('Enter a message.');
  const queryId = ++round;
  const record = conversation;
  $('input').value = '';
  // Each method has its own ordered queue. Slow methods never block faster cards.
  for (const method of methods) {
    const turn = { queryId, requestId: crypto.randomUUID(), user: input,
      queuedAt: new Date().toISOString(), methodId: method.id };
    record.methods[method.id].push(turn);
    const line = document.createElement('p'); line.textContent = `You: ${input}\nPending…`; method.log.append(line);
    method.pending++;
    method.status.textContent = `${method.pending} pending`;
    method.queue = method.queue.then(async () => {
      if (record !== conversation) { turn.error = 'Cancelled by new conversation before sending'; return; }
      const controller = new AbortController(); method.controller = controller;
      try {
        if (!method.sessionId) {
          const session = await api('/api/session', {methodId: method.id}, 'POST', controller.signal);
          if (record !== conversation) { await api('/api/reset', {sessionId: session.sessionId}); return; }
          method.sessionId = session.sessionId; turn.historyStorageBudget = session.tokenLimit;
        }
        if (record !== conversation) return;
        turn.startedAt = new Date().toISOString();
        const start = performance.now();
        const result = await api('/api/respond', { input, sessionId: method.sessionId,
          methodId: method.id, requestId: turn.requestId }, 'POST', controller.signal);
        Object.assign(turn, result, { elapsedMs: performance.now() - start, completedAt: new Date().toISOString() });
        const label = result.provenance === 'generated' ? 'Generated reply' : 'Selected authored reply';
        line.textContent = `You: ${input}\nCharacter: ${result.response}\n${label}`;
        if (record === conversation) {
          const tokens = result.metadata.promptTokens ?? result.metadata.contextTokens;
          method.status.textContent = tokens === undefined ? label : `${tokens} prompt tokens · ${label.toLowerCase()}`;
        }
      } catch (error) {
        turn.error = error.name === 'AbortError' ? 'Cancelled by new conversation' : error.message;
        turn.completedAt = new Date().toISOString();
        line.textContent = `You: ${input}\nError: ${turn.error}`;
        if (record === conversation) method.status.textContent = 'This method failed; other results are independent.';
      } finally {
        if (method.controller === controller) method.controller = null;
        if (record === conversation) { method.pending--; if (method.pending) method.status.textContent = `${method.pending} pending`; }
      }
    });
  }
}

$('match-form').addEventListener('submit', async event => {
  event.preventDefault();
  await run($('match-button'), 'Sending…', sendShared);
  $('input').focus();
});
async function resetConversation() {
  // Detach queued work first. The server reset invalidates all pending commits.
  const previous = conversation; conversation = null;
  for (const method of methods) method.controller?.abort();
  await Promise.allSettled(methods.map(async method => {
    if (method.sessionId) {
      try { await api('/api/reset', { sessionId: method.sessionId }); }
      catch { method.sessionId = null; }
    }
    method.queue = Promise.resolve(); method.pending = 0;
    method.log.replaceChildren(); method.status.textContent = method.note;
  }));
  previous.endedAt = new Date().toISOString();
  beginRecord();
  $('input').value = '';
}
$('reset-button').addEventListener('click', () => run($('reset-button'), 'Resetting…', resetConversation));
$('refresh-button').addEventListener('click', () => run($('refresh-button'), 'Checking…', async () => {
  await resetConversation();
  await Promise.allSettled(methods.filter(m => m.sessionId).map(m => api('/api/close', {sessionId: m.sessionId})));
  conversations.pop(); await initializeMethods();
}));
$('export-button').addEventListener('click', () => {
  const data = { format: 'semantic-human-comparison-v2', exportedAt: new Date().toISOString(), conversations };
  const url = URL.createObjectURL(new Blob([JSON.stringify(data, null, 2)], {type: 'application/json'}));
  const link = document.createElement('a'); link.href = url; link.download = 'semantic-comparison.json'; link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
});
$('input').addEventListener('input', clearMessages);

$('add-form').addEventListener('submit', async event => {
  event.preventDefault();
  await run($('add-button'), 'Saving…', async () => {
    ensureEditable();
    const result = await api('/api/phrases', { prompt: $('prompt').value, response: $('answer').value });
    await saved(result);
    $('add-form').reset();
  });
});

$('import-button').addEventListener('click', () => $('file').click());
$('file').addEventListener('change', async () => {
  const file = $('file').files[0];
  if (!file) return;
  await run($('import-button'), 'Loading…', async () => {
    ensureEditable();
    if (file.size > 1024 * 1024) throw new Error('Use a TSV file under 1 MB.');
    const bytes = await file.arrayBuffer();
    let tsv;
    try { tsv = new TextDecoder('utf-8', { fatal: true }).decode(bytes); }
    catch { throw new Error('The TSV file must use UTF-8 text.'); }
    await saved(await api('/api/import', { tsv }));
  });
  $('file').value = '';
});

window.addEventListener('resize', () => $('bank').querySelectorAll('textarea').forEach(fit));
run(null, null, async () => { await refreshBank(); await initializeMethods(); });
