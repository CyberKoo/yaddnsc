#!/usr/bin/env python3
"""TLS echo server for component tests.

Listens on 127.0.0.1:<port> for TLS connections, reads data and sends it back.

Usage:
    tls_test_server.py <port> <cert_pem> <key_pem>
    
Modes:
    echo       — echo back all received data (for TlsConnection testing)
    
When a client connects, the server performs a TLS handshake, then reads
4 bytes as a length prefix, then that many bytes, and echoes them back.
"""
import asyncio
import ssl
import sys
import signal

HOST = "127.0.0.1"


async def handle_client(reader: asyncio.StreamReader,
                        writer: asyncio.StreamWriter) -> None:
    """Echo: read 4-byte length prefix + data, write it back."""
    try:
        raw_len = await reader.readexactly(4)
        msg_len = int.from_bytes(raw_len, "big")
        data = await reader.readexactly(msg_len)

        writer.write(raw_len + data)
        await writer.drain()
    except asyncio.IncompleteReadError:
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

    shutdown_event = asyncio.Event()

    def handle_sig() -> None:
        shutdown_event.set()

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(sig, handle_sig)

    server = await asyncio.start_server(
        handle_client, HOST, port, ssl=ssl_ctx, reuse_address=True,
    )

    print(f"READY port={port} cert={cert_pem}", flush=True)

    async with server:
        await shutdown_event.wait()


if __name__ == "__main__":
    asyncio.run(main())
