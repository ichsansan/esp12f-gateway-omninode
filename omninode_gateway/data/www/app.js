/* ═══════════════════════════════════════════════════════════
   Omni-Node — Dashboard JavaScript
   WebSocket live-stream, config forms, OTA upload
   ═══════════════════════════════════════════════════════════ */

(function () {
  'use strict';

  // ── DOM refs ──────────────────────────────────────────
  const $ = (s) => document.querySelector(s);
  const $$ = (s) => document.querySelectorAll(s);

  // ── State ─────────────────────────────────────────────
  let config = {};
  let ws = null;
  let wsRetryTimer = null;

  // ── Tabs ──────────────────────────────────────────────
  $$('.tab').forEach((btn) => {
    btn.addEventListener('click', () => {
      $$('.tab').forEach((b) => b.classList.remove('active'));
      $$('.tab-panel').forEach((p) => p.classList.remove('active'));
      btn.classList.add('active');
      $('#tab-' + btn.dataset.tab).classList.add('active');
    });
  });

  // ── Toast ─────────────────────────────────────────────
  function toast(msg, isError) {
    const t = $('#toast');
    t.textContent = msg;
    t.className = 'toast' + (isError ? ' error' : '');
    setTimeout(() => t.classList.add('hidden'), 3000);
  }

  // ── Helpers ───────────────────────────────────────────
  function fmt(s) {
    return (s / 1000).toFixed(0) + 's';
  }
  function fmtUptime(s) {
    const d = Math.floor(s / 86400);
    const h = Math.floor((s % 86400) / 3600);
    const m = Math.floor((s % 3600) / 60);
    const sec = s % 60;
    let r = '';
    if (d) r += d + 'd ';
    if (h || d) r += h + 'h ';
    r += m + 'm ' + sec + 's';
    return r;
  }

  // ── Fetch helper ──────────────────────────────────────
  async function api(url, opts) {
    try {
      const res = await fetch(url, opts);
      if (res.status === 401) {
        toast('Auth required – please login', true);
        return null;
      }
      return await res.json();
    } catch (e) {
      toast('Request failed: ' + e.message, true);
      return null;
    }
  }

  // ═══════════════════════════════════════════════════════
  //  WebSocket
  // ═══════════════════════════════════════════════════════
  function connectWS() {
    if (ws && ws.readyState <= 1) return;
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    ws = new WebSocket(proto + '://' + location.host + '/ws');

    ws.onopen = () => {
      console.log('[WS] Connected');
      $('#hdr-status').className = 'badge badge-on';
      $('#hdr-status').textContent = 'LIVE';
    };

    ws.onmessage = (ev) => {
      try {
        const d = JSON.parse(ev.data);
        updateLive(d);
      } catch (e) {
        console.error('[WS] Parse error', e);
      }
    };

    ws.onclose = () => {
      $('#hdr-status').className = 'badge badge-off';
      $('#hdr-status').textContent = 'OFFLINE';
      // Auto-reconnect
      clearTimeout(wsRetryTimer);
      wsRetryTimer = setTimeout(connectWS, 3000);
    };
  }

  function updateLive(d) {
    // Header time
    if (d.ts) $('#hdr-time').textContent = d.ts;
    // Dashboard quick stats from WS
    if (d.uptime !== undefined) $('#dash-uptime').textContent = fmtUptime(d.uptime);
    if (d.heap !== undefined) $('#dash-heap').textContent = (d.heap / 1024).toFixed(1) + ' kB';
    if (d.mqtt !== undefined) {
      $('#dash-mqtt').textContent = d.mqtt ? 'Connected' : 'Disconnected';
      $('#dash-mqtt').style.color = d.mqtt ? '#1a1a1a' : '#ff1744';
    }
    if (d.wifi_rssi !== undefined) $('#dash-rssi').textContent = d.wifi_rssi + ' dBm';

    // Live IO
    if (d.pins && d.pins.length > 0) {
      const container = $('#live-io');
      container.innerHTML = '';
      d.pins.forEach((p) => {
        const card = document.createElement('div');
        card.className = 'card';
        const isOutput = p.type && p.type.includes('output');
        card.innerHTML =
          '<div class="card-label">' + escHtml(p.label) + ' (GPIO' + p.pin + ')</div>' +
          '<div class="card-value">' +
          (isOutput
            ? '<button class="btn btn-accent" style="padding:4px 12px;font-size:0.75rem" onclick="togglePin(' + p.pin + ',' + (p.value > 0.5 ? 0 : 1) + ')">' + (p.value > 0.5 ? 'ON' : 'OFF') + '</button>'
            : p.value.toFixed(2)) +
          '</div>';
        container.appendChild(card);
      });
    }
  }

  // Toggle output pin via MQTT (publish set command — needs server-side handling)
  // For now we do it via a simple API call
  window.togglePin = function (pin, val) {
    // Not directly possible without extending the firmware API.
    // This is a placeholder – in production, send via WS or a dedicated endpoint.
    toast('Toggle GPIO' + pin + ' → ' + val, false);
  };

  // ═══════════════════════════════════════════════════════
  //  Load Status & Config
  // ═══════════════════════════════════════════════════════
  async function loadStatus() {
    const d = await api('/api/status');
    if (!d) return;
    $('#dash-device').textContent = d.device_id;
    $('#dash-ip').textContent = d.wifi_ip;
    $('#dash-rssi').textContent = d.wifi_rssi + ' dBm';
    $('#dash-mqtt').textContent = d.mqtt_connected ? 'Connected' : 'Disconnected';
    $('#dash-uptime').textContent = fmtUptime(d.uptime);
    $('#dash-heap').textContent = (d.heap / 1024).toFixed(1) + ' kB';
    $('#dash-fw').textContent = 'v' + d.fw_version;
    $('#dash-mode').textContent = d.ap_mode ? 'AP (Setup)' : 'STA';
    if (d.time) $('#hdr-time').textContent = d.time;
  }

  async function loadConfig() {
    const d = await api('/api/config');
    if (!d) return;
    config = d;
    populateForms(d);
  }

  function populateForms(c) {
    // Network
    if (c.network) {
      $('#net-ssid').value   = c.network.ssid || '';
      $('#net-pass').value   = c.network.pass || '';
      const st = c.network.static_ip || false;
      $('#net-static').checked = st;
      $('#net-ip').value     = c.network.ip || '';
      $('#net-subnet').value = c.network.subnet || '';
      $('#net-gw').value     = c.network.gw || '';
      $('#net-dns1').value   = c.network.dns1 || '8.8.8.8';
      $('#net-dns2').value   = c.network.dns2 || '1.1.1.1';
      toggleStaticFields(st);
    }
    // MQTT
    if (c.mqtt) {
      $('#mqtt-broker').value = c.mqtt.broker || '';
      $('#mqtt-port').value   = c.mqtt.port || 1883;
      $('#mqtt-user').value   = c.mqtt.user || '';
      $('#mqtt-pass').value   = c.mqtt.pass || '';
      $('#mqtt-prefix').value = c.mqtt.prefix || 'nodes/01';
      $('#mqtt-lwt').value    = c.mqtt.lwt_topic || 'status';
    }
    // System
    $('#sys-device-id').value = c.device_id || 'OMNI-01';
    if (c.system) {
      $('#sys-web-pass').value = c.system.web_pass || '';
      $('#sys-poll').value     = c.system.poll_interval || 5000;
    }
    // IO
    renderIO(c.io_setup || []);
  }

  // ── Static IP toggle ──────────────────────────────────
  function toggleStaticFields(on) {
    ['#net-ip', '#net-subnet', '#net-gw'].forEach((s) => {
      $(s).disabled = !on;
    });
  }
  $('#net-static').addEventListener('change', function () {
    toggleStaticFields(this.checked);
  });

  // ═══════════════════════════════════════════════════════
  //  IO Pin Editor
  // ═══════════════════════════════════════════════════════
  function renderIO(pins) {
    const list = $('#io-list');
    list.innerHTML = '';
    (pins || []).forEach((p, i) => addIORow(p, i));
  }

  function addIORow(p, idx) {
    const list = $('#io-list');
    const row = document.createElement('div');
    row.className = 'io-row';
    row.dataset.idx = idx !== undefined ? idx : list.children.length;
    row.innerHTML =
      '<label>GPIO<input type="number" class="io-pin" min="0" max="16" value="' + (p ? p.pin : 0) + '"></label>' +
      '<label>Label<input type="text" class="io-label" maxlength="31" value="' + escAttr(p ? p.label : 'gpio') + '"></label>' +
      '<label>Type<select class="io-type">' +
        '<option value="input_analog"' + (p && p.type === 'input_analog' ? ' selected' : '') + '>Input Analog</option>' +
        '<option value="input_digital"' + (p && p.type === 'input_digital' ? ' selected' : '') + '>Input Digital</option>' +
        '<option value="output_digital"' + (p && p.type === 'output_digital' ? ' selected' : '') + '>Output Digital</option>' +
      '</select></label>' +
      '<label>Var Type<select class="io-vartype">' +
        '<option value="float32"' + (p && p.var_type === 'float32' ? ' selected' : '') + '>float32</option>' +
        '<option value="int16"' + (p && p.var_type === 'int16' ? ' selected' : '') + '>int16</option>' +
        '<option value="uint16"' + (p && p.var_type === 'uint16' ? ' selected' : '') + '>uint16</option>' +
        '<option value="bool"' + (p && p.var_type === 'bool' ? ' selected' : '') + '>bool</option>' +
      '</select></label>' +
      '<label>Multi<input type="number" class="io-mult" step="0.01" value="' + (p ? (p.multiplier || 1) : 1) + '"></label>' +
      '<button type="button" class="btn-remove" title="Remove">✕</button>';

    // Enable checkbox as data attribute
    const chk = document.createElement('input');
    chk.type = 'checkbox';
    chk.className = 'io-enabled';
    chk.checked = p ? p.enabled : true;
    // Insert before remove button
    const firstLabel = row.querySelector('label');
    const enableLabel = document.createElement('label');
    enableLabel.className = 'checkbox-row';
    enableLabel.style.gridColumn = '1 / -1';
    enableLabel.appendChild(chk);
    const span = document.createElement('span');
    span.textContent = 'Enabled';
    enableLabel.appendChild(span);
    row.insertBefore(enableLabel, row.firstChild);

    // Remove handler
    row.querySelector('.btn-remove').addEventListener('click', () => row.remove());

    list.appendChild(row);
  }

  $('#btn-add-pin').addEventListener('click', () => addIORow(null));

  function collectIO() {
    const rows = $$('.io-row');
    const arr = [];
    rows.forEach((r) => {
      arr.push({
        pin: parseInt(r.querySelector('.io-pin').value, 10),
        label: r.querySelector('.io-label').value,
        type: r.querySelector('.io-type').value,
        var_type: r.querySelector('.io-vartype').value,
        multiplier: parseFloat(r.querySelector('.io-mult').value) || 1,
        enabled: r.querySelector('.io-enabled').checked,
      });
    });
    return arr;
  }

  // ═══════════════════════════════════════════════════════
  //  Save Handlers
  // ═══════════════════════════════════════════════════════
  function buildFullConfig() {
    return {
      device_id: $('#sys-device-id').value,
      network: {
        ssid: $('#net-ssid').value,
        pass: $('#net-pass').value,
        static_ip: $('#net-static').checked,
        ip: $('#net-ip').value,
        subnet: $('#net-subnet').value,
        gw: $('#net-gw').value,
        dns1: $('#net-dns1').value,
        dns2: $('#net-dns2').value,
      },
      mqtt: {
        broker: $('#mqtt-broker').value,
        port: parseInt($('#mqtt-port').value, 10) || 1883,
        user: $('#mqtt-user').value,
        pass: $('#mqtt-pass').value,
        prefix: $('#mqtt-prefix').value,
        lwt_topic: $('#mqtt-lwt').value,
      },
      io_setup: collectIO(),
      system: {
        web_pass: $('#sys-web-pass').value,
        poll_interval: parseInt($('#sys-poll').value, 10) || 5000,
      },
    };
  }

  async function saveAll(section) {
    const payload = buildFullConfig();
    const res = await api('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    });
    if (res && res.ok) {
      toast(section + ' saved! Restart to apply.', false);
    } else {
      toast('Save failed', true);
    }
  }

  $('#form-network').addEventListener('submit', (e) => { e.preventDefault(); saveAll('Network'); });
  $('#form-mqtt').addEventListener('submit', (e) => { e.preventDefault(); saveAll('MQTT'); });
  $('#form-system').addEventListener('submit', (e) => { e.preventDefault(); saveAll('System'); });
  $('#btn-save-io').addEventListener('click', () => saveAll('IO'));

  // ── Restart & Factory Reset ───────────────────────────
  $('#btn-restart').addEventListener('click', async () => {
    if (!confirm('Restart the device?')) return;
    await api('/api/restart', { method: 'POST' });
    toast('Restarting…', false);
  });

  $('#btn-factory').addEventListener('click', async () => {
    if (!confirm('⚠ FACTORY RESET?\nAll settings will be erased!')) return;
    await api('/api/factory-reset', { method: 'POST' });
    toast('Factory reset…', false);
  });

  // ── OTA Upload ────────────────────────────────────────
  $('#form-ota').addEventListener('submit', async (e) => {
    e.preventDefault();
    const fileInput = $('#ota-file');
    if (!fileInput.files.length) { toast('Select a .bin file', true); return; }

    const file = fileInput.files[0];
    const formData = new FormData();
    formData.append('firmware', file, file.name);

    $('#ota-progress').style.display = 'block';

    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/ota', true);

    xhr.upload.onprogress = (ev) => {
      if (ev.lengthComputable) {
        const pct = Math.round((ev.loaded / ev.total) * 100);
        $('#ota-fill').style.width = pct + '%';
      }
    };

    xhr.onload = () => {
      try {
        const res = JSON.parse(xhr.responseText);
        toast(res.msg || 'Upload done', !res.ok);
      } catch (_) {
        toast('Upload complete', false);
      }
    };

    xhr.onerror = () => toast('Upload failed', true);
    xhr.send(formData);
  });

  // ── Escape helpers ────────────────────────────────────
  function escHtml(s) {
    const d = document.createElement('div');
    d.textContent = s;
    return d.innerHTML;
  }
  function escAttr(s) {
    return String(s || '').replace(/"/g, '&quot;').replace(/</g, '&lt;');
  }

  // ═══════════════════════════════════════════════════════
  //  Init
  // ═══════════════════════════════════════════════════════
  loadStatus();
  loadConfig();
  connectWS();

})();
