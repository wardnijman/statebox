# Statebox — Dynamic Convolution Analog Capture Plugin

This file is the authoritative development guide for the project. Read it before
writing code. It encodes not just *what* to build but the non-obvious *correct*
way to build it, including corrections to the original brainstorm.

---

## 1. What this plugin actually is (and the honest premise)

Statebox is a JUCE audio plugin (VST3 + AU) that reproduces the behavior of a
real analog unit through **measurement-based, level-dependent dynamic
convolution** plus an **explicit nonlinear stage**.

It is a **black-box / system-identification** model, not a white-box circuit
model. We do not derive behavior from component equations; we measure the unit's
behavior across operating points and reconstruct it.

Signal idea:

```
input → state analysis → interpolate level-dependent IR responses (dynamic convolution)
      → nonlinear stage (harmonics / saturation) → dry/wet → output safety
```

### The load-bearing truth, internalize this before coding

**Convolution is linear. It cannot create harmonics.** Convolving a sine with
*any* impulse response returns a sine at the same frequency, only filtered.

Consequences that drive the whole architecture:

- A bank of level-interpolated IRs gives a **level-dependent frequency
  response**: dynamic EQ, transient shaping, compression-like tone shifts. This
  is real, musical, and the core "voicing" of the unit.
- For a **steady-state signal at constant level**, the interpolation weights are
  constant → the system collapses to one LTI filter → **zero new harmonics**.
- Therefore the **harmonic/saturation character of analog gear (capacitor
  charge/discharge, soft clipping) MUST come from an explicit nonlinearity**, not
  from the convolution. The nonlinear stage is **core, not optional**, for any
  gear that distorts.

This mirrors the original 2018 prototype: its "naive analog behavior" final
stage was doing the harmonic work; the dynamic convolution did the voicing. We
keep both.

Mental model of the full system (not memoryless):

```
y(t) = NL( Σ_k w_k(state(t)) · (h_k * x)(t) )
state(t) = f(envelopes, crest factor, transient, user knobs, recovery memory)
```

---

## 2. Build & toolchain

- **Language:** C++20.
- **Build system:** CMake (>= 3.22). Developer has CMake 4.1.2 — **JUCE must be
  8.x**; JUCE 7 fails under CMake 4.
- **JUCE acquisition:** CMake `FetchContent`, pinned to a JUCE 8.x tag. Do **not**
  depend on the global `~/dev/audio/JUCE` clone for the build (it exists only as a
  Projucer/example reference). Pinned, reproducible, in-tree.
- **Generator:** Ninja preferred. Use `ccache` if present.
- **Plugin formats:** VST3 and AU (Standalone target for the Milestone 0 spike and
  for quick auditioning).
- Full Xcode (not just Command Line Tools) is recommended for AU `auval`/signing.

Canonical commands (fill in once CMakeLists exists):

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

`FetchContent` skeleton CLAUDE should generate:

```cmake
include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.0          # pin to a JUCE 8.x release; bump to latest 8.x
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)
```

---

## 3. Architecture

```
PluginProcessor
├── ParameterManager        (APVTS — automatable params only)
├── StateStore              (non-param state: profile ref, MIDI maps, UI; separate UndoManager)
├── MidiManager
├── PresetManager
├── CaptureLibrary          (installed .dcpack discovery, validation, relink)
├── BackgroundLoader        (file IO, WAV decode, build immutable RuntimeProfile)
├── DynamicConvolutionEngine
│   ├── StateEstimator       (envelopes, crest, transient, recovery → DynamicState)
│   ├── ConvolutionBank      (FIXED POOL of always-warm convolvers over grid points)
│   ├── KernelInterpolator   (state → per-grid-point weights)
│   ├── NonlinearStage       (waveshaper / drive — CORE, generates harmonics)
│   ├── DryWetMixer
│   └── OutputSafetyLimiter
└── PluginEditor
    ├── MainPanel / MeterView / KernelMapView / CaptureBrowser / AdvancedPanel / PresetPanel
```

Module boundaries are stable names; do not rename across modules without updating
this file. Build one module at a time, each with tests, per §10.

---

## 4. HARD RULES (real-time safety & correctness)

These are non-negotiable. Violations are bugs even if they "work."

**Audio thread (`processBlock`):**
- No heap allocation. No `new`/`delete`/`malloc`, no growing `std::vector`, no
  `String`. Pre-allocate all scratch in `prepareToPlay`.
- No locks, no file IO, no `ValueTree` mutation, no logging.
- Read parameters only via atomics / pre-snapshotted values; smooth them.
- Must handle denormals (FTZ/DAZ via `ScopedNoDenormals`) and never emit NaN/inf.

**Threading:**
- *Message thread:* UI, parameter gestures, menus, undo/redo.
- *Background thread:* load `.dcpack`, decode WAV, validate, build an **immutable**
  `RuntimeProfile`.
- *Profile swap:* hand the audio thread a new immutable profile via a lock-free
  pointer swap; **crossfade** old→new; never destroy a profile the audio thread
  may still touch (use `shared_ptr` ownership released on the message thread, or a
  reclaim queue).

**Parameter IDs:**
- All automatable params go through **APVTS** with **stable string IDs** (see §7).
- **Parameter IDs are permanent after the first release.** UI labels may change;
  IDs may NEVER change. Adding params is fine; renaming/removing breaks sessions.

**Latency:**
- Partitioned convolution is generally **not** zero-latency. Report actual latency
  via `setLatencySamples()`, and update it when quality mode / kernel length
  changes. Never hardcode 0.

---

## 5. DSP design decisions (these resolve open questions in the brainstorm)

### 5.1 Convolution: fixed warm pool + output-domain interpolation

Resolve the "Option A vs Option B" question decisively:

- Run a **fixed pool of convolvers, one per active grid point, ALL kept running
  and warm**. Per block, compute interpolation weights from the state vector;
  only neighbor weights are nonzero, but every convolver stays warm so a
  weight crossing in produces correct output immediately (no cold-start garbage).
- This is "interpolate the outputs," but the key correction over the brainstorm
  is: **do not bring convolvers in/out on demand** — a freshly activated
  partitioned convolver has empty overlap/FDL state and outputs garbage for the
  first blocks. Keep them warm.
- Cost scales with **grid size**, not "active" count. MVP: level-only × 6 kernels
  × stereo = 12 convolvers. Acceptable.
- Scaling later: for big multi-axis grids, run only a warm neighborhood and
  pre-warm crossing-in convolvers by feeding a short history ring buffer on
  activation. Document any such cap with `log`/comment — never silently truncate.

Avoid Option A (continuously reloading interpolated FIR coefficients on the audio
thread) for real-time morphing — reloads aren't RT-safe and cause artifacts. IR
*reloads* belong on the background thread (profile swap), not per-block morphing.

### 5.2 Nonlinear stage (CORE)

- Provide a real nonlinearity so the model can generate harmonics at constant
  level. Start simple: a static waveshaper driven by `drive`, oversampled to avoid
  aliasing. Path to upgrade: Wiener–Hammerstein (filter→NL→filter) or per-band
  shaper; later, a tiny ML **residual** trained against the real hardware error.
- Oversample the nonlinear stage (2–4×) once it exists; the linear convolution
  path does not need oversampling on its own.

### 5.3 State estimator (the "secret sauce")

Estimate analog-like memory, not just instantaneous amplitude:

```cpp
struct DynamicState {
    float peak, rmsFast, rmsSlow, crestFactor, transientAmount;
    float drive, recovery, hysteresis;          // recovery = slow falling envelope (discharge)
    float userTonePosition, userDrivePosition;  // hysteresis = fastEnv - slowEnv
};
```

Deterministic only in v1. No ML in the state path until the deterministic core is
validated.

### 5.4 Kernel grid & interpolation

- MVP grid: `level × knob × channel`. Cubic along level, linear/cubic along knob.
- Interpolating between identical kernels must be a no-op (test it).
- Level mapping must be smooth; weight changes must be smoothed to avoid zipper
  noise (modulating weights *is* what produces the dynamic, non-LTI behavior —
  smooth it, but don't over-smooth or you lose the transient character).

---

## 6. Capture format (`.dcpack`)

A capture is a packaged archive (developer reference; large IR audio is NOT stored
in DAW session state — see §8).

```
MyUnit.dcpack
├── manifest.json
├── kernels/   (L_level_00_knob_00.wav, R_level_00_knob_00.wav, ...; multiple variants allowed per cell)
├── noise/     (optional noise/hum profiles)
└── metadata/  (measurement notes)
```

`manifest.json` carries: formatVersion, name/vendor, sampleRate,
kernelLengthSamples, channels, axes (level dBFS list, knob list), normalization
(referenceDbfs, trimMode), latencySamples, createdWith.

Capture methodology notes (do not skip):
- Prefer **exponential sine sweeps (Farina)** over raw impulses for SNR; deconvolve
  to the IR. Raw impulse trains are acceptable for the prototype only.
- A "level-dependent IR" captured at amplitude A is an **effective
  describing-function IR** — the nonlinearity is partly baked in. This is expected
  and fine, but it does NOT replace the explicit nonlinear stage (§5.2).
- IRs are sample-rate-specific. Resample `sourceSampleRate → runtimeSampleRate`
  carefully; `RuntimeProfile` tracks both.
- Validation metrics per kernel: onset confidence, peak level, noise floor, DC
  offset, length adequacy, L/R phase consistency, deviation across repeats.

---

## 7. Parameters (APVTS), automation, MIDI

Stable IDs (permanent): `input_gain_db`, `output_gain_db`, `mix`, `drive`,
`profile_id`, `tone`, `dynamic_depth`, `recovery_ms`, `transient_sensitivity`,
`kernel_smoothing_ms`, `quality_mode`, `variant_amount`, `noise_amount`, `bypass`.

- Smooth every continuous automatable param with `SmoothedValue`.
- **Profile changes are NOT sample-accurate**: UI/host sets a requested profile id
  → background thread loads → audio thread receives immutable profile → crossfade.
- MIDI (MVP): MIDI-learn for params, optional CC mapping, optional note-triggered
  variant reseed. MIDI mappings are saved in plugin state but are **not** exposed
  as DAW parameters.

---

## 8. State, presets, undo (with corrections)

**State restoration** (`get/setStateInformation`) stores: APVTS param values,
profile reference (`id`, `hash`, `pathHint`), preset name, MIDI mappings, UI state,
version. **Never store IR audio in session state** — store the reference; on
missing profile, load a safe no-op/silent state, show a relink warning, update
`pathHint` on relink.

**Undo/redo — important correction to the brainstorm:**
- Do **NOT** pass an `UndoManager` to the APVTS that owns automatable params —
  it would record host-automation playback as undo transactions and pollute the
  tree.
- Drive undo through a **separate `ValueTree` + `UndoManager`** for *discrete UI
  actions only*: profile selection, preset rename, MIDI mapping changes, macro
  assignment, advanced-setting changes.
- Host automation is the source of truth during playback and is never undoable.
  Meters/internal dynamic state are never undoable.

**Presets** (`.dcpreset`): APVTS values + profile reference + MIDI/macro maps +
advanced settings. Never includes the IR library or cached FFT data.

---

## 9. CPU / quality modes

`Eco` (2 kernels, linear, short IR) · `Normal` (4 kernels, cubic on level, medium
IR) · `High` (8 kernels, cubic on level+knob, long IR) · `Render` (max, offline).
Real-time and offline-render quality may differ. Changing mode may change latency
(§4) and convolver count — handle the swap like a profile swap.

---

## 10. MVP scope & milestones

### Milestone 0 — prove the sound (do this FIRST, before the plugin scaffold)
A Standalone/offline render: load real (or synthetic) level kernels → envelope →
fixed warm 2-convolver pool → weighted output interpolation → simple drive
waveshaper → file/realtime out. **Goal: confirm it still sounds magical.** No
presets, no undo, no MIDI, no fancy UI. If the sound isn't there, stop and fix the
DSP before building plumbing.

### MVP (build only after Milestone 0 convinces you)
1. JUCE plugin shell (VST3/AU/Standalone) 2. APVTS params 3. basic UI
4. static profile loading 5. level-only dynamic convolution (warm pool)
6. 2-kernel output interpolation 7. nonlinear drive stage 8. dry/wet + in/out gain
9. state save/load 10. preset save/load 11. simple `.dcpack` parser.

**Do NOT build first:** ML residual, stochastic kernel variants, beautiful UI,
in-plugin capture workflow, multi-axis grids, cloud/marketplace.

**MVP success criterion:** with one captured unit (multiple level kernels), the
plugin smoothly morphs level-dependent responses with audible drive, no clicks,
correct automation/save/restore/undo, reported latency, and stable CPU.

---

## 11. Testing (required)

DSP/correctness:
- Static IR mode equals plain convolution (null test).
- Interpolating identical kernels is bit-neutral (or within float epsilon).
- Capture extraction from a synthetic IR train recovers the known IR.
- No allocation in `processBlock` (assert/instrument in debug).
- Profile swap under audio load: no clicks, no NaN/inf, no denormals.
- Latency reporting matches actual measured latency, per quality mode.
- Save/load and preset roundtrips; missing-profile recovery.
- Sample-rate conversion correctness; mono/stereo bus behavior.

Listening / validation (the scientific backing the project is after):
- Dry vs hardware vs plugin; ABX blind test; null/residual energy + THD/IMD vs a
  reference. A perfect model will NOT null to silence (it's an approximation) —
  judge by residual energy, harmonic structure, and ABX, not bit-equality.

Prefer `pluginval` for RT-safety/format validation.

---

## 12. Out of scope / research backlog

- ML model that *chooses* a kernel. (Risk: random IR switching = artifacts unless
  state-driven and crossfaded. If used at all, drive it from the state vector and
  only as a *residual* correction, not the primary path.)
- Stochastic multi-variant kernels (`variant_amount`): defer; `averaged` in MVP,
  `stable random per transient` later, never sample-by-sample switching.
- Multi-axis grids beyond level×knob, in-plugin capture, oversampling beyond the
  NL stage, cloud sharing.

---

## 13. Suggested file structure

```
Source/
├── PluginProcessor.{h,cpp}  PluginEditor.{h,cpp}
├── parameters/  ParameterIDs.h  ParameterLayout.h  ParameterSnapshot.h
├── dsp/         DynamicConvolutionEngine.{h,cpp}  StateEstimator.h
│                ConvolutionBank.h  KernelInterpolator.h  NonlinearStage.h  DryWetMixer.h
├── capture/     CaptureManifest.h  CaptureLibrary.h  CaptureLoader.h  KernelGrid.h  RuntimeProfile.h
├── state/       StateStore.h  PresetManager.h  MidiMappings.h
├── ui/          MainPanel.h  MeterView.h  KernelMapView.h  CaptureBrowser.h  AdvancedPanel.h
└── tests/       CaptureExtractionTests.cpp  StateRoundtripTests.cpp  DynamicConvolutionTests.cpp
```

---

## 14. Coding conventions

- C++20; match JUCE style for JUCE-touching code. RAII; no raw owning pointers.
- DSP types are RT-safe by construction; document any method as `[RT-safe]` or
  `[message-thread only]` / `[background thread]`.
- Keep `meta`/manifest formats versioned (`formatVersion`) and forward-tolerant.
- Build module-by-module, each landing with its tests green. Prefer small PRs.
