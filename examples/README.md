# nvmidi examples

Short scripts to run, read and copy from. Each one opens a window, prints and speaks what it is doing, and stops when you press escape.

## Start here

1. Plug in your keyboard or start your synthesiser.
2. Run **list_ports.nvgt**. Choose your input and output with the arrow keys and press enter.
3. Run any other example.

## The scripts

- **list_ports.nvgt**: choose your MIDI devices. Saves your choice in `midi_config.json`.
- **music_quickstart.nvgt**: plays a chord, an arpeggio and a scale. The best one to read first.
- **play_chord.nvgt**: one chord played in every pattern. Space plays it again.
- **player.nvgt**: two ways to play a note: let the plugin hold it, or start and stop it yourself.
- **sound_probe.nvgt**: plays four notes and checks the timing. Use it when you are not sure sound works.
- **echo_monitor.nvgt**: speaks everything your keyboard sends. Good for finding out what a knob or pedal does.
- **octave_up_forwarder.nvgt**: plays what you play on the keyboard on the output, one octave higher.
- **queue_limits.nvgt**: shows what happens when a script reads messages too slowly.

## Choosing a different device

The examples read `midi_config.json`, which `list_ports.nvgt` writes for you. You can also edit it by hand:

    { "match": "nord", "in_port": 0, "out_port": 0 }

`match` is part of a device's name (capitals don't matter). If no device has that name, the port numbers are used instead.

The file has to be in the folder you start the script from. If every example picks the wrong device, that is the first thing to check.

## Ideas to try

- In `music_quickstart.nvgt`, change `synth.tempo` and listen.
- In `play_chord.nvgt`, change `"C4", "E4", "G4"` to other note names.
- In `octave_up_forwarder.nvgt`, change `12` to `7` to play a fifth higher.

Every function is described in [../doc/API.md](../doc/API.md).
