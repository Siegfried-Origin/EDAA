"""Comparison page for edaa-replay results (docs/replay.md).

    python report.py <replay folder>

Reads <replay folder>/<recording>/metrics.json (written by edaa-replay) and
optional <replay folder>/scenes.json ({"<recording>": "description"}) and
labels.json ({"<variant id>": "label"}, for --taa experiments), and
writes <replay folder>/index.html: a table of the measures over all
recordings, and per recording a split comparison of two variants (whole
frames, zoom), a burst of consecutive frames played side by side, the
flicker heat maps and the flicker per frame.
"""

import html
import json
import os
import sys

PAGE = r"""<!doctype html>
<html lang="de">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>EDAA Replay</title>
<style>
:root { --bg: #111418; --panel: #1a1f26; --line: #2c333d; --text: #e6e9ee; --dim: #9aa4b1; --accent: #f0a030; --good: #5ec27a; --bad: #e0605a; }
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--text); font: 15px/1.5 system-ui, "Segoe UI", sans-serif; }
main { max-width: 1680px; margin: 0 auto; padding: 24px 16px 64px; }
h1 { font-size: 26px; margin: 0 0 4px; }
h2 { font-size: 20px; margin: 40px 0 4px; border-top: 1px solid var(--line); padding-top: 24px; }
p.dim, .dim { color: var(--dim); }
table { border-collapse: collapse; margin: 12px 0; font-variant-numeric: tabular-nums; }
th, td { padding: 4px 10px; border-bottom: 1px solid var(--line); text-align: right; white-space: nowrap; }
th:first-child, td:first-child { text-align: left; }
th { color: var(--dim); font-weight: 600; }
td.best { color: var(--good); font-weight: 600; }
td.worst { color: var(--bad); }
.wrap { overflow-x: auto; }
.controls { display: flex; flex-wrap: wrap; gap: 8px 16px; align-items: center; margin: 12px 0; }
select, button { background: var(--panel); color: var(--text); border: 1px solid var(--line); border-radius: 6px; padding: 4px 8px; font: inherit; }
button.on { border-color: var(--accent); color: var(--accent); }
.split { position: relative; width: 100%; aspect-ratio: 16 / 9; background: #000; overflow: hidden; cursor: crosshair; user-select: none; }
.split img { position: absolute; inset: 0; width: 100%; height: 100%; image-rendering: auto; }
.split.zoom img { image-rendering: pixelated; }
.split .b { clip-path: inset(0 0 0 50%); }
.split .bar { position: absolute; top: 0; bottom: 0; width: 2px; background: var(--accent); left: 50%; pointer-events: none; }
.split .tag { position: absolute; top: 8px; padding: 2px 8px; background: rgba(0,0,0,.65); border-radius: 4px; font-size: 13px; pointer-events: none; }
.split .tag.l { left: 8px; } .split .tag.r { right: 8px; }
.pair { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
.pair figure { margin: 0; }
.pair figcaption { color: var(--dim); font-size: 13px; padding: 2px 0; }
.pair img, .pair canvas { width: 100%; display: block; background: #000; }
.burst canvas { image-rendering: pixelated; }
svg.chart { width: 100%; height: 180px; background: var(--panel); border-radius: 6px; }
@media (max-width: 760px) { .pair { grid-template-columns: 1fr; } }
</style>
</head>
<body>
<main>
<h1>EDAA Replay – Kantenglättung im Vergleich</h1>
<p class="dim">Aufgenommene Spielszenen, offline durch dieselben Shader und denselben DLSS-Code wie im Spiel gespielt.
Angezeigt mit Elites Belichtung des jeweiligen Bildes, aber mit einer einfachen Kurve (Reinhard, sRGB) statt Elites eigener: Helligkeit und Farben weichen etwas vom Spiel ab, der Vergleich der Varianten untereinander stimmt.</p>
<p class="dim"><b>Flackern</b>: mittlere Helligkeitsänderung gegenüber dem vorigen Bild, entlang der Bewegungsvektoren verschoben (in % des Bildbereichs, kleiner = ruhiger).
<b>Flackern stark</b>: dasselbe nur über die 5 % unruhigsten Pixel (Kanten, Lichter). <b>Schärfe</b>: mittlerer Kantengradient (größer = schärfer).
Gemessen ohne Rand, ohne verdeckte/aufgedeckte Stellen und ohne die ersten 8 Bilder. Fremde Schiffe haben keine Bewegungsvektoren: dort misst „Flackern“ auch Schlieren.</p>
<div id="summary"></div>
<div id="scenes"></div>
</main>
<script>
const DATA = __DATA__;

const pct = v => v < 0 ? '–' : (v * 100).toFixed(2);
const num = (v, d) => v < 0 ? '–' : v.toFixed(d);

function cell(v, all, lowerBetter, fmt) {
  const ok = all.filter(x => x >= 0);
  const best = lowerBetter ? Math.min(...ok) : Math.max(...ok);
  const worst = lowerBetter ? Math.max(...ok) : Math.min(...ok);
  const cls = v === best ? 'best' : v === worst ? 'worst' : '';
  return `<td class="${cls}">${fmt(v)}</td>`;
}

function metricsTable(rec) {
  const vs = rec.variants;
  const col = k => vs.map(v => v[k]);
  let h = '<div class="wrap"><table><tr><th>Variante</th><th>Flackern stark %</th><th>Flackern %</th><th>Schärfe</th><th>GPU ms</th></tr>';
  for (const v of vs) {
    h += `<tr><td>${v.label}</td>` + cell(v.flicker_hot, col('flicker_hot'), true, pct) + cell(v.flicker, col('flicker'), true, pct) +
      cell(v.sharpness, col('sharpness'), false, x => num(x * 100, 2)) + `<td>${v.gpu_ms > 0 ? v.gpu_ms.toFixed(2) : '–'}</td></tr>`;
  }
  return h + '</table></div>';
}

function summary() {
  // Mean over the recordings, relative to the input without anti-aliasing.
  const ids = [], labels = {};
  for (const r of DATA) for (const v of r.variants) if (!labels[v.id]) { ids.push(v.id); labels[v.id] = v.label; }
  let h = '<h2>Alle Szenen</h2><p class="dim">Werte relativ zum Eingang ohne Kantenglättung (= 100), gemittelt über alle Szenen. Flackern: kleiner ist besser, Schärfe: größer ist schärfer.</p>';
  h += '<div class="wrap"><table><tr><th>Variante</th><th>Flackern stark</th><th>Flackern</th><th>Schärfe</th><th>GPU ms</th></tr>';
  const rows = ids.map(id => {
    const acc = { hot: [], fl: [], sh: [], ms: [] };
    for (const r of DATA) {
      const raw = r.variants.find(v => v.id === 'raw'), v = r.variants.find(v => v.id === id);
      if (!raw || !v) continue;
      acc.hot.push(100 * v.flicker_hot / raw.flicker_hot);
      acc.fl.push(100 * v.flicker / raw.flicker);
      acc.sh.push(100 * v.sharpness / raw.sharpness);
      if (v.gpu_ms > 0) acc.ms.push(v.gpu_ms);
    }
    const m = a => a.length ? a.reduce((x, y) => x + y, 0) / a.length : -1;
    return { id, hot: m(acc.hot), fl: m(acc.fl), sh: m(acc.sh), ms: m(acc.ms) };
  });
  const col = k => rows.filter(r => r.id !== 'raw').map(r => r[k]);
  for (const r of rows) {
    const c = (v, k, lb) => r.id === 'raw' ? `<td>${num(v, 0)}</td>` : cell(v, col(k), lb, x => num(x, 0));
    h += `<tr><td>${labels[r.id]}</td>${c(r.hot, 'hot', true)}${c(r.fl, 'fl', true)}${c(r.sh, 'sh', false)}<td>${r.ms > 0 ? r.ms.toFixed(2) : '–'}</td></tr>`;
  }
  document.getElementById('summary').innerHTML = h + '</table></div>';
}

function options(rec, sel) {
  return rec.variants.map(v => `<option value="${v.id}"${v.id === sel ? ' selected' : ''}>${v.label}</option>`).join('');
}

function scene(rec, index) {
  const el = document.createElement('section');
  const has = id => rec.variants.some(v => v.id === id);
  const a0 = has('taa') ? 'taa' : rec.variants[0].id;
  const b0 = has('dlaa') ? 'dlaa' : rec.variants[rec.variants.length - 1].id;
  const label = id => rec.variants.find(v => v.id === id).label;
  el.innerHTML = `
    <h2>${rec.title}</h2>
    <p class="dim">${rec.recording} · ${rec.frames} Bilder · Ausschnitt ${rec.size[0]}×${rec.size[1]} aus der Bildmitte${rec.on_foot ? ' · zu Fuß' : ''} · Elite-Belichtung ${Math.min(...rec.exposure.filter(e => e > 0)).toFixed(2)}–${Math.max(...rec.exposure).toFixed(2)}</p>
    ${metricsTable(rec)}
    <div class="controls">
      <label>Links <select class="a">${options(rec, a0)}</select></label>
      <label>Rechts <select class="b">${options(rec, b0)}</select></label>
      <label>Bild <select class="pick">${rec.picks.map(p => `<option value="${p}">${p}</option>`).join('')}</select></label>
      <button class="zoomb">Zoom ×3</button>
      <span class="dim">Maus bewegen: Trennlinie · Zoom: Klick setzt die Mitte</span>
    </div>
    <div class="split"><img class="a"><img class="b"><div class="bar"></div><span class="tag l"></span><span class="tag r"></span></div>
    <h3>Bewegung (${rec.burst.count} aufeinanderfolgende Bilder ab ${rec.burst.start}, Ausschnitt ${rec.burst.rect[2]}×${rec.burst.rect[3]}, doppelt vergrößert)</h3>
    <div class="controls"><button class="play on">Pause</button>
      <label>Tempo <select class="fps"><option value="60">Echtzeit (~60 fps)</option><option value="10" selected>langsam (10 fps)</option><option value="3">sehr langsam (3 fps)</option></select></label>
      <span class="dim frame-no"></span></div>
    <div class="pair burst"><figure><figcaption class="ca"></figcaption><canvas class="ba"></canvas></figure><figure><figcaption class="cb"></figcaption><canvas class="bb"></canvas></figure></div>
    <h3>Flackern je Pixel über die ganze Aufnahme</h3>
    <p class="dim">Schwarz = ruhig, rot → gelb → weiß = stärker (weiß ab 8 % Helligkeitsänderung je Bild).</p>
    <div class="pair"><figure><figcaption class="ha-c"></figcaption><img class="ha"></figure><figure><figcaption class="hb-c"></figcaption><img class="hb"></figure></div>
    <h3>Flackern je Bild</h3>
    <svg class="chart" viewBox="0 0 1000 180" preserveAspectRatio="none"></svg>
    <p class="dim"><span style="color:#5aa9e6">■</span> links · <span style="color:#f0a030">■</span> rechts · Höhe bis ${'{max}'} %</p>`;
  document.getElementById('scenes').appendChild(el);

  const q = s => el.querySelector(s);
  const split = q('.split');
  let zoom = false, cx = 0.5, cy = 0.5, frame = 0, timer = null, playing = true;

  function update() {
    const a = q('select.a').value, b = q('select.b').value, p = q('.pick').value;
    q('img.a').src = `${rec.recording}/frames/${a}_${p}.png`;
    q('img.b').src = `${rec.recording}/frames/${b}_${p}.png`;
    q('.tag.l').textContent = label(a);
    q('.tag.r').textContent = label(b);
    q('.ca').textContent = label(a);
    q('.cb').textContent = label(b);
    q('.ha').src = `${rec.recording}/heat/${a}.png`;
    q('.hb').src = `${rec.recording}/heat/${b}.png`;
    q('.ha-c').textContent = label(a);
    q('.hb-c').textContent = label(b);
    for (const img of split.querySelectorAll('img')) {
      img.style.transformOrigin = `${cx * 100}% ${cy * 100}%`;
      img.style.transform = zoom ? 'scale(3)' : '';
    }
    split.classList.toggle('zoom', zoom);
    chart(a, b);
  }

  function chart(a, b) {
    const va = rec.variants.find(v => v.id === a), vb = rec.variants.find(v => v.id === b);
    const all = va.flicker_series.concat(vb.flicker_series).filter(x => x >= 0);
    const max = Math.max(...all, 1e-6) * 1.1;
    const line = (s, color) => {
      const pts = s.map((v, i) => v < 0 ? null : `${(i / (s.length - 1) * 1000).toFixed(1)},${(180 - v / max * 170).toFixed(1)}`).filter(Boolean).join(' ');
      return `<polyline fill="none" stroke="${color}" stroke-width="2" vector-effect="non-scaling-stroke" points="${pts}"/>`;
    };
    q('svg.chart').innerHTML = line(va.flicker_series, '#5aa9e6') + line(vb.flicker_series, '#f0a030');
    const legend = q('svg.chart').nextElementSibling;
    legend.innerHTML = legend.innerHTML.replace(/Höhe bis [^%]*%/, `Höhe bis ${(max * 100).toFixed(2)} %`);
  }

  // Burst frames are loaded once and drawn from memory: no request per frame.
  const cache = {};
  function frames(id) {
    if (!cache[id]) cache[id] = Array.from({ length: rec.burst.count }, (_, i) => {
      const img = new Image();
      img.src = `${rec.recording}/burst/${id}_${i}.png`;
      return img;
    });
    return cache[id];
  }

  function draw(canvas, img) {
    canvas.width = rec.burst.rect[2];
    canvas.height = rec.burst.rect[3];
    if (img.complete && img.naturalWidth > 0) canvas.getContext('2d').drawImage(img, 0, 0);
  }

  function step() {
    const fa = frames(q('select.a').value), fb = frames(q('select.b').value);
    const missing = fa.concat(fb).filter(i => i.complete && i.naturalWidth === 0).length;
    const loading = fa.concat(fb).filter(i => !i.complete).length;
    draw(q('.ba'), fa[frame]);
    draw(q('.bb'), fb[frame]);
    q('.frame-no').textContent = missing ? `${missing} Bilder fehlen (Server läuft?)` : loading ? `lädt … ${loading}` : `Bild ${rec.burst.start + frame}`;
    if (!loading) frame = (frame + 1) % rec.burst.count;
  }

  function restart() {
    clearInterval(timer);
    if (playing) timer = setInterval(step, 1000 / Number(q('.fps').value));
  }

  function preload() {
    frames(q('select.a').value);
    frames(q('select.b').value);
  }

  split.addEventListener('mousemove', e => {
    if (zoom) return;
    const r = split.getBoundingClientRect();
    const x = Math.min(Math.max((e.clientX - r.left) / r.width, 0), 1);
    q('img.b').style.clipPath = `inset(0 0 0 ${x * 100}%)`;
    q('.bar').style.left = `${x * 100}%`;
  });
  split.addEventListener('click', e => {
    const r = split.getBoundingClientRect();
    if (!zoom) { cx = (e.clientX - r.left) / r.width; cy = (e.clientY - r.top) / r.height; }
    zoom = !zoom;
    q('.zoomb').classList.toggle('on', zoom);
    update();
  });
  q('.zoomb').addEventListener('click', () => { zoom = !zoom; q('.zoomb').classList.toggle('on', zoom); update(); });
  for (const s of ['select.a', 'select.b', '.pick']) q(s).addEventListener('change', () => { update(); preload(); step(); });
  q('.fps').addEventListener('change', restart);
  q('.play').addEventListener('click', () => {
    playing = !playing;
    q('.play').textContent = playing ? 'Pause' : 'Abspielen';
    q('.play').classList.toggle('on', playing);
    if (!playing) step();
    restart();
  });
  // Larger burst images: 2x, pixelated.
  for (const img of el.querySelectorAll('.burst canvas')) img.style.maxWidth = `${rec.burst.rect[2] * 2}px`;
  update(); preload(); step(); restart();
}

summary();
DATA.forEach(scene);
</script>
</body>
</html>
"""


def main(folder):
    scenes = {}
    names_path = os.path.join(folder, 'scenes.json')
    if os.path.exists(names_path):
        scenes = json.load(open(names_path, encoding='utf-8'))

    # Optional readable names for --taa experiments: {"<variant id>": "label"}.
    labels = {}
    labels_path = os.path.join(folder, 'labels.json')
    if os.path.exists(labels_path):
        labels = json.load(open(labels_path, encoding='utf-8'))

    data = []
    for name in sorted(os.listdir(folder)):
        path = os.path.join(folder, name, 'metrics.json')
        if not os.path.exists(path):
            continue
        rec = json.load(open(path, encoding='utf-8'))
        rec['title'] = html.escape(scenes.get(name, name))
        for v in rec['variants']:
            v['label'] = html.escape(labels.get(v['id'], v['label']))
        data.append(rec)

    if not data:
        print(f'no metrics.json below {folder}')
        return 1

    page = PAGE.replace('__DATA__', json.dumps(data, ensure_ascii=False))
    out = os.path.join(folder, 'index.html')
    with open(out, 'w', encoding='utf-8') as f:
        f.write(page)
    print(f'{len(data)} recordings -> {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
