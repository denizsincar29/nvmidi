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
rem The engine's own Angelscript symbols (asAllocMem, asGetLibraryOptions, ...)
rem are exported by nvgt.exe as plain C names. MinGW leaves them undefined and
rem the loader fills them in; MSVC needs them at link time, so an import library
rem is generated from the running engine's export table first.
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

where nvgt.exe >nul 2>&1
set NVGT_EXE=nvgt.exe
if defined NVGT_HOME set NVGT_EXE=%NVGT_HOME%\nvgt.exe
dumpbin /exports "%NVGT_EXE%" > build-msvc\nvgt_exports.txt || (
	echo could not read the export table of %NVGT_EXE% - set NVGT_HOME to the folder holding nvgt.exe
	exit /b 2
)
echo LIBRARY nvgt.exe > build-msvc\nvgt.def
echo EXPORTS >> build-msvc\nvgt.def
for /f "tokens=4" %%a in ('findstr /r /c:"^ *[0-9][0-9]*  *[0-9A-F][0-9A-F]*  *[0-9A-F][0-9A-F]*  *" build-msvc\nvgt_exports.txt') do echo %%a>> build-msvc\nvgt.def
lib /nologo /def:build-msvc\nvgt.def /machine:x64 /out:build-msvc\nvgt_import.lib || exit /b 2

cl /nologo /std:c++17 /O2 /EHsc /MD /LD /D__WINDOWS_MM__ /D_CRT_SECURE_NO_WARNINGS ^
   /Isrc /Ithird_party\rtmidi /Ithird_party\angelscript /Fo:build-msvc\ ^
   src\nvmidi.cpp third_party\rtmidi\RtMidi.cpp third_party\angelscript\scriptarray.cpp ^
   /Fe:nvmidi.dll /link build-msvc\nvgt_import.lib winmm.lib ole32.lib setupapi.lib ksuser.lib || exit /b 2

echo built nvmidi.dll with MSVC
popd
