# nvmidi-tester - build with: make   (from this folder)
#   Linux   needs libasound2-dev
#   macOS   needs nothing extra (CoreMIDI)
#   Windows needs MinGW-w64 (mingw32-make)
RTMIDI = ../../third_party/rtmidi
CXX   ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
SRC = nvmidi_tester.cpp $(RTMIDI)/RtMidi.cpp
INC = -I$(RTMIDI)

ifeq ($(OS),Windows_NT)
  TARGET = nvmidi-tester.exe
  CXXFLAGS += -D__WINDOWS_MM__ -static
  LIBS = -lwinmm
else ifeq ($(shell uname -s),Darwin)
  TARGET = nvmidi-tester
  CXXFLAGS += -D__MACOSX_CORE__
  LIBS = -framework CoreMIDI -framework CoreAudio -framework CoreFoundation
else
  TARGET = nvmidi-tester
  CXXFLAGS += -D__LINUX_ALSA__
  LIBS = -lasound -lpthread
endif

all: $(TARGET)
$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(INC) $(SRC) -o $@ $(LIBS)
clean:
	rm -f nvmidi-tester nvmidi-tester.exe
