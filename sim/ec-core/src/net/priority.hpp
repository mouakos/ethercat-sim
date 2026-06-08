// Process/thread scheduling priority control (L1 timing).
//
// The EtherCAT master drops a slave that misses too many cyclic frames. Our
// responder runs in userspace, so OS scheduling jitter can make us miss a
// cycle. Raising the priority keeps the capture/respond loop on-CPU and cuts
// the jitter that causes periodic OP drops.

#pragma once

namespace net {

// Raise the current process and thread to a high (not real-time) scheduling
// priority. Best-effort: logs a warning and continues if it cannot. Needs no
// elevation (HIGH_PRIORITY_CLASS, unlike REALTIME, is available to normal users).
void raise_responder_priority() noexcept;

}  // namespace net
