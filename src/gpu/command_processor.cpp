#include "switchrecomp/gpu/command_processor.h"

#include <cstdio>

namespace sr::gpu {

FifoWord ParseFifoWord(u32 word) {
    FifoWord fw;
    fw.method = static_cast<u16>(word & 0x0FFF);
    fw.subchannel = static_cast<u8>((word >> 13) & 0x7);
    fw.count = static_cast<u16>((word >> 16) & 0x3FFF);
    fw.opcode = static_cast<u8>((word >> 31) & 0x1);
    fw.is_long = fw.opcode == 1;
    return fw;
}

bool CommandProcessor::RegisterHandler(u16 method, MethodHandler handler) {
    if (handlers_.find(method) != handlers_.end()) {
        return false;
    }
    handlers_.emplace(method, std::move(handler));
    return true;
}

bool CommandProcessor::UnregisterHandler(u16 method) {
    return handlers_.erase(method) != 0;
}

Result<usize> CommandProcessor::ProcessBuffer(span<const u32> buffer,
                                              u8 subchannel) {
    usize i = 0;
    while (i < buffer.size()) {
        const FifoWord fw = ParseFifoWord(buffer[i]);
        ++i; // consume the command word itself

        const u8 sc = fw.subchannel != 0 ? fw.subchannel : subchannel;

        span<const u32> data;
        u32 imm = 0;
        if (fw.is_long) {
            if (fw.count > buffer.size() - i) {
                char msg[96];
                std::snprintf(msg, sizeof(msg),
                              "truncated long packet: method 0x%03X declares "
                              "%u words, %zu remain",
                              fw.method, static_cast<unsigned>(fw.count),
                              buffer.size() - i);
                return sr::common::Error{msg};
            }
            data = span<const u32>(buffer.data() + i, fw.count);
            i += fw.count;
        } else {
            imm = fw.count; // immediate payload lives in the count field
            data = span<const u32>(&imm, 1);
        }

        const auto it = handlers_.find(fw.method);
        if (it == handlers_.end()) {
            continue; // unregistered method: skip
        }
        if (!it->second(sc, fw.method, data)) {
            char msg[64];
            std::snprintf(msg, sizeof(msg),
                          "handler failed for method 0x%03X", fw.method);
            return sr::common::Error{msg};
        }
    }
    return i;
}

std::vector<u16> CommandProcessor::ListMethods() const {
    std::vector<u16> out;
    out.reserve(handlers_.size());
    for (const auto& kv : handlers_) {
        out.push_back(kv.first);
    }
    return out;
}

} // namespace sr::gpu
