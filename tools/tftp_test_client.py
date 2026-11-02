#!/usr/bin/env python3
"""
tftp_test_client.py - Minimal RFC-compliant TFTP client (RFC 1350/2347/2348/2349)
for testing a TFTP server implementation in memory.

Pure Python 3 standard library, no dependencies.

Performs a single 'get' (RRQ) or 'put' (WRQ) transfer with the options given on
the command line and verifies that:
  1. the server's OACK (or absence of OACK) complies with RFC 2347/2348/2349:
     - only valid requested options appear in OACK,
     - negotiated blksize is in [8, min(requested, server_max_blksize)],
     - negotiated timeout echoes the requested value,
     - negotiated tsize equals the transferred byte count (RRQ) or echoes the
       requested size (WRQ),
     - if no valid options were requested, the server does not send OACK;
  2. the negotiated blksize was applied to the DATA packets (every block except
     the last has size == blksize, and the last has size < blksize);
  3. the downloaded payload (for 'get' with --size) matches the deterministic
     in-memory test pattern.

Usage:
  ./tftp_test_client.py 127.0.0.1 -p 6969 get test.bin --size 50000
  ./tftp_test_client.py 127.0.0.1 -p 6969 get test.bin --size 50000 --blksize 1428 --tsize --timeout 3
  ./tftp_test_client.py 127.0.0.1 -p 6969 get test.bin --size 50000 -O BLKSIZE=1024 -O TSIZE=0
  ./tftp_test_client.py 127.0.0.1 -p 6969 get missing.bin --expect-error 1
  ./tftp_test_client.py 127.0.0.1 -p 6969 put upload.bin --size 10000 --blksize 1024 --tsize
"""
import argparse
import socket
import struct
import sys

RRQ, WRQ, DATA, ACK, ERROR, OACK = 1, 2, 3, 4, 5, 6
OPNAMES = {1: "RRQ", 2: "WRQ", 3: "DATA", 4: "ACK", 5: "ERROR", 6: "OACK"}
MIN_BLKSIZE, MAX_BLKSIZE = 8, 65464


class TftpError(Exception):
    pass


class Transfer:
    def __init__(self):
        self.oack = None          # dict of options from OACK, None if no OACK
        self.blksize = 512        # effective block size used for the transfer
        self.data = bytearray()   # downloaded data
        self.block_sizes = []     # payload size of every DATA packet
        self.error = None         # (code, msg) if server answered with ERROR


def make_test_data(size):
    """Deterministic in-memory byte pattern shared with the C++ tests."""
    return bytes((i * 7 + 3) & 0xFF for i in range(size))


def build_request(opcode, filename, options):
    pkt = struct.pack("!H", opcode) + f"{filename}\0octet\0".encode()
    for k, v in options:
        pkt += f"{k}\0{v}\0".encode()
    return pkt


def build_ack(block):
    return struct.pack("!HH", ACK, block & 0xFFFF)


def parse_options(payload):
    parts = payload.split(b"\0")
    if parts and parts[-1] == b"":
        parts = parts[:-1]
    if len(parts) % 2:
        raise TftpError(f"malformed OACK payload: {payload!r}")
    return {parts[i].decode().lower(): parts[i + 1].decode()
            for i in range(0, len(parts), 2)}


def parse_error(payload):
    code = struct.unpack("!H", payload[:2])[0]
    return code, payload[2:].split(b"\0")[0].decode(errors="replace")


class Client:
    def __init__(self, host, port, timeout, retries):
        self.host, self.port = host, port
        self.timeout, self.retries = timeout, retries

    def _exchange(self, sock, pkt, dest, peer=None):
        """Send pkt (if not None) and wait for a reply, resending on timeout."""
        for _ in range(self.retries + 1):
            if pkt is not None:
                sock.sendto(pkt, dest)
            try:
                while True:
                    raw, addr = sock.recvfrom(65536)
                    if peer and addr != peer:
                        continue
                    return struct.unpack("!H", raw[:2])[0], raw[2:], addr
            except socket.timeout:
                continue
        raise TftpError("timeout waiting for server")

    def download(self, filename, options):
        t = Transfer()
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.settimeout(self.timeout)
            op, body, peer = self._exchange(
                s, build_request(RRQ, filename, options), (self.host, self.port))
            pkt = None
            if op == OACK:
                t.oack = parse_options(body)
                t.blksize = int(t.oack.get("blksize", 512))
                pkt = build_ack(0)
                op, body, _ = self._exchange(s, pkt, peer, peer)
            expected = 1
            while True:
                if op == ERROR:
                    t.error = parse_error(body)
                    return t
                if op != DATA:
                    raise TftpError(f"unexpected {OPNAMES.get(op, op)} during download")
                block, payload = struct.unpack("!H", body[:2])[0], body[2:]
                if block == (expected & 0xFFFF):
                    t.data += payload
                    t.block_sizes.append(len(payload))
                    pkt = build_ack(block)
                    if len(payload) < t.blksize:
                        s.sendto(pkt, peer)
                        return t
                    expected += 1
                op, body, _ = self._exchange(s, pkt, peer, peer)

    def upload(self, filename, data, options):
        t = Transfer()
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.settimeout(self.timeout)
            op, body, peer = self._exchange(
                s, build_request(WRQ, filename, options), (self.host, self.port))
            if op == ERROR:
                t.error = parse_error(body)
                return t
            if op == OACK:
                t.oack = parse_options(body)
                t.blksize = int(t.oack.get("blksize", 512))
            elif not (op == ACK and struct.unpack("!H", body[:2])[0] == 0):
                raise TftpError(f"expected OACK or ACK 0, got opcode {op}")
            block, offset = 1, 0
            while True:
                chunk = data[offset:offset + t.blksize]
                pkt = struct.pack("!HH", DATA, block & 0xFFFF) + chunk
                send = pkt
                while True:
                    op, body, _ = self._exchange(s, send, peer, peer)
                    if op == ERROR:
                        t.error = parse_error(body)
                        return t
                    if op == ACK and struct.unpack("!H", body[:2])[0] == (block & 0xFFFF):
                        break
                    send = None  # duplicate/old ACK: keep waiting
                t.block_sizes.append(len(chunk))
                offset += len(chunk)
                block += 1
                if len(chunk) < t.blksize:
                    return t


def is_valid_option(op, key, val):
    if not val.isdigit():
        return False
    n = int(val)
    if key == "blksize":
        return n >= MIN_BLKSIZE
    if key == "timeout":
        return 1 <= n <= 255
    if key == "tsize":
        return n == 0 if op == "get" else True
    return False


def verify_transfer(op, t, req_options, expected_size, server_max_blksize, expect_error):
    """Return a list of failure strings (empty = PASS)."""
    errs = []
    req = {k.lower(): str(v) for k, v in req_options}
    valid_req = {k: v for k, v in req.items() if is_valid_option(op, k, v)}

    if expect_error is not None:
        if not t.error:
            return [f"expected ERROR {expect_error}, but transfer succeeded"]
        if t.error[0] != expect_error:
            return [f"expected ERROR {expect_error}, got ERROR {t.error[0]}: {t.error[1]}"]
        return []

    if t.error:
        return [f"server replied ERROR {t.error[0]}: {t.error[1]}"]

    if valid_req and t.oack is None:
        errs.append(f"expected OACK for {valid_req}, but server ignored the options")
    if not valid_req and t.oack is not None:
        errs.append(f"expected no OACK when no valid options were sent, got {t.oack}")

    if t.oack is not None:
        if not t.oack:
            errs.append("empty OACK")
        for k, v in t.oack.items():
            if k not in valid_req:
                errs.append(f"OACK contains unrequested or invalid option '{k}={v}' (RFC 2347)")
                continue
            if k == "blksize":
                r, got = int(req[k]), int(v)
                expected = min(r, MAX_BLKSIZE, server_max_blksize)
                if not MIN_BLKSIZE <= got <= min(r, MAX_BLKSIZE) or got != expected:
                    errs.append(f"blksize: requested {r}, expected {expected}, OACK {got}")
            elif k == "timeout" and int(v) != int(req[k]):
                errs.append(f"timeout: requested {req[k]}, OACK {v} (must echo)")
            elif k == "tsize":
                exp_tsize = len(t.data) if op == "get" else int(req[k])
                if int(v) != exp_tsize:
                    errs.append(f"tsize: OACK {v} != expected {exp_tsize}")
        for k in valid_req:
            if k not in t.oack:
                errs.append(f"requested '{k}' missing from OACK")

    sizes = t.block_sizes
    if sizes:
        bad = [i + 1 for i, s in enumerate(sizes[:-1]) if s != t.blksize]
        if bad:
            errs.append(f"DATA blocks {bad[:5]} != negotiated blksize {t.blksize}")
        if sizes[-1] >= t.blksize:
            errs.append(f"final DATA block ({sizes[-1]}) not shorter than blksize {t.blksize}")

    if op == "get" and expected_size is not None:
        if bytes(t.data) != make_test_data(expected_size):
            errs.append(f"downloaded payload ({len(t.data)} B) does not match expected {expected_size} B pattern")

    return errs


def build_option_list(args, upload_len):
    opts = []
    if args.blksize is not None:
        opts.append(("blksize", str(args.blksize)))
    if args.tsize:
        opts.append(("tsize", "0" if args.op == "get" else str(upload_len)))
    if args.opt_timeout is not None:
        opts.append(("timeout", str(args.opt_timeout)))
    for raw in args.opt:
        k, v = raw.split("=", 1)
        opts.append((k, v))
    return opts


def main():
    ap = argparse.ArgumentParser(description="In-memory CLI TFTP client and option-negotiation verifier")
    ap.add_argument("host", help="TFTP server IP")
    ap.add_argument("-p", "--port", type=int, default=69, help="TFTP server port")
    ap.add_argument("op", choices=["get", "put"], help="'get' (RRQ) or 'put' (WRQ)")
    ap.add_argument("filename", help="remote file name")

    ap.add_argument("-s", "--size", type=int, default=None,
                    help="payload size in bytes (verifies downloaded pattern on 'get', generates upload pattern on 'put')")
    ap.add_argument("--blksize", type=int, help="request RFC 2348 blksize=<N>")
    ap.add_argument("--tsize", action="store_true", help="request RFC 2349 tsize")
    ap.add_argument("--timeout", dest="opt_timeout", type=int, help="request RFC 2349 timeout=<N>")
    ap.add_argument("-O", "--opt", action="append", default=[], metavar="KEY=VAL",
                    help="append raw option KEY=VAL (repeatable)")

    ap.add_argument("--server-max-blksize", type=int, default=MAX_BLKSIZE,
                    help="server's upper blksize clamp limit")
    ap.add_argument("--expect-error", type=int, default=None, metavar="CODE",
                    help="expect TFTP ERROR <CODE> from server")
    ap.add_argument("-t", "--socket-timeout", type=float, default=1.0, help="socket timeout [s]")
    ap.add_argument("-r", "--retries", type=int, default=3, help="max retries")
    args = ap.parse_args()

    upload_size = args.size if args.size is not None else 10000
    upload_data = make_test_data(upload_size) if args.op == "put" else b""
    options = build_option_list(args, len(upload_data))
    client = Client(args.host, args.port, args.socket_timeout, args.retries)

    try:
        t = (client.download(args.filename, options) if args.op == "get"
             else client.upload(args.filename, upload_data, options))
        errs = verify_transfer(args.op, t, options, args.size, args.server_max_blksize, args.expect_error)
    except (TftpError, OSError, ValueError, IndexError, struct.error) as e:
        errs = [f"exception: {e}"]

    status = "PASS" if not errs else "FAIL"
    print(f"[{status}] {args.op.upper()} {args.filename} opts={dict(options)}")
    for e in errs:
        print(f"  -> {e}")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
