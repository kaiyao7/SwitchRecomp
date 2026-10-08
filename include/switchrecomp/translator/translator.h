#pragma once

#include "switchrecomp/common/result.h"
#include "switchrecomp/common/span.h"
#include "switchrecomp/common/types.h"
#include "switchrecomp/ir/ir.h"

#include <memory>

namespace sr::translator {

using sr::common::u64;
using sr::common::u8;

struct TranslateOptions {
    u64 base_address = 0;
    bool strict_decode = false; // error on unimplemented opcodes vs emit Undefined
};

// Translate a buffer of ARM64 bytes into an IR Module.
common::Result<std::unique_ptr<ir::Module>> Translate(
    common::span<const u8> code,
    const TranslateOptions& options = {});

} // namespace sr::translator
