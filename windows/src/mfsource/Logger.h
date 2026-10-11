#pragma once
#include <windows.h>
#include <stdio.h>

inline void Log(const char* format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    OutputDebugStringA("[PrismForgeMF] ");
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
}
