#!/usr/bin/env python3
"""Synthetic TS/PSI regression checks; never opens a DVB device or CAM server."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else "./dvb-scan").resolve())


def u16(n):
    return struct.pack(">H", n)


def crc(data):
    value = 0xFFFFFFFF
    for byte in data:
        value ^= byte << 24
        for _ in range(8):
            value = ((value << 1) ^ (0x04C11DB7 if value & 0x80000000 else 0)) & 0xFFFFFFFF
    return value


def section(table, extension, payload, version=0, number=0, last=0):
    body = u16(extension) + bytes([0xC1 | (version << 1), number, last]) + payload
    data = bytes([table]) + u16(0xB000 | (len(body) + 4)) + body
    return data + struct.pack(">I", crc(data))


def pat(services, version=0):
    return section(0, 7, b"".join(u16(sid) + u16(0xE000 | pid) for sid, pid in services), version)


def sdt(services, table=0x42):
    payload = u16(11) + b"\xff"
    for sid, name, provider, scrambled in services:
        desc = b"\x01" + bytes([len(provider)]) + provider + bytes([len(name)]) + name
        desc = b"\x48" + bytes([len(desc)]) + desc
        payload += u16(sid) + b"\xfc" + u16(0x8000 | (0x1000 if scrambled else 0) | len(desc)) + desc
    return section(table, 7, payload)


def ca(caid):
    return b"\x09\x04" + u16(caid) + u16(0xE000 | 2601)


def pmt(sid, program_ca=b"", elementary_ca=b"", version=0):
    stream = b"\x1b" + u16(0xE000 | 500) + u16(0xF000 | len(elementary_ca)) + elementary_ca
    return section(2, sid, u16(0xE000 | 500) + u16(0xF000 | len(program_ca)) + program_ca + stream, version)


def packet(pid, payload, cc=0, start=False):
    assert len(payload) <= 184
    return b"\x47" + u16(pid | (0x4000 if start else 0)) + bytes([0x10 | (cc & 15)]) + payload.ljust(184, b"\xff")


def packets(pid, data, cc=0):
    result = [packet(pid, b"\0" + data[:183], cc, True)]
    data = data[183:]
    while data:
        cc += 1
        result.append(packet(pid, data[:184], cc))
        data = data[184:]
    return result


def scan(data):
    with tempfile.NamedTemporaryFile(suffix=".ts") as fixture:
        fixture.write(data)
        fixture.flush()
        result = subprocess.run([binary, "--input", fixture.name], check=True, capture_output=True, timeout=5)
    return json.loads(result.stdout)


def join(parts):
    return b"".join(parts)


pat_bytes = join(packets(0, pat([(201, 100), (265, 101), (300, 102)])))
name = "Канал 201"
long_sdt = sdt([(201, b"\x15" + name.encode(), b"A" * 200, True), (265, b"Caf\xc2e", b"Provider", False)])
sdt_packets = packets(0x11, long_sdt)
assert len(sdt_packets) >= 2
bad_pmt = bytearray(pmt(300, ca(0xFFFF)))
bad_pmt[-1] ^= 1
stream = pat_bytes + join(sdt_packets) + join(packets(100, pmt(201, ca(0x09BD), ca(0x0B00) + ca(0x09BD))))
stream += join(packets(101, pmt(265))) + join(packets(102, bad_pmt))
found = {s["sid"]: s for s in scan(stream)}
assert sorted(found) == [201, 265, 300]
assert found[201]["name"] == name and found[201]["provider"] == "A" * 200
assert found[201]["caids"] == ["09BD", "0B00"] and found[201]["scrambled"]
assert found[265]["name"] == "Café" and not found[265]["scrambled"] and found[265]["pmt_seen"]
assert not found[300]["pmt_seen"] and found[300]["caids"] == []
print("PASS PAT/SDT/PMT, UTF-8/ISO6937, program+ES CAIDs, bad PMT CRC")

# Exact duplicate TS packets are harmless; a lost continuation is not assembled.
duplicate = pat_bytes + join([sdt_packets[0], sdt_packets[0]] + sdt_packets[1:])
assert scan(duplicate)[0]["name"] == name
broken = list(sdt_packets)
broken[1] = broken[1][:3] + bytes([(broken[1][3] & 0xF0) | ((broken[1][3] + 1) & 15)]) + broken[1][4:]
assert not scan(pat_bytes + join(broken))[0]["sdt_seen"]
print("PASS duplicate packets and continuity-loss rejection")

# PUSI pointer completes a prior section, then starts a second section in one TS.
first = sdt([(201, b"One", b"A" * 180, False)])
second = sdt([(265, b"Two", b"B", False)])
remaining = first[183:]
assert len(remaining) + len(second) + 1 <= 184
mixed = packet(0x11, b"\0" + first[:183], 0, True)
mixed += packet(0x11, bytes([len(remaining)]) + remaining + second, 1, True)
found = scan(pat_bytes + mixed)
assert [s["name"] for s in found[:2]] == ["One", "Two"]
print("PASS pointer-field continuation and multiple sections in a TS packet")

# Ignore CRC failures, SDT-other, malformed length, and recover at a new PUSI.
bad_pat = bytearray(pat([(201, 100)]))
bad_pat[-1] ^= 1
assert scan(join(packets(0, bad_pat))) == []
bad_sdt = bytearray(long_sdt)
bad_sdt[-1] ^= 1
assert not scan(pat_bytes + join(packets(0x11, bad_sdt)))[0]["sdt_seen"]
assert not scan(pat_bytes + join(packets(0x11, sdt([(201, b"Other", b"P", False)], 0x46))))[0]["sdt_seen"]
oversized = packet(0x11, b"\0\x42\xbf\xff", 0, True)
found = scan(pat_bytes + oversized + join(packets(0x11, sdt([(201, b"Recovered", b"P", False)]), 1)))
assert found[0]["name"] == "Recovered"
print("PASS MPEG CRC, actual-TS SDT filtering, section size bound and recovery")

# PMT version changes replace prior CAIDs, rather than accumulating stale systems.
found = scan(pat_bytes + join(packets(100, pmt(201, ca(0x09BD)))) + join(packets(100, pmt(201, version=1), 1)))
assert found[0]["pmt_seen"] and found[0]["caids"] == [] and not found[0]["scrambled"]
# Arbitrary malformed UTF-8 still produces valid JSON, with replacement characters.
found = scan(pat_bytes + join(packets(0x11, sdt([(201, b"\x15A\xffB", b"P", False)]))))
assert found[0]["name"] == "A\ufffdB"
# Memory/service count remains bounded even for an unusually large valid PAT.
many = scan(join(packets(0, pat([(i + 1, i + 100) for i in range(700)]))))
assert len(many) == 512
print("PASS PMT replacement, malformed text and service-count bound")

for args in [[], ["--scan"], ["--scan", "--adapter", "-1"], ["--scan", "--adapter", "4096"],
             ["--input", "missing", "--scan-seconds", "0"], ["--input", "missing", "--scan-seconds", "61"],
             ["--input", "missing", "--scan-seconds", "-1"], ["--input", "missing", "--scan-seconds", ""],
             ["--scan", "--adapter", "999999999999999999999999999"]]:
    result = subprocess.run([binary] + args, capture_output=True, timeout=5)
    assert result.returncode == 2, (args, result.returncode)
print("PASS CLI rejects invalid bounds before opening devices")
