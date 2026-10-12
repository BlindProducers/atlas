// SPDX-License-Identifier: GPL-3.0-or-later
//! Reference renderer for the OpenWurli tests: runs the original openwurli-dsp
//! crate (third_party/openwurli, built by build_reference.py) and writes raw
//! little-endian samples to stdout. Never linked into Atlas.
//!
//! Every command takes --out PATH (default stdout); chain takes --in PATH
//! (default stdin).
//!
//!   voice  --note N --velocity V --seconds S --rate R [--mlp] [--seed U] [--off S] [--chunk N]
//!          [--swing-scale X] [--air-drag-scale X] [--voicing-slope X] [--zero-trim] [--c2-emitter]
//!          One Voice (reed, hammer, pickup) with that CalibrationConfig, f64 samples in volts.
//!   chain  --rate R [--volume V] [--tremolo D] [--speaker C] [--chunk N]
//!          [--changes t:volume:v,t:tremolo:d,t:speaker:c,...]
//!          The engine's shared chain (oversampled preamp with the tremolo,
//!          volume network, power amp, speaker) on f64 input read from stdin,
//!          f64 output, as WurliEngine::render computes it after set_sample_rate.
//!          --changes calls the engine's set_volume / set_tremolo_depth /
//!          set_speaker_character at time t (seconds, floored to a sample).
//!   engine --rate R --seconds S [--volume V] [--tremolo D] [--speaker C] [--mlp]
//!          [--chunk N] [--events t:on:note:velocity,t:off:note,...]
//!          WurliEngine end to end (f32 samples, before the plugin's +14 dB).

use std::io::{Read, Write};

use openwurli_dsp::dk_preamp::DkPreamp;
use openwurli_dsp::engine::WurliEngine;
use openwurli_dsp::oversampler::Oversampler;
use openwurli_dsp::power_amp::PowerAmp;
use openwurli_dsp::preamp::PreampModel;
use openwurli_dsp::speaker::Speaker;
use openwurli_dsp::tables;
use openwurli_dsp::tremolo::Tremolo;

struct Args(Vec<String>);

impl Args {
    fn value(&self, name: &str) -> Option<&str> {
        self.0
            .iter()
            .position(|a| a == name)
            .and_then(|i| self.0.get(i + 1))
            .map(|s| s.as_str())
    }
    fn f64_or(&self, name: &str, default: f64) -> f64 {
        self.value(name).map(|v| v.parse().expect(name)).unwrap_or(default)
    }
    fn flag(&self, name: &str) -> bool {
        self.0.iter().any(|a| a == name)
    }
}

fn write_bytes(args: &Args, bytes: &[u8]) {
    match args.value("--out") {
        Some(path) => std::fs::write(path, bytes).unwrap(),
        None => std::io::stdout().write_all(bytes).unwrap(),
    }
}

fn write_f64(args: &Args, samples: &[f64]) {
    let mut bytes = Vec::with_capacity(samples.len() * 8);
    for s in samples {
        bytes.extend_from_slice(&s.to_le_bytes());
    }
    write_bytes(args, &bytes);
}

/// One Voice from `$dsp`: openwurli_dsp, or openwurli_dsp_emitter, the same crate built with
/// pickup.rs's C2_RETURNS_TO_GROUND false (build_reference.py).
macro_rules! render_voice {
    ($dsp:ident, $args:expr) => {{
        let args: &Args = $args;
        let note = args.f64_or("--note", 60.0) as u8;
        let velocity = args.f64_or("--velocity", 0.8);
        let rate = args.f64_or("--rate", 44_100.0);
        let total = (args.f64_or("--seconds", 1.0) * rate) as usize;
        let seed = args.f64_or("--seed", 0.0) as u32;
        let chunk = (args.f64_or("--chunk", 256.0) as usize).max(1);
        let off = args.value("--off").map(|v| (v.parse::<f64>().unwrap() * rate) as usize);
        let defaults = $dsp::tables::CalibrationConfig::default();
        let cfg = $dsp::tables::CalibrationConfig {
            swing_scale: args.f64_or("--swing-scale", defaults.swing_scale),
            air_drag_scale: args.f64_or("--air-drag-scale", defaults.air_drag_scale),
            voicing_slope: args.f64_or("--voicing-slope", defaults.voicing_slope),
            zero_trim: args.flag("--zero-trim"),
            ..defaults
        };

        let mut voice =
            $dsp::voice::Voice::note_on_with_config(note, velocity, rate, seed, args.flag("--mlp"), &cfg);
        let mut out = vec![0.0f64; total];
        let mut start = 0;
        while start < total {
            let mut end = (start + chunk).min(total);
            if let Some(off) = off {
                if start < off && off < end {
                    end = off;
                }
                if start == off {
                    voice.note_off();
                }
            }
            voice.render(&mut out[start..end]);
            start = end;
        }
        out
    }};
}

fn voice(args: &Args) {
    let out = if args.flag("--c2-emitter") {
        render_voice!(openwurli_dsp_emitter, args)
    } else {
        render_voice!(openwurli_dsp, args)
    };
    write_f64(args, &out);
}

/// engine.rs LinearSmoother, as WurliEngine ramps volume, depth and character.
struct Smoother {
    current: f64,
    target: f64,
    step: f64,
    remaining: u32,
    ramp: u32,
}

impl Smoother {
    fn new(value: f64, ramp: u32) -> Self {
        Self { current: value, target: value, step: 0.0, remaining: 0, ramp }
    }
    /// LinearSmoother::set_target.
    fn set_target(&mut self, target: f64) {
        if (target - self.target).abs() < 1e-9 {
            return;
        }
        self.target = target;
        let delta = target - self.current;
        if self.ramp == 0 {
            self.current = target;
            self.remaining = 0;
            return;
        }
        self.step = delta / self.ramp as f64;
        self.remaining = self.ramp;
    }
    fn next(&mut self) -> f64 {
        if self.remaining > 0 {
            self.current += self.step;
            self.remaining -= 1;
            if self.remaining == 0 {
                self.current = self.target;
            }
        }
        self.current
    }
}

fn one_pole_alpha(fc: f64, sr: f64) -> f64 {
    if !fc.is_finite() || fc >= 0.5 * sr {
        1.0
    } else {
        1.0 - (-2.0 * std::f64::consts::PI * fc / sr).exp()
    }
}

/// The shared chain of WurliEngine, fed from `input` instead of its voices.
struct Chain {
    preamp: DkPreamp,
    tremolo: Tremolo,
    oversampler: Oversampler,
    power_amp: PowerAmp,
    speaker: Speaker,
    oversample: bool,
    os_rate: f64,
    c9_state: f64,
    c9_alpha: f64,
    volume: Smoother,
    depth: Smoother,
    character: Smoother,
    r11: f64,
}

impl Chain {
    fn new(rate: f64, volume: f64, depth: f64, character: f64) -> Self {
        let oversample = rate < 88_200.0;
        let os_rate = if oversample { rate * 2.0 } else { rate };
        let ramp = ((rate * 0.005) as u32).max(1);
        let mut chain = Self {
            preamp: DkPreamp::new(os_rate),
            tremolo: Tremolo::new(depth, os_rate),
            oversampler: Oversampler::new(),
            power_amp: PowerAmp::new_at_sample_rate(os_rate),
            speaker: Speaker::new(rate),
            oversample,
            os_rate,
            c9_state: 0.0,
            c9_alpha: 1.0,
            volume: Smoother::new(volume, ramp),
            depth: Smoother::new(depth, ramp),
            character: Smoother::new(character, ramp),
            r11: tables::R11_REED_BAR_VOLUME_DEFAULT,
        };
        // WurliEngine::warm_up: 0.6 s of silence through the chain.
        let total = (rate * 0.6) as usize;
        let silence = vec![0.0f64; 512];
        let mut scratch = vec![0.0f64; 512];
        let mut done = 0;
        while done < total {
            let len = 512.min(total - done);
            chain.process(&silence[..len], &mut scratch[..len]);
            done += len;
        }
        chain
    }

    fn drive(&mut self, preamp_out: f64, drive_gain: f64) -> f64 {
        self.c9_state += self.c9_alpha * (preamp_out * drive_gain - self.c9_state);
        self.c9_state
    }

    fn process(&mut self, input: &[f64], output: &mut [f64]) {
        let len = input.len();
        let mut out_buf = vec![0.0f64; len];
        if self.oversample {
            let mut up = vec![0.0f64; len * 2];
            self.oversampler.upsample_2x(input, &mut up);
            for i in 0..len {
                let depth = self.depth.next();
                self.tremolo.set_depth(depth);
                let vol = self.volume.next();
                let drive_gain =
                    self.preamp.open_circuit_output_factor() * tables::volume_pot_gain(vol, self.r11);
                self.c9_alpha =
                    one_pole_alpha(tables::volume_pot_pole_hz(vol, self.r11), self.os_rate);
                for j in 0..2 {
                    let idx = i * 2 + j;
                    let r_ldr = self.tremolo.process();
                    self.preamp.set_ldr_resistance(r_ldr);
                    let preamp_out = self.preamp.process_sample(up[idx]);
                    let drive = self.drive(preamp_out, drive_gain);
                    up[idx] = self.power_amp.process(drive);
                }
            }
            self.oversampler.downsample_2x(&up, &mut out_buf);
        } else {
            for i in 0..len {
                let depth = self.depth.next();
                self.tremolo.set_depth(depth);
                let r_ldr = self.tremolo.process();
                self.preamp.set_ldr_resistance(r_ldr);
                let preamp_out = self.preamp.process_sample(input[i]);
                let vol = self.volume.next();
                let drive_gain =
                    self.preamp.open_circuit_output_factor() * tables::volume_pot_gain(vol, self.r11);
                self.c9_alpha =
                    one_pole_alpha(tables::volume_pot_pole_hz(vol, self.r11), self.os_rate);
                let drive = self.drive(preamp_out, drive_gain);
                out_buf[i] = self.power_amp.process(drive);
            }
        }
        for i in 0..len {
            let character = self.character.next();
            self.speaker.set_character(character);
            output[i] = self.speaker.process(out_buf[i]) * tables::POST_SPEAKER_GAIN;
        }
    }
}

fn chain(args: &Args) {
    let rate = args.f64_or("--rate", 44_100.0);
    let chunk = (args.f64_or("--chunk", 256.0) as usize).max(1);
    let mut chain = Chain::new(
        rate,
        args.f64_or("--volume", 0.8),
        args.f64_or("--tremolo", 0.5),
        args.f64_or("--speaker", 0.0),
    );
    let bytes = match args.value("--in") {
        Some(path) => std::fs::read(path).unwrap(),
        None => {
            let mut bytes = Vec::new();
            std::io::stdin().read_to_end(&mut bytes).unwrap();
            bytes
        }
    };
    let input: Vec<f64> = bytes
        .chunks_exact(8)
        .map(|b| f64::from_le_bytes(b.try_into().unwrap()))
        .collect();
    // (sample, parameter, value), in time order (stable for equal times).
    let mut changes: Vec<(usize, String, f64)> = args
        .value("--changes")
        .unwrap_or("")
        .split(',')
        .filter(|c| !c.is_empty())
        .map(|c| {
            let p: Vec<&str> = c.split(':').collect();
            let at = (p[0].parse::<f64>().unwrap() * rate) as usize;
            (at, p[1].to_string(), p[2].parse::<f64>().unwrap())
        })
        .collect();
    changes.sort_by_key(|c| c.0);

    let mut output = vec![0.0f64; input.len()];
    let mut start = 0;
    let mut next = 0;
    while start < input.len() {
        while next < changes.len() && changes[next].0 <= start {
            let (_, ref name, value) = changes[next];
            match name.as_str() {
                "volume" => chain.volume.set_target(value),
                "tremolo" => chain.depth.set_target(value),
                "speaker" => chain.character.set_target(value),
                other => panic!("unknown --changes parameter {other}"),
            }
            next += 1;
        }
        let mut end = (start + chunk).min(input.len());
        if next < changes.len() && changes[next].0 < end {
            end = changes[next].0;
        }
        chain.process(&input[start..end], &mut output[start..end]);
        start = end;
    }
    write_f64(args, &output);
}

fn engine(args: &Args) {
    let rate = args.f64_or("--rate", 44_100.0);
    let total = (args.f64_or("--seconds", 1.0) * rate) as usize;
    let chunk = (args.f64_or("--chunk", 256.0) as usize).max(1);
    let mut engine = WurliEngine::new(rate);
    engine.set_volume(args.f64_or("--volume", 0.8));
    engine.set_tremolo_depth(args.f64_or("--tremolo", 0.5));
    engine.set_speaker_character(args.f64_or("--speaker", 0.0));
    engine.set_mlp_enabled(args.flag("--mlp"));
    engine.set_sample_rate(rate);

    // (sample, note, velocity or None for an off)
    let mut events: Vec<(usize, u8, Option<f32>)> = args
        .value("--events")
        .unwrap_or("")
        .split(',')
        .filter(|e| !e.is_empty())
        .map(|e| {
            let p: Vec<&str> = e.split(':').collect();
            let at = (p[0].parse::<f64>().unwrap() * rate) as usize;
            let note = p[2].parse::<u8>().unwrap();
            let velocity = if p[1] == "on" { Some(p[3].parse::<f32>().unwrap()) } else { None };
            (at, note, velocity)
        })
        .collect();
    events.sort_by_key(|e| e.0);

    let mut out = vec![0.0f32; total];
    let mut start = 0;
    let mut next = 0;
    while start < total {
        while next < events.len() && events[next].0 <= start {
            let (_, note, velocity) = events[next];
            match velocity {
                Some(v) => engine.note_on(note, v),
                None => engine.note_off(note),
            }
            next += 1;
        }
        let mut end = (start + chunk).min(total);
        if next < events.len() && events[next].0 < end {
            end = events[next].0;
        }
        engine.render(&mut out[start..end]);
        start = end;
    }
    let mut bytes = Vec::with_capacity(total * 4);
    for s in &out {
        bytes.extend_from_slice(&s.to_le_bytes());
    }
    write_bytes(args, &bytes);
}

fn main() {
    let args = Args(std::env::args().skip(1).collect());
    match args.0.first().map(|s| s.as_str()) {
        Some("voice") => voice(&args),
        Some("chain") => chain(&args),
        Some("engine") => engine(&args),
        _ => {
            eprintln!("usage: openwurli-reference voice|chain|engine [options]");
            std::process::exit(2);
        }
    }
}
