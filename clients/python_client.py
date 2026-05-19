#!/usr/bin/env python3
"""Client ordinar alternativ pentru protocolul SQL Code Generator."""

import argparse
import socket
import struct
import sys
from pathlib import Path

SQLCG_PORT = 18081
SQLCG_FILE_CHUNK = 64 * 1024

OP_CONNECT = 0
OP_BYE = 5
OP_UPLOAD_BEGIN = 10
OP_UPLOAD_CHUNK = 11
OP_UPLOAD_END = 12
OP_DOWNLOAD_BEGIN = 13
OP_DOWNLOAD_CHUNK = 14
OP_DOWNLOAD_END = 15
OP_GENERATE_SQL = 20
OP_DOWNLOAD_SQL = 21
OP_VALIDATE_INSERT = 30
OP_JOB_STATUS = 31
OP_JOB_RESULT = 32
OP_OK = 40
OP_ERROR = 41


def recvall(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("connection closed")
        data.extend(chunk)
    return bytes(data)


def send_message(sock: socket.socket, client_id: int, op_id: int, payload: bytes = b"") -> None:
    sock.sendall(struct.pack("!IIII", len(payload), client_id, op_id, 0))
    if payload:
        sock.sendall(payload)


def recv_message(sock: socket.socket) -> tuple[int, int, bytes]:
    msg_size, client_id, op_id, _flags = struct.unpack("!IIII", recvall(sock, 16))
    payload = recvall(sock, msg_size) if msg_size else b""
    return client_id, op_id, payload


def request_text(sock: socket.socket, client_id: int, op_id: int, text: str) -> str:
    send_message(sock, client_id, op_id, text.encode())
    _reply_client, reply_op, payload = recv_message(sock)
    decoded = payload.decode(errors="replace")
    if reply_op != OP_OK:
        raise RuntimeError(decoded or "request failed")
    return decoded


def connect_protocol(sock: socket.socket) -> int:
    reply = request_text(sock, 0, OP_CONNECT, "")
    return int(reply)


def upload(sock: socket.socket, client_id: int, path: Path) -> None:
    print(request_text(sock, client_id, OP_UPLOAD_BEGIN, str(path)))
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(SQLCG_FILE_CHUNK)
            if not chunk:
                break
            send_message(sock, client_id, OP_UPLOAD_CHUNK, chunk)
            _reply_client, reply_op, payload = recv_message(sock)
            if reply_op != OP_OK:
                raise RuntimeError(payload.decode(errors="replace"))
    print(request_text(sock, client_id, OP_UPLOAD_END, ""))


def download_sql(sock: socket.socket, client_id: int, path: Path) -> None:
    send_message(sock, client_id, OP_DOWNLOAD_SQL)
    bytes_written = 0
    with path.open("wb") as stream:
        saw_begin = False
        while True:
            _reply_client, reply_op, payload = recv_message(sock)
            if reply_op == OP_DOWNLOAD_BEGIN:
                saw_begin = True
            elif reply_op == OP_DOWNLOAD_CHUNK:
                if not saw_begin:
                    raise RuntimeError("download chunk before begin")
                stream.write(payload)
                bytes_written += len(payload)
            elif reply_op == OP_DOWNLOAD_END:
                print(f"Saved {bytes_written} bytes to {path}")
                return
            elif reply_op == OP_ERROR:
                raise RuntimeError(payload.decode(errors="replace"))
            else:
                raise RuntimeError(f"unexpected op {reply_op}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=SQLCG_PORT)
    parser.add_argument("--input", type=Path)
    parser.add_argument("--generate", action="store_true")
    parser.add_argument("--download-sql", type=Path)
    parser.add_argument("--insert-file", type=Path)
    parser.add_argument("--status", default=None)
    parser.add_argument("--result", default=None)
    args = parser.parse_args()

    with socket.create_connection((args.host, args.port)) as sock:
        client_id = connect_protocol(sock)
        print(f"Connected as client {client_id}")
        if args.input:
            upload(sock, client_id, args.input)
        if args.generate:
            print(request_text(sock, client_id, OP_GENERATE_SQL, ""))
        if args.download_sql:
            download_sql(sock, client_id, args.download_sql)
        if args.insert_file:
            print(request_text(sock, client_id, OP_VALIDATE_INSERT, args.insert_file.read_text()))
        if args.status is not None:
            print(request_text(sock, client_id, OP_JOB_STATUS, args.status or "last"))
        if args.result is not None:
            print(request_text(sock, client_id, OP_JOB_RESULT, args.result or "last"))
        print(request_text(sock, client_id, OP_BYE, ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
