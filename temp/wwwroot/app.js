const $ = (id) => document.getElementById(id);
let busy = false;
let noticeTimer;
let bankRevision;

function clearMessages() {
  clearTimeout(noticeTimer);
  $('notice').hidden = true;
  $('error').hidden = true;
}

function showError(error) {
  $('error').textContent = error instanceof TypeError ? 'Cannot reach the local server.' : error.message;
  $('error').hidden = false;
}

async function api(route, body, method = 'POST') {
  const response = await fetch(route, {
    method, headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
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
  document.querySelectorAll('input, button, textarea').forEach(control => control.disabled = true);
  try { await action(); }
  catch (error) { showError(error); }
  finally {
    busy = false;
    if (button) button.textContent = originalLabel;
    document.querySelectorAll('input, button, textarea').forEach(control => control.disabled = false);
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
  bankRevision = snapshot.revision;
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

$('match-form').addEventListener('submit', async event => {
  event.preventDefault();
  await run($('match-button'), 'Matching…', async () => {
    $('response').hidden = true;
    const result = await api('/api/match', { input: $('input').value });
    $('response').textContent = result.response;
    $('response').hidden = false;
  });
  $('input').focus();
});
$('input').addEventListener('input', () => { $('response').hidden = true; clearMessages(); });

$('add-form').addEventListener('submit', async event => {
  event.preventDefault();
  await run($('add-button'), 'Saving…', async () => {
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
run(null, null, refreshBank);
