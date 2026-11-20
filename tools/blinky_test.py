#!/usr/bin/env python3
"""
Test the VSCP Blinky demo node over the network.

  blinky_test.py --host 192.168.1.50 link           VSCP link protocol (TCP/IP)
  blinky_test.py --host 192.168.1.50 udp            VSCP frames over UDP
  blinky_test.py --host 192.168.1.50 multicast      VSCP frames on the multicast group
  blinky_test.py --broker 192.168.1.9 mqtt          VSCP JSON over MQTT (mosquitto_pub/sub)
  blinky_test.py --host ... --broker ... all

Only the Python standard library is used (MQTT uses the mosquitto clients).
The tests change registers (zone, LED, decision matrix row 3) and put them
back afterwards.
"""

import argparse
import binascii
import json
import socket
import struct
import subprocess
import sys
import time

CLASS1_PROTOCOL = 0
READ_REGISTER, RW_RESPONSE, WRITE_REGISTER = 9, 10, 11
CLASS1_INFORMATION, INFO_ON, INFO_OFF = 20, 3, 4
CLASS1_CONTROL, CONTROL_TURNON = 30, 5

REG_ZONE, REG_STATUS, REG_CONTROL = 0, 2, 3
REG_COUNTER = 6
DM_ROW3 = 0x60 + 3 * 8

failures = 0


def check(name, ok, detail=""):
    global failures
    print("  %-50s %s %s" % (name, "ok" if ok else "FAILED", detail if not ok else ""))
    if not ok:
        failures += 1


# ---------------------------------------------------------------------------
#                              Transports
# ---------------------------------------------------------------------------


class LinkNode:
    """VSCP link protocol client."""

    def __init__(self, host, port, user, password, nickname):
        self.nickname = nickname
        self.s = socket.create_connection((host, port), 5)
        self.s.settimeout(0.2)
        self.buf = b""
        self.read_until_ok(3)
        assert self.cmd("user " + user).startswith("+OK"), "user"
        r = self.cmd("pass " + password)
        assert r.startswith("+OK"), "login failed: " + r

    def read_until_ok(self, timeout=2.0):
        end, lines = time.time() + timeout, []
        while time.time() < end:
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                line = line.decode(errors="replace").strip()
                if line:
                    lines.append(line)
                    if line.startswith(("+OK", "-OK")):
                        return lines
            try:
                data = self.s.recv(4096)
                if not data:
                    break
                self.buf += data
            except socket.timeout:
                pass
        return lines

    def cmd(self, c, timeout=2.0):
        self.s.sendall((c + "\r\n").encode())
        return "\n".join(self.read_until_ok(timeout))

    def send(self, vclass, vtype, data):
        d = "".join(",0x%02x" % b for b in data)
        r = self.cmd("send 0,%d,%d,0,,0,-%s" % (vclass, vtype, d))
        assert "+OK" in r, r

    def events(self, wait, until=None):
        got, end = [], time.time() + wait
        while time.time() < end and not (until and any(until(e) for e in got)):
            lines = self.cmd("retr 10", 1.0).split("\n")
            evs = [parse_link_event(l) for l in lines if l and not l.startswith(("+OK", "-OK"))]
            got += [e for e in evs if e]
            if not evs:
                time.sleep(0.05)
        return got

    def close(self):
        try:
            self.s.sendall(b"quit\r\n")
            time.sleep(0.1)
        finally:
            self.s.close()


def parse_link_event(line):
    p = line.split(",")
    if len(p) < 7:
        return None
    return {"class": int(p[1]), "type": int(p[2]), "guid": p[6], "data": [int(x, 0) for x in p[7:] if x]}


def make_frame(vclass, vtype, data=b"", guid=bytes(16)):
    """VSCP frame, version 0, not encrypted."""
    body = struct.pack(">HI", 0, 0) + bytes(7) + struct.pack(">HH", vclass, vtype) + guid
    body += struct.pack(">H", len(data)) + bytes(data)
    return b"\x00" + body + struct.pack(">H", binascii.crc_hqx(body, 0xFFFF))


def parse_frame(frame):
    if len(frame) < 38 or binascii.crc_hqx(frame[1:], 0xFFFF) != 0:
        return None
    vclass, vtype = struct.unpack(">HH", frame[14:18])
    size = struct.unpack(">H", frame[34:36])[0]
    return {"class": vclass, "type": vtype, "guid": frame[18:34].hex(":").upper(), "data": list(frame[36:36 + size])}


class FrameNode:
    """VSCP frames over UDP (to the node) or on the multicast group."""

    def __init__(self, host, port, nickname, group=None):
        self.nickname = nickname
        self.to = (group or host, port)
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.s.bind(("", port))
        if group:
            mreq = struct.pack("4s4s", socket.inet_aton(group), socket.inet_aton("0.0.0.0"))
            self.s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
            self.s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
            self.s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 0)
        self.s.settimeout(0.1)

    def send(self, vclass, vtype, data):
        self.s.sendto(make_frame(vclass, vtype, bytes(data)), self.to)

    def send_raw(self, frame):
        self.s.sendto(frame, self.to)

    def events(self, wait, until=None):
        got, end = [], time.time() + wait
        while time.time() < end and not (until and any(until(e) for e in got)):
            try:
                e = parse_frame(self.s.recv(2048))
                if e and e["guid"] != ":".join(["00"] * 16):  # Not our own frames
                    got.append(e)
            except socket.timeout:
                pass
        return got

    def close(self):
        self.s.close()


class MqttNode:
    """VSCP JSON events over MQTT with mosquitto_pub/mosquitto_sub."""

    def __init__(self, broker, port, guid, nickname, user=None, password=None):
        self.nickname = nickname
        self.auth = (["-u", user] if user else []) + (["-P", password] if password else [])
        self.broker = ["-h", broker, "-p", str(port)]
        self.in_topic = "vscp/%s/in" % guid
        self.sub = subprocess.Popen(
            ["mosquitto_sub"] + self.broker + self.auth + ["-t", "vscp/%s/#" % guid, "-v"],
            stdout=subprocess.PIPE, text=True)
        import os
        os.set_blocking(self.sub.stdout.fileno(), False)
        time.sleep(1.5)  # Let mosquitto_sub subscribe
        self.events(0.5)  # Skip retained messages

    def send(self, vclass, vtype, data):
        ev = {"vscpHead": 0, "vscpClass": vclass, "vscpType": vtype, "vscpGuid": "-",
              "vscpObId": 0, "vscpTimeStamp": 0, "vscpData": list(data)}
        subprocess.run(["mosquitto_pub"] + self.broker + self.auth + ["-t", self.in_topic, "-m", json.dumps(ev)],
                       check=True)

    def events(self, wait, until=None):
        got, end = [], time.time() + wait
        while time.time() < end and not (until and any(until(e) for e in got)):
            line = self.sub.stdout.readline()
            if not line:
                time.sleep(0.05)
                continue
            topic, _, payload = line.strip().partition(" ")
            if topic.endswith("/in") or not payload.startswith("{"):
                continue
            try:
                j = json.loads(payload)
                # VSCP JSON (vscpClass, ...) or the short keys of the firmware helper (class, ...)
                got.append({"class": j.get("vscpClass", j.get("class")), "type": j.get("vscpType", j.get("type")),
                            "guid": j.get("vscpGuid", j.get("guid", "")), "data": j.get("vscpData", j.get("data", [])),
                            "topic": topic})
            except (ValueError, KeyError):
                pass
        return got

    def close(self):
        self.sub.terminate()


# ---------------------------------------------------------------------------
#                                 Tests
# ---------------------------------------------------------------------------


def find(events, vclass, vtype, first=None):
    for e in events:
        if e["class"] == vclass and e["type"] == vtype and (first is None or e["data"][:1] == [first]):
            return e
    return None


def response(reg):
    return lambda e: e["class"] == CLASS1_PROTOCOL and e["type"] == RW_RESPONSE and e["data"][:1] == [reg]


def read_reg(node, reg):
    node.send(CLASS1_PROTOCOL, READ_REGISTER, [node.nickname, reg])
    e = find(node.events(1.5, response(reg)), CLASS1_PROTOCOL, RW_RESPONSE, reg)
    return e["data"][1] if e else None


def read_counter(node):
    c = [read_reg(node, REG_COUNTER + i) for i in range(4)]
    return None if None in c else (c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3]


def write_reg(node, reg, val, wait=1.5, events=False):
    """Write a register. With events, wait the whole time for other events too."""
    node.send(CLASS1_PROTOCOL, WRITE_REGISTER, [node.nickname, reg, val])
    evs = node.events(wait, None if events else response(reg))
    e = find(evs, CLASS1_PROTOCOL, RW_RESPONSE, reg)
    return (e["data"][1] if e else None), evs


def basic_tests(node, full):
    control = read_reg(node, REG_CONTROL)
    check("read control register", control is not None)
    if control is None:
        return
    zone = read_reg(node, REG_ZONE)
    v, _ = write_reg(node, REG_ZONE, 0x5A)
    check("write zone register", v == 0x5A, "got %r" % v)
    write_reg(node, REG_ZONE, zone)

    if not full:
        return

    # LED: needs control bit 7. Stop blinking and turn it on with a status write.
    write_reg(node, REG_CONTROL, control | 0x80)
    write_reg(node, REG_STATUS, 0)
    _, evs = write_reg(node, REG_STATUS, 0x02, events=True)
    status = read_reg(node, REG_STATUS)
    check("status write turns the LED on", status is not None and status & 0x02, "status %r" % status)
    check("CLASS1.INFORMATION ON sent", find(evs, CLASS1_INFORMATION, INFO_ON) is not None)
    _, evs = write_reg(node, REG_STATUS, 0, events=True)
    check("CLASS1.INFORMATION OFF sent", find(evs, CLASS1_INFORMATION, INFO_OFF) is not None)

    # Decision matrix row 3: CLASS1.CONTROL TURN-ON -> action 2 (LED on)
    saved = [read_reg(node, DM_ROW3 + i) for i in range(8)]
    for i, b in enumerate([0x00, 0x80, 0xFF, CLASS1_CONTROL, 0xFF, CONTROL_TURNON, 2, 0]):
        write_reg(node, DM_ROW3 + i, b, 0.6)
    node.send(CLASS1_CONTROL, CONTROL_TURNON, [0, 0, 0])
    evs = node.events(1.5)
    status = read_reg(node, REG_STATUS)
    check("decision matrix: TURN-ON turns the LED on", status is not None and status & 0x02, "status %r" % status)
    check("decision matrix: ON event sent", find(evs, CLASS1_INFORMATION, INFO_ON) is not None)
    write_reg(node, REG_STATUS, 0)
    for i, b in enumerate(saved):
        write_reg(node, DM_ROW3 + i, b if b is not None else 0, 0.6)

    # Millisecond counter
    write_reg(node, REG_CONTROL, control | 0x81)
    write_reg(node, REG_COUNTER, 0)
    a = read_counter(node)
    t = time.time()
    time.sleep(1.0)
    b = read_counter(node)
    elapsed = (time.time() - t) * 1000
    ms = None if None in (a, b) else b - a
    check("millisecond counter runs", ms is not None and abs(ms - elapsed) < 400,
          "counted %r ms in %.0f ms" % (ms, elapsed))
    write_reg(node, REG_COUNTER, 0)
    check("counter write clears it", (read_counter(node) or 0) < 1000)
    write_reg(node, REG_CONTROL, control)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=["link", "udp", "multicast", "mqtt", "all"])
    ap.add_argument("--host", help="IP address of the node")
    ap.add_argument("--port", type=int, default=9598)
    ap.add_argument("--user", default="vscp")
    ap.add_argument("--password", default="secret")
    ap.add_argument("--nickname", type=lambda s: int(s, 0), default=0x10)
    ap.add_argument("--udp-port", type=int, default=33333)
    ap.add_argument("--group", default="224.0.23.158")
    ap.add_argument("--mcast-port", type=int, default=44444)
    ap.add_argument("--broker", help="MQTT broker")
    ap.add_argument("--broker-port", type=int, default=1883)
    ap.add_argument("--guid", help="GUID of the node (for MQTT; read over the link if not given)")
    args = ap.parse_args()

    what = ["link", "udp", "multicast", "mqtt"] if args.what == "all" else [args.what]
    guid = args.guid

    if "link" in what or (("mqtt" in what) and not guid):
        print("VSCP link protocol, %s:%d" % (args.host, args.port))
        node = LinkNode(args.host, args.port, args.user, args.password, args.nickname)
        r = node.cmd("ggid")
        guid = guid or r.split("\n")[0].strip()
        check("GUID " + guid, guid.startswith("FF:FF:FF:FF:FF:FF:FF:FE"), r)
        if "link" in what:
            check("wcyd", "+OK" in node.cmd("wcyd"))
            basic_tests(node, True)
        node.close()

    if "udp" in what:
        print("UDP, %s:%d" % (args.host, args.udp_port))
        node = FrameNode(args.host, args.udp_port, args.nickname)
        basic_tests(node, False)
        bad = bytearray(make_frame(CLASS1_PROTOCOL, READ_REGISTER, bytes([args.nickname, REG_CONTROL])))
        bad[-1] ^= 0xFF
        node.send_raw(bytes(bad))
        check("frame with a bad CRC is ignored", find(node.events(1.5), CLASS1_PROTOCOL, RW_RESPONSE) is None)
        node.close()

    if "multicast" in what:
        print("Multicast, %s:%d" % (args.group, args.mcast_port))
        node = FrameNode(None, args.mcast_port, args.nickname, args.group)
        basic_tests(node, False)
        node.close()

    if "mqtt" in what:
        print("MQTT, %s:%d" % (args.broker, args.broker_port))
        node = MqttNode(args.broker, args.broker_port, guid, args.nickname)
        basic_tests(node, False)
        node.send(CLASS1_PROTOCOL, READ_REGISTER, [args.nickname, REG_CONTROL])
        evs = node.events(1.5)
        e = find(evs, CLASS1_PROTOCOL, RW_RESPONSE)
        check("published on vscp/<guid>/0/10", e is not None and e["topic"] == "vscp/%s/0/10" % guid,
              e and e["topic"])
        node.close()

    print("All tests passed" if not failures else "%d test(s) FAILED" % failures)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
