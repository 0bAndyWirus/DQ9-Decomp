# Peripheral implementation notes

## Backup device selection

Backup capacity is caller-supplied through `BackupType` or the optional ABI
configuration's `[cartridge]` section. Automatic selection uses a supported
existing save size and otherwise defaults to flash. No title-specific cartridge
signature is embedded in the runtime.

## Slot-1 card and backup bus

Card command bytes are kept in address order (first command byte at `0x040001a8`). ROMCTRL uses the documented block encoding: code 0 transfers no bytes, codes 1-6 transfer `0x100 << code` (512-16384 bytes), and code 7 transfers one 32-bit word. For a nonzero transfer, the initial data-ready delay is `(8 + gap) * clocks + 4` machine ticks, where `gap` is ROMCTRL bits 0-12 and bit 27 selects 8 or 5 clocks. Zero-length commands complete immediately. One initial readiness callback exposes the block; READY remains asserted through successive software/DMA reads until the last byte is consumed. Cartridge DMA drains available units up to one latched DMA completion per callback, then rearms a repeat channel without recursively draining it. Card exhaustion stops an incomplete DMA without stale extra writes or a DMA completion IRQ. Data-ready is ROMCTRL bit 23, transfer-busy is bit 31, and the card IRQ is requested by AUXSPICNT bit 14.

Normal-mode B7 reads below `0x8000` alias into the first `0x200`-byte card-data block using `0x8000 + (address & 0x1ff)`. Direct boot-loader reads instead return raw file bytes, padding past EOF with `0xff`, without the bus alias. The image mapping is read-only.

Backup SPI transactions require AUXSPICNT slot-enable bit 15 and serial-backup mode bit 13. Hold bit 6 preserves chip select across command/address/data bytes; clearing it before the final unit ends the transaction. EEPROM WRITE commits on deselect with WEL handling and page-wrap behavior. Flash supports WREN/WRDI, status, JEDEC ID, READ/FAST READ, page program/page write, page/sector/chip erase. Save imports accept exact-size raw `.sav`/`.dsv` data; DeSmuME DSV's 40-byte footer is recognized by its terminal `|-DESMUME SAVE-|` cookie and `padded_size`, then excluded from the imported bytes. Dirty saves are written to a sibling temporary file, fsynced, and atomically renamed after parent directories are created.

## SPI firmware and touchscreen

The synthetic 256-KiB firmware contains no firmware program code: bytes outside the required headers/settings/connection blocks remain erased (`0xff`) or zeroed header space. Header user-settings offset is `0x7fc0` (byte address `0x3fe00`); firmware user settings are at `0x3fe00` and `0x3ff00`, version 5, with update counters 0/1. User-settings CRC16 uses reflected polynomial `0xa001`, initial `0xffff`; Wi-Fi calibration and access-point CRCs use the same polynomial with initial 0. The three connection slots (plus the unused/hidden fourth area) have `status=0xff` and valid CRCs. A synthetic `00:09:bf:12:34:56` MAC and plausible radio-calibration values are provided. Layout follows the [GBATEK firmware header](https://problemkaputt.de/gbatek-ds-firmware-header.htm), [user settings](https://problemkaputt.de/gbatek-ds-firmware-user-settings.htm), [Wi-Fi calibration](https://problemkaputt.de/gbatek-ds-firmware-wifi-calibration-data.htm), and [access-point layout](https://problemkaputt.de/gbatek-ds-firmware-wifi-internet-access-points.htm).

Touch calibration in both firmware and `set_touch()` uses raw endpoints `(0x100,0x100)` and `(0xf00,0xf00)`, corresponding to screen calibration `(1,1)` and `(255,191)` in firmware coordinates. TSC control-channel numbers follow the TSC2046 (Y=1, Z1=3, Z2=4, X=5); 12-bit responses include the leading dummy bit, 8-bit mode returns the high eight conversion bits, and pen-up returns X=0/Y=`0xfff`. Power-manager register defaults report good battery, audio amplifier enabled, both backlights enabled, and maximum external-powered brightness.

## RTC

RTC GPIO transfers use CS-high selection and LSB-first command/parameter bytes, with SIO/SCK/CS direction fields honored. Status defaults to 24-hour mode. Date/time and time-only reads are sampled from host local time and returned as BCD; the 24-hour result also sets the hour PM flag for hours 12-23. Status/alarm/free/clock-adjust registers are stored; clock-setting data does not replace the host clock or apply oscillator correction. No RTC interrupt is generated.
