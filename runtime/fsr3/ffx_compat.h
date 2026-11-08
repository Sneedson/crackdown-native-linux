// Forced in front of the FidelityFX SDK's sources (build.sh): what they take from MSVC's headers.
#pragma once
#include <cstring>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <locale>
#include <codecvt>
#ifndef _countof
#define _countof( a ) ( sizeof( a ) / sizeof( ( a )[0] ) )
#endif
#include <cmath>
#include <cstdarg>
template <size_t N> inline int wcscpy_s( wchar_t ( &d )[N], const wchar_t *s ) { wcsncpy( d, s, N - 1 ); d[N - 1] = 0; return 0; }
inline int wcscpy_s( wchar_t *d, size_t n, const wchar_t *s ) { wcsncpy( d, s, n - 1 ); d[n - 1] = 0; return 0; }
template <size_t N> inline int strcpy_s( char ( &d )[N], const char *s ) { strncpy( d, s, N - 1 ); d[N - 1] = 0; return 0; }
inline int strcpy_s( char *d, size_t n, const char *s ) { strncpy( d, s, n - 1 ); d[n - 1] = 0; return 0; }
template <size_t N> inline int sprintf_s( char ( &d )[N], const char *f, ... ) { va_list a; va_start( a, f ); int r = vsnprintf( d, N, f, a ); va_end( a ); return r; }
inline int sprintf_s( char *d, size_t n, const char *f, ... ) { va_list a; va_start( a, f ); int r = vsnprintf( d, n, f, a ); va_end( a ); return r; }
// wchar_t is 4 bytes here (2 on Windows), which makes the SDK's private contexts larger than the opaque
// ones it hands out: those are made twice as large (everything that includes the SDK goes through here).
#include <FidelityFX/host/ffx_types.h>
#undef FFX_SDK_DEFAULT_CONTEXT_SIZE
#define FFX_SDK_DEFAULT_CONTEXT_SIZE ( 1024 * 256 )
