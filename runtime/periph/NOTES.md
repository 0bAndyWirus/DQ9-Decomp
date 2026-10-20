# Peripheral implementation notes

## Backup device selection

Backup capacity is caller-supplied through `BackupType` or the optional ABI
configuration's `[cartridge]` section. Automatic selection uses a supported
existing save size and otherwise defaults to flash. No title-specific cartridge
signature is embedded in the runtime.

## Slot-1 card and backup bus

Card command bytes are kept in address order (first command byte at `0x040001a8`). ROMCTRL uses the documented block encoding: code 0 transfers no bytes, codes 1-6 transfer `0x100 << (code-1)`, and code 7 transfers one 32-bit word. For a nonzero transfer, the initial data-ready delay is `(8 + gap) * clocks + 4` machine ticks, where `gap` is ROMCTRL bits 0-12 and bit 27 selects 8 or 5 clocks. Zero-length commands complete immediately. Later words advance after 2 ticks, and the bus stops on a ready word until software/DMA consumes it. Data-ready is ROMCTRL bit 23, transfer-busy is bit 31, and the card IRQ is requested by AUXSPICNT bit 14.

Normal-mode B7 reads below `0x8000` alias into the first `0x200`-byte card-data block using `0x8000 + (address & 0x1ff)`. Direct boot-loader reads instead return raw file bytes, padding past EOF with `0xff`, without the bus alias. The image mapping is read-only.

Backup SPI transactions require AUXSPICNT slot-enable bit 15 and serial-backup mode bit 13. Hold bit 6 preserves chip select across command/address/data bytes; clearing it before the final unit ends the transaction. EEPROM WRITE commits on deselect with WEL handling and page-wrap behavior. Flash supports WREN/WRDI, status, JEDEC ID, READ/FAST READ, page program/page write, page/sector/chip erase. Save imports accept exact-size raw `.sav`/`.dsv` data; DeSmuME DSV's 40-byte footer is recognized by its terminal `|-DESMUME SAVE-|` cookie and `padded_size`, then excluded from the imported bytes. Dirty saves are written to a sibling temporary file, fsynced, and atomically renamed after parent directories are created.

## SPI firmware and touchscreen

The synthetic 256-KiB firmware contains no firmware program code: bytes outside the required headers/settings/connection blocks remain erased (`0xff`) or zeroed header space. Header user-settings offset is `0x7fc0` (byte address `0x3fe00`); firmware user settings are at `0x3fe00` and `0x3ff00`, version 5, with update counters 0/1. User-settings CRC16 uses reflected polynomial `0xa001`, initial `0xffff`; Wi-Fi calibration and access-point CRCs use the same polynomial with initial 0. The three connection slots (plus the unused/hidden fourth area) have `status=0xff` and valid CRCs. A synthetic `00:09:bf:12:34:56` MAC and plausible radio-calibration values are provided. Layout follows the [GBATEK firmware header](https://problemkaputt.de/gbatek-ds-firmware-header.htm), [user settings](https://problemkaputt.de/gbatek-ds-firmware-user-settings.htm), [Wi-Fi calibration](https://problemkaputt.de/gbatek-ds-firmware-wifi-calibration-data.htm), and [access-point layout](https://problemkaputt.de/gbatek-ds-firmware-wifi-internet-access-points.htm).

Touch calibration uses interior calibration points: X raw `0x200`/`0xe00` at firmware pixels 33/225, and Y raw `0x200`/`0x800` at pixels 33/129. Firmware pixels are one-based; host input remains zero-based. Both axes therefore have exactly 16 ADC units per host pixel. This makes ADC conversion and the guest SDK's fixed-point inverse reversible without the one-pixel truncation loss of fractional synthetic calibration steps. Calibration points are not ADC saturation limits: legal touches beyond the interior points retain their full 12-bit response. No frontend or scripted-coordinate offset is applied. TSC control-channel numbers follow the TSC2046 (Y=1, Z1=3, Z2=4, X=5); 12-bit responses include the leading dummy bit, 8-bit mode returns the high eight conversion bits, and pen-up returns X=0/Y=`0xfff`. Power-manager register defaults report good battery, audio amplifier enabled, both backlights enabled, and maximum external-powered brightness.

## RTC

RTC GPIO transactions start when CS rises with SCK high. Commands accept ordinary and mirrored bit orders (for example `0x67`/`0xe6` select time-only read); parameters and replies are LSB first. Commands, writes and replies advance on falling SCK edges, matching the ARM7 SDK's low-clock sampling. Input-mode SIO retains the device-driven bit across host writes, and input SCK/CS direction fields retain their previous pin levels. Deselecting aborts an incomplete transfer; completed replies stop instead of wrapping. Transfer buffers are fixed seven-byte arrays.

Status defaults to 24-hour mode. Date/time and time-only reads return BCD from host local time, or the configured epoch plus emulated elapsed seconds and explicit timezone offset during deterministic replay. In 24-hour mode, hours 12-23 set the hour PM flag; 12-hour encoding uses a modulo-12 hour without that flag. Status/alarm/free/clock-adjust registers are stored, with ALARM1's three-byte mode selected by STATUS2's low nibble equalling 4. Clock-setting commands consume their full seven/three-byte payload without replacing the authoritative clock or applying oscillator correction. No RTC interrupt is generated.
