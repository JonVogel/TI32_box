# ESP32_File_Handling

Hardware-agnostic file I/O routing for TI BASIC and Scott Adams Adventure ESP32 ports. Header-only Arduino library.

## What's here

| File | Role |
|---|---|
| `file_io.h` | Routes BASIC's OPEN/CLOSE/PRINT#/INPUT#/EOF/REC operations to LittleFS, SD/SD_MMC, or mounted V9T9 disk images. Includes a small DSK<n> mount table. |
| `dsk_image.h` | V9T9 .DSK reader/writer: VIB, FDR, cluster chains, DIS/VAR + DIS/FIX records. |

## Hardware-agnostic SD

This header does **not** include `<SD.h>` or `<SD_MMC.h>`. Each downstream project picks its SD flavor (SD-over-SPI for the RGB-panel and OTG boards, SD_MMC for the Box-3 SENSOR add-on) and hands the resulting `fs::FS&` to `fio::setSDFs()` at boot:

```cpp
// Box-3 (SD_MMC native peripheral):
SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
if (SD_MMC.begin("/sdcard", true, false))
{
  fio::setSDFs(&SD_MMC);
}

// RGB / OTG (SD over SPI):
SPI.begin(sck, miso, mosi, cs);
if (SD.begin(cs, SPI))
{
  fio::setSDFs(&SD);
}
```

The shared file_io routes `SDCARD.*` ops through whatever pointer the project supplies.

## Used by

- esp32-s3-box-basic
- ti-extended-basic-esp32
- ti-basic-otg
- ti-scott-adams-esp32

## Embedding

```sh
git submodule add https://github.com/JonVogel/ESP32_File_Handling.git
```
