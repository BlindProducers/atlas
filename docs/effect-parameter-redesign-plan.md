# Atlas: Effect Parameter Redesign Plan

Plan as of 2026-10-12.

Build effect parameters only for the effect each slot has selected, in two stages: parked parameters first (about 100 MB less per instance), then a parameter table (about 70 MB more). Work happens on the `effect-parameter-redesign` branch, based on `memory-experiments`.

## The problem

Effect parameters are now about 70% of what one Atlas instance costs: roughly 260 MB at construction, of which about 225 MB is effect parameter controls.

- Each of the 4 effect chains (main plus 3 buses) has 20 slots plus one legacy fixed-order set that is never processed. That makes 21 sets per chain and 84 per instance.
- Every set builds all 37 effect types, whatever the slot has selected, including empty slots. That makes 3,108 effect shells per instance.
- Zach's on-demand change means a shell no longer builds its effect's audio processing until selected. But each shell still creates every parameter control of its type, with full modulation wiring: a value, a modulation sum, a switch, and entries in several name maps.
- One set of 37 types holds about 745 controls (about 494 of them modulatable) and costs about 35,000 allocations and 3.4 MB. Across 84 sets that is about 62,600 controls and 145,000 engine objects.
- Glitch Delay (196 controls) and Trance Gate (104) are about 40% of all controls on their own.

The controls also inflate everything built from them: the saved state is about 3 MB because it writes every control, and three per-instance name maps (about 25 MB) list all of them.

## Two designs, measured

Design 1 saves about 98 MB per instance with limited risk; Design 2 saves about 160–170 MB but changes every part of the code that reads parameters by value. Both were measured on macOS with a throwaway build (since reverted): one instance, prepared at 48 kHz, editor open.

| Design | What non-selected effects keep | Memory with editor open | Saving |
| --- | --- | --- | --- |
| Today (`memory-experiments`) | All controls with modulation wiring | 363 MB | — |
| Design 1: parked parameters | A plain value object per parameter, no wiring | 265 MB | about 98 MB |
| Design 2: parameter table | Plain numbers in a per-slot table | about 195–205 MB | about 160–170 MB |

The Design 2 figure is an upper bound: the experiment removed non-selected controls entirely, and the table plus its name lookup will add a few MB back.

**Recommendation: do both, in stages.** Almost all of Design 1's work (switching wiring on and off, dormant modulation routes, the destination cache fix) is also needed for Design 2. Design 1 ships about 100 MB on its own; Design 2 then replaces the parked value objects with table cells.

## Stage 0: safety net

Before any change, the tests must be able to prove that sound, saved state and routing come out identical. Both stages are judged against these.

- [ ] **Golden renders.** Extend `AtlasPreparedEffectTests --render` to cover every effect type in main and bus slots with non-default settings and a modulation route into an effect parameter; switching effects during playback; FX proxy automation; bus preset loading; the factory presets. Outputs must match the current build sample for sample.
- [ ] **Save/load round trip.** Saving, loading and saving again gives identical state, including settings of non-selected effects, modulation routes into them, FX automation assignments to them, and MIDI learn targets.
- [ ] **Slot operations.** Swap, move, clear, copy/paste and bus presets keep every type's remembered settings, routes and automation, as today.
- [ ] **Existing suites:** prepared effects, FX automation, FX bus presets, editor accessibility, Glitch Delay, Trance Gate, latency, timing stability.
- [ ] **Memory thresholds** on `--construction` and `--editor-memory`, so a regression fails a test.

One caveat: the Eight Ways factory preset already renders differently from run to run on the old code too (5 different outputs in 28 runs), probably a background analysis thread. Either fix that first or leave it out of the bit-exact comparison.

## Stage 1: parked parameters (about 100 MB less)

A shell keeps its parameter values whether or not it is selected, but only the selected effect gets modulation wiring. The value objects never move, so nothing that reads parameters by name or by pointer has to change.

1. **Two shell states.** *Parked:* only the base value object for each parameter, the same kinds of object as today, so the sound stays identical. *Live:* adds the modulation sum, the switch and the destination entries around those same values.
2. **Going live and leaving, on the main thread.** Hook into the point where Zach's change already reserves an effect's buffers (`scheduleEffectControlReservation` and `reserveSelectedControls`).
    - Going live: build the new effect's wiring outside the audio lock, attach it under the lock, reserve buffers, then reconnect that effect's modulation routes.
    - Leaving: disconnect the old effect's routes, apply that with processing paused (`processModulationChanges`), remove the shell from the preparation worker, then tear the wiring down.
    - This is the order oscillator slots already use for type changes (`SynthBase::prepareSelectedSources`).
3. **Effect changed from the audio thread** (FX proxy automation or MIDI learn on a slot's type): the slot passes audio through, latency-aligned, and raises the existing reservation request; the main thread makes it live. Unprepared effects already behave this way.
4. **Dormant modulation routes.** A route aimed at an effect that isn't selected stays in the modulation bank and in saved state, unplugged, and plugs in when that effect goes live. Today a route to a missing destination is silently dropped, and the disconnect paths dereference the destination without a null check; both need fixing.
5. **Destination cache fix.** `getMonoModulationDestinations()` and its siblings copy every child's entries into the caller's own maps and never remove them, so lookups would return wiring that has been destroyed. Make them build a fresh map.
6. **Preset loading order.** After `loadControls`, the selected effects go live before `loadModulations` runs, or dormant routes connect as they go live.
7. **Modulation matrix** lists only live destinations, and slot parameters get their own group.
8. **Legacy fixed-order sets** are never selected, so they stay parked and become cheap automatically. Remove the dead meters (compressor, chorus, phaser, flanger status outputs) that point at them. Keep their values: host parameter IDs are numbered from them.

## Stage 2: parameter table (about 70 MB more)

Parked value objects are replaced by plain numbers. Each parked parameter costs about 1.2 KB today, as an engine object plus name map entries; a table cell costs a few bytes.

1. **Per-slot table** of plain numbers for all 37 types. A name lookup shared by all instances maps each parameter name to its chain, slot, type and index.
2. **Permanent cells.** Every parameter name keeps a cell for the instance's lifetime; live controls mirror their cell.
    - The controls map keeps a fixed set of names, so its thread safety doesn't change. Today it is read from the audio thread, the main thread and the host's state threads without locks, which is only safe because its shape never changes.
    - A write can never hit a missing control. Writes made before an effect change (swap, paste, bus presets) still land.
3. **Move every consumer from value objects to cells:**
    - load and save (`LoadSave::stateToJson`, `loadControls`)
    - FX automation bindings and their 144 proxies
    - editor rows (`ValueBridge` for slot parameters)
    - MIDI learn writes, which arrive on the audio thread
    - swap, clear, copy/paste and bus presets
    - the spectral-compression macro targets and the GUI update list
4. **Host IDs:** count host parameter IDs from the parameter list, not from the controls map, so old projects' automation stays on the right parameters when legacy controls go.
5. **Optional:** replace the three per-instance name maps (controls, pending GUI updates, FX automation bindings; about 25 MB) with shared name tables plus flat per-instance arrays.

## What changes for users

Almost nothing audible; the visible change is a shorter modulation destination list.

- **Modulation matrix:** lists only the effects actually selected in slots, instead of all 37 effects for every slot, including empty ones.
- **Effect settings** are remembered when you switch a slot to another effect and back, exactly as today.
- **Switching by automation or MIDI learn during playback:** the new effect starts a few milliseconds later, after the main thread prepares it. Heavy effects already behave this way today.
- **Plugin loading** gets faster: construction builds far fewer objects.

## Risks and how each is covered

The main risk is something holding a pointer to wiring that has just been torn down; every teardown therefore happens on the main thread, with processing paused, after everything that reads the wiring has let go of it.

| Risk | How it's prevented | What catches it |
| --- | --- | --- |
| Crash from destroyed wiring (audio thread, preparation worker, queued modulation changes, editor rows) | Tear down only while paused, after the worker drops the shell and queued changes are applied. Editor rows hold values (Stage 1) or cells (Stage 2), which never go away | Type-switching tests during playback; editor accessibility tests |
| Stale editor rows during the async rebuild after a type change | Values and cells outlive the rows; add a per-slot type watcher so rows rebuild after automation-driven changes too | Editor tests switching types by automation and MIDI |
| Clicks when switching effects mid-play | Build wiring outside the audio lock; only attach it under the lock | Realtime switching test (no allocations, block time) |
| Lost or misplaced modulation routes | Dormant routes stay in the bank and saved state; destination cache rebuilt instead of merged | Save/load round trip; routes into non-selected effects |
| Host automation moving to the wrong parameter | Host IDs counted from the parameter list, not from controls | Host parameter ID snapshot test |
| Preset values lost on load | Selected effects go live before routes load; missing keys reset to defaults in the table too | Golden renders of factory and FX presets |

## Separate smaller items

These don't depend on the redesign and can ship any time; the saved-state fix gives REAPER the quickest win.

| Item | Gain | Effort | Notes |
| --- | --- | --- | --- |
| Save only non-default values | Saved state about 3 MB down to tens of KB | Small to medium | REAPER keeps a copy of the state per undo step, so this cuts REAPER's own memory. Must always write `bus_1_destination` and `osc_4_destination`: the Vital-layout migration checks for their presence |
| Editor reopen leak | About 17–24 MB each time the editor is reopened | Small | Measured with `--editor-memory` |
| Crash-proof value writes | Prevents a crash | Small | `SynthBase::valueChanged` uses `controls_[name]`, which inserts an empty entry for an unknown name and dereferences it |
| Eight Ways nondeterminism | Repeatable renders | Small to medium | Renders differently from run to run on every build |
| Host parameter cleanup | Shorter automation list; under 0.2 MB | Small | 86 legacy oscillator aliases and 384 modulation parameters. Compatibility decision: old projects lose automation on removed parameters |

## Code map

Stage 1 stays mostly inside the shells, the chain and modulation; Stage 2 reaches every consumer of parameter values.

| File | What it holds | Stage |
| --- | --- | --- |
| `src/synthesis/modules/prepared_effect_module.{h,cpp}` | Shell `init`, control `Description`, bindings to the built DSP, `reserveControls` | 1, 2 |
| `src/synthesis/modules/reorderable_effect_chain.{h,cpp}` | Slot shells, legacy sets, type selection on the audio thread, `reserveSelectedControls` | 1 |
| `src/synthesis/framework/synth_module.{h,cpp}` | Control creation, modulation destination maps, the merging getters | 1 |
| `src/synthesis/modules/source_slot_module.cpp` | The precedent: permanent values, only the active type built | reference |
| `src/common/synth_base.cpp` | `valueChanged`, reservation scheduling, `connectModulation`, `prepareSelectedSources` | 1, 2 |
| `src/common/load_save.cpp` | `stateToJson`, `loadControls`, `loadModulations`, migrations | 1, 2 |
| `src/synthesis/synth_engine/sound_engine.cpp` | Modulation connect and disconnect, dead legacy meters | 1 |
| `src/plugin/synth_plugin.cpp` | Host parameter IDs, FX automation bindings, `loadFxBusPreset` | 2 |
| `src/plugin/synth_editor.cpp` | Slot rows, modulation matrix, swap, clear, copy/paste | 1, 2 |
| `src/common/midi_manager.cpp` | MIDI learn writes on the audio thread | 2 |
