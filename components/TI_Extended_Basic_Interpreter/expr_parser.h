/*
 * TI BASIC Interpreter — Expression Parser
 *
 * Recursive descent parser for numeric and string expressions.
 * Handles operator precedence: () > unary- > ^ > * / > + - > comparisons
 *
 * Called by the Token Parser when an expression needs evaluation.
 * Reads tokens from the token stream and advances the position.
 */

#ifndef EXPR_PARSER_H
#define EXPR_PARSER_H

#include "tp_types.h"
#include "var_table.h"
#include <math.h>
#include <stdlib.h>

class ExprParser
{
public:
  ExprParser(VarTable* vars) : m_vars(vars) {}

  // File I/O hook — only EOF() uses it. Wired by TokenParser.
  typedef bool (*FileEofFn)(int unit);
  void setFileEof(FileEofFn f) { m_fileEof = f; }

  // --- Diagnostic signaling (warnings + errors) ---
  enum Severity : uint8_t { DS_NONE = 0, DS_WARNING = 1, DS_ERROR = 2 };

  bool hasError()    const { return m_severity == DS_ERROR; }
  bool hasWarning()  const { return m_severity == DS_WARNING; }
  bool hasDiag()     const { return m_severity != DS_NONE; }
  Severity severity() const { return m_severity; }
  const char* errorMsg() const { return m_errorMsg; }
  void clearError() { m_severity = DS_NONE; m_errorMsg[0] = '\0'; }

  void setError(const char* msg)   { recordDiag(DS_ERROR, msg); }
  void setWarning(const char* msg) { recordDiag(DS_WARNING, msg); }

private:
  void recordDiag(Severity sev, const char* msg)
  {
    // Errors supersede warnings; first of each severity wins.
    if (sev <= m_severity && m_severity != DS_NONE) return;
    m_severity = sev;
    int n = 0;
    while (msg[n] && n < (int)sizeof(m_errorMsg) - 1)
    {
      m_errorMsg[n] = msg[n];
      n++;
    }
    m_errorMsg[n] = '\0';
  }

public:

  // Evaluate a numeric expression, advancing pos through the token stream
  float evalNumeric(const uint8_t* tokens, int* pos)
  {
    return evalComparison(tokens, pos);
  }

  // Evaluate a boolean condition (for IF statements)
  bool evalCondition(const uint8_t* tokens, int* pos)
  {
    float val = evalComparison(tokens, pos);
    return val != 0;
  }

  // Evaluate a string expression: literals, variables, & concatenation,
  // and string functions (SEG$, CHR$, STR$, RPT$)
  void evalString(const uint8_t* tokens, int* pos, char* buf, int bufSize)
  {
    buf[0] = '\0';
    int outLen = 0;

    evalStringTerm(tokens, pos, buf, bufSize);
    outLen = strlen(buf);

    while (tokens[*pos] == TOK_CONCAT)
    {
      (*pos)++;
      char tmp[MAX_STR_LEN];
      evalStringTerm(tokens, pos, tmp, sizeof(tmp));
      int tmpLen = strlen(tmp);
      if (outLen + tmpLen < bufSize - 1)
      {
        strcat(buf, tmp);
        outLen += tmpLen;
      }
    }
  }

private:
  VarTable* m_vars;
  Severity m_severity = DS_NONE;
  char     m_errorMsg[40] = {0};
  FileEofFn m_fileEof = NULL;

  // --- String term evaluator ---

  void evalStringTerm(const uint8_t* tokens, int* pos, char* buf, int bufSize)
  {
    buf[0] = '\0';
    uint8_t tok = tokens[*pos];

    // String literal
    if (tok == TOK_STRING_LIT)
    {
      (*pos)++;
      int slen = tokens[*pos];
      (*pos)++;
      int copyLen = (slen < bufSize - 1) ? slen : bufSize - 1;
      memcpy(buf, &tokens[*pos], copyLen);
      buf[copyLen] = '\0';
      *pos += slen;
      return;
    }

    // Identifier (variable or function call)
    if (isIdentStart(tok))
    {
      char fname[16];
      int peekLen = peekIdent(tokens, *pos, fname, sizeof(fname));
      bool isFunctionCall = (tokens[*pos + peekLen] == TOK_LPAREN);

      // User-defined string function? Name ends with "$" (TI's marker for
      // string-returning identifiers). TI Extended BASIC accepts any name
      // for DEF — the "FN" prefix is a Microsoft/GW-BASIC convention that
      // does not apply here.
      if (isFunctionCall && peekLen >= 2 &&
          fname[peekLen - 1] == '$')
      {
        FnDef* fn = m_vars->findFn(fname, peekLen);
        if (fn != NULL && fn->isStringFn)
        {
          *pos += peekLen;
          if (tokens[*pos] == TOK_LPAREN) (*pos)++;
          if (fn->isStringParam)
          {
            char argBuf[MAX_STR_LEN];
            evalString(tokens, pos, argBuf, sizeof(argBuf));
            m_vars->setStr(fn->param, fn->paramLen, argBuf);
          }
          else
          {
            float arg = evalNumeric(tokens, pos);
            m_vars->setNum(fn->param, fn->paramLen, arg);
          }
          if (tokens[*pos] == TOK_RPAREN) (*pos)++;
          int bodyPos = 0;
          evalString(fn->body, &bodyPos, buf, bufSize);
          return;
        }
      }

      if (isFunctionCall)
      {
        if (strcasecmp(fname, "CHR$") == 0)
        {
          *pos += peekLen;
          if (tokens[*pos] == TOK_LPAREN) (*pos)++;
          float val = evalNumeric(tokens, pos);
          if (tokens[*pos] == TOK_RPAREN) (*pos)++;
          buf[0] = (char)(int)val;
          buf[1] = '\0';
          return;
        }

        if (strcasecmp(fname, "STR$") == 0)
        {
          *pos += peekLen;
          if (tokens[*pos] == TOK_LPAREN) (*pos)++;
          float val = evalNumeric(tokens, pos);
          if (tokens[*pos] == TOK_RPAREN) (*pos)++;
          snprintf(buf, bufSize, "%g", val);
          return;
        }

        if (strcasecmp(fname, "SEG$") == 0)
        {
          *pos += peekLen;
          if (tokens[*pos] == TOK_LPAREN) (*pos)++;
          char srcBuf[MAX_STR_LEN];
          evalString(tokens, pos, srcBuf, sizeof(srcBuf));
          if (tokens[*pos] == TOK_COMMA) (*pos)++;
          int start = (int)evalNumeric(tokens, pos) - 1;
          if (tokens[*pos] == TOK_COMMA) (*pos)++;
          int segLen = (int)evalNumeric(tokens, pos);
          if (tokens[*pos] == TOK_RPAREN) (*pos)++;

          int srcLen = strlen(srcBuf);
          if (start < 0) start = 0;
          if (start >= srcLen)
          {
            buf[0] = '\0';
            return;
          }
          if (start + segLen > srcLen) segLen = srcLen - start;
          if (segLen >= bufSize) segLen = bufSize - 1;
          memcpy(buf, &srcBuf[start], segLen);
          buf[segLen] = '\0';
          return;
        }

        if (strcasecmp(fname, "RPT$") == 0)
        {
          *pos += peekLen;
          if (tokens[*pos] == TOK_LPAREN) (*pos)++;
          char srcBuf[MAX_STR_LEN];
          evalString(tokens, pos, srcBuf, sizeof(srcBuf));
          if (tokens[*pos] == TOK_COMMA) (*pos)++;
          int count = (int)evalNumeric(tokens, pos);
          if (tokens[*pos] == TOK_RPAREN) (*pos)++;

          int srcLen = strlen(srcBuf);
          int outLen = 0;
          for (int i = 0; i < count; i++)
          {
            if (outLen + srcLen >= bufSize - 1) break;
            strcpy(&buf[outLen], srcBuf);
            outLen += srcLen;
          }
          return;
        }
      }

      // Not a function — treat as string variable or string array
      char name[16];
      bool isStr;
      int vlen = parseIdent(tokens, pos, name, sizeof(name), &isStr);

      // Array access A$(i) or A$(i,j)
      if (tokens[*pos] == TOK_LPAREN && isStr)
      {
        (*pos)++;
        int indices[MAX_ARRAY_DIMS];
        int nIdx = 0;
        while (nIdx < MAX_ARRAY_DIMS && tokens[*pos] != TOK_RPAREN &&
               tokens[*pos] != TOK_EOL)
        {
          indices[nIdx++] = (int)evalComparison(tokens, pos);
          if (tokens[*pos] == TOK_COMMA) (*pos)++;
        }
        if (tokens[*pos] == TOK_RPAREN) (*pos)++;

        const char* val = m_vars->getArrayStr(name, vlen, indices, nIdx);
        strncpy(buf, val, bufSize - 1);
        buf[bufSize - 1] = '\0';
        return;
      }

      if (isStr)
      {
        const char* val = m_vars->getStr(name, vlen);
        strncpy(buf, val, bufSize - 1);
        buf[bufSize - 1] = '\0';
      }
      else
      {
        buf[0] = '\0';
      }
      return;
    }

    // Parenthesized string expression
    if (tok == TOK_LPAREN)
    {
      (*pos)++;
      evalString(tokens, pos, buf, bufSize);
      if (tokens[*pos] == TOK_RPAREN) (*pos)++;
      return;
    }

    buf[0] = '\0';
  }

  // Logical OR (lowest precedence of the logical ops)
  float evalComparison(const uint8_t* tokens, int* pos)
  {
    float left = evalAnd(tokens, pos);
    while (tokens[*pos] == TOK_OR)
    {
      (*pos)++;
      float right = evalAnd(tokens, pos);
      left = (left != 0 || right != 0) ? -1 : 0;
    }
    return left;
  }

  // Logical AND (precedence above OR)
  float evalAnd(const uint8_t* tokens, int* pos)
  {
    float left = evalXor(tokens, pos);
    while (tokens[*pos] == TOK_AND)
    {
      (*pos)++;
      float right = evalXor(tokens, pos);
      left = (left != 0 && right != 0) ? -1 : 0;
    }
    return left;
  }

  // Logical XOR (precedence above AND)
  float evalXor(const uint8_t* tokens, int* pos)
  {
    float left = evalRelational(tokens, pos);
    while (tokens[*pos] == TOK_XOR)
    {
      (*pos)++;
      float right = evalRelational(tokens, pos);
      left = ((left != 0) ^ (right != 0)) ? -1 : 0;
    }
    return left;
  }

  // Relational: expr (= <> < > <= >=) expr
  //
  // TI encodes compound ops as two tokens:
  //   <=  →  TOK_LESS + TOK_EQUAL
  //   >=  →  TOK_GREATER + TOK_EQUAL
  //   <>  →  TOK_LESS + TOK_GREATER
  enum CmpOp { CMP_NONE, CMP_EQ, CMP_NE, CMP_LT, CMP_GT, CMP_LE, CMP_GE };

  float evalRelational(const uint8_t* tokens, int* pos)
  {
    float left = evalAddSub(tokens, pos);

    uint8_t tok  = tokens[*pos];
    uint8_t tok2 = tokens[(*pos) + 1];
    CmpOp op = CMP_NONE;
    int consumed = 0;

    if (tok == TOK_LESS && tok2 == TOK_EQUAL)       { op = CMP_LE; consumed = 2; }
    else if (tok == TOK_LESS && tok2 == TOK_GREATER){ op = CMP_NE; consumed = 2; }
    else if (tok == TOK_GREATER && tok2 == TOK_EQUAL){op = CMP_GE; consumed = 2; }
    else if (tok == TOK_EQUAL)                      { op = CMP_EQ; consumed = 1; }
    else if (tok == TOK_LESS)                       { op = CMP_LT; consumed = 1; }
    else if (tok == TOK_GREATER)                    { op = CMP_GT; consumed = 1; }

    if (op == CMP_NONE) return left;

    *pos += consumed;
    float right = evalAddSub(tokens, pos);
    switch (op)
    {
      case CMP_EQ: return (left == right) ? -1 : 0;
      case CMP_NE: return (left != right) ? -1 : 0;
      case CMP_LT: return (left <  right) ? -1 : 0;
      case CMP_GT: return (left >  right) ? -1 : 0;
      case CMP_LE: return (left <= right) ? -1 : 0;
      case CMP_GE: return (left >= right) ? -1 : 0;
      default: break;
    }
    return left;
  }

  // Addition and subtraction
  float evalAddSub(const uint8_t* tokens, int* pos)
  {
    float val = evalMulDiv(tokens, pos);

    while (true)
    {
      uint8_t tok = tokens[*pos];
      if (tok == TOK_PLUS)
      {
        (*pos)++;
        val += evalMulDiv(tokens, pos);
      }
      else if (tok == TOK_MINUS)
      {
        (*pos)++;
        val -= evalMulDiv(tokens, pos);
      }
      else
      {
        break;
      }
    }
    return val;
  }

  // Multiplication and division
  float evalMulDiv(const uint8_t* tokens, int* pos)
  {
    float val = evalPower(tokens, pos);

    while (true)
    {
      uint8_t tok = tokens[*pos];
      if (tok == TOK_MULTIPLY)
      {
        (*pos)++;
        val *= evalPower(tokens, pos);
      }
      else if (tok == TOK_DIVIDE)
      {
        (*pos)++;
        float divisor = evalPower(tokens, pos);
        if (divisor == 0)
        {
          // TI XB: divide-by-zero is a warning. Substitute ±MAX so the
          // program can keep running if ON WARNING != STOP.
          setWarning("WARNING: NUMBER TOO BIG");
          val = (val < 0) ? -1.4e38f : 1.4e38f;
        }
        else
        {
          val /= divisor;
        }
      }
      else
      {
        break;
      }
    }
    return val;
  }

  // Exponentiation (right-associative)
  float evalPower(const uint8_t* tokens, int* pos)
  {
    float val = evalUnary(tokens, pos);

    if (tokens[*pos] == TOK_POWER)
    {
      (*pos)++;
      val = pow(val, evalPower(tokens, pos));
    }
    return val;
  }

  // Unary minus, NOT
  float evalUnary(const uint8_t* tokens, int* pos)
  {
    if (tokens[*pos] == TOK_MINUS)
    {
      (*pos)++;
      return -evalUnary(tokens, pos);
    }
    if (tokens[*pos] == TOK_NOT)
    {
      (*pos)++;
      float val = evalUnary(tokens, pos);
      return (val == 0) ? -1 : 0;
    }
    return evalFactor(tokens, pos);
  }

  // Factor: number, variable, (expr), function call
  float evalFactor(const uint8_t* tokens, int* pos)
  {
    uint8_t tok = tokens[*pos];

    // Parenthesized expression
    if (tok == TOK_LPAREN)
    {
      (*pos)++;
      float val = evalComparison(tokens, pos);
      if (tokens[*pos] == TOK_RPAREN)
      {
        (*pos)++;
      }
      return val;
    }

    // Number literal (TOK_UNQUOTED_STR + length + ASCII digits)
    if (tok == TOK_UNQUOTED_STR)
    {
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

    // Identifier — numeric variable, string variable (returns 0), or function
    if (isIdentStart(tok))
    {
      char fname[16];
      int peekLen = peekIdent(tokens, *pos, fname, sizeof(fname));
      bool isFunctionCall = (tokens[*pos + peekLen] == TOK_LPAREN);

      // Constants and zero-arg functions that work without parens
      if (strcasecmp(fname, "PI") == 0)
      {
        *pos += peekLen;
        if (tokens[*pos] == TOK_LPAREN)
        {
          (*pos)++;
          if (tokens[*pos] == TOK_RPAREN) (*pos)++;
        }
        return (float)M_PI;
      }
      if (strcasecmp(fname, "RND") == 0)
      {
        *pos += peekLen;
        if (tokens[*pos] == TOK_LPAREN)
        {
          (*pos)++;
          if (tokens[*pos] == TOK_RPAREN) (*pos)++;
        }
        return (float)random(0, 32767) / 32767.0f;
      }

      // User-defined function call. TI Extended BASIC accepts any name
      // for DEF (e.g. DEF M(X)=...), so we look up by the bare identifier
      // — no "FN" prefix required. If the name isn't a registered DEF,
      // fall through and the caller treats it as an array reference or
      // an undefined variable.
      if (isFunctionCall)
      {
        FnDef* fn = m_vars->findFn(fname, peekLen);
        if (fn != NULL)
        {
          *pos += peekLen;
          if (tokens[*pos] == TOK_LPAREN) (*pos)++;

          // Evaluate argument and bind to parameter variable. TI semantics
          // leak the parameter into the caller's scope — we match that.
          if (fn->isStringParam)
          {
            char argBuf[MAX_STR_LEN];
            evalString(tokens, pos, argBuf, sizeof(argBuf));
            m_vars->setStr(fn->param, fn->paramLen, argBuf);
          }
          else
          {
            float arg = evalNumeric(tokens, pos);
            m_vars->setNum(fn->param, fn->paramLen, arg);
          }
          if (tokens[*pos] == TOK_RPAREN) (*pos)++;

          // Evaluate the saved body as a complete expression.
          int bodyPos = 0;
          float result;
          if (fn->isStringFn)
          {
            // Numeric factor context — evaluate as numeric (callers using
            // FN$ in string ctx use a separate path, not implemented yet)
            result = 0;   // TODO: string-returning FNs in numeric context
          }
          else
          {
            result = evalNumeric(fn->body, &bodyPos);
          }
          return result;
        }
      }

      if (isFunctionCall)
      {
        // Numeric functions taking a numeric argument
        if (strcasecmp(fname, "ABS") == 0 || strcasecmp(fname, "INT") == 0 ||
            strcasecmp(fname, "SGN") == 0 || strcasecmp(fname, "SIN") == 0 ||
            strcasecmp(fname, "COS") == 0 || strcasecmp(fname, "TAN") == 0 ||
            strcasecmp(fname, "ATN") == 0 || strcasecmp(fname, "SQR") == 0 ||
            strcasecmp(fname, "LOG") == 0 || strcasecmp(fname, "EXP") == 0 ||
            strcasecmp(fname, "RND") == 0)
        {
          *pos += peekLen;
          float arg = 0;
          if (tokens[*pos] == TOK_LPAREN)
          {
            (*pos)++;
            if (tokens[*pos] != TOK_RPAREN)
            {
              arg = evalComparison(tokens, pos);
            }
            if (tokens[*pos] == TOK_RPAREN) (*pos)++;
          }
          if (strcasecmp(fname, "ABS") == 0) return fabs(arg);
          if (strcasecmp(fname, "INT") == 0) return floor(arg);
          if (strcasecmp(fname, "SGN") == 0)
            return (arg > 0) ? 1 : (arg < 0 ? -1 : 0);
          if (strcasecmp(fname, "SIN") == 0) return sin(arg);
          if (strcasecmp(fname, "COS") == 0) return cos(arg);
          if (strcasecmp(fname, "TAN") == 0) return tan(arg);
          if (strcasecmp(fname, "ATN") == 0) return atan(arg);
          if (strcasecmp(fname, "SQR") == 0) return sqrt(arg);
          if (strcasecmp(fname, "LOG") == 0) return log(arg);
          if (strcasecmp(fname, "EXP") == 0) return exp(arg);
          if (strcasecmp(fname, "RND") == 0)
            return (float)random(0, 32767) / 32767.0f;
        }

        // EOF(n) or EOF(#n) — end-of-file test for an open unit
        if (strcasecmp(fname, "EOF") == 0)
        {
          *pos += peekLen;
          if (tokens[*pos] == TOK_LPAREN) (*pos)++;
          if (tokens[*pos] == TOK_HASH) (*pos)++;
          int unit = (int)evalComparison(tokens, pos);
          if (tokens[*pos] == TOK_RPAREN) (*pos)++;
          return (m_fileEof && m_fileEof(unit)) ? -1.0f : 0.0f;
        }

        // MAX(a,b), MIN(a,b) — two arguments
        if (strcasecmp(fname, "MAX") == 0 || strcasecmp(fname, "MIN") == 0)
        {
          *pos += peekLen;
          if (tokens[*pos] == TOK_LPAREN) (*pos)++;
          float a = evalComparison(tokens, pos);
          if (tokens[*pos] == TOK_COMMA) (*pos)++;
          float b = evalComparison(tokens, pos);
          if (tokens[*pos] == TOK_RPAREN) (*pos)++;
          if (strcasecmp(fname, "MAX") == 0) return (a > b) ? a : b;
          return (a < b) ? a : b;
        }

        // POS(haystack$, needle$, start) — find substring, 1-based
        if (strcasecmp(fname, "POS") == 0)
        {
          *pos += peekLen;
          if (tokens[*pos] == TOK_LPAREN) (*pos)++;
          char haystack[MAX_STR_LEN];
          evalString(tokens, pos, haystack, sizeof(haystack));
          if (tokens[*pos] == TOK_COMMA) (*pos)++;
          char needle[MAX_STR_LEN];
          evalString(tokens, pos, needle, sizeof(needle));
          int start = 1;
          if (tokens[*pos] == TOK_COMMA)
          {
            (*pos)++;
            start = (int)evalComparison(tokens, pos);
          }
          if (tokens[*pos] == TOK_RPAREN) (*pos)++;
          if (start < 1) start = 1;
          if (start > (int)strlen(haystack)) return 0;
          const char* found = strstr(&haystack[start - 1], needle);
          return found ? (float)(found - haystack + 1) : 0;
        }

        // Numeric functions taking a string argument
        if (strcasecmp(fname, "LEN") == 0 || strcasecmp(fname, "ASC") == 0 ||
            strcasecmp(fname, "VAL") == 0)
        {
          *pos += peekLen;
          char strBuf[MAX_STR_LEN];
          if (tokens[*pos] == TOK_LPAREN) (*pos)++;
          evalString(tokens, pos, strBuf, sizeof(strBuf));
          if (tokens[*pos] == TOK_RPAREN) (*pos)++;

          if (strcasecmp(fname, "LEN") == 0) return (float)strlen(strBuf);
          if (strcasecmp(fname, "ASC") == 0)
            return (strBuf[0] != '\0') ? (float)(uint8_t)strBuf[0] : 0;
          if (strcasecmp(fname, "VAL") == 0) return strtof(strBuf, NULL);
        }
      }

      // Not a function — treat as variable (possibly array access)
      char name[16];
      bool isStr;
      int vlen = parseIdent(tokens, pos, name, sizeof(name), &isStr);

      // Check for array access A(i) or A(i,j) or A(i,j,k)
      if (tokens[*pos] == TOK_LPAREN)
      {
        (*pos)++;
        int indices[MAX_ARRAY_DIMS];
        int nIdx = 0;
        while (nIdx < MAX_ARRAY_DIMS && tokens[*pos] != TOK_RPAREN &&
               tokens[*pos] != TOK_EOL)
        {
          indices[nIdx++] = (int)evalComparison(tokens, pos);
          if (tokens[*pos] == TOK_COMMA) (*pos)++;
        }
        if (tokens[*pos] == TOK_RPAREN) (*pos)++;

        if (isStr)
        {
          return 0;  // string array in numeric context
        }
        return m_vars->getArrayNum(name, vlen, indices, nIdx);
      }

      // Scalar variable
      if (isStr)
      {
        return 0;  // string variable in numeric context
      }
      return m_vars->getNum(name, vlen);
    }

    // Unknown — return 0 and advance
    (*pos)++;
    return 0;
  }
};

#endif // EXPR_PARSER_H
