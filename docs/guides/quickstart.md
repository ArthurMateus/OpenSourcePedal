# OpenSourcePedal: quick start

## Run it
1. Double-click `OpenSourcePedal.exe`. The first start takes up to 20 seconds.
2. Click **Settings...** (top right) and pick your audio interface as input and output. Use headphones
   or an interface: with a laptop mic and speakers the app mutes the input to avoid feedback.
3. Pick a sound from the preset bar at the top (clean, crunch, high-gain, lead, ambient), or turn the
   blocks on and off in the row below it (Input, Gate, Comp, Drive, Amp, Cab, EQ, Mod, Delay, Reverb,
   Doubler, Output). Click a block to see its knobs. **Save** stores your own preset.
4. The VST3 plugin is in the `VST3` folder. Copy `OpenSourcePedal.vst3` to `C:\Program Files\Common Files\VST3`.

## Match the tone of a song
You need two things:
- **The song**: a file you have the right to use (WAV, MP3 or FLAC), or a link if you have `yt-dlp` and `ffmpeg`
  installed yourself (they are not included). Only download recordings you have the right to.
- **Your DI**: a dry recording of your own guitar, with no amp, no effects. See `docs/recording-a-di.md`
  (FL Studio steps included). The match is only as good as this recording. Without it the bundled test
  clip is used, which is dark and makes clean tones sound muffled.

In the **Tone match** panel on the right: choose the song and your DI, type the start and end of a
guitar-heavy part (for example `1:05` and `1:40`), tick "isolated guitar" only if the file is a guitar
stem, then press **Match**. Wait for 100 %, press **Audition** to hear it, then **Save preset**.

## What to expect (honest limits)
- It finds the closest amp, gain, tone controls, cab and EQ it has. That is close, not identical to the record.
- Gain depends on how you play: matching a track played on a different guitar can give the wrong amount of gain.
- A dark or lossy DI makes clean matches muffled. A guitar-only stem gives the best result.
- Drums and vocals in the song confuse the match. Pick a section where the guitar is prominent.

## Command line
`osp-tonematch.exe --ref song.wav --di my-di.wav --out preset.json --render preview.wav --start 65 --end 100`
