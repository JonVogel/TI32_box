/*
 * TI BASIC Interpreter — Shared Types
 *
 * Token values match the TI-99/4A Extended BASIC internal format.
 * Reference: https://www.unige.ch/medecine/nouspikel/ti99/basic.htm
 *            https://ninerpedia.org/wiki/BASIC_tokens
 *
 * Storage format:
 *   Each line: [length byte] [tokens...] [0x00 terminator]
 *
 *   Variables:  raw ASCII chars (e.g. "A" = 0x41, "U$" = 0x55 0x24)
 *   Numbers:    0xC8 + length + ASCII digits (unquoted string)
 *   Strings:    0xC7 + length + ASCII chars (quoted string)
 *   Line #:     0xC9 + 2 binary bytes (after GOTO/GOSUB)
 *   Keywords:   single byte tokens >= 0x80
 *   ASCII:      bytes < 0x80 that aren't identifier chars are literal
 */

#ifndef TP_TYPES_H
#define TP_TYPES_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Token Parser return values
// ---------------------------------------------------------------------------

enum TPResult : uint8_t
{
  TP_NEXT_TOKEN,
  TP_NEXT_LINE,
  TP_GOTO_LINE,
  TP_GOTO_AFTER,    // Go to line AFTER the specified one (for RETURN)
  TP_NEXT_LOOP,
  TP_END_LOOP,
  TP_NEED_INPUT,
  TP_FINISHED,
  TP_STOPPED,       // STOP — like FINISHED but CONTINUE can resume
  TP_ERROR,
  TP_WARNING,       // Recoverable diagnostic — EM honors ON WARNING mode
  TP_CALL_SUB,      // Push return addr, jump to lineNum (subprogram entry)
  TP_SUB_RETURN     // Pop return addr and jump (SUBEND / SUBEXIT)
};

// Modes for ON BREAK / ON ERROR / ON WARNING handler configuration.
enum OnBreakMode   : uint8_t { OB_STOP = 0, OB_NEXT = 1 };
enum OnErrorMode   : uint8_t { OE_STOP = 0, OE_NEXT = 1, OE_GOTO = 2 };
enum OnWarningMode : uint8_t { OW_PRINT = 0, OW_STOP = 1, OW_NEXT = 2 };

// For CALL sub(args): when an arg is a bare variable reference, we record
// an alias so the sub's modifications to that parameter are copied back
// into the caller's variable on SUBEND/SUBEXIT (pass-by-value-result).
struct TPSubAlias
{
  char    paramName[12];
  uint8_t paramLen;
  char    callerName[12];
  uint8_t callerLen;
  bool    isStr;
};

struct TPResponse
{
  TPResult result;
  uint16_t lineNum;         // target line NUMBER for GOTO/GOSUB/…
  uint16_t lineIdx = 0;     // target line INDEX for TP_CALL_SUB
  char errorMsg[40];
  char prompt[80];
  bool cursorPrePositioned = false;   // ACCEPT AT: EM should NOT move cursor
  TPSubAlias aliases[6];              // only used with TP_CALL_SUB
  uint8_t aliasCount = 0;
};

// ---------------------------------------------------------------------------
// Token values (TI BASIC / Extended BASIC compatible)
// ---------------------------------------------------------------------------

enum Token : uint8_t
{
  // --- End of line / end of file ---
  TOK_EOL = 0x00,

  // --- Immediate commands (0x00-0x09) ---
  // NEW=0x00 conflicts with TOK_EOL, so we don't store NEW in the token stream.
  // Same for RUN — handled as pre-tokenize string match.
  TOK_CONTINUE = 0x01,
  TOK_LIST   = 0x02,
  TOK_BYE    = 0x03,
  TOK_NUM    = 0x04,   // NUMBER / AUTO
  TOK_OLD    = 0x05,
  TOK_RES    = 0x06,   // RESEQUENCE
  TOK_SAVE   = 0x07,
  TOK_MERGE  = 0x08,
  TOK_EDIT   = 0x09,

  // --- Statements (0x80-0xAF) ---
  TOK_ELSE       = 0x81,
  TOK_DCOLON     = 0x82,   // :: (Extended BASIC)
  TOK_BANG       = 0x83,   // ! (Extended BASIC comment)
  TOK_IF         = 0x84,
  TOK_GO         = 0x85,
  TOK_GOTO       = 0x86,
  TOK_GOSUB      = 0x87,
  TOK_RETURN     = 0x88,
  TOK_DEF        = 0x89,
  TOK_DIM        = 0x8A,
  TOK_END        = 0x8B,
  TOK_FOR        = 0x8C,
  TOK_LET        = 0x8D,
  TOK_BREAK      = 0x8E,
  TOK_UNBREAK    = 0x8F,
  TOK_TRACE      = 0x90,
  TOK_UNTRACE    = 0x91,
  TOK_INPUT      = 0x92,
  TOK_DATA       = 0x93,
  TOK_RESTORE    = 0x94,
  TOK_RANDOMIZE  = 0x95,
  TOK_NEXT       = 0x96,
  TOK_READ       = 0x97,
  TOK_STOP       = 0x98,
  TOK_DELETE     = 0x99,
  TOK_REM        = 0x9A,
  TOK_ON         = 0x9B,
  TOK_PRINT      = 0x9C,
  TOK_CALL       = 0x9D,
  TOK_OPTION     = 0x9E,
  TOK_OPEN       = 0x9F,
  TOK_CLOSE      = 0xA0,
  TOK_SUB        = 0xA1,
  TOK_DISPLAY    = 0xA2,
  TOK_IMAGE      = 0xA3,
  TOK_ACCEPT     = 0xA4,
  TOK_ERROR      = 0xA5,
  TOK_WARNING    = 0xA6,
  TOK_SUBEXIT    = 0xA7,
  TOK_SUBEND     = 0xA8,
  TOK_RUN        = 0xA9,
  TOK_LINPUT     = 0xAA,

  // --- Secondary keywords (0xB0-0xB8) ---
  TOK_THEN       = 0xB0,
  TOK_TO         = 0xB1,
  TOK_STEP       = 0xB2,
  TOK_COMMA      = 0xB3,
  TOK_SEMICOLON  = 0xB4,
  TOK_COLON      = 0xB5,
  TOK_RPAREN     = 0xB6,
  TOK_LPAREN     = 0xB7,
  TOK_CONCAT     = 0xB8,   // &

  // --- Operators (0xBA-0xC5) ---
  TOK_OR         = 0xBA,
  TOK_AND        = 0xBB,
  TOK_XOR        = 0xBC,
  TOK_NOT        = 0xBD,
  TOK_EQUAL      = 0xBE,   // = (used for assignment AND comparison)
  TOK_LESS       = 0xBF,
  TOK_GREATER    = 0xC0,
  TOK_PLUS       = 0xC1,
  TOK_MINUS      = 0xC2,
  TOK_MULTIPLY   = 0xC3,
  TOK_DIVIDE     = 0xC4,
  TOK_POWER      = 0xC5,

  // --- Literal markers (0xC7-0xCA) ---
  TOK_QUOTED_STR   = 0xC7,   // "..." — followed by length + chars
  TOK_UNQUOTED_STR = 0xC8,   // number literal or unquoted id — length + chars
  TOK_LINENUM      = 0xC9,   // line number — followed by 2 binary bytes
  TOK_EOF_MARK     = 0xCA,

  // --- Source-code aliases ---
  TOK_STRING_LIT   = TOK_QUOTED_STR,       // alias for 0xC7
  TOK_NUMBER_LIT   = TOK_UNQUOTED_STR,     // alias for 0xC8 (ASCII digits)

  // --- Numeric functions (0xCB-0xDC) ---
  TOK_ABS        = 0xCB,
  TOK_ATN        = 0xCC,
  TOK_COS        = 0xCD,
  TOK_EXP        = 0xCE,
  TOK_INT        = 0xCF,
  TOK_LOG        = 0xD0,
  TOK_SGN        = 0xD1,
  TOK_SIN        = 0xD2,
  TOK_SQR        = 0xD3,
  TOK_TAN        = 0xD4,
  TOK_LEN        = 0xD5,
  TOK_CHR        = 0xD6,   // CHR$
  TOK_RND        = 0xD7,
  TOK_SEG        = 0xD8,   // SEG$
  TOK_POS        = 0xD9,
  TOK_VAL        = 0xDA,
  TOK_STR        = 0xDB,   // STR$
  TOK_ASC        = 0xDC,

  // --- Extended BASIC functions (0xDD-0xFE) ---
  TOK_PI         = 0xDD,
  TOK_REC        = 0xDE,
  TOK_MAX        = 0xDF,
  TOK_MIN        = 0xE0,
  TOK_RPT        = 0xE1,   // RPT$
  TOK_NUMERIC    = 0xE8,
  TOK_DIGIT      = 0xE9,
  TOK_UALPHA     = 0xEA,
  TOK_SIZE       = 0xEB,
  TOK_ALL        = 0xEC,
  TOK_USING      = 0xED,
  TOK_BEEP       = 0xEE,
  TOK_ERASE      = 0xEF,
  TOK_AT         = 0xF0,
  TOK_BASE       = 0xF1,
  TOK_VARIABLE_KW = 0xF3,   // keyword in OPEN statements (VARIABLE)
  TOK_RELATIVE   = 0xF4,
  TOK_INTERNAL   = 0xF5,
  TOK_SEQUENTIAL = 0xF6,
  TOK_OUTPUT     = 0xF7,
  TOK_UPDATE     = 0xF8,
  TOK_APPEND     = 0xF9,
  TOK_FIXED      = 0xFA,
  TOK_PERMANENT  = 0xFB,
  TOK_TAB        = 0xFC,
  TOK_HASH       = 0xFD,   // #
  TOK_VALIDATE   = 0xFE
};

// ---------------------------------------------------------------------------
// Program line storage
// ---------------------------------------------------------------------------

#define MAX_LINES       1000
#define MAX_LINE_TOKENS 256

// TI-99/4A standard screen dimensions (used by CALL HCHAR/VCHAR/GCHAR
// bounds and wrap logic). Matches the real TI grid regardless of the
// physical display's char count in the main sketch.
#define TI_COLS         32
#define TI_ROWS         24
#define MAX_INPUT_LEN   140

struct ProgramLine
{
  uint16_t lineNum;
  uint8_t  tokens[MAX_LINE_TOKENS];
  uint8_t  length;
};

// ---------------------------------------------------------------------------
// Variable storage
// ---------------------------------------------------------------------------

#define MAX_VARS     100
#define MAX_VAR_NAME 16
#define MAX_STRINGS  200
#define MAX_STR_LEN  256

#define MAX_ARRAY_DIMS 3

struct Variable
{
  char name[MAX_VAR_NAME];
  bool isString;
  uint8_t dimCount;                  // 0 = scalar, 1-3 = array
  uint16_t dimSize[MAX_ARRAY_DIMS];  // size of each dimension (inclusive)
  float numVal;                      // scalar numeric value
  int strIndex;                      // scalar string index
  void* arrayData;                   // array data (float[] or int[])
};

// ---------------------------------------------------------------------------
// FOR loop stack
// ---------------------------------------------------------------------------

#define MAX_FOR_DEPTH 10

struct ForFrame
{
  char varName[MAX_VAR_NAME];
  float limit;
  float step;
  uint16_t forLineNum;
};

// ---------------------------------------------------------------------------
// GOSUB stack
// ---------------------------------------------------------------------------

#define MAX_GOSUB_DEPTH 10

struct GosubFrame
{
  uint16_t returnLineNum;
};

// ---------------------------------------------------------------------------
// Helpers to identify token types
// ---------------------------------------------------------------------------

// Is this byte a valid start of a variable name? (ASCII letter)
inline bool isIdentStart(uint8_t c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

// Is this byte a valid continuation of a variable name? (alnum or $)
inline bool isIdentCont(uint8_t c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '$';
}

// Parse an identifier from the token stream starting at *pos.
// Advances pos past the identifier. Returns length, or 0 if not an ident.
// Sets *isStr to true if name ends with $.
inline int parseIdent(const uint8_t* tokens, int* pos, char* name,
                      int maxLen, bool* isStr)
{
  int n = 0;
  if (!isIdentStart(tokens[*pos]))
  {
    name[0] = '\0';
    if (isStr) *isStr = false;
    return 0;
  }
  while (isIdentCont(tokens[*pos]) && n < maxLen - 1)
  {
    name[n++] = (char)tokens[*pos];
    (*pos)++;
  }
  name[n] = '\0';
  if (isStr)
  {
    *isStr = (n > 0 && name[n - 1] == '$');
  }
  return n;
}

// Peek at identifier without advancing pos
inline int peekIdent(const uint8_t* tokens, int pos, char* name, int maxLen)
{
  int n = 0;
  if (!isIdentStart(tokens[pos]))
  {
    name[0] = '\0';
    return 0;
  }
  while (isIdentCont(tokens[pos]) && n < maxLen - 1)
  {
    name[n++] = (char)tokens[pos++];
  }
  name[n] = '\0';
  return n;
}

#endif // TP_TYPES_H
