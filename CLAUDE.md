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

### Preferred signal architecture (Wiener–Hammerstein-ish)

```
input → input gain → state estimator
      → dynamic PRE convolution     (state-dependent pre-emphasis IR)
      → small nonlinear memory stage (harmonics / saturation)
      → dynamic POST convolution    (state-dependent post-emphasis IR)
      → input-modulated noise / residual
      → dry/wet → output gain → safety limiter
```

This pre-NL-post topology is far more analog-plausible than a single convolution
followed by a waveshaper, and is the **target** architecture. The MVP may start
with a single dynamic convolution → small NL (Milestone 0), then grow the POST
stage once the sound is validated.

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
│   ├── NonlinearStage       (SMALL waveshaper / drive — the ONLY harmonic source)
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

### 5.2 Nonlinear stage (CORE, but intentionally small)

- This is the **only** source of harmonics / saturation / IMD. Required if the gear
  distorts — but keep it subtle (see the ~10% philosophy in §1). The character
  should come from movement, not crunch.
- Topology: prefer **Wiener–Hammerstein** — dynamic PRE convolution → static
  nonlinearity → dynamic POST convolution (§1). The dynamic IRs supply pre/post
  emphasis; the waveshaper supplies the harmonics.
- Start simple: a static waveshaper driven by `drive`, oversampled (2–4×) to avoid
  aliasing. Upgrade path: per-band shaper; later a tiny ML **residual** trained
  against the real hardware error.
- Oversample only the nonlinear stage; the linear convolution path does not need
  oversampling on its own.

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

## 6. Capture format (`.dcpack`)

A capture is a packaged archive (developer reference; large IR audio is NOT stored
in DAW session state — see §8).

```
MyUnit.dcpack
├── manifest.json
├── kernels/   (L_level_00_knob_00.wav, R_level_00_knob_00.wav, ...; multiple variants allowed per cell)
├── noise/     (optional idle noise/hum profiles for the residual layer)
└── metadata/  (measurement notes)
```

`manifest.json` carries: formatVersion, name/vendor, sampleRate,
kernelLengthSamples, channels, axes (level dBFS list, knob list), normalization
(referenceDbfs, trimMode), latencySamples, createdWith.

Two capture modes (support both):
- **A. IR-train capture** — historically close to the 2018 prototype; good for
  level-dependent *snapshots*. Acceptable, lower SNR.
- **B. Exponential sine sweep (Farina)** — better SNR; can separate harmonic
  orders; better for nonlinear / Volterra-style analysis. Preferred for quality
  captures; deconvolve to the IR.

Methodology notes (do not skip):
- A kernel captured at amplitude A (especially via louder impulses) is an
  **effective describing-function snapshot**, not a pure linear IR — the
  nonlinearity is partly baked in. Treat it as such; it does NOT replace the
  explicit nonlinear stage (§5.2).
- IRs are sample-rate-specific. Resample `sourceSampleRate → runtimeSampleRate`
  carefully; `RuntimeProfile` tracks both.
- Validation metrics per kernel: onset confidence, peak level, noise floor, DC
  offset, length adequacy, L/R phase consistency, deviation across repeats.
- Optionally capture the unit's idle noise/hum for the residual layer (§5.5).

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

**Prove the DSP and the *sound* before any plugin plumbing.** Recommended order:

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
