// ti_platform.cpp
//
// Weak default no-op implementations of the platform hooks declared in
// ti_platform.h. Each downstream project provides strong overrides for
// the hooks its hardware actually implements; the linker prefers strong
// over weak so the project's versions win automatically.
//
// Why a .cpp file rather than inline-in-header weak definitions: GCC
// will happily inline an `inline __attribute__((weak))` empty body at
// every call site, which short-circuits the strong override. With
// definitions out-of-line in this .cpp, every call goes through a real
// symbol and link-time strong/weak resolution does what we want.

#include "ti_platform.h"
#include <cstdio>

__attribute__((weak)) void tiYield() {}

__attribute__((weak)) void tiPrintChar(char /*c*/) {}
__attribute__((weak)) void tiPrintString(const char* /*s*/) {}
__attribute__((weak)) void tiClearScreen() {}

__attribute__((weak)) void tiSetChar(int /*row*/, int /*col*/, char /*ch*/) {}
__attribute__((weak)) char tiGetChar(int /*row*/, int /*col*/) { return ' '; }
__attribute__((weak)) void tiSetScreenColor(int /*idx*/) {}
__attribute__((weak)) void tiSetCharColor(int /*set*/, int /*fg*/, int /*bg*/) {}
__attribute__((weak)) void tiSetCharPattern(int /*charCode*/,
                                            const uint8_t* /*bytes*/) {}
__attribute__((weak)) void tiGetCharPattern(int /*charCode*/,
                                            uint8_t* /*bytes*/) {}
__attribute__((weak)) void tiResetCharset() {}
__attribute__((weak)) void tiSetCharsetMode(int /*mode*/) {}
__attribute__((weak)) void tiMoveCursor(int /*row*/, int /*col*/) {}

__attribute__((weak)) void tiSpriteDraw(int /*slot*/) {}
__attribute__((weak)) void tiSpriteErase(int /*slot*/) {}

__attribute__((weak)) int  tiReadKey() { return -1; }
__attribute__((weak)) void tiReadJoystick(int /*unit*/, int* outX, int* outY)
{
  if (outX) *outX = 0;
  if (outY) *outY = 0;
}

__attribute__((weak)) void tiPair()   {}
__attribute__((weak)) void tiUnpair() {}

__attribute__((weak)) void tiSoundPlay(int /*duration*/,
                                       int /*f1*/, int /*v1*/,
                                       int /*f2*/, int /*v2*/,
                                       int /*f3*/, int /*v3*/,
                                       int /*f4*/, int /*v4*/) {}
__attribute__((weak)) void tiSoundStop() {}

__attribute__((weak)) void tiSay(const char* /*wordStr*/,
                                 const uint8_t* /*phraseBytes*/,
                                 int /*phraseLen*/) {}
__attribute__((weak)) int  tiSpget(const char* /*word*/,
                                   uint8_t* /*outBuf*/, int /*bufSize*/)
{
  return 0;
}

__attribute__((weak)) void tiWifiSet(const char* /*ssid*/, const char* /*pass*/) {}
__attribute__((weak)) void tiWifiForget() {}
__attribute__((weak)) void tiWifiStatus(char* out, int outSize)
{
  if (out && outSize > 0) snprintf(out, outSize, "OFFLINE");
}
__attribute__((weak)) void tiWifiOff() {}
__attribute__((weak)) void tiWifiOn()  {}
