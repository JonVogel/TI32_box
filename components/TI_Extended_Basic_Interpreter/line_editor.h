// Line editor types — kept in a header so Arduino's auto-prototype
// generator sees them before scanning the .ino. Implementation lives in
// ti-basic.ino next to the globals it touches (screenBuf, cursorCol, em…).

#pragma once

struct LineEdit
{
  char* buf;
  int   maxLen;          // max characters (not counting NUL)
  int   len;             // current content length
  int   pos;             // cursor position within buf (0..len)
  int   startCol;        // column on screen where the line begins
  int   startRow;        // row on screen where the line begins
  bool  historyEnabled;  // REDO + line-number recall
};

enum EditResult
{
  EDIT_CONTINUE = 0,
  EDIT_SUBMITTED,
  EDIT_BROKEN,
};

// TI BASIC's line editor has two distinct states:
//   ENTRY — fresh line being typed. Cursor implicit at end; arrows, DEL,
//           and prev/next line navigation are inert. Only append, BS,
//           Enter, ERASE, INS, REDO, and <lineN>+UP/DOWN do anything.
//   EDIT  — recalled line under active edit. Arrows move the cursor,
//           DEL removes the char under it, UP/DOWN browse sibling lines.
enum EditMode
{
  EM_ENTRY = 0,
  EM_EDIT,
};
