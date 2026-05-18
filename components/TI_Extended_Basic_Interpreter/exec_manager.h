/*
 * TI BASIC Interpreter — Execution Manager (EM)
 *
 * Manages program flow by feeding lines to the Token Parser
 * and responding to its flow control return values.
 *
 * Responsibilities:
 *   - Program storage (sorted array of tokenized lines)
 *   - RUN: iterate through lines, dispatch to TP
 *   - Handle GOTO, GOSUB, FOR/NEXT flow control
 *   - Handle BREAK (user interrupt)
 */

#ifndef EXEC_MANAGER_H
#define EXEC_MANAGER_H

#include "tp_types.h"
#include "token_parser.h"
#include "ti_platform.h"   // tiYield() — host's cooperative-yield primitive
#include <Arduino.h>

// Forward declaration for display output
typedef void (*PrintLineFn)(const char* str);
typedef void (*PrintErrorFn)(const char* str);
typedef void (*PrintStringFn2)(const char* str);
typedef bool (*GetLineFn)(char* buf, int bufSize);
typedef void (*ProgramEndedFn)();
typedef void (*PrepareInputFn)();

class ExecManager
{
public:
  ExecManager() : m_programSize(0), m_printLine(NULL), m_printError(NULL),
                  m_printString(NULL), m_getLine(NULL),
                  m_trace(false), m_breakpointCount(0) {}

  void setPrintLine(PrintLineFn fn) { m_printLine = fn; }
  void setPrintError(PrintErrorFn fn) { m_printError = fn; }
  void setPrintString(PrintStringFn2 fn) { m_printString = fn; }
  void setGetLine(GetLineFn fn) { m_getLine = fn; }
  void setProgramEnded(ProgramEndedFn fn) { m_programEnded = fn; }
  void setPrepareInput(PrepareInputFn fn) { m_prepareInput = fn; }
  // Optional per-iteration tick — called after every line so things
  // like the sprite layer can integrate motion while RUN is busy.
  typedef void (*PerLineTickFn)();
  void setPerLineTick(PerLineTickFn fn) { m_perLineTick = fn; }
  TokenParser* tp() { return &m_tp; }

  // --- Tracing (TRACE / UNTRACE) ---
  void setTrace(bool on) { m_trace = on; }

  // --- Breakpoints (BREAK / UNBREAK) ---
  static const int MAX_BREAKPOINTS = 16;
  void addBreakpoint(int lineNum)
  {
    for (int i = 0; i < m_breakpointCount; i++)
    {
      if (m_breakpoints[i] == lineNum) return;
    }
    if (m_breakpointCount < MAX_BREAKPOINTS)
    {
      m_breakpoints[m_breakpointCount++] = lineNum;
    }
  }
  void removeBreakpoint(int lineNum)
  {
    for (int i = 0; i < m_breakpointCount; i++)
    {
      if (m_breakpoints[i] == lineNum)
      {
        for (int j = i; j < m_breakpointCount - 1; j++)
        {
          m_breakpoints[j] = m_breakpoints[j + 1];
        }
        m_breakpointCount--;
        return;
      }
    }
  }
  void clearBreakpoints() { m_breakpointCount = 0; }
  bool hasBreakpoint(int lineNum) const
  {
    for (int i = 0; i < m_breakpointCount; i++)
    {
      if (m_breakpoints[i] == lineNum) return true;
    }
    return false;
  }

  // --- Program storage ---

  int programSize() const { return m_programSize; }

  ProgramLine* getLine(int idx)
  {
    return (idx >= 0 && idx < m_programSize) ? m_program[idx] : NULL;
  }

  // Find index where lineNum is or should be inserted
  int findLineIndex(uint16_t lineNum)
  {
    int lo = 0, hi = m_programSize - 1;
    while (lo <= hi)
    {
      int mid = (lo + hi) / 2;
      if (m_program[mid]->lineNum == lineNum)
      {
        return mid;
      }
      if (m_program[mid]->lineNum < lineNum)
      {
        lo = mid + 1;
      }
      else
      {
        hi = mid - 1;
      }
    }
    return lo;
  }

  bool storeLine(uint16_t lineNum, const uint8_t* tokens, int length)
  {
    int idx = findLineIndex(lineNum);

    // Replace existing
    if (idx < m_programSize && m_program[idx]->lineNum == lineNum)
    {
      memcpy(m_program[idx]->tokens, tokens, length);
      m_program[idx]->length = length;
      return true;
    }

    // Insert new
    if (m_programSize >= MAX_LINES)
    {
      return false;
    }

    ProgramLine* line = (ProgramLine*)malloc(sizeof(ProgramLine));
    if (!line)
    {
      return false;
    }

    line->lineNum = lineNum;
    memcpy(line->tokens, tokens, length);
    line->length = length;

    for (int i = m_programSize; i > idx; i--)
    {
      m_program[i] = m_program[i - 1];
    }
    m_program[idx] = line;
    m_programSize++;
    return true;
  }

  bool deleteLine(uint16_t lineNum)
  {
    int idx = findLineIndex(lineNum);
    if (idx >= m_programSize || m_program[idx]->lineNum != lineNum)
    {
      return false;
    }

    free(m_program[idx]);
    for (int i = idx; i < m_programSize - 1; i++)
    {
      m_program[i] = m_program[i + 1];
    }
    m_programSize--;
    return true;
  }

  void clearProgram()
  {
    for (int i = 0; i < m_programSize; i++)
    {
      free(m_program[i]);
      m_program[i] = NULL;
    }
    m_programSize = 0;
    m_canContinue = false;
  }

  // --- Program execution ---

  // Run a single line of tokens in immediate mode (no line number)
  void runImmediate(const uint8_t* tokens, int length)
  {
    TPResponse resp = m_tp.processLine(tokens, length, 0);

    switch (resp.result)
    {
      case TP_NEED_INPUT:
        if (m_prepareInput && !resp.cursorPrePositioned)
        {
          m_prepareInput();
        }
        if (m_printString)
        {
          m_printString(resp.prompt);
        }
        {
          char inputBuf[MAX_STR_LEN];
          inputBuf[0] = '\0';
          if (m_getLine)
          {
            m_getLine(inputBuf, sizeof(inputBuf));
          }
          m_tp.provideInputValue(inputBuf);
        }
        break;

      case TP_ERROR:
        if (m_printError)
        {
          char buf[60];
          snprintf(buf, sizeof(buf), "* %s", resp.errorMsg);
          m_printError(buf);
        }
        break;

      default:
        break;
    }

    // Reset graphics after any immediate command (TI behavior)
    if (m_programEnded)
    {
      m_programEnded();
    }
  }

  // --- DATA support (called from TP) ---

  // Reset DATA pointer to start or to first DATA at/after lineNum.
  void resetData(uint16_t lineNum)
  {
    if (lineNum == 0)
    {
      m_dataLineIdx = 0;
    }
    else
    {
      m_dataLineIdx = findLineIndex(lineNum);
    }
    m_dataTokenPos = 0;
    m_inDataStmt = false;
  }

  // Get the next DATA value as text. Returns false if no more data.
  bool nextData(char* buf, int bufSize)
  {
    while (m_dataLineIdx < m_programSize)
    {
      ProgramLine* line = m_program[m_dataLineIdx];

      // Scan for DATA token, then collect values until EOL
      while (m_dataTokenPos < line->length &&
             line->tokens[m_dataTokenPos] != TOK_EOL)
      {
        uint8_t tok = line->tokens[m_dataTokenPos];

        if (!m_inDataStmt)
        {
          if (tok == TOK_DATA)
          {
            m_inDataStmt = true;
            m_dataTokenPos++;
            continue;
          }
          m_dataTokenPos++;
          continue;
        }

        // Inside DATA — collect next value
        if (tok == TOK_COMMA || tok == 0x20)
        {
          m_dataTokenPos++;
          continue;
        }

        if (tok == TOK_COLON || tok == TOK_EOL)
        {
          m_inDataStmt = false;
          m_dataTokenPos++;
          continue;
        }

        if (tok == TOK_QUOTED_STR || tok == TOK_UNQUOTED_STR)
        {
          m_dataTokenPos++;
          int slen = line->tokens[m_dataTokenPos++];
          int copyLen = (slen < bufSize - 1) ? slen : bufSize - 1;
          memcpy(buf, &line->tokens[m_dataTokenPos], copyLen);
          buf[copyLen] = '\0';
          m_dataTokenPos += slen;
          return true;
        }

        // Skip anything else
        m_dataTokenPos++;
      }

      // End of line — move to next
      m_dataLineIdx++;
      m_dataTokenPos = 0;
      m_inDataStmt = false;
    }
    return false;
  }

  // Static callback adapters (for callback registration)
  static ExecManager* s_instance;
  static bool cbNextData(char* buf, int bufSize)
  {
    return s_instance ? s_instance->nextData(buf, bufSize) : false;
  }
  static void cbResetData(uint16_t lineNum)
  {
    if (s_instance) s_instance->resetData(lineNum);
  }

  void run()
  {
    if (m_programSize == 0)
    {
      if (m_printError) m_printError("* NO PROGRAM PRESENT");
      return;
    }

    // Install self-reference for data callbacks
    s_instance = this;
    m_tp.setDataCallbacks(cbNextData, cbResetData);

    int lineIdx;
    if (m_continueNext)
    {
      // CONTINUE: resume from saved state; DON'T reset vars, subs, DATA
      lineIdx = m_resumeLineIdx;
      m_continueNext = false;
    }
    else
    {
      // Fresh RUN: full reset
      resetData(0);
      m_tp.reset();
      scanForSubs();
      m_subDepth = 0;
      lineIdx = 0;
    }
    m_canContinue = false;
    m_running = true;

    // Drain any leftover characters in the serial buffer
    while (Serial.available())
    {
      Serial.read();
    }

    while (m_running && lineIdx >= 0 && lineIdx < m_programSize)
    {
      ProgramLine* line = m_program[lineIdx];

      // SUB block skip: in main flow (depth 0), a SUB declaration means
      // skip the whole subprogram body. Subs only run when CALLed. The
      // corresponding SUBEND is located in the sub table built at run start.
      if (m_subDepth == 0 && line->length > 0 &&
          line->tokens[0] == TOK_SUB)
      {
        // Find the matching end index from the sub table
        int skipTo = m_programSize;
        for (int s = 0; s < m_tp.vars()->subCount(); s++)
        {
          SubDef* sd = m_tp.vars()->subAt(s);
          if (sd && sd->declLineIdx == lineIdx)
          {
            skipTo = sd->endLineIdx + 1;
            break;
          }
        }
        lineIdx = skipTo;
        continue;
      }

      // TRACE: print the line number before executing it
      if (m_trace && m_printString)
      {
        char buf[12];
        snprintf(buf, sizeof(buf), "<%d>", line->lineNum);
        m_printString(buf);
      }

      // BREAKPOINT: stop before executing a line that's in the list.
      // Right after CONTINUE we skip the check once so we don't re-pause
      // on the same line that halted us.
      if (m_skipBreakOnce)
      {
        m_skipBreakOnce = false;
      }
      else if (hasBreakpoint(line->lineNum))
      {
        if (m_printLine)
        {
          char buf[40];
          m_printLine("");
          snprintf(buf, sizeof(buf), "* BREAKPOINT AT %d", line->lineNum);
          m_printLine(buf);
          m_printLine("");
          Serial.write(0x07);
        }
        m_resumeLineIdx = lineIdx;   // re-run the breakpointed line on CON
        m_canContinue = true;
        m_running = false;
        break;
      }

      TPResponse resp = m_tp.processLine(
        line->tokens, line->length, line->lineNum);

      lineIdx = handleResponse(resp, lineIdx);

      // Check for user break — ESC (0x1B), Ctrl+C (0x03), or TI CLEAR (12),
      // from either Serial or the BLE keyboard. Use peek on Serial so other
      // keys remain available for CALL KEY; the BLE path sets a flag from
      // the notify callback because bleKbPeek would race with reads.
      bool doBreak = false;
      if (Serial.available())
      {
        int c = Serial.peek();
        if (c == 0x1B || c == 0x03 || c == 12)
        {
          Serial.read();  // consume
          doBreak = true;
        }
      }
      if (bleKbBreakRequested)
      {
        bleKbBreakRequested = false;
        // Also drain the matching byte from the BLE ring buffer so it
        // doesn't later look like a keypress for CALL KEY.
        if (bleKbAvailable())
        {
          int c = bleKbPeek();
          if (c == 0x1B || c == 0x03 || c == 12)
          {
            bleKbRead();
          }
        }
        doBreak = true;
      }
      if (doBreak)
      {
        // ON BREAK NEXT: swallow the break and keep executing
        if (m_tp.onBreakMode() == OB_NEXT)
        {
          // fall through without halting
        }
        else
        {
          if (m_printLine)
          {
            m_printLine("");
            char buf[40];
            snprintf(buf, sizeof(buf), "* BREAKPOINT AT %d",
                     line->lineNum);
            m_printLine(buf);
          }
          if (lineIdx >= 0)
          {
            m_resumeLineIdx = lineIdx;
            m_canContinue = true;
          }
          m_running = false;
        }
      }

      if (m_perLineTick) m_perLineTick();
      if (m_throttleUs > 0)
      {
        // Bigger throttles use delay() (yields to BLE / watchdog);
        // smaller use delayMicroseconds() for finer-grained pacing.
        if (m_throttleUs >= 1000)
        {
          delay(m_throttleUs / 1000);
        }
        else
        {
          delayMicroseconds(m_throttleUs);
        }
      }
      tiYield();  // Real yield: blocks 1 tick so IDLE can run + reset WDT
    }

    if (!m_running || lineIdx >= m_programSize)
    {
      // Reset graphics (colors, char patterns) to editor defaults.
      // Screen content is preserved — only colors/patterns change.
      if (m_programEnded)
      {
        m_programEnded();
      }
      if (m_printLine)
      {
        m_printLine("");
        m_printLine("* READY *");
      }
    }
  }

private:
  ProgramLine* m_program[MAX_LINES];
  int m_programSize;
  bool m_running = false;

  bool m_trace;
  int  m_breakpoints[MAX_BREAKPOINTS];
  int  m_breakpointCount;

public:
  // Per-line throttle (microseconds). 0 = unthrottled (default), our
  // native ESP32 speed which is ~30x a real TI. Settable via
  // CALL SPEED(n). Useful values:
  //   285  ≈ TI Extended BASIC native speed
  //   666  ≈ stock TI BASIC native speed
  //   1000 = 1000 statements/sec — generic "slow"
  unsigned long m_throttleUs = 0;
private:

  // CONTINUE state. m_canContinue is set when STOP, breakpoint, or user
  // BREAK halts the program. m_continueNext tells run() to resume from
  // m_resumeLineIdx without resetting vars/subs/DATA. m_skipBreakOnce
  // suppresses the breakpoint check for the first iteration after
  // CONTINUE so we don't immediately re-pause on the line that already
  // halted us.
  bool m_canContinue = false;
  bool m_continueNext = false;
  bool m_skipBreakOnce = false;
  int  m_resumeLineIdx = 0;

public:
  void cont()
  {
    if (!m_canContinue)
    {
      if (m_printError) m_printError("* CAN'T CONTINUE");
      return;
    }
    m_continueNext = true;
    m_skipBreakOnce = true;
    run();
  }

  void clearContinueState() { m_canContinue = false; }
private:

  // CALL sub(...) return stack. Each entry is the line index to resume
  // at when SUBEND/SUBEXIT pops, plus pass-by-value-result aliases for
  // bare-variable arguments. Depth > 0 means we're currently inside a
  // subprogram; main flow is depth 0.
  struct SubFrame
  {
    int        returnLineIdx;
    TPSubAlias aliases[6];
    uint8_t    aliasCount;
  };
  static const int MAX_SUB_DEPTH = 8;
  SubFrame m_subStack[MAX_SUB_DEPTH];
  int m_subDepth = 0;

public:
  void pushSubCall(int returnLineIdx)
  {
    if (m_subDepth < MAX_SUB_DEPTH)
    {
      m_subStack[m_subDepth].returnLineIdx = returnLineIdx;
      m_subStack[m_subDepth].aliasCount = 0;
      m_subDepth++;
    }
  }
  int popSubCall()
  {
    if (m_subDepth == 0) return -1;
    return m_subStack[--m_subDepth].returnLineIdx;
  }
  int subDepth() const { return m_subDepth; }

  // Scan the program for SUB declarations, build the sub table.
  // Called once per RUN.
  void scanForSubs()
  {
    m_tp.vars()->clearSubs();
    for (int i = 0; i < m_programSize; i++)
    {
      ProgramLine* pl = m_program[i];
      int pos = 0;
      if (pos >= pl->length) continue;
      if (pl->tokens[pos] != TOK_SUB) continue;

      // Parse the SUB declaration: SUB name(param-list)
      pos++;   // past TOK_SUB
      if (!isIdentStart(pl->tokens[pos])) continue;

      char sname[16];
      int nameLen = 0;
      while (pos < pl->length &&
             isIdentCont(pl->tokens[pos]) &&
             nameLen < 15)
      {
        sname[nameLen++] = (char)pl->tokens[pos++];
      }

      // Capture param-list tokens (between LPAREN and RPAREN). SUB with
      // no parens is allowed — leaves paramTokensLen = 0.
      uint8_t params[32];
      int paramLen = 0;
      if (pos < pl->length && pl->tokens[pos] == TOK_LPAREN)
      {
        pos++;
        while (pos < pl->length &&
               pl->tokens[pos] != TOK_RPAREN &&
               pl->tokens[pos] != TOK_EOL &&
               paramLen < (int)sizeof(params))
        {
          params[paramLen++] = pl->tokens[pos++];
        }
      }

      // Scan forward for matching SUBEND.
      int endIdx = -1;
      for (int j = i + 1; j < m_programSize; j++)
      {
        if (m_program[j]->length > 0 &&
            m_program[j]->tokens[0] == TOK_SUBEND)
        {
          endIdx = j;
          break;
        }
      }
      if (endIdx < 0) endIdx = m_programSize;   // unterminated = EOF

      m_tp.vars()->defineSub(sname, nameLen, i, endIdx, params, paramLen);
    }
  }

private:

  TokenParser m_tp;
  PrintLineFn m_printLine;
  PrintErrorFn m_printError;
  PrintStringFn2 m_printString;
  GetLineFn m_getLine;
  ProgramEndedFn m_programEnded = NULL;
  PrepareInputFn m_prepareInput = NULL;
  PerLineTickFn  m_perLineTick  = NULL;

  // DATA reading state
  int m_dataLineIdx = 0;
  int m_dataTokenPos = 0;
  bool m_inDataStmt = false;    // true when we've entered a DATA statement

  // Process a TPResponse and return the next line index
  int handleResponse(const TPResponse& resp, int currentIdx)
  {
    switch (resp.result)
    {
      case TP_NEXT_TOKEN:
        // Shouldn't reach here — TP should resolve within processLine
        return currentIdx + 1;

      case TP_NEXT_LINE:
        return currentIdx + 1;

      case TP_CALL_SUB:
      {
        // Push return address (line after the CALL) and jump to the
        // first line of the sub body (one past the SUB declaration).
        if (m_subDepth >= MAX_SUB_DEPTH)
        {
          if (m_printError) m_printError("* STACK OVERFLOW");
          m_running = false;
          return -1;
        }
        SubFrame& f = m_subStack[m_subDepth++];
        f.returnLineIdx = currentIdx + 1;
        f.aliasCount = resp.aliasCount;
        for (uint8_t i = 0; i < resp.aliasCount; i++)
        {
          f.aliases[i] = resp.aliases[i];
        }
        return resp.lineIdx + 1;
      }

      case TP_SUB_RETURN:
      {
        if (m_subDepth == 0)
        {
          if (m_printError) m_printError("* MUST BE IN SUBPROGRAM");
          m_running = false;
          return -1;
        }
        SubFrame& f = m_subStack[m_subDepth - 1];
        // Pass-by-value-result: copy each param's final value back to
        // the caller's variable.
        VarTable* vars = m_tp.vars();
        for (uint8_t i = 0; i < f.aliasCount; i++)
        {
          const TPSubAlias& a = f.aliases[i];
          if (a.isStr)
          {
            const char* s = vars->getStr(a.paramName, a.paramLen);
            vars->setStr(a.callerName, a.callerLen, s ? s : "");
          }
          else
          {
            float v = vars->getNum(a.paramName, a.paramLen);
            vars->setNum(a.callerName, a.callerLen, v);
          }
        }
        m_subDepth--;
        return f.returnLineIdx;
      }

      case TP_GOTO_LINE:
      {
        int idx = findLineIndex(resp.lineNum);
        if (idx < m_programSize && m_program[idx]->lineNum == resp.lineNum)
        {
          return idx;
        }
        if (m_printLine)
        {
          char buf[60];
          m_printLine("");
          snprintf(buf, sizeof(buf), "* LINE NOT FOUND IN %d",
                   m_program[currentIdx]->lineNum);
          m_printLine(buf);
          m_printLine("");
          Serial.write(0x07);
        }
        m_running = false;
        return -1;
      }

      case TP_GOTO_AFTER:
      {
        // Go to the line AFTER the specified line (used by RETURN)
        int idx = findLineIndex(resp.lineNum);
        if (idx < m_programSize && m_program[idx]->lineNum == resp.lineNum)
        {
          return idx + 1;
        }
        if (m_printError) m_printError("* RETURN WITHOUT GOSUB");
        m_running = false;
        return -1;
      }

      case TP_NEXT_LOOP:
      {
        int forIdx = findLineIndex(resp.lineNum);
        if (forIdx >= m_programSize ||
            m_program[forIdx]->lineNum != resp.lineNum)
        {
          if (m_printError) m_printError("* FOR-NEXT NESTING");
          m_running = false;
          return -1;
        }
        ProgramLine* forLine = m_program[forIdx];

        // Single-line FOR/NEXT (FOR and NEXT on same line). Each
        // re-feed runs ONE iteration; we keep re-feeding until the
        // loop ends so the body runs the right number of times.
        if (forIdx == currentIdx)
        {
          while (true)
          {
            TPResponse loopResp = m_tp.processLine(
              forLine->tokens, forLine->length,
              forLine->lineNum, true);
            if (loopResp.result == TP_END_LOOP) break;
            if (loopResp.result == TP_ERROR)
            {
              return handleResponse(loopResp, currentIdx);
            }
            // Same per-iteration housekeeping the outer loop does
            if (m_perLineTick) m_perLineTick();
            if (m_throttleUs > 0)
            {
              if (m_throttleUs >= 1000) delay(m_throttleUs / 1000);
              else                      delayMicroseconds(m_throttleUs);
            }
            tiYield();
          }
          return currentIdx + 1;
        }

        // Multi-line FOR/NEXT: re-feed once (FOR's increment + check),
        // and if continuing, jump to the body (forIdx + 1) and let the
        // outer loop drive successive iterations.
        TPResponse loopResp = m_tp.processLine(
          forLine->tokens, forLine->length,
          forLine->lineNum, true);
        if (loopResp.result == TP_END_LOOP) return currentIdx + 1;
        return forIdx + 1;
      }

      case TP_END_LOOP:
        // Shouldn't reach here directly — handled in NEXT_LOOP
        return currentIdx + 1;

      case TP_NEED_INPUT:
      {
        // Move cursor to bottom of screen (TI behavior) — UNLESS the
        // statement already positioned it (ACCEPT AT).
        if (m_prepareInput && !resp.cursorPrePositioned)
        {
          m_prepareInput();
        }
        // Display the prompt
        if (m_printString)
        {
          m_printString(resp.prompt);
        }
        // Get input from user (blocking)
        char inputBuf[MAX_STR_LEN];
        inputBuf[0] = '\0';
        if (m_getLine)
        {
          m_getLine(inputBuf, sizeof(inputBuf));
        }
        // Feed input to TP
        m_tp.provideInputValue(inputBuf);
        return currentIdx + 1;
      }

      case TP_FINISHED:
        m_running = false;
        return -1;

      case TP_STOPPED:
        // STOP: allow CONTINUE to resume at the line after STOP.
        m_resumeLineIdx = currentIdx + 1;
        m_canContinue = true;
        m_running = false;
        if (m_printLine)
        {
          char buf[32];
          m_printLine("");
          snprintf(buf, sizeof(buf), "* STOPPED AT %d",
                   m_program[currentIdx]->lineNum);
          m_printLine(buf);
        }
        return -1;

      case TP_WARNING:
      {
        OnWarningMode wm = m_tp.onWarningMode();
        if (wm == OW_NEXT)
        {
          return currentIdx + 1;   // silent — value already substituted
        }
        if (wm == OW_PRINT)
        {
          if (m_printLine)
          {
            char buf[80];
            snprintf(buf, sizeof(buf), "* %s IN %d",
                     resp.errorMsg, m_program[currentIdx]->lineNum);
            m_printLine(buf);
          }
          return currentIdx + 1;
        }
        // OW_STOP: promote to error — fall through to TP_ERROR path.
        TPResponse upgraded = resp;
        upgraded.result = TP_ERROR;
        return handleResponse(upgraded, currentIdx);
      }

      case TP_ERROR:
      {
        uint16_t errLine = m_program[currentIdx]->lineNum;
        m_tp.setLastErrorLine(errLine);
        OnErrorMode em = m_tp.onErrorMode();

        if (em == OE_GOTO)
        {
          uint16_t target = m_tp.onErrorLine();
          int idx = findLineIndex(target);
          if (idx < m_programSize && m_program[idx]->lineNum == target)
          {
            // Reset handler so a re-error inside the handler doesn't
            // loop forever. Handler can re-arm ON ERROR if desired.
            m_tp.disarmOnError();
            return idx;
          }
          // Handler line missing — fall through to default STOP path
        }
        if (em == OE_NEXT)
        {
          return currentIdx + 1;
        }

        if (m_printLine)
        {
          char buf[80];
          m_printLine("");
          snprintf(buf, sizeof(buf), "* %s IN %d",
                   resp.errorMsg, errLine);
          m_printLine(buf);
          m_printLine("");
          Serial.write(0x07);   // BEL
        }
        m_running = false;
        return -1;
      }

      default:
        return currentIdx + 1;
    }
  }
};

// Static member definition
inline ExecManager* ExecManager::s_instance = NULL;

#endif // EXEC_MANAGER_H
