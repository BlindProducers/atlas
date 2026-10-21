# Maim source

The Maim effect runs the engine of Maim by Arden Butterfield
(https://github.com/ArdenButterfield/Maim, GPL v3, commit 985ed20, with the
GSM and LPC encoders Alessio added to it), a plug-in that circuit bends real
MP3 and Opus encoders. The files in this folder are Atlas's port of Maim's
controllers, GPL v3 or later like the rest of Atlas; the codecs they drive
are kept as libraries under `third_party`:

| Atlas | Maim |
| --- | --- |
| `maim_common.h` | `parameterIds.h`, the `Encoder` enum of `Mp3ControllerManager.h` |
| `queue_buffer.h`, `packet_loss_model.h` | `QueueBuffer.h`, `CodecControllers/PacketLossModel.h` |
| `frame_codec.h` | the framing of `Mp3ControllerManager::processBlock` and `OpusController::processBlock` |
| `mp3_codec.*`, `blade_codec.cpp` | `CodecControllers/MP3Controller.*`, `LameController.*`, `BladeController.*` |
| `mp3_manager.*` | `Mp3ControllerManager.*` |
| `opus_codec.*` | `CodecControllers/OpusController.*` |
| `gsm_codec.*`, `lpc_codec.*` | `CodecControllers/GsmController.*`, `LpcController.*`, with the framing and packet loss of `ResamplingController.*` |
| `resampler.*` | Atlas's own; Maim's `ResamplingController` interpolated linearly behind Butterworth filters |
| `maim_engine.*` | `CodecControllerManager.h` and `PluginProcessor.cpp` (drive, the post filters, makeup, dry/wet) |
| `third_party/maim/lame` | `lib/lame`: libmp3lame 3.99.5 and mpglib with Maim's bends (LGPL 2 or later) |
| `third_party/maim/blade` | `lib/blade/bladeenc`: BladeEnc 0.94.2 with Maim's bends (LGPL 2.1 or later) |
| `third_party/gsm` | `lib/gsm`: libgsm 1.0 patchlevel 22 by Jutta Degener and Carsten Bormann, unchanged |
| `third_party/opus` | `lib/opus`: the Opus submodule at the same commit (5854a9f, 1.5.2 plus 16), unchanged |

Atlas's wrapper is `src/synthesis/modules/maim_module.*`.

## What the effect exposes

Encoder is Maim's codec menu in Maim's order: Blade and LAME (the MP3
encoders), Opus, GSM and LPC. Bitrate is Maim's list, 8 to 320 kbps, which
Blade rounds up to 32 and Opus halves (Maim sets Opus to 500 bits a second
per listed kilobit). Turbo, Tilt (Maim's threshold bias) and Error are
Maim's; Error is cubed for the packet codecs, as in Maim.

Tilt and the butterfly act on long MDCT blocks, as in Maim: both encoders
code transients as short blocks, which neither bend touches, so a sawtooth
or a drum loop hears less of them than a pad does.

The MP3 bends are Maim's, by Maim's names where it had them: Frequency Shift
and Level Shift (Maim's horizontal and vertical shift of the MDCT, the level
shift scaled a hundredth up and a twentieth down), Feedback, the two
Butterfly amounts (Maim's drag box: straight is its x, crossed its y), and
for LAME alone Band Step and Window Step (Maim's second drag box) with Band
Invert, which Maim's parameter list has but its window never showed.

Maim's band reassignment matrix (twenty bands, each choosing its source
band) is not ported. Band Shift and Band Reverse stand in for its shift and
reset buttons: every band reads the band so many above or below it, wrapping
round, or the bands read from the top down. Randomize has no equivalent.

Packet loss is Maim's model: Loss Period is the cycle length in seconds
(Maim's rate control, cubed and scaled to 50 ms to 2.05 s), Loss Width the
delivered share of it, Loss Jitter as in Maim, and Lost Packets is Maim's
Loss/Stick switch. Opus, GSM and LPC each keep their own pattern at their
own rate. Maim's beat sync parameter did nothing and is left out.

LPC Sample Rate is the list from the LPC controller, capped at the
processing rate. Drive, Low Cut, High Cut, Makeup and Mix are Maim's sidebar.

## Changes

- Atlas runs its effects at an oversampled rate, so each codec runs at its
  own rate behind a windowed-sinc resampler (`resampler.*`): the MP3
  encoders at 32, 44.1 or 48 kHz, chosen as the processing rate's own rate
  when it is one of those or their doublings; Opus at 48 kHz; GSM at 8 kHz;
  LPC at its chosen rate, held sample by sample on the way back up as Maim's
  LPC controller does. Maim fed every codec the host's samples as they came
  and told Opus they were 48 kHz. At 44.1 and 48 kHz the MP3 path is Maim's
  exactly, resamplers and all bypassed.
- The latency the host is told follows the encoder: 2881 samples for LAME
  and 2209 for Blade at their rates (Maim measured 2880 and 2209; a
  cross-correlation with sub-sample interpolation puts LAME's peak exactly
  one sample later, at both 44.1 and 48 kHz), Opus's frame plus its
  lookahead (Maim reported zero), the codec's frame for GSM and LPC, and the
  resamplers' delay on top where they run. The dry signal is delayed by the
  same amount for the mix, and moves between two taps when the latency changes.
- A change of encoder or bitrate within MP3 is Maim's swap: the new encoder
  is primed on the last two frames and crossfaded in over one. Maim built it
  on the audio thread; here the manager builds it on a thread of its own
  while the old encoder keeps playing, and the first encoder is built the
  same way, so the audio thread never allocates. A change to or from Opus,
  GSM or LPC, or of the LPC rate, starts the new codec on a second path and
  crossfades to it once its pipeline has filled; Maim switched abruptly.
- The Error bend's shuffles used `rand()`, one generator for the whole
  process; `third_party/maim/lame/libmp3lame/encoder.c` and
  `third_party/maim/blade/bladeenc/codec.c` carry one per encoder
  (`bend_random`), the only change to the libraries. The packet codecs use
  the same kind of generator in place of `juce::Random`.
- The MP3 encoders report frames they cannot code or decode as silence
  instead of printing to the console.
- LAME builds from one configuration header, `atlas_config/config.h`, on
  every platform, without the SSE quantizer or the fast logarithm its
  Windows makefile turns on, so the encoder decides the same way on macOS and
  Windows. The libraries build without FMA contraction for the same reason.
- Everything but LAME has a reset. Atlas rebuilds the effect when a slot
  resets it, so none is needed in place.

## Tests

`AtlasTimingTests --maim` (`ctest -R atlas_maim`) checks the resampler's
gain, band and delay; that every codec's measured latency matches what it
reports at 44.1, 48, 88.2 and 96 kHz, and that Opus reports the lookahead
the latency assumes; MP3 bitrate and encoder swaps offline and on the
build thread; crossfades between every codec and LPC rate; that each bend
reaches LAME and Blade and the LAME-only bends leave Blade alone; Makeup,
Mix and the filters; packet loss in both modes and Error on the packet
codecs; the LPC vocoder's packing, pitch and level; the module's controls,
lanes and latency; a Maim slot in the plug-in; and the editor's labels.
`AtlasMacroTests --editor-accessibility` checks the encoder menu, the row
groups each encoder shows and the readouts. There is no oracle test
against Maim itself: its controllers are bound to JUCE's parameter tree.
