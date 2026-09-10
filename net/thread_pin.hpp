#pragma once

#include <cstdint>

namespace md {

// Pin current thread. Linux: real cpuset via pthread_setaffinity_np.
// macOS: THREAD_AFFINITY_POLICY is a *hint* (affinity tag), not a hard
// core pin — do not treat it as isolation from the matching thread.
bool pin_current_thread(int cpu_index) noexcept;

} // namespace md
