#!/usr/bin/env python3
"""DoT server for component tests — fully async.

Usage:
    dot_server.py <port> <cert_pem> <key_pem>

Listens on 127.0.0.1:<port> for DNS-over-TLS (RFC 7858) connections.

Built-in records (same as dns_server.py):
    yaddnsc.test            A    198.51.100.42
    yaddnsc.test            AAAA 2001:db8::42
    dot-timeout.yaddnsc.test  —  accept but hang (never respond)
    dot-reset.yaddnsc.test    —  close immediately after accepting
    dot-malformed.yaddnsc.test  —  return garbage response
    dot-zerolength.yaddnsc.test —  return 2-byte length prefix of 0
"""

import asyncio
import ssl
import struct
import signal
import sys

HOST = "127.0.0.1"

DEFAULT_RECORDS = {
    "yaddnsc.test": {"A": "198.51.100.42", "AAAA": "2001:db8::42"},
}

# Special hostnames.
TIMEOUT_HOST = "dot-timeout.yaddnsc.test"
RESET_HOST = "dot-reset.yaddnsc.test"
MALFORMED_HOST = "dot-malformed.yaddnsc.test"
ZERO_LENGTH_HOST = "dot-zerolength.yaddnsc.test"

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


def parse_query(data: bytes):
    ident = struct.unpack("!H", data[:2])[0]
    qdcount = struct.unpack("!H", data[4:6])[0]
    if qdcount == 0:
        return ident, "", "", b""
    qname, qend = decode_name(data, 12)
    qtype = struct.unpack("!H", data[qend : qend + 2])[0]
    qtype_str = TYPE_MAP.get(qtype, f"TYPE{qtype}")
    question = data[12 : qend + 4]
    return ident, qname, qtype_str, question


def build_response(ident: int, question: bytes, qname: str, qtype_str: str,
                   records: dict) -> bytes | None:
    """Build DNS response. Returns None if caller should hang (no response)."""
    rdata = records.get(qname, {}).get(qtype_str)

    if qname == TIMEOUT_HOST:
        return None

    if qname == RESET_HOST:
        return b""  # Empty bytes signals connection reset

    if qname == MALFORMED_HOST:
        # Return garbage (no valid DNS header)
        return b"\x00" * 16

    if qname == ZERO_LENGTH_HOST:
        # Two-byte length prefix of 0 is handled at the protocol layer;
        # we return a special marker and the wire framing layer handles it.
        return b"\x00\x00"  # 2-byte zero length

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
    else:
        header = struct.pack("!HHHHHH", ident, 0x8183, 1, 0, 0, 0)
        return header + question


async def handle_dot_client(reader: asyncio.StreamReader,
                            writer: asyncio.StreamWriter) -> None:
    """Handle a single DoT (TLS + DNS) connection."""
    try:
        # Read 2-byte big-endian length prefix (RFC 7858 §3.3)
        raw_len = await reader.readexactly(2)
        msg_len = struct.unpack("!H", raw_len)[0]

        if msg_len == 0:
            return

        data = await reader.readexactly(msg_len)
        ident, qname, qtype_str, question = parse_query(data)
        if not qname:
            return

        response = build_response(ident, question, qname, qtype_str, DEFAULT_RECORDS)

        if response is None:
            # Timeout host: never respond
            return

        if response == b"":
            # Reset host: close immediately
            return

        if response == b"\x00\x00":
            # Zero-length: write 2-byte 0 length prefix
            writer.write(b"\x00\x00")
            await writer.drain()
            return

        writer.write(struct.pack("!H", len(response)) + response)
        await writer.drain()

    except asyncio.IncompleteReadError:
        pass
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
        handle_dot_client, HOST, port, ssl=ssl_ctx,
        reuse_address=True,
    )

    print(f"READY port={port} cert={cert_pem}", flush=True)

    async with server:
        await shutdown_event.wait()


if __name__ == "__main__":
    asyncio.run(main())
