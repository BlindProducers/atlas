# ZL Compressor source

The Analogue Compressor effect runs the compressor engine of ZL Compressor by
zsliu98 (https://github.com/ZL-Audio/ZLCompressor, commit c11049e, October
2026). ZL Compressor is AGPL v3 (`LICENSE.zlcompressor.md`); the files in this
folder keep that licence, which GPL v3 section 13 permits combining with Atlas.
Atlas's wrapper, `src/synthesis/modules/analogue_compressor_module.*`, is GPL v3
or later.

| Atlas | ZL Compressor |
| --- | --- |
| `zl_compressor_dsp.h` | `source/dsp/chore/decibels.hpp`, `vector/vector_transform/vector_mag_to_db.hpp`, `compressor/computer/compression_computer.hpp`, `compressor/follower/ps_follower.hpp`, `compressor/styles/{clean,classic,optical,vocal}.hpp` |
| `zl_compressor_engine.*` | `source/zlp/compress_controller.cpp` (`prepareBuffer`, `process`, `processBuffer`, `appleSideBuffer`), `compress_attach.cpp` (how parameters reach the DSP), `source/dsp/splitter/inplace_ms_splitter.hpp` (`GainMode::kPre`) and `source/dsp/gain/gain.hpp` with `chore/smoothed_value.hpp` (`kFixLin`) |

## What the effect exposes

Mode is ZL's Compressor Style (Clean, Classic, Optical, Vocal). Threshold,
Ratio, Knee (ZL's knee width), Attack, Release, Makeup and Mix (ZL's Wet) are
ZL's controls with ZL's meaning:

- Threshold runs from -60 to 0 dB (ZL: -100 to 0) and defaults to -18 dB (ZL:
  -32), a better start for a synth's level. Ratio runs from 1:1 to 100:1 and
  Knee from 0 to 32 dB, as in ZL; Ratio defaults to 3:1 and Knee to 8 dB, ZL's
  defaults.
- Attack runs from 0 to 1000 ms and Release from 0 to 5000 ms in ZL's units
  (the follower's coefficient is exp(-2 pi 1000 / (rate * ms))), with ZL's
  defaults of 100 and 500 ms. Atlas stores the cube root of the time, so short
  times get fine keyboard steps; Ratio is stored as its log2.
- Makeup runs from -30 to 30 dB. Mix is ZL's Wet, from 0 to 100%: it scales the
  gain change, the 80 dB range and the makeup in dB, so 0 is the dry signal.

Everything else stays at ZL's defaults: the side chain is the input itself
(no external side chain, side EQ or side gain), Stereo Mode is Mid Side with
0% link and no swap, Range is 80 dB, Curve is 0 (a straight ratio above the
knee), and Pump, Smooth, Hold, RMS, Lookahead, Oversampling, the Clipper,
Delta and the loudness matcher are off. Those parts of ZL are not ported.

## Changes

- The atomics and the cross-thread update flags are gone: Atlas sets every
  parameter on the audio thread before a block, and the computer and follower
  recompute their coefficients when a value changed.
- Highway's vector loops are plain loops. ZL's vector paths use Highway's
  `Log`, `Exp` and `Sqrt`; the port uses `std::log` and `std::exp`, as ZL's
  scalar tails do, so results differ from ZL's in the last bits.
- Only the compression computer with `OutputDiff` is ported (no expansion or
  inflation directions); the follower has no Pump or Smooth states; the styles
  have no RMS tracker overloads.
- The stereo link at 0% is left out rather than computed: ZL's
  `y + 1 * (x - y)` is `x` up to rounding.
- The Vocal style's follower resets to unity gain (-1, as it holds the negated
  gain). ZL resets it to 0, which fades the output in from silence over the
  release time after every reset.
- Changing Mode starts the new style's followers from the gain change the old
  one was applying (for Optical, from the level the computer maps to that
  change). ZL resets them to no gain change, which jumps the gain, a click,
  while compressing. After a reset both start from no gain change.
- The engine processes in chunks of 256 samples with stack buffers, so it
  never allocates while playing.

## Tests

`AtlasAnalogueTests --compressor` (`ctest -R atlas_analogue_compressor`)
checks each style's static curve against the formula (the feed-forward styles
land on the curve, the feedback styles where their own output meets it), that
levels below the threshold pass unchanged, Mix and Makeup, mid/side
detection, attack and release, click-free mode changes between every pair of
styles, no allocation while processing, lanes 2-3, and every effect chain.
There is no committed oracle test: ZL's styles need its Highway fork. While
porting, a one-off harness built ZL's own headers at commit c11049e
(`compressor/`, `splitter/`, `gain/`, with Highway's static target) into ZL's
default controller path, with Atlas's Vocal reset, and compared it with this
engine on two seconds of stereo test signal in 128-sample blocks: every style,
four settings each (hard and soft knees, ratios 1.5 to 100, attack and release
from 0 to ZL's maximums, Wet 25% to 100%). The worst difference was 135 dB below
the signal's peak, float rounding.
