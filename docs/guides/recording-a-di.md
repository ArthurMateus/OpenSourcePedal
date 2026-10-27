# Recording a DI for tone match

Tone match ([ADR-0008](../adr/0008-tone-match.md), #117) needs a recording of **your own guitar**, played dry. It renders
that recording through the plugin's blocks to find a setting that sounds like the guitar in a song. This guide is what
an agent tells the owner (or any user) before a tone-match run, so the recording is usable the first time.

> The tone-match code, its ADR and `tools/tonematch/` arrive with PRs #118–#121. Until they merge, find them on those
> PR branches (the top of the stack is `dsp/117-tone-sections`).

## What a DI is

A **DI** (direct input) is the raw signal straight from the guitar: **no amp, no amp sim, no pedals, no effects**. It
is not "a clean tone". The tool supplies the tone, so it needs the plain guitar to start from.

Why it matters: the first real-song test used a stand-in DI decoded from a lossy preview. It had almost no treble
(2–6 kHz about 34 dB below its midrange), and every clean match came out muffled. A clean match can only pass on the
treble your DI has. `osp-tonematch` now prints every DI's treble reading and warns when it is too dark.

## Recording checklist

- Plug the guitar **straight into the audio interface's instrument (Hi-Z) input**. No pedals in between.
- Record the track **without any plugins on it**. Monitoring through an amp sim is fine as long as the recorded
  track stays dry (see FL Studio below).
- Save as **WAV**, 24-bit (32-bit float is fine too), 44.1 or 48 kHz. **Not MP3**, and not a phone or room
  recording.
- Set the level so the loudest hits **peak around −12 dBFS** and never clip. Adjust the **gain knob on the
  interface**, not the software fader.
- Guitar **volume and tone knobs fully up**.
- **Tune first.**

## What to play

Ideally one short file (20–60 s) per song, playing something close to that song's guitar part:

| File | Play | Pickup |
|---|---|---|
| `di-clean.wav` | the clean chords or arpeggios | the one you use for clean (often neck or middle) |
| `di-heavy.wav` | the riff, with palm mutes and power chords | bridge |
| `di-lead.wav` | a lead or melodic part | whichever suits the song |

Playing the same *kind* of part matters: the gain estimate compares playing dynamics, and a strummed DI matched to a
single-note part gives the wrong gain (ADR-0008, "Consequences and limits"). If one file per song is too much, a
single file that mixes clean chords, a palm-muted riff and some single notes is still far better than the bundled clip.

## FL Studio, step by step

FL records whatever passes through a mixer insert, **including its effects**. So record on one insert and listen on
another.

1. **Interface:** press **F10** (Settings), open **Audio**, and choose your interface's **ASIO driver** as the
   Device (named after the interface, otherwise "FL Studio ASIO"). A buffer of about 256 samples is fine.
2. **Recording insert:** press **F9** (Mixer) and click an empty insert, e.g. Insert 1. At the top right of the
   mixer, open the **input** dropdown and choose the interface input the guitar is plugged into. Pick the **single
   mono input** ("In 1"), not a stereo pair. Leave this insert's effect slots **empty**.
3. **To hear a nice tone while playing (optional):** put your amp sim and a tuner on a second insert (Insert 2).
   With Insert 1 selected, use the send arrow at the bottom of Insert 2 to route Insert 1 into it. If you hear the
   dry signal as well, lower or mute Insert 1's direct output. You play through the amp sim; the file stays dry.
4. **Level:** play your hardest part and watch Insert 1's meter. Turn the **interface's gain knob** until peaks sit
   around **−12 dB** with no red.
5. **Record:** click the **record-arm** button at the bottom of Insert 1, and name the file (`di-clean`, …) when FL
   asks. Press **record** in the transport (or **R**). If FL asks what to record, choose audio into the playlist as
   an audio clip. Press **Space** to start, play, and press Space to stop. Repeat for each part.
6. **The files:** FL saves recordings as WAV in its **Recorded** folder: in FL's Browser under *Recorded*, on disk
   usually `Documents\Image-Line\FL Studio\Audio\Recorded`. Send those raw files. **Don't use File → Export**: FL's
   default master track has a limiter that would change the DI.

Other recording software works the same way: a mono track from the instrument input, no plugins on it, WAV out.

## Running the match

- **With an agent:** give it the WAV files (e.g. copy them to Downloads and mention or attach them in the chat), plus
  the song files. It checks each DI's treble reading first, then runs the match and returns presets and A/B renders.
- **Yourself:**

  ```powershell
  python tools/tonematch/tonematch.py --ref song.mp3 --di di-clean.wav --stem --tones --render preview.wav
  ```

  `--stem` needs Demucs (`pip install demucs`) and gives much better results on full mixes. `--tones` gives one preset
  per guitar part and tone. Listen to each `-g<part>-t<tone>.wav` next to its `-ref.wav` (the song's own section).
