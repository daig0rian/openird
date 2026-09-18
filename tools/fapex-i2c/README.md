# fapex-i2c — FAPex I2C probe helper (stage B)

A small target-side tool that does I2C transactions through the vendor
**FAPex I2C character device** (`/dev/fapi/fapi174` etc., major 244) using the
*published* FAPEX ioctl ABI from the GPL headers `abst_libi2c.h` / `abst_lib.h`.
It contains **no vendor code** — it is a clean user of the same chardev contract
the vendor userland uses. We use it in Milestone 1 to probe the ISDB-T demod
(MB86A22, I2C slave `0x20`) and scan the I2C buses by hand, without the vendor
daemon (see `docs/specs/sb-tv02-wfpl-stage-b-fapex.md` §5.5).

## ABI (GPL `abst_lib.h` / `abst_libi2c.h`)

| ioctl | value | arg |
|---|---|---|
| `FAPEX_IOCCOM_I2C_OPEN`  | `_IOW(0xFF,0x60,OpenParam[24])` = `0x4018FF60` | `{blockIndex, speed, protocol, slaveAddress, type, timeOut}` |
| `FAPEX_IOCCOM_I2C_READ`  | `_IOR(0xFF,0x62,RWParam[20])`   = `0x8014FF62` | `{msSlaveAddress, txPtr, txLen, rxPtr, rxLen}` |
| `FAPEX_IOCCOM_I2C_WRITE` | `_IOW(0xFF,0x62,RWParam[20])`   = `0x4014FF62` | same struct |

`speed`: 1 = 100 kHz, 2 = 400 kHz. `protocol`: 0 = common, 1 = restart.
`type`: 0 = hardware, 1 = software. `timeOut` is in ms (set it so a non-ACK
returns an error instead of hanging). A READ transaction writes the tx bytes
(the register) then does a repeated-start read — exactly what the vendor
`MB86A22_..._Detect` does to read a demod register.

**The bus is selected by `OpenParam.blockIndex`, not by the chardev minor**
(verified on hardware: any of `fapi174..fapi177` reaches block 2). So
`blockIndex` 0/1/2 = SoC I2C0/I2C1/I2C2.

## Usage

```
fapex-i2c [--speed 100|400] [--timeout MS] [--restart] DEV BLOCK MODE ARGS
  read  SLAVE REG [LEN]        OPEN + read LEN bytes from REG
  write SLAVE REG [BYTE...]    OPEN + write REG (+bytes)
  xfer  SLAVE RXLEN [TXBYTE...] write TX then read RXLEN (RXLEN=0 = write only)
  scan  [FIRST LAST]           OPEN + read reg0 across a slave-address range
```

### TC90522/TC90532 tuner I2C gateway (register 0xfe)

The Toshiba demod reaches its tuners through an I2C repeater at register `0xfe`
(`TC90522_I2C_THRU_REG`). Use `xfer` to drive a tuner behind the demod (verified
on SB-TV02-WFPL, §5.5 — the ISDB-S tuner answers at 7-bit `0x60` behind the demod
ISDB-S submodule `0x22`):

```
# tuner READ  (demod submodule S, tuner 7-bit A):  write [0xfe, A<<1|1] then read N
fapex-i2c --restart /dev/fapi/fapi174 0 xfer 0x22 2 0xfe 0xC1     # read 2B from tuner 0x60
# tuner WRITE (set tuner reg pointer, then read it back):
fapex-i2c /dev/fapi/fapi174 0 write 0x22 0xfe 0xC0 0x00           # tuner 0x60 <- reg ptr 0x00
fapex-i2c --restart /dev/fapi/fapi174 0 xfer 0x22 1 0xfe 0xC1     # read 1B
```
`--restart` (repeated-start combined transfer) is needed for the gateway read.

## Example: identify the MB86A22 demod (SB-TV02-WFPL)

MB86A22 chip-ID register `0x00` reads back `0x23` (`0x13` for MB86A20S):

```
fapex-i2c /dev/fapi/fapi174 0 read 0x20 0x00     # expect: read slave 0x20 reg 0x00: 23
fapex-i2c /dev/fapi/fapi174 0 scan               # who ACKs on I2C0
```

## Build

```
scripts/build-fapex-i2c.sh [BOARD]      # BOARD default: mb86h61_softbank_sb-tv02-wfpl
```
Output: `tools/fapex-i2c/fapex-i2c` (static ARM/musl binary). `scp` it to the
device and run as root (needs the FAPex `fdli2c` chardev present).
