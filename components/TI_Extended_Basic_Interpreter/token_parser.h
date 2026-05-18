/*
 * TI BASIC Interpreter — Token Parser (TP)
 *
 * State machine that processes tokens one statement at a time.
 * Called by the Execution Manager with a line of tokens.
 * Returns flow control decisions (NEXT_LINE, GOTO, NEXT_LOOP, etc.)
 *
 * The TP owns the variable table and FOR stack.
 * It calls submodules for expression evaluation, PRINT, etc.
 */

#ifndef TOKEN_PARSER_H
#define TOKEN_PARSER_H

#include "tp_types.h"
#include "var_table.h"
#include "expr_parser.h"
#include "sprites.h"
#include "ti_platform.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

// Forward declaration — display output callback
typedef void (*PrintCharFn)(char c);
typedef void (*PrintStringFn)(const char* str);
typedef void (*ClearScreenFn)();

// Graphics callbacks — for CALL HCHAR, VCHAR, GCHAR, SCREEN, COLOR
typedef void (*SetCharFn)(int row, int col, char ch);
typedef char (*GetCharFn)(int row, int col);
typedef void (*SetScreenColorFn)(int colorIdx);
typedef void (*SetCharColorFn)(int charSet, int fg, int bg);
typedef void (*SetCharPatternFn)(int charCode, const uint8_t* pattern8);
typedef void (*GetCharPatternFn)(int charCode, uint8_t* out8);
typedef void (*ResetCharsetFn)();
// CALL CHARSET("PC" | "TI") — switch the active font ROM. Mode values match
// the TiFontMode enum (0 = PC default, 1 = TI authentic).
typedef void (*SetCharsetModeFn)(int mode);
typedef int  (*ReadKeyFn)();           // returns 0 = no key, else char code
// CALL JOYST(unit, X, Y) — fills X and Y with -4 / 0 / +4 axis state.
typedef void (*ReadJoystickFn)(int unit, int* outX, int* outY);
typedef void (*MoveCursorFn)(int row, int col);

// DATA support: EM scans program for next DATA value.
// nextData fills buf with next data value (as text). Returns false if no more.
typedef bool (*NextDataFn)(char* buf, int bufSize);
// resetData resets to the start of program, or to first DATA at/after lineNum.
typedef void (*ResetDataFn)(uint16_t lineNum);

// Command callbacks — provided by the EM for immediate commands
typedef void (*CmdNewFn)();
// LIST range: startLine/endLine of -1 means "unspecified" (open-ended).
typedef void (*CmdListFn)(int startLine, int endLine);
typedef void (*CmdRunFn)();
typedef void (*CmdSaveFn)(const char* filename);
typedef void (*CmdOldFn)(const char* filename);
typedef void (*CmdMergeFn)(const char* filename);
typedef void (*CmdDeleteFn)(const char* filename);
typedef void (*CmdContinueFn)();

// Sprite rendering — implemented by the sketch (drawCell + pixel layer).
typedef void (*SpriteDrawFn)(int slot);
typedef void (*SpriteEraseFn)(int slot);

// CALL SPEED(usPerLine) — sets execution-throttle in EM.
typedef void (*SetThrottleFn)(unsigned long us);

// PRINT USING <lineN>: looks up an IMAGE statement by line number.
// Out: pointer + length to the format string. Returns true on hit.
typedef bool (*ImageLookupFn)(uint16_t lineNum, const char** outStr,
                              int* outLen);

// File I/O callbacks — thin shims over file_io.h. Return 0 on success,
// or a TI Extended BASIC two-digit I/O error code (0..7, see
// fio::TIIoError) on failure. The shim is responsible for translating
// the underlying filesystem error to the appropriate TI code; the
// interpreter never sees raw fio return values.
typedef int (*FileOpenFn)(int unit, const char* spec, int mode,
                          int flags, int recLen);
// File flag bits — must mirror fio::OF_* in file_io.h.
static const int FF_INTERNAL = 0x01;
static const int FF_FIXED    = 0x02;
static const int FF_RELATIVE = 0x04;
// Per-unit RELATIVE record positioning (PRINT #N, REC=K).
typedef bool (*FileSeekRecFn)(int unit, long rec);
// RESTORE #N — rewind a file to the first record without closing it.
typedef bool (*FileRewindFn)(int unit);
typedef int (*FileCloseFn)(int unit);
typedef int (*FilePrintFn)(int unit, const char* text);
typedef int (*FileReadLineFn)(int unit, char* buf, int bufSize);
typedef bool (*FileEofFn)(int unit);
typedef void (*CmdByeFn)();
typedef void (*CmdDirFn)();
typedef void (*CmdSizeFn)();
typedef void (*CmdTraceFn)(bool enable);
typedef void (*CmdBreakFn)(const int* lines, int count, bool add);
typedef void (*CmdResFn)(int startLine, int increment);
typedef void (*CmdNumFn)(int startLine, int increment);

class TokenParser
{
public:
  TokenParser()
    : m_expr(&m_vars),
      m_printChar(tiPrintChar),
      m_printString(tiPrintString),
      m_clearScreen(tiClearScreen)
  {
  }

  // Set display output callbacks
  void setCallbacks(PrintCharFn pc, PrintStringFn ps, ClearScreenFn cs)
  {
    m_printChar = pc;
    m_printString = ps;
    m_clearScreen = cs;
  }

  // Set command callbacks
  void setCommandCallbacks(CmdNewFn n, CmdListFn l, CmdRunFn r,
                           CmdSaveFn s, CmdOldFn o, CmdByeFn b,
                           CmdDirFn d)
  {
    m_cmdNew = n;
    m_cmdList = l;
    m_cmdRun = r;
    m_cmdSave = s;
    m_cmdOld = o;
    m_cmdBye = b;
    m_cmdDir = d;
  }

  // Set graphics callbacks
  void setGraphicsCallbacks(SetCharFn sc, GetCharFn gc,
                            SetScreenColorFn ss, SetCharColorFn cc,
                            SetCharPatternFn cp)
  {
    m_setChar = sc;
    m_getChar = gc;
    m_setScreenColor = ss;
    m_setCharColor = cc;
    m_setCharPattern = cp;
  }

  void setReadKey(ReadKeyFn rk) { m_readKey = rk; }
  void setReadJoystick(ReadJoystickFn fn) { m_readJoystick = fn; }
  void setMoveCursor(MoveCursorFn mc) { m_moveCursor = mc; }
  void setGetCharPattern(GetCharPatternFn fn) { m_getCharPattern = fn; }
  void setResetCharset(ResetCharsetFn fn)     { m_resetCharset   = fn; }
  void setSetCharsetMode(SetCharsetModeFn fn) { m_setCharsetMode = fn; }

  void setCmdSize(CmdSizeFn f)   { m_cmdSize = f; }
  void setCmdTrace(CmdTraceFn f) { m_cmdTrace = f; }
  void setCmdBreak(CmdBreakFn f) { m_cmdBreak = f; }
  void setCmdRes(CmdResFn f)     { m_cmdRes = f; }
  void setCmdNum(CmdNumFn f)     { m_cmdNum = f; }
  void setCmdMerge(CmdMergeFn f) { m_cmdMerge = f; }
  void setCmdDelete(CmdDeleteFn f) { m_cmdDelete = f; }
  void setCmdContinue(CmdContinueFn f) { m_cmdContinue = f; }

  void setSpriteCallbacks(SpriteDrawFn d, SpriteEraseFn e)
  {
    m_spriteDraw  = d;
    m_spriteErase = e;
  }

  void setThrottleCallback(SetThrottleFn f) { m_setThrottle = f; }
  void setImageLookup(ImageLookupFn f) { m_imageLookup = f; }

  void setFileCallbacks(FileOpenFn o, FileCloseFn c, FilePrintFn p,
                        FileReadLineFn r, FileEofFn e)
  {
    m_fileOpen = o;
    m_fileClose = c;
    m_filePrint = p;
    m_fileReadLine = r;
    m_fileEof = e;
    m_expr.setFileEof(e);
  }
  void setFileSeekRec(FileSeekRecFn f) { m_fileSeekRec = f; }
  void setFileRewind(FileRewindFn f)   { m_fileRewind  = f; }
  void setDataCallbacks(NextDataFn nd, ResetDataFn rd)
  {
    m_nextData = nd;
    m_resetData = rd;
  }

  // Reset all state for a new RUN
  void reset()
  {
    m_vars.clear();
    m_forDepth = 0;
    m_gosubDepth = 0;
    m_onBreakMode   = OB_STOP;
    m_onErrorMode   = OE_STOP;
    m_onErrorLine   = 0;
    m_onWarningMode = OW_PRINT;
    m_lastErrorLine = 0;
    m_pendingIOError = -1;
    m_lastIOError    = 0;
    m_soundEndTime  = 0;
  }

  // --- ON BREAK / ON ERROR / ON WARNING accessors ---
  OnBreakMode   onBreakMode()   const { return m_onBreakMode; }
  OnErrorMode   onErrorMode()   const { return m_onErrorMode; }
  uint16_t      onErrorLine()   const { return m_onErrorLine; }
  OnWarningMode onWarningMode() const { return m_onWarningMode; }
  uint16_t      lastErrorLine() const { return m_lastErrorLine; }
  void setLastErrorLine(uint16_t ln) { m_lastErrorLine = ln; }
  void disarmOnError() { m_onErrorMode = OE_STOP; m_onErrorLine = 0; }

  // I/O error reporting. void-returning statement executors (execPrint
  // for PRINT #n, execInputFile for INPUT/LINPUT #n) can't surface a
  // TPResponse, so they stash a pending TI I/O error code here. The
  // statement-dispatch loop in processLine() checks this after each
  // statement and promotes it to TP_ERROR, which flows through the
  // existing ON ERROR / CALL ERR machinery in ExecManager.
  //   `tiCode` is one of the fio::TIIoError values (0..7).
  void raiseIOError(int tiCode)
  {
    m_pendingIOError = tiCode;
    m_lastIOError = tiCode;
  }
  int  lastIOError() const { return m_lastIOError; }
  bool hasPendingIOError() const { return m_pendingIOError >= 0; }
  int  pendingIOError() const { return m_pendingIOError; }
  void clearPendingIOError() { m_pendingIOError = -1; }

  // Process a full line of tokens. Returns flow control decision.
  TPResponse processLine(const uint8_t* tokens, int length,
                         uint16_t lineNum, bool isLoopIteration = false)
  {
    TPResponse resp;
    resp.result = TP_NEXT_LINE;
    resp.lineNum = 0;
    resp.errorMsg[0] = '\0';

    m_expr.clearError();

    int pos = 0;

    while (pos < length && tokens[pos] != TOK_EOL)
    {
      // Expression evaluator raised a diagnostic during the previous
      // statement — promote it to a line-level TP_ERROR / TP_WARNING.
      if (m_expr.hasDiag())
      {
        resp.result = m_expr.hasError() ? TP_ERROR : TP_WARNING;
        snprintf(resp.errorMsg, sizeof(resp.errorMsg), "%s",
                 m_expr.errorMsg());
        m_expr.clearError();
        return resp;
      }

      // File I/O raised a TI I/O error during the previous statement —
      // promote it to a line-level TP_ERROR. The two-digit code (per TI
      // Extended BASIC convention) is preserved in m_lastIOError so
      // CALL ERR can report it inside an ON ERROR handler.
      if (hasPendingIOError())
      {
        int code = pendingIOError();
        clearPendingIOError();
        resp.result = TP_ERROR;
        snprintf(resp.errorMsg, sizeof(resp.errorMsg),
                 "I/O ERROR %02d", code);
        return resp;
      }

      uint8_t tok = tokens[pos];

      // Statement separator. `::` tokenizes as two TOK_COLONs in a row;
      // collapse them so a `::` counts as one separator (and one throttle).
      if (tok == TOK_COLON)
      {
        pos++;
        while (tokens[pos] == TOK_COLON) pos++;
        if (m_throttleUs > 0)
        {
          if (m_throttleUs >= 1000) delay(m_throttleUs / 1000);
          else                      delayMicroseconds(m_throttleUs);
        }
        continue;
      }

      // Raw ASCII identifier = start of implicit assignment (e.g. A=5).
      // If no '=' follows, it's a syntax error (e.g. user typed "RUB").
      if (isIdentStart(tok))
      {
        if (!execAssignment(tokens, &pos))
        {
          resp.result = TP_ERROR;
          snprintf(resp.errorMsg, sizeof(resp.errorMsg), "SYNTAX ERROR");
          return resp;
        }
        continue;
      }

      // Dispatch based on statement type
      switch (tok)
      {
        case TOK_PRINT:
          pos++;
          execPrint(tokens, &pos);
          break;

        case TOK_LET:
          pos++;
          if (!execAssignment(tokens, &pos))
          {
            resp.result = TP_ERROR;
            snprintf(resp.errorMsg, sizeof(resp.errorMsg), "SYNTAX ERROR");
            return resp;
          }
          break;

        case TOK_GOTO:
        {
          pos++;
          resp = execGoto(tokens, &pos);
          return resp;
        }

        case TOK_GOSUB:
        {
          pos++;
          resp = execGosub(tokens, &pos, lineNum);
          return resp;
        }

        case TOK_RETURN:
        {
          pos++;
          resp = execReturn();
          return resp;
        }

        case TOK_IF:
        {
          pos++;
          resp = execIf(tokens, &pos, length);
          if (resp.result == TP_GOTO_LINE)
          {
            return resp;
          }
          // If result is NEXT_LINE but we consumed ELSE, keep processing
          if (resp.result == TP_NEXT_LINE)
          {
            return resp;
          }
          // TP_NEXT_TOKEN means keep processing rest of line (THEN clause)
          break;
        }

        case TOK_FOR:
        {
          pos++;
          resp = execFor(tokens, &pos, lineNum, isLoopIteration);
          if (resp.result != TP_NEXT_TOKEN)
          {
            return resp;
          }
          break;
        }

        case TOK_NEXT:
        {
          pos++;
          resp = execNext(tokens, &pos);
          return resp;
        }

        case TOK_CALL:
        {
          pos++;
          resp = execCall(tokens, &pos);
          if (resp.result != TP_NEXT_TOKEN)
          {
            return resp;
          }
          break;
        }

        case TOK_INPUT:
        {
          pos++;
          resp = execInput(tokens, &pos);
          return resp;
        }

        case TOK_LINPUT:
        {
          pos++;
          m_linputMode = true;
          resp = execInput(tokens, &pos);
          return resp;
        }

        case TOK_DISPLAY:
        {
          pos++;
          execDisplay(tokens, &pos);
          break;
        }

        case TOK_ACCEPT:
        {
          pos++;
          resp = execAccept(tokens, &pos);
          return resp;
        }

        case TOK_DIM:
        {
          pos++;
          execDim(tokens, &pos);
          break;
        }

        case TOK_READ:
        {
          pos++;
          execRead(tokens, &pos);
          break;
        }

        case TOK_DATA:
        {
          // DATA is passively consumed — scan past to EOL
          while (pos < length && tokens[pos] != TOK_EOL &&
                 tokens[pos] != TOK_COLON)
          {
            pos++;
          }
          break;
        }

        case TOK_RESTORE:
        {
          pos++;
          // RESTORE #n — rewind file unit n to its first record.
          if (tokens[pos] == TOK_HASH)
          {
            pos++;
            int unit = (int)m_expr.evalNumeric(tokens, &pos);
            if (m_fileRewind) m_fileRewind(unit);
            break;
          }
          // RESTORE [<line>] — reset DATA pointer.
          uint16_t lineNum = 0;
          if (tokens[pos] == TOK_LINENUM ||
              tokens[pos] == TOK_UNQUOTED_STR)
          {
            lineNum = readLineNum(tokens, &pos);
          }
          if (m_resetData) m_resetData(lineNum);
          break;
        }

        case TOK_RANDOMIZE:
        {
          pos++;
          // Optional seed value
          if (tokens[pos] != TOK_EOL && tokens[pos] != TOK_COLON)
          {
            float seed = m_expr.evalNumeric(tokens, &pos);
            randomSeed((unsigned long)seed);
          }
          else
          {
            randomSeed(micros());
          }
          break;
        }

        case TOK_ON:
        {
          pos++;
          resp = execOn(tokens, &pos, lineNum);
          if (resp.result == TP_GOTO_LINE)
          {
            return resp;
          }
          break;
        }

        case TOK_DEF:
        {
          pos++;
          execDef(tokens, &pos, length);
          return resp;   // DEF consumes the rest of the line
        }

        case TOK_SUB:
          // A SUB declaration at runtime means we're either inside a
          // call (EM skips directly to body) or we've fallen into one
          // from main flow (EM handles that skip). Either way nothing
          // to do here — just finish the line.
          return resp;

        case TOK_SUBEND:
        case TOK_SUBEXIT:
        {
          TPResponse out;
          out.result = TP_SUB_RETURN;
          out.errorMsg[0] = '\0';
          return out;
        }

        case TOK_OPTION:
        {
          pos++;
          // OPTION BASE 0 or 1
          if (tokens[pos] == TOK_BASE)
          {
            pos++;
            m_optionBase = (int)m_expr.evalNumeric(tokens, &pos);
          }
          else
          {
            while (pos < length && tokens[pos] != TOK_EOL &&
                   tokens[pos] != TOK_COLON)
            {
              pos++;
            }
          }
          break;
        }

        case TOK_LIST:
        {
          pos++;
          // Parse optional range: [start] [ - [end] ]
          // Accepts: LIST | LIST n | LIST n- | LIST -m | LIST n-m
          int startLine = -1;
          int endLine = -1;
          if (tokens[pos] == TOK_UNQUOTED_STR)
          {
            startLine = (int)readNumber(tokens, &pos);
            endLine = startLine;   // single line unless '-' follows
          }
          if (tokens[pos] == TOK_MINUS)
          {
            pos++;
            endLine = -1;   // open-ended after '-'
            if (tokens[pos] == TOK_UNQUOTED_STR)
            {
              endLine = (int)readNumber(tokens, &pos);
            }
          }
          if (m_cmdList) m_cmdList(startLine, endLine);
          return resp;
        }

        case TOK_BYE:
          if (m_cmdBye) m_cmdBye();
          return resp;

        case TOK_SAVE:
        {
          pos++;
          char filename[32] = "PROGRAM";
          extractFilename(tokens, &pos, filename, sizeof(filename));
          if (m_cmdSave) m_cmdSave(filename);
          return resp;
        }

        case TOK_OLD:
        {
          pos++;
          char filename[32] = "PROGRAM";
          extractFilename(tokens, &pos, filename, sizeof(filename));
          if (m_cmdOld) m_cmdOld(filename);
          return resp;
        }

        case TOK_MERGE:
        {
          pos++;
          char filename[32] = "PROGRAM";
          extractFilename(tokens, &pos, filename, sizeof(filename));
          if (m_cmdMerge) m_cmdMerge(filename);
          return resp;
        }

        case TOK_DELETE:
        {
          pos++;
          char filename[32] = "";
          extractFilename(tokens, &pos, filename, sizeof(filename));
          if (m_cmdDelete) m_cmdDelete(filename);
          return resp;
        }

        case TOK_CONTINUE:
          if (m_cmdContinue) m_cmdContinue();
          return resp;

        case TOK_SIZE:
          if (m_cmdSize) m_cmdSize();
          return resp;

        case TOK_TRACE:
          if (m_cmdTrace) m_cmdTrace(true);
          return resp;

        case TOK_UNTRACE:
          if (m_cmdTrace) m_cmdTrace(false);
          return resp;

        case TOK_BREAK:
        case TOK_UNBREAK:
        {
          pos++;
          int lines[16];
          int count = 0;
          while (count < 16 && tokens[pos] != TOK_EOL &&
                 tokens[pos] != TOK_COLON)
          {
            if (tokens[pos] == TOK_LINENUM ||
                tokens[pos] == TOK_UNQUOTED_STR)
            {
              lines[count++] = (int)readLineNum(tokens, &pos);
            }
            else
            {
              pos++;
            }
            if (tokens[pos] == TOK_COMMA) pos++;
          }
          if (m_cmdBreak) m_cmdBreak(lines, count, tok == TOK_BREAK);
          return resp;
        }

        case TOK_RES:
        case TOK_NUM:
        {
          pos++;
          int args[2] = {100, 10};
          int n = 0;
          while (n < 2 && tokens[pos] != TOK_EOL &&
                 tokens[pos] != TOK_COLON)
          {
            if (tokens[pos] == TOK_LINENUM ||
                tokens[pos] == TOK_UNQUOTED_STR)
            {
              args[n++] = (int)readLineNum(tokens, &pos);
            }
            else
            {
              pos++;
            }
            if (tokens[pos] == TOK_COMMA) pos++;
          }
          if (tok == TOK_RES)
          {
            if (m_cmdRes) m_cmdRes(args[0], args[1]);
          }
          else
          {
            if (m_cmdNum) m_cmdNum(args[0], args[1]);
          }
          return resp;
        }

        case TOK_BANG:
        case TOK_REM:
        case TOK_IMAGE:
          // Skip rest of line. IMAGE stores a format string for
          // PRINT USING <lineNum>: ... — the format is read by walking
          // the program for the matching line, not by executing here.
          return resp;  // NEXT_LINE

        case TOK_END:
          resp.result = TP_FINISHED;
          return resp;

        case TOK_STOP:
          resp.result = TP_STOPPED;
          return resp;

        case TOK_ELSE:
          // Reaching ELSE on the main dispatcher means a successful
          // THEN branch just finished — the ELSE clause must be
          // discarded. Without this, `IF c THEN x ELSE y` would run
          // BOTH x and y when c is true.
          while (pos < length && tokens[pos] != TOK_EOL)
          {
            uint8_t t = tokens[pos];
            if (t == TOK_QUOTED_STR || t == TOK_UNQUOTED_STR ||
                t == TOK_STRING_LIT)
            {
              pos++;
              int slen = tokens[pos++];
              pos += slen;
            }
            else
            {
              pos++;
            }
          }
          break;

        case TOK_OPEN:
          pos++;
          resp = execOpen(tokens, &pos);
          if (resp.result == TP_ERROR) return resp;
          break;

        case TOK_CLOSE:
          pos++;
          resp = execClose(tokens, &pos);
          if (resp.result == TP_ERROR) return resp;
          break;

        default:
          // Unknown token — skip past it. String/number literals carry
          // a length byte + N content bytes; a bare `pos++` would walk
          // those bytes back into the dispatcher and re-interpret them
          // as opcodes (e.g. a 7-char string body's length byte = 0x07
          // = TOK_SAVE → spurious SAVE invocation).
          if (tok == TOK_QUOTED_STR || tok == TOK_UNQUOTED_STR ||
              tok == TOK_STRING_LIT)
          {
            pos++;                    // past the type tag
            int slen = tokens[pos++]; // past the length byte
            pos += slen;              // past the content
          }
          else
          {
            pos++;
          }
          break;
      }
    }

    // Catch diagnostics raised by the final statement on the line.
    if (m_expr.hasDiag())
    {
      resp.result = m_expr.hasError() ? TP_ERROR : TP_WARNING;
      snprintf(resp.errorMsg, sizeof(resp.errorMsg), "%s",
               m_expr.errorMsg());
      m_expr.clearError();
    }

    return resp;  // NEXT_LINE
  }

  VarTable* vars() { return &m_vars; }

  // Called by EM after collecting INPUT from user
  void provideInputValue(const char* input)
  {
    provideInput(input);
  }

private:
  VarTable m_vars;
  ExprParser m_expr;

  ForFrame m_forStack[MAX_FOR_DEPTH];
  int m_forDepth = 0;

  // Pending INPUT state
  const uint8_t* m_inputTokens = NULL;
  int m_inputVarStart = 0;
  bool m_linputMode = false;   // LINPUT: whole line to first var, no comma split

  GosubFrame m_gosubStack[MAX_GOSUB_DEPTH];
  int m_gosubDepth = 0;

  int m_optionBase = 0;  // OPTION BASE 0 or 1 (affects DIM)

  // ON BREAK / ON ERROR / ON WARNING handler state
  OnBreakMode   m_onBreakMode   = OB_STOP;
  OnErrorMode   m_onErrorMode   = OE_STOP;
  uint16_t      m_onErrorLine   = 0;
  OnWarningMode m_onWarningMode = OW_PRINT;
  uint16_t      m_lastErrorLine = 0;   // for CALL ERR

  // Pending I/O error: set by void-returning file I/O sites (raiseIOError),
  // consumed and cleared by the statement-dispatch loop. -1 = no error.
  int m_pendingIOError = -1;
  // Last I/O error code surfaced — survives the dispatch consume so CALL
  // ERR(...) can report it after the ON ERROR handler runs.
  int m_lastIOError = 0;

  // CALL SOUND schedule — millis() past which the current (stub) sound
  // is finished. Positive-duration CALL SOUND waits for this before
  // scheduling the new one.
  unsigned long m_soundEndTime = 0;

  // Per-statement throttle in microseconds, set via CALL SPEED.
  // Applied at every statement separator (`::` / `:`) and once at the
  // end of each line, so multi-statement lines feel like a TI's pace.
  unsigned long m_throttleUs = 0;

public:
  void setThrottleUs(unsigned long us) { m_throttleUs = us; }
  unsigned long throttleUs() const { return m_throttleUs; }
private:

  PrintCharFn m_printChar;
  PrintStringFn m_printString;
  ClearScreenFn m_clearScreen;

  CmdNewFn m_cmdNew = NULL;
  CmdListFn m_cmdList = NULL;
  CmdRunFn m_cmdRun = NULL;
  CmdSaveFn m_cmdSave = NULL;
  CmdOldFn m_cmdOld = NULL;
  CmdByeFn m_cmdBye = NULL;
  CmdDirFn m_cmdDir = NULL;
  CmdSizeFn m_cmdSize = NULL;
  CmdTraceFn m_cmdTrace = NULL;
  CmdBreakFn m_cmdBreak = NULL;
  CmdResFn m_cmdRes = NULL;
  CmdNumFn m_cmdNum = NULL;
  CmdMergeFn m_cmdMerge = NULL;
  CmdDeleteFn m_cmdDelete = NULL;
  CmdContinueFn m_cmdContinue = NULL;

  // Graphics / sprite / input platform hooks default to the weak no-op
  // tiXxx symbols defined in ti_platform.cpp. A project provides strong
  // overrides with the same name to plug in real hardware behavior.
  // setSpriteCallbacks / setReadKey / etc. still work as before for
  // backward compatibility, but new code should use the strong-tiXxx
  // pattern instead.
  SpriteDrawFn  m_spriteDraw  = tiSpriteDraw;
  SpriteEraseFn m_spriteErase = tiSpriteErase;
  SetThrottleFn m_setThrottle = NULL;
  ImageLookupFn m_imageLookup = NULL;

  FileOpenFn     m_fileOpen     = NULL;
  FileCloseFn    m_fileClose    = NULL;
  FilePrintFn    m_filePrint    = NULL;
  FileReadLineFn m_fileReadLine = NULL;
  FileEofFn      m_fileEof      = NULL;
  FileSeekRecFn  m_fileSeekRec  = NULL;
  FileRewindFn   m_fileRewind   = NULL;

  SetCharFn        m_setChar        = tiSetChar;
  GetCharFn        m_getChar        = tiGetChar;
  SetScreenColorFn m_setScreenColor = tiSetScreenColor;
  SetCharColorFn   m_setCharColor   = tiSetCharColor;
  SetCharPatternFn m_setCharPattern = tiSetCharPattern;
  GetCharPatternFn m_getCharPattern = tiGetCharPattern;
  ResetCharsetFn   m_resetCharset   = tiResetCharset;
  SetCharsetModeFn m_setCharsetMode = tiSetCharsetMode;
  ReadKeyFn        m_readKey        = tiReadKey;
  ReadJoystickFn   m_readJoystick   = tiReadJoystick;
  MoveCursorFn     m_moveCursor     = tiMoveCursor;
  NextDataFn m_nextData = NULL;
  ResetDataFn m_resetData = NULL;

  // Extract filename from tokens (string literal or identifier)
  void extractFilename(const uint8_t* tokens, int* pos, char* buf, int bufSize)
  {
    if (tokens[*pos] == TOK_QUOTED_STR || tokens[*pos] == TOK_UNQUOTED_STR)
    {
      (*pos)++;
      int slen = tokens[*pos];
      (*pos)++;
      int copyLen = (slen < bufSize - 1) ? slen : bufSize - 1;
      memcpy(buf, &tokens[*pos], copyLen);
      buf[copyLen] = '\0';
      *pos += slen;
    }
    else if (isIdentStart(tokens[*pos]))
    {
      bool isStr;
      parseIdent(tokens, pos, buf, bufSize, &isStr);
    }
  }

  // Current cursor column (for PRINT comma tab stops)
  int m_printCol = 0;

  // Check if tokens at pos start a string expression (string var or func)
  bool isStringExprStart(const uint8_t* tokens, int pos)
  {
    if (!isIdentStart(tokens[pos])) return false;
    char name[16];
    peekIdent(tokens, pos, name, sizeof(name));
    int nameLen = strlen(name);
    // Ends with $ → string variable or string function
    if (nameLen > 0 && name[nameLen - 1] == '$') return true;
    return false;
  }

  // --- PRINT USING / IMAGE format engine ---
  //
  // Walks an IMAGE format string, substituting numeric edit fields
  // ('#' digit slots, optional '.' decimal point, optional leading
  // '-' sign space, '^^^^' scientific notation) with values from
  // the supplied list. Anything else in the IMAGE passes through
  // literally.
  //
  // Returns number of bytes written to `out`.
  int formatUsing(const char* image, int imageLen,
                  const uint8_t* tokens, int* pos,
                  char* out, int outSize)
  {
    int op = 0;
    int ip = 0;
    while (ip < imageLen)
    {
      char ch = image[ip];

      // Detect an edit field — a run of '#', '.', '-', '+', '^'
      if (ch == '#' || ch == '.' || ch == '-' ||
          ch == '+' || ch == '^')
      {
        int fieldStart = ip;
        while (ip < imageLen)
        {
          char fc = image[ip];
          if (fc != '#' && fc != '.' && fc != '-' &&
              fc != '+' && fc != '^') break;
          ip++;
        }
        const char* field = &image[fieldStart];
        int fieldLen = ip - fieldStart;

        // Read next value from token stream
        if (tokens[*pos] == TOK_COMMA || tokens[*pos] == TOK_SEMICOLON)
        {
          (*pos)++;
        }
        float val = m_expr.evalNumeric(tokens, pos);

        // Count digit slots
        int intSlots = 0, fracSlots = 0;
        bool sawDot = false;
        bool hasSign = false;
        bool hasExp  = false;
        int  expCarets = 0;
        for (int i = 0; i < fieldLen; i++)
        {
          char fc = field[i];
          if (fc == '#')      { if (sawDot) fracSlots++; else intSlots++; }
          else if (fc == '.') { sawDot = true; }
          else if (fc == '-' || fc == '+') { hasSign = true; }
          else if (fc == '^') { hasExp = true; expCarets++; }
        }
        if (!hasExp) expCarets = 0;

        // Format the value
        bool negative = (val < 0);
        float absVal = negative ? -val : val;
        char numbuf[32];

        if (hasExp && expCarets >= 2)
        {
          // Scientific notation: e.g. "##.##^^^^" → 1.23E+04
          int decimals = fracSlots;
          int sigDigits = intSlots + fracSlots;
          if (sigDigits < 1) sigDigits = 1;
          // Use printf %e style, then trim mantissa digits
          char tmp[32];
          snprintf(tmp, sizeof(tmp), "%.*e", decimals, absVal);
          snprintf(numbuf, sizeof(numbuf), "%s%s",
                   negative ? "-" : (hasSign ? " " : ""), tmp);
        }
        else
        {
          // Fixed-point
          char tmp[32];
          if (sawDot)
          {
            snprintf(tmp, sizeof(tmp), "%.*f", fracSlots, absVal);
          }
          else
          {
            snprintf(tmp, sizeof(tmp), "%.0f", absVal);
          }
          int tmpLen = strlen(tmp);
          int needed = tmpLen + (negative ? 1 : (hasSign ? 1 : 0));
          int width  = intSlots + (sawDot ? 1 : 0) + fracSlots;

          // Find length of integer part of tmp (before any '.')
          int tmpIntLen = 0;
          for (int i = 0; i < tmpLen; i++)
          {
            if (tmp[i] == '.') break;
            tmpIntLen++;
          }
          int intNeeded = tmpIntLen + (negative || hasSign ? 1 : 0);

          if (intNeeded > intSlots)
          {
            // Overflow — TI prints '#'s in every slot
            for (int i = 0; i < width; i++) numbuf[i] = '#';
            numbuf[width] = '\0';
          }
          else
          {
            int p = 0;
            int padCount = intSlots - intNeeded;
            for (int i = 0; i < padCount; i++) numbuf[p++] = ' ';
            if (negative)        numbuf[p++] = '-';
            else if (hasSign)    numbuf[p++] = ' ';
            memcpy(&numbuf[p], tmp, tmpLen);
            p += tmpLen;
            numbuf[p] = '\0';
          }
        }

        int nbLen = strlen(numbuf);
        for (int i = 0; i < nbLen && op < outSize - 1; i++)
        {
          out[op++] = numbuf[i];
        }
      }
      else
      {
        // Literal character
        if (op < outSize - 1) out[op++] = ch;
        ip++;
      }
    }
    out[op] = '\0';
    return op;
  }

  // --- Statement handlers ---

  // Build a formatted output line the same way PRINT does (`;`, `,`,
  // expressions) but into a buffer, then send it to file unit. Each
  // PRINT # statement writes exactly one line.
  void execPrintFile(const uint8_t* tokens, int* pos, int unit)
  {
    char out[MAX_STR_LEN];
    int olen = 0;
    int col = 0;

    while (tokens[*pos] != TOK_EOL && tokens[*pos] != TOK_COLON &&
           tokens[*pos] != TOK_BANG)
    {
      uint8_t tok = tokens[*pos];
      if (tok == TOK_SEMICOLON) { (*pos)++; continue; }
      if (tok == TOK_COMMA)
      {
        (*pos)++;
        int nextTab = ((col / 14) + 1) * 14;
        while (col < nextTab && olen < (int)sizeof(out) - 1)
        {
          out[olen++] = ' ';
          col++;
        }
        continue;
      }
      if (tok == TOK_QUOTED_STR || isStringExprStart(tokens, *pos))
      {
        char strBuf[MAX_STR_LEN];
        m_expr.evalString(tokens, pos, strBuf, sizeof(strBuf));
        int sl = strlen(strBuf);
        if (olen + sl < (int)sizeof(out) - 1)
        {
          memcpy(&out[olen], strBuf, sl);
          olen += sl;
          col  += sl;
        }
      }
      else if (tok == TOK_UNQUOTED_STR || isIdentStart(tok) ||
               tok == TOK_LPAREN || tok == TOK_MINUS || tok == TOK_NOT)
      {
        float val = m_expr.evalNumeric(tokens, pos);
        char buf[20];
        if (val >= 0) snprintf(buf, sizeof(buf), " %g ", val);
        else          snprintf(buf, sizeof(buf),  "%g ", val);
        int sl = strlen(buf);
        if (olen + sl < (int)sizeof(out) - 1)
        {
          memcpy(&out[olen], buf, sl);
          olen += sl;
          col  += sl;
        }
      }
      else
      {
        (*pos)++;
      }
    }
    out[olen] = '\0';

    if (m_filePrint)
    {
      int rc = m_filePrint(unit, out);
      if (rc != 0) raiseIOError(rc);
    }
  }

  void execPrint(const uint8_t* tokens, int* pos, bool addNewline = true)
  {
    // PRINT #n [, REC k] : ... — file output. Build one line's worth of
    // text into a buffer and hand it to the file I/O layer.
    if (tokens[*pos] == TOK_HASH)
    {
      (*pos)++;
      int unit = (int)m_expr.evalNumeric(tokens, pos);
      if (tokens[*pos] == TOK_COMMA && tokens[*pos + 1] == TOK_REC)
      {
        (*pos)++;   // comma
        (*pos)++;   // REC
        if (tokens[*pos] == TOK_EQUAL) (*pos)++;
        long rec = (long)m_expr.evalNumeric(tokens, pos);
        if (m_fileSeekRec) m_fileSeekRec(unit, rec);
      }
      if (tokens[*pos] == TOK_COLON) (*pos)++;
      execPrintFile(tokens, pos, unit);
      return;
    }

    // PRINT USING <imageRef>: list...   — TI-style formatted output.
    // imageRef is either a line-number-pointing-at-IMAGE or a string
    // literal/expression containing the format directly.
    if (tokens[*pos] == TOK_USING)
    {
      (*pos)++;
      char imageBuf[160];
      int  imageLen = 0;
      // Variant 1: line number reference
      if (tokens[*pos] == TOK_LINENUM ||
          tokens[*pos] == TOK_UNQUOTED_STR)
      {
        uint16_t ln = readLineNum(tokens, pos);
        if (m_imageLookup)
        {
          const char* p; int n;
          if (m_imageLookup(ln, &p, &n))
          {
            int copy = (n < (int)sizeof(imageBuf) - 1) ? n :
                       (int)sizeof(imageBuf) - 1;
            memcpy(imageBuf, p, copy);
            imageLen = copy;
          }
        }
      }
      else
      {
        // Variant 2: string expression
        m_expr.evalString(tokens, pos, imageBuf, sizeof(imageBuf));
        imageLen = strlen(imageBuf);
      }
      // Consume separator before the value list
      if (tokens[*pos] == TOK_COLON || tokens[*pos] == TOK_SEMICOLON)
      {
        (*pos)++;
      }
      // Trim leading whitespace from imageBuf since IMAGE captures the
      // literal source text after the keyword (which usually has a
      // single leading space).
      int trim = 0;
      while (trim < imageLen && imageBuf[trim] == ' ') trim++;

      char outBuf[200];
      int outLen = formatUsing(&imageBuf[trim], imageLen - trim,
                               tokens, pos, outBuf, sizeof(outBuf));
      if (m_printString) m_printString(outBuf);
      m_printCol += outLen;
      // PRINT USING ends with newline unless caller suppresses
      if (addNewline && m_printChar) m_printChar('\n');
      m_printCol = 0;
      return;
    }

    bool needNewline = true;

    while (tokens[*pos] != TOK_EOL && tokens[*pos] != TOK_COLON &&
           tokens[*pos] != TOK_BANG)
    {
      uint8_t tok = tokens[*pos];

      // TAB(n) — move cursor to column n (1-based)
      if (isIdentStart(tok))
      {
        char fname[8];
        int peekLen = peekIdent(tokens, *pos, fname, sizeof(fname));
        if (strcasecmp(fname, "TAB") == 0 &&
            tokens[*pos + peekLen] == TOK_LPAREN)
        {
          *pos += peekLen + 1;
          int tabCol = (int)m_expr.evalNumeric(tokens, pos) - 1;
          if (tokens[*pos] == TOK_RPAREN) (*pos)++;

          while (m_printCol < tabCol)
          {
            if (m_printChar) m_printChar(' ');
            m_printCol++;
          }
          needNewline = false;
          continue;
        }
      }

      if (tok == TOK_SEMICOLON)
      {
        (*pos)++;
        needNewline = false;
      }
      else if (tok == TOK_COMMA)
      {
        (*pos)++;
        int nextTab = ((m_printCol / 14) + 1) * 14;
        while (m_printCol < nextTab)
        {
          if (m_printChar)
          {
            m_printChar(' ');
          }
          m_printCol++;
        }
        needNewline = false;
      }
      else if (tok == TOK_QUOTED_STR || isStringExprStart(tokens, *pos))
      {
        // String literal or identifier that's a string var/function
        char strBuf[MAX_STR_LEN];
        m_expr.evalString(tokens, pos, strBuf, sizeof(strBuf));
        if (m_printString)
        {
          m_printString(strBuf);
        }
        m_printCol += strlen(strBuf);
        needNewline = true;
      }
      else if (tok == TOK_UNQUOTED_STR || isIdentStart(tok) ||
               tok == TOK_LPAREN || tok == TOK_MINUS ||
               tok == TOK_NOT)
      {
        float val = m_expr.evalNumeric(tokens, pos);
        char buf[20];
        // TI BASIC: leading space for positive (sign placeholder) + trailing space
        if (val >= 0)
        {
          snprintf(buf, sizeof(buf), " %g ", val);
        }
        else
        {
          snprintf(buf, sizeof(buf), "%g ", val);
        }
        if (m_printString)
        {
          m_printString(buf);
        }
        m_printCol += strlen(buf);
        needNewline = true;
      }
      else
      {
        (*pos)++;
        needNewline = true;
      }
    }

    if (needNewline && addNewline)
    {
      if (m_printChar)
      {
        m_printChar('\n');
      }
      m_printCol = 0;
    }
  }

  // --- String expression evaluator ---

  // String expression evaluation moved to ExprParser (m_expr.evalString)

  // Returns true on success, false on syntax error (no '=' after identifier).
  // DEF FNname(param) = expression
  //
  // Parses the signature and body and stores the function in the variable
  // table. The body is saved as a slice of the original token stream —
  // on call, ExprParser binds the parameter to its variable and evaluates
  // the saved body via evalNumeric / evalString.
  void execDef(const uint8_t* tokens, int* pos, int length)
  {
    if (!isIdentStart(tokens[*pos])) return;
    char fname[16];
    bool fnIsStr;
    int fnLen = parseIdent(tokens, pos, fname, sizeof(fname), &fnIsStr);

    if (tokens[*pos] != TOK_LPAREN) return;
    (*pos)++;

    if (!isIdentStart(tokens[*pos])) return;
    char pname[16];
    bool pIsStr;
    int pLen = parseIdent(tokens, pos, pname, sizeof(pname), &pIsStr);

    if (tokens[*pos] != TOK_RPAREN) return;
    (*pos)++;

    if (tokens[*pos] != TOK_EQUAL) return;
    (*pos)++;

    // Capture tokens from here to end of statement / line
    int bodyStart = *pos;
    while (*pos < length && tokens[*pos] != TOK_EOL &&
           tokens[*pos] != TOK_COLON)
    {
      (*pos)++;
    }
    int bodyLen = *pos - bodyStart;

    m_vars.defineFn(fname, fnLen, fnIsStr,
                    pname, pLen, pIsStr,
                    &tokens[bodyStart], bodyLen);
  }

  bool execAssignment(const uint8_t* tokens, int* pos)
  {
    char vname[MAX_VAR_NAME];
    bool isStr;
    int vlen = parseIdent(tokens, pos, vname, sizeof(vname), &isStr);

    // Array assignment: A(i)=... or A(i,j)=...
    if (tokens[*pos] == TOK_LPAREN)
    {
      (*pos)++;
      int indices[MAX_ARRAY_DIMS];
      int nIdx = 0;
      while (nIdx < MAX_ARRAY_DIMS && tokens[*pos] != TOK_RPAREN &&
             tokens[*pos] != TOK_EOL)
      {
        indices[nIdx++] = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;

      if (tokens[*pos] != TOK_EQUAL)
      {
        return false;
      }
      (*pos)++;
      if (isStr)
      {
        char buf[MAX_STR_LEN];
        m_expr.evalString(tokens, pos, buf, sizeof(buf));
        m_vars.setArrayStr(vname, vlen, indices, nIdx, buf);
      }
      else
      {
        float val = m_expr.evalNumeric(tokens, pos);
        m_vars.setArrayNum(vname, vlen, indices, nIdx, val);
      }
      return true;
    }

    // Scalar assignment
    if (tokens[*pos] != TOK_EQUAL)
    {
      return false;
    }
    (*pos)++;
    if (isStr)
    {
      char buf[MAX_STR_LEN];
      m_expr.evalString(tokens, pos, buf, sizeof(buf));
      m_vars.setStr(vname, vlen, buf);
    }
    else
    {
      float val = m_expr.evalNumeric(tokens, pos);
      m_vars.setNum(vname, vlen, val);
    }
    return true;
  }

  // READ var1,var2,... — pull values from DATA statements
  void execRead(const uint8_t* tokens, int* pos)
  {
    while (tokens[*pos] != TOK_EOL && tokens[*pos] != TOK_COLON &&
           tokens[*pos] != TOK_BANG)
    {
      if (!isIdentStart(tokens[*pos]))
      {
        (*pos)++;
        continue;
      }

      char vname[MAX_VAR_NAME];
      bool isStr;
      int vlen = parseIdent(tokens, pos, vname, sizeof(vname), &isStr);

      // Array target READ A(i)
      int indices[MAX_ARRAY_DIMS];
      int nIdx = 0;
      bool isArray = false;
      if (tokens[*pos] == TOK_LPAREN)
      {
        isArray = true;
        (*pos)++;
        while (nIdx < MAX_ARRAY_DIMS && tokens[*pos] != TOK_RPAREN &&
               tokens[*pos] != TOK_EOL)
        {
          indices[nIdx++] = (int)m_expr.evalNumeric(tokens, pos);
          if (tokens[*pos] == TOK_COMMA) (*pos)++;
        }
        if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      }

      // Get next data value
      char dataBuf[MAX_STR_LEN];
      if (!m_nextData || !m_nextData(dataBuf, sizeof(dataBuf)))
      {
        return;  // out of data
      }

      if (isStr)
      {
        if (isArray)
          m_vars.setArrayStr(vname, vlen, indices, nIdx, dataBuf);
        else
          m_vars.setStr(vname, vlen, dataBuf);
      }
      else
      {
        float val = strtof(dataBuf, NULL);
        if (isArray)
          m_vars.setArrayNum(vname, vlen, indices, nIdx, val);
        else
          m_vars.setNum(vname, vlen, val);
      }

      if (tokens[*pos] == TOK_COMMA) (*pos)++;
    }
  }

  // DISPLAY [AT(row,col)[:]] item [;item...]
  // Extended BASIC: like PRINT but with optional position.
  void execDisplay(const uint8_t* tokens, int* pos)
  {
    // Check for AT(row, col)
    if (isIdentStart(tokens[*pos]))
    {
      char fname[8];
      int peekLen = peekIdent(tokens, *pos, fname, sizeof(fname));
      if (strcasecmp(fname, "AT") == 0 &&
          tokens[*pos + peekLen] == TOK_LPAREN)
      {
        *pos += peekLen + 1;   // past AT and (
        int row = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        int col = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_RPAREN) (*pos)++;

        // Position cursor (1-based → 0-based)
        if (m_moveCursor) m_moveCursor(row - 1, col - 1);

        // Skip separator (:, ;) before the actual output
        if (tokens[*pos] == TOK_COLON || tokens[*pos] == TOK_SEMICOLON)
        {
          (*pos)++;
        }
      }
    }

    // Print the items — DISPLAY AT does NOT add a newline (no scroll)
    execPrint(tokens, pos, false);
  }

  // ACCEPT [AT(row,col)[:]] var
  TPResponse execAccept(const uint8_t* tokens, int* pos)
  {
    bool hadAt = false;

    // Check for AT(row, col)
    if (isIdentStart(tokens[*pos]))
    {
      char fname[8];
      int peekLen = peekIdent(tokens, *pos, fname, sizeof(fname));
      if (strcasecmp(fname, "AT") == 0 &&
          tokens[*pos + peekLen] == TOK_LPAREN)
      {
        *pos += peekLen + 1;
        int row = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        int col = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_RPAREN) (*pos)++;

        if (m_moveCursor) m_moveCursor(row - 1, col - 1);
        hadAt = true;

        if (tokens[*pos] == TOK_COLON || tokens[*pos] == TOK_SEMICOLON)
        {
          (*pos)++;
        }
      }
    }

    // Rest works like INPUT (no prompt)
    TPResponse resp;
    resp.result = TP_NEED_INPUT;
    resp.errorMsg[0] = '\0';
    resp.prompt[0] = '\0';          // no prompt for ACCEPT
    resp.cursorPrePositioned = hadAt;

    m_inputTokens = tokens;
    m_inputVarStart = *pos;
    while (tokens[*pos] != TOK_EOL && tokens[*pos] != TOK_COLON &&
           tokens[*pos] != TOK_BANG)
    {
      (*pos)++;
    }
    return resp;
  }

  // ON expr GOTO line1,line2,line3  or  ON expr GOSUB line1,line2,line3
  TPResponse execOn(const uint8_t* tokens, int* pos, uint16_t currentLine)
  {
    TPResponse resp;
    resp.result = TP_NEXT_LINE;
    resp.errorMsg[0] = '\0';

    // ON BREAK STOP | NEXT
    if (tokens[*pos] == TOK_BREAK)
    {
      (*pos)++;
      if (tokens[*pos] == TOK_STOP)      { m_onBreakMode = OB_STOP; (*pos)++; }
      else if (tokens[*pos] == TOK_NEXT) { m_onBreakMode = OB_NEXT; (*pos)++; }
      return resp;
    }

    // ON ERROR <line> | STOP | NEXT
    if (tokens[*pos] == TOK_ERROR)
    {
      (*pos)++;
      if (tokens[*pos] == TOK_STOP)      { m_onErrorMode = OE_STOP; (*pos)++; }
      else if (tokens[*pos] == TOK_NEXT) { m_onErrorMode = OE_NEXT; (*pos)++; }
      else if (tokens[*pos] == TOK_LINENUM ||
               tokens[*pos] == TOK_UNQUOTED_STR)
      {
        m_onErrorLine = readLineNum(tokens, pos);
        m_onErrorMode = OE_GOTO;
      }
      return resp;
    }

    // ON WARNING STOP | PRINT | NEXT
    if (tokens[*pos] == TOK_WARNING)
    {
      (*pos)++;
      if (tokens[*pos] == TOK_STOP)       { m_onWarningMode = OW_STOP;  (*pos)++; }
      else if (tokens[*pos] == TOK_PRINT) { m_onWarningMode = OW_PRINT; (*pos)++; }
      else if (tokens[*pos] == TOK_NEXT)  { m_onWarningMode = OW_NEXT;  (*pos)++; }
      return resp;
    }

    int index = (int)m_expr.evalNumeric(tokens, pos);

    bool isGosub = false;
    if (tokens[*pos] == TOK_GOSUB)
    {
      isGosub = true;
    }
    (*pos)++;  // past GOTO/GOSUB

    // Walk the line number list. Use the index-th one (1-based).
    int count = 0;
    uint16_t targetLine = 0;
    bool found = false;

    while (tokens[*pos] != TOK_EOL && tokens[*pos] != TOK_COLON &&
           tokens[*pos] != TOK_BANG)
    {
      count++;
      if (tokens[*pos] == TOK_LINENUM ||
          tokens[*pos] == TOK_UNQUOTED_STR)
      {
        uint16_t val = readLineNum(tokens, pos);
        if (count == index)
        {
          targetLine = val;
          found = true;
          // Don't break — consume remaining tokens to stay aligned
        }
      }
      else
      {
        (*pos)++;
      }
      if (tokens[*pos] == TOK_COMMA) (*pos)++;
    }

    if (!found)
    {
      // Index out of range — fall through, continue with next statement
      return resp;
    }

    if (isGosub)
    {
      if (m_gosubDepth >= MAX_GOSUB_DEPTH)
      {
        resp.result = TP_ERROR;
        snprintf(resp.errorMsg, sizeof(resp.errorMsg),
                 "STACK OVERFLOW");
        return resp;
      }
      m_gosubStack[m_gosubDepth++].returnLineNum = currentLine;
    }

    resp.result = TP_GOTO_LINE;
    resp.lineNum = targetLine;
    return resp;
  }

  // DIM A(10) or DIM A(5,10) or DIM A(3,5,2), and string versions
  void execDim(const uint8_t* tokens, int* pos)
  {
    while (tokens[*pos] != TOK_EOL && tokens[*pos] != TOK_COLON &&
           tokens[*pos] != TOK_BANG)
    {
      if (!isIdentStart(tokens[*pos]))
      {
        (*pos)++;
        continue;
      }

      char vname[MAX_VAR_NAME];
      bool isStr;
      int vlen = parseIdent(tokens, pos, vname, sizeof(vname), &isStr);

      if (tokens[*pos] != TOK_LPAREN) return;
      (*pos)++;

      int dims[MAX_ARRAY_DIMS];
      int nDims = 0;
      while (nDims < MAX_ARRAY_DIMS && tokens[*pos] != TOK_RPAREN &&
             tokens[*pos] != TOK_EOL)
      {
        int n = (int)m_expr.evalNumeric(tokens, pos);
        // DIM A(10) creates indices base..10, inclusive.
        // Storage size: n - base + 1 elements.
        // We still allocate n+1 slots for simplicity (index 0 unused if base=1).
        dims[nDims++] = n + 1;
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;

      m_vars.dimArray(vname, vlen, isStr, nDims, dims);

      // Handle multiple arrays in one DIM: DIM A(10),B(20)
      if (tokens[*pos] == TOK_COMMA) (*pos)++;
    }
  }

  // Read a line-number reference — TI-style TOK_LINENUM (0xC9 + 2 bytes)
  // preferred, but falls back to TOK_UNQUOTED_STR for backwards compat
  // and for tokenized output that somehow skipped the line-ref flag.
  uint16_t readLineNum(const uint8_t* tokens, int* pos)
  {
    if (tokens[*pos] == TOK_LINENUM)
    {
      (*pos)++;
      uint16_t ln = ((uint16_t)tokens[*pos] << 8) | tokens[*pos + 1];
      *pos += 2;
      return ln;
    }
    if (tokens[*pos] == TOK_UNQUOTED_STR)
    {
      return (uint16_t)readNumber(tokens, pos);
    }
    return 0;
  }

  // Read a number from token stream (TOK_UNQUOTED_STR + length + ASCII)
  // Advances pos past the number. Returns 0 if not a number token.
  float readNumber(const uint8_t* tokens, int* pos)
  {
    if (tokens[*pos] != TOK_UNQUOTED_STR)
    {
      return 0;
    }
    (*pos)++;
    int slen = tokens[*pos];
    (*pos)++;
    char buf[32];
    int copyLen = (slen < 31) ? slen : 31;
    memcpy(buf, &tokens[*pos], copyLen);
    buf[copyLen] = '\0';
    *pos += slen;
    return strtof(buf, NULL);
  }

  // OPEN #n:"DEVICE.NAME"[,mode-keywords...]
  // Modes recognized: INPUT, OUTPUT, UPDATE, APPEND, SEQUENTIAL, DISPLAY,
  //                   VARIABLE <n>, INTERNAL, RELATIVE, FIXED, PERMANENT.
  // Only SEQUENTIAL + DISPLAY + INPUT/OUTPUT/APPEND/UPDATE are actually
  // supported; INTERNAL / RELATIVE / FIXED accept and ignore.
  TPResponse execOpen(const uint8_t* tokens, int* pos)
  {
    TPResponse resp;
    resp.result = TP_NEXT_TOKEN;
    resp.errorMsg[0] = '\0';

    if (tokens[*pos] == TOK_HASH) (*pos)++;
    int unit = (int)m_expr.evalNumeric(tokens, pos);
    // TI Extended BASIC requires colon between unit and spec:
    //   OPEN #n:"DEVICE.NAME"[,attrs...]
    // Reject anything else as a syntax error rather than silently
    // passing an empty spec into the file layer and surfacing it as
    // a confusing I/O ERROR 01 at runtime.
    if (tokens[*pos] != TOK_COLON)
    {
      resp.result = TP_ERROR;
      snprintf(resp.errorMsg, sizeof(resp.errorMsg), "INCORRECT STATEMENT");
      return resp;
    }
    (*pos)++;

    char spec[64] = "";
    m_expr.evalString(tokens, pos, spec, sizeof(spec));

    int mode = 0;   // TI default = INPUT
    int flags = 0;
    int recLen = 0;
    while (tokens[*pos] == TOK_COMMA)
    {
      (*pos)++;
      uint8_t k = tokens[*pos];
      if (k == TOK_INPUT)    { mode = 0; (*pos)++; }
      else if (k == TOK_OUTPUT)  { mode = 1; (*pos)++; }
      else if (k == TOK_APPEND)  { mode = 2; (*pos)++; }
      else if (k == TOK_UPDATE)  { mode = 3; (*pos)++; }
      else if (k == TOK_INTERNAL) { flags |= FF_INTERNAL; (*pos)++; }
      else if (k == TOK_RELATIVE) { flags |= FF_RELATIVE; (*pos)++;
                                    // Optional max-record-count (ignored)
                                    if (tokens[*pos] == TOK_UNQUOTED_STR)
                                    {
                                      readNumber(tokens, pos);
                                    }
                                  }
      else if (k == TOK_FIXED)
      {
        flags |= FF_FIXED;
        (*pos)++;
        // FIXED is normally followed by a record length (default 80)
        if (tokens[*pos] == TOK_UNQUOTED_STR)
        {
          recLen = (int)readNumber(tokens, pos);
        }
        else
        {
          recLen = 80;
        }
      }
      else if (k == TOK_VARIABLE_KW)
      {
        (*pos)++;
        if (tokens[*pos] == TOK_UNQUOTED_STR) readNumber(tokens, pos);
      }
      else if (k == TOK_SEQUENTIAL || k == TOK_PERMANENT ||
               k == TOK_DISPLAY)
      {
        (*pos)++;
      }
      else
      {
        (*pos)++;
      }
    }

    // FIXED implies a default record length if user omitted one.
    if ((flags & FF_FIXED) && recLen == 0) recLen = 80;

    if (!m_fileOpen)
    {
      resp.result = TP_ERROR;
      snprintf(resp.errorMsg, sizeof(resp.errorMsg), "I/O ERROR 02");
      m_lastIOError = 2;
      return resp;
    }
    int err = m_fileOpen(unit, spec, mode, flags, recLen);
    if (err != 0)
    {
      resp.result = TP_ERROR;
      snprintf(resp.errorMsg, sizeof(resp.errorMsg), "I/O ERROR %02d", err);
      m_lastIOError = err;
      return resp;
    }
    return resp;
  }

  // CLOSE #n [, #m ...]
  TPResponse execClose(const uint8_t* tokens, int* pos)
  {
    TPResponse resp;
    resp.result = TP_NEXT_TOKEN;
    resp.errorMsg[0] = '\0';

    while (tokens[*pos] == TOK_HASH || tokens[*pos] == TOK_UNQUOTED_STR ||
           tokens[*pos] == TOK_COMMA)
    {
      if (tokens[*pos] == TOK_COMMA) { (*pos)++; continue; }
      if (tokens[*pos] == TOK_HASH)  { (*pos)++; }
      int unit = (int)m_expr.evalNumeric(tokens, pos);
      if (m_fileClose) m_fileClose(unit);
    }
    return resp;
  }

  TPResponse execGoto(const uint8_t* tokens, int* pos)
  {
    TPResponse resp;
    resp.errorMsg[0] = '\0';

    if (tokens[*pos] == TOK_LINENUM ||
        tokens[*pos] == TOK_UNQUOTED_STR)
    {
      resp.result = TP_GOTO_LINE;
      resp.lineNum = readLineNum(tokens, pos);
    }
    else
    {
      resp.result = TP_ERROR;
      snprintf(resp.errorMsg, sizeof(resp.errorMsg), "SYNTAX ERROR");
    }
    return resp;
  }

  TPResponse execGosub(const uint8_t* tokens, int* pos, uint16_t currentLine)
  {
    TPResponse resp;
    resp.errorMsg[0] = '\0';

    if (m_gosubDepth >= MAX_GOSUB_DEPTH)
    {
      resp.result = TP_ERROR;
      snprintf(resp.errorMsg, sizeof(resp.errorMsg), "STACK OVERFLOW");
      return resp;
    }

    if (tokens[*pos] == TOK_LINENUM ||
        tokens[*pos] == TOK_UNQUOTED_STR)
    {
      m_gosubStack[m_gosubDepth++].returnLineNum = currentLine;
      resp.result = TP_GOTO_LINE;
      resp.lineNum = readLineNum(tokens, pos);
    }
    else
    {
      resp.result = TP_ERROR;
      snprintf(resp.errorMsg, sizeof(resp.errorMsg), "SYNTAX ERROR");
    }
    return resp;
  }

  TPResponse execReturn()
  {
    TPResponse resp;
    resp.errorMsg[0] = '\0';

    if (m_gosubDepth <= 0)
    {
      resp.result = TP_ERROR;
      snprintf(resp.errorMsg, sizeof(resp.errorMsg), "RETURN WITHOUT GOSUB");
      return resp;
    }

    // Return to the line AFTER the GOSUB call
    resp.result = TP_GOTO_AFTER;
    resp.lineNum = m_gosubStack[--m_gosubDepth].returnLineNum;
    return resp;
  }

  TPResponse execIf(const uint8_t* tokens, int* pos, int length)
  {
    TPResponse resp;
    resp.result = TP_NEXT_TOKEN;
    resp.errorMsg[0] = '\0';

    bool cond = m_expr.evalCondition(tokens, pos);

    if (tokens[*pos] == TOK_THEN)
    {
      (*pos)++;
    }

    if (cond)
    {
      // Check if THEN is followed by a line number (GOTO shorthand).
      // Accept both TI-style TOK_LINENUM and legacy TOK_UNQUOTED_STR.
      if (tokens[*pos] == TOK_LINENUM ||
          tokens[*pos] == TOK_UNQUOTED_STR)
      {
        return execGoto(tokens, pos);
      }
      // Otherwise, continue processing rest of line (THEN clause)
      resp.result = TP_NEXT_TOKEN;
      return resp;
    }
    else
    {
      // Skip to ELSE or end of line
      int depth = 0;
      while (*pos < length && tokens[*pos] != TOK_EOL)
      {
        if (tokens[*pos] == TOK_ELSE && depth == 0)
        {
          (*pos)++;
          // Check if ELSE is followed by a line number
          if (tokens[*pos] == TOK_LINENUM ||
              tokens[*pos] == TOK_UNQUOTED_STR)
          {
            return execGoto(tokens, pos);
          }
          resp.result = TP_NEXT_TOKEN;
          return resp;
        }
        (*pos)++;
      }
      resp.result = TP_NEXT_LINE;
      return resp;
    }
  }

  TPResponse execFor(const uint8_t* tokens, int* pos, uint16_t lineNum,
                     bool isIteration)
  {
    TPResponse resp;
    resp.result = TP_NEXT_LINE;
    resp.errorMsg[0] = '\0';

    if (!isIdentStart(tokens[*pos]))
    {
      resp.result = TP_ERROR;
      snprintf(resp.errorMsg, sizeof(resp.errorMsg), "SYNTAX ERROR");
      return resp;
    }

    char vname[MAX_VAR_NAME];
    bool isStr;
    int vlen = parseIdent(tokens, pos, vname, sizeof(vname), &isStr);

    // Check if this variable is already on the FOR stack
    int stackIdx = findForVar(vname, vlen);

    // Always consume the FOR header tokens (`= start TO limit [STEP n]`)
    // so the dispatcher's pos lands on the body / EOL afterward — even
    // in iter mode. Without this, returning TP_NEXT_TOKEN from iter
    // mode would leave pos at TOK_EQUAL, and the main dispatch would
    // then walk into the literal-length byte of "1" (which happens to
    // be 0x01 = TOK_CONTINUE), firing CALL CONTINUE on every iteration.
    float startVal = 0, limitVal = 0, stepVal = 1;
    if (tokens[*pos] == TOK_EQUAL)
    {
      (*pos)++;
      startVal = m_expr.evalNumeric(tokens, pos);
    }
    if (tokens[*pos] == TOK_TO)
    {
      (*pos)++;
      limitVal = m_expr.evalNumeric(tokens, pos);
    }
    if (tokens[*pos] == TOK_STEP)
    {
      (*pos)++;
      stepVal = m_expr.evalNumeric(tokens, pos);
    }

    if (stackIdx >= 0)
    {
      // Iteration — increment and check limit (header already consumed)
      ForFrame* f = &m_forStack[stackIdx];
      Variable* v = m_vars.findNum(vname, vlen);
      if (v)
      {
        v->numVal += f->step;

        bool done = (f->step > 0) ? (v->numVal > f->limit)
                                  : (v->numVal < f->limit);
        if (done)
        {
          // Remove this frame and all above it
          m_forDepth = stackIdx;
          resp.result = TP_END_LOOP;
          return resp;
        }
      }
      // Loop continues — let dispatch keep running this line so a
      // single-line FOR :: BODY :: NEXT actually executes BODY each
      // iteration. (Multi-line FOR has nothing more on the FOR line,
      // so the dispatcher hits EOL and the outer code drives to body.)
      resp.result = TP_NEXT_TOKEN;
      return resp;
    }

    // First time through this FOR — header was parsed above. Set the
    // loop variable to the starting value and push a new FOR frame.
    m_vars.setNum(vname, vlen, startVal);

    // Push FOR frame
    if (m_forDepth < MAX_FOR_DEPTH)
    {
      ForFrame* f = &m_forStack[m_forDepth++];
      int copyLen = (vlen < MAX_VAR_NAME - 1) ? vlen : MAX_VAR_NAME - 1;
      memcpy(f->varName, vname, copyLen);
      f->varName[copyLen] = '\0';
      for (int i = 0; i < copyLen; i++)
      {
        f->varName[i] = toupper(f->varName[i]);
      }
      f->limit = limitVal;
      f->step = stepVal;
      f->forLineNum = lineNum;
    }

    // First-time setup done — keep dispatching this line so body
    // statements after FOR (`FOR I=1 TO 9 :: PRINT I :: NEXT I`) run.
    resp.result = TP_NEXT_TOKEN;
    return resp;
  }

  TPResponse execNext(const uint8_t* tokens, int* pos)
  {
    TPResponse resp;
    resp.errorMsg[0] = '\0';

    // Optional variable name after NEXT
    char varName[MAX_VAR_NAME] = "";
    int  varLen = 0;
    if (isIdentStart(tokens[*pos]))
    {
      bool isStr;
      varLen = parseIdent(tokens, pos, varName, sizeof(varName), &isStr);
      // Uppercase to match how execFor stores names
      for (int i = 0; i < varLen; i++)
      {
        if (varName[i] >= 'a' && varName[i] <= 'z') varName[i] -= 32;
      }
    }

    if (m_forDepth <= 0)
    {
      resp.result = TP_ERROR;
      snprintf(resp.errorMsg, sizeof(resp.errorMsg), "NEXT WITHOUT FOR");
      return resp;
    }

    // If a variable name was given, search the FOR stack for it and
    // pop any inner frames sitting above. (Real TI behavior — solves
    // nested-loop edge cases where a stale inner frame lingers after
    // an early exit.) No name = use the top of stack.
    int idx = m_forDepth - 1;
    if (varLen > 0)
    {
      while (idx >= 0 &&
             strcasecmp(m_forStack[idx].varName, varName) != 0)
      {
        idx--;
      }
      if (idx < 0)
      {
        resp.result = TP_ERROR;
        snprintf(resp.errorMsg, sizeof(resp.errorMsg),
                 "FOR-NEXT MISMATCH");
        return resp;
      }
      m_forDepth = idx + 1;   // pop everything above the match
    }

    ForFrame* f = &m_forStack[idx];
    resp.result = TP_NEXT_LOOP;
    resp.lineNum = f->forLineNum;
    return resp;
  }

  // Helper: read optional argument list "(arg, arg, ...)"
  // Returns number of args read into argv[], each evaluated as numeric.
  int readCallArgs(const uint8_t* tokens, int* pos, float* argv, int maxArgs)
  {
    int count = 0;
    if (tokens[*pos] != TOK_LPAREN) return 0;
    (*pos)++;
    while (count < maxArgs && tokens[*pos] != TOK_RPAREN &&
           tokens[*pos] != TOK_EOL)
    {
      argv[count++] = m_expr.evalNumeric(tokens, pos);
      if (tokens[*pos] == TOK_COMMA) (*pos)++;
    }
    if (tokens[*pos] == TOK_RPAREN) (*pos)++;
    return count;
  }

  TPResponse execCall(const uint8_t* tokens, int* pos)
  {
    TPResponse resp;
    resp.result = TP_NEXT_TOKEN;
    resp.errorMsg[0] = '\0';

    if (!isIdentStart(tokens[*pos]))
    {
      return resp;
    }

    char subName[32];
    bool isStr;
    parseIdent(tokens, pos, subName, sizeof(subName), &isStr);

    if (strcasecmp(subName, "CLEAR") == 0)
    {
      if (m_clearScreen) m_clearScreen();
      return resp;
    }

    if (strcasecmp(subName, "SCREEN") == 0)
    {
      float args[1];
      readCallArgs(tokens, pos, args, 1);
      if (m_setScreenColor) m_setScreenColor((int)args[0]);
      return resp;
    }

    if (strcasecmp(subName, "HCHAR") == 0)
    {
      // CALL HCHAR(row, col, char [, repeat])
      float args[4] = {1, 1, 32, 1};
      int n = readCallArgs(tokens, pos, args, 4);
      if (n < 3) return resp;
      int row = (int)args[0];
      int col = (int)args[1];
      int ch = (int)args[2];
      int rep = (n >= 4) ? (int)args[3] : 1;
      if (m_setChar)
      {
        for (int i = 0; i < rep; i++)
        {
          int r = row - 1;
          int c = col - 1 + i;
          // Wrap to next line if overflow (TI behavior)
          while (c >= TI_COLS)
          {
            c -= TI_COLS;
            r++;
          }
          if (r >= 0 && r < TI_ROWS)
          {
            m_setChar(r, c, (char)ch);
          }
        }
      }
      return resp;
    }

    if (strcasecmp(subName, "VCHAR") == 0)
    {
      // CALL VCHAR(row, col, char [, repeat])
      float args[4] = {1, 1, 32, 1};
      int n = readCallArgs(tokens, pos, args, 4);
      if (n < 3) return resp;
      int row = (int)args[0];
      int col = (int)args[1];
      int ch = (int)args[2];
      int rep = (n >= 4) ? (int)args[3] : 1;
      if (m_setChar)
      {
        for (int i = 0; i < rep; i++)
        {
          int r = row - 1 + i;
          int c = col - 1;
          // Wrap to next column if overflow
          while (r >= TI_ROWS)
          {
            r -= TI_ROWS;
            c++;
          }
          if (r >= 0 && r < TI_ROWS && c >= 0 && c < TI_COLS)
          {
            m_setChar(r, c, (char)ch);
          }
        }
      }
      return resp;
    }

    if (strcasecmp(subName, "GCHAR") == 0)
    {
      // CALL GCHAR(row, col, variable)
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      int row = (int)m_expr.evalNumeric(tokens, pos);
      if (tokens[*pos] == TOK_COMMA) (*pos)++;
      int col = (int)m_expr.evalNumeric(tokens, pos);
      if (tokens[*pos] == TOK_COMMA) (*pos)++;
      // Third arg is a variable to receive the character code
      if (isIdentStart(tokens[*pos]))
      {
        char vname[MAX_VAR_NAME];
        bool vIsStr;
        int vlen = parseIdent(tokens, pos, vname, sizeof(vname), &vIsStr);
        char ch = 32;
        if (m_getChar && row >= 1 && row <= TI_ROWS && col >= 1 && col <= TI_COLS)
        {
          ch = m_getChar(row - 1, col - 1);
        }
        m_vars.setNum(vname, vlen, (float)(uint8_t)ch);
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      return resp;
    }

    if (strcasecmp(subName, "CHARSET") == 0)
    {
      // CALL CHARSET — reset chars 32-127 to their ROM default patterns.
      // Leaves user-defined graphics chars (128+) alone.
      //
      // Non-TI extension: `CALL CHARSET("PC")` and `CALL CHARSET("TI")`
      // switch the active font ROM before resetting. "PC" is the default
      // simulator font; "TI" is the V9T9-derived authentic TI-99/4A font
      // (lowercase shows as small caps). The host persists the choice.
      if (tokens[*pos] == TOK_LPAREN)
      {
        (*pos)++;
        if (tokens[*pos] == TOK_QUOTED_STR || tokens[*pos] == TOK_STRING_LIT)
        {
          (*pos)++;
          int slen = tokens[*pos];
          (*pos)++;
          char arg[8];
          int n = (slen < (int)sizeof(arg) - 1) ? slen : (int)sizeof(arg) - 1;
          for (int i = 0; i < n; i++) arg[i] = (char)tokens[(*pos) + i];
          arg[n] = '\0';
          (*pos) += slen;
          int mode = -1;
          if      (strcasecmp(arg, "PC") == 0) mode = 0;
          else if (strcasecmp(arg, "TI") == 0) mode = 1;
          if (mode >= 0 && m_setCharsetMode) m_setCharsetMode(mode);
        }
        if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      }
      if (m_resetCharset) m_resetCharset();
      return resp;
    }

    if (strcasecmp(subName, "CHARPAT") == 0)
    {
      // CALL CHARPAT(char-code, string-var) — read a character's current
      // pattern as a 16-character hex string.
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      int code = (int)m_expr.evalNumeric(tokens, pos);
      if (tokens[*pos] == TOK_COMMA) (*pos)++;
      if (isIdentStart(tokens[*pos]))
      {
        char vname[MAX_VAR_NAME];
        bool vIsStr;
        int vlen = parseIdent(tokens, pos, vname, sizeof(vname), &vIsStr);
        if (vIsStr && m_getCharPattern && code >= 0 && code < 256)
        {
          uint8_t pat[8];
          m_getCharPattern(code, pat);
          char hex[17];
          for (int i = 0; i < 8; i++)
          {
            snprintf(&hex[i * 2], 3, "%02X", pat[i]);
          }
          hex[16] = '\0';
          m_vars.setStr(vname, vlen, hex);
        }
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      return resp;
    }

    if (strcasecmp(subName, "ERR") == 0)
    {
      // CALL ERR(err-num, severity, code, line) — returns info about the
      // last error caught by ON ERROR. We classify only I/O errors today,
      // so err-num is the TI I/O code (0..7), severity is 1 (recoverable
      // I/O) when an I/O error has occurred and 0 otherwise, code mirrors
      // err-num, and line is the line where the last error occurred.
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      const char* names[4] = {"", "", "", ""};
      char buf[4][MAX_VAR_NAME];
      int lens[4] = {0, 0, 0, 0};
      for (int i = 0; i < 4; i++)
      {
        if (tokens[*pos] == TOK_RPAREN || tokens[*pos] == TOK_EOL) break;
        if (isIdentStart(tokens[*pos]))
        {
          bool isStr;
          lens[i] = parseIdent(tokens, pos, buf[i], sizeof(buf[i]), &isStr);
          names[i] = buf[i];
        }
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      float vals[4] = {
        (float)m_lastIOError,
        (m_lastIOError > 0) ? 1.0f : 0.0f,
        (float)m_lastIOError,
        (float)m_lastErrorLine
      };
      for (int i = 0; i < 4; i++)
      {
        if (lens[i] > 0) m_vars.setNum(names[i], lens[i], vals[i]);
      }
      return resp;
    }

    // --- Sprite support (Phase 1: create / delete / locate / pattern /
    //     magnify). Motion + collision detection land in Phase 2/3.
    if (strcasecmp(subName, "SPRITE") == 0)
    {
      // CALL SPRITE(#n, char, color, row, col [, rVel, cVel] [, #n, ...])
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      while (tokens[*pos] != TOK_RPAREN && tokens[*pos] != TOK_EOL &&
             tokens[*pos] != TOK_COLON && tokens[*pos] != TOK_BANG)
      {
        if (tokens[*pos] == TOK_HASH) (*pos)++;
        int n = (int)m_expr.evalNumeric(tokens, pos);
        if (!sprites::validSlot(n)) break;
        sprites::Sprite& s = sprites::g_sprites[n];
        if (s.active && m_spriteErase) m_spriteErase(n);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        s.charCode = (uint8_t)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        s.colorIdx = (uint8_t)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        s.row      = (int16_t)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        s.col      = (int16_t)m_expr.evalNumeric(tokens, pos);
        // Optional velocity args. Only consume if a comma is followed
        // by something other than `#` (which would start the next
        // sprite in a multi-arg `CALL SPRITE(#1,...,#2,...)`).
        s.rowVel = 0; s.colVel = 0;
        if (tokens[*pos] == TOK_COMMA && tokens[*pos + 1] != TOK_HASH)
        {
          (*pos)++;
          s.rowVel = (int16_t)m_expr.evalNumeric(tokens, pos);
          if (tokens[*pos] == TOK_COMMA) (*pos)++;
          s.colVel = (int16_t)m_expr.evalNumeric(tokens, pos);
        }
        s.magnify = sprites::g_magnify;
        s.active  = true;
        // Seed snapshot so a POSITION/COINC right after CALL SPRITE
        // returns the just-set position instead of the default (1,1).
        s.snapRow     = s.row;
        s.snapCol     = s.col;
        s.snapMagnify = s.magnify;
        if (m_spriteDraw) m_spriteDraw(n);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      return resp;
    }

    if (strcasecmp(subName, "DELSPRITE") == 0)
    {
      // CALL DELSPRITE(#n [, #n ...]) or CALL DELSPRITE(ALL)
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      if (tokens[*pos] == TOK_ALL)
      {
        (*pos)++;
        for (int i = 1; i <= sprites::MAX_SPRITES; i++)
        {
          if (sprites::g_sprites[i].active)
          {
            if (m_spriteErase) m_spriteErase(i);
            sprites::g_sprites[i].active = false;
          }
        }
      }
      else
      {
        while (tokens[*pos] != TOK_RPAREN && tokens[*pos] != TOK_EOL &&
               tokens[*pos] != TOK_COLON && tokens[*pos] != TOK_BANG)
        {
          if (tokens[*pos] == TOK_HASH) (*pos)++;
          int n = (int)m_expr.evalNumeric(tokens, pos);
          if (sprites::validSlot(n) && sprites::g_sprites[n].active)
          {
            if (m_spriteErase) m_spriteErase(n);
            sprites::g_sprites[n].active = false;
          }
          if (tokens[*pos] == TOK_COMMA) (*pos)++;
        }
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      return resp;
    }

    if (strcasecmp(subName, "LOCATE") == 0)
    {
      // CALL LOCATE(#n, row, col [, #n, row, col ...])
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      while (tokens[*pos] != TOK_RPAREN && tokens[*pos] != TOK_EOL &&
             tokens[*pos] != TOK_COLON && tokens[*pos] != TOK_BANG)
      {
        if (tokens[*pos] == TOK_HASH) (*pos)++;
        int n = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        int r = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        int c = (int)m_expr.evalNumeric(tokens, pos);
        if (sprites::validSlot(n) && sprites::g_sprites[n].active)
        {
          if (m_spriteErase) m_spriteErase(n);
          sprites::g_sprites[n].row     = (int16_t)r;
          sprites::g_sprites[n].col     = (int16_t)c;
          sprites::g_sprites[n].snapRow = (int16_t)r;
          sprites::g_sprites[n].snapCol = (int16_t)c;
          if (m_spriteDraw) m_spriteDraw(n);
        }
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      return resp;
    }

    if (strcasecmp(subName, "PATTERN") == 0)
    {
      // CALL PATTERN(#n, char-code [, #n, char-code ...])
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      while (tokens[*pos] != TOK_RPAREN && tokens[*pos] != TOK_EOL &&
             tokens[*pos] != TOK_COLON && tokens[*pos] != TOK_BANG)
      {
        if (tokens[*pos] == TOK_HASH) (*pos)++;
        int n = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        int code = (int)m_expr.evalNumeric(tokens, pos);
        if (sprites::validSlot(n) && sprites::g_sprites[n].active)
        {
          if (m_spriteErase) m_spriteErase(n);
          sprites::g_sprites[n].charCode = (uint8_t)code;
          if (m_spriteDraw) m_spriteDraw(n);
        }
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      return resp;
    }

    if (strcasecmp(subName, "POSITION") == 0)
    {
      // CALL POSITION(#n, row-var, col-var [, #n, row-var, col-var ...])
      // Writes the sprite's current TI-pixel coordinates into the
      // listed numeric variables.
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      while (tokens[*pos] != TOK_RPAREN && tokens[*pos] != TOK_EOL &&
             tokens[*pos] != TOK_COLON && tokens[*pos] != TOK_BANG)
      {
        if (tokens[*pos] == TOK_HASH) (*pos)++;
        int n = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        // row variable
        char rname[MAX_VAR_NAME]; int rlen = 0; bool rIsStr;
        if (isIdentStart(tokens[*pos]))
        {
          rlen = parseIdent(tokens, pos, rname, sizeof(rname), &rIsStr);
        }
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        // col variable
        char cname[MAX_VAR_NAME]; int clen = 0; bool cIsStr;
        if (isIdentStart(tokens[*pos]))
        {
          clen = parseIdent(tokens, pos, cname, sizeof(cname), &cIsStr);
        }
        if (sprites::validSlot(n) && sprites::g_sprites[n].active)
        {
          // snapRow/snapCol = position at the end of the last
          // spriteTick, so all queries within one BASIC iteration
          // are coherent even if a tick fires between them.
          if (rlen > 0)
            m_vars.setNum(rname, rlen,
                          (float)sprites::g_sprites[n].snapRow);
          if (clen > 0)
            m_vars.setNum(cname, clen,
                          (float)sprites::g_sprites[n].snapCol);
        }
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      return resp;
    }

    if (strcasecmp(subName, "DISTANCE") == 0)
    {
      // CALL DISTANCE(#s1, #s2, var)            — sprite-sprite
      // CALL DISTANCE(#s, row, col, var)        — sprite-point
      // Returns sum of squared row/col deltas (TI convention; no sqrt).
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      if (tokens[*pos] == TOK_HASH) (*pos)++;
      int s1 = (int)m_expr.evalNumeric(tokens, pos);
      if (tokens[*pos] == TOK_COMMA) (*pos)++;
      int r1 = 0, c1 = 0, valid1 = 0;
      if (sprites::validSlot(s1) && sprites::g_sprites[s1].active)
      {
        r1 = sprites::g_sprites[s1].snapRow;
        c1 = sprites::g_sprites[s1].snapCol;
        valid1 = 1;
      }
      // Second arg may be #spriteN or a row literal
      int r2 = 0, c2 = 0, valid2 = 0;
      if (tokens[*pos] == TOK_HASH)
      {
        (*pos)++;
        int s2 = (int)m_expr.evalNumeric(tokens, pos);
        if (sprites::validSlot(s2) && sprites::g_sprites[s2].active)
        {
          r2 = sprites::g_sprites[s2].snapRow;
          c2 = sprites::g_sprites[s2].snapCol;
          valid2 = 1;
        }
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
      }
      else
      {
        r2 = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        c2 = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        valid2 = 1;
      }
      // Result variable
      char vname[MAX_VAR_NAME]; int vlen = 0; bool vIsStr;
      if (isIdentStart(tokens[*pos]))
      {
        vlen = parseIdent(tokens, pos, vname, sizeof(vname), &vIsStr);
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      if (valid1 && valid2 && vlen > 0)
      {
        long dr = r1 - r2, dc = c1 - c2;
        m_vars.setNum(vname, vlen, (float)(dr * dr + dc * dc));
      }
      else if (vlen > 0)
      {
        // Sprite inactive — TI returns -1
        m_vars.setNum(vname, vlen, -1.0f);
      }
      return resp;
    }

    if (strcasecmp(subName, "COINC") == 0)
    {
      // CALL COINC(#s1, #s2, tolerance, var)         — sprite pair
      // CALL COINC(#s, row, col, tolerance, var)     — sprite-point
      // CALL COINC(ALL, var)                         — any pair?
      // Result: -1 if coincident within tolerance, 0 otherwise.
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;

      if (tokens[*pos] == TOK_ALL)
      {
        (*pos)++;
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        char vname[MAX_VAR_NAME]; int vlen = 0; bool vIsStr;
        if (isIdentStart(tokens[*pos]))
        {
          vlen = parseIdent(tokens, pos, vname, sizeof(vname), &vIsStr);
        }
        if (tokens[*pos] == TOK_RPAREN) (*pos)++;
        // Check every active pair for any overlap (8-px box per side
        // of magnify-1 sprite, scale up for magnified).
        int hit = 0;
        for (int a = 1; a <= sprites::MAX_SPRITES && !hit; a++)
        {
          if (!sprites::g_sprites[a].active) continue;
          for (int b = a + 1; b <= sprites::MAX_SPRITES && !hit; b++)
          {
            if (!sprites::g_sprites[b].active) continue;
            int sa = sprites::footprint(sprites::g_sprites[a].snapMagnify);
            int sb = sprites::footprint(sprites::g_sprites[b].snapMagnify);
            int ra = sprites::g_sprites[a].snapRow;
            int ca = sprites::g_sprites[a].snapCol;
            int rb = sprites::g_sprites[b].snapRow;
            int cb = sprites::g_sprites[b].snapCol;
            if (ra < rb + sb && rb < ra + sa &&
                ca < cb + sb && cb < ca + sa) hit = -1;
          }
        }
        if (vlen > 0) m_vars.setNum(vname, vlen, (float)hit);
        return resp;
      }

      if (tokens[*pos] == TOK_HASH) (*pos)++;
      int s1 = (int)m_expr.evalNumeric(tokens, pos);
      if (tokens[*pos] == TOK_COMMA) (*pos)++;

      bool spritePair = (tokens[*pos] == TOK_HASH);
      int s2 = 0, r2 = 0, c2 = 0;
      if (spritePair)
      {
        (*pos)++;
        s2 = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
      }
      else
      {
        r2 = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        c2 = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
      }

      int tolerance = (int)m_expr.evalNumeric(tokens, pos);
      if (tokens[*pos] == TOK_COMMA) (*pos)++;

      char vname[MAX_VAR_NAME]; int vlen = 0; bool vIsStr;
      if (isIdentStart(tokens[*pos]))
      {
        vlen = parseIdent(tokens, pos, vname, sizeof(vname), &vIsStr);
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;

      int result = 0;
      if (sprites::validSlot(s1) && sprites::g_sprites[s1].active)
      {
        int r1 = sprites::g_sprites[s1].snapRow;
        int c1 = sprites::g_sprites[s1].snapCol;
        int rr, cc;
        if (spritePair && sprites::validSlot(s2) &&
            sprites::g_sprites[s2].active)
        {
          rr = sprites::g_sprites[s2].snapRow;
          cc = sprites::g_sprites[s2].snapCol;
        }
        else if (!spritePair)
        {
          rr = r2; cc = c2;
        }
        else
        {
          if (vlen > 0) m_vars.setNum(vname, vlen, 0.0f);
          return resp;
        }
        long dr = r1 - rr, dc = c1 - cc;
        if (dr < 0) dr = -dr;
        if (dc < 0) dc = -dc;
        if (dr <= tolerance && dc <= tolerance) result = -1;
      }
      if (vlen > 0) m_vars.setNum(vname, vlen, (float)result);
      return resp;
    }

    if (strcasecmp(subName, "MOTION") == 0)
    {
      // CALL MOTION(#n, rowVel, colVel [, #n, rowVel, colVel ...])
      // Velocity range -128..127. Effective pixel motion is vel/8 per
      // 60 Hz frame, integrated by the sketch's spriteTick().
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      while (tokens[*pos] != TOK_RPAREN && tokens[*pos] != TOK_EOL &&
             tokens[*pos] != TOK_COLON && tokens[*pos] != TOK_BANG)
      {
        if (tokens[*pos] == TOK_HASH) (*pos)++;
        int n = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        int rv = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        int cv = (int)m_expr.evalNumeric(tokens, pos);
        if (sprites::validSlot(n) && sprites::g_sprites[n].active)
        {
          sprites::g_sprites[n].rowVel = (int16_t)rv;
          sprites::g_sprites[n].colVel = (int16_t)cv;
          // Reset accumulators so a fresh velocity starts cleanly
          sprites::g_sprites[n].subRow = 0;
          sprites::g_sprites[n].subCol = 0;
        }
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      return resp;
    }

    if (strcasecmp(subName, "MAGNIFY") == 0)
    {
      // CALL MAGNIFY(size) — size 1..4
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      int size = (int)m_expr.evalNumeric(tokens, pos);
      if (size < 1) size = 1;
      if (size > 4) size = 4;
      // All future sprites inherit this magnify. For existing sprites,
      // re-apply so they render at the new size (TI behavior).
      sprites::g_magnify = (uint8_t)size;
      for (int i = 1; i <= sprites::MAX_SPRITES; i++)
      {
        if (sprites::g_sprites[i].active)
        {
          if (m_spriteErase) m_spriteErase(i);
          sprites::g_sprites[i].magnify = (uint8_t)size;
          if (m_spriteDraw) m_spriteDraw(i);
        }
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      return resp;
    }

    if (strcasecmp(subName, "TIMER") == 0)
    {
      // CALL TIMER(numeric-var) — write current millis() into the var.
      // Use it to measure elapsed time between two points:
      //   100 CALL TIMER(T0)
      //   ... do stuff ...
      //   200 CALL TIMER(T1)
      //   210 PRINT "ELAPSED MS:";T1-T0
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      if (isIdentStart(tokens[*pos]))
      {
        char vname[MAX_VAR_NAME];
        bool isStr;
        int vlen = parseIdent(tokens, pos, vname, sizeof(vname), &isStr);
        m_vars.setNum(vname, vlen, (float)millis());
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      return resp;
    }

    if (strcasecmp(subName, "DELAY") == 0)
    {
      // CALL DELAY(ms) — block for ms milliseconds. Yields to the BLE
      // task / display refresh / sprite ticker so the simulator stays
      // responsive during the wait.
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      long ms = (long)m_expr.evalNumeric(tokens, pos);
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      if (ms > 0) delay((unsigned long)ms);
      return resp;
    }

    if (strcasecmp(subName, "SPEED") == 0)
    {
      // CALL SPEED(microsecondsPerLine)
      //   0    = unthrottled (default, native ESP32 speed)
      //   285  ≈ TI Extended BASIC authentic speed
      //   666  ≈ stock TI BASIC authentic speed
      // Helps with games written for the original TI's pace.
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      unsigned long us = (unsigned long)m_expr.evalNumeric(tokens, pos);
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      if (m_setThrottle) m_setThrottle(us);
      return resp;
    }

    if (strcasecmp(subName, "PAIR") == 0)
    {
      // CALL PAIR — open a 30-second BLE-HID pairing window. Non-TI
      // extension (real TI never had Bluetooth) but useful on the ESP32
      // ports for triggering pairing without leaving BASIC. Accepts an
      // optional empty () for syntactic consistency with other CALLs.
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      tiPair();
      return resp;
    }

    if (strcasecmp(subName, "UNPAIR") == 0)
    {
      // CALL UNPAIR — forget every currently-bonded BLE HID peer. Non-TI
      // extension. Useful for clearing stale bonds without a reflash.
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      tiUnpair();
      return resp;
    }

    if (strcasecmp(subName, "WIFI") == 0)
    {
      // CALL WIFI                  — print current status line
      // CALL WIFI(ssid$, pass$)    — store creds + connect
      // CALL WIFI("forget")        — clear creds + disconnect
      // Non-TI extension. Real TI had no WiFi.
      auto putLine = [&](const char* s) {
        if (m_printString)
        {
          m_printString(s);
          m_printString("\n");
        }
      };

      if (tokens[*pos] != TOK_LPAREN)
      {
        char line[96];
        tiWifiStatus(line, sizeof(line));
        putLine(line);
        return resp;
      }
      (*pos)++;   // past LPAREN

      char arg1[80] = {0};
      m_expr.evalString(tokens, pos, arg1, sizeof(arg1));

      if (tokens[*pos] == TOK_RPAREN)
      {
        // Single-string form. "forget" / "on" / "off" are recognized
        // verbs; anything else falls back to status (we don't store
        // a single-arg SSID with no password).
        (*pos)++;
        if (strcasecmp(arg1, "forget") == 0)
        {
          tiWifiForget();
          putLine("WIFI: credentials cleared");
        }
        else if (strcasecmp(arg1, "off") == 0)
        {
          tiWifiOff();
          putLine("WIFI: off");
        }
        else if (strcasecmp(arg1, "on") == 0)
        {
          tiWifiOn();
          putLine("WIFI: on");
        }
        else
        {
          char line[96];
          tiWifiStatus(line, sizeof(line));
          putLine(line);
        }
        return resp;
      }

      // Two-string form: ssid, pass.
      if (tokens[*pos] == TOK_COMMA) (*pos)++;
      char arg2[80] = {0};
      m_expr.evalString(tokens, pos, arg2, sizeof(arg2));
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      tiWifiSet(arg1, arg2);
      putLine("WIFI: credentials stored; connecting");
      return resp;
    }

    if (strcasecmp(subName, "SOUND") == 0)
    {
      // CALL SOUND(duration, freq1, vol1 [, freq2, vol2, freq3, vol3,
      //                                    freq4, vol4])
      //   duration  : milliseconds, 1..4250 (i.e. .001 to 4.250 sec)
      //   freq      : Hz, or -1..-8 for noise channels
      //   volume    : 0 (loudest) to 30 (silent)
      //
      // TI semantics:
      //   - Sounds play asynchronously; the program keeps running.
      //   - A new CALL SOUND with positive duration waits for the
      //     PREVIOUS sound to finish before starting (and returning).
      //   - Negative duration cancels any current sound and starts
      //     the new one immediately, returning right away.
      //
      // Stub: no audio wired yet, so we simulate the timing only.
      // Programs that use CALL SOUND for pacing will run at the
      // right speed; programs that listen for the notes won't hear
      // anything.
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      int duration = (int)m_expr.evalNumeric(tokens, pos);
      int freqs[4] = {0, 0, 0, 0};
      int vols[4]  = {30, 30, 30, 30};   // 30 = silent
      int voices = 0;
      while (tokens[*pos] == TOK_COMMA && voices < 4)
      {
        (*pos)++;
        freqs[voices] = (int)m_expr.evalNumeric(tokens, pos);
        if (tokens[*pos] == TOK_COMMA) (*pos)++;
        vols[voices]  = (int)m_expr.evalNumeric(tokens, pos);
        voices++;
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;

      int absDur = (duration >= 0) ? duration : -duration;
      if (absDur > 4250) absDur = 4250;

      if (duration > 0)
      {
        // Wait for any previous sound's scheduled end time, then
        // schedule this new sound and return.
        unsigned long now = millis();
        while (now < m_soundEndTime)
        {
          delay(10);
          now = millis();
        }
        tiSoundStop();
        tiSoundPlay(absDur, freqs[0], vols[0], freqs[1], vols[1],
                            freqs[2], vols[2], freqs[3], vols[3]);
        m_soundEndTime = now + (unsigned long)absDur;
      }
      else
      {
        // Negative duration cancels in-flight sound and schedules
        // the new one; return immediately.
        tiSoundStop();
        tiSoundPlay(absDur, freqs[0], vols[0], freqs[1], vols[1],
                            freqs[2], vols[2], freqs[3], vols[3]);
        m_soundEndTime = millis() + (unsigned long)absDur;
      }
      return resp;
    }

    if (strcasecmp(subName, "SAY") == 0)
    {
      // CALL SAY([word-string] [, phrase-string])
      //
      // Speaks vocabulary words named in word-string (space-separated),
      // then any pre-fetched LPC phrase in phrase-string. Either arg may
      // be omitted (leading comma skips the word-string).
      //
      //   CALL SAY("HELLO TEXAS INSTRUMENTS")
      //   CALL SPGET("HELLO", P$) :: CALL SAY(, P$)
      //   CALL SAY("READY", P$)
      //
      // Default weak hook is a no-op; the host wires a real TMS5220
      // synth to the I²S audio mixer.
      char wordBuf[128];
      uint8_t phraseBuf[256];
      int phraseLen = 0;
      wordBuf[0] = '\0';
      bool haveWords = false;
      bool havePhrase = false;
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      if (tokens[*pos] != TOK_COMMA && tokens[*pos] != TOK_RPAREN &&
          tokens[*pos] != TOK_EOL && tokens[*pos] != TOK_COLON)
      {
        m_expr.evalString(tokens, pos, wordBuf, sizeof(wordBuf));
        haveWords = (wordBuf[0] != '\0');
      }
      if (tokens[*pos] == TOK_COMMA)
      {
        (*pos)++;
        char tmp[256];
        m_expr.evalString(tokens, pos, tmp, sizeof(tmp));
        int slen = (int)strlen(tmp);
        if (slen > 0 && slen <= (int)sizeof(phraseBuf))
        {
          memcpy(phraseBuf, tmp, slen);
          phraseLen = slen;
          havePhrase = true;
        }
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      tiSay(haveWords ? wordBuf : NULL,
            havePhrase ? phraseBuf : NULL, phraseLen);
      return resp;
    }

    if (strcasecmp(subName, "SPGET") == 0)
    {
      // CALL SPGET(word-string, phrase-variable$)
      //
      // Looks up word in the speech ROM vocabulary and stores its LPC
      // byte sequence in the supplied string variable so the program can
      // pass it back to CALL SAY (the trick most TI speech demos rely on
      // for splicing / pitch-modifying phrases).
      //
      // Known limitation: our string storage is null-terminated, so any
      // 0x00 byte inside the LPC phrase truncates the result here. Real
      // TI strings are length-prefixed and bit-clean. To be addressed
      // when the synth lands and we know whether real phrases ever
      // contain interior nulls.
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      char word[32];
      m_expr.evalString(tokens, pos, word, sizeof(word));
      if (tokens[*pos] == TOK_COMMA) (*pos)++;
      if (isIdentStart(tokens[*pos]))
      {
        char vname[MAX_VAR_NAME];
        bool vIsStr;
        int vlen = parseIdent(tokens, pos, vname, sizeof(vname), &vIsStr);
        if (vIsStr)
        {
          uint8_t buf[256];
          int n = tiSpget(word, buf, sizeof(buf) - 1);
          if (n < 0) n = 0;
          if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
          buf[n] = 0;
          m_vars.setStr(vname, vlen, (const char*)buf);
        }
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      return resp;
    }

    if (strcasecmp(subName, "VERSION") == 0)
    {
      // CALL VERSION(numeric-variable) — returns the Extended BASIC
      // version number (110 = v1.10). Real TI Ext BASIC returns 110
      // since v1.00 was pulled for bugs; we match that.
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      if (isIdentStart(tokens[*pos]))
      {
        char vname[MAX_VAR_NAME];
        bool vIsStr;
        int vlen = parseIdent(tokens, pos, vname, sizeof(vname), &vIsStr);
        m_vars.setNum(vname, vlen, 110.0f);
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      return resp;
    }

    if (strcasecmp(subName, "COLOR") == 0)
    {
      // CALL COLOR(charset, fg, bg)
      float args[3] = {1, 2, 1};
      int n = readCallArgs(tokens, pos, args, 3);
      if (n < 3) return resp;
      if (m_setCharColor)
      {
        m_setCharColor((int)args[0], (int)args[1], (int)args[2]);
      }
      return resp;
    }

    if (strcasecmp(subName, "KEY") == 0)
    {
      // CALL KEY(mode, key_var, status_var)
      // mode: 0=ignore (always current), 1/2/3=modes (we treat all same)
      // key: char code of pressed key (or -1 if none)
      // status: 1 if new key, 0 if same as last, -1 if no key
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      int mode = (int)m_expr.evalNumeric(tokens, pos);
      (void)mode;
      if (tokens[*pos] == TOK_COMMA) (*pos)++;

      // Read the key variable name
      char keyVar[MAX_VAR_NAME] = "";
      int keyLen = 0;
      if (isIdentStart(tokens[*pos]))
      {
        bool vIsStr;
        keyLen = parseIdent(tokens, pos, keyVar, sizeof(keyVar), &vIsStr);
      }
      if (tokens[*pos] == TOK_COMMA) (*pos)++;

      char stVar[MAX_VAR_NAME] = "";
      int stLen = 0;
      if (isIdentStart(tokens[*pos]))
      {
        bool vIsStr;
        stLen = parseIdent(tokens, pos, stVar, sizeof(stVar), &vIsStr);
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;

      int key = m_readKey ? m_readKey() : 0;
      int status;
      if (key > 0)
      {
        status = 1;  // new key available
      }
      else
      {
        key = -1;
        status = 0;  // no key
      }

      if (keyLen > 0) m_vars.setNum(keyVar, keyLen, (float)key);
      if (stLen > 0)  m_vars.setNum(stVar, stLen, (float)status);
      return resp;
    }

    if (strcasecmp(subName, "JOYST") == 0)
    {
      // CALL JOYST(unit, X, Y) — unit is 1 (only one pad supported);
      // X and Y get -4/0/+4 from the BLE gamepad's D-pad / hat or from
      // the keyboard's arrow keys (whichever is active).
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      int unit = (int)m_expr.evalNumeric(tokens, pos);
      if (tokens[*pos] == TOK_COMMA) (*pos)++;

      char xVar[MAX_VAR_NAME] = "";
      int xLen = 0;
      if (isIdentStart(tokens[*pos]))
      {
        bool vIsStr;
        xLen = parseIdent(tokens, pos, xVar, sizeof(xVar), &vIsStr);
      }
      if (tokens[*pos] == TOK_COMMA) (*pos)++;

      char yVar[MAX_VAR_NAME] = "";
      int yLen = 0;
      if (isIdentStart(tokens[*pos]))
      {
        bool vIsStr;
        yLen = parseIdent(tokens, pos, yVar, sizeof(yVar), &vIsStr);
      }
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;

      int x = 0, y = 0;
      if (m_readJoystick) m_readJoystick(unit, &x, &y);
      if (xLen > 0) m_vars.setNum(xVar, xLen, (float)x);
      if (yLen > 0) m_vars.setNum(yVar, yLen, (float)y);
      return resp;
    }

    if (strcasecmp(subName, "CHAR") == 0)
    {
      // CALL CHAR(char_code, "hex_string")
      // hex_string is 16 hex digits (8 bytes = 8 rows of 8 pixels).
      // Up to 64 digits for 4 consecutive characters.
      if (tokens[*pos] == TOK_LPAREN) (*pos)++;
      int charCode = (int)m_expr.evalNumeric(tokens, pos);
      if (tokens[*pos] == TOK_COMMA) (*pos)++;
      char hexBuf[128];
      m_expr.evalString(tokens, pos, hexBuf, sizeof(hexBuf));
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;

      // Parse hex string into 8-byte pattern, up to 8 chars worth
      int hlen = strlen(hexBuf);
      int numChars = (hlen + 15) / 16;
      for (int c = 0; c < numChars; c++)
      {
        uint8_t pattern[8] = {0};
        for (int b = 0; b < 8; b++)
        {
          int idx = c * 16 + b * 2;
          if (idx + 1 >= hlen) break;
          char hi = hexBuf[idx];
          char lo = hexBuf[idx + 1];
          uint8_t byte = 0;
          if (hi >= '0' && hi <= '9') byte = (hi - '0') << 4;
          else if (hi >= 'A' && hi <= 'F') byte = (hi - 'A' + 10) << 4;
          else if (hi >= 'a' && hi <= 'f') byte = (hi - 'a' + 10) << 4;
          if (lo >= '0' && lo <= '9') byte |= (lo - '0');
          else if (lo >= 'A' && lo <= 'F') byte |= (lo - 'A' + 10);
          else if (lo >= 'a' && lo <= 'f') byte |= (lo - 'a' + 10);
          pattern[b] = byte;
        }
        if (m_setCharPattern)
        {
          m_setCharPattern(charCode + c, pattern);
        }
      }
      return resp;
    }

    // User-defined subprogram lookup — CALL name(args)
    {
      int nameLen = (int)strlen(subName);
      SubDef* sd = m_vars.findSub(subName, nameLen);
      if (sd != NULL)
      {
        TPResponse out;
        out.result = TP_CALL_SUB;
        out.errorMsg[0] = '\0';
        out.aliasCount = 0;

        // Bind positional arguments to parameter names. Parameters are
        // stored as raw-ASCII identifier token slices separated by commas.
        if (tokens[*pos] == TOK_LPAREN) (*pos)++;

        // Walk the saved paramTokens and the caller's arg list in parallel.
        int pp = 0;
        while (pp < sd->paramTokensLen)
        {
          // Skip commas between params
          if (sd->paramTokens[pp] == TOK_COMMA) { pp++; continue; }
          if (!isIdentStart(sd->paramTokens[pp])) { pp++; continue; }

          char pname[16];
          int pLen = 0;
          while (pp < sd->paramTokensLen &&
                 isIdentCont(sd->paramTokens[pp]) &&
                 pLen < 15)
          {
            pname[pLen++] = (char)sd->paramTokens[pp++];
          }
          bool pIsStr = (pLen > 0 && pname[pLen - 1] == '$');

          // Evaluate the matching caller argument (skip leading comma)
          if (tokens[*pos] == TOK_COMMA) (*pos)++;
          if (tokens[*pos] == TOK_RPAREN ||
              tokens[*pos] == TOK_EOL ||
              tokens[*pos] == TOK_COLON)
          {
            break;   // too few args — just stop
          }

          // Pass-by-value-result detection: if the caller's arg is a bare
          // variable (ident followed by comma / rparen / eol / colon),
          // record an alias so SUBEND copies the sub's final value back
          // into the caller's variable.
          char callerName[12];
          int  callerLen = 0;
          bool isBareVar = false;
          bool callerIsStr = false;
          if (isIdentStart(tokens[*pos]))
          {
            int probe = *pos;
            while (isIdentCont(tokens[probe]) && callerLen < 11)
            {
              callerName[callerLen++] = (char)tokens[probe++];
            }
            uint8_t nxt = tokens[probe];
            if (nxt == TOK_COMMA || nxt == TOK_RPAREN ||
                nxt == TOK_EOL   || nxt == TOK_COLON)
            {
              isBareVar = true;
              callerIsStr = (callerLen > 0 &&
                             callerName[callerLen - 1] == '$');
            }
            else
            {
              callerLen = 0;   // not bare — reset
            }
          }

          if (pIsStr)
          {
            char argBuf[MAX_STR_LEN];
            m_expr.evalString(tokens, pos, argBuf, sizeof(argBuf));
            m_vars.setStr(pname, pLen, argBuf);
          }
          else
          {
            float v = m_expr.evalNumeric(tokens, pos);
            m_vars.setNum(pname, pLen, v);
          }

          if (isBareVar && pIsStr == callerIsStr &&
              out.aliasCount < 6)
          {
            TPSubAlias& a = out.aliases[out.aliasCount];
            int i;
            for (i = 0; i < pLen && i < 11; i++) a.paramName[i] = pname[i];
            a.paramName[i] = '\0';
            a.paramLen = (uint8_t)pLen;
            for (i = 0; i < callerLen && i < 11; i++) a.callerName[i] = callerName[i];
            a.callerName[i] = '\0';
            a.callerLen = (uint8_t)callerLen;
            a.isStr = pIsStr;
            out.aliasCount++;
          }
        }
        if (tokens[*pos] == TOK_RPAREN) (*pos)++;

        // Jump to the first body line (SUB declaration index + 1).
        out.lineIdx = sd->declLineIdx;   // EM advances to body = idx+1
        return out;
      }
    }

    resp.result = TP_ERROR;
    snprintf(resp.errorMsg, sizeof(resp.errorMsg),
             "BAD NAME: %s", subName);
    return resp;
  }

  // --- INPUT handling ---
  // INPUT [prompt;] var [, var...]
  TPResponse execInput(const uint8_t* tokens, int* pos)
  {
    TPResponse resp;
    resp.result = TP_NEED_INPUT;
    resp.errorMsg[0] = '\0';
    resp.prompt[0] = '\0';

    // INPUT #n [, REC k] / LINPUT #n — read from a file unit instead of user.
    if (tokens[*pos] == TOK_HASH)
    {
      (*pos)++;
      int unit = (int)m_expr.evalNumeric(tokens, pos);
      if (tokens[*pos] == TOK_COMMA && tokens[*pos + 1] == TOK_REC)
      {
        (*pos)++;   // comma
        (*pos)++;   // REC
        if (tokens[*pos] == TOK_EQUAL) (*pos)++;
        long rec = (long)m_expr.evalNumeric(tokens, pos);
        if (m_fileSeekRec) m_fileSeekRec(unit, rec);
      }
      if (tokens[*pos] == TOK_COLON) (*pos)++;
      execInputFile(tokens, pos, unit);
      m_linputMode = false;   // reset flag even on file path
      TPResponse ok;
      ok.result = TP_NEXT_TOKEN;
      ok.errorMsg[0] = '\0';
      return ok;
    }

    // Optional prompt: "text";
    if (tokens[*pos] == TOK_STRING_LIT)
    {
      (*pos)++;
      int slen = tokens[*pos];
      (*pos)++;
      int copyLen = (slen < (int)sizeof(resp.prompt) - 1) ? slen :
                    (int)sizeof(resp.prompt) - 1;
      memcpy(resp.prompt, &tokens[*pos], copyLen);
      resp.prompt[copyLen] = '\0';
      *pos += slen;
      // Expect : or ; separator
      if (tokens[*pos] == TOK_COLON || tokens[*pos] == TOK_SEMICOLON)
      {
        (*pos)++;
      }
    }
    else
    {
      strcpy(resp.prompt, "? ");
    }

    // Save where the variable list starts (for provideInput)
    m_inputTokens = tokens;
    m_inputVarStart = *pos;

    // Skip to end of statement so EM advances past it
    while (tokens[*pos] != TOK_EOL && tokens[*pos] != TOK_COLON &&
           tokens[*pos] != TOK_BANG)
    {
      (*pos)++;
    }

    return resp;
  }

  // INPUT #n / LINPUT #n — read a line from the file and assign to
  // the listed variables. LINPUT takes the whole line into one string;
  // INPUT splits on commas like interactive INPUT.
  void execInputFile(const uint8_t* tokens, int* pos, int unit)
  {
    char line[MAX_STR_LEN];
    line[0] = '\0';
    if (m_fileReadLine)
    {
      int rc = m_fileReadLine(unit, line, sizeof(line));
      // rc == 2 is the file_io EOF sentinel — that becomes "I/O ERROR 04"
      // (past-EOF) in TI terms, but only if the program tries to read
      // beyond EOF without checking EOF() first. The shim translates.
      if (rc != 0) raiseIOError(rc);
    }

    // LINPUT: first variable gets the full line (if string); extra vars
    // get empty. For INPUT, split the line on commas.
    const char* p = line;
    while (tokens[*pos] != TOK_EOL && tokens[*pos] != TOK_COLON &&
           tokens[*pos] != TOK_BANG)
    {
      if (tokens[*pos] == TOK_COMMA) { (*pos)++; continue; }
      if (!isIdentStart(tokens[*pos])) { (*pos)++; continue; }

      char vname[MAX_VAR_NAME];
      bool isStr;
      int vlen = parseIdent(tokens, pos, vname, sizeof(vname), &isStr);

      char field[MAX_STR_LEN];
      int fl = 0;
      if (m_linputMode)
      {
        // Whole remaining line goes to first var; subsequent vars empty
        while (*p && fl < (int)sizeof(field) - 1) field[fl++] = *p++;
      }
      else
      {
        // Skip leading spaces
        while (*p == ' ') p++;
        while (*p && *p != ',' && fl < (int)sizeof(field) - 1)
        {
          field[fl++] = *p++;
        }
        if (*p == ',') p++;
      }
      field[fl] = '\0';

      if (isStr)
      {
        m_vars.setStr(vname, vlen, field);
      }
      else
      {
        m_vars.setNum(vname, vlen, (float)atof(field));
      }
    }
  }

  // Called by the EM after user input is received
  void provideInput(const char* input)
  {
    int pos = m_inputVarStart;
    const char* inputPtr = input;

    // LINPUT: the entire line (commas included) goes into the first
    // string variable. No parsing, no trimming.
    if (m_linputMode)
    {
      m_linputMode = false;
      while (m_inputTokens[pos] != TOK_EOL &&
             m_inputTokens[pos] != TOK_COLON &&
             !isIdentStart(m_inputTokens[pos]))
      {
        pos++;
      }
      if (isIdentStart(m_inputTokens[pos]))
      {
        char vname[MAX_VAR_NAME];
        bool isStr;
        int vlen = parseIdent(m_inputTokens, &pos, vname,
                              sizeof(vname), &isStr);
        if (isStr)
        {
          m_vars.setStr(vname, vlen, input);
        }
      }
      return;
    }

    while (m_inputTokens[pos] != TOK_EOL && m_inputTokens[pos] != TOK_COLON)
    {
      if (m_inputTokens[pos] == TOK_COMMA)
      {
        pos++;
        continue;
      }

      if (isIdentStart(m_inputTokens[pos]))
      {
        char vname[MAX_VAR_NAME];
        bool isStr;
        int vlen = parseIdent(m_inputTokens, &pos, vname,
                              sizeof(vname), &isStr);

        // Extract next value from input (comma-separated)
        char valBuf[MAX_STR_LEN];
        int valLen = 0;
        // Skip leading spaces
        while (*inputPtr == ' ')
        {
          inputPtr++;
        }
        // Read until comma or end
        while (*inputPtr != '\0' && *inputPtr != ',' &&
               valLen < (int)sizeof(valBuf) - 1)
        {
          valBuf[valLen++] = *inputPtr++;
        }
        valBuf[valLen] = '\0';
        // Trim trailing spaces
        while (valLen > 0 && valBuf[valLen - 1] == ' ')
        {
          valBuf[--valLen] = '\0';
        }
        // Skip comma
        if (*inputPtr == ',')
        {
          inputPtr++;
        }

        // Store value
        if (isStr)
        {
          m_vars.setStr(vname, vlen, valBuf);
        }
        else
        {
          m_vars.setNum(vname, vlen, strtof(valBuf, NULL));
        }
      }
      else
      {
        pos++;
      }
    }
  }

  // Find a variable on the FOR stack, returns index or -1
  int findForVar(const char* name, int nameLen)
  {
    for (int i = m_forDepth - 1; i >= 0; i--)
    {
      if ((int)strlen(m_forStack[i].varName) == nameLen &&
          strncasecmp(m_forStack[i].varName, name, nameLen) == 0)
      {
        return i;
      }
    }
    return -1;
  }
};

#endif // TOKEN_PARSER_H
