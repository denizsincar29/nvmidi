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
rem No import library is built, and no nvgt.exe is read. Here is why that is
rem safe, since an earlier version of this comment got it wrong the other way
rem and a later one got it wrong again by dropping the import library on the
rem strength of the mistake.
rem
rem nvgt_plugin.h hands the plugin the engine's functions as pointers: the
rem build defines NVGT_PLUGIN_STATIC and the header's X() list turns every name
rem into a pointer variable (nvgt_plugin.h:69-70 declares asAllocMem and
rem asFreeMem, prepare_plugin() fills them from nvgt_plugin_shared). So the
rem plugin never imports them.
rem
rem What does import them is the Angelscript library compiled into this dll.
rem third_party/angelscript was written against an engine that exports these as
rem plain C functions, and scriptarray.cpp takes the address of two of them
rem outright (scriptarray.cpp:21-22, "static asALLOCFUNC_t userAlloc =
rem asAllocMem;") and calls a third by name (:274). src/nvgt_plugin.h turns
rem those names into pointer *variables* in this translation unit, so MSVC
rem resolves a call against a variable and wants a function-shaped symbol that
rem never existed. Measured on run 36908725123, five minutes after the import
rem library was dropped:
rem
rem   scriptarray.obj : error LNK2001: unresolved external symbol asAllocMem
rem     Hint: void * (__cdecl* asAllocMem)(unsigned __int64)
rem           (?asAllocMem@@3P6APEAX_K@ZEA)
rem
rem The mangled name settles it - "3P6A" is a pointer to a function, so the
rem symbol is the variable. The eight that fail are exactly the pointer names
rem from the header's list, and the call sites are in Angelscript, not in
rem nvmidi.cpp.
rem
rem The fix is one line per name, below: an alias declaration in an inline
rem function that is never called. It is well formed whether or not the name
rem was already declared - "typedef int T; void f(){ typedef int T; }" compiles
rem - so it needs no conditional, and it only makes the variable callable when
rem there is nothing else, which is never inside the engine, where the header
rem is compiled the other way and the real functions are switched on at
rem nvgt_plugin.h:98. Two separate declarations in one translation unit are
rem allowed as long as they agree, and these do.
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

rem One linker alias per as* name, used by the build below and by nothing else.
rem
rem Written with rem rather than with a bare echo: cmd ends the comment at
rem the word, so "rem" followed directly by a "#" is a comment in every other
rem interpreter and a live redirect here. The first version nested the echo in
rem parentheses and cmd answered ") was unexpected at this time." at that line
rem on the runner, exit 255, before the compiler ran - run 36909839651. Every
rem line below is inert text, so no line in this block can fail that way; the
rem first ">" creates the file, the rest append.
rem Write the alias header. Every line is an "if exist" guard
rem around a redirect, rather than the "rem ... >" form the block above
rem used to be: cmd ends a rem at the word, but it also did not perform
rem the redirection there - measured four runs on the runner, including
rem after the target was quoted - and a form that is supposed to work and
rem does not is worse than one whose rule is written down. The guard says
rem which of the file's own two steps failed, and the mkdir below means
rem this cannot be a missing-directory error.
if not exist "build-msvc" mkdir "build-msvc"
if not exist "build-msvc" (
	echo could not create the build directory
	exit /b 2
)

echo #pragma comment(linker, "/alternatename:__imp_asAllocMem=asAllocMem") > "build-msvc\nvgt_import.inc"
if not exist "build-msvc\nvgt_import.inc" (
	echo writing the alias header failed
	exit /b 2
)
echo #pragma comment(linker, "/alternatename:__imp_asFreeMem=asFreeMem") >> "build-msvc\nvgt_import.inc"
echo #pragma comment(linker, "/alternatename:__imp_asGetLibraryOptions=asGetLibraryOptions") >> "build-msvc\nvgt_import.inc"
echo #pragma comment(linker, "/alternatename:__imp_asGetActiveContext=asGetActiveContext") >> "build-msvc\nvgt_import.inc"
echo #pragma comment(linker, "/alternatename:__imp_asAcquireExclusiveLock=asAcquireExclusiveLock") >> "build-msvc\nvgt_import.inc"
echo #pragma comment(linker, "/alternatename:__imp_asReleaseExclusiveLock=asReleaseExclusiveLock") >> "build-msvc\nvgt_import.inc"
echo #pragma comment(linker, "/alternatename:__imp_asAcquireSharedLock=asAcquireSharedLock") >> "build-msvc\nvgt_import.inc"
echo #pragma comment(linker, "/alternatename:__imp_asReleaseSharedLock=asReleaseSharedLock") >> "build-msvc\nvgt_import.inc"
echo #pragma comment(linker, "/alternatename:__imp_asAtomicInc=asAtomicInc") >> "build-msvc\nvgt_import.inc"
echo #pragma comment(linker, "/alternatename:__imp_asAtomicDec=asAtomicDec") >> "build-msvc\nvgt_import.inc"
echo #pragma comment(linker, "/alternatename:__imp_asThreadCleanup=asThreadCleanup") >> "build-msvc\nvgt_import.inc"
echo #pragma comment(linker, "/alternatename:__imp_asGetLibraryVersion=asGetLibraryVersion") >> "build-msvc\nvgt_import.inc"
echo #pragma comment(linker, "/alternatename:__imp_asPrepareMultithread=asPrepareMultithread") >> "build-msvc\nvgt_import.inc"

if not exist build-msvc\nvgt_import.inc (
	echo the alias header was not written
	type nul
	exit /b 2
)
echo the alias header is in place

rem The compile itself. Everything above wrote one file and did nothing else;
rem this is the line the file exists for. It was lost once already: the block
rem above was rewritten in 7e266e5 and the replacement stopped at the last
rem alias line, so the batch ran, wrote its header, reported success and never
rem invoked a compiler - the job's own step log was empty between "Run
rem scripts\build_win_msvc.bat" and the next step, and the dll was simply
rem absent afterwards. A batch file that silently does nothing looks exactly
rem like one that worked, so the echo at the end matters as much as the line:
rem it is the only thing in this file that says the compile was reached.
rem
rem /FI force-includes the generated alias header into each of the three
rem translation units, which is the whole point of writing it - see the block
rem above for why each name needs an alias.

echo about to compile, the header is %CD%\build-msvc\nvgt_import.inc
dir /b build-msvc 2>&1
cl /nologo /std:c++17 /O2 /EHsc /MD /LD /D__WINDOWS_MM__ /D_CRT_SECURE_NO_WARNINGS /Isrc /Ithird_party\rtmidi /Ithird_party\angelscript /Fo:build-msvc\ /FIbuild-msvc\nvgt_import.inc src\nvmidi.cpp third_party\rtmidi\RtMidi.cpp third_party\angelscript\scriptarray.cpp /Fe:nvmidi.dll /link winmm.lib ole32.lib setupapi.lib ksuser.lib
if errorlevel 1 exit /b 2

echo built nvmidi.dll with MSVC
popd
