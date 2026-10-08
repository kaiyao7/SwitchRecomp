// IR core implementation (Phase 1: T-RC-1-01)
// Serialization (to_json/from_json) is implemented in T-RC-1-02.
#include "switchrecomp/ir/ir.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace sr::ir {

// ---------------------------------------------------------------------------
// Lane layouts
// ---------------------------------------------------------------------------

LaneInfo GetLaneInfo(LaneKind kind) {
    switch (kind) {
        case LaneKind::F32x4:
            return {4, 32, TypeId::F32};
        case LaneKind::None:
            break;
    }
    return {0, 0, TypeId::Void};
}

// ---------------------------------------------------------------------------
// IRBuilder
// ---------------------------------------------------------------------------

Constant* IRBuilder::CreateConstant(u64 value, Type type) {
    auto c = std::make_unique<Constant>(value, type);
    c->set_id(next_id_++);
    Constant* raw = c.get();
    owned_.push_back(std::move(c));
    return raw;
}

Constant* IRBuilder::CreateConstant(u64 value_lo, u64 value_hi, Type type) {
    auto c = std::make_unique<Constant>(value_lo, value_hi, type);
    c->set_id(next_id_++);
    Constant* raw = c.get();
    owned_.push_back(std::move(c));
    return raw;
}

Instruction* IRBuilder::CreateInstruction(Opcode op, Type type,
                                          std::vector<Value*> operands) {
    auto i = std::make_unique<Instruction>();
    i->set_id(next_id_++);
    i->set_opcode(op);
    i->set_type(type);
    i->set_operands(std::move(operands));
    Instruction* raw = i.get();
    // Instructions are stored in builder-owned vector; blocks hold raw ptrs.
    owned_.push_back(std::move(i));
    return raw;
}

// ---- Arithmetic (all binary, result type = a's type) ----

Instruction* IRBuilder::CreateAdd(Value* a, Value* b) {
    return CreateInstruction(Opcode::Add, a->type(), {a, b});
}
Instruction* IRBuilder::CreateSub(Value* a, Value* b) {
    return CreateInstruction(Opcode::Sub, a->type(), {a, b});
}
Instruction* IRBuilder::CreateAnd(Value* a, Value* b) {
    return CreateInstruction(Opcode::And, a->type(), {a, b});
}
Instruction* IRBuilder::CreateOr(Value* a, Value* b) {
    return CreateInstruction(Opcode::Or, a->type(), {a, b});
}
Instruction* IRBuilder::CreateXor(Value* a, Value* b) {
    return CreateInstruction(Opcode::Xor, a->type(), {a, b});
}
Instruction* IRBuilder::CreateNot(Value* a) {
    return CreateInstruction(Opcode::Not, a->type(), {a});
}
Instruction* IRBuilder::CreateShl(Value* a, Value* b) {
    return CreateInstruction(Opcode::Shl, a->type(), {a, b});
}
Instruction* IRBuilder::CreateShr(Value* a, Value* b) {
    return CreateInstruction(Opcode::Shr, a->type(), {a, b});
}
Instruction* IRBuilder::CreateSar(Value* a, Value* b) {
    return CreateInstruction(Opcode::Sar, a->type(), {a, b});
}
Instruction* IRBuilder::CreateMul(Value* a, Value* b) {
    return CreateInstruction(Opcode::Mul, a->type(), {a, b});
}

// ---- Memory ----

Instruction* IRBuilder::CreateLoad(Value* addr, Type result_type) {
    return CreateInstruction(Opcode::Load, result_type, {addr});
}
Instruction* IRBuilder::CreateStore(Value* addr, Value* value) {
    return CreateInstruction(Opcode::Store, Type::Void(), {addr, value});
}

// ---- Control flow ----

Instruction* IRBuilder::CreateBranch(BasicBlock* /*target*/) {
    return CreateInstruction(Opcode::Branch, Type::Void(), {});
}
Instruction* IRBuilder::CreateCondBranch(Value* cond, BasicBlock* /*if_true*/,
                                         BasicBlock* /*if_false*/) {
    return CreateInstruction(Opcode::CondBranch, Type::Void(), {cond});
}
Instruction* IRBuilder::CreateReturn() {
    return CreateInstruction(Opcode::Return, Type::Void(), {});
}
Instruction* IRBuilder::CreateCall(u64 target) {
    return CreateInstruction(Opcode::Call, Type::Void(), {CreateConstant(target, Type::U32())});
}
Instruction* IRBuilder::CreateIndirectBranch(Value* target) {
    return CreateInstruction(Opcode::IndirectBranch, Type::Void(), {target});
}

// ---- Conversion / special ----

Instruction* IRBuilder::CreateCast(Value* value, Type to_type) {
    return CreateInstruction(Opcode::Cast, to_type, {value});
}
Instruction* IRBuilder::CreateSvc(u32 svc_id) {
    return CreateInstruction(Opcode::Svc, Type::Void(), {CreateConstant(svc_id, Type::U32())});
}
Instruction* IRBuilder::CreateMov(Value* dst, Value* src) {
    return CreateInstruction(Opcode::Mov, dst->type(), {dst, src});
}

// ---- RC-FP-004: scalar FP negate / abs / conditional select ----

Instruction* IRBuilder::CreateNeg(Value* dst, Value* src) {
    return CreateInstruction(Opcode::Neg, dst->type(), {dst, src});
}
Instruction* IRBuilder::CreateAbs(Value* dst, Value* src) {
    return CreateInstruction(Opcode::Abs, dst->type(), {dst, src});
}
Instruction* IRBuilder::CreateSelect(Value* dst, Value* cond,
                                     Value* if_true, Value* if_false) {
    return CreateInstruction(Opcode::Select, dst->type(),
                             {dst, cond, if_true, if_false});
}

// ---- RC-FP-005: scalar FP sqrt / round-to-integer ----

Instruction* IRBuilder::CreateSqrt(Value* dst, Value* src) {
    return CreateInstruction(Opcode::Sqrt, dst->type(), {dst, src});
}
Instruction* IRBuilder::CreateFrintN(Value* dst, Value* src) {
    return CreateInstruction(Opcode::FrintN, dst->type(), {dst, src});
}
Instruction* IRBuilder::CreateFrintP(Value* dst, Value* src) {
    return CreateInstruction(Opcode::FrintP, dst->type(), {dst, src});
}
Instruction* IRBuilder::CreateFrintM(Value* dst, Value* src) {
    return CreateInstruction(Opcode::FrintM, dst->type(), {dst, src});
}
Instruction* IRBuilder::CreateFrintZ(Value* dst, Value* src) {
    return CreateInstruction(Opcode::FrintZ, dst->type(), {dst, src});
}
Instruction* IRBuilder::CreateFrintA(Value* dst, Value* src) {
    return CreateInstruction(Opcode::FrintA, dst->type(), {dst, src});
}

// ---- SIMD stage 2: broadcast / insert lane / extract lane ----

Instruction* IRBuilder::CreateBroadcast(Value* dst, Value* src) {
    return CreateInstruction(Opcode::Broadcast, dst->type(), {dst, src});
}
Instruction* IRBuilder::CreateInsertLane(Value* dst, Value* lane, Value* src) {
    return CreateInstruction(Opcode::InsertLane, dst->type(),
                             {dst, lane, src});
}
Instruction* IRBuilder::CreateExtractLane(Value* dst, Value* src,
                                          Value* lane) {
    return CreateInstruction(Opcode::ExtractLane, Type::U32(),
                             {dst, src, lane});
}

// ---------------------------------------------------------------------------
// Module::verify — structural checks
// ---------------------------------------------------------------------------

bool Module::verify() const {
    for (const Function* f : funcs_) {
        if (!f) return false;
        const auto& blocks = f->blocks();
        if (blocks.empty()) continue; // empty function is allowed

        std::unordered_set<u64> seen_ids;

        for (const BasicBlock* bb : blocks) {
            if (!bb) return false;
            for (const Instruction* i : bb->instructions()) {
                if (!i) return false;
                u64 id = i->id();
                if (id == 0 || seen_ids.count(id)) return false;
                seen_ids.insert(id);

                for (Value* op : i->operands()) {
                    if (!op) return false;
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Minimal JSON helpers (no third-party dependency)
// ---------------------------------------------------------------------------

namespace {

// JSON value type for parsing only (to_json builds strings directly).
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double num = 0.0;
    u64 uval = 0;
    bool is_int = false;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue* find(const std::string& key) const {
        for (const auto& kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    const JsonValue& at(const std::string& key) const {
        const JsonValue* v = find(key);
        if (!v) throw std::runtime_error("json key not found: " + key);
        return *v;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s), pos_(0) {}

    JsonValue Parse() {
        SkipWs();
        JsonValue v = ParseValue();
        SkipWs();
        return v;
    }

private:
    const std::string& s_;
    size_t pos_;

    void SkipWs() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_])))
            ++pos_;
    }

    char Peek() {
        if (pos_ >= s_.size()) throw std::runtime_error("unexpected end of json");
        return s_[pos_];
    }
    char Next() {
        if (pos_ >= s_.size()) throw std::runtime_error("unexpected end of json");
        return s_[pos_++];
    }

    JsonValue ParseValue() {
        SkipWs();
        char c = Peek();
        if (c == '{') return ParseObject();
        if (c == '[') return ParseArray();
        if (c == '"') return ParseString();
        if (c == 't' || c == 'f') return ParseBool();
        if (c == 'n') return ParseNull();
        return ParseNumber();
    }

    JsonValue ParseObject() {
        JsonValue v;
        v.type = JsonValue::Type::Object;
        Next(); // '{'
        SkipWs();
        if (Peek() == '}') { Next(); return v; }
        while (true) {
            SkipWs();
            JsonValue key = ParseString();
            SkipWs();
            if (Next() != ':') throw std::runtime_error("expected ':' in json object");
            JsonValue val = ParseValue();
            v.obj.emplace_back(key.str, std::move(val));
            SkipWs();
            char c = Next();
            if (c == '}') break;
            if (c != ',') throw std::runtime_error("expected ',' or '}' in json object");
        }
        return v;
    }

    JsonValue ParseArray() {
        JsonValue v;
        v.type = JsonValue::Type::Array;
        Next(); // '['
        SkipWs();
        if (Peek() == ']') { Next(); return v; }
        while (true) {
            v.arr.push_back(ParseValue());
            SkipWs();
            char c = Next();
            if (c == ']') break;
            if (c != ',') throw std::runtime_error("expected ',' or ']' in json array");
        }
        return v;
    }

    JsonValue ParseString() {
        JsonValue v;
        v.type = JsonValue::Type::String;
        if (Next() != '"') throw std::runtime_error("expected '\"'");
        std::string out;
        while (true) {
            char c = Next();
            if (c == '"') break;
            if (c == '\\') {
                char esc = Next();
                switch (esc) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    default: out += esc; break;
                }
            } else {
                out += c;
            }
        }
        v.str = std::move(out);
        return v;
    }

    JsonValue ParseBool() {
        JsonValue v;
        v.type = JsonValue::Type::Bool;
        if (s_.compare(pos_, 4, "true") == 0) {
            v.b = true;
            pos_ += 4;
        } else if (s_.compare(pos_, 5, "false") == 0) {
            v.b = false;
            pos_ += 5;
        } else {
            throw std::runtime_error("invalid bool literal");
        }
        return v;
    }

    JsonValue ParseNull() {
        JsonValue v;
        v.type = JsonValue::Type::Null;
        if (s_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
        } else {
            throw std::runtime_error("invalid null literal");
        }
        return v;
    }

    JsonValue ParseNumber() {
        JsonValue v;
        v.type = JsonValue::Type::Number;
        size_t start = pos_;
        bool is_int = true;
        if (Peek() == '-') ++pos_;
        while (pos_ < s_.size() &&
               (std::isdigit(static_cast<unsigned char>(s_[pos_])) ||
                s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E' ||
                s_[pos_] == '+' || s_[pos_] == '-')) {
            if (s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E')
                is_int = false;
            ++pos_;
        }
        std::string num_str = s_.substr(start, pos_ - start);
        if (is_int) {
            v.uval = std::stoull(num_str);
            v.is_int = true;
        } else {
            v.num = std::stod(num_str);
        }
        return v;
    }
};

// ---- Type / Opcode string mapping ----

const char* TypeToString(TypeId id) {
    switch (id) {
        case TypeId::Void: return "Void";
        case TypeId::U8: return "U8";
        case TypeId::U16: return "U16";
        case TypeId::U32: return "U32";
        case TypeId::U64: return "U64";
        case TypeId::S8: return "S8";
        case TypeId::S16: return "S16";
        case TypeId::S32: return "S32";
        case TypeId::S64: return "S64";
        case TypeId::F32: return "F32";
        case TypeId::F64: return "F64";
        case TypeId::Ptr: return "Ptr";
        case TypeId::V128: return "V128";
    }
    return "Unknown";
}

// Defined below; needed by the full-type helpers that precede it.
TypeId StringToType(const std::string& s);

const char* LaneToString(LaneKind k) {
    switch (k) {
        case LaneKind::None: return "";
        case LaneKind::F32x4: return "F32x4";
    }
    return "Unknown";
}

LaneKind StringToLane(const std::string& s) {
    if (s == "F32x4") return LaneKind::F32x4;
    throw std::runtime_error("unknown lane kind: " + s);
}

// Full type serialization, e.g. "F32" or "V128:F32x4".
std::string TypeToStringFull(Type t) {
    std::string out = TypeToString(t.id);
    if (t.id == TypeId::V128 && t.lane != LaneKind::None) {
        out += ":";
        out += LaneToString(t.lane);
    }
    return out;
}

// Parse a full type string ("F32", "V128:F32x4").
Type StringToTypeFull(const std::string& s) {
    size_t colon = s.find(':');
    if (colon == std::string::npos)
        return {StringToType(s)};
    Type t{StringToType(s.substr(0, colon))};
    t.lane = StringToLane(s.substr(colon + 1));
    return t;
}

TypeId StringToType(const std::string& s) {
    if (s == "Void") return TypeId::Void;
    if (s == "U8") return TypeId::U8;
    if (s == "U16") return TypeId::U16;
    if (s == "U32") return TypeId::U32;
    if (s == "U64") return TypeId::U64;
    if (s == "S8") return TypeId::S8;
    if (s == "S16") return TypeId::S16;
    if (s == "S32") return TypeId::S32;
    if (s == "S64") return TypeId::S64;
    if (s == "F32") return TypeId::F32;
    if (s == "F64") return TypeId::F64;
    if (s == "Ptr") return TypeId::Ptr;
    if (s == "V128") return TypeId::V128;
    throw std::runtime_error("unknown type: " + s);
}

const char* OpcodeToString(Opcode op) {
    switch (op) {
        case Opcode::Nop: return "Nop";
        case Opcode::Add: return "Add";
        case Opcode::Sub: return "Sub";
        case Opcode::And: return "And";
        case Opcode::Or: return "Or";
        case Opcode::Xor: return "Xor";
        case Opcode::Not: return "Not";
        case Opcode::Shl: return "Shl";
        case Opcode::Shr: return "Shr";
        case Opcode::Sar: return "Sar";
        case Opcode::Mul: return "Mul";
        case Opcode::Div: return "Div";
        case Opcode::Load: return "Load";
        case Opcode::Store: return "Store";
        case Opcode::Branch: return "Branch";
        case Opcode::CondBranch: return "CondBranch";
        case Opcode::Return: return "Return";
        case Opcode::Call: return "Call";
        case Opcode::IndirectBranch: return "IndirectBranch";
        case Opcode::Cast: return "Cast";
        case Opcode::Svc: return "Svc";
        case Opcode::Undefined: return "Undefined";
        case Opcode::Mov: return "Mov";
        case Opcode::Compare: return "Compare";
        case Opcode::Convert: return "Convert";
        case Opcode::Neg: return "Neg";
        case Opcode::Abs: return "Abs";
        case Opcode::Select: return "Select";
        case Opcode::Sqrt: return "Sqrt";
        case Opcode::FrintN: return "FrintN";
        case Opcode::FrintP: return "FrintP";
        case Opcode::FrintM: return "FrintM";
        case Opcode::FrintZ: return "FrintZ";
        case Opcode::FrintA: return "FrintA";
        case Opcode::Broadcast: return "Broadcast";
        case Opcode::InsertLane: return "InsertLane";
        case Opcode::ExtractLane: return "ExtractLane";
    }
    return "Unknown";
}

Opcode StringToOpcode(const std::string& s) {
    if (s == "Nop") return Opcode::Nop;
    if (s == "Add") return Opcode::Add;
    if (s == "Sub") return Opcode::Sub;
    if (s == "And") return Opcode::And;
    if (s == "Or") return Opcode::Or;
    if (s == "Xor") return Opcode::Xor;
    if (s == "Not") return Opcode::Not;
    if (s == "Shl") return Opcode::Shl;
    if (s == "Shr") return Opcode::Shr;
    if (s == "Sar") return Opcode::Sar;
    if (s == "Mul") return Opcode::Mul;
    if (s == "Div") return Opcode::Div;
    if (s == "Load") return Opcode::Load;
    if (s == "Store") return Opcode::Store;
    if (s == "Branch") return Opcode::Branch;
    if (s == "CondBranch") return Opcode::CondBranch;
    if (s == "Return") return Opcode::Return;
    if (s == "Call") return Opcode::Call;
    if (s == "IndirectBranch") return Opcode::IndirectBranch;
    if (s == "Cast") return Opcode::Cast;
    if (s == "Svc") return Opcode::Svc;
    if (s == "Undefined") return Opcode::Undefined;
    if (s == "Mov") return Opcode::Mov;
    if (s == "Compare") return Opcode::Compare;
    if (s == "Convert") return Opcode::Convert;
    if (s == "Neg") return Opcode::Neg;
    if (s == "Abs") return Opcode::Abs;
    if (s == "Select") return Opcode::Select;
    if (s == "Sqrt") return Opcode::Sqrt;
    if (s == "FrintN") return Opcode::FrintN;
    if (s == "FrintP") return Opcode::FrintP;
    if (s == "FrintM") return Opcode::FrintM;
    if (s == "FrintZ") return Opcode::FrintZ;
    if (s == "FrintA") return Opcode::FrintA;
    if (s == "Broadcast") return Opcode::Broadcast;
    if (s == "InsertLane") return Opcode::InsertLane;
    if (s == "ExtractLane") return Opcode::ExtractLane;
    throw std::runtime_error("unknown opcode: " + s);
}

[[maybe_unused]] std::string EscapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::string Module::to_json() const {
    std::ostringstream os;
    os << "{\n  \"functions\": [";
    bool first_fn = true;
    for (const Function* fn : funcs_) {
        if (!first_fn) os << ",";
        first_fn = false;
        os << "\n    {\n";
        os << "      \"guest_address\": " << fn->guest_address() << ",\n";
        os << "      \"blocks\": [";
        bool first_bb = true;
        for (const BasicBlock* bb : fn->blocks()) {
            if (!first_bb) os << ",";
            first_bb = false;
            os << "\n        {\n          \"instructions\": [";
            bool first_i = true;
            for (const Instruction* i : bb->instructions()) {
                if (!first_i) os << ",";
                first_i = false;
                os << "\n            {";
                os << "\"id\": " << i->id() << ", ";
                os << "\"opcode\": \"" << OpcodeToString(i->opcode()) << "\", ";
                os << "\"type\": \"" << TypeToStringFull(i->type()) << "\", ";
                os << "\"operands\": [";
                bool first_op = true;
                for (Value* op : i->operands()) {
                    if (!first_op) os << ", ";
                    first_op = false;
                    os << "{\"id\": " << op->id() << ", ";
                    if (auto* c = dynamic_cast<Constant*>(op)) {
                        os << "\"kind\": \"constant\", ";
                        os << "\"type\": \"" << TypeToStringFull(c->type()) << "\", ";
                        if (c->type().id == TypeId::V128) {
                            // 128-bit payload as hexadecimal strings.
                            auto hex = [](u64 v) {
                                char buf[19];
                                std::snprintf(buf, sizeof(buf), "0x%016llx",
                                              static_cast<unsigned long long>(v));
                                return std::string(buf);
                            };
                            os << "\"value_lo\": \"" << hex(c->value()) << "\", ";
                            os << "\"value_hi\": \"" << hex(c->value_hi()) << "\"";
                        } else {
                            os << "\"value\": " << c->value();
                        }
                    } else {
                        os << "\"kind\": \"instruction\"";
                    }
                    os << "}";
                }
                os << "]}";
            }
            os << "\n          ]\n        }";
        }
        os << "\n      ]\n    }";
    }
    os << "\n  ]\n}";
    return os.str();
}

std::unique_ptr<Module> Module::from_json(const std::string& json) {
    JsonParser parser(json);
    JsonValue root = parser.Parse();

    auto module = std::make_unique<Module>();

    // Per-function storage: keeps constants alive for the module's lifetime
    // (instructions are owned by BasicBlocks). value_map resolves operand
    // references by id.
    struct FunctionData {
        std::vector<std::unique_ptr<Constant>> constants;
        std::unordered_map<u64, Value*> value_map;
    };
    // Module-level owner for all FunctionData. Kept alive via a static map
    // keyed by Module* (Phase 1 research code; proper Module-owned storage
    // will replace this in a later refactor).
    static std::unordered_map<Module*,
                              std::vector<std::unique_ptr<FunctionData>>>
        s_owned;
    std::vector<std::unique_ptr<FunctionData>> fn_datas;

    const JsonValue* funcs = root.find("functions");
    if (!funcs) return module; // empty module

    for (const JsonValue& fn_j : funcs->arr) {
        Function* fn = module->AddFunction();
        fn->set_guest_address(
            fn_j.at("guest_address").uval);

        auto fd = std::make_unique<FunctionData>();
        const JsonValue& blocks_j = fn_j.at("blocks");

        // Pass 1: create all instructions + constants, register ids.
        for (const JsonValue& bb_j : blocks_j.arr) {
            BasicBlock* bb = fn->AddBlock();
            const JsonValue& instrs_j = bb_j.at("instructions");
            for (const JsonValue& i_j : instrs_j.arr) {
                u64 id = i_j.at("id").uval;
                Opcode op = StringToOpcode(i_j.at("opcode").str);
                Type type = StringToTypeFull(i_j.at("type").str);

                auto instr = std::make_unique<Instruction>();
                instr->set_id(id);
                instr->set_opcode(op);
                instr->set_type(type);
                fd->value_map[id] = instr.get();

                // Create constants referenced by operands (once per id).
                const JsonValue& ops_j = i_j.at("operands");
                for (const JsonValue& op_j : ops_j.arr) {
                    if (op_j.at("kind").str == "constant") {
                        u64 op_id = op_j.at("id").uval;
                        if (fd->value_map.count(op_id)) continue;
                        Type ctype = StringToTypeFull(op_j.at("type").str);
                        std::unique_ptr<Constant> c;
                        if (ctype.id == TypeId::V128) {
                            auto parse_hex = [](const std::string& s) {
                                return static_cast<u64>(
                                    std::strtoull(s.c_str(), nullptr, 0));
                            };
                            u64 lo = parse_hex(op_j.at("value_lo").str);
                            u64 hi = parse_hex(op_j.at("value_hi").str);
                            c = std::make_unique<Constant>(lo, hi, ctype);
                        } else {
                            u64 cval = op_j.at("value").uval;
                            c = std::make_unique<Constant>(cval, ctype);
                        }
                        c->set_id(op_id);
                        fd->value_map[op_id] = c.get();
                        fd->constants.push_back(std::move(c));
                    }
                }
                bb->AddOwned(std::move(instr));
            }
        }

        // Pass 2: wire operands by id.
        size_t bb_idx = 0;
        for (const JsonValue& bb_j : blocks_j.arr) {
            BasicBlock* bb = fn->blocks()[bb_idx++];
            const JsonValue& instrs_j = bb_j.at("instructions");
            size_t i_idx = 0;
            for (const JsonValue& i_j : instrs_j.arr) {
                Instruction* i = bb->instructions()[i_idx++];
                const JsonValue& ops_j = i_j.at("operands");
                std::vector<Value*> operands;
                operands.reserve(ops_j.arr.size());
                for (const JsonValue& op_j : ops_j.arr) {
                    u64 op_id = op_j.at("id").uval;
                    auto it = fd->value_map.find(op_id);
                    if (it == fd->value_map.end())
                        throw std::runtime_error("unresolved operand id: " +
                                                 std::to_string(op_id));
                    operands.push_back(it->second);
                }
                i->set_operands(std::move(operands));
            }
        }

        fn_datas.push_back(std::move(fd));
    }

    s_owned[module.get()] = std::move(fn_datas);
    return module;
}

} // namespace sr::ir

