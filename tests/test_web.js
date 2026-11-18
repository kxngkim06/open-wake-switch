const { JSDOM } = require('jsdom');
const fs = require('fs');
let html = fs.readFileSync('../webapp/index.html', 'utf8').replace(/<script src=[^>]+><\/script>/, '');
const published = [];
let handlers = {};
const fakeMqtt = { connect: (url, opts) => {
  published.push({ url, user: opts.username });
  const c = { on: (ev, fn) => { handlers[ev] = fn; return c; }, subscribe: () => {}, end: () => {},
              publish: (topic, msg, o) => published.push({ topic, msg, retain: o.retain }) };
  return c; } };
const dom = new JSDOM(html, { runScripts: 'dangerously', url: 'https://example.netlify.app/',
  beforeParse(w) { w.mqtt = fakeMqtt;
    w.localStorage.setItem('ows.creds', JSON.stringify({ host: 'y6218111.ala.us-east-1.emqxsl.com', user: 'phone', pass: 'x' })); } });
const w = dom.window, $ = (id) => w.document.getElementById(id);
let fails = 0; const ok = (c, m) => { console.log((c ? 'ok  : ' : 'FAIL: ') + m); if (!c) fails++; };
const send = (t, o) => handlers.message(t, Buffer.from(typeof o === 'string' ? o : JSON.stringify(o)));

ok(published[0].url === 'wss://y6218111.ala.us-east-1.emqxsl.com:8084/mqtt', 'connects to wss :8084/mqtt with saved creds');
handlers.connect();
send('ows/online', '1');
ok($('connText').textContent === '기기 온라인', 'online pill');

const now = new Date(); const pad = (n) => String(n).padStart(2, '0');
const tomorrow = new Date(now.getTime() + 86400000);
const ds = (d) => `${d.getFullYear()}-${pad(d.getMonth()+1)}-${pad(d.getDate())}`;
const cfg = { enabled: true, days: Array.from({length:7}, (_, i) => ({ on: i>0 && i<6, t: '07:30' })),
  autoOffMin: 0, graceMin: 10, override: null, oneShots: [] };
send('ows/config', cfg);
send('ows/state', { fw: '1.0.0', timeOk: true, last: 'ON', lastSrc: 'alarm', lastAt: ds(now)+'T07:30', rssi: -60,
  next: { at: ds(tomorrow)+'T07:30', kind: 'weekly' }, nextWeekly: { date: ds(tomorrow), t: '07:30' } });
ok($('nextBig').textContent === '내일 07:30', 'next alarm shows "내일 07:30" -> ' + $('nextBig').textContent);
ok($('ovInfo').textContent.includes('내일 07:30 (기본)'), 'override card shows default next weekly');
ok($('days').children.length === 7 && $('days').children[0].textContent.startsWith('월'), 'weekly editor Mon-first, 7 rows');

// "this time only" -> 06:45
$('ovTime').value = '06:45'; $('btnOvSet').click();
let p = published.at(-1); let body = JSON.parse(p.msg);
ok(p.topic === 'ows/config/set' && p.retain && body.override.date === ds(tomorrow) && body.override.t === '06:45', 'override publish');

// nap +30
$('napChips').children[2].click();
p = published.at(-1); body = JSON.parse(p.msg);
const exp = new Date(Date.now() + 30*60000);
ok(body.oneShots.length === 1 && body.oneShots[0].endsWith(`${pad(exp.getHours())}:${pad(exp.getMinutes())}`), 'nap +30분 one-shot ' + body.oneShots[0]);

// edit weekly: Tue time + save
const tue = $('days').children[1]; const ti = tue.querySelector('input[type=time]');
ti.value = '08:15'; ti.dispatchEvent(new w.Event('change'));
ok($('savebar').classList.contains('show'), 'save bar appears when edited');
// meanwhile device echoes a quick edit (override) -> draft must keep Tue edit
send('ows/config', { ...cfg, override: { date: ds(tomorrow), t: '06:45' } });
ok($('days').children[1].querySelector('input[type=time]').value === '08:15', 'unsaved weekly edit survives incoming config');
$('btnSave').click();
p = published.at(-1); body = JSON.parse(p.msg);
ok(body.days[2].t === '08:15' && body.override && body.override.t === '06:45', 'save sends Tue 08:15 and keeps override');
send('ows/config', body);
ok(!$('savebar').classList.contains('show'), 'save bar hides after device confirms');

// manual ON blocked when offline
const before = published.length;
send('ows/online', '0'); $('btnOn').click();
ok(published.length === before, 'ON not sent while device offline');
send('ows/online', '1'); $('btnOn').click();
ok(published.at(-1).topic === 'ows/cmd' && published.at(-1).msg === 'ON' && !published.at(-1).retain, 'ON sent (not retained)');

console.log(fails ? '\nSOME WEB TESTS FAILED' : '\nALL WEB TESTS PASSED');
process.exit(fails ? 1 : 0);
