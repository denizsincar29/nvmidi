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
# The flag. NVGT_SKIP_ARRAY_ADDON is read in src/nvmidi.cpp; it is added to a
# copy of the local CXXFLAGS rather than passed on the make command line,
# because a command-line CXXFLAGS= replaces the whole variable and silently
# drops -D__WINDOWS_MM__ with it - and a MinGW build without that define takes
# the Linux RtMidi branch and then includes a source file in the checkout root
# called Windows.h on a case-insensitive filesystem. That build produces no dll
# at all and exits 0 while doing it (measured, run 36702495507).
#
# The header prerequisite is load-bearing. This file's own target used to be
# listed in SOURCES, and on a case-insensitive filesystem `nvmidi-noarr.dll`
# resolves to `nvmidi.dll` - so the target was its own prerequisite and make
# refused it ("Circular dependency dropped"), which it does without failing, so
# the step went green having built nothing.
nvmidi-noarr.dll: $(SOURCES) src/nvmidi.h
	$(CXX) $(CXXFLAGS) -DNVGT_SKIP_ARRAY_ADDON $(INCLUDES) $(SOURCES) -o $@ $(LDFLAGS) $(LIBS)
