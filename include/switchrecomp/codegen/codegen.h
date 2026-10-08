#pragma once

#include "switchrecomp/common/result.h"
#include "switchrecomp/ir/ir.h"

#include <string>

namespace sr::codegen {

struct CodegenOptions {
    std::string module_name = "guest";
    bool emit_comments = true;
};

// Lower an IR Module to C11 source code.
common::Result<std::string> GenerateC(const ir::Module& module,
                                      const CodegenOptions& options = {});

} // namespace sr::codegen
