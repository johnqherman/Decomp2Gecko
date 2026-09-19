#pragma once

#ifdef _WIN32
#include <process.h>
#define d2g_getpid _getpid
#else
#include <unistd.h>
#define d2g_getpid getpid
#endif
