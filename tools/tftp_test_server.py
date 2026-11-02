#!/usr/bin/env python3
"""
tftp_test_server.py - Minimal in-memory RFC-compliant TFTP server
(RFC 1350/2347/2348/2349) for testing a TFTP client implementation.

Pure Python 3 standard library, no dependencies, no filesystem I/O.

Features:
  - Serves an in-memory file (--file, default "sample.bin") of --size bytes
    (deterministic make_test_data() pattern). Requests for any other filename
    receive TFTP ERROR 1 (File not found).
  - Receives uploaded (WRQ) payloads in memory and checks whether the received
    bytes match make_test_data(len(received)) ("data_valid": true/false).
  - Prints "READY <port>" once bound and "SESSION <json>" to stdout after each
    transfer so tests can inspect the negotiated options and outcome in memory.
  - Fault/behavior flags for testing client edge cases:
      --max-blksize N   clamp negotiated blksize to N (RFC 2348 server clamping)
      --ignore-options  act as legacy RFC 1350 server (no OACK, reply DATA 1 / ACK 0)
      --bad-oack        inject an unrequested option in OACK (client must ERROR 8)
      --drop N          drop the N-th packet from the client (packet-loss test)

Usage:
  ./tftp_test_server.py -p 6969
  ./tftp_test_server.py -p 0 --size 20000 --once
"""
import argparse
import json
import socket
import struct
import sys

RRQ, WRQ, DATA, ACK, ERROR, OACK = 1, 2, 3, 4, 5, 6
OPNAMES = {1: "RRQ", 2: "WRQ", 3: "DATA", 4: "ACK", 5: "ERROR", 6: "OACK"}
MIN_BLKSIZE, MAX_BLKSIZE = 8, 65464


def make_test_data(size):
    """Deterministic in-memory byte pattern shared with the C++ tests."""
    return bytes((i * 7 + 3) & 0xFF for i in range(size))


def parse_options(payload):
    parts = payload.split(b"\0")
    if parts and parts[-1] == b"":
        parts = parts[:-1]
    return {parts[i].decode(errors="replace").lower(): parts[i + 1].decode(errors="replace")
            for i in range(0, len(parts) - 1, 2)}


def build_oack(opts):
    pkt = struct.pack("!H", OACK)
    for k, v in opts.items():
        pkt += f"{k}\0{v}\0".encode()
    return pkt


def build_ack(block):
    return struct.pack("!HH", ACK, block & 0xFFFF)


def build_data(block, payload):
    return struct.pack("!HH", DATA, block & 0xFFFF) + payload


def build_error(code, msg):
    return struct.pack("!HH", ERROR, code) + msg.encode() + b"\0"


def negotiate(req_opts, file_size, is_write, args):
    """Return (oack_dict, blksize, timeout_s)."""
    if args.ignore_options:
        return {}, 512, args.timeout

    oack, blksize, timeout_s = {}, 512, args.timeout

    if req_opts.get("blksize", "").isdigit():
        req_bs = int(req_opts["blksize"])
        if req_bs >= MIN_BLKSIZE:
            blksize = min(req_bs, args.max_blksize, MAX_BLKSIZE)
            oack["blksize"] = str(blksize)

    if req_opts.get("tsize", "").isdigit():
        oack["tsize"] = req_opts["tsize"] if is_write else str(file_size)

    if req_opts.get("timeout", "").isdigit():
        t = int(req_opts["timeout"])
        if 1 <= t <= 255:
            timeout_s = float(t)
            oack["timeout"] = str(t)

    if args.bad_oack:
        oack["unrequested_opt"] = "123"

    return oack, blksize, timeout_s


def handle_session(op, filename, req_opts, client_addr, args):
    summary = {
        "op": OPNAMES.get(op, str(op)),
        "filename": filename,
        "req_options": req_opts,
        "oack_options": {},
        "blksize": 512,
        "blocks": 0,
        "bytes": 0,
        "dropped": 0,
        "data_valid": False,
        "client_error": None,
        "ok": False,
    }

    data = b""
    if op == RRQ:
        if filename != args.file:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.sendto(build_error(1, "File not found"), client_addr)
            return summary
        data = make_test_data(args.size)

    oack, blksize, timeout = negotiate(req_opts, len(data), op == WRQ, args)
    summary["oack_options"] = oack
    summary["blksize"] = blksize
    rx_count = 0

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)

        def send_and_recv(pkt, send_first=True):
            nonlocal rx_count
            for attempt in range(args.retries + 1):
                if attempt > 0 or send_first:
                    s.sendto(pkt, client_addr)
                try:
                    while True:
                        raw, addr = s.recvfrom(65536)
                        if addr != client_addr:
                            s.sendto(build_error(5, "Unknown TID"), addr)
                            continue
                        rx_count += 1
                        if rx_count in args.drop:
                            summary["dropped"] += 1
                            continue
                        rop = struct.unpack("!H", raw[:2])[0]
                        if rop == ERROR:
                            code = struct.unpack("!H", raw[2:4])[0]
                            msg = raw[4:].split(b"\0")[0].decode(errors="replace")
                            summary["client_error"] = [code, msg]
                        return rop, raw[2:]
                except socket.timeout:
                    continue
            return None, None

        if op == RRQ:
            if oack:
                rop, rbody = send_and_recv(build_oack(oack))
                if rop != ACK or struct.unpack("!H", rbody[:2])[0] != 0:
                    return summary
            block, offset = 1, 0
            while True:
                chunk = data[offset:offset + blksize]
                rop, rbody = send_and_recv(build_data(block, chunk))
                while rop == ACK and struct.unpack("!H", rbody[:2])[0] == ((block - 1) & 0xFFFF):
                    rop, rbody = send_and_recv(build_data(block, chunk), send_first=False)
                if rop != ACK or struct.unpack("!H", rbody[:2])[0] != (block & 0xFFFF):
                    return summary
                summary["blocks"] += 1
                summary["bytes"] += len(chunk)
                offset += len(chunk)
                block += 1
                if len(chunk) < blksize:
                    summary["data_valid"] = True
                    summary["ok"] = True
                    return summary

        elif op == WRQ:
            last_pkt = build_oack(oack) if oack else build_ack(0)
            received = bytearray()
            expected = 1
            while True:
                rop, rbody = send_and_recv(last_pkt)
                if rop != DATA:
                    return summary
                blk, chunk = struct.unpack("!H", rbody[:2])[0], rbody[2:]
                if blk == (expected & 0xFFFF):
                    received += chunk
                    summary["blocks"] += 1
                    summary["bytes"] += len(chunk)
                    last_pkt = build_ack(blk)
                    if len(chunk) < blksize:
                        s.sendto(last_pkt, client_addr)
                        summary["data_valid"] = (bytes(received) == make_test_data(len(received)))
                        summary["ok"] = True
                        return summary
                    expected += 1


def main():
    ap = argparse.ArgumentParser(description="Minimal in-memory RFC-compliant TFTP test server")
    ap.add_argument("-b", "--bind", default="127.0.0.1")
    ap.add_argument("-p", "--port", type=int, default=6969, help="UDP port (0 = ephemeral)")
    ap.add_argument("-f", "--file", default="sample.bin", help="in-memory file name served on RRQ")
    ap.add_argument("-s", "--size", type=int, default=20000, help="size in bytes of the in-memory file")
    ap.add_argument("-t", "--timeout", type=float, default=1.0)
    ap.add_argument("-r", "--retries", type=int, default=3)
    ap.add_argument("--max-blksize", type=int, default=MAX_BLKSIZE, help="clamp negotiated blksize")
    ap.add_argument("--ignore-options", action="store_true", help="ignore all options (no OACK)")
    ap.add_argument("--bad-oack", action="store_true", help="send an unrequested option in OACK")
    ap.add_argument("--drop", type=int, action="append", default=[], metavar="N",
                    help="drop the N-th packet received from the client (repeatable)")
    ap.add_argument("--once", action="store_true", help="handle a single transfer and exit")
    ap.add_argument("--idle-timeout", type=float, default=10.0,
                    help="seconds to wait for a request before exiting (default: 10)")
    args = ap.parse_args()

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as srv:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((args.bind, args.port))
        srv.settimeout(args.idle_timeout)
        print(f"READY {srv.getsockname()[1]}", flush=True)

        while True:
            try:
                raw, addr = srv.recvfrom(65536)
            except socket.timeout:
                return 3
            if len(raw) < 4:
                continue
            op = struct.unpack("!H", raw[:2])[0]
            if op not in (RRQ, WRQ):
                srv.sendto(build_error(4, "Expected RRQ or WRQ"), addr)
                continue
            parts = raw[2:].split(b"\0")
            filename = parts[0].decode(errors="replace")
            req_opts = parse_options(b"\0".join(parts[2:]))
            summary = handle_session(op, filename, req_opts, addr, args)
            print(f"SESSION {json.dumps(summary, sort_keys=True)}", flush=True)
            if args.once:
                return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
