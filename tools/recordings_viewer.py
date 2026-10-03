#!/usr/bin/env python3
"""Recordings viewer for P0 note-taker devices.

Smaarthi's API has no "list conversations" query yet, so the viewer builds its list
from the device's own upload log:
  * EVENTS.LOG from the device SD card (LOG/EVENTS.LOG, plus EVENTS.OLD), and/or
  * live, from the device's USB serial log (--serial).
Each upload event carries the conversation id and the media URL, so the page can
play every recording in the browser.

NOTE: playback works only because assets.smaarthi.com currently serves audio
without authentication (a P0 security gap). Once the backend makes storage private,
this viewer needs a signed-URL endpoint from the backend.

Usage:
  python3 tools/recordings_viewer.py --events /Volumes/NO\\ NAME/LOG/EVENTS.LOG
  python3 tools/recordings_viewer.py --serial /dev/cu.usbmodem1101   # needs pyserial
Then open http://localhost:8080
"""
import argparse, html, json, os, re, threading, time
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

INDEX = os.path.join(os.path.dirname(os.path.abspath(__file__)), "recordings.json")
UPLOADED = re.compile(r"seq=(\d+) conv=(\S+) url=(\S*)")
NAMED = re.compile(r"seq=(\d+) name=(\S+)")
NAME_PARTS = re.compile(r"^(nt-[0-9a-f]{12})_([^_]*)_([^_]*)_(\d{8})_(.+)\.ogg$")

lock = threading.Lock()
recordings = {}        # conv id -> record
pending_names = {}     # (source, seq) -> file name, until the matching "uploaded" arrives


def load_index():
    if os.path.exists(INDEX):
        with open(INDEX) as f:
            for r in json.load(f):
                recordings[r["conv"]] = r


def save_index():
    tmp = INDEX + ".tmp"
    with open(tmp, "w") as f:
        json.dump(sorted(recordings.values(), key=lambda r: r.get("seen", "")), f, indent=1)
    os.replace(tmp, INDEX)


def describe(name):
    m = NAME_PARTS.match(name or "")
    if not m:
        return {}
    dev, store, sales, seq, when = m.groups()
    if re.match(r"^\d{8}T\d{6}Z$", when):
        when = f"{when[0:4]}-{when[4:6]}-{when[6:8]} {when[9:11]}:{when[11:13]}:{when[13:15]} UTC"
    else:
        when = f"clock not set ({when})"
    return {"device": dev, "store": store, "salesperson": sales, "start": when}


def ingest(source, event, detail, t=""):
    """Feed one device event. Returns True if the index changed."""
    if event == "uploaded_name":
        m = NAMED.search(detail)
        if m:
            seq, name = m.groups()
            with lock:
                pending_names[(source, seq)] = name
                for r in recordings.values():          # the name can arrive after the upload line
                    if r["source"] == source and r["seq"] == seq and not r.get("name"):
                        r["name"] = name
                        r.update(describe(name))
                        return True
        return False
    if event != "uploaded":
        return False
    m = UPLOADED.search(detail)
    if not m:
        return False
    seq, conv, url = m.groups()
    with lock:
        if conv in recordings:
            return False
        name = pending_names.pop((source, seq), "")
        r = {"conv": conv, "url": url, "seq": seq, "source": source, "name": name,
             "seen": t or time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
        r.update(describe(name))
        recordings[conv] = r
    return True


def ingest_events_file(path):
    changed = False
    src = os.path.abspath(path)
    with open(path, errors="replace") as f:
        for line in f:
            try:
                e = json.loads(line)
            except ValueError:
                continue
            changed |= ingest(src, e.get("ev", ""), e.get("detail", ""), e.get("t", ""))
    return changed


def follow_serial(port):
    import serial                                  # pyserial: in the ESP-IDF Python env
    rx = re.compile(r"event: (uploaded_name|uploaded) (.*)$")
    while True:
        try:
            with serial.Serial(port, 115200, timeout=1) as s:
                buf = b""
                while True:
                    buf += s.read(4096)
                    *lines, buf = buf.split(b"\n")
                    for raw in lines:
                        m = rx.search(raw.decode("utf-8", "replace").strip())
                        if m and ingest("serial:" + port, m.group(1), m.group(2)):
                            with lock:
                                save_index()
        except Exception as ex:                    # port vanishes on device reset: retry
            print("serial:", ex)
            time.sleep(2)


PAGE = """<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>
<title>Note Taker recordings</title>
<style>
:root{--bg:#f6f5f0;--card:#fffdf8;--ink:#16262b;--muted:#5e686e;--line:#dcdad2;--accent:#1d6b63}
@media (prefers-color-scheme:dark){:root{--bg:#121b1e;--card:#1b272b;--ink:#e8eeec;--muted:#9fb2ae;--line:#2c3a3e;--accent:#5fc2b6}}
body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.45 -apple-system,system-ui,sans-serif}
main{max-width:1100px;margin:0 auto;padding:24px 16px}
h1{font-size:24px;margin:0 0 4px}.sub{color:var(--muted);margin:0 0 18px}
.bar{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:14px}
input,select{padding:8px;border:1px solid var(--line);border-radius:6px;background:var(--card);color:var(--ink)}
.rec{display:grid;grid-template-columns:1fr auto;gap:6px 16px;background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px;margin-bottom:10px}
.meta{color:var(--muted);font-size:13px}.t{font-weight:600}
audio{width:340px;max-width:100%}.empty{color:var(--muted);padding:40px 0;text-align:center}
.warn{font-size:13px;color:var(--muted);border-left:3px solid #b04a12;padding-left:10px;margin:0 0 16px}
@media (max-width:640px){.rec{grid-template-columns:1fr}audio{width:100%}}
</style>
<main><h1>Recordings</h1><p class=sub id=count></p>
<p class=warn>Playback uses public asset links (P0 only). Don't share this page outside the team.</p>
<div class=bar><input id=q placeholder="Filter: device, store, salesperson"><select id=dev><option value="">All devices</option></select></div>
<div id=list></div></main>
<script>
let all=[];
function esc(s){return String(s||'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function render(){
 const q=document.getElementById('q').value.toLowerCase(),d=document.getElementById('dev').value;
 const rows=all.filter(r=>(!d||r.device===d)&&(!q||[r.device,r.store,r.salesperson,r.name].join(' ').toLowerCase().includes(q)));
 document.getElementById('count').textContent=rows.length+' of '+all.length+' recordings, newest first';
 document.getElementById('list').innerHTML=rows.length?rows.map(r=>`<div class=rec><div><div class=t>${esc(r.store||'?')} · ${esc(r.salesperson||'?')} · #${esc(r.seq)}</div>
 <div class=meta>${esc(r.start||'start unknown')} · ${esc(r.device||r.source)} · conversation ${esc(r.conv)}</div></div>
 <audio controls preload=none src="${esc(r.url)}"></audio></div>`).join(''):'<div class=empty>No uploads yet. They appear here as devices upload.</div>';
}
async function load(){
 const keep=[...document.querySelectorAll('audio')].some(a=>!a.paused);if(keep)return;   // don't reset while playing
 all=(await (await fetch('/api/recordings')).json()).reverse();
 const sel=document.getElementById('dev'),cur=sel.value,devs=[...new Set(all.map(r=>r.device).filter(Boolean))];
 sel.innerHTML='<option value="">All devices</option>'+devs.map(x=>`<option>${esc(x)}</option>`).join('');sel.value=cur;
 render();
}
document.getElementById('q').oninput=render;document.getElementById('dev').onchange=render;
load();setInterval(load,15000);
</script>"""


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/api/recordings":
            with lock:
                body = json.dumps(sorted(recordings.values(), key=lambda r: (r.get("start", ""), r["seen"]))).encode()
            ctype = "application/json"
        elif self.path in ("/", "/index.html"):
            body, ctype = PAGE.encode(), "text/html; charset=utf-8"
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--events", action="append", default=[], help="EVENTS.LOG file(s) from a device SD card")
    ap.add_argument("--serial", help="follow a device's USB serial log live, e.g. /dev/cu.usbmodem1101")
    ap.add_argument("--port", type=int, default=8080)
    a = ap.parse_args()

    load_index()
    for p in a.events:
        if ingest_events_file(p):
            print("loaded", p)
    save_index()
    if a.serial:
        threading.Thread(target=follow_serial, args=(a.serial,), daemon=True).start()
    print(f"{len(recordings)} recordings. Open http://localhost:{a.port}")
    ThreadingHTTPServer(("127.0.0.1", a.port), Handler).serve_forever()   # local only


if __name__ == "__main__":
    main()
