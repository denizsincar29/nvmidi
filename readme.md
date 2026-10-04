# nvmidi

MIDI for [NVGT](https://nvgt.dev), the NonVisual Gaming Toolkit.

With nvmidi your NVGT game or app can:

- **play notes and chords** on a synthesiser, a keyboard's built-in sounds or any other MIDI device;
- **listen to a MIDI keyboard**, pad controller or wind controller.

## Install

1. Download the project:

        git clone https://github.com/denizsincar29/nvmidi.git

2. Install the plugin (this downloads the newest version for your NVGT):

        nvgt nvmidi/scripts/get_nvmidi.nvgt

3. Copy `midi.nvgt` from the project into your own script's folder, and add one line at the top of your script:

        #include "midi.nvgt"

That's all. Run step 2 again whenever you want the latest version.

## Try it first

The `examples` folder has ready-to-run scripts. Start here:

1. Plug in your keyboard or start your synthesiser.
2. Run `examples/list_ports.nvgt`. It lets you choose your MIDI devices with the arrow keys and remembers them.
3. Run `examples/music_quickstart.nvgt`. You should hear a chord, an arpeggio and a scale.

## Your first sound

```angelscript
#include "midi.nvgt"

void main() {
	midi_output@ synth = midi_output_create();
	if (!synth.open(0, "my game")) {   // 0 is the first output on your list
		screen_reader_speak("No MIDI output: " + midi_last_error_text());
		return;
	}
	midi_output_play_chord(synth, {"C4", "E4", "G4"}, 100); // names and a volume (0-127)
	synth.close();
}
```

Notes are written as names like `C4` (middle C), `F#3` or `Bb5`.

## Listening to your keyboard

```angelscript
#include "midi.nvgt"

void main() {
	midi_input@ keyboard = midi_input_create();
	if (!keyboard.open(0, "my game")) {   // 0 is the first input on your list
		screen_reader_speak("No MIDI input: " + midi_last_error_text());
		return;
	}

	midi_message m;
	while (!key_pressed(KEY_ESCAPE)) {
		// Read everything that has arrived, every time around the loop.
		while (keyboard.next_message(m)) {
			midi_message_view v(m);
			if (v.is_note_on())
				screen_reader_speak(v.note_name() + ", velocity " + v.velocity());
		}
		wait(5);
	}
	keyboard.close();
}
```

Always read in a loop like the one above, and always call `wait()` in your main loop. If your script reads too slowly, the oldest messages are lost.

## Longer notes and tempo

```angelscript
midi_output@ synth = midi_output_create();
synth.open(0, "my game");
synth.tempo = 96.0;                         // beats per minute

midi_note@ note = midi_note_named("E4", 100);
note.length = synth.duration(1.0, MIDI_BEATS);   // one beat long
synth.play_note_wait(note);                      // plays it, comes back when it ends
```

To play several notes use an array, then pick how they are laid out in time:

```angelscript
array<midi_note@> notes;
notes.insert_last(midi_note_named("C4", 100));
notes.insert_last(midi_note_named("E4", 100));
notes.insert_last(midi_note_named("G4", 100));

synth.play_chord_wait(notes);                         // all together
synth.play_midi_chord_wait(notes, "arpeggio");        // one after another
```

Other patterns: `spread`, `quick`, `fast`, `strum`, `sequence` and `repeat`.

## The examples

| Script | What it does |
| --- | --- |
| `list_ports.nvgt` | Choose your input and output devices. Run this first. |
| `music_quickstart.nvgt` | Plays a chord, an arpeggio and a scale. |
| `play_chord.nvgt` | One chord played in every pattern. Space repeats it. |
| `player.nvgt` | Two ways to play a note. |
| `sound_probe.nvgt` | Four notes and a timing check, to see that sound works. |
| `echo_monitor.nvgt` | Speaks everything your keyboard sends. |
| `octave_up_forwarder.nvgt` | Plays what you play on the keyboard, one octave higher. |
| `queue_limits.nvgt` | Shows what happens when a script reads too slowly. |

More in [examples/README.md](examples/README.md).

## If something doesn't work

- **"No matching symbol"**: the plugin isn't installed. Run the install step again.
- **"No MIDI output" or "Cannot open"**: nothing is plugged in or running. Plug in your device, or start a software synth, and run `list_ports.nvgt`. On Windows the output list normally includes "Microsoft GS Wavetable Synth", which makes sound without any extra hardware.
- **No sound**: run `sound_probe.nvgt`. If it says the timing is right but you hear nothing, check that the output you chose is the one that makes sound.
- **A note keeps ringing**: call `synth.stop_all_notes()`. The `play_note` and `play_chord` functions start notes and return straight away; they don't stop them by themselves. The `_wait` versions do.

## More help

- [doc/API.md](doc/API.md): every function and what it does.
- [TECHNICAL.md](TECHNICAL.md): building the plugin yourself and shipping it with a game.

## Not included

Playing or reading MIDI files, and creating virtual MIDI ports on Windows.

## Credits

Uses RtMidi by Gary P. Scavone. NVGT is by Sam Tupy. See [license.md](license.md).
