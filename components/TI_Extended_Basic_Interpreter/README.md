# TI_Extended_Basic_Interpreter

The pure language layer for TI Extended BASIC running on ESP32. Header-only Arduino library with no display or board dependencies — drawing, file I/O, BLE input, and audio are all supplied by the host sketch via callbacks.

## What's here

| File | Role |
|---|---|
| `token_parser.h` | Tokenizer + statement executor for BASIC source. |
| `expr_parser.h` | Expression evaluator with operator precedence. |
| `exec_manager.h` | Top-level program runner: line storage, GOTO/GOSUB, FOR/NEXT, breakpoints, throttle. |
| `var_table.h` | Numeric and string variable storage. |
| `line_editor.h` | Editor surface that the host sketch wires its keyboard input into. |
| `tp_types.h` | Token enums, error codes, file-mode flags. |
| `sprites.h` | TMS9918-style sprite data structures (rendering is host-supplied). |

## Used by

- [esp32-s3-box-basic](https://github.com/JonVogel/esp32-s3-box-basic) — ESP32-S3-Box-3 port
- [ti-extended-basic-esp32](https://github.com/JonVogel/ti-extended-basic-esp32) — 800×480 RGB-panel reference build
- ti-basic-otg — ESP32-S3-USB-OTG variant

## Embedding

Add as a git submodule next to your sketch and pass `--libraries .` to `arduino-cli compile`:

```sh
git submodule add https://github.com/JonVogel/TI_Extended_Basic_Interpreter.git
```
