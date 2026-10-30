// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace Nx2Sampler {

inline constexpr double MaxDelaySeconds = 600.0;
inline constexpr double MaxDurationSeconds = 120.0;
inline constexpr std::size_t MaxTargets = 16;
inline constexpr std::size_t MaxSamples = 262144;
inline constexpr std::size_t MaxPatterns = 32;
inline constexpr std::size_t MaxPatternLength = 255;
inline constexpr std::size_t MaxOutputPathLength = 4096;

struct Options {
    // Exact host names, or trailing '*' prefix matches, e.g. CPUCore_* and GPUBack.
    std::vector<std::string> patterns;
    double delay_seconds = 60.0;
    double duration_seconds = 30.0;
    std::string output_path = "nx2_samples.txt";
};

enum class State { Disabled, Waiting, Sampling, Reporting, Completed, Cancelled, Failed };

struct Status {
    State state = State::Disabled;
    std::size_t targets = 0;
    std::size_t samples = 0;
    // Nonempty for Failed, including invalid input, unsupported platform, and output errors.
    std::string error;
};

// Construct on the SDL lifecycle thread: that thread is never a sampling target (even '*').
// A session accepts at most one start attempt. All methods are serialized internally.
// A rejected repeat returns Failed without overwriting GetStatus() for the first attempt.
// Stop is idempotent, cancels waits/collection/reporting, and joins; call it BEFORE core shutdown.
// Cancellation is cooperative: an in-progress native unwind, symbol lookup or file
// operation must return before joining. A bounded process supervisor is the backstop.
// The destructor also stops, but is not a substitute for that shutdown ordering.
// Discovery rejects >16 matches or a duration/target budget >262144 samples (1100 Hz + margin).
// Reporting fails above 65536 symbol addresses or 32 MiB of distinct folded stack keys.
// Cancellation during collection writes no reports; during reporting files may be partial.
class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Status Start(Options options);
    // EDEN_NX2_SAMPLE: comma-separated patterns; absent/empty means Disabled.
    // EDEN_NX2_SAMPLE_START / EDEN_NX2_SAMPLE_SECONDS: strict finite seconds, defaults 60/30.
    // EDEN_NX2_SAMPLE_OUT: report path (default nx2_samples.txt); also writes <path>.folded.
    // Invalid or over-limit requests fail, rather than being silently clamped.
    Status StartFromEnvironment();
    Status GetStatus() const;
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Nx2Sampler
