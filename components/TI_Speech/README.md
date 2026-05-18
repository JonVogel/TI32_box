# TI_Speech

Build-time bake of the TI-99/4A Speech Synthesizer cartridge ROM (`spchrom.bin`) into a `PROGMEM` C array so `CALL SAY` / `CALL SPGET` can do real vocabulary lookups against authentic TI speech data.

## Why this exists

`speech_rom.h` is the cartridge ROM baked into a `PROGMEM` C array so the firmware can do real vocabulary lookups without runtime file I/O. It ships in the repo so a fresh clone builds with working speech.

If you want to swap in a different dump (Speech Editor extended vocab, Geneve speech ROM, etc.), run `extract.py` to overwrite `speech_rom.h` from your own binary.

## Regenerate

```sh
python extract.py path/to/spchrom.bin
```

The expected ROM is 32 768 bytes, SHA-256 `1A7481F3E7E2D464772B540F9B9E4D529DC61DF372F181ADF7432F0DF9876C16` (the canonical TI Speech Synthesizer cartridge dump shipped with MESS / MAME). Other dumps may work; the script warns on mismatch but proceeds.

## ROM format

Documented inline in the generated header and in `extract.py`. Briefly:

```
Offset 0:  0xAA marker (skip)
Offset 1:  Root vocabulary node (binary tree)
Node:      [1B nameLen][N bytes ASCII name][2B prev BE]
           [2B next BE][1B pad][2B LPC offset BE][1B LPC len]
```

Source: Marc Rousseau's `ti99sim/src/util/dumpspch.cpp`, function `ReadNode`.

The tree contains roughly 373 vocabulary words (digits, letters, color names, numbers, common verbs, etc.) — every word `CALL SAY` recognizes on a real TI.

## Files

| File | Purpose |
|---|---|
| `extract.py` | Reads a `spchrom.bin` dump and writes `speech_rom.h` |
| `speech_rom.h` | Baked cartridge ROM (PROGMEM C array). Committed. |
| `README.md` | This file |
