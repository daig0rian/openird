# yb1-cmd script: the smallest command set that puts raw TS into the TS input
# FIFOs (docs/specs/poe-br100-yb1-av-command.md section 5.1).  For the EMMA3SEP
# boards that run the same AV firmware (POE-BR100-YB1, SB-TV06-WRIP).
#
#   power on -> AV firmware to phys 0x30000 -> insmod csd -> rmmod val_merge csd
#   yb1-cmd -v min-rawts.cmd          (once per boot: Initialise is one-shot)
#
# Six commands only: Initialise, OpenDevice x3, input source, filter allocate,
# set PID.  Nothing here touches the descrambler.  Device 2 feeds FIFO 2 (put
# the heavy PIDs there: the drainer copies it by XDMA), device 0 feeds FIFO 0.
# The PIDs below are the ones NHK uses on both terrestrial and BS (video,
# audio, PCR), plus PAT and the fixed SI PIDs; a real driver enumerates them
# from PAT/PMT/CAT (docs/specs/poe-br100-yb1-ts-fifo.md section 6).

next 1 0x0100 0xa0010800

auto 0x1900 0 0 0
auto 0x1901 4 4 0 1 = 0x00010000 > $0
auto 0x1901 4 4 0 1 = 0x01010001 > $1
auto 0x1901 4 4 0 1 = 0x02010002 > $2
auto 0x1904 8 0 0 $0 3
auto 0x1904 8 0 0 $2 3

# device 2 -> FIFO 2
auto 0x190a 16 4 0 7 @12 $2 0xffffff00 > $3
auto 0x190b 8 0 0 $3 0x0100
auto 0x190a 16 4 0 7 @12 $2 0xffffff00 > $3
auto 0x190b 8 0 0 $3 0x0110
auto 0x190a 16 4 0 7 @12 $2 0xffffff00 > $3
auto 0x190b 8 0 0 $3 0x0111
auto 0x190a 16 4 0 7 @12 $2 0xffffff00 > $3
auto 0x190b 8 0 0 $3 0x01ff
auto 0x190a 16 4 0 7 @12 $2 0xffffff00 > $3
auto 0x190b 8 0 0 $3 0x0000

# device 0 -> FIFO 0
auto 0x190a 16 4 0 7 @12 $0 0xffffff00 > $3
auto 0x190b 8 0 0 $3 0x0010
auto 0x190a 16 4 0 7 @12 $0 0xffffff00 > $3
auto 0x190b 8 0 0 $3 0x0011
auto 0x190a 16 4 0 7 @12 $0 0xffffff00 > $3
auto 0x190b 8 0 0 $3 0x0012
auto 0x190a 16 4 0 7 @12 $0 0xffffff00 > $3
auto 0x190b 8 0 0 $3 0x0014
