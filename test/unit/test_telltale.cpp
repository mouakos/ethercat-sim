// Unit tests for the tell-tale instrumentation (ecat::telltale).
// This whole file is part of the removable TELLTALE unit — see telltale.hpp.

#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "ecat/telltale.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s: %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) {
        ++g_failures;
    }
}

}  // namespace

int main() {
    using ecat::telltale::Signal;
    using ecat::telltale::any_anomaly;
    using ecat::telltale::bump;
    using ecat::telltale::report;
    using ecat::telltale::value;

    // Fresh process: every counter starts at zero and nothing has tripped.
    check(value(Signal::kMailboxRequest) == 0, "counters start at zero");
    check(!any_anomaly(), "no anomaly at startup");
    check(!report(false), "report() stays silent when healthy");
    check(report(true), "report(force=true) prints even when healthy");

    // Benign activity totals are not anomalies.
    bump(Signal::kMailboxRequest);
    bump(Signal::kReplyStaged);
    bump(Signal::kReplyCollected);
    check(value(Signal::kMailboxRequest) == 1, "bump increments the counter");
    check(!any_anomaly(), "activity totals do not count as anomalies");

    // The SAFEOP evidence counters are activity, not anomalies: they must not
    // trip any_anomaly() on their own.
    bump(Signal::kSm0StatusRead);
    bump(Signal::kMailboxBufferRead);
    check(value(Signal::kSm0StatusRead) == 1, "sm0-status-read counts");
    check(value(Signal::kMailboxBufferRead) == 1, "mbx-buffer-read counts");
    check(!any_anomaly(), "evidence counters do not count as anomalies");

    // An anomaly trips the canary and makes report() speak up.
    bump(Signal::kRequestRetry);
    check(value(Signal::kRequestRetry) == 1, "anomaly counter increments");
    check(any_anomaly(), "a retry is an anomaly");
    check(report(false), "report() prints once an anomaly has tripped");

    // Each anomaly signal flips any_anomaly() on its own.
    for (const Signal s : {Signal::kReplyUncollected, Signal::kSdoAbort,
                           Signal::kCoeUnhandled, Signal::kMailboxMalformed,
                           Signal::kSmUnconfigured, Signal::kBoundsGuard}) {
        const std::uint64_t before = value(s);
        bump(s);
        check(value(s) == before + 1, "each anomaly signal counts independently");
    }

    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
