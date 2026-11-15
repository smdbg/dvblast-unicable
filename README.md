# DVBlast Unicable

Standalone Linux tools for using **Unicable I / EN50494** satellite equipment with [DVBlast](https://www.videolan.org/projects/dvblast.html).

`unicable-tune` programs one user band, tunes a DVB-S/S2 frontend and keeps it open while DVBlast receives the transport stream in external-tuning mode (`-f -`). The companion `dvb-scan` discovers adapters and lists services from an already tuned stream.

These are ordinary C command-line programs. No headend application, tsdecrypt, CAS server or container is required. DVBlast is installed separately.

## What is included

- **`unicable-tune`** — configurable adapter, transponder, EN50494 user band and signal monitoring.
- **`dvb-scan`** — frontend discovery and bounded PAT/PMT/SDT service scanning, with JSON output.
- **`lock-adapter7`** — compatibility symlink to `unicable-tune`, for scripts using the original helper name.

The hardware-facing code was extracted unchanged from a working TBS6909x / STiD135 headend deployment. It accepts other adapter numbers and uses the Linux DVB API, but compatibility with every frontend/LNB combination has not been established.

## Build and install

Requirements: Linux, a C compiler, Make, Linux DVB userspace headers and libc with `iconv`. Debian/Ubuntu with glibc is the primary target. Python 3 is needed only for tests.

```sh
sudo apt-get update
sudo apt-get install build-essential linux-libc-dev python3 dvblast

git clone https://github.com/smdbg/dvblast-unicable.git
cd dvblast-unicable
make -j2
make check
sudo make install
```

Programs are installed in `/usr/local/bin`. `PREFIX`, `DESTDIR`, `CC`, `CPPFLAGS`, `CFLAGS`, `LDFLAGS`, `LDLIBS` and `PYTHON` can be overridden. For packaging, for example:

```sh
make install DESTDIR=/tmp/unicable-package PREFIX=/usr
```

There is no libdvbcsa or libdvbv5 dependency. The helper itself does not link against DVBlast. A working driver and appropriate access to `/dev/dvb` are still required; installing these tools does not install tuner drivers or firmware.

## Quick start

### 1. Discover the adapter

```sh
dvb-scan --list
```

This reports `adapterN/frontend0`, driver name, capabilities and advertised delivery systems. Choose the adapter connected to the Unicable feed. The examples below use **adapter 0**.

Check device permissions if necessary:

```sh
ls -l /dev/dvb/adapter0/{frontend0,demux0,dvr0}
```

On systems where the devices belong to the `video` group, the user running both programs needs membership of that group and a new login for it to take effect. Discovery opens the frontend read-only; tuning needs write access.

### 2. Check the tuning calculation

Use the user-band number and frequency assigned to this tuner by your LNB/router. **Slot numbers in this CLI are 1–8**, not protocol indices 0–7. Frequencies are in **MHz**; symbol rate is in **symbols per second**.

```sh
unicable-tune \
  --adapter 0 \
  --frequency 12654 --symbol-rate 30000000 --polarization H \
  --delivery-system DVB-S2 --modulation 8PSK --fec 5/6 --rolloff 0.20 \
  --slot 3 --slot-frequency 1680 --position A \
  --dry-run
```

This profile produces:

```text
DiSEqC: e0 10 5a 4e 48; tuner=1682000kHz (Unicable rounding compensation)
```

The 1682 MHz tuner setting is intentional: it compensates the EN50494 command's 4 MHz rounding. The **user band's nominal frequency remains 1680 MHz**. `--dry-run` never opens a DVB device.

These are example parameters from the tested setup, not an automatically maintained satellite/channel database. Match the transponder and the LNB's actual user-band table to your installation.

### 3. Tune and keep the helper running

In the first terminal:

```sh
unicable-tune \
  --adapter 0 \
  --frequency 12654 --symbol-rate 30000000 --polarization H \
  --delivery-system DVB-S2 --modulation 8PSK --fec 5/6 --rolloff 0.20 \
  --slot 3 --slot-frequency 1680 --position A \
  --seconds 0 --json
```

Wait for the `stable_lock` event and check that subsequent `status` events keep reporting `"lock":true`. Keep this process alive while receiving. `--seconds 0` runs until SIGINT/SIGTERM; the default is only 60 seconds. Closing the frontend can stop reception, depending on the driver.

### 4. Discover services

In another terminal, with the helper still running:

```sh
dvb-scan --scan --adapter 0 --scan-seconds 12
```

The scanner does not retune the frontend or change LNB voltage. It uses its own `DMX_OUT_TSDEMUX_TAP` filter rather than consuming a shared `dvr0` reader. If this tap is unsupported by the driver, it returns an error instead of switching to a shared reader.

An offline TS file can also be inspected:

```sh
dvb-scan --input capture.ts
```

Example service output:

```json
[
  {
    "sid": 265,
    "name": "Example service",
    "provider": "Example provider",
    "scrambled": false,
    "caids": [],
    "pmt_pid": 1000,
    "pmt_seen": true,
    "sdt_seen": true
  }
]
```

Choose the SID from your actual scan. Incomplete PMT/SDT acquisition is shown explicitly; `scrambled:false` by itself is not proof that a service is clear when its tables were not received.

### 5. Start DVBlast with external tuning

Copy and edit the example output configuration:

```sh
cp examples/dvblast.conf ./outputs.conf
```

Replace the destination and SID with your values. Start DVBlast in the second terminal:

```sh
dvblast -a 0 -n 0 -f - -i 0 \
  -c ./outputs.conf -U -C -e -W -Y
```

**`-f -` is essential:** DVBlast receives from the already tuned frontend instead of applying its own tuning parameters. Use the same adapter and `frontend0` for both processes. Leave the helper running. Do not add an ordinary numeric `-f` or a separate DVBlast tuning/DiSEqC configuration to this workflow.

`-U` selects UDP output; `-C -e` enables DVB signaling and EPG passthrough; `-W -Y` enables EMM/ECM passthrough. This helper does not itself filter, decode or modify transport-stream packets. The selected multicast destination must be routed to the intended interface on your machine. DVBlast's [manual](https://github.com/videolan/dvblast/blob/master/dvblast.1) documents these switches and external tuning; its [README](https://github.com/videolan/dvblast/blob/master/README) describes output configuration.

## Tuning options

| Option | Values / units | Existing default |
| --- | --- | --- |
| `--adapter` | 0–255, always `frontend0` | 7 |
| `--frequency` | 10700–12750 MHz, up to 3 decimal places | 12654 |
| `--symbol-rate` | 100000–65000000 symbols/s | 30000000 |
| `--polarization` | H or V | H |
| `--delivery-system` | DVB-S or DVB-S2 | DVB-S2 |
| `--slot` | 1–8, EN50494 user band | 3 |
| `--slot-frequency` | 950–2150 MHz, up to 3 decimal places | 1680 |
| `--position` | A or B, EN50494 satellite-position bit | A |
| `--seconds` | 0–86400; 0 means until signalled | 60 |
| `--modulation` | auto, QPSK, 8PSK | See below |
| `--fec` | auto, 1/2, 2/3, 3/4, 4/5, 5/6, 6/7, 7/8, 8/9, 3/5, 9/10 | See below |
| `--rolloff` | auto, 0.20, 0.25, 0.35 | See below |
| `--json` | Newline-delimited JSON | Off |
| `--dry-run` | Calculate only, no hardware access | Off |
| `--help` | Show usage | — |

The original no-argument profile is retained: adapter7, UB3 / 1680 MHz / A, 12654 H, 30000000, DVB-S2 / 8PSK / FEC 5/6 / rolloff 0.20. **For a new installation, pass your settings explicitly.** Changing only `--adapter`, `--seconds`, `--json` or `--dry-run` retains that original profile.

Supplying tuning options switches unspecified FEC and rolloff to automatic selection. DVB-S uses QPSK and 0.35 rolloff. DVB-S2 requests automatic modulation when advertised by the frontend, otherwise uses 8PSK as a compatibility hint. The tested STiD135 driver searches modulation internally; other frontends may require explicit `--modulation QPSK` or `8PSK`. A dry run cannot query hardware capabilities.

## Multiple tuners

Run one helper and one DVBlast process per adapter, using a **different assigned user band for each tuner on the same Unicable feed**. Do not assume all LNBs use the same slot-frequency table. For example, a second tuner can use slot 4 / 2040 MHz only if that is how the router is configured.

Start tuners sequentially on a shared coax path. This version has no shared-bus command coordinator, automatic user-band allocation or collision avoidance. A second application retuning the same frontend or reprogramming the same user band can interrupt reception.

## Limits and operational behavior

- **EN50494 / Unicable I only.** No EN50607/JESS/Unicable II, PIN-protected commands, additional committed switches or motor control.
- Universal Ku LNB only: LO 9750 MHz below 11700 MHz, otherwise LO 10600 MHz. No configurable wideband/C-band LO.
- `frontend0` and `demux0` only. Explicit DVB-S2X, MIS/ISI and PLS settings are not implemented.
- The helper sends one tuning command and monitors lock. It does not automatically retry a different slot, retune on lock loss or select another transponder.
- Driver/device support determines whether the frontend can stay open alongside DVBlast and whether the scanner's private TS tap is available.
- The scanner has bounded memory and duration; it is not a continuous EPG collector or channel database.

See [status, exit codes and scanner details](docs/DETAILS.md) for integration into other software.

## Tests

`make check` uses only dry runs and synthetic fixtures. It covers EN50494 command bits for all eight slots and A/B/H/V/bands, fractional MHz and rounding, CLI validation, signal-metric scaling, PAT/PMT/SDT parsing, CRC/continuity rejection, DVB text conversion and bounds. It never opens a DVB device or sends a real DiSEqC command.

Hardware tuning behavior was exercised in the original TBS setup; this publication does not claim broad hardware certification.

## License and references

GPL-2.0-only; see [LICENSE](LICENSE) and [NOTICE](NOTICE). This is an independent companion tool, not an official DVBlast release. The EN50494 calculation follows the approach in VDR's `cDiseqc::SetScrFrequency()`. Linux DVB API and upstream implementation references are listed in [docs/DETAILS.md](docs/DETAILS.md).
