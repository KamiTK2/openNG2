#pragma once
// NG2 DIAG: whole-program guest function entry counter (see tools/gen_trace_all.py).
#include <atomic>
#include <cstdint>

namespace ng2trace {
inline std::atomic<bool> g_on{false};
extern const uint32_t kAddrs[];
extern uint32_t g_counts[];
extern uint32_t g_lr[];
uint32_t Count();
void StartTriggerThread();
}  // namespace ng2trace
