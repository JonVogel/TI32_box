/*
 * TI Extended BASIC Simulator v0.2
 * For ESP32-S3-USB-OTG
 *
 * Refactored architecture:
 *   - Execution Manager (EM): handles program flow
 *   - Token Parser (TP): state machine interprets tokens
 *   - Expression Parser: evaluates arithmetic/logical expressions
 *   - Variable Table: manages numeric and string variables
 *
 * Display: 28 columns x 24 rows (8x8 pixel characters)
 * Storage: Programs tokenized in RAM, saved as text to LittleFS
 *
 * Board settings (Arduino IDE):
 *   Board: "ESP32S3 Dev Module"
 *   Partition: Custom (8MB with SPIFFS)
 *   USB CDC On Boot: "Enabled"
 */

#define LGFX_AUTODETECT
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "audio.h"
#include "tms5220.h"   // triggers arduino-cli discovery of TI_Speech library
#include <LittleFS.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include "ti_font.h"

// TI Speech Synthesizer cartridge ROM bake. Generated locally from a
// user-supplied spchrom.bin via TI_Speech/extract.py. The file is
// gitignored (copyrighted TI material); without it, CALL SAY / CALL
// SPGET silently fall back to no-op stubs (compile still succeeds).
#if __has_include("TI_Speech/speech_rom.h")
  #include "TI_Speech/speech_rom.h"
  #define HAVE_SPEECH_ROM 1
#elif __has_include("speech_rom.h")
  #include "speech_rom.h"
  #define HAVE_SPEECH_ROM 1
#else
  #define HAVE_SPEECH_ROM 0
#endif
#include "ble_keyboard.h"
#include "exec_manager.h"
#include "file_io.h"
#include "program_io.h"
#include "sprites.h"
#include "line_editor.h"
#include "web_files.h"

// LCD pins (ESP32-S3-Box-3, per esp-bsp/esp-box-3)
#define LCD_DC    4
#define LCD_CS    5
#define LCD_SCLK  7
#define LCD_MOSI  6
#define LCD_RST   48    // through inverting level shifter U18A: GPIO HIGH = panel reset
#define LCD_BL    47

// Buttons (Box-3: only BOOT and MUTE on the mainboard)
#define BTN_OK    0     // BOOT button — also used as pairing trigger
#define BTN_MUTE  1     // MUTE switch (active low, gated through D-flip-flop)

// SD card on the Box-3 SENSOR add-on board, native SD-MMC 1-bit mode.
// Pins come from esp-bsp/esp-box-3.h's SD-MMC defines.
#define SD_CLK   11     // BSP_SD_CLK
#define SD_CMD   14     // BSP_SD_CMD
#define SD_D0     9     // BSP_SD_D0
#define SD_PWR   43     // BSP_SD_POWER — drive HIGH to power the SD slot

// Display geometry — Box-3 panel is 320x240 landscape, full TI 32-col grid fits
#define COLS       32
#define ROWS       24
#define CHAR_W     8
#define CHAR_H     8
#define SCREEN_W   320
#define SCREEN_H   240

// Display offsets for centering — (320-32*8)/2 = 32, (240-24*8)/2 = 24
#define DISPLAY_X_OFFSET 32
#define DISPLAY_Y_OFFSET 24

// Status bar at the bottom
#define STATUS_Y   (DISPLAY_Y_OFFSET + ROWS * CHAR_H)

// ---------------------------------------------------------------------------
// Forward declarations for functions defined later in this file.
// The Arduino IDE auto-inserts these when compiling a .ino, but plain
// C++ (ESP-IDF) doesn't, so we list the ones referenced out of order.
// ---------------------------------------------------------------------------
struct LineEdit;
static void fillBackground(uint16_t bg);
static void drawPairingScreen();
static void updatePairingCountdown(unsigned long remainMs);
static bool editorBufferIsAutoFillOnly(const LineEdit& s);
static void spriteRedrawAll();
static void catPrintLine(const char* s);
extern int  g_catLines;
extern bool g_catCancelled;

// ---------------------------------------------------------------------------
// Keyword table — maps text to tokens (used by tokenizer)
// ---------------------------------------------------------------------------

struct KeywordEntry
{
  const char* text;
  Token token;
};

static const KeywordEntry keywords[] =
{
  // RUN, NEW, DIR are handled as pre-tokenize string commands
  // (so they're not listed here)
  {"LIST",       TOK_LIST},
  {"OLD",        TOK_OLD},
  {"SAVE",       TOK_SAVE},
  {"BYE",        TOK_BYE},
  {"NUMBER",     TOK_NUM},
  {"NUM",        TOK_NUM},
  {"PRINT",      TOK_PRINT},
  {"USING",      TOK_USING},
  {"DISPLAY",    TOK_DISPLAY},
  {"ACCEPT",     TOK_ACCEPT},
  {"GOTO",       TOK_GOTO},
  {"GO TO",      TOK_GOTO},
  {"GOSUB",      TOK_GOSUB},
  {"RETURN",     TOK_RETURN},
  {"IF",         TOK_IF},
  {"THEN",       TOK_THEN},
  {"ELSE",       TOK_ELSE},
  {"FOR",        TOK_FOR},
  {"TO",         TOK_TO},
  {"STEP",       TOK_STEP},
  {"NEXT",       TOK_NEXT},
  {"LET",        TOK_LET},
  {"INPUT",      TOK_INPUT},
  {"LINPUT",     TOK_LINPUT},
  {"DIM",        TOK_DIM},
  {"REM",        TOK_REM},
  {"END",        TOK_END},
  {"STOP",       TOK_STOP},
  {"DATA",       TOK_DATA},
  {"READ",       TOK_READ},
  {"RESTORE",    TOK_RESTORE},
  {"RANDOMIZE",  TOK_RANDOMIZE},
  {"DEF",        TOK_DEF},
  {"ON",         TOK_ON},
  {"OPTION",     TOK_OPTION},
  {"BASE",       TOK_BASE},
  {"BREAK",      TOK_BREAK},
  {"UNBREAK",    TOK_UNBREAK},
  {"ERROR",      TOK_ERROR},
  {"WARNING",    TOK_WARNING},
  {"CONTINUE",   TOK_CONTINUE},
  {"CON",        TOK_CONTINUE},
  {"RESEQUENCE", TOK_RES},
  {"RES",        TOK_RES},
  {"SIZE",       TOK_SIZE},
  {"MERGE",      TOK_MERGE},
  {"CALL",       TOK_CALL},
  {"SUB",        TOK_SUB},
  {"SUBEND",     TOK_SUBEND},
  {"SUBEXIT",    TOK_SUBEXIT},
  {"OPEN",       TOK_OPEN},
  {"CLOSE",      TOK_CLOSE},
  {"OUTPUT",     TOK_OUTPUT},
  {"UPDATE",     TOK_UPDATE},
  {"APPEND",     TOK_APPEND},
  {"SEQUENTIAL", TOK_SEQUENTIAL},
  {"RELATIVE",   TOK_RELATIVE},
  {"INTERNAL",   TOK_INTERNAL},
  {"FIXED",      TOK_FIXED},
  {"PERMANENT",  TOK_PERMANENT},
  {"VARIABLE",   TOK_VARIABLE_KW},
  {"REC",        TOK_REC},
  {"DELETE",     TOK_DELETE},
  {"IMAGE",      TOK_IMAGE},
  {"TRACE",      TOK_TRACE},
  {"UNTRACE",    TOK_UNTRACE},
  {"AND",        TOK_AND},
  {"OR",         TOK_OR},
  {"XOR",        TOK_XOR},
  {"NOT",        TOK_NOT},
  {NULL, TOK_EOL}
};

// ---------------------------------------------------------------------------
// Tokenizer (converts text to tokens)
// ---------------------------------------------------------------------------

static int skipSpaces(const char* src, int pos)
{
  while (src[pos] == ' ')
  {
    pos++;
  }
  return pos;
}

static int matchKeyword(const char* src, int pos)
{
  int bestMatch = -1;
  int bestLen = 0;

  for (int i = 0; keywords[i].text != NULL; i++)
  {
    const char* kw = keywords[i].text;
    int klen = strlen(kw);
    bool match = true;

    for (int j = 0; j < klen; j++)
    {
      if (toupper(src[pos + j]) != kw[j])
      {
        match = false;
        break;
      }
    }

    if (match && klen > bestLen)
    {
      char next = src[pos + klen];
      if (next == '\0' || !isalnum(next))
      {
        bestMatch = i;
        bestLen = klen;
      }
    }
  }

  return bestMatch;
}

static int tokenizeLine(const char* src, uint8_t* tokens, int maxLen)
{
  int pos = 0;
  int out = 0;

  while (src[pos] != '\0')
  {
    pos = skipSpaces(src, pos);
    if (src[pos] == '\0')
    {
      break;
    }

    // REM — rest of line is literal text
    if (out > 0 && tokens[out - 1] == TOK_REM)
    {
      int remLen = strlen(&src[pos]);
      if (out + 2 + remLen >= maxLen)
      {
        return -1;
      }
      tokens[out++] = TOK_STRING_LIT;
      tokens[out++] = (uint8_t)remLen;
      memcpy(&tokens[out], &src[pos], remLen);
      out += remLen;
      break;
    }

    // Quoted string
    if (src[pos] == '"')
    {
      pos++;
      int start = pos;
      while (src[pos] != '\0' && src[pos] != '"')
      {
        pos++;
      }
      int slen = pos - start;
      if (src[pos] == '"')
      {
        pos++;
      }
      if (out + 2 + slen >= maxLen)
      {
        return -1;
      }
      tokens[out++] = TOK_STRING_LIT;
      tokens[out++] = (uint8_t)slen;
      memcpy(&tokens[out], &src[start], slen);
      out += slen;
      continue;
    }

    // Number literal — stored as TOK_UNQUOTED_STR + length + ASCII digits
    if (isdigit(src[pos]) || (src[pos] == '.' && isdigit(src[pos + 1])))
    {
      int start = pos;
      while (isdigit(src[pos]) || src[pos] == '.' || src[pos] == 'E' ||
             src[pos] == 'e' ||
             ((src[pos] == '+' || src[pos] == '-') &&
              (src[pos - 1] == 'E' || src[pos - 1] == 'e')))
      {
        pos++;
      }
      int slen = pos - start;
      if (slen > 255 || out + 2 + slen >= maxLen)
      {
        return -1;
      }
      tokens[out++] = TOK_UNQUOTED_STR;
      tokens[out++] = (uint8_t)slen;
      memcpy(&tokens[out], &src[start], slen);
      out += slen;
      continue;
    }

    // Operators and punctuation
    bool foundOp = true;
    switch (src[pos])
    {
      case '+':  tokens[out++] = TOK_PLUS;       pos++; break;
      case '-':  tokens[out++] = TOK_MINUS;      pos++; break;
      case '*':  tokens[out++] = TOK_MULTIPLY;   pos++; break;
      case '/':  tokens[out++] = TOK_DIVIDE;     pos++; break;
      case '^':  tokens[out++] = TOK_POWER;      pos++; break;
      case '&':  tokens[out++] = TOK_CONCAT;     pos++; break;
      case '(':  tokens[out++] = TOK_LPAREN;     pos++; break;
      case ')':  tokens[out++] = TOK_RPAREN;     pos++; break;
      case ',':  tokens[out++] = TOK_COMMA;      pos++; break;
      case ';':  tokens[out++] = TOK_SEMICOLON;  pos++; break;
      case ':':  tokens[out++] = TOK_COLON;      pos++; break;
      case '=':  tokens[out++] = TOK_EQUAL;      pos++; break;
      case '<':
        // TI encodes compound comparisons as two separate tokens:
        // <=  →  TOK_LESS + TOK_EQUAL
        // <>  →  TOK_LESS + TOK_GREATER
        // >=  →  TOK_GREATER + TOK_EQUAL
        tokens[out++] = TOK_LESS;
        if (src[pos + 1] == '=')      { tokens[out++] = TOK_EQUAL;   pos += 2; }
        else if (src[pos + 1] == '>') { tokens[out++] = TOK_GREATER; pos += 2; }
        else                          {                              pos++;    }
        break;
      case '>':
        tokens[out++] = TOK_GREATER;
        if (src[pos + 1] == '=') { tokens[out++] = TOK_EQUAL; pos += 2; }
        else                     {                            pos++;    }
        break;
      default:
        foundOp = false;
        break;
    }
    if (foundOp)
    {
      continue;
    }

    // Keyword match
    int kwIdx = matchKeyword(src, pos);
    if (kwIdx >= 0)
    {
      if (out >= maxLen)
      {
        return -1;
      }
      tokens[out++] = keywords[kwIdx].token;
      pos += strlen(keywords[kwIdx].text);
      continue;
    }

    // Variable name
    if (isalpha(src[pos]))
    {
      // Variable name — stored as raw ASCII bytes (TI format)
      int start = pos;
      while (isalnum(src[pos]) || src[pos] == '_')
      {
        pos++;
      }
      if (src[pos] == '$')
      {
        pos++;
      }
      int vlen = pos - start;
      if (out + vlen >= maxLen)
      {
        return -1;
      }
      for (int i = 0; i < vlen; i++)
      {
        tokens[out++] = toupper(src[start + i]);
      }
      continue;
    }

    pos++;
  }

  if (out >= maxLen)
  {
    return -1;
  }
  tokens[out++] = TOK_EOL;
  return out;
}

// ---------------------------------------------------------------------------
// Detokenizer (converts tokens back to text for LIST/SAVE)
// ---------------------------------------------------------------------------

static void appendStr(char* buf, int& out, int bufSize, const char* str)
{
  int slen = strlen(str);
  int copyLen = (out + slen < bufSize - 1) ? slen : bufSize - 1 - out;
  memcpy(&buf[out], str, copyLen);
  out += copyLen;
}

static int detokenizeLine(const uint8_t* tokens, int length, char* buf,
                          int bufSize)
{
  int pos = 0;
  int out = 0;

  while (pos < length && tokens[pos] != TOK_EOL)
  {
    uint8_t tok = tokens[pos++];

    // Raw ASCII identifier — variable name
    if (isIdentStart(tok))
    {
      if (out < bufSize - 1) buf[out++] = tok;
      while (pos < length && isIdentCont(tokens[pos]))
      {
        if (out < bufSize - 1) buf[out++] = tokens[pos];
        pos++;
      }
      continue;
    }

    // String literal
    if (tok == TOK_QUOTED_STR)
    {
      uint8_t slen = tokens[pos++];
      if (out + slen + 2 >= bufSize) break;
      buf[out++] = '"';
      memcpy(&buf[out], &tokens[pos], slen);
      out += slen;
      pos += slen;
      buf[out++] = '"';
      continue;
    }

    // Number / unquoted string
    if (tok == TOK_UNQUOTED_STR)
    {
      uint8_t slen = tokens[pos++];
      int copyLen = (out + slen < bufSize - 1) ? slen : bufSize - 1 - out;
      memcpy(&buf[out], &tokens[pos], copyLen);
      out += copyLen;
      pos += slen;
      continue;
    }

    // Operators
    switch (tok)
    {
      case TOK_PLUS:       appendStr(buf, out, bufSize, "+"); continue;
      case TOK_MINUS:      appendStr(buf, out, bufSize, "-"); continue;
      case TOK_MULTIPLY:   appendStr(buf, out, bufSize, "*"); continue;
      case TOK_DIVIDE:     appendStr(buf, out, bufSize, "/"); continue;
      case TOK_POWER:      appendStr(buf, out, bufSize, "^"); continue;
      case TOK_EQUAL:      appendStr(buf, out, bufSize, "="); continue;
      case TOK_LESS:       appendStr(buf, out, bufSize, "<"); continue;
      case TOK_GREATER:    appendStr(buf, out, bufSize, ">"); continue;
      // Compound comparisons (<=, <>, >=) are stored as two-token
      // sequences and reconstructed in the keyword loop below; we
      // don't need single-token cases for them here.
      case TOK_CONCAT:     appendStr(buf, out, bufSize, "&"); continue;
      case TOK_AND:        appendStr(buf, out, bufSize, " AND "); continue;
      case TOK_OR:         appendStr(buf, out, bufSize, " OR "); continue;
      case TOK_NOT:        appendStr(buf, out, bufSize, " NOT "); continue;
      case TOK_LPAREN:     appendStr(buf, out, bufSize, "("); continue;
      case TOK_RPAREN:     appendStr(buf, out, bufSize, ")"); continue;
      case TOK_COMMA:      appendStr(buf, out, bufSize, ","); continue;
      case TOK_SEMICOLON:  appendStr(buf, out, bufSize, ";"); continue;
      case TOK_COLON:      appendStr(buf, out, bufSize, ":"); continue;
      default: break;
    }

    // Keywords — look up in keyword table
    for (int i = 0; keywords[i].text != NULL; i++)
    {
      if (keywords[i].token == tok)
      {
        int klen = strlen(keywords[i].text);
        if (out + klen + 2 >= bufSize) break;
        if (out > 0 && buf[out - 1] != ' ') buf[out++] = ' ';
        memcpy(&buf[out], keywords[i].text, klen);
        out += klen;
        buf[out++] = ' ';
        break;
      }
    }
  }

  buf[out] = '\0';
  return out;
}

// ---------------------------------------------------------------------------
// Display driver
// ---------------------------------------------------------------------------

// LovyanGFX's autodetect probes the SPI bus, identifies the panel chip
// (ILI9342C on Box-3 V3), and configures pins, SPI host, backlight PWM,
// and reset handling automatically.
static LGFX tft;


static int cursorCol = 0;
static int cursorRow = 0;

// TI Extended BASIC colors (black text on cyan background)
static uint16_t fgColor = 0x0000;  // black
static uint16_t bgColor = 0x07FF;  // cyan

// Display framebuffer
static char screenBuf[ROWS][COLS];
static char prevScreenBuf[ROWS][COLS];

// TI color palette (indices 1-16) → RGB565
static const uint16_t tiPalette[17] =
{
  0x0000,   // 0 unused
  0x0000,   // 1 transparent (resolves to screen color in drawCell)
  0x0000,   // 2 black
  0x0585,   // 3 medium green
  0x2D8B,   // 4 light green
  0x0012,   // 5 dark blue
  0x0417,   // 6 light blue
  0x8000,   // 7 dark red
  0x0EBF,   // 8 cyan
  0xE000,   // 9 medium red
  0xF2A3,   // 10 light red
  0xD5C0,   // 11 dark yellow
  0xE600,   // 12 light yellow
  0x0280,   // 13 dark green
  0xB816,   // 14 magenta
  0xC618,   // 15 gray
  0xFFFF,   // 16 white
};

// Per-character color palette indices (1-16; 1=transparent=use screen color)
static uint8_t charFgIdx[256];
static uint8_t charBgIdx[256];
static uint8_t screenColorIdx = 8;   // cyan by default

static void initDisplay()
{
  tft.init();
  tft.setRotation(1);            // landscape, USB on the right
  tft.setColorDepth(16);
  tft.setSwapBytes(true);        // pushImage data is little-endian uint16_t;
                                 // panel wants big-endian RGB565.
  tft.setBrightness(192);        // 0..255 backlight PWM duty
  tft.fillScreen(bgColor);
  tft.setTextSize(1);
  tft.setTextColor(fgColor, bgColor);
}

// Resolve a palette index to RGB565, with transparency (1) falling through
// to the screen color.
static uint16_t resolveColor(uint8_t idx)
{
  if (idx < 1 || idx > 16) return 0;
  if (idx == 1)
  {
    return tiPalette[screenColorIdx];
  }
  return tiPalette[idx];
}

static void drawCell(int col, int row)
{
  uint8_t ch = (uint8_t)screenBuf[row][col];
  int px = col * CHAR_W + DISPLAY_X_OFFSET;
  int py = row * CHAR_H + DISPLAY_Y_OFFSET;

  uint16_t fg = resolveColor(charFgIdx[ch]);
  uint16_t bg = resolveColor(charBgIdx[ch]);

  uint16_t pixBuf[64];
  for (int y = 0; y < 8; y++)
  {
    uint8_t bits = charPatterns[ch][y];
    for (int x = 0; x < 8; x++)
    {
      pixBuf[y * 8 + x] = (bits & 0x80) ? fg : bg;
      bits <<= 1;
    }
  }
  // LovyanGFX equivalent of Adafruit's drawRGBBitmap (note arg order swap)
  tft.pushImage(px, py, 8, 8, pixBuf);
}

static void refreshScreen()
{
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      if (screenBuf[r][c] != prevScreenBuf[r][c])
      {
        drawCell(c, r);
        prevScreenBuf[r][c] = screenBuf[r][c];
      }
    }
  }
}

static void redrawScreen()
{
  // 24x32 = 768 cells * ~150us per drawCell = ~115ms — over the 5s WDT
  // limit only if stacked with other work, but a per-row yield is cheap
  // (24 yields) and keeps IDLE happy during boot screen + CALL CLEAR.
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      drawCell(c, r);
      prevScreenBuf[r][c] = screenBuf[r][c];
    }
    tiYield();
  }
}

static void scrollUp()
{
  memcpy(&screenBuf[0][0], &screenBuf[1][0], COLS * (ROWS - 1));
  memset(&screenBuf[ROWS - 1][0], 0x20, COLS);
  refreshScreen();
  spriteRedrawAll();    // sprites sit on top of the char grid
  int y = (ROWS - 1) * CHAR_H + DISPLAY_Y_OFFSET;
  tft.fillRect(DISPLAY_X_OFFSET, y, COLS * CHAR_W, CHAR_H, bgColor);
}

// Host strong override of ti_platform.h's weak tiYield().
//
// The interpreter, file I/O, and any other long-running BASIC code path
// calls this so the FreeRTOS scheduler can run lower-priority tasks —
// in particular the per-CPU IDLE task. delay(1) puts the calling task
// into the Blocked state for one FreeRTOS tick (~1 ms at HZ=1000),
// which is enough for IDLE1 to run, do its housekeeping, and reset its
// task-watchdog subscription. Arduino's bare yield() / FreeRTOS's
// taskYIELD() only switch among same-or-higher-priority tasks and
// would NOT let IDLE run.
void tiYield()
{
  delay(1);
}

void tiPrintChar(char c)
{
  // Mirror output to serial terminal for copy/paste
  Serial.write(c);
  if (c == '\n')
  {
    Serial.write('\r');
  }

  // TI behavior: cursor always on bottom row (ROWS-1 = 23).
  // '\n' scrolls up one row and resets column to 0.
  if (c == '\n')
  {
    scrollUp();
    cursorRow = ROWS - 1;
    cursorCol = 0;
    return;
  }

  // Column wrap — scroll up and start fresh at col 0
  if (cursorCol >= COLS)
  {
    scrollUp();
    cursorRow = ROWS - 1;
    cursorCol = 0;
  }

  screenBuf[cursorRow][cursorCol] = c;
  drawCell(cursorCol, cursorRow);
  prevScreenBuf[cursorRow][cursorCol] = c;
  cursorCol++;
}

void tiPrintString(const char* str)
{
  while (*str)
  {
    tiPrintChar(*str++);
  }
}

static void printLine(const char* str)
{
  tiPrintString(str);
  tiPrintChar('\n');
}

// TI-style error print: blank line, error message, blank line, plus a
// BEL (0x07) to the serial terminal so monitors that honor it beep.
static void printError(const char* str)
{
  printLine("");
  printLine(str);
  printLine("");
  Serial.write(0x07);
  // TI-authentic "HONK" — short 220 Hz buzz on any error.
  tiSoundPlay(200, 220, 0, 0, 30, 0, 30, 0, 30);
}

void tiClearScreen()
{
  memset(screenBuf, ' ', COLS * ROWS);
  fillBackground(bgColor);
  // TI behavior: cursor on bottom row after CLEAR
  cursorCol = 0;
  cursorRow = ROWS - 1;
}

// Move cursor to bottom row for INPUT (TI behavior — INPUT always at row 24).
// With the new print model, we just need to ensure we're at col 0.
static void gfxPrepareInput()
{
  if (cursorCol > 0)
  {
    tiPrintChar('\n');
  }
}

// Reset graphics to editor defaults (called when program ends)
// Reset all character colors to default: fg=black(2), bg=transparent(1).
// Screen color defaults to cyan(8), so transparent bg shows cyan.
static void gfxResetColors()
{
  for (int i = 0; i < 256; i++)
  {
    charFgIdx[i] = 2;   // black
    charBgIdx[i] = 1;   // transparent (→ screen color)
  }
  screenColorIdx = 8;   // cyan
  fgColor = tiPalette[2];
  bgColor = tiPalette[8];
}

static void gfxReset()
{
  gfxResetColors();
  initCharPatterns();
  // Repaint the whole panel so the border drops the old CALL SCREEN
  // color (drawCell only paints inside the 32x24 char grid). On the
  // Box-3 SPI panel via LovyanGFX, the brief fill is unnoticeable.
  fillBackground(bgColor);
  redrawScreen();
}

// Graphics callbacks for CALL HCHAR, VCHAR, GCHAR, SCREEN, COLOR

void tiSetChar(int row, int col, char ch)
{
  if (row < 0 || row >= ROWS || col < 0 || col >= COLS) return;
  screenBuf[row][col] = ch;
  drawCell(col, row);
  prevScreenBuf[row][col] = ch;
}

char tiGetChar(int row, int col)
{
  if (row < 0 || row >= ROWS || col < 0 || col >= COLS) return 32;
  return screenBuf[row][col];
}

void tiSetScreenColor(int colorIdx)
{
  if (colorIdx < 1 || colorIdx > 16) return;
  screenColorIdx = colorIdx;
  bgColor = tiPalette[colorIdx];
  // Paint the entire panel including the border (area outside the
  // 32x24 char grid), then redraw chars on top. Without the
  // fillBackground call, the border keeps the old color even though
  // the inner grid changes.
  fillBackground(bgColor);
  redrawScreen();
  spriteRedrawAll();
}

// CALL COLOR(set, fg, bg) — sets colors for a group of 8 characters.
// Extended BASIC character sets:
//   Set 1 = chars 32-39, Set 2 = 40-47, ... Set 16 = 152-159
// Move cursor to a specific position (for DISPLAY AT, ACCEPT AT).
// row, col are 0-based.
void tiMoveCursor(int row, int col)
{
  if (row < 0) row = 0;
  if (row >= ROWS) row = ROWS - 1;
  if (col < 0) col = 0;
  if (col >= COLS) col = COLS - 1;
  cursorRow = row;
  cursorCol = col;
}

// CALL KEY: read one key from Serial or BLE keyboard without blocking.
// Returns 0 if no key available, else the character code.
int tiReadKey()
{
  if (Serial.available())
  {
    return Serial.read() & 0xFF;
  }
  if (bleKbAvailable())
  {
    return bleKbRead() & 0xFF;
  }
  return 0;
}

// CALL CHAR: redefine a character's 8x8 bitmap pattern
void tiSetCharPattern(int charCode, const uint8_t* pattern)
{
  if (charCode < 0 || charCode > 255) return;
  memcpy(charPatterns[charCode], pattern, 8);
  // Redraw any cells using this character
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      if ((uint8_t)screenBuf[r][c] == (uint8_t)charCode)
      {
        drawCell(c, r);
      }
    }
  }
}

// CALL CHARPAT: read a character's current 8×8 pattern
void tiGetCharPattern(int charCode, uint8_t* out)
{
  if (charCode < 0 || charCode > 255)
  {
    memset(out, 0, 8);
    return;
  }
  memcpy(out, charPatterns[charCode], 8);
}

// CALL CHARSET: reset characters 32-127 to their ROM default patterns.
// Leaves user-defined graphics slots (128+) alone. Uses whichever font
// ROM is currently active (PC default or V9T9 TI authentic).
void tiResetCharset()
{
  resetBasicChars();
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      uint8_t ch = (uint8_t)screenBuf[r][c];
      if (ch >= 32 && ch < 128) drawCell(c, r);
    }
  }
}

// CALL CHARSET("PC" | "TI"): switch the active font ROM and persist the
// choice to NVS so it survives reboot. The interpreter calls this *before*
// it calls tiResetCharset, so we just update the mode here and let the
// CHARSET reset do the actual pattern + screen refresh.
//
// Mode values: 0 = TI_FONT_PC (PC-style lowercase), 1 = TI_FONT_TI (V9T9
// authentic TI font with small-caps lowercase).
void tiSetCharsetMode(int mode)
{
  TiFontMode m = (mode == TI_FONT_TI) ? TI_FONT_TI : TI_FONT_PC;
  if (m == getTiFontMode()) return;   // no-op when unchanged
  setTiFontMode(m);
  Preferences prefs;
  prefs.begin("boxbasic", false);
  prefs.putUChar("fontmode", (uint8_t)m);
  prefs.end();
  Serial.printf("tiSetCharsetMode: now %s\n", m == TI_FONT_TI ? "TI" : "PC");
}

void tiSetCharColor(int charSet, int fg, int bg)
{
  if (charSet < 1 || charSet > 16) return;
  if (fg < 1 || fg > 16) return;
  if (bg < 1 || bg > 16) return;

  int firstChar = 32 + (charSet - 1) * 8;
  for (int i = 0; i < 8; i++)
  {
    int ch = firstChar + i;
    if (ch >= 0 && ch < 256)
    {
      charFgIdx[ch] = (uint8_t)fg;
      charBgIdx[ch] = (uint8_t)bg;
    }
  }

  // Redraw any cells using characters in this set
  for (int r = 0; r < ROWS; r++)
  {
    for (int c = 0; c < COLS; c++)
    {
      uint8_t ch = (uint8_t)screenBuf[r][c];
      if (ch >= firstChar && ch < firstChar + 8)
      {
        drawCell(c, r);
      }
    }
  }
}

// Fill the display: char area = bgColor, borders (outside 24 rows) = black.
static void fillBackground(uint16_t bg)
{
  // Fill the entire panel with the screen color so the TI border (the
  // area outside the 32x24 char grid) matches the screen color, as on a
  // real TI-99/4A.
  tft.fillScreen(bg);
}

// TI-Texas logo — 3×3 character grid taken straight from the TI title
// screen. Each entry is the CALL CHAR pattern for one cell (row,col).
// Cells are laid out row-major: 129=(1,1) 130=(1,2) 131=(1,3)
//                                132=(2,1) 133=(2,2) 134=(2,3)
//                                135=(3,1) 136=(3,2) 137=(3,3)
static const uint8_t tiLogoChars[9][8] =
{
  {0x00, 0x01, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03},  // (1,1)
  {0x00, 0xFC, 0x04, 0x05, 0x05, 0x04, 0x06, 0x02},  // (1,2)
  {0x00, 0x00, 0x80, 0x40, 0x40, 0x80, 0x00, 0x0C},  // (1,3)
  {0x03, 0xFF, 0x80, 0xC0, 0x40, 0x60, 0x38, 0x1C},  // (2,1)
  {0x0C, 0x19, 0x21, 0x21, 0x3D, 0x05, 0x05, 0x05},  // (2,2)
  {0x12, 0xBA, 0x8A, 0x8A, 0xBA, 0xA1, 0xA1, 0xA1},  // (2,3)
  {0x0E, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00},  // (3,1)
  {0xC4, 0xE2, 0x31, 0x10, 0x18, 0x0C, 0x07, 0x03},  // (3,2)
  {0x22, 0x4C, 0x90, 0x20, 0x40, 0x40, 0x20, 0xE0},  // (3,3)
};

// Redefine char codes 129..137 with the logo patterns and place them in
// a 3×3 grid on screen with the top-left at (startRow, startCol).
static void drawTexasLogo(int startRow, int startCol)
{
  for (int i = 0; i < 9; i++)
  {
    memcpy(charPatterns[129 + i], tiLogoChars[i], 8);
  }
  for (int r = 0; r < 3; r++)
  {
    for (int c = 0; c < 3; c++)
    {
      int ch = 129 + r * 3 + c;
      screenBuf[startRow + r][startCol + c] = (char)ch;
      drawCell(startCol + c, startRow + r);
    }
  }
}

// 8x8 copyright glyph (©) used in the splash-screen copyright line.
static const uint8_t copyrightBitmap[8] =
{
  0x3C,  // ..####..
  0x42,  // .#....#.
  0x99,  // #..##..#
  0xA1,  // #.#....#
  0xA1,  // #.#....#
  0x99,  // #..##..#
  0x42,  // .#....#.
  0x3C,  // ..####..
};

// TI-99/4A boot screen: colored stripes top and bottom, centered text.
// Pattern approximates the 1981 TI home computer startup screen.
static void showBootScreen()
{
  // While the splash is up, run BLE scan in aggressive TITLE mode so a
  // keyboard the user just powered on connects within a second or two.
  // The setup() flow (and the recursive re-show paths below) flip back
  // to INTERACTIVE after this function returns; nested re-entries
  // re-arm TITLE on each entry, which is the desired behavior.
  bleKbSetMode(BLE_KB_TITLE);

  // Clear display to cyan in the char area, black outside
  fillBackground(tiPalette[8]);

  // Redefine char 128 as the © copyright symbol
  memcpy(charPatterns[128], copyrightBitmap, 8);

  // Stripe colors (approximating the TI pattern left to right)
  const uint8_t stripes[] = {
    9, 4, 2, 12, 13, 14,        // left group
    5, 3, 14, 9, 15, 6, 10, 12, 9   // right group
  };
  const int numStripes = sizeof(stripes);

  // Top and bottom colored bars: 15 stripes + 1 gap = 16 slots spanning
  // the full character-area width.
  const int stripeW = (COLS * CHAR_W) / 16;
  const int stripeH = 24;        // 3 rows × 8px
  const int gapEnd  = 7 * stripeW;   // gap occupies slot 6 (after 6 stripes)

  // Top band — TI rows 1-3
  int topY = DISPLAY_Y_OFFSET;
  for (int i = 0; i < numStripes; i++)
  {
    int x = DISPLAY_X_OFFSET +
            ((i < 6) ? i * stripeW : (i - 6) * stripeW + gapEnd);
    tft.fillRect(x, topY, stripeW, stripeH, tiPalette[stripes[i]]);
  }

  // Bottom band — TI rows 19-21 (0-indexed 18-20)
  int bottomY = DISPLAY_Y_OFFSET + 18 * CHAR_H;
  for (int i = 0; i < numStripes; i++)
  {
    int x = DISPLAY_X_OFFSET +
            ((i < 6) ? i * stripeW : (i - 6) * stripeW + gapEnd);
    tft.fillRect(x, bottomY, stripeW, stripeH, tiPalette[stripes[i]]);
  }

  // Draw centered text directly via the framebuffer.
  // Our display is 28 cols; "TEXAS INSTRUMENTS" (17) → col 5 start.
  auto drawText = [](const char* text, int row) {
    int len = strlen(text);
    int col = (COLS - len) / 2;
    if (col < 0) col = 0;
    for (int i = 0; i < len && col + i < COLS; i++)
    {
      screenBuf[row][col + i] = text[i];
      drawCell(col + i, row);
    }
  };

  // Texas logo — 3×3 char grid at TI rows 6-8 (0-indexed 5-7)
  drawTexasLogo(5, (COLS - 3) / 2);

  drawText("TEXAS INSTRUMENTS",             9);   // TI row 10
  drawText("HOME COMPUTER",                11);   // TI row 12
  drawText("READY-PRESS ANY KEY TO BEGIN", 16);   // TI row 17
  drawText("\x80" "1981    TEXAS INSTRUMENTS", 22);   // TI row 23 with ©

  Serial.println("PRESS ANY KEY TO CONTINUE");

  // Wait for any key — from Serial or BLE keyboard. Keep BLE scanning
  // alive so reconnect can complete while we're sitting here.
  // Wait for any key — from Serial or BLE keyboard. Keep BLE scanning
  // alive so reconnect can complete while we're sitting here. Also
  // handle the BLE-pairing takeover so a BOOT-button press here
  // brings up the pairing UI immediately and restores the title
  // screen when the pairing window closes.
  {
    bool prevPair = false;
    bool wasInBootUI = true;
    unsigned long lastCount = 0;
    while (!Serial.available() && !bleKbAvailable())
    {
      bleKbTask();
      webfiles::tick();
      bool nowPair = BleHidHost::userInitiatedPairing();
      if (nowPair != prevPair)
      {
        if (nowPair)
        {
          drawPairingScreen();
          wasInBootUI = false;
        }
        else if (!wasInBootUI)
        {
          // Pairing window closed — redraw the boot UI we hid.
          fillBackground(tiPalette[8]);
          showBootScreen();
          // showBootScreen has its own wait loops, so we'd recurse;
          // bail out of THIS wait instead and let the recursion
          // handle further input.
          return;
        }
        prevPair = nowPair;
        lastCount = 0;
      }
      if (nowPair)
      {
        unsigned long now = millis();
        if (now - lastCount >= 500)
        {
          updatePairingCountdown(BleHidHost::pairingRemainingMs());
          lastCount = now;
        }
      }
      yield();
      delay(10);
    }
  }
  while (Serial.available()) { Serial.read(); delay(2); }
  while (bleKbAvailable())   { bleKbRead();  delay(2); }

  // TI-authentic key-acknowledge beep: F6 (1397 Hz) for 166 ms.
  tiSoundPlay(166, 1397, 0, 0, 30, 0, 30, 0, 30);

  // Clear and show the menu screen
  fillBackground(tiPalette[8]);
  for (int r = 0; r < ROWS; r++)
  {
    memset(screenBuf[r], ' ', COLS);
    memset(prevScreenBuf[r], 0, COLS);
  }

  auto drawText2 = [](const char* text, int row, int col) {
    int len = strlen(text);
    for (int i = 0; i < len && col + i < COLS; i++)
    {
      screenBuf[row][col + i] = text[i];
      drawCell(col + i, row);
    }
  };

  drawText2("TEXAS INSTRUMENTS",     0, 5);
  drawText2("HOME COMPUTER",         1, 7);
  drawText2("PRESS",                 3, 2);
  drawText2("1 FOR TI BASIC",        5, 2);
  drawText2("2 FOR TI EXTENDED BASIC", 7, 2);

  Serial.println("PRESS 1 OR 2 TO CONTINUE");

  // Wait for any key — from Serial or BLE keyboard. Keep BLE scanning
  // alive so reconnect can complete while we're sitting here. Also
  // handle the BLE-pairing takeover so a BOOT-button press here
  // brings up the pairing UI immediately and restores the title
  // screen when the pairing window closes.
  {
    bool prevPair = false;
    bool wasInBootUI = true;
    unsigned long lastCount = 0;
    while (!Serial.available() && !bleKbAvailable())
    {
      bleKbTask();
      webfiles::tick();
      bool nowPair = BleHidHost::userInitiatedPairing();
      if (nowPair != prevPair)
      {
        if (nowPair)
        {
          drawPairingScreen();
          wasInBootUI = false;
        }
        else if (!wasInBootUI)
        {
          // Pairing window closed — redraw the boot UI we hid.
          fillBackground(tiPalette[8]);
          showBootScreen();
          // showBootScreen has its own wait loops, so we'd recurse;
          // bail out of THIS wait instead and let the recursion
          // handle further input.
          return;
        }
        prevPair = nowPair;
        lastCount = 0;
      }
      if (nowPair)
      {
        unsigned long now = millis();
        if (now - lastCount >= 500)
        {
          updatePairingCountdown(BleHidHost::pairingRemainingMs());
          lastCount = now;
        }
      }
      yield();
      delay(10);
    }
  }
  while (Serial.available()) { Serial.read(); delay(2); }
  while (bleKbAvailable())   { bleKbRead();  delay(2); }

  // TI-authentic key-acknowledge beep on 1/2 selection too.
  tiSoundPlay(166, 1397, 0, 0, 30, 0, 30, 0, 30);

  tiClearScreen();
}

static void showStatus(const char* /*msg*/)
{
  // No-op on Box-3 — the bottom status strip was removed, and Serial
  // output was too noisy in the monitor. Callers still pass formatted
  // status strings; we just drop them.
}

// Full-screen takeover while BLE pairing is active. The user
// triggered pairing (BOOT button or watchdog), so they're not using
// BASIC right now — we use the whole panel to clearly explain
// what's happening and show a live countdown.
static void drawPairingScreen()
{
  // Dark blue background, white text.
  tft.fillScreen(0x0012);
  tft.setTextColor(0xFFFF, 0x0012);

  tft.setTextSize(2);
  tft.setCursor(36, 30);
  tft.print("BLE PAIRING");

  tft.setTextSize(1);
  tft.setCursor(20, 70);
  tft.print("Press the PAIR button");
  tft.setCursor(20, 82);
  tft.print("on your keyboard now.");

  tft.setCursor(20, 110);
  tft.print("Watching for advertisers");
  tft.setCursor(20, 122);
  tft.print("with a HID profile...");

  tft.setCursor(20, 200);
  tft.setTextColor(0xC618, 0x0012);   // light gray
  tft.print("(Window expires in:        s)");
  tft.setTextColor(0xFFFF, 0x0012);
}

// Update only the dynamic countdown each tick. Avoids a full
// redraw every second.
static void updatePairingCountdown(unsigned long remainMs)
{
  // Clear just the digits area.
  tft.fillRect(170, 200, 30, 10, 0x0012);
  tft.setTextSize(1);
  tft.setTextColor(0xFFFF, 0x0012);
  tft.setCursor(170, 200);
  unsigned long s = (remainMs + 999) / 1000;
  tft.print(s);
  tft.setTextColor(fgColor, bgColor);
}

// ---------------------------------------------------------------------------
// Execution Manager instance (declared early so the line editor can look up
// program lines for line-number + UP-arrow recall)
// ---------------------------------------------------------------------------
static ExecManager em;

// ---------------------------------------------------------------------------
// Line editor (shared by getInputLine and checkInput)
//
// Supports TI-style keys: DEL (7), INS toggle (4), ERASE (2), CLEAR (12 =
// break), REDO (14), arrows (8/9/10/11), line-number + UP for recall.
// Single-row editing only — long lines aren't wrapped visually but are
// kept in the buffer and submitted intact on Enter.
// ---------------------------------------------------------------------------

static char inputBuf[MAX_INPUT_LEN + 1];
static int  inputPos = 0;          // len of input so far (for main loop)
static bool inputReady = false;

static char lastCommandLine[MAX_INPUT_LEN + 1] = {0};
static bool editInsertMode = false;

// NUMBER mode: when active, editorBeginLine pre-fills the prompt with the
// next auto-incrementing line number. Set by cmdNumber(); cleared when the
// user presses Enter on an empty line.
static int  numModeStart  = 0;
static int  numModeIncr   = 0;
static int  numModeNext   = 0;
static bool numModeActive = false;

// Current editor mode (see line_editor.h). Starts in ENTRY on every new
// line, flips to EDIT on successful recall (REDO or <N>+UP/<N>+DOWN).
static EditMode editMode = EM_ENTRY;

// The program line currently under edit — used by EDIT-mode UP/DOWN
// to move to the previous/next program line. -1 when not in EDIT mode.
static int lastRecalledLineNum = -1;

// Forward decls (needed by line-number recall and UP/DOWN commit)
static int detokenizeLine(const uint8_t* tokens, int length, char* buf,
                          int bufSize);
static int tokenizeLine(const char* src, uint8_t* tokens, int maxLen);

// Draw a solid block cursor at the current position (inverted colors)
static void drawCursor(bool visible)
{
  int px = cursorCol * CHAR_W + DISPLAY_X_OFFSET;
  int py = cursorRow * CHAR_H + DISPLAY_Y_OFFSET;
  if (visible)
  {
    tft.fillRect(px, py, CHAR_W, CHAR_H, tiPalette[2]);
  }
  else
  {
    drawCell(cursorCol, cursorRow);
  }
}

// Sync global cursorCol/cursorRow with the edit state's logical position
static void editSyncCursor(const LineEdit& s)
{
  int col = s.startCol + s.pos;
  if (col >= COLS) col = COLS - 1;
  cursorCol = col;
  cursorRow = s.startRow;
}

// Redraw buffer content starting at `fromPos`, plus `eraseExtra` trailing
// cells (used after a shrink). Single-row only.
static void redrawLineTail(const LineEdit& s, int fromPos, int eraseExtra)
{
  for (int i = fromPos; i < s.len; i++)
  {
    int col = s.startCol + i;
    if (col >= COLS) break;
    screenBuf[s.startRow][col] = s.buf[i];
    drawCell(col, s.startRow);
  }
  for (int i = 0; i < eraseExtra; i++)
  {
    int col = s.startCol + s.len + i;
    if (col >= COLS) break;
    screenBuf[s.startRow][col] = ' ';
    drawCell(col, s.startRow);
  }
}

// Double-buffered serial input for fast paste.
//
// Serial.available()/Serial.read() is called by the editor at the editor's
// own pace — about one character per display refresh. That's far slower
// than USB CDC can deliver bytes during a paste, so the Arduino-CDC ring
// buffer (~4 KB) overflows and lines get dropped.
//
// pasteDrainSerial() sucks every byte out of Serial into a much larger
// local ring buffer as often as we can call it (top of loop, inside the
// editor's blink/yield points, inside getInputLine, etc.). editorReadChar
// then reads from this buffer, decoupling the producer (USB) from the
// consumer (display-bound editor) entirely.
#define PASTE_BUF_SIZE 16384
static uint8_t pasteBuf[PASTE_BUF_SIZE];
static int pasteHead = 0;
static int pasteTail = 0;

static void pasteDrainSerial()
{
  while (Serial.available())
  {
    int next = (pasteHead + 1) % PASTE_BUF_SIZE;
    if (next == pasteTail) break;   // full — back-pressure onto Serial
    pasteBuf[pasteHead] = (uint8_t)Serial.read();
    pasteHead = next;
  }
}

static bool pasteAvailable()
{
  return pasteHead != pasteTail;
}

static int pasteRead()
{
  if (pasteHead == pasteTail) return -1;
  uint8_t c = pasteBuf[pasteTail];
  pasteTail = (pasteTail + 1) % PASTE_BUF_SIZE;
  return c;
}

// Reads the next editor byte from the paste buffer (Serial side) or BLE,
// normalizing Serial line endings (\r\n → one Enter; lone \n → \r) and
// dropping tabs (since \t = RIGHT-arrow in the TI encoding).
// Returns -1 if nothing is available.
static int editorReadChar()
{
  static bool skipNextLf = false;

  // Top up from Serial before each read so we don't block on the editor's
  // pace while USB has more bytes waiting.
  pasteDrainSerial();

  while (pasteAvailable())
  {
    uint8_t c = (uint8_t)pasteRead();
    if (c == '\r') { skipNextLf = true;  return '\r'; }
    if (c == '\n')
    {
      if (skipNextLf) { skipNextLf = false; continue; }
      return '\r';
    }
    skipNextLf = false;
    if (c == '\t') continue;
    return c;
  }

  if (bleKbAvailable())
  {
    return bleKbRead();
  }
  return -1;
}

// Find the program index of a line with this lineNum, or -1 if absent.
static int findProgramLineIndex(int lineNum)
{
  for (int i = 0; i < em.programSize(); i++)
  {
    if (em.getLine(i)->lineNum == lineNum) return i;
  }
  return -1;
}

// Replace buffer contents with `src`. Used by REDO and line-number recall.
static void editReplaceLine(LineEdit& s, const char* src)
{
  int oldLen = s.len;
  int n = 0;
  while (src[n] && n < s.maxLen)
  {
    s.buf[n] = src[n];
    n++;
  }
  s.buf[n] = '\0';
  s.len = n;
  s.pos = 0;
  redrawLineTail(s, 0, (oldLen > s.len) ? (oldLen - s.len) : 0);
  editSyncCursor(s);
}

// Re-tokenize the current edit buffer and store it back into the program.
// Called before UP/DOWN navigation so edits to the line are preserved as
// if Enter had been pressed. Returns false only on tokenize failure.
static bool commitEditedLine(const LineEdit& s)
{
  int p = 0;
  while (p < s.len && s.buf[p] == ' ') p++;
  if (p >= s.len) return true;
  if (!isdigit((unsigned char)s.buf[p])) return true;

  int lineNum = 0;
  while (p < s.len && isdigit((unsigned char)s.buf[p]))
  {
    lineNum = lineNum * 10 + (s.buf[p] - '0');
    p++;
  }
  while (p < s.len && s.buf[p] == ' ') p++;

  if (p >= s.len)
  {
    em.deleteLine((uint16_t)lineNum);
    return true;
  }

  uint8_t toks[MAX_LINE_TOKENS];
  int len = tokenizeLine(&s.buf[p], toks, MAX_LINE_TOKENS);
  if (len < 0) return false;
  em.storeLine((uint16_t)lineNum, toks, len);
  return true;
}

// Load the program line at `idx` into the edit buffer, flip to EDIT mode,
// and remember the line number so subsequent UP/DOWN browse prev/next.
static void loadProgramLineToEdit(LineEdit& s, int idx)
{
  ProgramLine* pl = em.getLine(idx);
  if (!pl) return;
  char tmp[MAX_INPUT_LEN + 1];
  int n = snprintf(tmp, sizeof(tmp), "%d ", pl->lineNum);
  detokenizeLine(pl->tokens, pl->length, &tmp[n], sizeof(tmp) - n);
  editReplaceLine(s, tmp);
  lastRecalledLineNum = pl->lineNum;
  editMode = EM_EDIT;
}

// Test whether the current buffer contains nothing but decimal digits.
static bool editBufferIsAllDigits(const LineEdit& s)
{
  if (s.len == 0) return false;
  for (int i = 0; i < s.len; i++)
  {
    if (!isdigit((unsigned char)s.buf[i])) return false;
  }
  return true;
}

// Remove the char at `s.pos` (if any) and redraw the tail.
static void editDeleteAtCursor(LineEdit& s)
{
  if (s.pos >= s.len) return;
  for (int i = s.pos; i < s.len - 1; i++) s.buf[i] = s.buf[i + 1];
  s.len--;
  s.buf[s.len] = '\0';
  redrawLineTail(s, s.pos, 1);
  editSyncCursor(s);
}

// Backspace: move cursor left then delete the char now under it.
static void editBackspace(LineEdit& s)
{
  if (s.pos == 0) return;
  s.pos--;
  editDeleteAtCursor(s);
}

// Insert or overwrite `c` at the cursor and advance.
static void editTypeChar(LineEdit& s, uint8_t c)
{
  if (s.len >= s.maxLen) return;

  if (editInsertMode && s.pos < s.len)
  {
    for (int i = s.len; i > s.pos; i--) s.buf[i] = s.buf[i - 1];
    s.buf[s.pos] = c;
    s.len++;
    s.buf[s.len] = '\0';
    redrawLineTail(s, s.pos, 0);
    s.pos++;
    editSyncCursor(s);
  }
  else
  {
    s.buf[s.pos] = c;
    if (s.pos == s.len)
    {
      s.len++;
      s.buf[s.len] = '\0';
    }
    int col = s.startCol + s.pos;
    if (col < COLS)
    {
      screenBuf[s.startRow][col] = c;
      drawCell(col, s.startRow);
    }
    Serial.write(c);       // mirror to serial for paste visibility
    s.pos++;
    editSyncCursor(s);
  }
}

// Wipe the current line and return to ENTRY mode.
static void editEraseLine(LineEdit& s)
{
  int oldLen = s.len;
  s.len = 0;
  s.pos = 0;
  s.buf[0] = '\0';
  redrawLineTail(s, 0, oldLen);
  editSyncCursor(s);
  editMode = EM_ENTRY;
  lastRecalledLineNum = -1;
}

static EditResult processEditChar(uint8_t c, LineEdit& s)
{
  // ----- handled identically in both modes -----

  // Enter: commit line. Only match '\r' — '\n' (10) is DOWN on TI.
  // Serial's '\n' is normalized to '\r' at the read site.
  if (c == '\r')
  {
    // NUMBER mode: if user pressed Enter without adding anything past the
    // auto-fill, exit NUMBER mode and throw away the buffer so we don't
    // accidentally delete the line with that number.
    if (numModeActive && s.historyEnabled && editorBufferIsAutoFillOnly(s))
    {
      numModeActive = false;
      s.len = 0;
      s.pos = 0;
      s.buf[0] = '\0';
    }
    s.buf[s.len] = '\0';
    if (s.historyEnabled)
    {
      strncpy(lastCommandLine, s.buf, sizeof(lastCommandLine) - 1);
      lastCommandLine[sizeof(lastCommandLine) - 1] = '\0';
    }
    cursorCol = s.startCol + s.len;
    if (cursorCol >= COLS) cursorCol = COLS - 1;
    cursorRow = s.startRow;
    tiPrintChar('\n');
    editMode = EM_ENTRY;
    lastRecalledLineNum = -1;
    return EDIT_SUBMITTED;
  }

  // CLEAR — break
  if (c == 12) return EDIT_BROKEN;

  // ERASE (FCTN+3) — wipe line in either mode, drop back to ENTRY
  if (c == 2)
  {
    editEraseLine(s);
    return EDIT_CONTINUE;
  }

  // INS (FCTN+2) — toggle insert mode (global flag, both edit modes)
  if (c == 4)
  {
    editInsertMode = !editInsertMode;
    return EDIT_CONTINUE;
  }

  // REDO (FCTN+8) — reload last-entered line, flip to EDIT
  if (c == 14)
  {
    if (s.historyEnabled && lastCommandLine[0] != '\0')
    {
      editReplaceLine(s, lastCommandLine);
      editMode = EM_EDIT;
    }
    return EDIT_CONTINUE;
  }

  // BKSP (127) — delete previous char. Works in both modes since cursor
  // naturally sits at end during ENTRY.
  if (c == 127)
  {
    editBackspace(s);
    return EDIT_CONTINUE;
  }

  // Printable — typing always feeds the buffer
  if (c >= 32 && c < 127)
  {
    editTypeChar(s, c);
    return EDIT_CONTINUE;
  }

  // ----- Cursor movement & DEL work in every edit context -----

  // LEFT (8, FCTN+S) — cursor left
  if (c == 8)
  {
    if (s.pos > 0) { s.pos--; editSyncCursor(s); }
    return EDIT_CONTINUE;
  }

  // RIGHT (9, FCTN+D) — cursor right
  if (c == 9)
  {
    if (s.pos < s.len) { s.pos++; editSyncCursor(s); }
    return EDIT_CONTINUE;
  }

  // DEL (7, FCTN+1) — delete char at cursor
  if (c == 7)
  {
    editDeleteAtCursor(s);
    return EDIT_CONTINUE;
  }

  // ----- UP/DOWN are mode-aware -----
  //
  //   INPUT (historyEnabled=false): no-op
  //   ENTRY (editor prompt, not yet recalled): if the buffer is all digits,
  //     jump to EDIT mode on that program line
  //   EDIT  (a line is currently under edit): commit the current buffer,
  //     then move to the previous/next program line; past the boundary
  //     exits EDIT mode

  if (c == 11)   // UP (FCTN+E)
  {
    if (!s.historyEnabled) return EDIT_CONTINUE;

    if (editMode == EM_ENTRY)
    {
      if (editBufferIsAllDigits(s))
      {
        int idx = findProgramLineIndex(atoi(s.buf));
        if (idx >= 0) loadProgramLineToEdit(s, idx);
      }
      return EDIT_CONTINUE;
    }

    // EM_EDIT — commit and navigate to previous line
    int oldLine = lastRecalledLineNum;
    if (!commitEditedLine(s))
    {
      printError("* SYNTAX ERROR");
      editEraseLine(s);
      return EDIT_CONTINUE;
    }
    int idx = findProgramLineIndex(oldLine);
    if (idx < 0) { editEraseLine(s); return EDIT_CONTINUE; }
    if (idx > 0)
    {
      tiPrintChar('\n');
      s.startCol = cursorCol;
      s.startRow = cursorRow;
      s.len = 0; s.pos = 0; s.buf[0] = '\0';
      loadProgramLineToEdit(s, idx - 1);
    }
    else
    {
      editEraseLine(s);
    }
    return EDIT_CONTINUE;
  }

  if (c == 10)   // DOWN (FCTN+X)
  {
    if (!s.historyEnabled) return EDIT_CONTINUE;

    if (editMode == EM_ENTRY)
    {
      if (editBufferIsAllDigits(s))
      {
        int idx = findProgramLineIndex(atoi(s.buf));
        if (idx >= 0) loadProgramLineToEdit(s, idx);
      }
      return EDIT_CONTINUE;
    }

    // EM_EDIT — commit and navigate to next line
    int oldLine = lastRecalledLineNum;
    if (!commitEditedLine(s))
    {
      printError("* SYNTAX ERROR");
      editEraseLine(s);
      return EDIT_CONTINUE;
    }
    int idx = findProgramLineIndex(oldLine);
    if (idx < 0) { editEraseLine(s); return EDIT_CONTINUE; }
    if (idx < em.programSize() - 1)
    {
      tiPrintChar('\n');
      s.startCol = cursorCol;
      s.startRow = cursorRow;
      s.len = 0; s.pos = 0; s.buf[0] = '\0';
      loadProgramLineToEdit(s, idx + 1);
    }
    else
    {
      editEraseLine(s);
    }
    return EDIT_CONTINUE;
  }

  return EDIT_CONTINUE;
}

static bool getInputLine(char* buf, int bufSize)
{
  LineEdit s = { buf, bufSize - 1, 0, 0, cursorCol, cursorRow, false };
  buf[0] = '\0';

  bool cursorVisible = false;
  unsigned long lastBlink = 0;
  const unsigned long BLINK_MS = 400;

  while (true)
  {
    unsigned long now = millis();
    if (now - lastBlink >= BLINK_MS)
    {
      cursorVisible = !cursorVisible;
      editSyncCursor(s);
      drawCursor(cursorVisible);
      lastBlink = now;
    }

    bleKbTask();
    webfiles::tick();

    int c;
    while ((c = editorReadChar()) >= 0)
    {
      if (cursorVisible)
      {
        editSyncCursor(s);
        drawCursor(false);
        cursorVisible = false;
      }

      EditResult r = processEditChar((uint8_t)c, s);
      if (r == EDIT_SUBMITTED) return true;
      if (r == EDIT_BROKEN)    return false;   // INPUT aborted
    }
    yield();
  }
}

// ---------------------------------------------------------------------------
// Editor prompt (non-blocking, called from loop)
// ---------------------------------------------------------------------------
static bool editorCursorVisible = false;
static unsigned long editorLastBlink = 0;
static const unsigned long EDITOR_BLINK_MS = 400;

static LineEdit editorState = { inputBuf, MAX_INPUT_LEN, 0, 0, 0, 0, true };
static bool editorLineActive = false;   // true between start-of-line and Enter

static void editorBeginLine()
{
  editorState.buf = inputBuf;
  editorState.maxLen = MAX_INPUT_LEN;
  editorState.len = 0;
  editorState.pos = 0;
  editorState.startCol = cursorCol;
  editorState.startRow = cursorRow;
  editorState.historyEnabled = true;
  inputBuf[0] = '\0';
  inputPos = 0;
  editorLineActive = true;

  // NUMBER mode: pre-fill with the next auto line number + space
  if (numModeActive)
  {
    char numStr[8];
    int n = snprintf(numStr, sizeof(numStr), "%d ", numModeNext);
    for (int i = 0; i < n; i++)
    {
      editTypeChar(editorState, (uint8_t)numStr[i]);
    }
    numModeNext += numModeIncr;
  }
}

// True if the buffer is exactly "<digits> " (at least one trailing space)
// with nothing else — the NUMBER-mode auto-fill form. Lets us detect
// "Enter without adding anything" so we can exit NUMBER mode cleanly
// instead of deleting the line.
static bool editorBufferIsAutoFillOnly(const LineEdit& s)
{
  if (s.len == 0) return false;
  int p = 0;
  if (!isdigit((unsigned char)s.buf[p])) return false;
  while (p < s.len && isdigit((unsigned char)s.buf[p])) p++;
  if (p >= s.len || s.buf[p] != ' ') return false;
  while (p < s.len && s.buf[p] == ' ') p++;
  return p >= s.len;
}

static void editorCursorTick()
{
  if (!editorLineActive) return;
  unsigned long now = millis();
  if (now - editorLastBlink >= EDITOR_BLINK_MS)
  {
    editorCursorVisible = !editorCursorVisible;
    editSyncCursor(editorState);
    drawCursor(editorCursorVisible);
    editorLastBlink = now;
  }
}

static void checkInput()
{
  if (!editorLineActive)
  {
    editorBeginLine();
  }

  editorCursorTick();

  int c;
  while ((c = editorReadChar()) >= 0)
  {
    if (editorCursorVisible)
    {
      editSyncCursor(editorState);
      drawCursor(false);
      editorCursorVisible = false;
    }

    EditResult r = processEditChar((uint8_t)c, editorState);
    inputPos = editorState.len;

    if (r == EDIT_SUBMITTED)
    {
      inputReady = true;
      editorLineActive = false;
      return;
    }
    if (r == EDIT_BROKEN)
    {
      // No running program at the prompt — CLEAR just stays where it is
      continue;
    }
  }
}

// ---------------------------------------------------------------------------
// Forward declaration for recursive use by cmdOld
// ---------------------------------------------------------------------------
static void processInput(const char* input);

// ---------------------------------------------------------------------------
// Command callbacks (invoked by Token Parser for immediate commands)
// ---------------------------------------------------------------------------

static void cmdNew()
{
  em.clearProgram();
  tiClearScreen();
  printLine("** READY **");
  showStatus("NEW program");
}

// LIST [n[-m]] / LIST n- / LIST -m / LIST  (full)
// startLine == 0 → from beginning, endLine == -1 → to end.
static void cmdList(int startLine, int endLine)
{
  char buf[256];
  // Route through catPrintLine so LIST also paginates every 23 lines
  // (matches TI behavior + DIR/CAT) and yields per line.
  g_catLines = 0;
  g_catCancelled = false;
  for (int i = 0; i < em.programSize() && !g_catCancelled; i++)
  {
    ProgramLine* line = em.getLine(i);
    if (startLine > 0 && line->lineNum < startLine) continue;
    if (endLine   > 0 && line->lineNum > endLine)   break;
    int n = snprintf(buf, sizeof(buf), "%d ", line->lineNum);
    detokenizeLine(line->tokens, line->length, &buf[n], sizeof(buf) - n);
    catPrintLine(buf);
  }
}

static void cmdRun()
{
  em.run();
}

static void cmdBye()
{
  tiClearScreen();
  printLine("** GOODBYE **");
  delay(500);
  ESP.restart();
}

// LineSource / LineSink bridges from progio:: to our ExecManager.
// Used by cmdSave / cmdOld on the DSK target path (which needs V9T9
// PROGRAM-format binary instead of text .bas files). Function pointers
// can't capture state, so these reach into the file-scope `em` global.
// Strip any trailing TOK_EOL terminator from the token stream so the
// library writes the V9T9-correct [length][toks][0x00] layout — TI
// PROGRAM format uses 0x00 as the line terminator, not our internal
// TOK_EOL marker.
static int progSrcCount() { return em.programSize(); }
static void progSrcGet(int idx, int* outLineNum,
                       const uint8_t** outToks, int* outLen)
{
  ProgramLine* line = em.getLine(idx);
  *outLineNum = line->lineNum;
  *outToks    = line->tokens;
  int len = line->length;
  if (len > 0 && line->tokens[len - 1] == TOK_EOL) len--;
  *outLen     = len;
}
static const progio::LineSource PROG_SRC = { progSrcCount, progSrcGet };

static void progDstClear() { em.clearProgram(); }
static void progDstStore(int lineNum, const uint8_t* toks, int len)
{
  // ExecManager wants TOK_EOL terminated. The library strips 0x00 so
  // we get bare tokens here; append our terminator before storing.
  uint8_t lineToks[MAX_LINE_TOKENS];
  if (len + 1 > MAX_LINE_TOKENS) return;
  if (len > 0) memcpy(lineToks, toks, (size_t)len);
  lineToks[len] = TOK_EOL;
  em.storeLine(lineNum, lineToks, len + 1);
}
static const progio::LineSink PROG_DST = { progDstClear, progDstStore };

// SAVE <spec>
//   FLASH.NAME  / bare NAME    → text .bas in LittleFS
//   SDCARD.NAME                → text .bas in SD_MMC
//   DSKn.NAME                  → V9T9 PROGRAM binary in mounted .dsk
//
// Bare-name back-compat: pre-routing programs used `SAVE FOO` to save
// to FLASH. progio::parseTarget defaults bare names to FLASH so those
// still work.
static void cmdSave(const char* filename)
{
  progio::Target tgt;
  if (!progio::parseTarget(filename, tgt))
  {
    printError("* BAD FILE NAME");
    return;
  }

  // DSK target — write V9T9 PROGRAM-format binary into the mounted image.
  if (tgt.kind == progio::KIND_DSK)
  {
    dsk::DskImage* img = fio::dskImage(tgt.drive);
    if (!img) { printError("* NOT MOUNTED"); return; }
    if (img->readOnly()) { printError("* WRITE PROTECTED"); return; }
    uint8_t* progBuf = (uint8_t*)malloc(8192);
    if (!progBuf) { printError("* OUT OF MEMORY"); return; }
    int size = progio::saveProgramBytes(PROG_SRC, progBuf, 8192);
    if (size <= 0)
    {
      free(progBuf);
      printError("* FILE ERROR");
      return;
    }
    // 0x01 = PROGRAM file flag in V9T9 catalog entry. cmdDirOn's
    // type-string formatter uses the same convention (e.g.->01 == "PROGRAM").
    bool ok = img->writeRawFile(tgt.tiName, progBuf, size, 0x01);
    free(progBuf);
    if (!ok)
    {
      printError("* FILE ERROR");
      return;
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "SAVED: DSK%c.%s (%d bytes)",
             fio::driveToChar(tgt.drive), tgt.tiName, size);
    printLine(msg);
    return;
  }

  // FLASH or SDCARD — text .bas file.
  fs::FS* fsRef = &LittleFS;
  const char* devLabel = "FLASH";
  if (tgt.kind == progio::KIND_SDCARD)
  {
    if (!fio::g_sdOk) { printError("* DEVICE NOT PRESENT"); return; }
    fsRef = &SD_MMC;
    devLabel = "SDCARD";
  }

  char path[48];
  snprintf(path, sizeof(path), "/%s.bas", tgt.name);
  File f = fsRef->open(path, "w");
  if (!f)
  {
    printError("* FILE ERROR");
    return;
  }

  char buf[256];
  for (int i = 0; i < em.programSize(); i++)
  {
    ProgramLine* line = em.getLine(i);
    int n = snprintf(buf, sizeof(buf), "%d ", line->lineNum);
    detokenizeLine(line->tokens, line->length, &buf[n], sizeof(buf) - n);
    f.println(buf);
    tiYield();
  }
  f.close();

  char msg[64];
  snprintf(msg, sizeof(msg), "SAVED: %s%s", devLabel, path);
  printLine(msg);
}

// OLD <spec>
//   FLASH.NAME / bare NAME    → load text .bas from LittleFS
//   SDCARD.NAME               → load text .bas from SD_MMC
//   DSKn.NAME                 → load V9T9 PROGRAM binary from .dsk image
//
// Smart .bas fallback applies to flat-fs targets: OLD FLASH.FOO finds
// /FOO if it exists, else /FOO.bas. SAVE-created files (.bas extension)
// load transparently from a bare name.
static void cmdOld(const char* filename)
{
  progio::Target tgt;
  if (!progio::parseTarget(filename, tgt))
  {
    printError("* BAD FILE NAME");
    return;
  }

  if (tgt.kind == progio::KIND_DSK)
  {
    dsk::DskImage* img = fio::dskImage(tgt.drive);
    if (!img) { printError("* NOT MOUNTED"); return; }
    uint8_t* progBuf = (uint8_t*)malloc(8192);
    if (!progBuf) { printError("* OUT OF MEMORY"); return; }
    int size = img->readRawFile(tgt.tiName, progBuf, 8192);
    if (size < 12)
    {
      free(progBuf);
      printError("* FILE ERROR");
      return;
    }
    bool ok = progio::loadProgramBytes(progBuf, size, PROG_DST,
                                       MAX_LINE_TOKENS);
    free(progBuf);
    if (!ok)
    {
      printError("* BAD PROGRAM");
      return;
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "LOADED: DSK%c.%s (%d lines)",
             fio::driveToChar(tgt.drive), tgt.tiName, em.programSize());
    printLine(msg);
    return;
  }

  fs::FS* fsRef = &LittleFS;
  const char* devLabel = "FLASH";
  if (tgt.kind == progio::KIND_SDCARD)
  {
    if (!fio::g_sdOk) { printError("* DEVICE NOT PRESENT"); return; }
    fsRef = &SD_MMC;
    devLabel = "SDCARD";
  }

  char path[48];
  if (!progio::resolveExistingPath(*fsRef, tgt.name, path, sizeof(path)))
  {
    printError("* FILE ERROR");
    return;
  }
  File f = fsRef->open(path, "r");
  if (!f)
  {
    printError("* FILE ERROR");
    return;
  }

  em.clearProgram();
  while (f.available())
  {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0)
    {
      processInput(line.c_str());
    }
  }
  f.close();

  char msg[64];
  snprintf(msg, sizeof(msg), "LOADED: %s%s", devLabel, path);
  printLine(msg);
}

// MERGE <spec> — load text-format lines from FLASH or SDCARD and fold
// them into the current program. Line-number collisions overwrite
// (matching TI's MERGE behavior). Unlike OLD, the existing program is
// NOT cleared first. DSK targets are not supported — V9T9 PROGRAM
// binaries are a whole-program format and can't be partially merged.
static void cmdMerge(const char* filename)
{
  progio::Target tgt;
  if (!progio::parseTarget(filename, tgt))
  {
    printError("* BAD FILE NAME");
    return;
  }

  if (tgt.kind == progio::KIND_DSK)
  {
    // V9T9 PROGRAM format isn't line-text and can't be merged sensibly.
    printError("* BAD FILE NAME");
    return;
  }

  fs::FS* fsRef = &LittleFS;
  const char* devLabel = "FLASH";
  if (tgt.kind == progio::KIND_SDCARD)
  {
    if (!fio::g_sdOk) { printError("* DEVICE NOT PRESENT"); return; }
    fsRef = &SD_MMC;
    devLabel = "SDCARD";
  }

  char path[48];
  if (!progio::resolveExistingPath(*fsRef, tgt.name, path, sizeof(path)))
  {
    printError("* FILE ERROR");
    return;
  }
  File f = fsRef->open(path, "r");
  if (!f)
  {
    printError("* FILE ERROR");
    return;
  }

  int merged = 0;
  while (f.available())
  {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0)
    {
      processInput(line.c_str());
      merged++;
    }
    tiYield();
  }
  f.close();

  char msg[64];
  snprintf(msg, sizeof(msg), "MERGED: %s%s (%d lines)",
           devLabel, path, merged);
  printLine(msg);
}

// --- File-listing helpers ported from ti-extended-basic-esp32 ---
// CAT/CATALOG/DIR pages output 23 lines at a time, prompts to continue,
// and lets the user bail with ESC / Ctrl-C / Ctrl-L.
int  g_catLines     = 0;
bool g_catCancelled = false;
static void catPrintLine(const char* s)
{
  if (g_catCancelled) return;

  // Count SCREEN rows this line will occupy — a long BASIC line wraps
  // across multiple 32-char display rows. Empty line still takes 1.
  int len = s ? (int)strlen(s) : 0;
  int rows = (len + COLS - 1) / COLS;
  if (rows < 1) rows = 1;

  // If printing this line would push us past the 23-row content area
  // (row 24 is reserved for the prompt), page BEFORE we print — so the
  // user always sees full content before the prompt and we never spill
  // a partial line off the top.
  if (g_catLines + rows > 23)
  {
    printLine("* PRESS ANY KEY TO CONTINUE *");
    int c = -1;
    while (c < 0)
    {
      bleKbTask();
      webfiles::tick();
      tiYield();
      c = editorReadChar();
    }
    if (c == 0x1B || c == 0x03 || c == 12)
    {
      g_catCancelled = true;
      return;
    }
    g_catLines = 0;
  }

  printLine(s);
  g_catLines += rows;
  // Per-line yield (DIR on a big SD card can be 100+ entries — don't
  // wait 23 rows to let the scheduler run).
  tiYield();
}

// DIR [device] — list files on a device.
//   FLASH (default)  → internal LittleFS
//   SDCARD or SD     → external SD on the SENSOR add-on
//   DSK1..DSK9 / DSKA→ V9T9 disk-image catalog (mounted via MOUNT)
static void cmdDirOn(const char* device)
{
  g_catLines = 0;
  g_catCancelled = false;

  if (strncasecmp(device, "DSK", 3) == 0 &&
      (device[4] == '\0' || device[4] == ' ') &&
      fio::driveFromChar(device[3]) > 0)
  {
    int drive = fio::driveFromChar(device[3]);
    dsk::DskImage* img = fio::dskImage(drive);
    if (!img)
    {
      printError("* NOT MOUNTED");
      return;
    }
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "DSK%c  %s  %d FREE",
             fio::driveToChar(drive), img->vib().name,
             img->freeSectors());
    catPrintLine(hdr);
    catPrintLine("FILENAME    TYPE      SIZE");

    dsk::DskImage::CatEntry ents[64];
    int n = img->listCatalog(ents, 64);
    for (int i = 0; i < n && !g_catCancelled; i++)
    {
      const auto& e = ents[i];
      char typeStr[12];
      if (e.flags & 0x01)
      {
        snprintf(typeStr, sizeof(typeStr), "PROGRAM");
      }
      else
      {
        const char* k1 = (e.flags & 0x02) ? "INT" : "DIS";
        const char* k2 = (e.flags & 0x40) ? "VAR" : "FIX";
        snprintf(typeStr, sizeof(typeStr), "%s/%s %d", k1, k2, e.recLen);
      }
      char lock = (e.flags & 0x08) ? 'P' : ' ';
      char buf[40];
      snprintf(buf, sizeof(buf), "%-10s %c %-10s %4d",
               e.name, lock, typeStr, e.totalSectors);
      catPrintLine(buf);
    }
    if (n == 0) catPrintLine("  (empty)");
    return;
  }

  fs::FS* fsRef = nullptr;
  const char* label = "FLASH";
  if (strcasecmp(device, "SDCARD") == 0 || strcasecmp(device, "SD") == 0)
  {
    if (!fio::g_sdOk)
    {
      printError("* DEVICE NOT PRESENT");
      return;
    }
    fsRef = &SD_MMC;
    label = "SDCARD";
  }
  else
  {
    fsRef = &LittleFS;
    label = "FLASH";
  }

  char hdr[32];
  snprintf(hdr, sizeof(hdr), "%s:", label);
  catPrintLine(hdr);

  File root = fsRef->open("/");
  if (!root)
  {
    // open("/") failed entirely — would be the SD filesystem not really
    // mounted under the path we expect. Print a loud error rather than
    // a misleading "(no files)" so the user knows the card-side is broken.
    catPrintLine("  * CANNOT OPEN ROOT");
    Serial.printf("[%s] cmdDirOn: open(\"/\") returned false\n", label);
    return;
  }
  if (!root.isDirectory())
  {
    catPrintLine("  * ROOT NOT A DIRECTORY");
    Serial.printf("[%s] cmdDirOn: open(\"/\") is not a directory\n", label);
    return;
  }
  File f = root.openNextFile();
  int shown = 0;
  int total = 0;
  while (f && !g_catCancelled)
  {
    total++;
    const char* name = f.name();
    bool hide = f.isDirectory() || name[0] == '.' ||
                strcasecmp(name, "System Volume Information") == 0;
    if (!hide)
    {
      char buf[48];
      snprintf(buf, sizeof(buf), "  %-20s %d", name, (int)f.size());
      catPrintLine(buf);
      shown++;
    }
    f.close();   // CRITICAL: SD_MMC has a 5-slot file-handle pool by default;
                 // leaking handles in this loop exhausts it on cards with
                 // more than ~5 entries and subsequent SD opens silently fail.
    f = root.openNextFile();
  }
  root.close();
  if (shown == 0 && total == 0)
  {
    catPrintLine("  (root directory is empty)");
  }
  else if (shown == 0)
  {
    // We saw entries, but all of them were filtered out (dirs / dotfiles
    // / Windows volume metadata). Tell the user explicitly so they don't
    // think the listing is broken.
    char buf[48];
    snprintf(buf, sizeof(buf), "  (no files, %d hidden)", total);
    catPrintLine(buf);
  }
}

// Wrapper for the (currently unused) tokenized TOK_DIR path. Pre-tokenize
// dispatch in processInput calls cmdDirOn directly with the parsed device.
static void cmdDir() { cmdDirOn("FLASH"); }

// SIZE — print free memory, TI-style. Real TI reported stack + program
// space separately; we approximate with free heap (for stack/vars) and
// bytes remaining in the tokenized-program buffer (for program).
static void cmdSize()
{
  char buf[48];
  int programUsed = 0;
  for (int i = 0; i < em.programSize(); i++)
  {
    ProgramLine* pl = em.getLine(i);
    if (pl) programUsed += pl->length + 4;    // rough per-line overhead
  }
  int programFree = (MAX_LINES * 140) - programUsed;  // approximate
  if (programFree < 0) programFree = 0;

  snprintf(buf, sizeof(buf), "%d BYTES OF STACK FREE",
           (int)ESP.getFreeHeap());
  printLine(buf);
  snprintf(buf, sizeof(buf), "%d BYTES OF PROGRAM SPACE FREE", programFree);
  printLine(buf);
}

// TRACE / UNTRACE — forward to the execution manager so it can print
// line numbers as they execute.
static void cmdTrace(bool enable)
{
  em.setTrace(enable);
}

// Breakpoint list — stored in exec_manager. cmdBreak is called with
// add=true for BREAK and add=false for UNBREAK. A zero-length list
// means "all" (BREAK alone does nothing at prompt, UNBREAK alone clears).
static void cmdBreak(const int* lines, int count, bool add)
{
  if (count == 0)
  {
    if (!add) em.clearBreakpoints();
    return;
  }
  for (int i = 0; i < count; i++)
  {
    if (add) em.addBreakpoint(lines[i]);
    else     em.removeBreakpoint(lines[i]);
  }
}

// NUMBER [start, increment] — enters auto-line-number mode. Handled by
// the editor loop; we just set the state flags and first line number.
static void cmdNumber(int startLine, int increment)
{
  numModeStart = startLine;
  numModeIncr  = (increment > 0) ? increment : 10;
  numModeNext  = startLine;
  numModeActive = true;
}

// RESEQUENCE [start, increment] — renumber program lines. Also updates
// GOTO / GOSUB / THEN / ELSE line-number references inside each line.
static void cmdResequence(int startLine, int increment)
{
  if (increment <= 0) increment = 10;
  if (startLine <= 0) startLine = 100;

  int n = em.programSize();
  if (n == 0) return;

  // Build oldLineNum → newLineNum mapping
  static uint16_t mapOld[MAX_LINES];
  static uint16_t mapNew[MAX_LINES];
  for (int i = 0; i < n; i++)
  {
    mapOld[i] = em.getLine(i)->lineNum;
    mapNew[i] = (uint16_t)(startLine + i * increment);
  }

  // Rewrite each line's tokens: replace old line-number references with
  // the new numbers (as ASCII digits in TOK_UNQUOTED_STR form). Tokens
  // following GOTO/GOSUB/THEN/ELSE/RESTORE hold line numbers.
  for (int i = 0; i < n; i++)
  {
    ProgramLine* pl = em.getLine(i);
    uint8_t newToks[MAX_LINE_TOKENS];
    int outPos = 0;
    int p = 0;
    bool expectLineNum = false;
    while (p < pl->length && pl->tokens[p] != TOK_EOL)
    {
      uint8_t t = pl->tokens[p];
      if (t == TOK_GOTO || t == TOK_GOSUB || t == TOK_THEN ||
          t == TOK_ELSE || t == TOK_RESTORE)
      {
        expectLineNum = true;
        newToks[outPos++] = t;
        p++;
        continue;
      }
      if (expectLineNum && t == TOK_UNQUOTED_STR)
      {
        uint8_t slen = pl->tokens[p + 1];
        char num[8];
        int copyLen = (slen < 7) ? slen : 7;
        memcpy(num, &pl->tokens[p + 2], copyLen);
        num[copyLen] = '\0';
        int oldNum = atoi(num);
        int newNum = oldNum;
        for (int j = 0; j < n; j++)
        {
          if (mapOld[j] == oldNum) { newNum = mapNew[j]; break; }
        }
        char buf[8];
        int nLen = snprintf(buf, sizeof(buf), "%d", newNum);
        newToks[outPos++] = TOK_UNQUOTED_STR;
        newToks[outPos++] = (uint8_t)nLen;
        memcpy(&newToks[outPos], buf, nLen);
        outPos += nLen;
        p += 2 + slen;
        expectLineNum = false;
        continue;
      }
      // Comma after GOTO/GOSUB inside ON ... GOTO list: keep expectLineNum
      if (expectLineNum && t == TOK_COMMA)
      {
        newToks[outPos++] = t;
        p++;
        continue;
      }
      expectLineNum = false;
      newToks[outPos++] = t;
      p++;
    }
    newToks[outPos++] = TOK_EOL;
    memcpy(pl->tokens, newToks, outPos);
    pl->length = outPos;
  }

  // Finally, rewrite the line numbers themselves
  for (int i = 0; i < n; i++)
  {
    em.getLine(i)->lineNum = mapNew[i];
  }
}

// --- File I/O shims (wired into TokenParser via setFileCallbacks) ---
//
// The file_io.h layer routes OPEN/CLOSE/PRINT#/INPUT#/EOF() to either
// LittleFS (FLASH./SDCARD.) or a mounted V9T9 disk image (DSKn.).
// Display-agnostic — same shims work on both the RGB-panel and OTG
// builds.
static int shimFileOpen(int unit, const char* spec, int mode,
                        int flags, int recLen)
{
  return fio::openFile(unit, spec, (fio::Mode)mode, flags, recLen);
}
static int shimFileClose(int unit) { return fio::closeFile(unit); }
static int shimFilePrint(int unit, const char* text)
{
  return fio::printLineTo(unit, text);
}
static int shimFileReadLine(int unit, char* buf, int bufSize)
{
  return fio::readLineFrom(unit, buf, bufSize);
}
static bool shimFileEof(int unit) { return fio::isEof(unit); }
static bool shimFileSeekRec(int unit, long rec)
{
  return fio::seekRecord(unit, rec);
}
static bool shimFileRewind(int unit)
{
  return fio::rewindFile(unit);
}

// --- Sprite stub callbacks ---
//
// --- Software sprite layer ---
//
// Ported from ti-extended-basic-esp32, retuned for the Box-3's 1:1
// panel-to-TI-pixel mapping (vs the RGB panel's 2x scale). Sprites are
// drawn directly onto the LCD via LovyanGFX with per-sprite save-under
// buffers in PSRAM, then composite-blitted in a single pushImage call
// per update — this avoids per-pixel SPI overhead and tearing.
//
// The TI-99/4A's TMS9918 had hardware sprites at pixel resolution; we
// emulate them in software on top of the character grid. spriteCellBounds
// returns the 32x24 cells that the sprite overlaps so we can restore
// those cells (via drawCell or save-under blit) when a sprite moves.

static void spriteCellBounds(const sprites::Sprite& s,
                             int& r0, int& c0, int& r1, int& c1)
{
  int body  = sprites::bodySize(s.magnify);
  int scale = (s.magnify == sprites::MAG_2 ||
               s.magnify == sprites::MAG_4) ? 2 : 1;
  int pxH = body * scale;   // sprite size in TI pixels
  int pxW = body * scale;
  int topTi  = s.row - 1;   // 1-based → 0-based
  int leftTi = s.col - 1;
  r0 = topTi  / 8;
  c0 = leftTi / 8;
  r1 = (topTi  + pxH - 1) / 8;
  c1 = (leftTi + pxW - 1) / 8;
  if (r0 < 0) r0 = 0;
  if (c0 < 0) c0 = 0;
  if (r1 > ROWS - 1) r1 = ROWS - 1;
  if (c1 > COLS - 1) c1 = COLS - 1;
}

// Save-under: when a sprite is drawn we capture the chars it covers in
// a per-sprite PSRAM buffer, then on erase we blit it back in one call.
// Footprint upper bound on Box-3 (1:1, no panel scaling): mag-4 = 32px,
// can straddle up to 5 cells × 8 = 40 px aligned. 48 leaves margin.
// 48*48*2 = 4.6 KB per sprite × 28 = ~130 KB in PSRAM (lazy-alloc).
static const int SPRITE_SAVE_MAX_DIM = 48;
static const int SPRITE_SAVE_MAX_PX  = SPRITE_SAVE_MAX_DIM * SPRITE_SAVE_MAX_DIM;
struct SpriteSave
{
  uint16_t* pixels = nullptr;
  int  x = 0, y = 0, w = 0, h = 0;
  bool valid = false;
};
static SpriteSave g_spriteSave[sprites::MAX_SPRITES + 1];

// Shared scratch used to composite (chars + sprite pixels) before the
// single pushImage. Per-pixel writes to the panel are slow over SPI.
static uint16_t* g_spriteCompBuf = nullptr;
static void ensureSpriteCompBuf()
{
  if (g_spriteCompBuf) return;
  g_spriteCompBuf = (uint16_t*)heap_caps_malloc(
      SPRITE_SAVE_MAX_PX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
}

static int spriteIndexOf(const sprites::Sprite& s)
{
  return (int)(&s - &sprites::g_sprites[0]);
}

static void ensureSpriteSaveBuf(int slot)
{
  if (slot < 0 || slot > sprites::MAX_SPRITES) return;
  if (g_spriteSave[slot].pixels) return;
  g_spriteSave[slot].pixels = (uint16_t*)heap_caps_malloc(
      SPRITE_SAVE_MAX_PX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
}

// Render the chars in cell rect (c0..c1, r0..r1) into dst as a tightly
// packed `w` × `h` RGB565 bitmap. Same per-pixel logic as drawCell but
// targets memory instead of the panel. Box-3 1:1: each TI pixel = 1
// panel pixel (no 2x multiplier).
static void renderCellsToBuffer(int r0, int c0, int r1, int c1,
                                uint16_t* dst, int w)
{
  for (int rOff = 0; rOff <= r1 - r0; rOff++)
  {
    for (int cOff = 0; cOff <= c1 - c0; cOff++)
    {
      int cellR = r0 + rOff;
      int cellC = c0 + cOff;
      if (cellR < 0 || cellR >= ROWS || cellC < 0 || cellC >= COLS) continue;
      uint8_t ch = (uint8_t)screenBuf[cellR][cellC];
      uint16_t fg = resolveColor(charFgIdx[ch]);
      uint16_t bg = resolveColor(charBgIdx[ch]);
      int dstX0 = cOff * CHAR_W;
      int dstY0 = rOff * CHAR_H;
      for (int py = 0; py < 8; py++)
      {
        uint8_t bits = charPatterns[ch][py];
        uint16_t* row = &dst[(dstY0 + py) * w + dstX0];
        for (int px = 0; px < 8; px++)
        {
          row[px] = (bits & 0x80) ? fg : bg;
          bits <<= 1;
        }
      }
    }
  }
}

// Repaint the character cells under the sprite. Fast path: blit the
// save-under buffer captured by spriteDraw. Fallback: per-cell drawCell
// (used when no save buf is allocated yet, e.g. initial draw after
// CALL SPRITE).
static void spriteErase(const sprites::Sprite& s)
{
  if (!s.active) return;
  int slot = spriteIndexOf(s);
  SpriteSave& sv = g_spriteSave[slot];
  if (sv.valid && sv.pixels)
  {
    tft.pushImage(sv.x, sv.y, sv.w, sv.h, sv.pixels);
    sv.valid = false;
    return;
  }
  int r0, c0, r1, c1;
  spriteCellBounds(s, r0, c0, r1, c1);
  for (int r = r0; r <= r1; r++)
  {
    for (int c = c0; c <= c1; c++)
    {
      drawCell(c, r);
    }
  }
}

// Draw one sprite at its current position. Transparent pixels (pattern
// bit 0) leave whatever was on screen beneath. Box-3 is 1:1 — no panel
// scaling factor.
static void spriteDraw(const sprites::Sprite& s)
{
  if (!s.active) return;

  int slot = spriteIndexOf(s);
  int r0, c0, r1, c1;
  spriteCellBounds(s, r0, c0, r1, c1);
  int saveW = (c1 - c0 + 1) * CHAR_W;
  int saveH = (r1 - r0 + 1) * CHAR_H;
  int saveX = c0 * CHAR_W + DISPLAY_X_OFFSET;
  int saveY = r0 * CHAR_H + DISPLAY_Y_OFFSET;

  // Fallback to per-pixel paint if footprint exceeds scratch (shouldn't
  // happen in practice — even mag-4 fits in 48x48) or PSRAM alloc failed.
  if (saveW <= 0 || saveH <= 0 || saveW * saveH > SPRITE_SAVE_MAX_PX)
  {
    int body  = sprites::bodySize(s.magnify);
    int scale = (s.magnify == sprites::MAG_2 ||
                 s.magnify == sprites::MAG_4) ? 2 : 1;
    uint16_t fg = resolveColor(s.colorIdx);
    int baseY = DISPLAY_Y_OFFSET + (s.row - 1);
    int baseX = DISPLAY_X_OFFSET + (s.col - 1);
    for (int sr = 0; sr < body; sr++)
    {
      for (int sc = 0; sc < body; sc++)
      {
        if (!sprites::pixelOn(s.charCode, s.magnify, sr, sc, charPatterns))
        {
          continue;
        }
        for (int dy = 0; dy < scale; dy++)
        {
          for (int dx = 0; dx < scale; dx++)
          {
            int py = baseY + sr * scale + dy;
            int px = baseX + sc * scale + dx;
            if (py >= DISPLAY_Y_OFFSET &&
                py < DISPLAY_Y_OFFSET + ROWS * CHAR_H &&
                px >= DISPLAY_X_OFFSET &&
                px < DISPLAY_X_OFFSET + COLS * CHAR_W)
            {
              tft.drawPixel(px, py, fg);
            }
          }
        }
      }
    }
    return;
  }

  // Fast path: composite chars + sprite pixels into one buffer, then
  // blit in a single pushImage call. Avoids per-pixel SPI overhead.
  ensureSpriteSaveBuf(slot);
  ensureSpriteCompBuf();
  SpriteSave& sv = g_spriteSave[slot];
  if (!sv.pixels || !g_spriteCompBuf)
  {
    return;   // PSRAM alloc failed — silent skip is fine
  }

  // 1. Render the chars under the sprite into the per-sprite save
  //    buffer (used by next spriteErase).
  renderCellsToBuffer(r0, c0, r1, c1, sv.pixels, saveW);
  sv.x = saveX; sv.y = saveY; sv.w = saveW; sv.h = saveH;
  sv.valid = true;

  // 2. Copy save buf → composition scratch and overlay sprite pixels.
  memcpy(g_spriteCompBuf, sv.pixels,
         (size_t)saveW * saveH * sizeof(uint16_t));

  int body  = sprites::bodySize(s.magnify);
  int scale = (s.magnify == sprites::MAG_2 ||
               s.magnify == sprites::MAG_4) ? 2 : 1;
  uint16_t fg = resolveColor(s.colorIdx);
  int baseY = DISPLAY_Y_OFFSET + (s.row - 1);
  int baseX = DISPLAY_X_OFFSET + (s.col - 1);
  int yMax = DISPLAY_Y_OFFSET + ROWS * CHAR_H;
  int xMax = DISPLAY_X_OFFSET + COLS * CHAR_W;
  for (int sr = 0; sr < body; sr++)
  {
    for (int sc = 0; sc < body; sc++)
    {
      if (!sprites::pixelOn(s.charCode, s.magnify, sr, sc, charPatterns))
      {
        continue;
      }
      for (int dy = 0; dy < scale; dy++)
      {
        int py = baseY + sr * scale + dy;
        if (py < DISPLAY_Y_OFFSET || py >= yMax) continue;
        int bufY = py - saveY;
        if (bufY < 0 || bufY >= saveH) continue;
        uint16_t* row = &g_spriteCompBuf[bufY * saveW];
        for (int dx = 0; dx < scale; dx++)
        {
          int px = baseX + sc * scale + dx;
          if (px < DISPLAY_X_OFFSET || px >= xMax) continue;
          int bufX = px - saveX;
          if (bufX < 0 || bufX >= saveW) continue;
          row[bufX] = fg;
        }
      }
    }
  }

  // 3. Blit the composed frame in one shot.
  tft.pushImage(saveX, saveY, saveW, saveH, g_spriteCompBuf);
}

// Redraw every active sprite. Order matters: TI Extended BASIC gives
// sprite #1 the highest priority (it sits on top of #2, which sits on
// top of #3, ...). Drawing in *reverse* index order means the highest-
// numbered paints first and the lowest-numbered paints last, so #1
// ends up on top.
static void spriteRedrawAll()
{
  for (int i = sprites::MAX_SPRITES; i >= 1; i--)
  {
    spriteDraw(sprites::g_sprites[i]);
  }
}

// Strong overrides of the weak tiSpriteDraw / tiSpriteErase symbols
// in the shared interpreter (ti_platform.cpp). The interpreter calls
// these whenever BASIC executes CALL SPRITE / CALL PATTERN / etc.
// Drawing one sprite must not paint over higher-priority sprites it
// overlaps, so after drawing #n we redraw #n-1..#1 to restore order.
void tiSpriteDraw(int n)
{
  if (!sprites::validSlot(n)) return;
  spriteDraw(sprites::g_sprites[n]);
  for (int p = n - 1; p >= 1; p--)
  {
    if (sprites::g_sprites[p].active) spriteDraw(sprites::g_sprites[p]);
  }
}

void tiSpriteErase(int n)
{
  if (sprites::validSlot(n)) spriteErase(sprites::g_sprites[n]);
}

// Strong overrides for CALL PAIR / CALL UNPAIR — bridge into BleHidHost.
void tiPair()
{
  BleHidHost::requestPairingMode();
}

void tiUnpair()
{
  BleHidHost::requestUnpairAll();
}

// Strong overrides for CALL WIFI — route to the web_files manager.
void tiWifiSet(const char* ssid, const char* pass)
{
  webfiles::setCredentials(ssid, pass);
}
void tiWifiForget()
{
  webfiles::forget();
}
void tiWifiStatus(char* out, int outSize)
{
  webfiles::status(out, outSize);
}
void tiWifiOff()
{
  webfiles::radioOff();
}
void tiWifiOn()
{
  webfiles::radioOn();
}

// `BLE` shell command — prints the bonded-peer table to the BASIC console
// for diagnosing pairing / reconnect issues from inside BASIC.
static void cmdBle()
{
  BleHidHost::describePeers(printLine);
}

// CALL SAY / CALL SPGET — speech synth host implementation.
//
// Stage 2a: real vocabulary lookup against the TI Speech Synthesizer
// cartridge ROM (spchrom.bin baked into TI_Speech/speech_rom.h via
// extract.py). Looks up word names in the cartridge's binary search
// tree and returns the LPC byte sequence for each phrase.
//
// Stage 2b: TMS5220 LPC-10 synth (ported from MAME, BSD-3-Clause —
// see TI_Speech/tms5220.h) wired into the I²S audio mixer at 8 kHz
// native, resampled to the 44.1 kHz codec rate. tiSay now actually
// plays audio: for each vocabulary word it looks up the LPC byte
// range in PROGMEM, copies into a small stack buffer, and feeds the
// synth via feedSpeechBytes(); then blocks via isSpeechTalking() so
// CALL SAY is synchronous from BASIC's perspective (matches real TI
// behavior where the interpreter waits for speech to finish).

#if HAVE_SPEECH_ROM
// Walk the vocabulary binary search tree to find a word.
// Format (see TI_Speech/README.md):
//   [1B nameLen][N bytes ASCII name][2B prev BE][2B next BE]
//   [1B pad][2B dataOffset BE][1B dataLen]
// Root lives at offset 1 (offset 0 is the 0xAA marker). Leaves have
// prev = next = 0. Returns true and fills outDataOffset/outDataLen on
// hit; returns false on miss.
static bool speechRomLookup(const char* word,
                            uint16_t* outDataOffset,
                            uint8_t* outDataLen)
{
  uint16_t offset = 1;   // root
  while (offset != 0 && offset + 1 < SPEECH_ROM_SIZE)
  {
    uint8_t nameLen = pgm_read_byte(&speechRom[offset]);
    if (nameLen == 0 || nameLen > 24) return false;   // sentinel / corrupt

    char name[25];
    for (uint8_t i = 0; i < nameLen; i++)
    {
      name[i] = (char)pgm_read_byte(&speechRom[offset + 1 + i]);
    }
    name[nameLen] = '\0';

    int cmp = strcasecmp(word, name);

    uint16_t descOff = offset + 1 + nameLen;
    if (descOff + 7 >= SPEECH_ROM_SIZE) return false;

    if (cmp == 0)
    {
      *outDataOffset =
        ((uint16_t)pgm_read_byte(&speechRom[descOff + 5]) << 8) |
                   pgm_read_byte(&speechRom[descOff + 6]);
      *outDataLen = pgm_read_byte(&speechRom[descOff + 7]);
      return true;
    }

    uint16_t prev =
      ((uint16_t)pgm_read_byte(&speechRom[descOff]) << 8) |
                 pgm_read_byte(&speechRom[descOff + 1]);
    uint16_t next =
      ((uint16_t)pgm_read_byte(&speechRom[descOff + 2]) << 8) |
                 pgm_read_byte(&speechRom[descOff + 3]);
    offset = (cmp < 0) ? prev : next;
  }
  return false;
}
#endif   // HAVE_SPEECH_ROM

// Copy `len` bytes of LPC data out of PROGMEM at `dataOff` into RAM, then
// hand them to the audio mixer's TMS5220 instance. Splitting the read
// from the feed lets us pgm_read_byte() one byte at a time without
// holding the audio mutex across the whole copy.
static void queueLpcRange(uint16_t dataOff, uint8_t len)
{
#if HAVE_SPEECH_ROM
  if (len == 0) return;
  uint8_t tmp[256];                          // max single-word LPC size
  for (uint8_t i = 0; i < len; i++)
  {
    tmp[i] = pgm_read_byte(&speechRom[dataOff + i]);
  }
  feedSpeechBytes(tmp, len);
#else
  (void)dataOff; (void)len;
#endif
}

void tiSay(const char* wordStr, const uint8_t* phraseBytes, int phraseLen)
{
#if HAVE_SPEECH_ROM
  // Greedy longest-match scan over the vocabulary. The TI Speech ROM
  // contains 12 multi-word entries ("TEXAS INSTRUMENTS", "TRY AGAIN",
  // "WHAT WAS THAT", etc.), and a naive strtok-on-space would miss
  // them. Algorithm: tokenize on space, then at each token position
  // try the longest concatenation that exists in the vocab and
  // advance past it. If any position fails to match anything, the
  // whole utterance aborts to "UHOH" (matches real TI behavior).
  //
  // Commas are stripped from the input first so "HELLO, WORLD"
  // tokenises as ["HELLO", "WORLD"]. Periods are kept as-is because
  // the "." vocab entry should remain reachable as a 1-char lookup.
  if (wordStr && *wordStr)
  {
    char buf[128];
    int outIdx = 0;
    for (const char* p = wordStr;
         *p && outIdx < (int)sizeof(buf) - 1; p++)
    {
      if (*p == ',') continue;
      buf[outIdx++] = *p;
    }
    buf[outIdx] = '\0';

    char* tokens[16];
    int nTokens = 0;
    char* tk = strtok(buf, " ");
    while (tk && nTokens < (int)(sizeof(tokens) / sizeof(tokens[0])))
    {
      tokens[nTokens++] = tk;
      tk = strtok(NULL, " ");
    }

    struct WordEntry { uint16_t off; uint8_t len; };
    WordEntry words[16];
    int nWords = 0;
    bool anyMissing = false;

    int pos = 0;
    while (pos < nTokens &&
           nWords < (int)(sizeof(words) / sizeof(words[0])))
    {
      // Try the longest concatenation tokens[pos..end-1] that's in
      // the vocab. end starts at nTokens (longest) and shrinks.
      int bestEnd = -1;
      uint16_t bestOff = 0;
      uint8_t  bestLen = 0;
      char concat[64];

      for (int end = nTokens; end > pos; end--)
      {
        int p = 0;
        bool fits = true;
        for (int i = pos; i < end; i++)
        {
          int slen = (int)strlen(tokens[i]);
          int needed = slen + (i > pos ? 1 : 0);
          if (p + needed >= (int)sizeof(concat))
          {
            fits = false;
            break;
          }
          if (i > pos) concat[p++] = ' ';
          memcpy(&concat[p], tokens[i], slen);
          p += slen;
        }
        if (!fits) continue;
        concat[p] = '\0';

        uint16_t off = 0;
        uint8_t  len = 0;
        if (speechRomLookup(concat, &off, &len))
        {
          bestEnd = end;
          bestOff = off;
          bestLen = len;
          break;   // longest match wins
        }
      }

      if (bestEnd < 0)
      {
        Serial.printf("tiSay: \"%s\" NOT IN VOCAB at tok %d -> UHOH\n",
                      tokens[pos], pos);
        anyMissing = true;
        break;
      }

      words[nWords].off = bestOff;
      words[nWords].len = bestLen;
      nWords++;
      Serial.printf("tiSay: matched tokens[%d..%d] @ 0x%04X (%u bytes)\n",
                    pos, bestEnd - 1, bestOff, bestLen);
      pos = bestEnd;
    }

    // Per-phrase pump: push one phrase, wait for the synth to drain
    // before pushing the next. The synth drains the FIFO on its STOP
    // frame, so concatenating multiple phrases into one feedSpeechBytes
    // call would lose all but the first.
    //
    // After the synth reports done, the I²S DMA chain still holds
    // ~40-50ms of buffered audio that hasn't reached the DAC yet —
    // including the synth's 23ms energy-ramp fade-out tail. Add a
    // 150 ms breathing-room delay so the previous word's tail plays
    // fully before the next word's first sample is generated;
    // without this, back-to-back CALL SAY calls truncate audibly.
    auto playPhrase = [](uint16_t off, uint8_t len)
    {
      queueLpcRange(off, len);
      unsigned long deadline = millis() + 5000;
      while (isSpeechTalking())
      {
        if ((long)(millis() - deadline) > 0)
        {
          Serial.println("tiSay: per-phrase TIMEOUT — forcing stop");
          stopSpeech();
          return;
        }
        delay(5);
      }
      delay(150);
    };

    if (anyMissing)
    {
      uint16_t uhOff = 0;
      uint8_t  uhLen = 0;
      if (speechRomLookup("UHOH", &uhOff, &uhLen))
      {
        playPhrase(uhOff, uhLen);
      }
    }
    else
    {
      for (int i = 0; i < nWords; i++)
      {
        playPhrase(words[i].off, words[i].len);
      }
    }
  }

  // Then any pre-fetched LPC phrase (typically from a prior CALL SPGET).
  // Real TI plays words first, then phrase data; we mirror that order.
  if (phraseBytes && phraseLen > 0)
  {
    feedSpeechBytes(phraseBytes, (size_t)phraseLen);
  }

  // Block until the synth drains. Real TI's CALL SAY is synchronous and
  // BASIC programs rely on that — they CALL SAY then immediately PRINT
  // assuming the line is silent. Yield to other tasks while we wait.
  // Hard timeout of 10 seconds protects against synth state bugs locking
  // the interpreter forever; real phrases are well under 2 seconds.
  unsigned long deadline = millis() + 10000;
  while (isSpeechTalking())
  {
    if ((long)(millis() - deadline) > 0)
    {
      Serial.println("tiSay: TIMEOUT — synth never reported done. Forcing stop.");
      stopSpeech();
      break;
    }
    delay(5);
  }
#else
  Serial.printf("tiSay: stub (no speech ROM baked) — words=\"%s\" phrase=%d\n",
                wordStr ? wordStr : "(none)", phraseLen);
  (void)phraseBytes;
#endif
}

int tiSpget(const char* word, uint8_t* outBuf, int bufSize)
{
#if HAVE_SPEECH_ROM
  uint16_t dataOff = 0;
  uint8_t  dataLen = 0;
  if (!speechRomLookup(word, &dataOff, &dataLen))
  {
    Serial.printf("tiSpget: \"%s\" not in vocabulary\n", word);
    return 0;
  }
  int n = (dataLen < bufSize) ? dataLen : bufSize;
  for (int i = 0; i < n; i++)
  {
    outBuf[i] = pgm_read_byte(&speechRom[dataOff + i]);
  }
  Serial.printf("tiSpget: \"%s\" -> %d bytes from 0x%04X\n",
                word, n, dataOff);
  return n;
#else
  Serial.printf("tiSpget: stub (no speech ROM baked) — \"%s\"\n", word);
  (void)outBuf; (void)bufSize;
  return 0;
#endif
}

// 60 Hz integration of sprite velocity. Each velocity unit is 1/8 of
// a TI pixel per frame, so a 16 ms tick advances by vel/8. Sprites
// that crossed a pixel boundary get erased and redrawn at their new
// position. Wraps at TI screen edges (row in [1,256], col in [1,256])
// — TI VDP behavior; bounds-checking is the BASIC program's job.
static void spriteTick()
{
  // Yield first so we yield once per BASIC line (this is registered as
  // the interpreter's per-line tick callback in setup). Without it, a
  // tight BASIC numeric loop never gives FreeRTOS a slot — the
  // interpreter's own tiYield call at the end of each line still helps,
  // but doing it here too is cheap and covers the sprite-demo case
  // (movement looks the same when the scheduler can't catch up).
  tiYield();

  static unsigned long lastTick = 0;
  unsigned long now = millis();
  if (lastTick == 0) { lastTick = now; return; }
  if (now - lastTick < 16) return;
  unsigned long elapsed = now - lastTick;
  lastTick = now;
  int frames = (int)(elapsed / 16);
  if (frames < 1) frames = 1;
  if (frames > 8) frames = 8;

  for (int i = 1; i <= sprites::MAX_SPRITES; i++)
  {
    sprites::Sprite& s = sprites::g_sprites[i];
    if (!s.active) continue;
    if (s.rowVel == 0 && s.colVel == 0) continue;

    int16_t prevRow = s.row, prevCol = s.col;
    s.subRow += (int32_t)s.rowVel * frames;
    s.subCol += (int32_t)s.colVel * frames;

    int16_t dr = (int16_t)(s.subRow / 8);
    int16_t dc = (int16_t)(s.subCol / 8);
    s.subRow -= (int32_t)dr * 8;
    s.subCol -= (int32_t)dc * 8;

    if (dr == 0 && dc == 0) continue;

    int16_t nr = s.row + dr;
    int16_t nc = s.col + dc;
    while (nr < 1)   nr += 256;
    while (nr > 256) nr -= 256;
    while (nc < 1)   nc += 256;
    while (nc > 256) nc -= 256;

    if (nr != prevRow || nc != prevCol)
    {
      spriteErase(s);
      s.row = nr;
      s.col = nc;
      spriteDraw(s);
      // Restore higher-priority sprites that may overlap #i's new
      // footprint. Sprite #1 is highest priority and must paint last.
      for (int p = i - 1; p >= 1; p--)
      {
        if (sprites::g_sprites[p].active) spriteDraw(sprites::g_sprites[p]);
      }
    }
  }

  // Snapshot every active sprite's position so BASIC sees a coherent
  // frame. POSITION / COINC / DISTANCE read snap*; physics updates
  // row/col live each tick.
  for (int i = 1; i <= sprites::MAX_SPRITES; i++)
  {
    sprites::Sprite& s = sprites::g_sprites[i];
    if (!s.active) continue;
    s.snapRow     = s.row;
    s.snapCol     = s.col;
    s.snapMagnify = s.magnify;
  }
}

// --- CALL JOYST callback ---
void tiReadJoystick(int unit, int* outX, int* outY)
{
  *outX = bleGpJoystickX(unit);
  *outY = bleGpJoystickY(unit);
}

static void cmdContinue() { em.cont(); }

// DELETE [device.]name — device-aware delete.
//   FLASH.NAME      → /NAME on LittleFS
//   SDCARD.NAME     → /NAME on SD
//   DSKn.NAME       → file inside mounted V9T9 image
//   bare NAME       → /NAME.bas on LittleFS (legacy SAVE convention)
static void cmdDelete(const char* filename)
{
  if (!filename || filename[0] == '\0')
  {
    printError("* BAD FILE NAME");
    return;
  }

  fio::Device dev = fio::DEV_NONE;
  char innerPath[48];
  int drive = 0;
  if (fio::parseSpec(filename, dev, innerPath, sizeof(innerPath), drive))
  {
    bool ok = false;
    char label[56];
    if (dev == fio::DEV_FLASH)
    {
      ok = LittleFS.exists(innerPath) && LittleFS.remove(innerPath);
      snprintf(label, sizeof(label), "FLASH%s", innerPath);
    }
    else if (dev == fio::DEV_SD)
    {
      if (!fio::g_sdOk)
      {
        printError("* DEVICE NOT PRESENT");
        return;
      }
      ok = SD_MMC.exists(innerPath) && SD_MMC.remove(innerPath);
      snprintf(label, sizeof(label), "SDCARD%s", innerPath);
    }
    else if (dev == fio::DEV_DSK)
    {
      dsk::DskImage* img = fio::dskImage(drive);
      if (!img)
      {
        printError("* NOT MOUNTED");
        return;
      }
      if (img->readOnly())
      {
        printError("* WRITE PROTECTED");
        return;
      }
      ok = img->deleteFile(innerPath);
      snprintf(label, sizeof(label), "DSK%c.%s",
               fio::driveToChar(drive), innerPath);
    }
    if (!ok)
    {
      printError("* FILE ERROR");
      return;
    }
    char msg[72];
    snprintf(msg, sizeof(msg), "DELETED: %s", label);
    printLine(msg);
    return;
  }

  // Legacy: bare name → /NAME.bas on LittleFS (matches SAVE / OLD)
  char path[48];
  snprintf(path, sizeof(path), "/%s.bas", filename);
  if (!LittleFS.exists(path) || !LittleFS.remove(path))
  {
    printError("* FILE ERROR");
    return;
  }
  char msg[64];
  snprintf(msg, sizeof(msg), "DELETED: %s", path);
  printLine(msg);
}

// --- V9T9 .dsk mount table persistence ---
// /mounts.cfg on LittleFS — three lines, one per drive; blank = unmounted.
static void saveMounts()
{
  File f = LittleFS.open("/mounts.cfg", "w");
  if (!f) return;
  for (int d = 1; d <= fio::MAX_DSK; d++)
  {
    f.println(fio::dskImagePath(d));
  }
  f.close();
}

static void loadMounts()
{
  File f = LittleFS.open("/mounts.cfg", "r");
  if (!f) return;
  for (int d = 1; d <= fio::MAX_DSK && f.available(); d++)
  {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0)
    {
      fio::mountDskImage(d, line.c_str());
    }
  }
  f.close();
}

// MOUNT DSK<n> <spec>
//   <spec> forms accepted:
//     FLASH.FILE.DSK    — internal LittleFS
//     SDCARD.FILE.DSK   — external SD card
//     /FILE.DSK         — absolute SD path (legacy)
//     FILE              — bare name, .DSK auto-appended, SD card
static void cmdMount(int drive, const char* imageName)
{
  if (drive < 1 || drive > fio::MAX_DSK)
  {
    printError("* BAD DEVICE");
    return;
  }
  if (!fio::mountDskImage(drive, imageName))
  {
    int reason = fio::g_mounts[drive].img.openReason;
    const char* why = (reason == 1) ? "* OPEN FAILED" :
                      (reason == 2) ? "* READ FAILED" :
                      (reason == 3) ? "* BAD VIB" :
                                      "* MOUNT FAILED";
    printError(why);
    return;
  }
  saveMounts();
  char msg[64];
  snprintf(msg, sizeof(msg), "DSK%c = %s  [%s  %d sectors]",
           fio::driveToChar(drive), imageName,
           fio::g_mounts[drive].img.vib().name,
           fio::g_mounts[drive].img.vib().totalSectors);
  printLine(msg);
}

// NEWDISK <device.name> "VOLNAME" [SSSD|DSSD|DSDD]
// Creates a fresh V9T9 disk image at the given LittleFS or SD location.
static void cmdNewDisk(const char* spec, const char* volName,
                       int totalSectors)
{
  if (!spec || !spec[0])
  {
    printError("* BAD FILE NAME");
    return;
  }

  bool fromFlash;
  char fsPath[48];
  if (!fio::resolveMountSpec(spec, fromFlash, fsPath, sizeof(fsPath)))
  {
    printError("* BAD DEVICE");
    return;
  }
  if (!fromFlash && !fio::g_sdOk)
  {
    printError("* DEVICE NOT PRESENT");
    return;
  }
  fs::FS& fsTarget = fromFlash ? (fs::FS&)LittleFS : (fs::FS&)SD_MMC;

  // Y/N overwrite confirmation
  if (fsTarget.exists(fsPath))
  {
    char warn[64];
    snprintf(warn, sizeof(warn), "* %s EXISTS. OVERWRITE? (Y/N)", spec);
    printLine(warn);
    int c = 0;
    do
    {
      c = editorReadChar();
      bleKbTask();
      webfiles::tick();
      yield();
    } while (c < 0);
    if (c != 'Y' && c != 'y')
    {
      printLine("CANCELLED");
      return;
    }
    fsTarget.remove(fsPath);
  }

  if (!dsk::DskImage::create(fsTarget, fsPath, volName, totalSectors))
  {
    printError("* CREATE FAILED");
    return;
  }
  // Box-3's SPI panel does not have the RGB-DMA tearing issue that
  // ti-extended-basic-esp32's RGB panel had during flash erases, so we
  // skip the paintBorder() / redrawScreen() recovery the main repo does.
  char msg[64];
  const char* sizeLabel = (totalSectors == 360)  ? "SSSD" :
                          (totalSectors == 720)  ? "DSSD" : "DSDD";
  snprintf(msg, sizeof(msg), "CREATED %s %s [%s  %d sectors]",
           sizeLabel, spec, volName, totalSectors);
  printLine(msg);
}

// COPY <src-spec> <dst-spec>
// Line-level copy between FLASH, SDCARD, and mounted DSK drives.
static void cmdCopy(const char* src, const char* dst)
{
  if (fio::openFile(5, src, fio::MODE_INPUT) != 0)
  {
    printError("* SOURCE OPEN FAILED");
    return;
  }
  if (fio::openFile(6, dst, fio::MODE_OUTPUT) != 0)
  {
    fio::closeFile(5);
    printError("* DEST OPEN FAILED");
    return;
  }
  int lines = 0;
  char line[MAX_STR_LEN];
  while (!fio::isEof(5))
  {
    if (fio::readLineFrom(5, line, sizeof(line)) != 0) break;
    fio::printLineTo(6, line);
    lines++;
  }
  fio::closeFile(6);
  fio::closeFile(5);
  char msg[48];
  snprintf(msg, sizeof(msg), "COPIED %d LINES", lines);
  printLine(msg);
}

static void cmdUnmount(int drive)
{
  if (drive < 1 || drive > fio::MAX_DSK)
  {
    printError("* BAD DEVICE");
    return;
  }
  fio::unmountDskImage(drive);
  saveMounts();
  char msg[32];
  snprintf(msg, sizeof(msg), "DSK%c UNMOUNTED", fio::driveToChar(drive));
  printLine(msg);
}

// ---------------------------------------------------------------------------
// Command processor (handles typed input)
// ---------------------------------------------------------------------------

static void processInput(const char* input)
{
  int pos = 0;
  while (input[pos] == ' ')
  {
    pos++;
  }

  if (input[pos] == '\0')
  {
    return;
  }

  // Pre-tokenize commands — string-matched immediate commands
  // These don't get tokens; they're handled directly.
  if (strncasecmp(&input[pos], "NEW", 3) == 0 &&
      (input[pos + 3] == '\0' || input[pos + 3] == ' '))
  {
    cmdNew();
    return;
  }
  if (strncasecmp(&input[pos], "RUN", 3) == 0 &&
      (input[pos + 3] == '\0' || input[pos + 3] == ' '))
  {
    cmdRun();
    return;
  }
  // BLE — list bonded HID peers and current pairing-mode state.
  // Diagnostic only; the actual pairing trigger is CALL PAIR.
  if (strncasecmp(&input[pos], "BLE", 3) == 0 &&
      (input[pos + 3] == '\0' || input[pos + 3] == ' '))
  {
    cmdBle();
    return;
  }
  // CAT[ALOG] / DIR — list files on a device (defaults to FLASH).
  // TI-style: CATALOG is the conventional name; DIR is kept as a habit alias.
  {
    int kwLen = 0;
    if (strncasecmp(&input[pos], "CATALOG", 7) == 0 &&
        (input[pos + 7] == '\0' || input[pos + 7] == ' '))
    {
      kwLen = 7;
    }
    else if (strncasecmp(&input[pos], "CAT", 3) == 0 &&
             (input[pos + 3] == '\0' || input[pos + 3] == ' '))
    {
      kwLen = 3;
    }
    else if (strncasecmp(&input[pos], "DIR", 3) == 0 &&
             (input[pos + 3] == '\0' || input[pos + 3] == ' '))
    {
      kwLen = 3;
    }
    if (kwLen > 0)
    {
      int p = pos + kwLen;
      while (input[p] == ' ') p++;
      cmdDirOn(input[p] == '\0' ? "FLASH" : &input[p]);
      return;
    }
  }
  // MOUNT [DSK<n> <image>] — bare MOUNT lists currently-mounted drives.
  if (strncasecmp(&input[pos], "MOUNT", 5) == 0 &&
      (input[pos + 5] == '\0' || input[pos + 5] == ' '))
  {
    int p = pos + 5;
    while (input[p] == ' ') p++;
    if (input[p] == '\0')
    {
      int shown = 0;
      for (int d = 1; d <= fio::MAX_DSK; d++)
      {
        if (!fio::g_mounts[d].mounted) continue;
        char buf[64];
        snprintf(buf, sizeof(buf), "DSK%c = %s",
                 fio::driveToChar(d), fio::g_mounts[d].spec);
        printLine(buf);
        shown++;
      }
      if (shown == 0) printLine("(no disks mounted)");
      return;
    }
    int drive = 0;
    if (strncasecmp(&input[p], "DSK", 3) == 0)
    {
      drive = fio::driveFromChar(input[p + 3]);
    }
    if (drive == 0)
    {
      printError("* BAD DEVICE");
      return;
    }
    p += 4;
    while (input[p] == ' ') p++;
    if (input[p] == '"')
    {
      p++;
      char name[48]; int n = 0;
      while (input[p] && input[p] != '"' && n < (int)sizeof(name) - 1)
      {
        name[n++] = input[p++];
      }
      name[n] = '\0';
      cmdMount(drive, name);
    }
    else if (input[p] != '\0')
    {
      cmdMount(drive, &input[p]);
    }
    else
    {
      printError("* BAD FILE NAME");
    }
    return;
  }
  // UNMOUNT DSK<n>
  if (strncasecmp(&input[pos], "UNMOUNT", 7) == 0 &&
      (input[pos + 7] == '\0' || input[pos + 7] == ' '))
  {
    int p = pos + 7;
    while (input[p] == ' ') p++;
    int drive = 0;
    if (strncasecmp(&input[p], "DSK", 3) == 0)
    {
      drive = fio::driveFromChar(input[p + 3]);
    }
    if (drive == 0)
    {
      printError("* BAD DEVICE");
      return;
    }
    cmdUnmount(drive);
    return;
  }
  // NEWDISK <spec> "VOLNAME" [SSSD|DSSD|DSDD]
  if (strncasecmp(&input[pos], "NEWDISK", 7) == 0 &&
      (input[pos + 7] == '\0' || input[pos + 7] == ' '))
  {
    int p = pos + 7;
    while (input[p] == ' ') p++;

    char spec[48] = {0};
    int n = 0;
    while (input[p] && input[p] != ' ' && input[p] != ',' &&
           n < (int)sizeof(spec) - 1)
    {
      spec[n++] = input[p++];
    }
    spec[n] = '\0';

    char volName[12] = "NEWDISK";
    while (input[p] == ' ' || input[p] == ',') p++;
    if (input[p] == '"')
    {
      p++;
      n = 0;
      while (input[p] && input[p] != '"' && n < (int)sizeof(volName) - 1)
      {
        char c = input[p++];
        if (c >= 'a' && c <= 'z') c -= 32;
        volName[n++] = c;
      }
      volName[n] = '\0';
      if (input[p] == '"') p++;
    }

    int totalSectors = 360;   // SSSD default
    while (input[p] == ' ' || input[p] == ',') p++;
    if (strncasecmp(&input[p], "DSSD", 4) == 0) totalSectors = 720;
    else if (strncasecmp(&input[p], "DSDD", 4) == 0) totalSectors = 1440;

    cmdNewDisk(spec, volName, totalSectors);
    return;
  }
  // COPY <src> <dst>
  if (strncasecmp(&input[pos], "COPY", 4) == 0 &&
      (input[pos + 4] == '\0' || input[pos + 4] == ' '))
  {
    int p = pos + 4;
    while (input[p] == ' ') p++;

    char src[48] = {0};
    int n = 0;
    while (input[p] && input[p] != ' ' && input[p] != ',' &&
           n < (int)sizeof(src) - 1)
    {
      src[n++] = input[p++];
    }
    src[n] = '\0';

    while (input[p] == ' ' || input[p] == ',') p++;

    char dst[48] = {0};
    n = 0;
    while (input[p] && input[p] != ' ' && n < (int)sizeof(dst) - 1)
    {
      dst[n++] = input[p++];
    }
    dst[n] = '\0';

    if (!src[0] || !dst[0])
    {
      printError("* BAD FILE NAME");
      return;
    }
    cmdCopy(src, dst);
    return;
  }

  // Numbered line — store in program
  if (isdigit(input[pos]))
  {
    char* endp;
    uint16_t lineNum = (uint16_t)strtol(&input[pos], &endp, 10);
    pos = endp - input;

    while (input[pos] == ' ')
    {
      pos++;
    }

    // Just a line number — delete line
    if (input[pos] == '\0')
    {
      em.deleteLine(lineNum);
      return;
    }

    // Tokenize and store
    uint8_t tokens[MAX_LINE_TOKENS];
    int len = tokenizeLine(&input[pos], tokens, MAX_LINE_TOKENS);
    if (len < 0)
    {
      printError("* SYNTAX ERROR");
      return;
    }

    if (!em.storeLine(lineNum, tokens, len))
    {
      printError("* MEMORY FULL");
      return;
    }

    char status[40];
    snprintf(status, sizeof(status), "Lines: %d  Free: %dK",
             em.programSize(), (int)(ESP.getFreeHeap() / 1024));
    showStatus(status);
    return;
  }

  // Tokenize and execute immediately through the TP
  uint8_t tokens[MAX_LINE_TOKENS];
  int len = tokenizeLine(&input[pos], tokens, MAX_LINE_TOKENS);
  if (len < 0)
  {
    printLine("* SYNTAX ERROR");
    return;
  }

  em.runImmediate(tokens, len);
}

// ---------------------------------------------------------------------------
// SD lazy bring-up. Called from setup() once (initial mount) and again
// from fio::ensureSdReady() whenever the card has been invalidated and
// a caller needs it. The driver-level mount is idempotent enough that
// calling SD_MMC.end() before begin() works for retries.
// ---------------------------------------------------------------------------
static bool tryInitSd()
{
  Serial.println("[SD] attempting mount: SD_MMC.begin(\"/sdcard\", 1-bit, no-format, 20MHz)...");
  // Make sure any prior failed mount state is fully torn down so the
  // next begin() reprobes the card from scratch.
  SD_MMC.end();
  bool ok = SD_MMC.begin("/sdcard", /*mode1bit=*/true,
                          /*format_if_mount_failed=*/false,
                          /*sdmmc_frequency=*/20000);
  if (!ok)
  {
    Serial.println("[SD] mount FAILED. SD_MMC.begin returned false.");
    Serial.println("[SD]   Common causes: no SENSOR add-on board attached,");
    Serial.println("[SD]   no card inserted, wrong pin mapping, or unsupported card.");
    fio::setSDFs(nullptr);
    return false;
  }
  uint8_t cardType = SD_MMC.cardType();
  uint64_t cardSize = SD_MMC.cardSize();
  uint64_t totalBytes = SD_MMC.totalBytes();
  uint64_t usedBytes  = SD_MMC.usedBytes();
  const char* typeStr =
      (cardType == CARD_NONE)  ? "NONE" :
      (cardType == CARD_MMC)   ? "MMC"  :
      (cardType == CARD_SD)    ? "SD"   :
      (cardType == CARD_SDHC)  ? "SDHC" : "UNKNOWN";
  Serial.printf("[SD] mount OK. type=%s size=%lluMB total=%lluKB used=%lluKB\n",
                typeStr,
                (unsigned long long)(cardSize / (1024ULL * 1024ULL)),
                (unsigned long long)(totalBytes / 1024ULL),
                (unsigned long long)(usedBytes / 1024ULL));
  if (cardType == CARD_NONE || totalBytes == 0)
  {
    Serial.println("[SD] WARNING: mount returned OK but card reports no usable filesystem.");
    Serial.println("[SD]   Common causes: card is exFAT (not FAT32), card needs reformat,");
    Serial.println("[SD]   or card is physically present but the FS is corrupt.");
  }
  fio::setSDFs(&SD_MMC);
  return true;
}

// ---------------------------------------------------------------------------
// Setup and main loop
// ---------------------------------------------------------------------------

void setup()
{
  // Bump the RX buffer up from the 256-byte default so long program
  // pastes aren't dropped before the editor can drain them.
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  delay(500);

  // Silence the noisier subsystems' INFO chatter. NimBLE's GAP/GATT
  // events and WiFi's connection lifecycle each emit ~10 lines per
  // operation; useful for first bring-up, hostile for normal use.
  // Errors and warnings stay visible. Re-raise any of these to
  // ESP_LOG_INFO when actually debugging that subsystem.
  esp_log_level_set("NimBLE",     ESP_LOG_WARN);
  esp_log_level_set("BLE_INIT",   ESP_LOG_WARN);  // NimBLE port init
  esp_log_level_set("wifi",       ESP_LOG_WARN);
  esp_log_level_set("wifi_init",  ESP_LOG_WARN);
  esp_log_level_set("wpa",        ESP_LOG_WARN);
  esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);
  esp_log_level_set("dhcpc",      ESP_LOG_WARN);
  esp_log_level_set("phy_init",   ESP_LOG_WARN);

  // Box-3 has no independently-driven user LEDs: D3 LED-GREEN tracks
  // LCD_CTRL via U18B, so it lights automatically with the backlight.

  // Initialize LittleFS
  if (!LittleFS.begin(true))
  {
    Serial.println("LittleFS mount failed!");
  }
  else
  {
    Serial.println("LittleFS mounted.");
  }

  // NOTE: SD card init has to come AFTER initDisplay() below. LovyanGFX's
  // LGFX_AUTODETECT probes board signatures in a way that breaks the
  // SDMMC VFS mount even though the card itself stays present. Running
  // SD_MMC.begin() afterward avoids the rescue dance entirely.

  initDisplay();
  initAudio();

  // Now safe to bring up the SD card on the Box-3 SENSOR add-on (if
  // present). GPIO43 gates SD power through a P-FET (AO3401A), so it's
  // ACTIVE LOW — drive it LOW to turn the SD slot on. GPIO43 is U0TXD by
  // default; with CDCOnBoot=cdc the UART is unused, so we can repurpose
  // this pin freely.
  //
  // Init is now lazy via fio::ensureSdReady(): we register the init +
  // invalidate callbacks here, kick a first mount attempt, and then any
  // future SD failure (card yanked, DMA mem alloc fail) drops g_sdOk to
  // false and the NEXT access re-runs the init. Without the lazy
  // pattern, a one-time failure leaves the card unreachable until
  // reboot.
  pinMode(SD_PWR, OUTPUT);
  digitalWrite(SD_PWR, LOW);
  delay(50);
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);

  fio::setSDInitCallback([]() -> bool { return tryInitSd(); });
  fio::setSDInvalidateCallback([]() {
    Serial.println("[SD] invalidated. Re-init will fire on next access.");
    SD_MMC.end();
    fio::setSDFs(nullptr);
  });
  tryInitSd();

  // Restore any DSK<n> mounts from /mounts.cfg on LittleFS so that
  // `MOUNT` state survives reboots.
  loadMounts();

  // Restore font mode from NVS so `CALL CHARSET("TI")` survives reboot.
  {
    Preferences prefs;
    prefs.begin("boxbasic", true);
    uint8_t saved = prefs.getUChar("fontmode", TI_FONT_PC);
    prefs.end();
    setTiFontMode(saved == TI_FONT_TI ? TI_FONT_TI : TI_FONT_PC);
  }
  initCharPatterns();
  gfxResetColors();

  // Initialize framebuffer
  for (int r = 0; r < ROWS; r++)
  {
    memset(screenBuf[r], ' ', COLS);
    memset(prevScreenBuf[r], 0, COLS);
  }

  tiClearScreen();

  // Bring up BLE HID keyboard input BEFORE the boot screen so the scan
  // can start reconnecting while the user is still on the splash screens.
  // (F12 or BOOT button = pairing mode.)
  bleKbInit();

  // WiFi + web file manager. Reads NVS for credentials and starts WiFi
  // in STA mode. Non-blocking — the connect happens in the background
  // while the title screen and interpreter come up. BLE-first ordering
  // (above) gives BLE the cleaner coexistence slot per Espressif's
  // RF coexistence guide. With no creds in NVS, this is a no-op until
  // the user types CALL WIFI("ssid","pass") at the prompt.
  webfiles::begin();

  // Show TI boot screen and wait for a key
  showBootScreen();

  // User has dismissed the splash; switch BLE keyboard scan from
  // aggressive TITLE mode to idle INTERACTIVE mode. Radio goes quiet
  // unless a previously-connected bonded peer drops out, in which case
  // a single 60-second background-scan window will fire to reconnect.
  bleKbSetMode(BLE_KB_INTERACTIVE);

  // Cursor starts at bottom row (TI behavior)
  cursorRow = ROWS - 1;
  cursorCol = 0;
  printLine("* READY *");
  tiPrintString(">");

  // Graphics / sprite / input glue is provided via strong overrides
  // of the weak tiXxx symbols declared in ti_platform.h — no
  // setCallbacks needed for those. Language-layer callbacks (file I/O,
  // command dispatch, throttle) still use explicit setCallbacks.
  em.tp()->setCommandCallbacks(cmdNew, cmdList, cmdRun, cmdSave,
                               cmdOld, cmdBye, cmdDir);
  em.tp()->setCmdSize(cmdSize);
  em.tp()->setCmdTrace(cmdTrace);
  em.tp()->setCmdBreak(cmdBreak);
  em.tp()->setCmdRes(cmdResequence);
  em.tp()->setCmdNum(cmdNumber);
  em.tp()->setCmdMerge(cmdMerge);
  em.tp()->setCmdDelete(cmdDelete);
  em.tp()->setCmdContinue(cmdContinue);
  em.tp()->setFileCallbacks(shimFileOpen, shimFileClose, shimFilePrint,
                            shimFileReadLine, shimFileEof);
  em.tp()->setFileSeekRec(shimFileSeekRec);
  em.tp()->setFileRewind(shimFileRewind);
  em.tp()->setThrottleCallback([](unsigned long us) {
    em.m_throttleUs = us;
    em.tp()->setThrottleUs(us);
  });
  em.setProgramEnded(gfxReset);
  em.setPerLineTick(spriteTick);
  em.setPrepareInput(gfxPrepareInput);
  em.setPrintLine(printLine);
  em.setPrintError(printError);
  em.setPrintString(tiPrintString);
  em.setGetLine(getInputLine);

  char statusBuf[40];
  snprintf(statusBuf, sizeof(statusBuf), "TI BASIC Sim | Free: %dK",
           (int)(ESP.getFreeHeap() / 1024));
  showStatus(statusBuf);

  Serial.println("TI Extended BASIC Simulator v0.2");
  Serial.println("Type BASIC commands. Serial input active.");
}

void loop()
{
  pasteDrainSerial();
  bleKbTask();
  webfiles::tick();
  checkInput();
  spriteTick();

  // Full-screen takeover while BLE pairing is open. User triggered
  // pairing (BOOT button / F12 / watchdog) so they're not using
  // BASIC; we use the whole panel for a clear "PAIRING — press the
  // pair button" UI with a live countdown. On exit, restore the
  // BASIC display from screenBuf.
  static bool prevPairingMode = false;
  static unsigned long lastCountdown = 0;
  // Show takeover UI only for user-initiated pairing (BOOT / F12).
  // Silent watchdog reconnects open the same scan window but keep
  // userInitiatedPairing() = false, so a running BASIC program isn't
  // interrupted just because the keyboard fell asleep.
  bool nowPairing = BleHidHost::userInitiatedPairing();
  if (nowPairing != prevPairingMode)
  {
    if (nowPairing)
    {
      drawPairingScreen();
    }
    else
    {
      // Exit: restore the BASIC display we hid.
      fillBackground(bgColor);
      redrawScreen();
    }
    prevPairingMode = nowPairing;
    lastCountdown = 0;
  }
  if (nowPairing)
  {
    unsigned long now = millis();
    if (now - lastCountdown >= 500)
    {
      updatePairingCountdown(BleHidHost::pairingRemainingMs());
      lastCountdown = now;
    }
  }

  if (inputReady)
  {
    processInput(inputBuf);
    tiPrintString(">");
    inputPos = 0;
    inputReady = false;
  }

  // Yield once per main-task pass so the FreeRTOS scheduler can run
  // IDLE1, the BLE host task, etc. tiYield() is the host's strong
  // override of the interpreter's weak hook — same yield primitive
  // used everywhere in the interpreter and graphics layers.
  tiYield();
}
