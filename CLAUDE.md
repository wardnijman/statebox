# Statebox — State-Based Measured Hardware Model (dynamic convolution + small nonlinearity)

This file is the authoritative development guide. Read it before writing code. It
encodes not just *what* to build but the non-obvious *correct* way to build it,
including corrections distilled from extended design discussion.

---

## 1. What this plugin actually is (and the honest premise)

Statebox is a JUCE audio plugin (VST3 + AU). It is best described **not** as an
"analog model" or a saturator, but as a **state-based measured hardware model**:
it captures *how a real unit moves over time* across operating points and
reconstructs that motion, then adds only a small amount of nonlinearity and noise.

It is a **black-box / system-identification** model, not a white-box circuit
model. The novelty — and the likely reason the 2018 prototype sounded better than
conventional analog emulations — is the emphasis. Most emulations chase harmonics
and get the *dynamic tone movement, memory, hysteresis, and recovery* wrong, and
the ear is extremely sensitive to exactly those. Statebox inverts the priority.

**Intended proportion of the "analog life" (a design philosophy, not a fixed ratio):**

- ~85% — dynamic convolution (state-dependent tone + memory + movement)
- ~10% — nonlinearity (harmonics / saturation), intentionally subtle
- ~5%  — input-modulated noise / drift / residual texture

The secret sauce is that the box never stops moving — not the distortion. Don't
build "another saturator"; that competes with neural amp models. Build the moving,
measured hardware behavior, and add only a little grit on top.

### Three layers of "analog life" (keep them conceptually separate)

1. **Dynamic convolution** — state/level-dependent filtering. Captures tone,
   transient behavior, attack/release coloration, capacitor *memory* effects,
   level-dependent resonances, dynamic tone shifts. This is the bulk of the sound.
2. **Nonlinearity** — the *only* layer that creates harmonics, saturation, and
   intermodulation. Small but **non-negotiable** if the gear distorts at all.
3. **Noise / residual** — hiss, hum, rumble, drift, crackle. Adds life and masks
   digital sterility. Must be **input-modulated** (level scales amount, transients
   trigger crackle, envelope colors it) to feel analog. This is NOT a harmonic
   engine.

### The load-bearing truth — internalize before coding

**Convolution is linear. It cannot create harmonics.** Convolving a sine with
*any* impulse response returns a sine at the *same* frequency, only filtered
(amplitude / phase / ringing). A filter — analog or digital — does not by itself
create new frequencies. (Analog filters "add harmonics" only because the circuit
contains *nonlinear components*, not because of the filtering. A linear capacitor
likewise only causes time-smearing, phase shift, and frequency-dependent response
— memory, not harmonics.) To make a 2nd harmonic you need a nonlinear relation
like `y = x + 0.2·x²` (since `sin² = 0.5 − 0.5·cos(2t)`); a cubic term makes a 3rd.

Two cases worth distinguishing (the prototype's capture sits between them):

- **Linear IRs measured per level.** At any fixed state this is an LTI filter →
  zero new harmonics. Period.
- **"Louder-impulse" / describing-function capture** (impulses sent *through* the
  nonlinear unit). The extracted kernel is an amplitude-conditioned *snapshot*, so
  when the state varies during playback the whole system is **globally nonlinear**
  and produces movement that feels analog. *But* for a steady tone at constant
  amplitude the weights are constant, one snapshot is selected, and you are back to
  an LTI filter → still **no steady-state harmonic distortion or IMD**.

So the honest claim is: dynamic convolution is **state-dependent filtering that
approximates part of a nonlinear system's behavior** (transients, memory,
hysteresis, level-dependent resonance) — *not* a substitute for harmonic
generation. Steady-state harmonics, IMD, and saturation require the explicit
nonlinear stage (Layer 2). It is **core, not optional** — just intentionally small.

This mirrors the 2018 prototype: the "naive analog behavior" final stage was doing
the harmonic work; the dynamic convolution did the moving voicing. Keep both.

### Preferred signal architecture (Wiener–Hammerstein-ish, with a MEASURED nonlinearity)

```
input → input gain → state estimator
      → dynamic PRE convolution   (state-interpolated linear IR bank h1 — voicing/movement)
      → MEASURED nonlinear block   (parallel-Hammerstein from captured h2,h3) + small hand 'drive'
      → dynamic POST convolution   (optional, state-dependent post-emphasis)
      → input-modulated noise / residual
      → dry/wet → output gain → safety limiter
```

**Decision (locked):** the harmonics come from a **measured** generalized-Hammerstein
nonlinearity — the higher-order kernels (h2, h3, …) extracted from the
exponential-sweep capture (§6) — *plus* a small optional hand-tuned `drive` on top.
The dynamic convolution bank supplies the moving linear voicing (h1); the measured
Hammerstein block supplies real, measured harmonics rather than an invented
waveshape. This pre-NL-post topology is far more analog-plausible than one
convolution + a guessed waveshaper, and is the **target** architecture. The MVP may
start with a single dynamic convolution → small hand drive (Milestone 0), then add
the measured Hammerstein block and POST stage once the sound is validated.

Mental model (not memoryless):

```
y(t) = POST_state( NL( PRE_state( x(t) ) ) ) + noise(state(t))
state(t) = f(envelopes, crest factor, transient, user knobs, recovery memory)
```

---

## 2. Build & toolchain

- **Language:** C++20.
- **Build system:** CMake (>= 3.22). Developer has CMake 4.1.2 — **JUCE must be
  8.x**; JUCE 7 fails under CMake 4.
- **JUCE acquisition:** CMake `FetchContent`, pinned to a **known-good JUCE 8.x
  tag** (FetchContent first — simplest for AI-assisted scaffolding and reproducible
  builds; submodule is an acceptable alternative). Do **not** depend on the global
  `~/dev/audio/JUCE` clone for the build (it exists only as a Projucer/example
  reference). Bump the tag deliberately, don't track moving `main`.
- **Generator:** Ninja preferred. Use `ccache` if present.
- **Plugin formats:** VST3 and AU (Standalone target for the Milestone 0 spike and
  quick auditioning).
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
  GIT_TAG 8.0.0          # pin to a known-good JUCE 8.x release; bump deliberately
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
│   ├── ConvolutionBank      (FIXED POOL of always-warm convolvers; used for PRE and POST stages)
│   ├── KernelInterpolator   (state → per-grid-point weights)
│   ├── NonlinearStage       (measured Hammerstein h2,h3 + small hand drive — the ONLY harmonic source)
│   ├── NoiseResidual        (input-modulated hiss/hum/drift — texture, not harmonics)
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
  via `setLatencySamples()`. Never hardcode 0.
- **Do not change latency during playback** — it disrupts host PDC and is jarring.
  Prefer one fixed latency across quality modes; if a mode genuinely needs a
  different latency, require a reload/stop and warn the user rather than retuning
  live.

---

## 5. DSP design decisions (these resolve open questions from the design discussion)

### 5.1 Convolution: fixed warm pool + output-domain interpolation

Resolve the "Option A vs Option B" question decisively:

- Run a **fixed pool of convolvers, one per active grid point, ALL kept running
  and warm**. Per block, compute interpolation weights from the state vector; only
  neighbor weights are nonzero, but every convolver stays warm so a weight crossing
  in produces correct output immediately (no cold-start garbage):
  `out = kernelA_out * weightA + kernelB_out * weightB + ...`
- Key correction: **do not bring convolvers in/out on demand.** A freshly activated
  partitioned convolver has empty overlap/FDL state and outputs garbage for the
  first blocks. Keep them warm.
- Cost scales with **grid size**, not "active" count. MVP: level-only × 6 kernels ×
  stereo = 12 always-warm convolvers. Acceptable.

Avoid Option A (continuously reloading interpolated FIR coefficients on the audio
thread) for real-time morphing — reloads aren't RT-safe and cause artifacts. IR
*reloads* belong on the background thread (profile swap), not per-block morphing.

Rule of thumb for the pool:
- **MVP:** fixed warm pool for the whole (small) grid.
- **Later:** warm-neighborhood + background pre-warm + crossfade (cost can then
  scale closer to active neighborhood — but only with correct pre-warming).
- **Never:** cold-start a convolver directly into the audible mix.

Note: PRE and POST dynamic convolutions (§1) each need their own warm pool.

### 5.2 Nonlinear stage (CORE, MEASURED, but intentionally small)

- This is the **only** source of harmonics / saturation / IMD. Required if the gear
  distorts — but keep it subtle (see the ~10% philosophy in §1). The character
  should come from movement, not crunch.
- **Measured, not invented.** The harmonics come from a **parallel/generalized
  Hammerstein** block whose kernels (h2, h3, …) are extracted from the
  exponential-sweep capture (Farina higher-order IRs / synchronized swept-sine,
  §6): `y_nl ≈ h2∗x² + h3∗x³ (+ …)` summed with the linear voicing path. A small
  hand-tuned `drive` waveshaper sits on top for user control / pushing beyond the
  measured amount.
- Topology: **Wiener–Hammerstein** — dynamic PRE convolution → measured Hammerstein
  NL (+ small drive) → optional dynamic POST convolution (§1).
- Oversample the nonlinear path (2–4×) to avoid aliasing of the generated
  harmonics; the linear convolution path does not need oversampling on its own.
- MVP shortcut: a single dynamic conv → small hand `drive` is fine for Milestone 0;
  swap in the measured Hammerstein kernels after the go/no-go gate. Later upgrade:
  a tiny ML **residual** trained against the real hardware error.

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
- **Sub-sample alignment is mandatory.** Output-domain interpolation equals
  coefficient interpolation: `wA·(hA∗x) + wB·(hB∗x) = (wA·hA + wB·hB)∗x`. If two
  kernels are misaligned by even one sample, the interpolated kernel has two peaks
  → comb filtering while morphing. Align all grid kernels to a common
  fractional-sample origin at capture time (§6). Optionally min-phase-reconstruct
  kernels before banking for better-behaved interpolation (trade-off: discards the
  unit's real phase character — decide consciously).
- Level mapping must be smooth; weight changes must be smoothed to avoid zipper
  noise. Modulating the weights *is* what produces the dynamic, non-LTI behavior —
  smooth it, but don't over-smooth or you lose the transient character.

### 5.5 Noise / residual layer (texture, not harmonics)

- Sampled or synthesized hiss / hum / rumble / crackle that makes the model feel
  alive. Optionally captured from the unit's idle state (§6).
- Must be **input-modulated** to feel analog: input level scales amount, transients
  can trigger crackle, the envelope can color it. Static additive noise feels dead.
- Explicitly NOT a harmonic engine — noise is (mostly) input-*independent* energy;
  harmonics are deterministic multiples of the input. Do not fake saturation with
  noise.
- Controlled by `noise_amount`; small by default (~5%, §1).

---

## 6. Capture (`.dcpack`) — format, method, protocol

A capture packages a measured unit (developer reference; large IR audio is NOT
stored in DAW session state — see §8). Mental model: you are **sampling states of a
system**, reconstructing `f(input, state) → output` — not "recording a filter."

```
MyUnit.dcpack
├── manifest.json
├── kernels/    (linear IRs h1: L_level_00_knob_00.wav, ...; variants allowed per cell)
├── harmonics/  (measured higher-order kernels h2,h3,... per operating point)
├── temporal/   (Dataset B: step/recovery/hysteresis measurements)
├── noise/      (idle noise/hum profiles for the residual layer)
└── metadata/   (measurement notes, calibration)
```

`manifest.json` carries: formatVersion, name/vendor, sampleRate (capture, e.g.
96000), bitDepth, kernelLengthSamples, channels, axes (level list + units, knob
list), normalization (referenceDbfs, trimMode), latencyReferenceSamples (measured
loopback), inputCalibration (dBFS↔analog level, e.g. −18 dBFS = +4 dBu),
harmonicOrders captured, createdWith.

### Canonical method: exponential sine sweep (ESS / Farina)

Retire manual impulse-train detection (keep it only for experimentation). Use ESS:
- Deconvolve the recorded sweep against the inverse filter → linear IR `h1` at t=0.
- **The nonlinearity is separated, not baked in.** Harmonic products appear as
  their own IRs at *negative* time offsets (Δt_N = T·ln(N)/ln(f2/f1) before h1).
  Window them out → h2, h3, … = the **measured Hammerstein** kernels (§5.2). (See
  Farina 2000; Novak et al. synchronized swept-sine, for the parallel-Hammerstein
  identification.) This is why ESS beats louder impulses: a raw louder-impulse /
  sine-tone capture yields a *describing-function snapshot* that mixes the
  nonlinearity into the kernel; ESS gives a clean h1 **and** the separable harmonics.

### Per-operating-point protocol (deterministic)

Identical scripted sequence for every grid point, repeated N≥5×:
```
reset → 2 s conditioning (pink noise or sine at operating level)
      → settle 200 ms → 3 s ESS (20 Hz→20 kHz) → settle 500 ms → store
```
- **Pre-conditioning is mandatory** — analog gear has memory; "silent 10 s then
  measure" ≠ "hammered 10 s then measure." Fixed inter-measurement timing controls
  thermal/memory state.
- Store **mean** (the deterministic kernel) + **std-dev** + raw variants. The
  std-dev map is itself a feature: it shows where the unit is "alive"
  (drift/instability) and where to spend the noise/variant budget (§5.5). Modes:
  stable=mean, analog=sample a variant, research=analyze drift.

### Alignment & calibration (don't skip — interpolation depends on it)

- **Loopback latency reference:** measure interface-out→in directly once; subtract
  from every capture. Required for absolute timing, dry/wet phase coherence, and
  sub-sample aligning all kernels to a common origin (§5.4).
- **Level-axis units:** a sweep is constant-amplitude with ~3 dB crest factor;
  program material is 10–20 dB. Pin the level axis to the **analog input level at
  the unit** (calibrated, e.g. −18 dBFS ↔ +4 dBu), and give the plugin a mapping
  from runtime envelope statistics to that axis. Don't push the AD past 0 dBFS —
  stage gain so "+3 dB" lands at the unit, not the converter.

### Two datasets

- **Dataset A — static personality:** the `level × knob` grid of ESS captures →
  h1 bank + h2/h3 harmonic kernels. Quasi-static; ESS smears any fast dynamics
  during the sweep, so this is the *average* response at each operating point.
- **Dataset B — temporal behavior** → parameters for the StateEstimator (§5.3):
  - step up/down at several levels → attack/release → `recovery_ms`
  - ascending vs descending level staircase → `hysteresis`
  - quiet→transient→quiet → overshoot/ringing → `transientAmount`
  - two-tone IMD (e.g. 19+20 kHz) → quantify/validate the Hammerstein nonlinearity
  - repeat-over-time → thermal drift → noise/variant layer

### Hardware setup
Interface Out → unit → Interface In; same clock; AGC/limiters/monitoring FX off;
96 kHz / 24-bit. IRs are sample-rate-specific — resample
`sourceSampleRate → runtimeSampleRate` carefully (`RuntimeProfile` tracks both).

### Validation metrics per kernel
onset confidence, peak level, noise floor, DC offset, length adequacy, L/R phase
consistency, deviation across repeats, harmonic-IR SNR.

### Build-order note
The capture utility is ~half the project. **Do not let it gate Milestone 0** —
hand-capture 3–6 kernels (a sweep + any deconvolution tool, or synthetic) to prove
the engine sounds magical first; build the polished utility after the go/no-go gate
(§10).

---

## 7. Parameters (APVTS), automation, MIDI

Stable IDs (permanent): `input_gain_db`, `output_gain_db`, `mix`, `drive`,
`profile_id`, `tone`, `dynamic_depth`, `recovery_ms`, `transient_sensitivity`,
`kernel_smoothing_ms`, `quality_mode`, `variant_amount`, `noise_amount`, `bypass`.

- Smooth every continuous automatable param with `SmoothedValue`.
- **Profile changes are NOT sample-accurate**: UI/host sets a requested profile id
  → background thread loads → audio thread receives immutable profile → crossfade.
- `drive` and `noise_amount` should default low — the design leans on movement, not
  saturation/noise (§1).
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

**Undo/redo — important correction:**
- Do **NOT** pass an `UndoManager` to the APVTS that owns automatable params — it
  would record host-automation playback as undo transactions and pollute the tree.
- Drive undo through a **separate `ValueTree` + `UndoManager`** for *discrete UI
  actions only*: profile selection, preset rename, MIDI mapping changes, macro
  assignment, advanced-setting changes, capture relinking.
- Host automation is the source of truth during playback and is never undoable.
  Meters/internal dynamic state are never undoable.

**Presets** (`.dcpreset`): APVTS values + profile reference + MIDI/macro maps +
advanced settings. Never includes the IR library or cached FFT data.

---

## 9. CPU / quality modes

`Eco` (2 kernels, linear, short IR) · `Normal` (4 kernels, cubic on level, medium
IR) · `High` (8 kernels, cubic on level+knob, long IR) · `Render` (max, offline).
Real-time and offline-render quality may differ. Prefer a **single fixed latency
across the real-time modes** (§4); treat a mode change like a profile swap (reload
the convolver set) rather than retuning latency live during playback.

---

## 10. Build order, milestones & MVP scope

**Project decision (2026-06-23):** the dynamic-convolution *sound* was already
validated by Ward's 2018 prototype, so Milestone 0's "does it sound magical?"
purpose is essentially satisfied. Captures are labor-intensive and gate all
downstream real-world data, so we are **building the Capture Tool FIRST** (see the
Capture Tool plan at the end of this section and §6), then the plugin engine.
Milestone 0 remains a useful quick sanity check once the tool produces real
captures, but it no longer blocks.

**Default plugin order** (applies once engine work begins — **prove DSP/sound
before plugin plumbing**):

1. Milestone 0 offline / standalone renderer (below)
2. Real/synthetic kernel-bank loading + test
3. Warm convolver pool + weighted interpolation
4. Envelope / state estimator
5. Small nonlinear stage (and, when ready, the PRE/POST topology)
6. **Render comparison** against the real hardware (and/or the old prototype) —
   null/residual + ABX. This is the **go/no-go gate**.
7. *Only then* the JUCE plugin shell (VST3/AU/Standalone)
8. APVTS automation
9. State restore / presets
10. UI / MIDI / undo

### Milestone 0 — prove the sound (do this FIRST)
A Standalone/offline render: load real (or synthetic) level kernels → envelope →
fixed warm 2-convolver pool → weighted output interpolation → small drive
waveshaper → file/realtime out. **Goal: confirm it still sounds magical.** No
presets, no undo, no MIDI, no fancy UI. If the sound isn't there, fix the DSP
before building plumbing. (This also fits the workflow need to hear "oh, it works"
early instead of drowning in APVTS/MIDI/preset yak-shaving.)

### MVP (build only after step 6 convinces you)
1. JUCE plugin shell (VST3/AU/Standalone) 2. APVTS params 3. basic UI
4. static profile loading 5. level-only dynamic convolution (warm pool)
6. 2-kernel output interpolation 7. small nonlinear drive stage 8. dry/wet + in/out
gain 9. state save/load 10. preset save/load 11. simple `.dcpack` parser.

**Do NOT build first:** ML residual, stochastic kernel variants, beautiful UI,
in-plugin capture workflow, multi-axis grids, cloud/marketplace.

**MVP success criterion:** with one captured unit (multiple level kernels), the
plugin smoothly morphs level-dependent responses with subtle drive, no clicks,
correct automation/save/restore/undo, reported latency, and stable CPU.

### Capture Tool — phased plan (the current first build)

A **separate JUCE console-app target** (`tools/capture/`, not the plugin binary).
One stack end-to-end (JUCE `dsp::FFT`, `AudioFormat`, `AudioBuffer`) so the DSP
carries straight into the live front-end and the plugin — no throwaway context.

1. **Offline DSP core + CLI + self-tests** — sweep generation, ESS deconvolution,
   harmonic-order separation (h1 + h2/h3), loopback-latency + sub-sample alignment,
   normalization, `.dcpack` writer/reader. Verifiable with no audio hardware: a
   synthetic sweep through a known polynomial nonlinearity must recover the known
   harmonic kernels; round-trip a `.dcpack`. Locks the format (keep it
   engine-driven — don't capture data the engine can't use, or omit what it needs).
2. **Live capture front-end** (`juce_audio_devices`) — scripted sequence
   (condition → settle → sweep → settle → store), N reps → mean + std, loopback
   reference, level calibration.
3. **GUI / polish** — operating-point grid editor, run/abort, validation meters,
   `.dcpack` browser. The "press Capture Profile" experience.

Dataset B (temporal, §6) follows once Dataset A captures work.

---

## 11. Testing (required)

DSP/correctness:
- Static IR mode equals plain convolution (null test).
- Interpolating identical kernels is bit-neutral (or within float epsilon).
- Capture extraction from a synthetic IR train recovers the known IR.
- No allocation in `processBlock` (assert/instrument in debug).
- Profile swap under audio load: no clicks, no NaN/inf, no denormals.
- Latency reporting matches actual measured latency; latency stable per mode.
- Save/load and preset roundtrips; missing-profile recovery.
- Sample-rate conversion correctness; mono/stereo bus behavior.

Listening / validation (the scientific backing the project is after):
- Dry vs hardware vs plugin; ABX blind test; null/residual energy + THD/IMD vs a
  reference. A perfect model will NOT null to silence (it's an approximation) —
  judge by residual energy, harmonic structure, and ABX, not bit-equality.
- Sanity check the layer split: at constant level the convolution path alone must
  add no harmonics (it's linear); harmonic content must come from the NL stage.

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
├── dsp/         DynamicConvolutionEngine.{h,cpp}  StateEstimator.h  ConvolutionBank.h
│                KernelInterpolator.h  NonlinearStage.h  NoiseResidual.h  DryWetMixer.h
├── capture/     CaptureManifest.h  CaptureLibrary.h  CaptureLoader.h  KernelGrid.h  RuntimeProfile.h  HammersteinKernels.h
├── state/       StateStore.h  PresetManager.h  MidiMappings.h
├── ui/          MainPanel.h  MeterView.h  KernelMapView.h  CaptureBrowser.h  AdvancedPanel.h
└── tests/       CaptureExtractionTests.cpp  StateRoundtripTests.cpp  DynamicConvolutionTests.cpp
```

The **Statebox Capture Tool** (ESS → deconvolve → separate h1/h2/h3 → align →
normalize → write `.dcpack`) is a **separate JUCE console-app target** at
`tools/capture/` (not part of the plugin binary). Per §10 it is the **first** thing
built (offline DSP core), with live audio I/O and a GUI layered on later.

---

## 14. Coding conventions

- C++20; match JUCE style for JUCE-touching code. RAII; no raw owning pointers.
- DSP types are RT-safe by construction; document any method as `[RT-safe]` or
  `[message-thread only]` / `[background thread]`.
- Keep `meta`/manifest formats versioned (`formatVersion`) and forward-tolerant.
- Build module-by-module, each landing with its tests green. Prefer small PRs.
