#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile Diopser with tiny host shims and Atlas's explicit 20 Hz frequency floor.

No Cargo dependencies or network are required. The source filter and NIH-plug
smoothers are loaded unchanged. The extracted update method changes only the
per-stage minimum from 5 to 20 Hz; plugin/GUI/host scaffolding is replaced.
The standalone oracle is for tests, never linked into Atlas.
"""
from pathlib import Path
import argparse
import json
import os
import subprocess

p = argparse.ArgumentParser()
p.add_argument('source', type=Path, help='nih-plug/plugins/diopser')
p.add_argument('output', type=Path)
p.add_argument('--rustc', default='rustc')
a = p.parse_args()
source = a.source.resolve()
nih = source.parents[1]
lib = (source / 'src/lib.rs').read_text()

def block_after(text, marker):
    start = text.index('{', text.index(marker))
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

rust = r'''
#![feature(portable_simd)]
#![allow(dead_code, unused_imports)]
extern crate self as atomic_float;
extern crate self as nih_plug;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::simd::f32x2;
use std::io::{Read, Write};
#[derive(Debug, Default)]
pub struct AtomicF32(AtomicU32);
impl AtomicF32 {
    pub fn new(x: f32) -> Self { Self(AtomicU32::new(x.to_bits())) }
    pub fn load(&self, o: Ordering) -> f32 { f32::from_bits(self.0.load(o)) }
    pub fn store(&self, x: f32, o: Ordering) { self.0.store(x.to_bits(), o) }
}
#[macro_export] macro_rules! nih_debug_assert { ($($x:tt)*) => { debug_assert!($($x)*) }; }
#[macro_export] macro_rules! nih_debug_assert_ne { ($($x:tt)*) => { debug_assert_ne!($($x)*) }; }
pub mod debug { pub use crate::nih_debug_assert; }
FILTER_MODULE
SMOOTHING_MODULE
use smoothing::{Smoother, SmoothingStyle};
const MAX_BLOCK_SIZE: usize = 64;
mod params { pub const MAX_NUM_FILTERS: usize = 512;
    pub fn unnormalize_automation_precision(n: f32) -> u32 { 512 - (n * 511.0).round() as u32 }
}
#[derive(Clone, Copy, PartialEq)] enum SpreadStyle { Octaves, Linear }
struct Val<T: Copy>(T);
impl<T: Copy> Val<T> { fn value(&self) -> T { self.0 } }
struct FloatParam { value: f32, smoothed: Smoother<f32> }
impl FloatParam {
    fn new(value: f32, logarithmic: bool) -> Self {
        let smoothed = Smoother::new(if logarithmic { SmoothingStyle::Logarithmic(100.0) }
                                    else { SmoothingStyle::Linear(100.0) });
        smoothed.reset(value); Self { value, smoothed }
    }
    fn set(&mut self, value: f32, rate: f32, reset: bool) {
        if reset { self.smoothed.reset(value); }
        else if self.value != value { self.smoothed.set_target(rate, value); }
        self.value = value;
    }
}
struct Editor;
impl Editor { fn is_open(&self) -> bool { false } }
struct Spectrum;
impl Spectrum { fn compute(&self, _: &Buffer) {} }
struct Params {
    filter_stages: Val<i32>, filter_frequency: FloatParam, filter_resonance: FloatParam,
    filter_spread_octaves: FloatParam, filter_spread_style: Val<SpreadStyle>,
    automation_precision: Val<f32>, bypass: Val<bool>, editor_state: Editor,
}
struct Buffer { audio: Vec<[f32; 2]> }
struct Block<'a> { audio: &'a mut [[f32; 2]] }
struct Sample<'a>(&'a mut [f32; 2]);
impl Buffer {
    fn iter_blocks(&mut self, size: usize) -> impl Iterator<Item=(usize, Block<'_>)> {
        self.audio.chunks_mut(size).enumerate().map(|(i, audio)| (i, Block { audio }))
    }
}
impl Block<'_> {
    fn iter_samples(&mut self) -> impl Iterator<Item=Sample<'_>> { self.audio.iter_mut().map(Sample) }
}
impl Sample<'_> {
    unsafe fn to_simd_unchecked(&self) -> f32x2 { f32x2::from_array(*self.0) }
    unsafe fn from_simd_unchecked(&mut self, value: f32x2) { *self.0 = value.to_array(); }
}
enum ProcessStatus { Normal }
struct Diopser {
    params: Params, sample_rate: AtomicF32, filters: [filter::Biquad<f32x2>; 512],
    bypass_smoother: Smoother<f32>, should_update_filters: AtomicBool,
    next_filter_smoothing_in: i32, spectrum_input: Spectrum,
}
impl Diopser {
    fn new(rate: f32) -> Self {
        Self { params: Params {
            filter_stages: Val(0), filter_frequency: FloatParam::new(200.0, true),
            filter_resonance: FloatParam::new(0.5, true), filter_spread_octaves: FloatParam::new(0.0, false),
            filter_spread_style: Val(SpreadStyle::Octaves), automation_precision: Val(1.0),
            bypass: Val(false), editor_state: Editor,
        }, sample_rate: AtomicF32::new(rate), filters: [filter::Biquad::default(); 512],
        bypass_smoother: Smoother::new(SmoothingStyle::Linear(10.0)),
        should_update_filters: AtomicBool::new(true), next_filter_smoothing_in: 1, spectrum_input: Spectrum }
    }
    fn set(&mut self, values: [f32; 7], reset: bool) {
        let rate = self.sample_rate.load(Ordering::Relaxed);
        let stages = values[0] as i32;
        let style = if values[4] == 0.0 { SpreadStyle::Octaves } else { SpreadStyle::Linear };
        if stages != self.params.filter_stages.0 || style != self.params.filter_spread_style.0 {
            self.should_update_filters.store(true, Ordering::Release);
        }
        self.params.filter_stages.0 = stages;
        self.params.filter_frequency.set(values[1], rate, reset);
        self.params.filter_resonance.set(values[2], rate, reset);
        self.params.filter_spread_octaves.set(values[3], rate, reset);
        self.params.filter_spread_style.0 = style;
        self.params.automation_precision.0 = values[5];
        let bypass = values[6] != 0.0;
        if self.params.bypass.0 != bypass {
            self.bypass_smoother.set_target(rate, if bypass { 1.0 } else { 0.0 });
        }
        self.params.bypass.0 = bypass;
        if reset { self.reset(); }
    }
    fn reset(&mut self) RESET_BODY
    fn process(&mut self, buffer: &mut Buffer) -> ProcessStatus PROCESS_BODY
}
impl Diopser UPDATE_BODY
fn u32read(input: &mut impl Read) -> u32 {
    let mut b = [0u8; 4]; input.read_exact(&mut b).unwrap(); u32::from_le_bytes(b)
}
fn f32read(input: &mut impl Read) -> f32 { f32::from_bits(u32read(input)) }
fn main() {
    let args: Vec<_> = std::env::args().collect();
    let mut input = std::io::BufReader::new(std::fs::File::open(&args[1]).unwrap());
    let mut output = std::io::BufWriter::new(std::fs::File::create(&args[2]).unwrap());
    for _ in 0..u32read(&mut input) {
        let rate = f32read(&mut input);
        let mut dsp = Diopser::new(rate);
        for _ in 0..u32read(&mut input) {
            let reset = u32read(&mut input) != 0;
            let count = u32read(&mut input) as usize;
            let values = std::array::from_fn(|_| f32read(&mut input));
            dsp.set(values, reset);
            let mut buffer = Buffer { audio: (0..count).map(|_| [f32read(&mut input), f32read(&mut input)]).collect() };
            dsp.process(&mut buffer);
            for pair in buffer.audio { for sample in pair { output.write_all(&sample.to_le_bytes()).unwrap(); } }
        }
    }
}
'''
rust = rust.replace('FILTER_MODULE', '#[path = ' + json.dumps(str(source / 'src/filter.rs')) + '] mod filter;')
rust = rust.replace('SMOOTHING_MODULE', '#[path = ' + json.dumps(str(nih / 'src/params/smoothing.rs')) + '] mod smoothing;')
rust = rust.replace('RESET_BODY', block_after(lib, '    fn reset(&mut self)'))
rust = rust.replace('PROCESS_BODY', block_after(lib, '    fn process('))
# The one intentional DSP range change requested for Atlas also applies after spread.
update_body = block_after(lib, 'impl Diopser {')
assert update_body.count('const MIN_FREQUENCY: f32 = 5.0;') == 1
update_body = update_body.replace('const MIN_FREQUENCY: f32 = 5.0;', 'const MIN_FREQUENCY: f32 = 20.0;')
rust = rust.replace('UPDATE_BODY', update_body)
a.output.parent.mkdir(parents=True, exist_ok=True)
rs = a.output.with_suffix('.rs')
rs.write_text(lib.split('#!')[0] + rust)
# The supplied source requires portable_simd. Enable it for this isolated test
# oracle when only stable Rust is installed; no production build uses Rust.
subprocess.run([a.rustc, '--edition=2021', '-C', 'opt-level=3', str(rs), '-o', str(a.output)],
               env=dict(os.environ, RUSTC_BOOTSTRAP='1'), check=True)
print('Built Diopser oracle: original DSP with the intentional 20 Hz per-stage minimum')
