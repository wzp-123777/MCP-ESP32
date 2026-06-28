#!/usr/bin/env python3
from __future__ import annotations

import selectors
import socket
import ssl
import sys
import threading


LISTEN_HOST = "0.0.0.0"
LISTEN_PORT = 8080
UPSTREAM_HOST = "127.0.0.1"
UPSTREAM_PORT = 18080
NAPCAT_HOST = "127.0.0.1"
NAPCAT_PORT = 6099
CERT_FILE = "/opt/mcp-robot/https-proxy.crt"
KEY_FILE = "/opt/mcp-robot/https-proxy.key"
BUFFER_SIZE = 65536
MCP_PREFIXES = (
    "/esp32_ws",
    "/healthz",
    "/mcp",
    "/ws",
)
MCP_API_PREFIXES = ()


def _log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def _route_for_initial_bytes(initial: bytes) -> tuple[str, int]:
    try:
        line = initial.split(b"\r\n", 1)[0].decode("ascii", errors="ignore")
        _method, target, *_rest = line.split()
    except ValueError:
        return UPSTREAM_HOST, UPSTREAM_PORT
    path = target.split("?", 1)[0]
    if path == "/":
        return NAPCAT_HOST, NAPCAT_PORT
    if path == "/webui" or path.startswith("/webui/"):
        return NAPCAT_HOST, NAPCAT_PORT
    if any(path == prefix or path.startswith(prefix + "/") for prefix in MCP_API_PREFIXES):
        return UPSTREAM_HOST, UPSTREAM_PORT
    if any(path == prefix or path.startswith(prefix.rstrip("/") + "/") for prefix in MCP_PREFIXES):
        return UPSTREAM_HOST, UPSTREAM_PORT
    return NAPCAT_HOST, NAPCAT_PORT


def _pipe_plain(client: socket.socket, upstream_host: str = UPSTREAM_HOST, upstream_port: int = UPSTREAM_PORT) -> None:
    upstream = socket.create_connection((upstream_host, upstream_port), timeout=15)
    client.settimeout(None)
    upstream.settimeout(None)
    # Keep sockets in blocking mode. Non-blocking sendall can raise after a
    # partial write on slower public connections, truncating large JS/CSS files.
    selector = selectors.DefaultSelector()
    selector.register(client, selectors.EVENT_READ, upstream)
    selector.register(upstream, selectors.EVENT_READ, client)
    try:
        while True:
            events = selector.select(timeout=300)
            if not events:
                continue
            for key, _ in events:
                source: socket.socket = key.fileobj
                target: socket.socket = key.data
                try:
                    data = source.recv(BUFFER_SIZE)
                except OSError:
                    return
                if not data:
                    return
                try:
                    target.sendall(data)
                except OSError:
                    return
    finally:
        selector.close()
        upstream.close()
        client.close()


def _copy_blocking(source: socket.socket, target: socket.socket) -> None:
    try:
        while True:
            data = source.recv(BUFFER_SIZE)
            if not data:
                break
            target.sendall(data)
    except OSError:
        pass
    finally:
        for sock in (source, target):
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def _handle_https(client: socket.socket, context: ssl.SSLContext) -> None:
    tls: ssl.SSLSocket | None = None
    upstream: socket.socket | None = None
    try:
        tls = context.wrap_socket(client, server_side=True)
        upstream = socket.create_connection((UPSTREAM_HOST, UPSTREAM_PORT), timeout=15)
        a = threading.Thread(target=_copy_blocking, args=(tls, upstream), daemon=True)
        b = threading.Thread(target=_copy_blocking, args=(upstream, tls), daemon=True)
        a.start()
        b.start()
        a.join()
        b.join()
    except Exception as exc:
        _log(f"https request failed: {exc}")
    finally:
        for sock in (upstream, tls, client):
            if sock is not None:
                try:
                    sock.close()
                except Exception:
                    pass


def _serve() -> None:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(CERT_FILE, KEY_FILE)
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((LISTEN_HOST, LISTEN_PORT))
    listener.listen(128)
    _log(f"mixed HTTP/HTTPS proxy listening on {LISTEN_HOST}:{LISTEN_PORT}")
    while True:
        client, addr = listener.accept()
        try:
            client.settimeout(5)
            first = client.recv(BUFFER_SIZE, socket.MSG_PEEK)
            client.settimeout(None)
            target = _handle_https if first and first[0] == 0x16 else _pipe_plain
            if target is _handle_https:
                thread = threading.Thread(target=target, args=(client, context), daemon=True)
            else:
                upstream_host, upstream_port = _route_for_initial_bytes(first or b"")
                thread = threading.Thread(target=target, args=(client, upstream_host, upstream_port), daemon=True)
            thread.start()
        except Exception as exc:
            _log(f"accept failed from {addr}: {exc}")
            client.close()


if __name__ == "__main__":
    _serve()
