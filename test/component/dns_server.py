#!/usr/bin/env python3
"""DNS server for component tests — fully async.

Usage:
    dns_server.py <port>

Listens on 127.0.0.1:<port> for both UDP and TCP DNS queries.

Built-in records:
    yaddnsc.test          A    198.51.100.42
    yaddnsc.test          AAAA 2001:db8::42
    truncate.yaddnsc.test A    198.51.100.99   (UDP: TC=1, TCP: normal)
    malformed.yaddnsc.test A   —               (returns garbage)
"""

import asyncio
import socket
import struct
import signal
import sys

DEFAULT_RECORDS = {
    "yaddnsc.test": {"A": "198.51.100.42", "AAAA": "2001:db8::42"},
}

# Hostnames that trigger special behaviour.
TRUNCATE_HOST = "truncate.yaddnsc.test"
MALFORMED_HOST = "malformed.yaddnsc.test"

HOST = "127.0.0.1"
TYPE_MAP = {1: "A", 28: "AAAA"}

# ---------------------------------------------------------------------------
# Wire-format helpers
# ---------------------------------------------------------------------------


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
        labels.append(data[offset:offset + length].decode())
        offset += length
    return ".".join(labels), offset


def build_a_record(ip: str, ttl: int = 60) -> bytes:
    parts = [int(x) for x in ip.split(".")]
    rdata = bytes(parts)
    answer = struct.pack("!H", 0xC00C)  # pointer to offset 12
    answer += struct.pack("!HH", 1, 1)  # TYPE A, CLASS IN
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
            raw[i * 2:(i + 1) * 2] = (int(p, 16) if p else 0).to_bytes(2, "big")
        for i, p in enumerate(right_parts):
            idx = fill + len(left_parts) + i
            raw[idx * 2:(idx + 1) * 2] = (int(p, 16) if p else 0).to_bytes(2, "big")
    else:
        parts_list = ip.split(":")
        for i, p in enumerate(parts_list):
            raw[i * 2:(i + 1) * 2] = (int(p, 16) if p else 0).to_bytes(2, "big")
    rdata = bytes(raw)
    answer = struct.pack("!H", 0xC00C)
    answer += struct.pack("!HH", 28, 1)  # TYPE AAAA, CLASS IN
    answer += struct.pack("!I", ttl)
    answer += struct.pack("!H", len(rdata))
    answer += rdata
    return answer


def build_response(ident: int, question: bytes, qname: str, qtype_str: str,
                   records: dict, is_udp: bool = True) -> bytes:
    """Build a DNS response with QR=1, RA=1."""
    rdata = records.get(qname, {}).get(qtype_str)

    if qname == TRUNCATE_HOST and is_udp:
        # Return truncated response (TC=1) on UDP — triggers TCP fallback.
        # Flags: QR=1, TC=1, RA=1 => 0x8180 | 0x0200 = 0x8380
        flags = 0x8380
        header = struct.pack("!HHHHHH", ident, flags, 1, 0, 0, 0)
        return header + question

    if qname == MALFORMED_HOST:
        # Return garbage — should be rejected by the validator.
        return b"\x00" * 12 + question

    if not rdata and qtype_str == "A":
        rdata = "198.51.100.1"  # fallback

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
        header = struct.pack("!HHHHHH", ident, 0x8183, 1, 0, 0, 0)  # NXDOMAIN
        return header + question


def parse_query(data: bytes):
    """Parse a DNS query, return (ident, qname, qtype_str, question_wire)."""
    ident = struct.unpack("!H", data[:2])[0]
    qdcount = struct.unpack("!H", data[4:6])[0]
    if qdcount == 0:
        return ident, "", "", b""
    qname, qend = decode_name(data, 12)
    qtype = struct.unpack("!H", data[qend:qend + 2])[0]
    qtype_str = TYPE_MAP.get(qtype, f"TYPE{qtype}")
    question = data[12:qend + 4]
    return ident, qname, qtype_str, question


# ---------------------------------------------------------------------------
# UDP — asyncio DatagramProtocol
# ---------------------------------------------------------------------------


class DNSProtocol(asyncio.DatagramProtocol):
    """Asynchronous UDP DNS responder."""

    def __init__(self, records: dict) -> None:
        self.records = records
        self.transport: asyncio.DatagramTransport | None = None

    def connection_made(self, transport: asyncio.DatagramTransport) -> None:
        self.transport = transport

    def datagram_received(self, data: bytes, addr: tuple[str, int]) -> None:
        try:
            ident, qname, qtype_str, question = parse_query(data)
            if not qname:
                return
            response = build_response(ident, question, qname, qtype_str,
                                      self.records, is_udp=True)
            self.transport.sendto(response, addr)
        except Exception:
            pass

    def error_received(self, exc: Exception) -> None:
        pass


async def create_udp_endpoint(host: str, port: int, records: dict) -> asyncio.DatagramTransport:
    """Create a UDP datagram endpoint for DNS."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setblocking(False)
    sock.bind((host, port))

    loop = asyncio.get_running_loop()
    transport, _ = await loop.create_datagram_endpoint(
        lambda: DNSProtocol(records),
        sock=sock,
    )
    return transport


# ---------------------------------------------------------------------------
# TCP — asyncio Stream-based
# ---------------------------------------------------------------------------


async def handle_tcp_client(reader: asyncio.StreamReader,
                            writer: asyncio.StreamWriter,
                            records: dict) -> None:
    """Handle a single TCP DNS connection."""
    try:
        raw_len = await reader.readexactly(2)
        msg_len = struct.unpack("!H", raw_len)[0]
        data = await reader.readexactly(msg_len)

        ident, qname, qtype_str, question = parse_query(data)
        response = build_response(ident, question, qname, qtype_str,
                                  records, is_udp=False)

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


async def run_tcp(host: str, port: int, records: dict) -> None:
    """Serve TCP DNS."""
    server = await asyncio.start_server(
        lambda r, w: handle_tcp_client(r, w, records),
        host, port,
        reuse_address=True,
    )
    async with server:
        await server.serve_forever()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


async def main() -> None:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 15353
    records = DEFAULT_RECORDS
    shutdown_event = asyncio.Event()

    def handle_sig() -> None:
        shutdown_event.set()

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(sig, handle_sig)

    # Start UDP and TCP servers concurrently.
    udp_transport = await create_udp_endpoint(HOST, port, records)
    tcp_task = asyncio.create_task(run_tcp(HOST, port, records))

    print(f"READY port={port}", flush=True)

    await shutdown_event.wait()

    # Graceful shutdown.
    udp_transport.close()
    tcp_task.cancel()
    try:
        await tcp_task
    except asyncio.CancelledError:
        pass


if __name__ == "__main__":
    asyncio.run(main())
