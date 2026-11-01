#pragma once

#include "common/types.h"

// The emulator's notion of time, measured in guest CPU cycles. Guest-visible time (the
// timebase, timers) derives from this, never from the host clock, so runs are deterministic.
class VirtualClock {
public:
    static constexpr u64 k_cpu_frequency = 3'200'000'000; // 3.2 GHz
    // psdevwiki, "LV2 Functions and Syscalls": sys_time_get_timebase_frequency.
    static constexpr u64 k_timebase_frequency = 79'800'000;

    void advance(u64 cycles) { cycles_ += cycles; }
    u64 cycles() const { return cycles_; }

    // The value the PPU's time base register would show after this many cycles.
    u64 timebase() const {
        // 79.8 MHz / 3.2 GHz reduces to 399 / 16000. This overflows after ~160 days of guest time.
        return cycles_ * 399 / 16'000;
    }

    // The first cycle at which timebase() reaches `ticks`.
    static constexpr u64 cycle_of_timebase(u64 ticks) { return (ticks * 16'000 + 398) / 399; }

private:
    u64 cycles_ = 0;
};
