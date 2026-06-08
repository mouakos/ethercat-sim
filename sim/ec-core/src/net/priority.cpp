#include "net/priority.hpp"

#include <cstdio>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace net {

void raise_responder_priority() noexcept {
#if defined(_WIN32)
    // ABOVE_NORMAL_PRIORITY_CLASS + THREAD_PRIORITY_HIGHEST: enough to avoid
    // routine preemption, but deliberately NOT TIME_CRITICAL — a time-critical
    // thread that wakes every poll interval can starve Npcap's own packet-
    // delivery threads and *increase* response latency (observed: more OP drops).
    if (SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) == 0) {
        std::fprintf(stderr, "warning: SetPriorityClass(ABOVE_NORMAL) failed (%lu)\n",
                     static_cast<unsigned long>(GetLastError()));
    }
    if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) == 0) {
        std::fprintf(stderr, "warning: SetThreadPriority(HIGHEST) failed (%lu)\n",
                     static_cast<unsigned long>(GetLastError()));
    }
    // Pin the responder thread to the top logical core to stop the scheduler
    // migrating it (cache/TLB churn is a jitter source). This is a *preferred*
    // core, not a reserved one — Windows can't evict the OS from a core in
    // userspace (that needs a kernel RT driver). Run diagnostics pinned to other
    // cores (start /affinity) so they don't contend with the responder.
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    if (si.dwNumberOfProcessors >= 2) {
        const DWORD_PTR mask = static_cast<DWORD_PTR>(1)
                               << (si.dwNumberOfProcessors - 1);
        if (SetThreadAffinityMask(GetCurrentThread(), mask) == 0) {
            std::fprintf(stderr, "warning: SetThreadAffinityMask(core %lu) failed (%lu)\n",
                         static_cast<unsigned long>(si.dwNumberOfProcessors - 1),
                         static_cast<unsigned long>(GetLastError()));
        } else {
            std::printf("responder pinned to core %lu (priority ABOVE_NORMAL/HIGHEST)\n",
                        static_cast<unsigned long>(si.dwNumberOfProcessors - 1));
        }
    }
#endif
}

}  // namespace net
