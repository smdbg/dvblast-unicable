#!/usr/bin/env python3
"""Exercise EN50494 calculation/CLI without opening hardware (always --dry-run)."""
import json
from pathlib import Path
import subprocess
import sys

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else "./unicable-tune").resolve())


def run(*args):
    return subprocess.run([binary, "--dry-run", *map(str, args)], capture_output=True, text=True, timeout=2)


def settings(*args):
    result = run("--json", *args)
    assert result.returncode == 0, (args, result.stderr)
    value = json.loads(result.stdout)
    assert value["event"] == "tuning"
    return value


legacy = run()
assert legacy.returncode == 0
assert legacy.stdout.splitlines() == [
    "adapter7/frontend0: EN50494 UB3/index2 1680MHz position A; RF12654H SR30000000 DVB-S2 8PSK FEC5/6",
    "DiSEqC: e0 10 5a 4e 48; tuner=1682000kHz (Unicable rounding compensation)"]
assert settings()["fec"] == "5/6"
assert settings("--seconds", 0)["modulation"] == "8PSK"
assert settings("--adapter", 0)["adapter"] == 0
assert settings("--frequency", 12654)["fec"] == "auto"
assert settings("--delivery-system", "DVB-S")["modulation"] == "QPSK"
assert settings("--frequency", 12654, "--modulation", "8PSK", "--fec", "5/6", "--rolloff", "0.20")["fec"] == "5/6"
print("PASS legacy profile, zero duration, explicit overrides and general auto defaults")

# Golden reference vectors, then check each independent EN50494 bank/slot bit.
vectors = [
    (12654, 1680, "e0 10 5a 4e 48", 1682000, 10600),
    (11054, 1210, "e0 10 5a 49 17", 1212000, 9750),
    (11699, 1680, "e0 10 5a 4a 2d", 1679000, 9750),
    (11700, 1680, "e0 10 5a 4d 59", 1680000, 10600),
    (11054.125, 1210.5, "e0 10 5a 49 17", 1211875, 9750),
]
for frequency, ub, command, tuned, lo in vectors:
    value = settings("--frequency", frequency, "--slot-frequency", ub)
    assert value["diseqc"] == command, value
    assert value["tuner_khz"] == tuned and value["lo_mhz"] == lo, value
for slot in range(1, 9):
    for position in "AB":
        for polarization in "VH":
            for frequency, base in [(11054, 0x217), (12654, 0x248)]:
                # Use UB1680 for high band and UB1210 for low band.
                ub = 1210 if frequency == 11054 else 1680
                value = settings("--slot", slot, "--position", position, "--polarization", polarization,
                                 "--frequency", frequency, "--slot-frequency", ub)
                packet = bytes.fromhex(value["diseqc"])
                assert packet[:3] == bytes.fromhex("e0 10 5a")
                assert packet[3] >> 5 == slot - 1
                assert bool(packet[3] & 0x10) == (position == "B")
                assert bool(packet[3] & 0x08) == (polarization == "H")
                assert bool(packet[3] & 0x04) == (frequency >= 11700)
                assert ((packet[3] & 3) << 8 | packet[4]) == (0x117 if ub == 1210 else base)
print("PASS LO switch, rounding correction, fractional MHz, all 8 slots and A/B H/V bank bits")

invalid = [
    ("--seconds", v) for v in ["", "-1", "+1", " 1", "1x", "86401", "184467440737095516160"]
] + [("--adapter", "256"), ("--adapter", "-1"), ("--slot", "0"), ("--slot", "9"),
     ("--frequency", "10699.999"), ("--frequency", "12750.001"), ("--frequency", "12654."),
     ("--frequency", "1e4"), ("--frequency", "nan"), ("--frequency", "12654.0001"),
     ("--frequency", "9999999999999999999999999999999999999999999999999999"),
     ("--slot-frequency", "949.999"), ("--slot-frequency", "2150.001"),
     ("--symbol-rate", "30000"), ("--symbol-rate", "65000001"),
     ("--polarization", "R"), ("--position", "C"), ("--delivery-system", "DVB-T2"),
     ("--modulation", "bogus"), ("--fec", "0"), ("--rolloff", "0.10"),
     ("--delivery-system", "DVB-S", "--modulation", "8PSK"), ("--unknown", "value"), ("--slot",)]
for args in invalid:
    result = run(*args)
    assert result.returncode == 2, (args, result.returncode, result.stderr)
print("PASS strict numeric, range and incompatible-parameter validation")
