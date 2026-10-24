#include "host_context.h"

#include <cstdlib>

namespace {
// The worker yields twice; each return to the scheduler must preserve both
// context identities and the worker's continuation point.
HostContext::Handle g_scheduler = nullptr;
HostContext::Handle g_worker = nullptr;
int g_steps = 0;
int g_memory = 0;
void ContinuationProbe(void*) {
    int local = 0;
    for (;;) {
        g_memory = ++local;
        HostContext::Switch(g_scheduler);
    }
}

void Worker(void*)
{
    if (HostContext::Current() != g_worker) {
        std::abort();
    }
    ++g_steps;
    HostContext::Switch(g_scheduler);
    if (HostContext::Current() != g_worker) {
        std::abort();
    }
    ++g_steps;
    HostContext::Switch(g_scheduler);
}
} // namespace

int main()
{
    if (!HostContext::InitializeScheduler(&g_scheduler) ||
        HostContext::Current() != g_scheduler) {
        return 1;
    }
    g_worker = HostContext::Create(64 * 1024, Worker, nullptr);
    if (!g_worker) {
        return 1;
    }

    HostContext::Switch(g_worker);
    if (g_steps != 1 || HostContext::Current() != g_scheduler) {
        return 1;
    }
    HostContext::Switch(g_worker);
    if (g_steps != 2 || HostContext::Current() != g_scheduler) {
        return 1;
    }

    HostContext::Destroy(g_worker);
    // A RAM/register-only restore cannot rewind native locals or a native return PC.
    g_worker = HostContext::Create(64 * 1024, ContinuationProbe, nullptr);
    if (!g_worker) return 1;
    HostContext::Switch(g_worker);
    const int savedMemory = g_memory; // save at logical step 1
    HostContext::Switch(g_worker);    // advance to logical step 2
    g_memory = savedMemory;          // restore all simulated guest memory
    HostContext::Switch(g_worker);   // the native continuation still advances to 3
    if (g_memory != 3) return 1;     // a true restored continuation would produce 2
    HostContext::Destroy(g_worker);
    HostContext::ShutdownScheduler(g_scheduler);
}
