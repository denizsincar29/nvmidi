// nvmidi_win_compat.h - the names nvmidi_loopback.c calls, defined twice.
//
// The driver is one file compiled by two different compilers that do not
// share a libc: mingw-w64 on windows, where a WCHAR is two bytes because the
// platform says so, and any host compiler on linux, where wchar_t is four and
// the windows headers are missing entirely. The first version of the offline
// run solved the second case by giving the driver shim-only names
// (shim_wcpy, shim_wlen, ...) - and then the real windows build failed,
// because on windows the shim is not compiled and those names do not exist.
// The offline harness had been paid for with the build it was meant to check.
//
// So the names live here, once, and this header is included by the driver
// itself - not by the shim. On windows each name is the real function; on
// linux the shim has already defined it as a static inline by the time this
// header is reached (the loader includes the shim before the driver), and the
// windows branch below is not taken. Either way the driver source is the same
// text, which is the only arrangement in which a linux pass says anything
// about the windows build.
//
// Inclusion order is the whole contract: the shim first, then the driver.
// The #ifndef makes that explicit rather than assumed - if this header is
// ever reached without the shim on a non-windows build, the -Wimplicit
// diagnostics that follow are the intended failure, not a mystery.
#ifndef NVMIDI_WIN_COMPAT_H
#define NVMIDI_WIN_COMPAT_H

#if defined(_WIN32)

// Each of these is the function the shim helper was replacing, chosen so the
// driver's call sites read the same on both compilers. wcsncpy is the one
// that is not a plain rename: the shim's shim_wncpy pads with zeros, exactly
// as wcsncpy does, which is why the driver is allowed to pass a slot array
// and rely on the tail being cleared.
#define shim_wlen    wcslen
#define shim_wcpy    wcscpy
#define shim_wncpy   wcsncpy
#define shim_w2mb    shim_win_w2mb

// Not a rename either. The driver needs one thing here - the number of bytes
// in a wide string, including its null - and it wrote it as
// (shim_wlen(s) + 1) * sizeof(WCHAR). On windows that is GetModuleFileNameW's
// contract already, so the two sides cannot drift by a term.
static inline int shim_win_w2mb(const WCHAR *in, char *out, int outn) {
	return WideCharToMultiByte(CP_ACP, 0, in, -1, out, outn, NULL, NULL);
}

// ... and this one writes "midiN" without asking any library for the
// conversion. Both candidate names were tried and both are traps here:
// swprintf is the C99 one, which wants the count inside the format string,
// and a windows target that includes msvcrt's headers meets it as a static
// inline of the other shape - "static declaration of 'swprintf' follows
// non-static declaration". _snwprintf has the signature the driver wants and
// is also declared static inline by the same headers, so it fails the same
// way, with the compiler printing the declaration it found underneath the
// implicit-declaration error. The driver's only case is a small decimal
// number, so the digits are built here and no unresolved name is left for the
// linker to find or not find.
//
// Wide, not narrow: the callers pass a WCHAR slot array that winmm then reads
// as a device name, and on windows a WCHAR is two bytes - the digit is stored
// once per element, not once per byte.
static inline int shim_win_wprintf(WCHAR *d, size_t n, int num) {
	int len = 0;
	char digits[12];
	if (!d || n == 0) return -1;
	if (num < 0) num = 0;
	do { digits[len++] = (char)('0' + num % 10); num /= 10; } while (num && len < 11);
	if ((size_t)len + 5 > n) return -1;
	{
		int i;
		for (i = 0; i < 4; i++) d[i] = L"midi"[i];
		for (i = 0; i < len; i++) d[4 + i] = (WCHAR)digits[len - 1 - i];
		d[4 + len] = 0;
	}
	return 4 + len;
}
#define shim_wprintf shim_win_wprintf

// The two string literals the driver hands to the registry. On windows the
// compiler's own L"..." is already the right width, and these have to stay
// macros: the linux versions are macros too, and a call site cannot be one
// thing and then the other.
//
// Concatenation, not token pasting, and that is not a style choice: L##name
// glues the L onto the identifier to make the single token Lmidi, which is
// undeclared - "use of undeclared identifier 'Lmidi'" from the compiler, with
// the macro expansion printed underneath it. L"" #name is a wide empty string
// followed by the name as its own string literal; the compiler joins them at
// translation, so the result is the wide L"midi" the registry call wants.
#define SHIM_L(name) L"" #name
#define SHIM_STR(s)  L"" s

// szPname is 32 WCHARs in both MIDIOUTCAPS and MIDIINCAPS, and the driver
// loops to one less than this. Taken from the struct rather than written as
// 32 so that a header revision that widens the field moves this with it.
#define SHIM_CAPS_NAME_LEN (sizeof(((MIDIOUTCAPS *)0)->szPname) / sizeof(WCHAR))

#else

// The shim already defined every one of these - as macros for SHIM_L and
// SHIM_STR, as static inline functions for the four wide-string helpers.
// Nothing to add; this branch exists so the absence is deliberate and the
// next reader does not go looking for the definitions here.
#if !defined(NVMIDI_WIN_SHIM_H)
#error "include nvmidi_win_shim.h before nvmidi_loopback.c on a non-windows build"
#endif

#endif /* _WIN32 */
#endif /* NVMIDI_WIN_COMPAT_H */
