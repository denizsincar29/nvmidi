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

# Backend selection. RtMidi compiles exactly one of these in.
ifeq ($(OS),Windows_NT)
    TARGET    = nvmidi.dll
    CXXFLAGS += -D__WINDOWS_MM__
    LDFLAGS   = -shared -static-libgcc -static-libstdc++ -Wl,--kill-at
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
