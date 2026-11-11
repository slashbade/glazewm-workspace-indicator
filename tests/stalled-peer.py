"""Exercise the production WinHTTP client against a peer that never replies.
Run after scripts/build.ps1 -Test: python tests/stalled-peer.py
"""
import base64
import hashlib
from pathlib import Path
import socket
import subprocess
import threading


def serve(listener):
    while True:
        try:
            peer, _ = listener.accept()
        except OSError:
            return
        with peer:
            peer.settimeout(8)
            try:
                request = b""
                while b"\r\n\r\n" not in request:
                    part = peer.recv(4096)
                    if not part:
                        break
                    request += part
                headers = dict(line.split(b":", 1) for line in request.split(b"\r\n")[1:] if b":" in line)
                key = next(value.strip() for name, value in headers.items() if name.lower() == b"sec-websocket-key")
                accept = base64.b64encode(hashlib.sha1(key + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
                peer.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
                while peer.recv(4096):
                    pass  # Deliberately never send a query response.
            except (OSError, StopIteration):
                pass


with socket.socket() as listener:
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    threading.Thread(target=serve, args=(listener,), daemon=True).start()
    result = subprocess.run(
        [str(Path(__file__).resolve().parents[1] / "build" / "tests.exe"), "--stall-port", str(listener.getsockname()[1])],
        timeout=12,
    )
    raise SystemExit(result.returncode)
