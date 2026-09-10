#include "thread_pin.hpp"

#include <pthread.h>

#if defined(__linux__)
#include <sched.h>
#elif defined(__APPLE__)
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#endif

namespace md {

bool pin_current_thread(int cpu_index) noexcept {
    if (cpu_index < 0) return false;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<unsigned>(cpu_index), &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#elif defined(__APPLE__)
    // Not a hard pin. Tag is best-effort; threads with different tags
    // are *preferred* on different cores.
    thread_affinity_policy_data_t policy;
    policy.affinity_tag = cpu_index + 1;
    return thread_policy_set(pthread_mach_thread_np(pthread_self()),
                             THREAD_AFFINITY_POLICY,
                             reinterpret_cast<thread_policy_t>(&policy),
                             THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;
#else
    (void)cpu_index;
    return false;
#endif
}

} // namespace md
