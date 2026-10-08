#pragma once

// Internal HLE implementation of the thread / synchronization SVCs.
//
// Lives under src/runtime because it is an internal runtime facility; the
// public runtime header is unchanged. Handlers are registered by name with
// the process-global HleServiceRegistry and dispatched through it.
//
// SVC id conventions used here are project-internal placeholders (they do not
// necessarily match Horizon kernel numbers); see the table in svc_thread.cpp.

#include "switchrecomp/common/types.h"

#include <string_view>

namespace sr::runtime {

using sr::common::u32;

class HleServiceRegistry;

// Register all thread / synchronization SVC handlers (CreateThread,
// StartThread, ExitThread, WaitSynchronization, SleepThread, GetResult) with
// |registry| by name. Duplicate registrations are ignored (the registry
// rejects them), so this is safe to call more than once.
void RegisterThreadSvcs(HleServiceRegistry& registry);

// Resolve a thread-related SVC id to its registered service name; returns an
// empty view when |svc_id| is not one of the thread SVCs.
std::string_view ThreadSvcName(u32 svc_id);

} // namespace sr::runtime
