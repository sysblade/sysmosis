var STATE_CLASSES = ['s0', 's1', 's2', 's3', 's4'];

function formatDuration(seconds) {
    var h = Math.floor(seconds / 3600);
    var m = Math.floor(seconds % 3600 / 60);
    var s = seconds % 60;
    if (h > 0) return h + 'h' + m + 'm';
    if (m > 0) return m + 'm' + s + 's';
    return s + 's';
}

function yesNo(value) {
    return value
        ? '<span class="ok">YES</span>'
        : '<span class="err">NO</span>';
}

function relayState(value) {
    return value
        ? '<span class="ok">ON</span>'
        : '<span class="lbl">OFF</span>';
}

function setText(id, text) {
    document.getElementById(id).textContent = text;
}

function setHTML(id, html) {
    document.getElementById(id).innerHTML = html;
}

function handleUnauthorized(response) {
    if (response.status === 401) {
        location.href = '/login';
        return true;
    }
    return false;
}

function updateStatus(d) {
    var badge = document.getElementById('badge');
    badge.textContent = d.state || '?';
    badge.className = 'badge ' + (STATE_CLASSES[d.state_id] || 's0');
    setText('ip-hdr', d.ip || '');

    if (d.lcd) {
        setText('lcd-display', d.lcd.join('\n'));
    }

    setHTML('sw',   yesNo(d.source_water));
    setHTML('fo',   yesNo(d.faucet_open));
    setText('tds',  d.tds_ppm);
    setHTML('lps',  d.lps ? '<span class="warn">LOW</span>'  : '<span class="ok">OK</span>');
    setHTML('hps',  d.hps ? '<span class="warn">HIGH</span>' : '<span class="ok">OK</span>');
    setHTML('leak', d.leak_detected ? '<span class="err">LEAK!</span>' : '<span class="ok">NONE</span>');

    setHTML('pump',  relayState(d.pump));
    setHTML('inlet', relayState(d.inlet_valve));
    setHTML('fv',    relayState(d.flush_valve));

    setText('upt',  formatDuration(d.uptime_s || 0));
    setText('prod', formatDuration(d.production_time_s || 0));
    setText('fcyc', d.flush_cycles_total || 0);
    setText('ptot', formatDuration(d.production_total_s || 0));

    var flushInfo = document.getElementById('flush-info');
    if (d.flush_remaining_s > 0) {
        flushInfo.style.display = '';
        setText('freason', d.flush_reason);
        setText('frem',    d.flush_remaining_s);
    } else {
        flushInfo.style.display = 'none';
    }

    document.getElementById('maint-controls').style.display = d.state_id === 4 ? 'block' : 'none';
}

function refresh() {
    fetch('/status')
        .then(function(r) {
            if (handleUnauthorized(r)) return null;
            return r.json();
        })
        .then(function(d) {
            if (!d) return;
            updateStatus(d);
        })
        .catch(function() {});
}

function postAction(action) {
    fetch('/control', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'action=' + action,
    })
    .then(function(r) {
        if (handleUnauthorized(r)) return null;
        return r.json();
    })
    .then(function(d) {
        if (!d) return;
        if (!d.ok) alert(d.message);
        else refresh();
    })
    .catch(function() {});
}

function confirmReset() {
    if (confirm('Reset the device?')) postAction('reset');
}

document.addEventListener('keydown', function(e) {
    if (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA') return;
    if (e.key === 'm' || e.key === 'M') postAction('maintenance_toggle');
    else if (e.key === 'f' || e.key === 'F') postAction('flush_start');
    else if (e.key === 'r' || e.key === 'R') confirmReset();
});

refresh();
setInterval(refresh, 2000);
