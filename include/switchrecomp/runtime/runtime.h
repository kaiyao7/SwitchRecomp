#pragma once

#include "switchrecomp/common/types.h"
// sr_vreg: unified AArch64 SIMD&FP vector register bank (SIMD phase 1).
#include "switchrecomp/runtime/vreg.h"

namespace sr::runtime {

using sr::common::u32;
using sr::common::u64;
using sr::common::usize;

// Full guest CPU state (ARM64 AArch64).
struct GuestContext {
    u64 x[31];   // x0..x30 (x30 = LR)
    u64 sp;
    u64 pc;
    u32 nzcv;    // flags
    // Unified SIMD&FP register bank: V0..V31, 128-bit each. The B/H/S/D scalar
    // views and the lane views are aliases accessed through the sr_v_* helpers
    // in vreg.h (scalar writes zero the unused upper bits).
    sr_vreg v[32];
    u32 fpcr;
    u32 fpsr;
    // HLE scratch state used by the thread synchronization SVCs. Kept as plain
    // scalars so GuestContext stays POD / standard-layout. accessed under a
    // runtime-internal mutex from worker threads.
    u64 shared_counter;   // accumulated by worker threads (Join makes it stable)
    u64 thread_handle;    // most recently created thread handle
};

static_assert(sizeof(GuestContext) >= 0, "GuestContext must be standard layout");

// Convert a guest virtual address to a host pointer.
// Returns nullptr on unmapped / permission error.
void* GuestToHost(GuestContext* ctx, u64 guest_addr, usize size, bool write);

// Perform an indirect guest call (used by codegen for bl/br with unknown target).
void CallGuest(GuestContext* ctx, u64 target_addr);

// Dispatch an SVC instruction.
void HandleSvc(GuestContext* ctx, u32 svc_id);

} // namespace sr::runtime
