// Shared helpers for patching Penumbra.exe in memory.
#pragma once

#include <windows.h>

typedef unsigned char u8;

extern char g_dir[MAX_PATH];	// folder holding the DLL (and the game), with trailing backslash

void Log(const char* fmt, ...);
int CfgInt(const char* key, int def);

// Byte checks against the expected original code. They log a mismatch and return false.
bool Expect(DWORD addr, const void* bytes, size_t n, const char* what);
bool ExpectDword(DWORD addr, DWORD val, const char* what);
bool ExpectCall(DWORD site, DWORD target, const char* what);

void Write(DWORD addr, const void* bytes, size_t n);
void WriteDword(DWORD addr, DWORD val);
void RedirectCall(DWORD site, const void* fn);

// High framerate (HighFramerate.cpp)
int ChooseLogicRate();
bool VerifyHighFramerate();
void ApplyHighFramerate(int rate);
