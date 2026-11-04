#!/usr/bin/env python3
"""tcp975-peer.py -- the Linux end of the issue 975 reproduction (tcp975.c).

Run as root on a Linux host the test clients can reach:

    python3 -I tcp975-peer.py <data-port> <control-port>

The data port + 3 is a second data port whose connections get a fixed 64 KiB
receive buffer, so a window closed on it stays exactly closed (shutfin).

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
    MUTE <port> / UNMUTE <port>
                           drop / restore only what this end sends on that
                           connection
    DROPFIN <port>         drop that connection's segments carrying a FIN, as
                           a peer with no window for it would (Linux 6.x and
                           later takes a FIN into a zero window)
    INQ <port>             "OK <bytes>" received but not yet read
    RCVWND <port>          "OK <bytes>" of window this end still offers (ss)
    READ <port> <n>        read <n> bytes now, even while STOPREAD holds
    INJECT <port> <kind>   send, from outside this end's stack, crafted copies
                           of the last pure ACK this end sent on that
                           connection (needs scapy):
                             dup3      three unchanged copies -- duplicates
                             win3      three, each with a different window --
                                       window updates, not duplicates
                             stalezero one acknowledging 1000 bytes less, with
                                       a zero window -- an old segment
                             submss    one with window field 1 -- a window
                                       less than a segment wide

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
sockets = {}       # (ip, port) -> the accepted data socket
last_ack = {}      # (ip, port) -> scapy copy of the last pure ACK we sent
stop_until = {}    # (ip, port) -> monotonic time to resume reading
rules = set()      # (ip, port) currently dropped
rules_out = set()  # (ip, port) with only this end's segments dropped
rules_fin = set()  # (ip, port) whose FINs are dropped


def log(message):
    stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    print("%s %s" % (stamp, message), flush=True)


def server_port(ip, port):
    data = sockets.get((ip, port))
    return data.getsockname()[1] if data is not None else data_port


def iptables(action, ip, port):
    local = server_port(ip, port)
    for chain, match in (("INPUT", ["-s", ip, "--sport", str(port),
                                    "--dport", str(local)]),
                         ("OUTPUT", ["-d", ip, "--dport", str(port),
                                     "--sport", str(local)])):
        subprocess.run(["iptables", action, chain, "-p", "tcp"] + match
                       + ["-j", "DROP"], check=True)


def cleanup():
    with lock:
        for ip, port in list(rules):
            iptables("-D", ip, port)
            rules.discard((ip, port))
            log("cleanup: UNDROP %s:%d" % (ip, port))
        for ip, port in list(rules_fin):
            fin_drop("-D", ip, port)
            rules_fin.discard((ip, port))
        for ip, port in list(rules_out):
            output_drop("-D", ip, port)
            rules_out.discard((ip, port))


atexit.register(cleanup)
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))


def track_acks():
    # Record the newest pure ACK this end sends on each data connection, so
    # INJECT can forge one with the right sequence, acknowledgement and
    # timestamps. Sniffed after this end's own stack, before the wire.
    try:
        from scapy.all import sniff, TCP
    except ImportError:
        log("tracker: no scapy, INJECT unavailable")
        return

    def record(packet):
        if TCP in packet and len(packet[TCP].payload) == 0:
            with lock:
                last_ack[(packet["IP"].dst, packet[TCP].dport)] = packet

    sniff(filter="(tcp src port %d or tcp src port %d) and less 200"
          % (data_port, data_port + 3), prn=record, store=False)


macs = {}
l2socket = None


def resolve_mac(ip):
    # Ahead of time, so an INJECT is not held up by ARP.
    try:
        from scapy.all import getmacbyip
        macs[ip] = getmacbyip(ip)
    except ImportError:
        pass


def inject(ip, port, kind):
    from scapy.all import IP, TCP, Ether, getmacbyip, sendp
    with lock:
        packet = last_ack.get((ip, port))
    if packet is None:
        return "ERR no ACK seen"
    base = packet[IP].copy()
    del base.chksum
    del base[TCP].chksum
    copies = []
    if kind == "dup3":
        copies = [base.copy() for _ in range(3)]
    elif kind == "win3":
        for delta in (1, 2, 3):
            copy = base.copy()
            copy[TCP].window = (base[TCP].window + delta) & 0xffff
            copies.append(copy)
    elif kind == "stalezero":
        copy = base.copy()
        copy[TCP].ack = (base[TCP].ack - 1000) & 0xffffffff
        copy[TCP].window = 0
        copies.append(copy)
    elif kind == "submss":
        copy = base.copy()
        copy[TCP].window = 1
        copies.append(copy)
    else:
        return "ERR kind"
    global l2socket
    mac = macs.get(ip) or getmacbyip(ip)
    if l2socket is None:
        from scapy.all import conf
        l2socket = conf.L2socket(iface="ens5")
    for copy in copies:
        # Layer 2, so this end's own iptables rules (MUTE, DROP) do not
        # apply to it; through one open socket, so the three go out within
        # milliseconds, well before the client's retransmission timeout.
        l2socket.send(Ether(dst=mac) / copy)
        time.sleep(0.002)
    return "OK seq %d ack %d win %s" % (base[TCP].seq, copies[0][TCP].ack,
                                        ",".join(str(c[TCP].window)
                                                 for c in copies))


def serve_data(connection, peer):
    with lock:
        counts[peer] = 0
        sockets[peer] = connection
    if peer[0] not in macs:
        threading.Thread(target=resolve_mac, args=(peer[0],),
                         daemon=True).start()
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
                    "--dport", str(port), "--sport", str(server_port(ip, port)),
                    "-j", "DROP"], check=True)


def fin_drop(action, ip, port):
    subprocess.run(["iptables", action, "INPUT", "-p", "tcp", "-s", ip,
                    "--sport", str(port), "--dport", str(server_port(ip, port)),
                    "--tcp-flags", "FIN", "FIN", "-j", "DROP"], check=True)


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
            elif words[0] in ("MUTE", "UNMUTE"):
                port = int(words[1])
                with lock:
                    output_drop("-I" if words[0] == "MUTE" else "-D", ip, port)
                    if words[0] == "MUTE":
                        rules_out.add((ip, port))
                    else:
                        rules_out.discard((ip, port))
                reply = "OK"
            elif words[0] == "DROPFIN":
                port = int(words[1])
                with lock:
                    fin_drop("-I", ip, port)
                    rules_fin.add((ip, port))
                reply = "OK"
            elif words[0] == "RCVWND":
                # The window field of the last ACK this end sent, scaled by
                # this end's receive window scale (ss "wscale:snd,rcv"): with
                # everything acknowledged, what it still offers.
                port = int(words[1])
                out = subprocess.run(["ss", "-tinH", "state", "established",
                    "( sport = :%d and dport = :%d )" % (server_port(ip, port),
                                                         port)],
                    capture_output=True, text=True).stdout
                import re
                scale = re.search(r"wscale:\d+,(\d+)", out)
                with lock:
                    packet = last_ack.get((ip, port))
                if scale is None or packet is None:
                    reply = "OK -1"
                else:
                    from scapy.all import TCP
                    reply = "OK %d" % (packet[TCP].window << int(scale.group(1)))
            elif words[0] == "INQ":
                import fcntl, struct, termios
                with lock:
                    data = sockets[(ip, int(words[1]))]
                queued = struct.unpack("i", fcntl.ioctl(
                    data.fileno(), termios.FIONREAD, b"\0" * 4))[0]
                reply = "OK %d" % queued
            elif words[0] == "READ":
                key, left = (ip, int(words[1])), int(words[2])
                with lock:
                    data = sockets[key]
                while left > 0:
                    chunk = data.recv(min(left, 1 << 20))
                    if not chunk:
                        break
                    left -= len(chunk)
                    with lock:
                        counts[key] += len(chunk)
                reply = "OK"
            elif words[0] == "INJECT":
                reply = inject(ip, int(words[1]), words[2])
            elif words[0] == "STOPREAD":
                with lock:
                    stop_until[(ip, int(words[1]))] = (time.monotonic()
                                                       + float(words[2]))
                reply = "OK"
        except (IndexError, ValueError, KeyError, OSError,
                subprocess.CalledProcessError) as error:
            reply = "ERR %s" % error
        if words and words[0] not in ("COUNT", "INQ", "RCVWND"):
            log("control %s: %s -> %s" % (ip, line.strip(), reply))
        connection.sendall((reply + "\n").encode())
    connection.close()


def listen(port, handler, receive_buffer=0):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if receive_buffer:
        # Fixed, and so no autotuning: inherited by every accepted socket.
        server.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
    server.bind(("0.0.0.0", port))
    server.listen(64)
    while True:
        connection, peer = server.accept()
        threading.Thread(target=handler, args=(connection, peer),
                         daemon=True).start()


threading.Thread(target=listen, args=(data_port, serve_data),
                 daemon=True).start()
threading.Thread(target=listen, args=(data_port + 3, serve_data, 65536),
                 daemon=True).start()
threading.Thread(target=track_acks, daemon=True).start()
log("listening: data %d, control %d" % (data_port, control_port))
listen(control_port, serve_control)
