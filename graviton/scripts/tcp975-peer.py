#!/usr/bin/env python3
"""tcp975-peer.py -- the Linux end of the issue 975 reproduction (tcp975.c).

Run as root on a Linux host the test clients can reach:

    python3 -I tcp975-peer.py <data-port> <control-port>

The data port accepts connections and reads (and counts) everything sent to it.
The control port takes one command per line from a client and answers one line:

    DROP <port>            black-hole the data connection from <port> of the
                           asking host, both directions, with iptables
    UNDROP <port>          lift that again
    COUNT <port>           "OK <bytes>" read so far from that connection
    STOPREAD <port> <s>    stop reading that connection for <s> seconds, so
                           its receive window closes while this end stays alive
    SILENTREOPEN <port> <pause> <hold>
                           stop reading for <pause> seconds, then read again
                           while dropping everything this end sends on that
                           connection for <hold> seconds: the window update
                           that reopens the window is lost (issue 1044)

Rules match one connection's 4-tuple, so the control connection and every
other test connection carry on untouched. Every rule added is removed again on
exit. Each command is logged with a UTC timestamp.
"""
import atexit
import signal
import socket
import subprocess
import sys
import threading
import time

data_port = int(sys.argv[1])
control_port = int(sys.argv[2])

lock = threading.Lock()
counts = {}        # (ip, port) -> bytes read
stop_until = {}    # (ip, port) -> monotonic time to resume reading
rules = set()      # (ip, port) currently dropped
rules_out = set()  # (ip, port) with only this end's segments dropped


def log(message):
    stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    print("%s %s" % (stamp, message), flush=True)


def iptables(action, ip, port):
    for chain, match in (("INPUT", ["-s", ip, "--sport", str(port),
                                    "--dport", str(data_port)]),
                         ("OUTPUT", ["-d", ip, "--dport", str(port),
                                     "--sport", str(data_port)])):
        subprocess.run(["iptables", action, chain, "-p", "tcp"] + match
                       + ["-j", "DROP"], check=True)


def cleanup():
    with lock:
        for ip, port in list(rules):
            iptables("-D", ip, port)
            rules.discard((ip, port))
            log("cleanup: UNDROP %s:%d" % (ip, port))
        for ip, port in list(rules_out):
            output_drop("-D", ip, port)
            rules_out.discard((ip, port))


atexit.register(cleanup)
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))


def serve_data(connection, peer):
    with lock:
        counts[peer] = 0
    log("data: connection from %s:%d" % peer)
    while True:
        with lock:
            pause = stop_until.get(peer, 0) - time.monotonic()
        if pause > 0:
            time.sleep(min(pause, 1.0))
            continue
        try:
            chunk = connection.recv(1 << 20)
        except OSError as error:
            log("data: %s:%d recv error %s" % (peer + (error,)))
            break
        if not chunk:
            log("data: %s:%d EOF after %d bytes" % (peer + (counts[peer],)))
            break
        with lock:
            counts[peer] += len(chunk)
    connection.close()


def output_drop(action, ip, port):
    subprocess.run(["iptables", action, "OUTPUT", "-p", "tcp", "-d", ip,
                    "--dport", str(port), "--sport", str(data_port),
                    "-j", "DROP"], check=True)


def silent_reopen(ip, port, pause, hold):
    # Start dropping just before reading resumes, so the window update that
    # reading sends is the first thing lost.
    time.sleep(max(pause - 0.5, 0))
    with lock:
        output_drop("-I", ip, port)
        rules_out.add((ip, port))
    log("silent-reopen: %s:%d outbound dropped, reading again" % (ip, port))
    time.sleep(hold + 0.5)
    with lock:
        output_drop("-D", ip, port)
        rules_out.discard((ip, port))
    log("silent-reopen: %s:%d outbound restored" % (ip, port))


def serve_control(connection, peer):
    ip = peer[0]
    reader = connection.makefile("r")
    for line in reader:
        words = line.split()
        reply = "ERR"
        try:
            if words[0] == "DROP":
                port = int(words[1])
                with lock:
                    iptables("-I", ip, port)
                    rules.add((ip, port))
                reply = "OK"
            elif words[0] == "UNDROP":
                port = int(words[1])
                with lock:
                    iptables("-D", ip, port)
                    rules.discard((ip, port))
                reply = "OK"
            elif words[0] == "COUNT":
                with lock:
                    reply = "OK %d" % counts.get((ip, int(words[1])), 0)
            elif words[0] == "SILENTREOPEN":
                port, pause, hold = int(words[1]), float(words[2]), float(words[3])
                with lock:
                    stop_until[(ip, port)] = time.monotonic() + pause
                threading.Thread(target=silent_reopen,
                                 args=(ip, port, pause, hold),
                                 daemon=True).start()
                reply = "OK"
            elif words[0] == "STOPREAD":
                with lock:
                    stop_until[(ip, int(words[1]))] = (time.monotonic()
                                                       + float(words[2]))
                reply = "OK"
        except (IndexError, ValueError, subprocess.CalledProcessError) as error:
            reply = "ERR %s" % error
        if words and words[0] != "COUNT":
            log("control %s: %s -> %s" % (ip, line.strip(), reply))
        connection.sendall((reply + "\n").encode())
    connection.close()


def listen(port, handler):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", port))
    server.listen(64)
    while True:
        connection, peer = server.accept()
        threading.Thread(target=handler, args=(connection, peer),
                         daemon=True).start()


threading.Thread(target=listen, args=(data_port, serve_data),
                 daemon=True).start()
log("listening: data %d, control %d" % (data_port, control_port))
listen(control_port, serve_control)
