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
rem The fix is src/nvgt_shims.cpp plus one /ALTERNATENAME per name, below. The
rem shim gives the linker a real function to resolve each name to, with C
rem linkage so the symbol needs no mangling; the alias binds the undefined
rem plain name to it. The header's pointer variables stay the single source of
rem the actual engine address, and prepare_plugin() fills them as before.
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

rem The alias list, handed to the linker directly rather than through a
rem generated header. An earlier attempt wrote the same aliases as
rem "#pragma comment(linker, ...)" lines into a file and force-included it; the
rem link still failed with the same eight unresolved names on run 37010648892,
rem so the pragma never reached the linker. A switch on the command line cannot
rem be lost that way, and the job log shows it verbatim.
rem
rem The direction matters and was written backwards at first. The symbol that is
rem undefined in this link is the plain function name (angelscript.h:590
rem declares asAllocMem with no definition here); the symbol that is defined is
rem the extern "C" forwarder in src/nvgt_shims.cpp, whose symbol is its own
rem name. So "the undefined asAllocMem is, for this link, asAllocMem_shim".
set ALIASES=/ALTERNATENAME:asAllocMem=asAllocMem_shim /ALTERNATENAME:asFreeMem=asFreeMem_shim /ALTERNATENAME:asGetLibraryOptions=asGetLibraryOptions_shim /ALTERNATENAME:asGetActiveContext=asGetActiveContext_shim /ALTERNATENAME:asAcquireExclusiveLock=asAcquireExclusiveLock_shim /ALTERNATENAME:asReleaseExclusiveLock=asReleaseExclusiveLock_shim /ALTERNATENAME:asAcquireSharedLock=asAcquireSharedLock_shim /ALTERNATENAME:asReleaseSharedLock=asReleaseSharedLock_shim /ALTERNATENAME:asAtomicInc=asAtomicInc_shim /ALTERNATENAME:asAtomicDec=asAtomicDec_shim /ALTERNATENAME:asThreadCleanup=asThreadCleanup_shim /ALTERNATENAME:asGetLibraryVersion=asGetLibraryVersion_shim /ALTERNATENAME:asPrepareMultithread=asPrepareMultithread_shim

cl /nologo /std:c++17 /O2 /EHsc /MD /LD /D__WINDOWS_MM__ /D_CRT_SECURE_NO_WARNINGS /Isrc /Ithird_party\rtmidi /Ithird_party\angelscript /Fo:build-msvc\ src\nvmidi.cpp src\nvgt_shims.cpp third_party\rtmidi\RtMidi.cpp third_party\angelscript\scriptarray.cpp /Fe:nvmidi.dll /link winmm.lib ole32.lib setupapi.lib ksuser.lib %ALIASES%
if errorlevel 1 exit /b 2

echo built nvmidi.dll with MSVC
popd
