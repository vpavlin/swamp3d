#!/usr/bin/env python3
"""A fake Bambu Lab printer on localhost, for testing swamp_bambu.hpp without a printer:
implicit-TLS FTPS (USER/PASS/TYPE/PBSZ/PROT/PASV/STOR/QUIT), MQTT 3.1.1 over TLS (CONNECT,
SUBSCRIBE, PUBLISH; answers pushall and project_file like the printer does) and SSDP NOTIFYs.

  bambu_mock.py <workdir> <ftps_port> <mqtt_port> <ssdp_port> <serial> <access_code>

Everything it receives is logged as JSON lines to <workdir>/mock.log; uploads land in <workdir>/sd/.
"""
import json, os, socket, ssl, struct, subprocess, sys, threading, time

work, ftps_port, mqtt_port, ssdp_port, SERIAL, CODE = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5], sys.argv[6]
os.makedirs(work + "/sd", exist_ok=True)
crt, key = work + "/mock.crt", work + "/mock.key"
if not os.path.exists(crt):
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", crt, "-days", "2", "-subj", "/CN=bambu-mock"],
                   check=True, capture_output=True)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(crt, key)
lock = threading.Lock()
def log(**kw):
    with lock, open(work + "/mock.log", "a") as f: f.write(json.dumps(kw) + "\n")

state = {"gcode_state": "IDLE", "mc_percent": 0, "subtask_name": ""}

# ---- FTPS ---------------------------------------------------------------------------------------
def ftps_session(raw):
    c = ctx.wrap_socket(raw, server_side=True)
    f = c.makefile("rb")
    def send(s): c.sendall((s + "\r\n").encode())
    send("220 Bambu FTP ready")
    user_ok = authed = False
    data_srv = None
    while True:
        line = f.readline().decode().strip()
        if not line: break
        cmd, _, arg = line.partition(" ")
        cmd = cmd.upper()
        if cmd == "USER": user_ok = arg == "bblp"; send("331 Password required")
        elif cmd == "PASS":
            authed = user_ok and arg == CODE
            log(ftp="login", ok=authed)
            send("230 Logged in" if authed else "530 Login incorrect")
        elif not authed: send("530 Not logged in")
        elif cmd in ("TYPE", "PBSZ", "PROT"): send("200 OK")
        elif cmd == "PASV":
            data_srv = socket.socket(); data_srv.bind(("127.0.0.1", 0)); data_srv.listen(1)
            p = data_srv.getsockname()[1]
            send("227 Entering Passive Mode (127,0,0,1,%d,%d)" % (p // 256, p % 256))
        elif cmd == "STOR":
            name = os.path.basename(arg)
            d, _ = data_srv.accept()
            send("150 Opening data connection")
            dc = ctx.wrap_socket(d, server_side=True)
            n = 0
            with open(work + "/sd/" + name, "wb") as out:
                while True:
                    try: b = dc.recv(65536)
                    except (ConnectionResetError, ssl.SSLError) as e: log(ftp="data-error", error=str(e)); break
                    if not b: break
                    out.write(b); n += len(b)
            try: dc.unwrap()
            except Exception: pass
            dc.close(); data_srv.close()
            log(ftp="stor", name=name, bytes=n)
            send("226 Transfer complete")
        elif cmd == "QUIT": send("221 Bye"); break
        else: send("502 Not implemented")
    c.close()

# ---- MQTT ---------------------------------------------------------------------------------------
def mqtt_read(c):
    h = c.recv(1)
    if not h: return None, None
    mult, n = 1, 0
    while True:
        b = c.recv(1)[0]
        n += (b & 127) * mult; mult *= 128
        if not b & 128: break
    body = b""
    while len(body) < n:
        chunk = c.recv(n - len(body))
        if not chunk: return None, None
        body += chunk
    return h[0] >> 4, body

def mqtt_len(n):
    out = b""
    while True:
        b = n % 128; n //= 128
        out += bytes([b | (128 if n else 0)])
        if not n: return out

def mqtt_publish(c, topic, payload):
    t = topic.encode(); body = struct.pack(">H", len(t)) + t + json.dumps(payload).encode()
    c.sendall(bytes([0x30]) + mqtt_len(len(body)) + body)

def mqtt_session(raw):
    c = ctx.wrap_socket(raw, server_side=True)
    typ, body = mqtt_read(c)
    if typ != 1: c.close(); return
    # CONNECT: skip protocol name/level/flags/keepalive, then client id, user, pass
    i = 2 + struct.unpack(">H", body[0:2])[0] + 4
    def s(i):
        L = struct.unpack(">H", body[i:i+2])[0]; return body[i+2:i+2+L].decode(), i + 2 + L
    _, i = s(i); user, i = s(i); pw, i = s(i)
    ok = user == "bblp" and pw == CODE
    log(mqtt="connect", ok=ok)
    c.sendall(bytes([0x20, 2, 0, 0 if ok else 5]))
    if not ok: c.close(); return
    report = "device/%s/report" % SERIAL
    while True:
        typ, body = mqtt_read(c)
        if typ is None or typ == 14: break
        if typ == 8:   # SUBSCRIBE
            pid = body[0:2]; L = struct.unpack(">H", body[2:4])[0]; topic = body[4:4+L].decode()
            log(mqtt="subscribe", topic=topic)
            c.sendall(bytes([0x90, 3]) + pid + bytes([0 if topic == report else 0x80]))
        elif typ == 3:   # PUBLISH
            L = struct.unpack(">H", body[0:2])[0]; topic = body[2:2+L].decode(); msg = json.loads(body[2+L:])
            log(mqtt="publish", topic=topic, msg=msg)
            if topic != "device/%s/request" % SERIAL: continue
            if "pushing" in msg:
                mqtt_publish(c, report, {"print": dict(state, command="push_status", nozzle_temper=27.5, bed_temper=24.0, mc_remaining_time=0)})
            elif msg.get("print", {}).get("command") == "project_file":
                p = msg["print"]
                exists = os.path.exists(work + "/sd/" + p.get("file", "")) and p.get("url", "").endswith(p.get("file", "#"))
                mqtt_publish(c, report, {"print": {"command": "project_file", "sequence_id": p.get("sequence_id"), "result": "success" if exists else "FAIL",
                                                   "reason": "" if exists else "file not found"}})
                if exists:
                    state.update(gcode_state="PREPARE", subtask_name=p.get("subtask_name", ""))
                    mqtt_publish(c, report, {"print": dict(state, command="push_status")})
    c.close()

def serve(port, handler):
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port)); srv.listen(5)
    while True:
        raw, _ = srv.accept()
        threading.Thread(target=handler, args=(raw,), daemon=True).start()

def ssdp():
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    msg = ("NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1990\r\nServer: UPnP/1.0\r\nLocation: 127.0.0.1\r\nNT: urn:bambulab-com:device:3dprinter:1\r\n"
           "USN: %s\r\nCache-Control: max-age=1800\r\nDevModel.bambu.com: N2S\r\nDevName.bambu.com: Mock A1\r\nDevSignal.bambu.com: -40\r\n"
           "DevConnect.bambu.com: lan\r\nDevBind.bambu.com: free\r\n\r\n" % SERIAL).encode()
    while True:
        u.sendto(msg, ("127.0.0.1", ssdp_port)); time.sleep(0.5)

threading.Thread(target=serve, args=(ftps_port, ftps_session), daemon=True).start()
threading.Thread(target=serve, args=(mqtt_port, mqtt_session), daemon=True).start()
threading.Thread(target=ssdp, daemon=True).start()
open(work + "/ready", "w").write("ok")
while True: time.sleep(1)
