#include "UdonLuau/Program.hpp"

#include <format>
#include <unordered_map>

namespace UdonLuau {

    std::vector<uint8_t> Program::ByteCode() const {
        std::vector<uint8_t> bytes;
        bytes.reserve(code.size() * 4);
        for (uint32_t word : code) {
            bytes.push_back(static_cast<uint8_t>(word >> 24));
            bytes.push_back(static_cast<uint8_t>(word >> 16));
            bytes.push_back(static_cast<uint8_t>(word >> 8));
            bytes.push_back(static_cast<uint8_t>(word));
        }
        return bytes;
    }

    namespace {

        std::string_view OpName(OpCode op) {
            switch (op) {
                case OpCode::Nop: return "NOP";
                case OpCode::Push: return "PUSH";
                case OpCode::Pop: return "POP";
                case OpCode::JumpIfFalse: return "JUMP_IF_FALSE";
                case OpCode::Jump: return "JUMP";
                case OpCode::Extern: return "EXTERN";
                case OpCode::Annotation: return "ANNOTATION";
                case OpCode::JumpIndirect: return "JUMP_INDIRECT";
                case OpCode::Copy: return "COPY";
            }
            return "?";
        }

        std::string ValueText(const HeapValue& v) {
            switch (v.kind) {
                case ValueKind::Default: return "default";
                case ValueKind::Null: return "null";
                case ValueKind::Boolean: return v.boolean ? "true" : "false";
                case ValueKind::Integer: return std::to_string(v.integer);
                case ValueKind::Unsigned: return std::format("0x{:X}", v.unsignedInteger);
                case ValueKind::Real: return std::format("{}", v.real);
                case ValueKind::String: return std::format("\"{}\"", v.text);
                case ValueKind::This: return "this";
                case ValueKind::Type: return std::format("typeof({})", v.text);
            }
            return "?";
        }

    } // namespace

    std::string Disassemble(const Program& program) {
        std::string out = ".data\n";
        for (size_t i = 0; i < program.heap.size(); ++i) {
            const HeapSlot& slot = program.heap[i];
            out += std::format("  {:#06x} {}{}: {} = {}\n", i, slot.exported ? "export " : "", slot.symbol, slot.type, ValueText(slot.value));
        }

        std::unordered_map<uint32_t, const EntryPoint*> entries;
        for (const EntryPoint& e : program.entryPoints) entries[e.address] = &e;

        out += ".code\n";
        for (size_t pc = 0; pc < program.code.size();) {
            uint32_t address = static_cast<uint32_t>(pc * 4);
            if (auto it = entries.find(address); it != entries.end()) out += std::format("{}:\n", it->second->name);

            auto op = static_cast<OpCode>(program.code[pc]);
            out += std::format("  {:#06x} {}", address, OpName(op));
            if (OperandCount(op) == 1 && pc + 1 < program.code.size()) {
                uint32_t arg = program.code[pc + 1];
                bool heapOperand = op == OpCode::Push || op == OpCode::Extern || op == OpCode::JumpIndirect;
                if (heapOperand && arg < program.heap.size()) {
                    const HeapSlot& slot = program.heap[arg];
                    out += std::format(" {}", slot.symbol);
                    if (op == OpCode::Extern && slot.value.kind == ValueKind::String) out += std::format(" ; {}", slot.value.text);
                } else {
                    out += std::format(" {:#06x}", arg);
                }
                pc += 2;
            } else {
                pc += 1;
            }
            out += '\n';
        }
        return out;
    }

} // namespace UdonLuau
