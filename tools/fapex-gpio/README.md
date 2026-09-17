# fapex-gpio — FAPex GPIO bring-up helper (stage B)

A small target-side tool that drives and reads GPIO pins through the vendor
**FAPex GPIO character device** (`/dev/fapi/fapi186`, major 244) using the
*published* FAPEX ioctl ABI from the GPL headers `abst_libgpio.h` / `abst_lib.h`.
It contains **no vendor code** — it is a clean user of the same chardev contract
the vendor userland uses. We use it in Milestone 1 to bring the board up by hand
(for example, to power the Dione H58 by driving its supply/reset GPIOs) without
running the vendor daemon, whose board init races our USB stack.

## ABI (GPL `abst_lib.h` / `abst_libgpio.h`)

| ioctl | value | arg |
|---|---|---|
| `FAPEX_IOCCOM_GPIO_OPEN`     | `_IOW(0xFF,0x40,{u32 pin;u32 function})` = `0x4008FF40` | `{pin, function}` |
| `FAPEX_IOCCOM_GPIO_READBIT`  | `_IOR(0xFF,0x41,int32_t)` = `0x8004FF41` | `*int32` ← level |
| `FAPEX_IOCCOM_GPIO_WRITEBIT` | `_IOW(0xFF,0x41,int32_t)` = `0x4004FF41` | `*int32` → level |

`function`: `FAPEX_GPIO_OUT_0` = `0x0100` (output, drive 0), `FAPEX_GPIO_OUT_1` =
`0x0101` (output, drive 1), `FAPEX_GPIO_IN_GENERIC_SW_FUNCTION` = `0x01010000`
(read a switch/strap). `OPEN` binds one pin to the open file; a 2nd `OPEN` on the
same fd is a no-op. The configured **pad state persists after `close()`** (the
FAPI handle is only the userland reference), but this tool keeps fds open in the
`hold`/`seq` modes so a rail stays asserted for as long as it runs.

## Usage

```
fapex-gpio DEV read  PIN...                 open each PIN as input, read, print
fapex-gpio DEV set   PIN LVL                open PIN as output LVL (0|1); exit
fapex-gpio DEV write PIN LVL                open PIN out, WRITEBIT LVL; exit
fapex-gpio DEV pulse PIN LOW_MS END_LVL     drive !END_LVL for LOW_MS, then END_LVL, hold
fapex-gpio DEV hold  [--delay-ms N] PIN:LVL...   open each PIN out LVL, hold
fapex-gpio DEV seq   TOKEN...               PIN:LVL | w:PIN:LVL | s:MS, then hold
```

`hold`/`seq`/`pulse` block until `SIGINT`/`SIGTERM`, then release (close) the pins.

## Example: bring the Dione H58 online (SB-TV02-WFPL)

Observed from the vendor daemon (`WirelessDtvPower`) with `fapex-trace`; see
`docs/specs/sb-tv02-wfpl-stage-b-fapex.md` §5.3. Pins: 77/78/81 = DIONE
1.2/1.8/3.3V, 48/49 = demod 1.2/3.3V, 74 = DIONE reset. Assert reset, ramp the
rails, release reset:

```
fapex-gpio /dev/fapi/fapi186 seq \
  81:0 78:0 77:0 49:0 48:0 74:0 \
  w:77:1 w:81:1 w:78:1 w:49:1 w:48:1 s:50 w:74:1
```

With `usbcore`/`nop-usb-xceiv`/`musb_hdrc` loaded, the Dione then enumerates on
the internal hub as `06b8:102d` and `h57fmb` binds it.

## Build

```
scripts/build-fapex-gpio.sh [BOARD]     # BOARD default: mb86h61_softbank_sb-tv02-wfpl
```
Output: `tools/fapex-gpio/fapex-gpio` (static ARM/musl binary). `scp` it to the
device and run as root.
