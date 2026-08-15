#!/usr/bin/env python3
"""Validate the STM32 TCP Echo server with text and binary payloads."""

from __future__ import annotations

import argparse
import select
import socket
import ssl
import sys
import time


DEFAULT_HOST = "192.168.2.10"
DEFAULT_PORT = 4433
DEFAULT_SIZE = 4096
DEFAULT_REPEAT = 3
DEFAULT_TIMEOUT = 3.0
DEFAULT_TOKEN = "stm32h753"
AUTH_OK = b"OK\r\n"
AUTH_FAILED = b"ERR authentication failed\r\n"


def create_tls_connection(
    host: str, port: int, timeout: float
) -> ssl.SSLSocket:
    """Open a TLS 1.2 connection to the development certificate server."""
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.maximum_version = ssl.TLSVersion.TLSv1_2
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    tcp_socket = socket.create_connection((host, port), timeout=timeout)
    try:
        return context.wrap_socket(tcp_socket, server_hostname=None)
    except Exception:
        tcp_socket.close()
        raise


def receive_exact(connection: socket.socket, length: int) -> bytes:
    """Receive exactly length bytes or stop if the peer closes."""
    received = bytearray()
    while len(received) < length:
        chunk = connection.recv(length - len(received))
        if not chunk:
            break
        received.extend(chunk)
    return bytes(received)


def authenticate(connection: socket.socket, token: str) -> None:
    """Authenticate one connection before starting the Echo exchange."""
    connection.sendall(f"AUTH {token}\r\n".encode("utf-8"))
    response = receive_exact(connection, len(AUTH_OK))
    if response != AUTH_OK:
        raise PermissionError(f"authentication rejected: {response!r}")


def exchange(
    host: str, port: int, token: str, payload: bytes, timeout: float
) -> bytes:
    """Send and receive concurrently so TCP flow control cannot deadlock."""
    received = bytearray()
    sent_offset = 0

    with create_tls_connection(host, port, timeout) as connection:
        authenticate(connection, token)
        connection.setblocking(False)
        deadline = time.monotonic() + timeout

        while (sent_offset < len(payload)) or (len(received) < len(payload)):
            read_list = [connection] if len(received) < len(payload) else []
            write_list = [connection] if sent_offset < len(payload) else []
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    f"no progress for {timeout:g}s "
                    f"(sent={sent_offset}, received={len(received)})"
                )

            readable, writable, _ = select.select(
                read_list, write_list, [], remaining
            )
            progressed = False

            if writable:
                try:
                    sent_length = connection.send(
                        memoryview(payload)[sent_offset:]
                    )
                    if sent_length == 0:
                        raise ConnectionError("connection closed while sending")
                    sent_offset += sent_length
                    progressed = True
                except (ssl.SSLWantReadError, ssl.SSLWantWriteError):
                    pass

            if readable:
                try:
                    chunk = connection.recv(
                        min(4096, len(payload) - len(received))
                    )
                    if not chunk:
                        break
                    received.extend(chunk)
                    progressed = True
                except (ssl.SSLWantReadError, ssl.SSLWantWriteError):
                    pass

            if progressed:
                deadline = time.monotonic() + timeout

    return bytes(received)


def first_difference(expected: bytes, actual: bytes) -> str:
    """Return a concise description of the first payload mismatch."""
    common_length = min(len(expected), len(actual))
    for index in range(common_length):
        if expected[index] != actual[index]:
            return (
                f"byte {index}: expected=0x{expected[index]:02x} "
                f"actual=0x{actual[index]:02x}"
            )

    if len(expected) != len(actual):
        return f"length: expected={len(expected)} actual={len(actual)}"

    return "no difference"


def verify_case(
    label: str,
    host: str,
    port: int,
    token: str,
    payload: bytes,
    timeout: float,
) -> None:
    """Execute one Echo exchange and fail if any byte differs."""
    echoed = exchange(host, port, token, payload, timeout)
    matched = echoed == payload
    print(
        f"{label}: sent={len(payload)} received={len(echoed)} "
        f"match={'yes' if matched else 'no'}"
    )

    if not matched:
        raise RuntimeError(f"{label}: echo mismatch ({first_difference(payload, echoed)})")


def verify_rejected(host: str, port: int, timeout: float) -> None:
    """Confirm that an invalid token is rejected and not echoed."""
    with create_tls_connection(host, port, timeout) as connection:
        connection.sendall(b"AUTH definitely-invalid-token\r\n")
        response = receive_exact(connection, len(AUTH_FAILED))

    matched = response == AUTH_FAILED
    print(f"invalid-token: rejected={'yes' if matched else 'no'}")
    if not matched:
        raise RuntimeError(f"unexpected authentication response: {response!r}")


def verify_auth_stream_handling(
    host: str, port: int, token: str, timeout: float
) -> None:
    """Check split AUTH input and data following AUTH in the same TCP stream."""
    payload = b"combined-auth-and-data\r\n"
    auth_line = f"AUTH {token}\r\n".encode("utf-8")

    with create_tls_connection(host, port, timeout) as connection:
        split_at = max(1, len(auth_line) // 2)
        connection.sendall(auth_line[:split_at])
        time.sleep(0.05)
        connection.sendall(auth_line[split_at:])
        response = receive_exact(connection, len(AUTH_OK))
        if response != AUTH_OK:
            raise RuntimeError(f"split authentication failed: {response!r}")

    with create_tls_connection(host, port, timeout) as connection:
        connection.sendall(auth_line + payload)
        response = receive_exact(connection, len(AUTH_OK) + len(payload))
        expected = AUTH_OK + payload
        if response != expected:
            raise RuntimeError(
                "combined authentication/data mismatch "
                f"({first_difference(expected, response)})"
            )

    print("auth-stream: split=yes combined-data=yes")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Test an STM32/lwIP MbedTLS Echo server."
    )
    parser.add_argument("--host", default=DEFAULT_HOST, help="server IPv4 address")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="TCP port")
    parser.add_argument(
        "--token",
        default=DEFAULT_TOKEN,
        help="pre-shared authentication token",
    )
    parser.add_argument(
        "--size",
        type=int,
        default=DEFAULT_SIZE,
        help="binary payload size in bytes",
    )
    parser.add_argument(
        "--repeat",
        type=int,
        default=DEFAULT_REPEAT,
        help="number of binary reconnect tests",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT,
        help="connect and receive timeout in seconds",
    )
    args = parser.parse_args()

    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if args.size < 1:
        parser.error("--size must be at least 1")
    if args.repeat < 1:
        parser.error("--repeat must be at least 1")
    if args.timeout <= 0:
        parser.error("--timeout must be greater than 0")
    if "\r" in args.token or "\n" in args.token:
        parser.error("--token must not contain CR or LF")

    return args


def main() -> int:
    args = parse_args()
    text_payload = "STM32H753 TCP Echo test: 日本語 UTF-8\r\n".encode("utf-8")
    binary_payload = bytes(index % 251 for index in range(args.size))

    print(
        f"target={args.host}:{args.port} "
        f"payload_size={args.size} repeat={args.repeat}"
    )

    try:
        verify_rejected(args.host, args.port, args.timeout)
        verify_auth_stream_handling(args.host, args.port, args.token, args.timeout)
        verify_case(
            "text", args.host, args.port, args.token, text_payload, args.timeout
        )
        for attempt in range(1, args.repeat + 1):
            verify_case(
                f"binary[{attempt}/{args.repeat}]",
                args.host,
                args.port,
                args.token,
                binary_payload,
                args.timeout,
            )
    except (OSError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1

    print("PASS: all TCP echo tests succeeded")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
