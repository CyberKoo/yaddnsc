#!/usr/bin/env python3
"""DoH server for component tests — fully async.

Usage:
    doh_server.py <port> <cert_pem> <key_pem>

Listens on 127.0.0.1:<port> for DNS-over-HTTPS (RFC 8484) connections.
Handles HTTP/1.1 POST to /dns-query with Content-Type: application/dns-message.

Built-in records:
    yaddnsc.test            A    198.51.100.42
    yaddnsc.test            AAAA 2001:db8::42
    doh-timeout.yaddnsc.test  —  accept but hang (never respond)
    doh-404.yaddnsc.test      —  return HTTP 404 with dns-message body
    doh-500.yaddnsc.test      —  return HTTP 500 with dns-message body
    doh-malformed.yaddnsc.test  —  return wrong Content-Type
"""

import asyncio
import ssl
import struct
import signal
import sys
import re

HOST = "127.0.0.1"

DEFAULT_RECORDS = {
    "yaddnsc.test": {"A": "198.51.100.42", "AAAA": "2001:db8::42"},
    "doh-chunked.yaddnsc.test": {"A": "198.51.100.42"},
}

TIMEOUT_HOST = "doh-timeout.yaddnsc.test"
HTTP_404_HOST = "doh-404.yaddnsc.test"
HTTP_500_HOST = "doh-500.yaddnsc.test"
MALFORMED_HOST = "doh-malformed.yaddnsc.test"
CHUNKED_HOST = "doh-chunked.yaddnsc.test"
INVALID_DNS_HOST = "doh-invalid-dns.yaddnsc.test"
MALFORMED_HEADER_HOST = "doh-malformed-header.yaddnsc.test"

TYPE_MAP = {1: "A", 28: "AAAA"}


def encode_name(name: str) -> bytes:
    parts = name.rstrip(".").split(".")
    out = b"".join(bytes([len(p)]) + p.encode() for p in parts)
    return out + b"\x00"


def decode_name(data: bytes, offset: int) -> tuple[str, int]:
    labels = []
    jumped = False
    while offset < len(data):
        length = data[offset]
        if length == 0:
            offset += 1
            break
        if length & 0xC0:
            ptr = ((length & 0x3F) << 8) | data[offset + 1]
            if not jumped:
                offset += 2
                jumped = True
            sub_name, _ = decode_name(data, ptr)
            labels.append(sub_name)
            break
        offset += 1
        labels.append(data[offset : offset + length].decode())
        offset += length
    return ".".join(labels), offset


def build_a_record(ip: str, ttl: int = 60) -> bytes:
    parts = [int(x) for x in ip.split(".")]
    rdata = bytes(parts)
    answer = struct.pack("!H", 0xC00C)
    answer += struct.pack("!HH", 1, 1)
    answer += struct.pack("!I", ttl)
    answer += struct.pack("!H", len(rdata))
    answer += rdata
    return answer


def build_aaaa_record(ip: str, ttl: int = 60) -> bytes:
    raw = bytearray(16)
    if "::" in ip:
        left, right = ip.split("::")
        left_parts = left.split(":") if left else []
        right_parts = right.split(":") if right else []
        fill = 8 - len(left_parts) - len(right_parts)
        for i, p in enumerate(left_parts):
            raw[i * 2 : (i + 1) * 2] = (int(p, 16) if p else 0).to_bytes(2, "big")
        for i, p in enumerate(right_parts):
            idx = fill + len(left_parts) + i
            raw[idx * 2 : (idx + 1) * 2] = (int(p, 16) if p else 0).to_bytes(2, "big")
    else:
        parts_list = ip.split(":")
        for i, p in enumerate(parts_list):
            raw[i * 2 : (i + 1) * 2] = (int(p, 16) if p else 0).to_bytes(2, "big")
    rdata = bytes(raw)
    answer = struct.pack("!H", 0xC00C)
    answer += struct.pack("!HH", 28, 1)
    answer += struct.pack("!I", ttl)
    answer += struct.pack("!H", len(rdata))
    answer += rdata
    return answer


def parse_dns_query(data: bytes):
    ident = struct.unpack("!H", data[:2])[0]
    qdcount = struct.unpack("!H", data[4:6])[0]
    if qdcount == 0:
        return ident, "", "", b""
    qname, qend = decode_name(data, 12)
    qtype = struct.unpack("!H", data[qend : qend + 2])[0]
    qtype_str = TYPE_MAP.get(qtype, f"TYPE{qtype}")
    question = data[12 : qend + 4]
    return ident, qname, qtype_str, question


def build_dns_response(ident: int, question: bytes, qname: str,
                       qtype_str: str, rcode: int = 0) -> bytes | None:
    """Build DNS response. Returns None if caller should hang."""
    rdata = DEFAULT_RECORDS.get(qname, {}).get(qtype_str)

    if rcode == 3:  # NXDOMAIN
        header = struct.pack("!HHHHHH", ident, 0x8183, 1, 0, 0, 0)
        return header + question

    if not rdata and qtype_str == "A":
        rdata = "198.51.100.1"

    if rdata:
        if qtype_str == "A":
            answer = build_a_record(rdata)
        elif qtype_str == "AAAA":
            answer = build_aaaa_record(rdata)
        else:
            answer = b""
        header = struct.pack("!HHHHHH", ident, 0x8180, 1, 1, 0, 0)
        return header + question + answer

    # NXDOMAIN
    header = struct.pack("!HHHHHH", ident, 0x8183, 1, 0, 0, 0)
    return header + question


def build_http_response(status: int, content_type: str, body: bytes) -> bytes:
    """Build an HTTP/1.1 response."""
    header = (
        f"HTTP/1.1 {status} {'OK' if status == 200 else 'Error'}\r\n"
        f"Content-Type: {content_type}\r\n"
        f"Content-Length: {len(body)}\r\n"
        f"Connection: close\r\n"
        f"\r\n"
    ).encode("ascii")
    return header + body


# Regex to parse the Request-Line
REQ_LINE_RE = re.compile(rb"POST\s+(\S+)\s+HTTP/1\.\d")


async def handle_doh_client(reader: asyncio.StreamReader,
                            writer: asyncio.StreamWriter) -> None:
    """Handle a single DoH (HTTP + TLS) connection."""
    try:
        # Read the request line and headers (stop at \r\n\r\n).
        request_bytes = b""
        while b"\r\n\r\n" not in request_bytes:
            chunk = await reader.read(4096)
            if not chunk:
                return
            request_bytes += chunk
            if len(request_bytes) > 16384:
                return  # Header too large

        # Parse Content-Length.
        headers_end = request_bytes.find(b"\r\n\r\n")
        content_length = 0
        for line in request_bytes[:headers_end].decode("ascii", errors="replace").split("\r\n")[1:]:
            if line.lower().startswith("content-length:"):
                try:
                    content_length = int(line.split(":")[1].strip())
                except (ValueError, IndexError):
                    pass
                break

        # Read the body
        body_start = headers_end + 4
        body = request_bytes[body_start:]
        while len(body) < content_length:
            chunk = await reader.read(4096)
            if not chunk:
                break
            body += chunk

        if not body:
            writer.write(build_http_response(400, "text/plain", b"Empty request"))
            await writer.drain()
            return

        ident, qname, qtype_str, question = parse_dns_query(body)
        if not qname:
            writer.write(build_http_response(400, "text/plain", b"Bad DNS query"))
            await writer.drain()
            return

        # Special behaviour hostnames
        if qname == TIMEOUT_HOST:
            # Hang forever — never respond.
            await asyncio.sleep(3600)
            return

        if qname == HTTP_404_HOST:
            # Return a proper DNS response wrapped in HTTP 404 with correct content type.
            # This tests that the DoH resolver properly reads the HTTP status code.
            dns_body = build_dns_response(ident, question, qname, qtype_str, rcode=3)
            writer.write(build_http_response(404, "application/dns-message",
                                             dns_body if dns_body else b""))
            await writer.drain()
            return

        if qname == HTTP_500_HOST:
            # Return a proper DNS response wrapped in HTTP 500.
            dns_body = build_dns_response(ident, question, qname, qtype_str, rcode=2)
            writer.write(build_http_response(500, "application/dns-message",
                                             dns_body if dns_body else b""))
            await writer.drain()
            return

        if qname == MALFORMED_HOST:
            # Wrong Content-Type — the DoH resolver's http parser will reject it.
            writer.write(build_http_response(200, "text/html", b"<html></html>"))
            await writer.drain()
            return

        if qname == INVALID_DNS_HOST:
            # Return valid HTTP with dns-message type but garbage DNS body.
            # The DoH resolver's DNS validator will reject this.
            dns_body = b"\x00" * 12  # Header only, no matching question
            writer.write(build_http_response(200, "application/dns-message", dns_body))
            await writer.drain()
            return

        if qname == MALFORMED_HEADER_HOST:
            # Invalid Content-Length value — causes parse_response to return
            # HEADER_PARSE_FAILED, which to_dns_error maps to PARSE.
            resp = (
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: application/dns-message\r\n"
                b"Content-Length: abc\r\n"
                b"\r\n"
            )
            writer.write(resp)
            await writer.drain()
            return

        if qname == CHUNKED_HOST:
            # Return a chunked transfer-encoded response.
            dns_response = build_dns_response(ident, question, qname, qtype_str)
            if dns_response is None:
                return
            resp = (
                f"HTTP/1.1 200 OK\r\n"
                f"Content-Type: application/dns-message\r\n"
                f"Transfer-Encoding: chunked\r\n"
                f"Connection: close\r\n"
                f"\r\n"
            ).encode("ascii")
            chunk = hex(len(dns_response))[2:].encode("ascii") + b"\r\n" + dns_response + b"\r\n"
            trailer = b"0\r\n\r\n"
            writer.write(resp + chunk + trailer)
            await writer.drain()
            return

        # Normal response
        dns_response = build_dns_response(ident, question, qname, qtype_str)
        if dns_response is None:
            return

        writer.write(build_http_response(200, "application/dns-message", dns_response))
        await writer.drain()

    except Exception:
        pass
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except (BrokenPipeError, ConnectionResetError):
            pass


async def main() -> None:
    port = int(sys.argv[1])
    cert_pem = sys.argv[2]
    key_pem = sys.argv[3]

    ssl_ctx = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    ssl_ctx.load_cert_chain(cert_pem, key_pem)
    ssl_ctx.set_alpn_protocols(["http/1.1"])

    shutdown_event = asyncio.Event()

    def handle_sig() -> None:
        shutdown_event.set()

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(sig, handle_sig)

    server = await asyncio.start_server(
        handle_doh_client, HOST, port, ssl=ssl_ctx,
        reuse_address=True,
    )

    print(f"READY port={port} cert={cert_pem}", flush=True)

    async with server:
        await shutdown_event.wait()


if __name__ == "__main__":
    asyncio.run(main())
