# nvmidi license

nvmidi - MIDI input and output for NVGT
Copyright (c) 2026 Дениз

This software is provided "as-is", without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the
use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim
   that you wrote the original software. If you use this software in a
   product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

## Third party components

**RtMidi** (`third_party/rtmidi`) - realtime MIDI i/o C++ classes.
Copyright (c) 2003-2023 Gary P. Scavone. Distributed under a permissive
MIT-style license; see `third_party/rtmidi/LICENSE`. RtMidi provides the
platform layer: WinMM on Windows, ALSA on Linux, CoreMIDI on macOS.

**AngelScript SDK headers** (`third_party/angelscript`) - `angelscript.h` and
the `scriptarray` addon header.
Copyright (c) 2003-2026 Andreas Jonsson. Distributed under the zlib license.
These are interface headers only; the AngelScript runtime itself is provided
by NVGT at load time and is never linked into this plugin.
