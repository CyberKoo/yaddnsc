#!/usr/bin/env python3
"""Integration test simulation server.

Handles DNS (UDP 53), mDNS (UDP 5353), DoT (TCP 853),
DoH (TCP 443), and a dummy HTTP API (TCP 8080) that
records update requests from yaddnsc's simple driver.
"""

import asyncio
import http.server
import json
import logging
import os
import select
import socket
import ssl
import struct
import threading
import time
from pathlib import Path

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
DNS_HOST = "0.0.0.0"
DNS_PORT = int(os.environ.get("SIM_DNS_PORT", 53))
MDNS_PORT = int(os.environ.get("SIM_MDNS_PORT", 5353))
DOT_PORT = int(os.environ.get("SIM_DOT_PORT", 853))
DOH_PORT = int(os.environ.get("SIM_DOH_PORT", 443))
API_PORT = int(os.environ.get("SIM_API_PORT", 8080))
MDNS_ADDR = "224.0.0.251"
CERT_DIR = Path(os.environ.get("SIM_CERT_DIR", "/tmp/sim-certs"))
CERT_FILE = CERT_DIR / "sim.crt"
KEY_FILE = CERT_DIR / "sim.key"

logging.basicConfig(
    level=logging.INFO,
    format="[%(levelname)s] %(name)s: %(message)s",
)
log = logging.getLogger("sim")

# ---------------------------------------------------------------------------
# Shared state
# ---------------------------------------------------------------------------
# DNS response table: hostname -> { record_type -> ip }
DNS_RESPONSES: dict[str, dict[str, str]] = {
    "yaddnsc.test": {"A": "198.51.100.1", "AAAA": "2001:db8::1"},
    "test.local": {"A": "198.51.100.4"},
    "ip": {"A": "198.51.100.3"},
}

update_requests: list[dict] = []
_update_lock = threading.Lock()

# ---------------------------------------------------------------------------
# DNS wire-format helpers  (RFC 1035)
# ---------------------------------------------------------------------------

def make_dns_header(ident: int, qr: int, rcode: int = 0,
                    qdcount: int = 1, ancount: int = 1) -> bytes:
    flags = (qr << 15) | (1 << 7)  # QR + RA
    flags |= rcode & 0x0F
    return struct.pack("!HHHHHH", ident, flags, qdcount, ancount, 0, 0)


def encode_name(name: str) -> bytes:
    """Encode a domain name into DNS wire format (without compression)."""
    out = b""
    for label in name.rstrip(".").split("."):
        out += bytes([len(label)]) + label.encode()
    out += b"\x00"
    return out


def decode_name(data: bytes, offset: int) -> tuple[str, int]:
    """Decode a domain name from wire format, handling compression pointers."""
    labels = []
    jumped = False
    while offset < len(data):
        length = data[offset]
        if length == 0:
            offset += 1
            break
        if length & 0xC0:  # Pointer
            ptr = ((length & 0x3F) << 8) | data[offset + 1]
            if not jumped:
                offset += 2
                jumped = True
            # recurse into pointer
            sub_name, _ = decode_name(data, ptr)
            labels.append(sub_name)
            break
        offset += 1
        labels.append(data[offset:offset + length].decode())
        offset += length
    return ".".join(labels), offset


def parse_dns_query(data: bytes) -> tuple[int, str, str]:
    """Parse a DNS query, return (id, qname, qtype_str)."""
    ident = struct.unpack("!H", data[:2])[0]
    qdcount = struct.unpack("!H", data[4:6])[0]
    if qdcount == 0:
        return ident, "", ""
    offset = 12  # skip header
    qname, offset = decode_name(data, offset)
    qtype = struct.unpack("!H", data[offset:offset + 2])[0]
    type_names = {1: "A", 28: "AAAA"}
    return ident, qname, type_names.get(qtype, f"TYPE{qtype}")


def build_dns_response(ident: int, qname: str, qtype_str: str,
                       rdata: str) -> bytes:
    """Build a DNS response with one answer record."""
    qtype = 1 if qtype_str == "A" else 28
    # Encode rdata
    if qtype_str == "A":
        parts = [int(x) for x in rdata.split(".")]
        rdata_bytes = bytes(parts)
        rdlength = 4
    else:
        # IPv6: simplify by using uncompressed form
        parts_list = rdata.split(":")
        raw = bytearray(16)
        if "::" in rdata:
            # Expand ::
            left, right = rdata.split("::")
            left_parts = left.split(":") if left else []
            right_parts = right.split(":") if right else []
            fill = 8 - len(left_parts) - len(right_parts)
            for i, p in enumerate(left_parts):
                raw[i * 2:(i + 1) * 2] = (int(p, 16) if p else 0).to_bytes(2, "big")
            for i, p in enumerate(right_parts):
                idx = fill + len(left_parts) + i
                raw[idx * 2:(idx + 1) * 2] = (int(p, 16) if p else 0).to_bytes(2, "big")
        else:
            for i, p in enumerate(parts_list):
                raw[i * 2:(i + 1) * 2] = (int(p, 16) if p else 0).to_bytes(2, "big")
        rdata_bytes = bytes(raw)
        rdlength = 16

    qname_wire = encode_name(qname)
    header = make_dns_header(ident, qr=1, ancount=1)

    # Question section (echo query)
    question = qname_wire + struct.pack("!HH", qtype, 1)

    # Answer section (compressed name pointer to question)
    answer = struct.pack("!H", 0xC00C)  # name pointer to offset 12
    answer += struct.pack("!HH", qtype, 1)  # TYPE, CLASS IN
    answer += struct.pack("!I", 60)  # TTL 60s
    answer += struct.pack("!H", rdlength)
    answer += rdata_bytes

    return header + question + answer


# ---------------------------------------------------------------------------
# DNS / mDNS (UDP)
# ---------------------------------------------------------------------------

def run_udp_dns_server(host: str, port: int, is_mdns: bool = False):
    """Thread-based UDP DNS responder for a single port."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    try:
        sock.bind((host, port))
    except OSError as e:
        log.error("DNS/%s bind to %s:%d failed: %s", "mDNS" if is_mdns else "UDP", host, port, e)
        sock.close()
        return
    if is_mdns:
        try:
            mreq = struct.pack("4sl", socket.inet_aton(MDNS_ADDR), socket.INADDR_ANY)
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        except OSError as e:
            log.error("mDNS join multicast group failed: %s", e)
            sock.close()
            return

    log.info("DNS/%s listening on %s:%d", "mDNS" if is_mdns else "UDP", host, port)

    while True:
        try:
            data, addr = sock.recvfrom(512)
            ident, qname, qtype_str = parse_dns_query(data)
            if not qname:
                continue
            log.debug("DNS query: %s %s from %s", qtype_str, qname, addr)

            resp_table = DNS_RESPONSES.get(qname, {})
            rdata = resp_table.get(qtype_str)
            if not rdata and qtype_str == "A":
                rdata = "198.51.100.1"

            if rdata:
                response = build_dns_response(ident, qname, qtype_str, rdata)
                sock.sendto(response, addr)
                log.debug("DNS response: %s -> %s", qname, rdata)
            else:
                header = make_dns_header(ident, qr=1, rcode=3, ancount=0)
                question = encode_name(qname) + struct.pack("!HH",
                    1 if qtype_str == "A" else 28, 1)
                sock.sendto(header + question, addr)
        except Exception as e:
            log.warning("DNS error: %s", e)


# ---------------------------------------------------------------------------
# DoT (TCP + TLS)
# ---------------------------------------------------------------------------

async def handle_dot_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    """Handle a single DoT connection."""
    try:
        data = await reader.readexactly(2)
        length = struct.unpack("!H", data)[0]
        query = await reader.readexactly(length)

        ident, qname, qtype_str = parse_dns_query(query)
        log.debug("DoT query: %s %s", qtype_str, qname)

        resp_table = DNS_RESPONSES.get(qname, {})
        rdata = resp_table.get(qtype_str, "198.51.100.1")
        response = build_dns_response(ident, qname, qtype_str, rdata)

        writer.write(struct.pack("!H", len(response)) + response)
        await writer.drain()
    except asyncio.IncompleteReadError:
        pass
    except Exception as e:
        log.warning("DoT error: %s", e)
    finally:
        writer.close()


async def tcp_dns_server(host: str, port: int, ssl_context: ssl.SSLContext | None = None):
    """Start TCP DNS server (for DoT when SSL context is provided)."""
    loop = asyncio.get_running_loop()
    server = await asyncio.start_server(
        handle_dot_client, host, port, ssl=ssl_context,
        reuse_address=True, reuse_port=True,
    )
    proto = "DoT" if ssl_context else "TCP/DNS"
    log.info("%s listening on %s:%d", proto, host, port)
    async with server:
        await server.serve_forever()


def tcp_dns_server_wrapper(host: str, port: int, ssl_context: ssl.SSLContext | None = None):
    """Thread wrapper for async TCP DNS server."""
    asyncio.run(tcp_dns_server(host, port, ssl_context))


# ---------------------------------------------------------------------------
# DoH (HTTP + TLS)
# ---------------------------------------------------------------------------

class DohHandler(http.server.BaseHTTPRequestHandler):
    """DNS-over-HTTPS handler (RFC 8484)."""

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        if length == 0:
            self.send_error(400)
            return
        body = self.rfile.read(length)
        ident, qname, qtype_str = parse_dns_query(body)
        log.debug("DoH POST: %s %s", qtype_str, qname)

        resp_table = DNS_RESPONSES.get(qname, {})
        rdata = resp_table.get(qtype_str, "198.51.100.1")
        response = build_dns_response(ident, qname, qtype_str, rdata)

        self.send_response(200)
        self.send_header("Content-Type", "application/dns-message")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)

    def do_GET(self):
        self.send_error(405)  # Method Not Allowed

    # Suppress default logging
    def log_message(self, format, *args):
        log.debug("DoH: %s", format % args)


def run_doh_server(host: str, port: int, ssl_context: ssl.SSLContext):
    """Run DoH server in a background thread."""
    server = http.server.HTTPServer((host, port), DohHandler)
    server.socket = ssl_context.wrap_socket(server.socket, server_side=True)
    log.info("DoH listening on %s:%d", host, port)
    server.serve_forever()


# ---------------------------------------------------------------------------
# Dummy API  (records update requests)
# ---------------------------------------------------------------------------

class ApiHandler(http.server.BaseHTTPRequestHandler):
    """Dummy API that records update requests from yaddnsc."""

    def do_GET(self):
        if self.path == "/health":
            self._json(200, {"status": "ok"})
        elif self.path == "/logs":
            with _update_lock:
                self._json(200, list(update_requests))
        elif self.path == "/myip":
            # Returns a fixed IP for HttpIpSource testing
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len("198.51.100.1")))
            self.end_headers()
            self.wfile.write(b"198.51.100.1")
        elif self.path == "/reset":
            with _update_lock:
                update_requests.clear()
            self._json(200, {"reset": True})
        elif self.path.startswith("/update"):
            self._record_request("GET")
            # Return success — simple driver expects 2xx + non-empty body
            self._json(200, {"success": True})
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path.startswith("/update"):
            body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            with _update_lock:
                update_requests.append({
                    "method": "POST",
                    "path": self.path,
                    "body": body.decode(),
                    "headers": dict(self.headers),
                    "time": time.time(),
                })
            self._json(200, {"success": True})
        else:
            self.send_error(404)

    def _record_request(self, method: str):
        with _update_lock:
            update_requests.append({
                "method": method,
                "path": self.path,
                "headers": dict(self.headers),
                "time": time.time(),
            })

    def _json(self, status: int, data: dict):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        log.debug("API: %s", format % args)


def run_api_server(host: str, port: int):
    """Run dummy API server in background thread."""
    server = http.server.HTTPServer((host, port), ApiHandler)
    log.info("Dummy API listening on %s:%d", host, port)
    server.serve_forever()


# ---------------------------------------------------------------------------
# TLS certificate generation
# ---------------------------------------------------------------------------

def ensure_certificates():
    """Generate self-signed cert if not present."""
    if CERT_FILE.exists() and KEY_FILE.exists():
        return
    CERT_DIR.mkdir(parents=True, exist_ok=True)
    log.info("Generating self-signed TLS certificate...")
    import subprocess
    subprocess.run([
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-keyout", str(KEY_FILE),
        "-out", str(CERT_FILE), "-days", "365", "-nodes",
        "-subj", "/CN=sim/O=yaddnsc/C=XX",
        "-addext", "subjectAltName=DNS:sim,DNS:localhost,IP:127.0.0.1",
    ], check=True, capture_output=True)
    log.info("Certificate generated: %s", CERT_FILE)


def create_ssl_context() -> ssl.SSLContext:
    """Create TLS server context with self-signed cert."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(str(CERT_FILE), str(KEY_FILE))
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

async def main():
    ensure_certificates()
    ssl_ctx = create_ssl_context()

    # Start background thread servers
    threads = [
        threading.Thread(target=run_api_server, args=(DNS_HOST, API_PORT), daemon=True),
        threading.Thread(target=run_doh_server, args=(DNS_HOST, DOH_PORT, ssl_ctx), daemon=True),
        threading.Thread(target=run_udp_dns_server, args=(DNS_HOST, DNS_PORT), daemon=True),
        threading.Thread(target=run_udp_dns_server, args=(DNS_HOST, MDNS_PORT, True), daemon=True),
        threading.Thread(target=tcp_dns_server_wrapper, args=(DNS_HOST, DOT_PORT, ssl_ctx), daemon=True),
    ]
    for t in threads:
        t.start()

    log.info("All servers started. Ready for test.")
    # Keep running
    while True:
        await asyncio.sleep(3600)


if __name__ == "__main__":
    asyncio.run(main())
