#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the OpenWurli reference renderer with rustc alone (no Cargo, no network).

Compiles melange-primitives and openwurli-dsp unchanged from their checkouts
(the opt-in third_party/melange and third_party/openwurli submodules) with the
features OpenWurli ships by default, then tests/openwurli/reference.rs against
them. A second copy of openwurli-dsp, openwurli_dsp_emitter, is built with
pickup.rs's one-line C-2 fork flipped (C2_RETURNS_TO_GROUND = false), for
Atlas's Pickup Low Cut. The renderer is a test oracle and is never linked into
Atlas.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('openwurli', type=Path, help='openwurli checkout')
p.add_argument('melange', type=Path, help='melange checkout (the rev openwurli pins)')
p.add_argument('output', type=Path)
p.add_argument('--rustc', default='rustc')
a = p.parse_args()

reference = Path(__file__).resolve().parent / 'reference.rs'
primitives = a.melange.resolve() / 'crates/melange-primitives/src/lib.rs'
dsp = a.openwurli.resolve() / 'crates/openwurli-dsp/src/lib.rs'
output = a.output.resolve()
output.parent.mkdir(parents=True, exist_ok=True)

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)

    def rustc(*args):
        subprocess.run([a.rustc, '-C', 'opt-level=3', '--cap-lints', 'allow', *map(str, args)], check=True)

    rustc('--edition', '2021', '--crate-type', 'rlib', '--crate-name', 'melange_primitives',
          '-o', tmp / 'libmelange_primitives.rlib', primitives)
    # openwurli-dsp's default feature set (Cargo.toml): the behavioural power amp.
    rustc('--edition', '2024', '--crate-type', 'rlib', '--crate-name', 'openwurli_dsp',
          '--cfg', 'feature="legacy-power-amp"', '-L', tmp,
          '--extern', f'melange_primitives={tmp / "libmelange_primitives.rlib"}',
          '-o', tmp / 'libopenwurli_dsp.rlib', dsp)
    emitter = tmp / 'openwurli-dsp-emitter'
    shutil.copytree(dsp.parent, emitter)
    pickup = emitter / 'pickup.rs'
    fork = 'const C2_RETURNS_TO_GROUND: bool = true;'
    text = pickup.read_text()
    if text.count(fork) != 1:
        raise SystemExit('pickup.rs no longer has the C-2 fork: ' + fork)
    pickup.write_text(text.replace(fork, 'const C2_RETURNS_TO_GROUND: bool = false;'))
    rustc('--edition', '2024', '--crate-type', 'rlib', '--crate-name', 'openwurli_dsp_emitter',
          '--cfg', 'feature="legacy-power-amp"', '-L', tmp,
          '--extern', f'melange_primitives={tmp / "libmelange_primitives.rlib"}',
          '-o', tmp / 'libopenwurli_dsp_emitter.rlib', emitter / 'lib.rs')
    rustc('--edition', '2024', '--crate-type', 'bin', '--crate-name', 'openwurli_reference', '-L', tmp,
          '--extern', f'openwurli_dsp={tmp / "libopenwurli_dsp.rlib"}',
          '--extern', f'openwurli_dsp_emitter={tmp / "libopenwurli_dsp_emitter.rlib"}', '-o', output, reference)
