// HLE implementation of the thread / synchronization SVCs.
//
// The handlers are registered by name with the process-global
// HleServiceRegistry (see svc_table.cpp) and dispatched through it, so
// recompiled guest code reaches them via `sr_rt_handle_svc(ctx, imm16)`.
//
// Design (per the simplified HLE contract):
//   - CreateThread starts a host worker thread immediately; the worker does
//     not execute translated guest code. Instead it atomically increments
//     ctx->shared_counter a fixed number of times under the runtime mutex.
//     This models "the guest worker performed some work" and is enough to
//     verify the runtime's thread HLE without a real translator round-trip
//     for the worker path.
//   - WaitSynchronization joins the thread identified by ctx->x[0].
//   - GetResult copies ctx->shared_counter into ctx->x[0] for inspection.
//
// Concurrency safety: a single process-global mutex guards both the thread
// handle table and ctx->shared_counter increments. WaitSynchronization
// removes the thread from the table under the mutex, then joins OUTSIDE the
// mutex to avoid deadlocking against workers that need the same mutex.
#include "svc_thread.h"

#include "hle_service.h"
#include "switchrecomp/platform/thread.h"

#include <map>
#include <memory>
#include <thread>

namespace sr::runtime {

namespace {

// Number of increments each worker performs. Chosen large enough that a
// missing lock would observably lose updates on typical test hardware.
constexpr u64 kWorkerIncrements = 10000;

// ---- Process-global thread state ----

platform::Mutex& ThreadStateMutex() {
    static platform::Mutex m;
    return m;
}

// handle -> worker thread. unique_ptr because platform::Thread is move-only
// (it wraps std::thread) and is not copyable.
std::map<u64, std::unique_ptr<platform::Thread>>& ThreadTable() {
    static std::map<u64, std::unique_ptr<platform::Thread>> table;
    return table;
}

// Monotonic handle source. Must be called under ThreadStateMutex().
u64 AllocateHandle() {
    static u64 next = 0;
    return ++next;
}

// ---- SVC handlers ----

void svc_CreateThread(GuestContext* ctx) {
    // Create + start the worker immediately (StartThread is a no-op).
    u64 handle = 0;
    {
        platform::MutexGuard g(ThreadStateMutex());
        handle = AllocateHandle();
        // The worker captures the guest context by raw pointer. The guest
        // contract is that WaitSynchronization is issued before the context
        // is destroyed, so the pointer stays valid for the worker's lifetime.
        ThreadTable()[handle] = std::unique_ptr<platform::Thread>(
            new platform::Thread([ctx]() {
                for (u64 i = 0; i < kWorkerIncrements; ++i) {
                    platform::MutexGuard g(ThreadStateMutex());
                    ++ctx->shared_counter;
                }
            }));
    }
    ctx->thread_handle = handle;
    ctx->x[0] = handle; // return the thread handle to the guest
}

void svc_StartThread(GuestContext* ctx) {
    // Simplified: CreateThread already started the worker. Report success.
    ctx->x[0] = 0;
}

void svc_ExitThread(GuestContext* /*ctx*/) {
    // No-op: exiting the calling host thread would terminate the process.
    // The simplified HLE model does not run translated guest code in workers,
    // so there is no guest thread to actually exit.
}

void svc_WaitSynchronization(GuestContext* ctx) {
    // ctx->x[0] = handle of the thread to join.
    const u64 handle = ctx->x[0];
    std::unique_ptr<platform::Thread> thread;
    {
        platform::MutexGuard g(ThreadStateMutex());
        auto& table = ThreadTable();
        auto it = table.find(handle);
        if (it != table.end()) {
            thread = std::move(it->second);
            table.erase(it);
        }
    }
    // Join outside the mutex: workers need the mutex to increment.
    if (thread) {
        thread->Join();
    }
    ctx->x[0] = 0; // success
}

void svc_SleepThread(GuestContext* /*ctx*/) {
    // Sleep(0): yield the host CPU. Models the guest yielding its timeslice.
    std::this_thread::yield();
}

void svc_GetResult(GuestContext* ctx) {
    // Expose the accumulated counter to the guest via x0.
    ctx->x[0] = ctx->shared_counter;
}

// SVC id -> (name, handler) table. Ids are project-internal placeholders; they
// are intentionally grouped with the existing 0x0A-0x0C range used by the
// threads fixture and do not claim to match Horizon kernel numbers.
struct ThreadSvcEntry {
    u32 id;
    const char* name;
    HleHandler handler;
};

const ThreadSvcEntry kThreadSvcEntries[] = {
    {0x0A, "CreateThread",        &svc_CreateThread},
    {0x0B, "WaitSynchronization", &svc_WaitSynchronization},
    {0x0C, "GetResult",           &svc_GetResult},
    {0x22, "StartThread",         &svc_StartThread},
    {0x23, "ExitThread",          &svc_ExitThread},
    {0x24, "SleepThread",         &svc_SleepThread},
};

} // namespace

void RegisterThreadSvcs(HleServiceRegistry& registry) {
    for (const ThreadSvcEntry& entry : kThreadSvcEntries) {
        // Register ignores duplicate names (returns false) which is fine:
        // built-in initialization may run more than once across tests.
        registry.Register(entry.name, entry.handler);
    }
}

std::string_view ThreadSvcName(u32 svc_id) {
    for (const ThreadSvcEntry& entry : kThreadSvcEntries) {
        if (entry.id == svc_id) return entry.name;
    }
    return {};
}

} // namespace sr::runtime
