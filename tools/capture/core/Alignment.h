#pragma once

#include <vector>

namespace statebox::capture
{

// Fractional position of the largest-magnitude peak, via parabolic interpolation
// around the integer peak. Used to find a kernel's true sub-sample onset so the
// whole grid can share a common origin (CLAUDE.md §5.4: misaligned kernels comb
// when interpolated).
double estimatePeakPosition (const std::vector<float>& ir);

// Shift a signal by deltaSamples (positive = delay) using an FFT phase ramp.
// Exact for band-limited signals. Intended for sub-sample residuals; the integer
// part is applied by index shifting first to minimise circular wrap-around.
std::vector<float> fractionalShift (const std::vector<float>& ir, double deltaSamples);

// Shift so the estimated peak lands exactly on targetIndex (integer part by index
// move + zero fill, fractional part by phase ramp).
std::vector<float> alignPeakTo (const std::vector<float>& ir, double targetIndex);

} // namespace statebox::capture
