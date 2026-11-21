# AtlasBench

AtlasBench measures how much CPU Atlas builds use and compares them. It loads
each build as a VST3, so it can time any `Atlas.vst3`: the build you just
made, a copy of an earlier build, or a release downloaded from GitHub. It does
not link Atlas, so building it compiles only JUCE and one source file.

## Building

```sh
cmake -S . -B build -DATLAS_BUILD_BENCH=ON
cmake --build build --config Release --parallel 4 --target AtlasBench
```

The program is `build/AtlasBench_artefacts/Release/AtlasBench.exe` on Windows
and `build/AtlasBench_artefacts/Release/AtlasBench` on macOS.

## Keeping builds to compare against

Copy a build's whole `Atlas.vst3` bundle into `bench/baselines/<name>/`, which
git ignores. Name it after the commit or release it came from:

```powershell
robocopy build\Atlas_artefacts\Release\VST3\Atlas.vst3 bench\baselines\e0b5035\Atlas.vst3 /MIR
gh release download v2.0.0-beta.2 -R blindproducers/atlas -p "*Windows-VST3.zip" -D bench\baselines\v2.0.0-beta.2
```

(Unzip the release into the same folder.)

## Comparing builds

List the builds with `--plugin`. The first is the baseline, and the others are
compared with it:

```powershell
build\AtlasBench_artefacts\Release\AtlasBench.exe `
  --plugin bench\baselines\e0b5035\Atlas.vst3 `
  --plugin build\Atlas_artefacts\Release\VST3\Atlas.vst3 `
  --out bench\results\my-change.json
```

It prints one line per scenario as it goes, then a table:

```
scenario              [A]         [B]         vs A      p99 block [A] -> [last]
init                  5.35 %      3.95 %      -26.2 %   8.4 % -> 6.3 %
...
[B] vs [A], geometric mean over 16 scenarios: -13.9 %
```

- **Load** is the time spent in `processBlock` as a share of the audio's
  duration, on one core. 4 % means one second of audio took 40 ms to render.
  Lower is better. A build's figure is its best round.
- **vs A** is the change from the baseline. Negative means faster.
- **p99 block** is the 99th-percentile time of a single block as a share of
  the block's duration (the median across rounds). Spikes there cause dropouts
  even when the average load is low.
- The **geometric mean** sums up the whole table in one figure.

`--save-audio <dir>` also keeps each build's timed audio and prints how far
each build is from the first, in dB below its level (`identical` when every
sample matches). Rounding differences from a change of FFT or SIMD code land
around -110 to -140 dB, far below hearing. A scenario that already varies
between rounds of one build (Convolution Reverb, whose background IR loading
finishes at different moments) differs for that reason alone; compare a build
with itself to see how much.

`--compare a.json b.json` prints saved results side by side. Saved results
come from whatever the machine was doing that day, so for a decision, time
both builds in one run.

## Timing a load

`--load` times what a user waits for instead of the audio: a host loading the
plugin and opening its window. All of it runs on the host's message thread,
and while that thread is held the host's own window stands still and a screen
reader says nothing, so on Windows with NVDA a slow load is a silence.

```powershell
build\AtlasBench_artefacts\Release\AtlasBench.exe --load `
  --plugin bench\baselines\e0b5035\Atlas.vst3 `
  --plugin build\Atlas_artefacts\Release\VST3\Atlas.vst3
```

Each build runs in its own process each round, as above. In it AtlasBench
loads the plugin, prepares it, saves its state and restores it (as a host
does for a project), opens the editor in a window, closes it and opens it
again, then does the same with a second instance beside the first, which is
what every further instance in a project costs. An audio thread plays the
first instance meanwhile. It prints each step's median, best and worst time,
what the process had committed after it (Windows), and every stretch of 20 ms
or more that one message held the message thread while an editor was open.

- The window opens without taking the keyboard focus, so a running screen
  reader ignores it. `--focus` brings it to the front and focuses it, as a
  host does; the screen reader then reads it, and the time the plugin spends
  answering its questions is in the figures. Warn whoever is using the
  computer first.
- `--set name=value` changes a control in the state that is restored, to time
  a patch other than the initial one (`--set osc_1_type=2` for a Sample slot).
- `--settle` is how long each open editor is watched (1500 ms).
- The first instance pays for what the process does once: loading the
  library, its static tables, the description of every effect type.

### Where the time goes (Windows)

`--profile <file>` samples the message thread's call stack about a thousand
times a second through the first build's first round and writes the raw
addresses. `bench/symbolize_profile.py` names them: the plugin's functions
from the linker's map file, which needs no debug build, and the functions of
system libraries from their export tables.

```powershell
cmake -S . -B build "-DCMAKE_MODULE_LINKER_FLAGS_RELEASE=/INCREMENTAL:NO /MAP:C:/temp/Atlas.map"
cmake --build build --config Release --parallel 4 --target Atlas_VST3
build\AtlasBench_artefacts\Release\AtlasBench.exe --load --rounds 1 `
  --plugin build\Atlas_artefacts\Release\VST3\Atlas.vst3 --profile C:\temp\load.txt
python bench\symbolize_profile.py C:\temp\load.txt --map C:\temp\Atlas.map --stage instantiate
```

For each stage it prints the functions the samples were in, the plugin
functions on the stacks (each counted once a sample, so a caller shows the
whole of what it called), and the commonest stacks. `--grep <text>` prints
the whole stacks, every library, that have a matching function on them.

## How it measures

Each round runs every build once, alternating (A, B, A, B, ...), each in its
own process. Alternating means a CPU heating up or a background task affects
every build alike. In each run AtlasBench:

1. loads the plugin at 48 kHz with 128-sample blocks,
2. for each scenario, takes the build's own initial patch, changes the
   controls the scenario lists, loads that state, and gives background work
   1.5 s to finish,
3. renders 1 s untimed, then times 6 s of audio block by block on a
   high-priority thread while the main thread keeps dispatching messages, as
   a host's message thread would. A chord sounds every second and is held for
   0.8 s.

If a build drops a control the scenario sets (an effect it doesn't have, say),
the line says `not supported` and the table marks it, since that build
measured something else. A scenario that comes out silent or non-finite is
flagged too.

## Scenarios

`AtlasBench --list` prints the standard set: the initial patch, wavetable and
analog unison, Analog Hard Sync, Braids, OpenWurli and OpenB3, three filter pairs, and six effect
chains. `--effects` measures each effect alone instead, and `--filters` each
voice filter model alone, plus the Reverb model at 2 kHz (its default 100 Hz
cutoff skips its FIR) and on single notes, the Scream model driven hard
without autogain, and Scream and Reverb as a Filter FX; `--effects` also has Spectral Lock in its Spectral
mode, the one that runs an FFT. `--list --effects` and `--list --filters` print them.
`--only "fx-*,init"` picks scenarios by name.

## Getting steady numbers

On a quiet machine, most scenarios repeat within about 2 % from round to round,
so treat smaller differences as noise. To keep the noise down:

- plug a laptop in and choose the Best performance power mode,
- close DAWs and browsers,
- use `--rounds 5` when the difference you are looking for is small,
- try `--core 2` (or another core that isn't core 0) to keep the audio thread
  on one core.

Other options: `--seconds`, `--block`, `--rate`, `--warmup`, `--settle`.
`AtlasBench --help` lists them all.
