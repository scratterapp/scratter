#pragma once

// Keeping real-time memory in RAM (spec section 4, "Physical RAM Pinning").
//
// If the OS swaps out a page of the ring buffer, the next time the audio
// thread touches it the thread stalls on a page fault while the page is read
// back in, and you hear a click. Locking the pages tells the OS never to do
// that. Call this once on the ring buffer, after allocating it and before
// starting the sink.
//
//   Linux:   mlock(), limited by RLIMIT_MEMLOCK
//   Windows: VirtualLock, after raising the working set size if needed
//
// The audio thread's scheduling priority isn't here: each AudioSink raises
// its own thread (see audio_sink.hpp).

#include <cstddef>
#include <expected>
#include <span>
#include <system_error>

namespace am::pal {

// Locks the pages covering `memory` into RAM, faulting in any that aren't
// resident yet, so the audio thread never takes a page fault on them. Fails
// if the OS limit on locked memory is too low.
std::expected<void, std::error_code> lock_memory(std::span<const std::byte> memory);

// Undoes lock_memory(). Call it before freeing the memory.
std::expected<void, std::error_code> unlock_memory(std::span<const std::byte> memory);

} // namespace am::pal
