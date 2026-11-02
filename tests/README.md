# TFTP Test Suite

The test suite verifies `tftp::Server`, `tftp::Client`, and `tftp::Packet` (RFC 1350, 2347, 2348, 2349) using GoogleTest and two standalone Python 3 reference scripts in `tools/`. All tests run completely in memory without creating or reading files on disk.

## Structure

| Target / File | Purpose |
| :--- | :--- |
| `tftp_unit_tests` (`tests/test_packet.cpp`) | Packet serialization/parsing unit tests and a C++ `Client` <-> `Server` in-memory loopback transfer. |
| `tftp_server_tests` (`tests/test_server.cpp`) | Starts `tftp::Server` in memory on an ephemeral port and exercises it with `tools/tftp_test_client.py`. |
| `tftp_client_tests` (`tests/test_client.cpp`) | Starts `tools/tftp_test_server.py` in memory on an ephemeral port and exercises `tftp::Client` against it. |
| `tests/test_utils.h` | Shared in-memory helpers (`makeTestData`, `runProcess`, `BackgroundProcess`). |

## Building & Running

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

To disable building tests when embedding the library, pass `-DTFTP_BUILD_TESTS=OFF`.

## Python Reference Tools (`tools/`)

Both scripts use only the Python 3 standard library, operate entirely in memory using the shared deterministic `make_test_data(size)` byte pattern, and can be run directly from the CLI.

### `tools/tftp_test_client.py` (tests a TFTP server)

Performs a single `get` (RRQ) or `put` (WRQ) transfer in memory and verifies RFC 2347/2348/2349 compliance (expected `OACK`, no unrequested options, `blksize` clamping and `DATA` packet framing, `tsize`, `timeout`, and payload bytes).

```bash
# Download 50000 bytes without options and verify in-memory pattern
./tools/tftp_test_client.py 127.0.0.1 -p 6969 get test.bin --size 50000

# Download with blksize, tsize, and timeout negotiation
./tools/tftp_test_client.py 127.0.0.1 -p 6969 get test.bin --size 50000 --blksize 1428 --tsize --timeout 3

# Send custom/raw options (e.g. uppercase or unknown options)
./tools/tftp_test_client.py 127.0.0.1 -p 6969 get test.bin --size 50000 -O BLKSIZE=1024 -O foobar=1

# Expect a specific TFTP error code from the server
./tools/tftp_test_client.py 127.0.0.1 -p 6969 get missing.bin --expect-error 1

# Upload 10000 in-memory bytes with blksize and tsize
./tools/tftp_test_client.py 127.0.0.1 -p 6969 put upload.bin --size 10000 --blksize 1024 --tsize
```

### `tools/tftp_test_server.py` (tests a TFTP client)

Serves an in-memory file (`-f sample.bin -s 20000`) on RRQ, receives uploaded payloads in memory on WRQ (`"data_valid": true/false`), prints `READY <port>` once bound, and prints `SESSION <json>` to stdout after each transfer.

```bash
# Serve an in-memory 20000-byte sample.bin on an ephemeral port for one transfer
./tools/tftp_test_server.py -p 0 -f sample.bin -s 20000 --once

# Fault / behavior injection flags
./tools/tftp_test_server.py -p 0 --once --max-blksize 1024
./tools/tftp_test_server.py -p 0 --once --ignore-options
./tools/tftp_test_server.py -p 0 --once --bad-oack
./tools/tftp_test_server.py -p 0 --once --drop 2
```
