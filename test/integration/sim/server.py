#!/usr/bin/env python3
"""Integration test simulation server — fully async.

Handles DNS (UDP 53), mDNS (UDP 5353), DoT (TCP 853),
DoH (TCP 443), and a dummy HTTP API (TCP 8080) that
records update requests from yaddnsc's simple driver.

All I/O runs in a single asyncio event loop — no threads.
"""

import asyncio
import json
import logging
import os
import socket
import ssl
import struct
import time
from pathlib import Path

from aiohttp import web

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
    "http.yaddnsc.test": {"A": "198.51.100.2"},
    "iface.yaddnsc.test": {"A": "198.51.100.1"},
    "test.local": {"A": "198.51.100.4"},
    "ip": {"A": "198.51.100.3"},
}

update_requests: list[dict] = []
_update_lock = asyncio.Lock()

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


def resolve(qname: str, qtype_str: str) -> str | None:
    """Look up a DNS name, returning rdata or None for NXDOMAIN."""
    resp_table = DNS_RESPONSES.get(qname, {})
    rdata = resp_table.get(qtype_str)
    if not rdata and qtype_str == "A":
        rdata = "198.51.100.1"
    return rdata


# ---------------------------------------------------------------------------
# DNS / mDNS (UDP) — asyncio DatagramProtocol
# ---------------------------------------------------------------------------


class DNSProtocol(asyncio.DatagramProtocol):
    """Asynchronous UDP DNS responder (also handles mDNS)."""

    def __init__(self, is_mdns: bool = False) -> None:
        self.is_mdns = is_mdns
        self.transport: asyncio.DatagramTransport | None = None

    def connection_made(self, transport: asyncio.DatagramTransport) -> None:
        self.transport = transport
        # Nothing extra needed; multicast membership is set up on the socket
        # before it is passed to create_datagram_endpoint.

    def datagram_received(self, data: bytes, addr: tuple[str, int]) -> None:
        try:
            ident, qname, qtype_str = parse_dns_query(data)
            if not qname:
                return
            log.debug("DNS/%s query: %s %s from %s",
                       "mDNS" if self.is_mdns else "UDP", qtype_str, qname, addr)

            rdata = resolve(qname, qtype_str)
            if rdata:
                response = build_dns_response(ident, qname, qtype_str, rdata)
                self.transport.sendto(response, addr)
                log.debug("DNS/%s response: %s -> %s",
                           "mDNS" if self.is_mdns else "UDP", qname, rdata)
            else:
                header = make_dns_header(ident, qr=1, rcode=3, ancount=0)
                question = encode_name(qname) + struct.pack("!HH",
                    1 if qtype_str == "A" else 28, 1)
                self.transport.sendto(header + question, addr)
        except Exception as e:
            log.warning("DNS/%s error: %s",
                        "mDNS" if self.is_mdns else "UDP", e)

    def error_received(self, exc: Exception) -> None:
        log.error("DNS/%s socket error: %s",
                  "mDNS" if self.is_mdns else "UDP", exc)


async def create_udp_endpoint(host: str, port: int, is_mdns: bool = False) -> None:
    """Create a UDP datagram endpoint for DNS or mDNS.

    The socket is created manually before passing it to asyncio so that
    SO_REUSEADDR and multicast membership (for mDNS) can be set upfront.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # NOTE: SO_REUSEPORT is intentionally omitted — it is unnecessary on
    # loopback and can cause duplicate-delivery on some platforms.
    sock.setblocking(False)
    sock.bind((host, port))

    if is_mdns:
        mreq = struct.pack("4sl", socket.inet_aton(MDNS_ADDR), socket.INADDR_ANY)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        log.info("mDNS listening on %s:%d (multicast %s)", host, port, MDNS_ADDR)
    else:
        log.info("DNS/UDP listening on %s:%d", host, port)

    loop = asyncio.get_running_loop()
    await loop.create_datagram_endpoint(
        lambda: DNSProtocol(is_mdns),
        sock=sock,
    )


# ---------------------------------------------------------------------------
# DoT (TCP + TLS) — asyncio Stream-based
# ---------------------------------------------------------------------------


async def handle_dot_client(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    """Handle a single DoT connection."""
    try:
        data = await reader.readexactly(2)
        length = struct.unpack("!H", data)[0]
        query = await reader.readexactly(length)

        ident, qname, qtype_str = parse_dns_query(query)
        log.debug("DoT query: %s %s", qtype_str, qname)

        rdata = resolve(qname, qtype_str)
        if rdata:
            response = build_dns_response(ident, qname, qtype_str, rdata)
        else:
            response = make_dns_header(ident, qr=1, rcode=3, ancount=0)
            response += encode_name(qname) + struct.pack("!HH",
                1 if qtype_str == "A" else 28, 1)

        writer.write(struct.pack("!H", len(response)) + response)
        await writer.drain()
    except asyncio.IncompleteReadError:
        pass
    except Exception as e:
        log.warning("DoT error: %s", e)
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except (BrokenPipeError, ConnectionResetError):
            pass


async def run_dot(host: str, port: int, ssl_context: ssl.SSLContext | None = None) -> None:
    """Serve DoT (TCP DNS with TLS)."""
    server = await asyncio.start_server(
        handle_dot_client, host, port, ssl=ssl_context,
        reuse_address=True, reuse_port=True,
    )
    proto = "DoT" if ssl_context else "TCP/DNS"
    log.info("%s listening on %s:%d", proto, host, port)
    async with server:
        await server.serve_forever()


# ---------------------------------------------------------------------------
# DoH (HTTP + TLS) — aiohttp
# ---------------------------------------------------------------------------


async def doh_handler(request: web.Request) -> web.Response:
    """DNS-over-HTTPS handler (RFC 8484)."""
    body = await request.read()
    if len(body) == 0:
        return web.Response(status=400)

    ident, qname, qtype_str = parse_dns_query(body)
    log.debug("DoH POST: %s %s", qtype_str, qname)

    rdata = resolve(qname, qtype_str)
    if rdata:
        response = build_dns_response(ident, qname, qtype_str, rdata)
    else:
        response = make_dns_header(ident, qr=1, rcode=3, ancount=0)
        response += encode_name(qname) + struct.pack("!HH",
            1 if qtype_str == "A" else 28, 1)
    return web.Response(
        body=response,
        content_type="application/dns-message",
    )


async def run_doh(host: str, port: int, ssl_context: ssl.SSLContext) -> None:
    """Serve DoH via aiohttp."""
    app = web.Application()
    app.router.add_post("/dns-query", doh_handler)
    # Also accept POST on any path (backward compat with previous impl)
    app.router.add_post("/{tail:.*}", doh_handler)

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, host, port, ssl_context=ssl_context)
    await site.start()
    log.info("DoH listening on %s:%d", host, port)

    # Run forever
    await asyncio.Event().wait()


# ---------------------------------------------------------------------------
# Dummy API (aiohttp) — records update requests from yaddnsc
# ---------------------------------------------------------------------------


async def api_health(request: web.Request) -> web.Response:
    return web.json_response({"status": "ok"})


async def api_logs(request: web.Request) -> web.Response:
    async with _update_lock:
        return web.json_response(list(update_requests))


async def api_myip(request: web.Request) -> web.Response:
    return web.Response(
        text="198.51.100.1",
        content_type="text/plain",
    )


async def api_reset(request: web.Request) -> web.Response:
    async with _update_lock:
        update_requests.clear()
    return web.json_response({"reset": True})


async def api_update_get(request: web.Request) -> web.Response:
    async with _update_lock:
        update_requests.append({
            "method": "GET",
            "path": str(request.rel_url),
            "headers": dict(request.headers),
            "time": time.time(),
        })
    return web.json_response({"success": True})


async def api_update_post(request: web.Request) -> web.Response:
    body = await request.read()
    async with _update_lock:
        update_requests.append({
            "method": "POST",
            "path": str(request.rel_url),
            "body": body.decode(),
            "headers": dict(request.headers),
            "time": time.time(),
        })
    return web.json_response({"success": True})


def build_api_app() -> web.Application:
    """Build the dummy API aiohttp application."""
    app = web.Application()

    app.router.add_get("/health", api_health)
    app.router.add_get("/logs", api_logs)
    app.router.add_get("/myip", api_myip)
    app.router.add_get("/reset", api_reset)
    app.router.add_get("/update", api_update_get)
    app.router.add_get("/update/{tail:.*}", api_update_get)
    app.router.add_post("/update", api_update_post)
    app.router.add_post("/update/{tail:.*}", api_update_post)

    return app


async def run_api(host: str, port: int) -> None:
    """Serve dummy API via aiohttp."""
    app = build_api_app()
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, host, port)
    await site.start()
    log.info("Dummy API listening on %s:%d", host, port)

    # Run forever
    await asyncio.Event().wait()


# ---------------------------------------------------------------------------
# TLS certificate generation
# ---------------------------------------------------------------------------


def ensure_certificates() -> None:
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


async def main() -> None:
    ensure_certificates()
    ssl_ctx = create_ssl_context()

    # Start all servers concurrently in the same event loop.
    servers = await asyncio.gather(
        create_udp_endpoint(DNS_HOST, DNS_PORT),
        create_udp_endpoint(DNS_HOST, MDNS_PORT, is_mdns=True),
        run_dot(DNS_HOST, DOT_PORT, ssl_ctx),
        run_doh(DNS_HOST, DOH_PORT, ssl_ctx),
        run_api(DNS_HOST, API_PORT),
        return_exceptions=True,
    )

    # Log any startup failure
    for s in servers:
        if isinstance(s, Exception):
            log.error("Server failed to start: %s", s)

    # Keep running indefinitely (run_forever servers never return)
    await asyncio.Event().wait()


if __name__ == "__main__":
    asyncio.run(main())
