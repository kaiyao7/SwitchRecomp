#pragma once

#include "switchrecomp/common/result.h"
#include "switchrecomp/common/span.h"
#include "switchrecomp/common/types.h"

#include <functional>
#include <map>
#include <vector>

namespace sr::gpu {

using sr::common::Result;
using sr::common::span;
using sr::common::u16;
using sr::common::u32;
using sr::common::u8;
using sr::common::usize;

// Decoded FIFO command word (simplified Maxwell layout):
//   [11:0]  method
//   [15:13] subchannel
//   [29:16] count (long packet word count / immediate 16-bit data)
//   [31]    opcode (0 = immediate, 1 = long packet)
struct FifoWord {
    u16 method;     // bits [11:0]
    u8 subchannel;  // bits [15:13]
    u8 opcode;      // bit  [31]   (0 = immediate, 1 = long packet)
    u16 count;      // bits [29:16]
    bool is_long;   // opcode == 1
};

// Decode a 32-bit FIFO word into its fields.
FifoWord ParseFifoWord(u32 word);

// Method handler: (subchannel, method, data) -> true on success.
// For long packets `data` holds the `count` payload words; for immediate
// packets it holds a single word containing the immediate value.
using MethodHandler =
    std::function<bool(u8 subchannel, u16 method, span<const u32> data)>;

// Parses guest FIFO command buffers and dispatches methods to handlers.
class CommandProcessor {
public:
    // Register a handler for a method. Returns false if already registered.
    bool RegisterHandler(u16 method, MethodHandler handler);
    // Remove a handler. Returns false if the method was not registered.
    bool UnregisterHandler(u16 method);

    // Process a buffer of FIFO words. Methods without a registered handler
    // are skipped. The handler receives the word's subchannel field, or
    // `subchannel` when that field is zero. Returns the number of words
    // consumed, or an Error when the buffer is malformed or a handler
    // reports failure.
    Result<usize> ProcessBuffer(span<const u32> buffer, u8 subchannel = 0);

    // List registered methods in ascending order.
    std::vector<u16> ListMethods() const;

private:
    std::map<u16, MethodHandler> handlers_;
};

} // namespace sr::gpu
