/*
 * TI BASIC Interpreter — Variable Table
 *
 * Manages numeric and string variables (scalar and arrays).
 * String values are stored in a separate string pool.
 * Arrays up to 3 dimensions.
 */

#ifndef VAR_TABLE_H
#define VAR_TABLE_H

#include "tp_types.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// DEF FN single-line user-defined function. Body is a slice of the
// original tokenized program line, so we don't need a full re-parse at
// call time — just bind the parameter and call evalNumeric/evalString
// on the stored tokens.
struct FnDef
{
  char    name[16];
  uint8_t nameLen;
  char    param[16];
  uint8_t paramLen;
  bool    isStringFn;      // FNX$ returns a string
  bool    isStringParam;   // parameter is a string (like Y$)
  uint8_t body[128];
  uint8_t bodyLen;
};

// Extended-BASIC user-defined subprogram: SUB name(params) .. SUBEND.
// At RUN init, ExecManager scans the program for SUB declarations and
// records the name, the line index of the declaration itself, the line
// index of the matching SUBEND, and the parameter-list token slice
// (between the LPAREN and RPAREN of the declaration). CALL dispatch
// uses those bytes to know the parameter names.
struct SubDef
{
  char    name[16];
  uint8_t nameLen;
  uint16_t declLineIdx;    // index in program table of the SUB line
  uint16_t endLineIdx;     // index of the matching SUBEND
  uint8_t paramTokens[32]; // tokens between LPAREN and RPAREN
  uint8_t paramTokensLen;
};

class VarTable
{
public:
  void clear()
  {
    // Free all string values
    for (int i = 0; i < m_strCount; i++)
    {
      if (m_strings[i])
      {
        free(m_strings[i]);
        m_strings[i] = NULL;
      }
    }
    // Free all array data
    for (int i = 0; i < m_varCount; i++)
    {
      if (m_vars[i].arrayData)
      {
        free(m_vars[i].arrayData);
        m_vars[i].arrayData = NULL;
      }
    }
    m_varCount = 0;
    m_strCount = 0;
    m_fnCount = 0;           // DEFs cleared on NEW / RUN
  }

  // --- DEF FN storage ---

  static const int MAX_FNS = 16;

  // Define (or redefine) a function by name.
  // Returns false if the DEF table is full.
  bool defineFn(const char* name, int nameLen, bool isStringFn,
                const char* param, int paramLen, bool isStringParam,
                const uint8_t* body, int bodyLen)
  {
    if (nameLen > 15 || paramLen > 15) return false;
    if (bodyLen > (int)sizeof(m_fns[0].body)) return false;
    int slot = findFnSlot(name, nameLen);
    if (slot < 0)
    {
      if (m_fnCount >= MAX_FNS) return false;
      slot = m_fnCount++;
    }
    FnDef& d = m_fns[slot];
    memcpy(d.name, name, nameLen);
    d.nameLen = (uint8_t)nameLen;
    memcpy(d.param, param, paramLen);
    d.paramLen = (uint8_t)paramLen;
    d.isStringFn = isStringFn;
    d.isStringParam = isStringParam;
    memcpy(d.body, body, bodyLen);
    d.bodyLen = (uint8_t)bodyLen;
    return true;
  }

  FnDef* findFn(const char* name, int nameLen)
  {
    int slot = findFnSlot(name, nameLen);
    return (slot >= 0) ? &m_fns[slot] : NULL;
  }

  // --- SUB subprogram storage ---

  static const int MAX_SUBS = 16;

  void clearSubs() { m_subCount = 0; }

  bool defineSub(const char* name, int nameLen,
                 int declLineIdx, int endLineIdx,
                 const uint8_t* paramTokens, int paramTokensLen)
  {
    if (nameLen > 15) return false;
    if (paramTokensLen > (int)sizeof(m_subs[0].paramTokens)) return false;
    if (m_subCount >= MAX_SUBS) return false;
    SubDef& s = m_subs[m_subCount++];
    memcpy(s.name, name, nameLen);
    s.nameLen = (uint8_t)nameLen;
    s.declLineIdx = (uint16_t)declLineIdx;
    s.endLineIdx  = (uint16_t)endLineIdx;
    memcpy(s.paramTokens, paramTokens, paramTokensLen);
    s.paramTokensLen = (uint8_t)paramTokensLen;
    return true;
  }

  SubDef* findSub(const char* name, int nameLen)
  {
    for (int i = 0; i < m_subCount; i++)
    {
      if (m_subs[i].nameLen == nameLen &&
          memcmp(m_subs[i].name, name, nameLen) == 0)
      {
        return &m_subs[i];
      }
    }
    return NULL;
  }

  int subCount() const { return m_subCount; }
  SubDef* subAt(int i) { return (i >= 0 && i < m_subCount) ? &m_subs[i] : NULL; }

private:
  int findFnSlot(const char* name, int nameLen)
  {
    for (int i = 0; i < m_fnCount; i++)
    {
      if (m_fns[i].nameLen == nameLen &&
          memcmp(m_fns[i].name, name, nameLen) == 0)
      {
        return i;
      }
    }
    return -1;
  }

  FnDef m_fns[MAX_FNS];
  int   m_fnCount = 0;
  SubDef m_subs[MAX_SUBS];
  int    m_subCount = 0;

public:

  // --- Scalar access ---

  Variable* findNum(const char* name, int nameLen)
  {
    return findOrCreate(name, nameLen, false);
  }

  Variable* findStr(const char* name, int nameLen)
  {
    return findOrCreate(name, nameLen, true);
  }

  float getNum(const char* name, int nameLen)
  {
    Variable* v = findOrCreate(name, nameLen, false);
    return (v && v->dimCount == 0) ? v->numVal : 0;
  }

  void setNum(const char* name, int nameLen, float val)
  {
    Variable* v = findOrCreate(name, nameLen, false);
    if (v && v->dimCount == 0)
    {
      v->numVal = val;
    }
  }

  const char* getStr(const char* name, int nameLen)
  {
    Variable* v = findOrCreate(name, nameLen, true);
    if (v && v->dimCount == 0 &&
        v->strIndex >= 0 && v->strIndex < m_strCount)
    {
      return m_strings[v->strIndex] ? m_strings[v->strIndex] : "";
    }
    return "";
  }

  void setStr(const char* name, int nameLen, const char* val)
  {
    Variable* v = findOrCreate(name, nameLen, true);
    if (!v || v->dimCount != 0)
    {
      return;
    }

    // If the variable already owns a slot, reuse it in place rather
    // than allocating a fresh one — otherwise every reassignment grows
    // m_strCount and the pool fills up after MAX_STRINGS unique writes
    // (e.g. a FOR/READ loop reading ~200 DATA values into one var hits
    // the cap even though only one variable is live).
    if (v->strIndex >= 0 && v->strIndex < MAX_STRINGS)
    {
      if (m_strings[v->strIndex]) free(m_strings[v->strIndex]);
      int len = (int)strlen(val);
      m_strings[v->strIndex] = (char*)malloc(len + 1);
      if (m_strings[v->strIndex]) strcpy(m_strings[v->strIndex], val);
      return;
    }
    v->strIndex = allocString(val);
  }

  // --- Array access ---

  // Dimension an array. dimSizes are INCLUSIVE (DIM A(10) → dimSize=11).
  // Returns true on success.
  bool dimArray(const char* name, int nameLen, bool isString,
                int nDims, const int* dimSizes)
  {
    if (nDims < 1 || nDims > MAX_ARRAY_DIMS) return false;

    Variable* v = findOrCreate(name, nameLen, isString);
    if (!v) return false;

    // Free previous array data if any
    if (v->arrayData)
    {
      free(v->arrayData);
      v->arrayData = NULL;
    }

    v->dimCount = (uint8_t)nDims;
    int total = 1;
    for (int i = 0; i < nDims; i++)
    {
      v->dimSize[i] = (uint16_t)dimSizes[i];
      total *= dimSizes[i];
    }

    if (isString)
    {
      v->arrayData = calloc(total, sizeof(int));
      // Initialize all strIndex to -1 (empty)
      int* p = (int*)v->arrayData;
      for (int i = 0; i < total; i++) p[i] = -1;
    }
    else
    {
      v->arrayData = calloc(total, sizeof(float));
    }
    return v->arrayData != NULL;
  }

  // Compute flat offset from indices
  static int arrayOffset(Variable* v, const int* indices, int nIndices)
  {
    if (nIndices != v->dimCount) return -1;
    int offset = 0;
    int stride = 1;
    for (int i = v->dimCount - 1; i >= 0; i--)
    {
      if (indices[i] < 0 || indices[i] >= v->dimSize[i]) return -1;
      offset += indices[i] * stride;
      stride *= v->dimSize[i];
    }
    return offset;
  }

  float getArrayNum(const char* name, int nameLen,
                    const int* indices, int nIndices)
  {
    Variable* v = findOrCreate(name, nameLen, false);
    if (!v || v->dimCount == 0) return 0;
    int off = arrayOffset(v, indices, nIndices);
    if (off < 0 || !v->arrayData) return 0;
    return ((float*)v->arrayData)[off];
  }

  void setArrayNum(const char* name, int nameLen,
                   const int* indices, int nIndices, float val)
  {
    Variable* v = findOrCreate(name, nameLen, false);
    if (!v) return;
    // Auto-dimension to default size 11 (DIM A(10)) if not already
    if (v->dimCount == 0)
    {
      int defSize = 11;
      int defDims[MAX_ARRAY_DIMS];
      for (int i = 0; i < nIndices; i++) defDims[i] = defSize;
      dimArray(name, nameLen, false, nIndices, defDims);
    }
    int off = arrayOffset(v, indices, nIndices);
    if (off < 0 || !v->arrayData) return;
    ((float*)v->arrayData)[off] = val;
  }

  const char* getArrayStr(const char* name, int nameLen,
                          const int* indices, int nIndices)
  {
    Variable* v = findOrCreate(name, nameLen, true);
    if (!v || v->dimCount == 0) return "";
    int off = arrayOffset(v, indices, nIndices);
    if (off < 0 || !v->arrayData) return "";
    int idx = ((int*)v->arrayData)[off];
    if (idx < 0 || idx >= m_strCount) return "";
    return m_strings[idx] ? m_strings[idx] : "";
  }

  void setArrayStr(const char* name, int nameLen,
                   const int* indices, int nIndices, const char* val)
  {
    Variable* v = findOrCreate(name, nameLen, true);
    if (!v) return;
    if (v->dimCount == 0)
    {
      int defSize = 11;
      int defDims[MAX_ARRAY_DIMS];
      for (int i = 0; i < nIndices; i++) defDims[i] = defSize;
      dimArray(name, nameLen, true, nIndices, defDims);
    }
    int off = arrayOffset(v, indices, nIndices);
    if (off < 0 || !v->arrayData) return;

    int* arr = (int*)v->arrayData;
    // Same in-place-reuse logic as scalar setStr — without this, a
    // FOR/READ loop into an array element would leak a slot per write
    // and saturate MAX_STRINGS after ~200 reassignments.
    if (arr[off] >= 0 && arr[off] < MAX_STRINGS)
    {
      if (m_strings[arr[off]]) free(m_strings[arr[off]]);
      int len = (int)strlen(val);
      m_strings[arr[off]] = (char*)malloc(len + 1);
      if (m_strings[arr[off]]) strcpy(m_strings[arr[off]], val);
      return;
    }
    arr[off] = allocString(val);
  }

  int varCount() const { return m_varCount; }

private:
  Variable m_vars[MAX_VARS];
  int m_varCount = 0;

  char* m_strings[MAX_STRINGS];
  int m_strCount = 0;

  Variable* findOrCreate(const char* name, int nameLen, bool isString)
  {
    for (int i = 0; i < m_varCount; i++)
    {
      if (m_vars[i].isString == isString &&
          (int)strlen(m_vars[i].name) == nameLen &&
          strncasecmp(m_vars[i].name, name, nameLen) == 0)
      {
        return &m_vars[i];
      }
    }

    if (m_varCount >= MAX_VARS) return NULL;

    Variable* v = &m_vars[m_varCount++];
    int copyLen = (nameLen < MAX_VAR_NAME - 1) ? nameLen : MAX_VAR_NAME - 1;
    memcpy(v->name, name, copyLen);
    v->name[copyLen] = '\0';
    for (int i = 0; i < copyLen; i++)
    {
      v->name[i] = toupper(v->name[i]);
    }
    v->isString = isString;
    v->dimCount = 0;
    v->arrayData = NULL;
    v->numVal = 0;
    v->strIndex = -1;
    return v;
  }

  int allocString(const char* val)
  {
    if (m_strCount >= MAX_STRINGS) return -1;
    int idx = m_strCount++;
    int len = strlen(val);
    m_strings[idx] = (char*)malloc(len + 1);
    if (m_strings[idx]) strcpy(m_strings[idx], val);
    return idx;
  }
};

#endif // VAR_TABLE_H
