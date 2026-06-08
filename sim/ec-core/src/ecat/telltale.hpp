// ============================================================================
//  TELL-TALES  —  removable diagnostic instrumentation for the mailbox/CoE path
// ============================================================================
//
//  WHAT THIS IS
//  ------------
//  A small set of always-on, very cheap counters that flag mailbox/CoE
//  anomalies early — e.g. a master retrying because it rejected our reply, an
//  SDO we had to abort, a malformed request, or mailbox traffic before the
//  SyncManagers are configured. Plus a one-line health summary printed off the
//  per-frame path.
//
//  WHY IT IS SAFE FOR THE REAL-TIME PATH
//  -------------------------------------
//   * Each signal is a single relaxed atomic increment (~1 ns): no allocation,
//     no lock, never blocks. The CoE/mailbox path is acyclic anyway; the couple
//     of call sites that sit on the cyclic read path are guarded and trivial.
//   * The summary line is emitted only periodically (caller-driven, e.g. every
//     2 s) and only when something has tripped, so healthy operation is silent.
//
//  HOW TO FIND EVERY TELL-TALE  (the whole point of this module)
//  ------------------------------------------------------------
//        grep -rn "TELLTALE" sim/
//  Every call site is tagged with a `// TELLTALE` comment, and all the machinery
//  lives in telltale.hpp / telltale.cpp (+ test_telltale.cpp).
//
//  HOW TO REMOVE THEM  (a future iteration, once weeks of activity trip nothing)
//  ---------------------------------------------------------------------------
//   1. Delete telltale.hpp, telltale.cpp, test_telltale.cpp and their CMake refs.
//   2. Delete every line tagged `// TELLTALE` — each is self-contained: a bump()
//      call, the one Esc field `mbx_last_req_counter`, and the report()/announce
//      calls in main.cpp.
//  Nothing else depends on this module — removal cannot change slave behaviour.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace ecat::telltale {

// The signals we count. Keep this list short and meaningful — each one should be
// a question worth answering at a glance. The first three are benign *activity*
// totals; the rest are *anomalies* worth investigating.
enum class Signal : std::size_t {
    kMailboxRequest = 0,  // a CoE mailbox request was serviced            (activity)
    kReplyStaged,         // a reply was staged into the mailbox-in SM      (activity)
    kReplyCollected,      // the master read a staged reply back            (activity)
    kRequestRetry,        // request reused the previous counter: the master retried,
                          //   i.e. it rejected our last reply         (*** key canary ***)
    kReplyUncollected,    // we staged a reply while the previous one's full bit was
                          //   still set: the master never read it (it isn't reading us
                          //   — the cyclic-mode SAFEOP symptom)        (*** key canary ***)
    kSdoAbort,            // an SDO request was answered with an abort
    kCoeUnhandled,        // a CoE service / opcode we do not implement
    kMailboxMalformed,    // request too short / structurally invalid
    kSmUnconfigured,      // mailbox traffic while the SyncManagers aren't configured
    kBoundsGuard,         // a bounds check prevented an out-of-range access
    // Evidence counters (not anomalies) for the SAFEOP / SM0-handshake
    // investigation — see docs/coe-safeop-brief.md. They answer "what is the
    // master actually doing in cyclic mode?" and are reported but never trip
    // any_anomaly(). Safe to delete once the SAFEOP path is understood.
    kSm0StatusRead,       // master read the SM0 (mailbox-out) status byte 0x0805  (evidence)
    kMailboxBufferRead,   // master read the SM1 reply buffer start 0x1080         (evidence)
    kMailboxDeferred,     // a request arrived while the previous reply was unread; we
                          //   deferred it (single-buffer backpressure) instead of
                          //   clobbering the staged reply                          (evidence)
    kCount
};

// Increment a signal's counter. Relaxed atomic; safe to call from the RT
// responder thread.
void bump(Signal s) noexcept;

// Read a signal's current count.
[[nodiscard]] std::uint64_t value(Signal s) noexcept;

// True if any *anomaly* signal (everything except the benign activity totals
// kMailboxRequest / kReplyStaged / kReplyCollected) is non-zero.
[[nodiscard]] bool any_anomaly() noexcept;

// Print a one-line "[telltale] ..." health summary to stderr. By default prints
// only when any_anomaly() is true (so healthy operation stays silent); pass
// force=true to print regardless. Returns true if it printed. Call this off the
// per-frame hot path (e.g. on a ~2 s timer).
bool report(bool force = false) noexcept;

}  // namespace ecat::telltale
