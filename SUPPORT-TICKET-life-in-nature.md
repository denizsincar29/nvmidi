# Support ticket — Life in Nature

**To:** the developer of *Life in Nature*
**From:** Дениз (@denizsincar29 on Telegram, denizsincar29 on GitHub)
**Date:** 2026-10-03

## What I would like

I would like to play in-game pianos — the ones in houses and villas — from a
real MIDI keyboard plugged into the computer, not only from the PC keyboard.
I made a library for MIDI in NVGT for exactly this, and I am willing to help
integrate it into your game.

## Why it is worth it

A MIDI keyboard sends the same note messages a PC keyboard does, plus two
things a PC keyboard has no way to express: **how hard** a key was struck
(velocity, 1–127) and **when** it was released. Right now the game can only
know "a key went down" and "a key went up". With MIDI, a piano can be played
the way a piano responds — soft passages and hard ones, and a note that decays
the moment the finger leaves the key instead of hanging until the next note.

## What I am asking for

**1. MIDI input device selection in settings.**
Next to the audio device list, a list of the available MIDI input devices, so
the player can pick the keyboard once and have the game remember it. Ports on
Windows are enumerated with `midiInGetNumDevs()` / `midiInGetDevCaps()`, or by
the library I made, which already does this and lists the ports by name.

**2. Playing dynamics: velocity becomes volume.**
When a key is struck with velocity 100, the sound in the sound pool should
play at the corresponding loudness — a note struck at 40 should be noticeably
quieter than the same note struck at 120. Practically this means scaling the
note's volume by velocity / 127 (or, to sound closer to a real piano, by a
curve like (velocity / 127)²).

**3. Realistic release: fade out on key up.**
When the key is released, the note should fade down over a short time — a few
hundred milliseconds — instead of stopping dead or sustaining. At the moment a
released note sounds as if the sustain pedal is held down; the fade is what
makes it sound like a real instrument.

**4. The sustain pedal suspends the fade.**
When the sustain pedal is down (MIDI CC 64), a released key must **not** fade
out and must **not** stop — the note keeps sounding, exactly as a real piano
does, and only fades when the pedal comes back up. The pedal arrives as
controller change 64 with value ≥ 64 for down and < 64 for up.

## Technical notes for the developer

- I made an NVGT library for MIDI input and output, so the integration does not
  need a new MIDI dependency in the engine:
  github.com/denizsincar29/nvmidi (releases carry a prebuilt `nvmidi.dll`).
  It exposes input and output ports, note on / note off with velocity,
  controller changes (including the sustain pedal), pitch bend, and reads
  messages one at a time in the game loop.
- If the game already exposes a way to set a sound's volume and to stop it,
  the whole feature is three things: read port list into settings; on note on,
  play the sample with volume scaled by velocity; on note off, schedule a fade
  (unless CC 64 is down).
- The pitch mapping is the one MIDI always uses: note number 60 is middle C
  (C4), each step is one semitone.

## If you have questions or want to try it

Ask me in the game's DMs, or on Telegram at **@denizsincar29**. I am happy to
help build the piano out properly myself — and in particular, if you can give
me the sound for each key, I will do the integration work.

Keep up the good work — the pianos are a lovely touch and this would make them
playable.
