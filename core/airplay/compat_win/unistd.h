/* CRTCast for Windows: the bits of <unistd.h> that UxPlay uses, for MSVC. */
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#include <process.h>
#include <stdint.h>
#include <string.h>
#include <BaseTsd.h>

#ifndef _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#define _SSIZE_T_DEFINED
#endif

static __inline int usleep(unsigned int microseconds) {
    Sleep((microseconds + 999) / 1000);
    return 0;
}
static __inline unsigned int sleep(unsigned int seconds) {
    Sleep(seconds * 1000);
    return 0;
}

#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif
