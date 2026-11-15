# Status, scanning and implementation details

## JSON tuning/status output

`unicable-tune --json` writes one JSON object per line to stdout. Human diagnostics and errors go to stderr. The `tuning` event contains the selected adapter/transponder/user band, calculated LNB band/LO, EN50494 bytes and compensated tuner frequency.

Example status (values are illustrative):

```json
{"event":"status","adapter":0,"elapsed_seconds":4,"status":31,"lock":true,"cnr_db":10.8,"strength_dbm":-43.2,"strength_percent":null,"quality_percent":null}
```

The driver determines which metrics are available:

- `FE_SCALE_DECIBEL` values are divided by 1000 and reported as `cnr_db` / `strength_dbm`.
- `FE_SCALE_RELATIVE` values in 0–65535 produce a percentage in 0–100.
- Missing, unsupported or invalid metrics are `null`. Percentages are not estimated from dB values.

A `stable_lock` event is emitted once after three consecutive locked samples. It means a stable lock has been observed, not that lock will remain present forever. Continue monitoring `status.lock`; status is sampled about once per second. The final `result` reports whether stable lock was ever observed, locked-sample count and lock-loss count.

## Tuning exit codes

| Code | Meaning |
| --- | --- |
| 0 | Dry-run/help succeeded, or termination by SIGINT/SIGTERM, or stable lock was observed during a finite run. |
| 1 | Device, ioctl or another runtime error. |
| 2 | Invalid options or frequency combination. |
| 3 | A finite run ended without observing stable lock. |

A signalled process can exit 0 without ever obtaining lock. A finite run that obtained lock and subsequently lost it can also exit 0. For ongoing health, use live status rather than only the exit code or the first `stable_lock` event.

## EN50494 calculation

All frequency arithmetic is integer kHz. For a universal Ku LNB:

```text
LO = RF < 11700 MHz ? 9750 MHz : 10600 MHz
IF = RF - LO
word = floor((IF_kHz + UB_kHz + 2000) / 4000) - 350
tuner_kHz = (word + 350) * 4000 - IF_kHz
```

The command starts with `e0 10 5a`; its remaining bits encode user-band index (`slot - 1`), satellite position, polarization, low/high band and the 10-bit tuning word. Both the command word and compensated tuner frequency are range-checked.

The tested example RF12654 H, UB3 / 1680 MHz / A generates `e0 10 5a 4e 48` and tunes the frontend to 1682000 kHz. This compensation is intentional, not an error in the slot-frequency setting.

The hardware sequence clears/sets the delivery system, turns continuous tone off, supplies 13 V, raises voltage to 18 V for the EN50494 command, returns to 13 V, then sets the DVBv5 tuning properties. Only the requested user band is programmed. The helper keeps the frontend open until the configured duration or termination signal.

## Scanner interface

```sh
dvb-scan --list
dvb-scan --scan --adapter 0 --scan-seconds 12
dvb-scan --input capture.ts --scan-seconds 12
```

Discovery opens `frontend0` read-only and reports a JSON array with adapter number, path, availability, frontend name, capability flags and delivery systems. An unavailable device has an `error` field. No `/dev/dvb` directory produces an empty array.

A live scan requires an already tuned frontend. It opens `demux0` with a private all-TS `DMX_OUT_TSDEMUX_TAP` filter and reads that same descriptor. It does not open/retune the frontend or modify voltage. Unsupported private tap is an explicit error; there is no fallback that could consume another program's shared DVR stream.

A scan lasts up to 1–60 seconds (default 12). File input may finish earlier at EOF. Limits include 512 services, 32 CAIDs per service and 4096-byte sections. PAT, actual-transport SDT and PMT sections are CRC-checked; broken continuity discards incomplete sections. Services must appear in a valid PAT. PMT changes replace prior CA information rather than accumulating stale descriptors.

Each service includes `sid`, `name`, `provider`, `scrambled`, `caids`, `pmt_pid`, `pmt_seen` and `sdt_seen`. CAIDs are four-digit uppercase hex strings without `0x`. A missing name is an empty string. Acquisition flags allow the caller to distinguish incomplete tables from a known result. DVB text is converted to UTF-8 and serialized as valid escaped JSON.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| No adapters listed | Kernel driver, firmware and `/dev/dvb`; this project supplies no driver. |
| Permission denied | Ownership/permissions of the selected frontend/demux/DVR and the running user's group membership. |
| No lock | Transponder values, correct input/feed, assigned user band and frequency, position A/B, explicit modulation/FEC, driver support. |
| Lock disappears when DVBlast starts | Confirm `-f -`, matching adapter/frontend, and no competing tuner process or reused user band. |
| Reception stops after a minute | Use `--seconds 0` and keep the helper alive; the default finite run is 60 seconds. |
| Scanner reports private-tap ioctl error | The driver's demux must support `DMX_OUT_TSDEMUX_TAP`; use file input for offline inspection. |
| Scan returns incomplete names or CA status | Allow more scan time and check `pmt_seen`/`sdt_seen`, lock and table reception. |

## Primary references

- [Linux DVB frontend properties](https://www.kernel.org/doc/html/latest/userspace-api/media/dvb/fe_property_parameters.html)
- [Linux FE_GET_PROPERTY](https://www.kernel.org/doc/html/latest/userspace-api/media/dvb/fe-get-property.html)
- [Linux demux types](https://www.kernel.org/doc/html/latest/userspace-api/media/dvb/dmx_types.html)
- [VDR DiSEqC implementation](https://github.com/vdr-projects/vdr/blob/master/diseqc.c)
- [TBS STiD135 frontend implementation](https://github.com/tbsdtv/linux_media/blob/latest/drivers/media/dvb-frontends/stid135/stid135-fe.c)
- [DVBlast manual, including `-f -`](https://github.com/videolan/dvblast/blob/master/dvblast.1)
