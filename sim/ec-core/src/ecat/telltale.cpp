// Tell-tale instrumentation — see telltale.hpp for the what/why and, crucially,
// how to find (grep -rn "TELLTALE" sim/) and remove the whole thing later.

#include "ecat/telltale.hpp"

#include <array>
#include <atomic>
#include <cstdio>

namespace ecat::telltale {
namespace {

std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Signal::kCount)>
    g_counts{};

std::uint64_t load(Signal s) noexcept {
    return g_counts[static_cast<std::size_t>(s)].load(std::memory_order_relaxed);
}

}  // namespace

void bump(Signal s) noexcept {
    g_counts[static_cast<std::size_t>(s)].fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t value(Signal s) noexcept { return load(s); }

bool any_anomaly() noexcept {
    return load(Signal::kRequestRetry) != 0 || load(Signal::kReplyUncollected) != 0 ||
           load(Signal::kSdoAbort) != 0 || load(Signal::kCoeUnhandled) != 0 ||
           load(Signal::kMailboxMalformed) != 0 || load(Signal::kSmUnconfigured) != 0 ||
           load(Signal::kBoundsGuard) != 0;
}

bool report(bool force) noexcept {
    if (!force && !any_anomaly()) {
        return false;  // healthy — stay silent
    }
    std::fprintf(
        stderr,
        "[telltale] req=%llu staged=%llu collected=%llu | retry=%llu uncollected=%llu "
        "abort=%llu unhandled=%llu malformed=%llu sm-unconfigured=%llu bounds=%llu "
        "| sm0-read=%llu mbx-buf-read=%llu deferred=%llu\n",
        static_cast<unsigned long long>(load(Signal::kMailboxRequest)),
        static_cast<unsigned long long>(load(Signal::kReplyStaged)),
        static_cast<unsigned long long>(load(Signal::kReplyCollected)),
        static_cast<unsigned long long>(load(Signal::kRequestRetry)),
        static_cast<unsigned long long>(load(Signal::kReplyUncollected)),
        static_cast<unsigned long long>(load(Signal::kSdoAbort)),
        static_cast<unsigned long long>(load(Signal::kCoeUnhandled)),
        static_cast<unsigned long long>(load(Signal::kMailboxMalformed)),
        static_cast<unsigned long long>(load(Signal::kSmUnconfigured)),
        static_cast<unsigned long long>(load(Signal::kBoundsGuard)),
        static_cast<unsigned long long>(load(Signal::kSm0StatusRead)),
        static_cast<unsigned long long>(load(Signal::kMailboxBufferRead)),
        static_cast<unsigned long long>(load(Signal::kMailboxDeferred)));
    return true;
}

}  // namespace ecat::telltale
