@echo off
rem Build nvmidi.dll with MSVC, and this is not a preference.
rem
rem nvgt.exe on Windows is an MSVC build (it loads msvcp_win.dll / ucrtbase.dll).
rem A MinGW build of this plugin carries libstdc++'s string, nvgt carries MSVC's,
rem and the two do not agree on where a std::string keeps its data: measured on
rem Диниз's machine 2026-10-01, a MinGW-built plugin received "nord" as
rem size=0 data=0x64726f6e (MSVC stores the characters at offset 0 and the size at
rem offset 16; libstdc++ expects the pointer at 0 and the size at 8). Every
rem string argument was empty or garbage, and midi_config::load() faulted on the
rem misread pointer. The same source built with MSVC against the same engine
rem receives size=4 first byte=110, midi_note_number("C4") answers 60, and
rem load_if_present() returns true.
rem
rem No import library is built, and that is not an omission.
rem
rem This script used to read nvgt.exe's export table with dumpbin and turn it
rem into nvgt_import.lib, on the belief that MSVC needs asAllocMem and the rest
rem at link time. The belief was mine and it was wrong. src/nvgt_plugin.h shows
rem what actually happens: nvgt_plugin_shared carries the engine's functions as
rem pointers, and prepare_plugin() copies them into this translation unit's own
rem globals (nvgt_plugin.h:171-181, "name = shared->f_##name"). Nothing is
rem imported by name, so an import library has nothing to satisfy.
rem
rem Measured 2026-10-01: neither Windows build exports them. The PE of
rem stub/nvgt_windows.bin carries 21 exported names and asAllocMem is not among
rem them; nor does the Linux engine, whose ELF .dynsym holds 248 defined symbols
rem and no as* at all. The plugin loads and registers 183 names with 0 refused
rem against that same Linux engine, so the pointer path is the one that works.
rem
rem That leaves the script with no NVGT_HOME and no nvgt.exe, on purpose. The
rem .dll is built entirely from this repository's source.
setlocal
set HERE=%~dp0
pushd "%HERE%.."

if not exist build-msvc mkdir build-msvc

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
	echo no vswhere at "%VSWHERE%" - install the Visual Studio C++ tools
	exit /b 2
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
	echo vswhere found no installation with the C++ tools
	exit /b 2
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 2

cl /nologo /std:c++17 /O2 /EHsc /MD /LD /D__WINDOWS_MM__ /D_CRT_SECURE_NO_WARNINGS ^
   /Isrc /Ithird_party\rtmidi /Ithird_party\angelscript /Fo:build-msvc\ ^
   src\nvmidi.cpp third_party\rtmidi\RtMidi.cpp third_party\angelscript\scriptarray.cpp ^
   /Fe:nvmidi.dll /link winmm.lib ole32.lib setupapi.lib ksuser.lib || exit /b 2

echo built nvmidi.dll with MSVC
popd
