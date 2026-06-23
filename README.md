# Statebox

A measurement-based dynamic-convolution analog-capture audio plugin (JUCE, VST3/AU).

Statebox models a real analog unit as a **black box**: it captures the unit's
behavior across operating points (level, settings) and reconstructs it with
**level-dependent dynamic convolution** plus an **explicit nonlinear stage** — the
combination that gives both the unit's voicing *and* its harmonic character.

See [`CLAUDE.md`](./CLAUDE.md) for the full build specification, architecture, and
real-time-safety rules. Start with **Milestone 0** (prove the sound) before the
plugin scaffold.

## Status

Early. Toolchain setup script: [`install-audio-dev-tools.sh`](./install-audio-dev-tools.sh).

## Quick start

```bash
./install-audio-dev-tools.sh           # one-time: Homebrew, cmake, ninja, ccache, JUCE ref clone
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo   # once CMakeLists exists
cmake --build build
ctest --test-dir build --output-on-failure
```

Requires macOS, CMake ≥ 3.22 (4.x ok), and JUCE 8.x (fetched by the build).
