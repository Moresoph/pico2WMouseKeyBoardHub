import argparse
import copy
import http.server
import json
import pathlib
import subprocess
import tempfile
import threading

ROOT = pathlib.Path(__file__).resolve().parents[1]
ASSETS = ROOT / "esp32" / "main" / "web"


def fixture():
    inputs = [
        dict(slot=1, name="FILCO <script>alert(1)</script>", address="00:18:00:3D:38:89",
             transport="Classic", saved=True, bonded=True, connected=True, encrypted=True,
             hid=True, ready=True, paused=False, pending=False, roles=1, record_slot=1, reports=10, interval_ms=None),
        dict(slot=2, name="M720", address="F9:37:F0:2A:90:27",
             transport="BLE", saved=True, bonded=True, connected=True, encrypted=True,
             hid=True, ready=True, paused=False, pending=False, roles=2, record_slot=2, reports=20, interval_ms=7.5),
    ]
    computers = [
        dict(slot=i, saved=i <= 2, connected=i == 1, encrypted=i == 1, ready=i == 1,
             suspended=False, address="01:02:03:04:05:06" if i <= 2 else "",
             interval_ms=7.5 if i == 1 else None) for i in range(1, 8)
    ]
    return dict(schema=1, firmware="0.5.2-softap-status", uptime_ms=1000, ssid="PicoHub-test",
                ip="192.168.4.1", clients=1, uart_online=True, uart_packets=100,
                uart_overflows=0, inputs=inputs, pico_fresh=True, pico_age_ms=100,
                pico_firmware="0.5.2", selected_pc=1, switching=False, mouse_buffer=False,
                storage_fault=False, computers=computers, management_open=False, remaining_ms=0,
                token="",trial=False,scanning=False,operation="",auth=0,auto_enrollment=False,
                records=[dict(slot=i,saved=i<=2,role=i if i<=2 else 0,active=i<=2,order=i,address="AA:BB:CC:DD:EE:FF",
                              name="FILCO <script>alert(1)</script>" if i==1 else "Saved M720" if i==2 else "",
                              transport="Classic" if i==1 else "BLE") for i in range(1,6)],candidates=[])


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/api/status":
            self.server.reads += 1
            if self.server.mode == "http_error":
                self.send_error(503)
                return
            payload = copy.deepcopy(fixture())
            if self.server.mode == "stale":
                payload.update(pico_fresh=False, computers=None, selected_pc=None)
            elif self.server.mode == "malformed":
                payload["computers"] = []
            elif self.server.mode == "storage_fault":
                payload["storage_fault"] = True
            elif self.server.mode == "management":
                payload.update(management_open=True,remaining_ms=100000,token="TEST",candidates=[
                    dict(id=0,name="<script>unsafe</script>",address="01:02:03:04:05:06",transport="BLE",hid=True)])
            elif self.server.mode == "scan_waiting":
                payload.update(management_open=True,remaining_ms=90000,token="TEST",scanning=True,scan_waiting=True,
                               operation="扫描已排队：等待旧连接结束后自动开始")
            elif self.server.mode == "trial":
                payload.update(management_open=True,remaining_ms=119000,token="TEST",trial=True,trial_role=2,
                               trial_phase=2,trial_address="03:03:03:03:03:03",operation="正在连接选中的新设备")
            elif self.server.mode == "passkey":
                payload.update(management_open=True,remaining_ms=110000,token="TEST",trial=True,trial_role=2,
                               trial_phase=2,trial_address="03:03:03:03:03:03",auth=2,auth_slot=2)
            elif self.server.mode == "automatic":
                payload.update(management_open=True,remaining_ms=100000,token="TEST",auto_enrollment=True,
                               scanning=True,operation="自动录入已开启")
            elif self.server.mode == "missing_names":
                for entry in payload["records"]: entry["name"]=""
                for entry in payload["inputs"]: entry["name"]=""
            body = json.dumps(payload, ensure_ascii=False).encode()
            content_type = "application/json"
        elif path == "/test.js":
            body = b"""
const timer = setInterval(() => {
  const field = document.querySelector('#auth input');
  if (!field) return;
  clearInterval(timer);
  field.value = '456789';
  setTimeout(() => {
    const current = document.querySelector('#auth input');
    document.body.dataset.passkeyPreserved =
      current === field && current.value === '456789' ? 'yes' : 'no';
  }, 3500);
}, 100);
"""
            content_type = "application/javascript"
        else:
            names = {"/": ("index.html", "text/html"), "/app.js": ("app.js", "application/javascript"),
                     "/style.css": ("style.css", "text/css")}
            if path not in names:
                self.send_error(404)
                return
            name, content_type = names[path]
            body = (ASSETS / name).read_bytes()
            if path == "/" and self.server.mode == "passkey":
                body = body.replace(b"</head>", b'<script src="/test.js" defer></script></head>')
        self.send_response(200)
        self.send_header("Content-Type", content_type + "; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--browser", default=r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe")
    args = parser.parse_args()
    browser = pathlib.Path(args.browser)
    if not browser.is_file():
        raise RuntimeError("Chromium browser missing; pass --browser with an installed browser path")
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.mode, server.reads = "ready", 0
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        for mode in ("ready", "stale", "http_error", "malformed", "storage_fault", "management",
                     "scan_waiting", "trial", "passkey", "automatic", "missing_names"):
            server.mode, server.reads = mode, 0
            with tempfile.TemporaryDirectory(prefix="pico-web-browser-") as profile:
                result = subprocess.run([
                    str(browser), "--headless=new", "--disable-gpu", "--no-first-run",
                    "--disable-background-networking", "--disable-extensions",
                    "--user-data-dir=" + profile, "--virtual-time-budget=5500", "--dump-dom",
                    f"http://127.0.0.1:{server.server_port}/",
                ], capture_output=True, timeout=45)
                if result.returncode:
                    raise RuntimeError(result.stderr.decode(errors="replace"))
                dom = result.stdout.decode("utf-8")
            assert server.reads >= 1, mode + ": JavaScript did not fetch the status API"
            assert dom.count('class="card') == 9, mode + ": missing input/computer cards"
            pc_dom = dom.split('<section id="computers"', 1)[1].split("</section>", 1)[0]
            if mode in ("ready","management","scan_waiting","trial","passkey","automatic","missing_names"):
                assert 'class="card selected"' in dom and "HID 就绪" in dom
                if mode!="missing_names": assert "&lt;script&gt;alert(1)&lt;/script&gt;" in dom
                assert "<script>alert(1)</script>" not in dom
                assert "7.5 ms" in dom and "正在接收键鼠输入" in dom
                assert dom.count('class="record"') == (6 if mode=="management" else 5)
                if mode=="management":
                    assert "&lt;script&gt;unsafe&lt;/script&gt;" in dom and "<script>unsafe</script>" not in dom
                    assert 'id="scan-ble" disabled' not in dom
                else: assert 'id="scan-ble" disabled' in dom
                if mode=="scan_waiting": assert "扫描排队中，将自动开始" in dom
                if mode=="trial": assert "验证新设备（鼠标 03:03:03:03:03:03）" in dom
                if mode=="passkey":
                    assert 'data-passkey-preserved="yes"' in dom
                    assert server.reads >= 2
                if mode=="automatic":
                    assert "自动录入开启" in dom and "自动录入中：先点" in dom
                if mode=="missing_names": assert "鼠标 · 名称尚未获取" in dom
                if mode=="ready":
                    assert "Saved M720" in dom and "未授权：长按" in dom
                    assert dom.count(">删除此设备</button>")==7 and dom.count(">删除此电脑</button>")==7
                if mode=="management":
                    assert '>删除此设备</button>' in dom and '>设置名称</button>' in dom
            else:
                assert 'class="card selected"' not in pc_dom and "HID 就绪" not in pc_dom
                assert "状态未知" in dom
                if mode == "http_error":
                    assert "HTTP 503" in dom
                if mode == "malformed":
                    assert "不兼容" in dom
                if mode == "storage_fault":
                    assert "配对存储异常" in dom
            print("Browser UI passed:", mode)
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


if __name__ == "__main__":
    main()
