#!/usr/bin/env python3
"""nettput-peer -- the Linux end of a nettput throughput run.

Deliberately dependency-free: the peer for these tests is the c7g.metal builder,
and installing anything there is a change to the machine that every other
experiment then has to trust. python3 is already present, so this is the whole
of it.

Protocol, driven entirely by the Haiku side so this end needs no arguments
beyond a port:

    client -> peer   16 bytes: b"NTPT" | mode | 3 pad | uint64 big-endian length
    mode 'T'         client transmits <length> bytes; peer reads them all and
                     replies with a single b"A" so the client can time the run
                     to the arrival of the last byte rather than to its own last
                     write returning
    mode 'R'         peer transmits <length> bytes; client reads them

Each connection is served in turn and the peer's own view of the transfer is
printed, which is worth having: when the two ends disagree about the rate, the
gap is the story. A large disagreement on a transmit run means bytes were
buffered and not yet delivered, which is exactly the error the acknowledgement
exists to prevent.

    nettput-peer.py [-p port] [-b bufsize] [--once]
"""

import argparse
import socket
import struct
import sys
import time

MAGIC = b"NTPT"
HEADER_SIZE = 16
MODE_TRANSMIT = ord("T")  # client transmits, we receive
MODE_RECEIVE = ord("R")  # we transmit, client receives

DEFAULT_PORT = 5301
DEFAULT_BUFFER = 64 * 1024


def read_exactly(sock, count):
    """Read exactly count bytes, or return what arrived before EOF."""
    chunks = []
    remaining = count
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            break
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def human_rate(byte_count, seconds):
    if seconds <= 0:
        return "instant (unmeasurable)"
    megabits = (byte_count * 8) / 1e6
    mebibytes = byte_count / (1024 * 1024)
    return "%.1f Mbit/s (%.1f MiB/s)" % (megabits / seconds, mebibytes / seconds)


def serve_transmit(sock, length, buffer_size):
    """The client is sending; drain the stream and acknowledge the last byte."""
    received = 0
    start = time.monotonic()
    while received < length:
        chunk = sock.recv(min(buffer_size, length - received))
        if not chunk:
            break
        received += len(chunk)
    elapsed = time.monotonic() - start

    if received != length:
        print(
            "  peer: short receive, %d of %d bytes" % (received, length),
            file=sys.stderr,
        )

    # Only acknowledge a complete transfer. Acknowledging a short one would
    # hand the client a plausible-looking result for a run that did not finish.
    if received == length:
        sock.sendall(b"A")

    print("  peer received %d bytes in %.3f s -- %s"
          % (received, elapsed, human_rate(received, elapsed)))


def serve_receive(sock, length, buffer_size):
    """The client wants to receive; blast length bytes at it."""
    payload = bytes(bytearray(i & 0xFF for i in range(buffer_size)))

    sent = 0
    start = time.monotonic()
    try:
        while sent < length:
            chunk = min(buffer_size, length - sent)
            sock.sendall(payload[:chunk] if chunk != buffer_size else payload)
            sent += chunk
    except (BrokenPipeError, ConnectionResetError) as error:
        print("  peer: client went away after %d bytes (%s)" % (sent, error),
              file=sys.stderr)
    elapsed = time.monotonic() - start

    print("  peer sent %d bytes in %.3f s -- %s"
          % (sent, elapsed, human_rate(sent, elapsed)))


def serve_one(listener, buffer_size):
    sock, address = listener.accept()
    print("connection from %s:%d" % address, flush=True)
    try:
        header = read_exactly(sock, HEADER_SIZE)
        if len(header) != HEADER_SIZE or header[:4] != MAGIC:
            print("  peer: not a nettput client (bad header), dropping",
                  file=sys.stderr)
            return
        mode = header[4]
        (length,) = struct.unpack(">Q", header[8:16])
        print("  request: mode %r, %d bytes" % (chr(mode), length), flush=True)

        if mode == MODE_TRANSMIT:
            serve_transmit(sock, length, buffer_size)
        elif mode == MODE_RECEIVE:
            serve_receive(sock, length, buffer_size)
        else:
            print("  peer: unknown mode %r" % chr(mode), file=sys.stderr)
    finally:
        sock.close()
        sys.stdout.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("-b", "--bufsize", type=int, default=DEFAULT_BUFFER)
    parser.add_argument("--once", action="store_true",
                        help="serve a single connection and exit")
    args = parser.parse_args()

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("0.0.0.0", args.port))
    listener.listen(4)
    print("nettput-peer listening on 0.0.0.0:%d" % args.port, flush=True)

    try:
        while True:
            serve_one(listener, args.bufsize)
            if args.once:
                break
    except KeyboardInterrupt:
        print("\nnettput-peer stopping")
    finally:
        listener.close()


if __name__ == "__main__":
    main()
