# nvmidi - MIDI input and output for NVGT
#
# Builds the plugin as a shared library:
#   Linux   -> nvmidi.so   (ALSA by default; needs libasound2-dev to build)
#   Windows -> nvmidi.dll  (needs MinGW-w64, links winmm)
#
# Usage:
#   make                 # build for the current platform
#   make MIDI_BACKEND=dummy   # linux, no MIDI hardware, no external deps
#   make NVGT_SRC=...    # build against an nvgt source checkout
#   make clean
#
# NVGT_SRC only needs to point at the nvgt source tree when you have not
# vendored src/nvgt_plugin.h. The plugin compiles against that single header
# plus the Angelscript SDK headers in third_party/.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -fPIC -Wall -Wextra
INCLUDES  = -Isrc -Ithird_party/rtmidi -Ithird_party/angelscript

# scriptarray.cpp is the array add-on behind CScriptArray. The plugin calls
# into it (Create, GetSize, At, SetValue) to read the arrays a script hands
# over, so its symbols have to be linked into the plugin rather than
# resolved against the host, which exports no such thing.
SOURCES   = src/nvmidi.cpp third_party/rtmidi/RtMidi.cpp \
            third_party/angelscript/scriptarray.cpp

# The default name. A variable and not a literal, because the no-addon target
# below would otherwise be the same file spelled differently - see the comment
# there for what that cost.
TARGET      ?= nvmidi.dll

ifeq ($(OS),Windows_NT)
    TARGET    = nvmidi.dll
    CXXFLAGS += -D__WINDOWS_MM__
    # -static, and the plugin dies without it.
    #
    # A released dll was built with only -static-libgcc -static-libstdc++, which
    # leaves the compiler's other runtime libraries shared. The dll ended up
    # importing libwinpthread-1.dll - measured by reading the import table of
    # the shipped v0.8.0 asset, not inferred: it was the one entry in that
    # table that is not part of Windows (KERNEL32, WINMM and the api-ms-win-crt-*
    # runtime are; libwinpthread-1.dll is not).
    #
    # Windows does not resolve a library's dependencies one function at a time.
    # A dll whose import table names something the machine does not have is
    # refused as a whole, and nvgt's only words for that are
    #
    #     Compilation error: file: nvmidi
    #     line: 0 (0)
    #     ERROR: failed to load plugin
    #
    # with no script code run at all - the same silent wall ALSA produced on
    # linux in v0.7.1, from the same class of mistake. A user hit it twice.
    #
    # -static pulls every MinGW runtime library in, so the shipped dll leans on
    # nothing but the operating system. The cost is size, which does not matter
    # here, and the gain is that "did it load" stops depending on what the user
    # happens to have installed. The workflow asserts the import table after
    # this, so a regression is a red build and not another user report.
    LDFLAGS   = -shared -static -static-libgcc -static-libstdc++ -Wl,--kill-at
    LIBS      = -lwinmm -lole32 -lsetupapi -lksuser
    RM        = del /Q
else
    TARGET    = nvmidi.so
    CXX           ?= g++
    # Which RtMidi backend a *default* build carries.
    #
    # ALSA is a build of convenience, not a build to ship. Defining
    # __LINUX_ALSA__ makes the plugin carry a "needed: libasound.so.2" entry,
    # and the dynamic loader refuses a library whose dependencies it cannot
    # resolve - all of it, before a single symbol is looked up. So this build
    # loads only on a machine that has ALSA, and on one that does not the
    # engine reports "failed to load plugin" with no way for the script to say
    # anything: the script is never run. For your own machine `make` is fine.
    #
    # To build a plugin that ships, use MIDI_BACKEND=dummy (below), which
    # depends on nothing but libc and libstdc++, so the loader always takes
    # it. Ports then report "dummy" through midi_api_name() and no hardware is
    # touched - the plugin tells the script *why* instead of dying at load.
    ifeq ($(MIDI_BACKEND),dummy)
        CXXFLAGS  += -D__RTMIDI_DUMMY__
    else ifeq ($(MIDI_BACKEND),alsa)
        CXXFLAGS  += -D__LINUX_ALSA__
        LIBS       = -lasound
    else ifeq ($(MIDI_BACKEND),jack)
        CXXFLAGS  += -D__UNIX_JACK__
        LIBS       = -ljack
    else
        CXXFLAGS  += -D__LINUX_ALSA__
        LIBS       = -lasound
    endif
    LDFLAGS       = -shared
    LIBS         += -lpthread
    RM            = rm -f
endif

.PHONY: all clean check

all: $(TARGET)

$(TARGET): $(SOURCES) src/nvmidi.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SOURCES) -o $(TARGET) $(LDFLAGS) $(LIBS)

# Compiles the plugin source and the add-on without RtMidi, and without
# emitting an object: a fast way to catch a typo in the Angelscript
# bindings before waiting for the link.
check:
	$(CXX) $(CXXFLAGS) $(INCLUDES) -fsyntax-only src/nvmidi.cpp third_party/angelscript/scriptarray.cpp

clean:
	$(RM) $(TARGET)

# Compile the plugin and the add-on, then link only the objects whose symbol
# tables actually reference the add-on.
#
# This exists because of one line that will not say what it is. The ordinary
# link has never once produced a dll, make reports success, and the diagnostics
# that would explain it are not in the CI log: measured in run 36709928607, the
# last line the runner carries out of the compile is scriptarray.cpp:370,
# while that file has 380 lines, and the link's own output - ld writes "cannot
# find -l...", "undefined reference", "Error 1" - appears nowhere in the log at
# all. So neither "the link is fine and the dll is written somewhere else" nor
# "the link fails silently" can be told apart from the outside.
#
# That is the same trap the ordinary target set once already: a silent step at
# the end of a chain, read as whatever the reader expected. So this splits the
# chain: the compile runs as three separate steps that each have to exit 0, and
# the link runs as a step of its own. If the dll is missing afterwards, the
# object files and the linker's own words are both in the tree.
#
# The reference is what makes the flag correct. In the no-addon build
# src/nvmidi.cpp is compiled with -DNVGT_SKIP_ARRAY_ADDON, so it never calls
# into scriptarray.cpp and does not contain its symbols; link it against
# scriptarray.o anyway and the linker pulls the whole object in to satisfy a
# reference that does not exist. Measured, run 36703826070: that is what fails
# ("undefined reference to CreateScriptArray" and four more), which is a fact
# about the flag's reach and not about the build. Asking the object files which
# of them carry an undefined reference to the add-on's entry points answers it
# directly, and the answer is the list of objects to link.
#
# nm comes from the toolchain that is already compiling. On the windows runner
# that is x86_64-w64-mingw32-g++ and x86_64-w64-mingw32-nm; elsewhere g++ and
# nm, since the object format is whatever the compiler emits either way.
NM ?= $(patsubst g++%,nm%,$(firstword $(CXX)))

obj:
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c src/nvmidi.cpp -o nvmidi.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c third_party/rtmidi/RtMidi.cpp -o RtMidi.o
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c third_party/angelscript/scriptarray.cpp -o scriptarray.o

link: obj
	echo --- undefined references that name the array add-on ---
	type nul > refs.txt
	for %f in (nvmidi.o RtMidi.o scriptarray.o) do @($(NM) -u %f | findstr /C:"CreateScriptArray" >> refs.txt || echo %f HAS-NONE)
	echo --- which objects have to be linked, and why ---
	type refs.txt
	echo --- linking ---
	set ADDON=
	for /f "tokens=3" %a in ('findstr /C:"CreateScriptArray" refs.txt') do @set ADDON=%a
	echo addon-object-from-refs.txt=%ADDON%
	$(CXX) nvmidi.o RtMidi.o -o nvmidi.dll $(LDFLAGS) $(LIBS)
	echo LINK-WITHOUT-ADDON=%ERRORLEVEL%
	if exist nvmidi.dll echo LINK-WITHOUT-ADDON-WROTE-THE-DLL
	if exist nvmidi.dll del /Q nvmidi.dll
	$(CXX) nvmidi.o RtMidi.o scriptarray.o -o nvmidi.dll $(LDFLAGS) $(LIBS)
	echo LINK-WITH-ADDON=%ERRORLEVEL%
	if exist nvmidi.dll echo LINK-WITH-ADDON-WROTE-THE-DLL
	dir /B nvmidi.dll

# The same source, built with the array add-on's registration left out.
#
# Why a separate target and not one jar built twice: nvgt loads a plugin by
# name through #pragma, and running one script against two different nvmidi.dll
# files in turn answers nothing, because a build is a fact and not a
# comparison. The comparison needs both modules present at once, which means
# both files in one directory, which means the second one cannot be called
# nvmidi.dll. This target is therefore dead code for any ordinary user and
# exists for exactly one measurement.
#
# The header prerequisite is load-bearing. This file's own target used to be
# listed in SOURCES, and on a case-insensitive filesystem `nvmidi-noarr.dll`
# resolves to `nvmidi.dll` - so the target was its own prerequisite and make
# refused it ("Circular dependency dropped"), which it does without failing, so
# the step went green having built nothing.
# SECOND NAME. The engine resolves a plugin name to a *file*: it asks the
# operating system for the library with that name, so a build of this module
# has to exist both as `nvmidi.dll` (what a user's `#pragma plugin nvmidi`
# asks for) and under a different name (what the workflow's second probe asks
# for, so that the two builds can be tested side by side in one directory).
#
# None of this is done in make. On Windows the make on the runner cannot start
# cmd's built-ins at all: `del` and `copy` are not programs, so make's
# CreateProcess fails and the target dies - measured twice, runs 36702218450
# (`make clean`, CreateProcess(NULL, del /Q nvmidi.dll)) and 36703826070
# (CreateProcess(NULL, copy /Y nvmidi.dll nvmidi-noarr.dll), Makefile:146).
#
# So this is not a target the workflow builds by name. It is the same command
# the ordinary target runs, with the flag, written under $(TARGET): make is not
# being asked for a file called nvmidi-noarr.dll, it is being asked to run that
# command again with the flag set. The name the e2e sees is the workflow's
# doing.
#
# $(TARGET) is a prerequisite, and that is what makes it a second build rather
# than a first one. The file it names already exists - the ordinary build wrote
# it moments earlier and the workflow has not renamed it away yet - so make
# considers this target up to date and runs no recipe at all, leaving whatever
# the ordinary build wrote sitting in the tree as if it were the no-addon
# library. Measured, run 36709045673: the step asked for this target with -o
# to keep the ordinary dll, make answered "Nothing to be done for 'all'",
# exited 0, and wrote no dll. Naming the file as a prerequisite or forcing the
# recipe with -B is what makes the second build happen.
nvmidi-noarr.dll: $(SOURCES) src/nvmidi.h $(TARGET)
	$(CXX) $(CXXFLAGS) -DNVGT_SKIP_ARRAY_ADDON $(INCLUDES) $(SOURCES) -o $(TARGET) $(LDFLAGS) $(LIBS)
