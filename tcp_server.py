# tcp_server.py (개선판)
import socket, re

HOST = "0.0.0.0"
PORT = 5000

ALLOWED_MACS = {"f4:65:0b:31:f1:58",}

def canonicalize_mac(s: str) -> str:
    hexonly = re.sub(r'[^0-9a-fA-F]', '', s).lower()
    if len(hexonly) == 12:
        return ":".join(hexonly[i:i+2] for i in range(0, 12, 2))
    return s.strip().lower()

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, PORT))
    s.listen(1)
    print(f"Listening on {HOST}:{PORT}")

    while True:
        conn, addr = s.accept()
        with conn:
            print("Client:", addr)
            # 첫 줄을 안전하게 1줄만 읽기
            mac_bytes = b""
            while b"\n" not in mac_bytes and len(mac_bytes) < 64:
                chunk = conn.recv(64)
                if not chunk:
                    break
                mac_bytes += chunk
            raw = mac_bytes.decode("utf-8", errors="ignore")
            mac_line = raw.split("\n", 1)[0].strip()   # 첫 줄만 사용
            mac_norm = canonicalize_mac(mac_line)
            print("RAW MAC:", repr(mac_line))
            print("MAC   ->", mac_norm)

            if mac_norm in ALLOWED_MACS or not ALLOWED_MACS:
                conn.sendall(b"OK\n")
                print("Auth OK")
            else:
                conn.sendall(b"DENY\n")
                print("Auth DENY")
                continue

            # 이후 데이터 수신
            try:
                while True:
                    data = conn.recv(1024)
                    if not data:
                        break
                    line = data.decode("utf-8", errors="ignore").strip()
                    if line:
                        print("RX:", line)
                        # conn.sendall(b"ACK\n")
            except Exception as e:
                print("Error:", e)
