#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace UdonLuau {

    enum class OpCode : uint32_t {
        Nop          = 0,
        Push         = 1,
        Pop          = 2,
        JumpIfFalse  = 4,
        Jump         = 5,
        Extern       = 6,
        Annotation   = 7,
        JumpIndirect = 8,
        Copy         = 9,
    };

    enum class ValueKind : uint8_t {
        Default,
        Null,
        Boolean,
        Integer,
        Unsigned,
        Real,
        String,
        This,
        Type,
        Construct,
        Array,
    };

    /// <summary>The initial value of a heap slot. Integer also carries enum values; This means
    /// the behaviour, its GameObject or its Transform, chosen by the slot type; Type names a
    /// System.Type by its Udon name in text; Construct is the result of calling the constructor
    /// extern named in text with the constant arguments; Array is an array of the element type
    /// named in text holding the arguments.</summary>
    struct HeapValue {
        ValueKind              kind = ValueKind::Default;
        bool                   boolean = false;
        int64_t                integer = 0;
        uint64_t               unsignedInteger = 0;
        double                 real = 0.0;
        std::string            text;
        std::vector<HeapValue> arguments;
    };

    /// <summary>An annotation written above a variable, such as <c>-- @range(0, 10)</c>. String
    /// arguments are unquoted.</summary>
    struct FieldAttribute {
        std::string              name;
        std::vector<std::string> arguments;
    };

    struct HeapSlot {
        std::string                 symbol;
        std::string                 type;
        HeapValue                   value;
        bool                        exported = false;
        std::vector<FieldAttribute> attributes;
    };

    struct EntryPoint {
        std::string name;
        uint32_t    address = 0;
    };

    enum class SyncInterpolation : uint8_t {
        None,
        Linear,
        Smooth,
    };

    /// <summary>The behaviour sync mode a script requires, matching UdonSharp's BehaviourSyncMode.</summary>
    enum class BehaviourSyncMode : uint8_t {
        Any,
        None,
        NoVariableSync,
        Continuous,
        Manual,
    };

    struct SyncVariable {
        std::string       symbol;
        SyncInterpolation interpolation = SyncInterpolation::None;
    };

    struct NetworkParameter {
        std::string symbol;
        std::string type;
    };

    /// <summary>An entry point VRChat may run when another client calls it over the network, with
    /// the heap symbols its arguments are written to. maxEventsPerSecond is 0 for the SDK default.</summary>
    struct NetworkCallable {
        std::string                   entryPoint;
        int                           maxEventsPerSecond = 0;
        std::vector<NetworkParameter> parameters;
    };

    /// <summary>A compiled Udon program: code words, heap layout with initial values, exported
    /// entry points, sync metadata and the annotations written at the top of the file. Heap
    /// addresses are indices into heap.</summary>
    struct Program {
        std::vector<uint32_t>     code;
        std::vector<HeapSlot>     heap;
        std::vector<EntryPoint>   entryPoints;
        std::vector<SyncVariable> sync;
        BehaviourSyncMode         syncMode = BehaviourSyncMode::Any;
        std::vector<NetworkCallable> networkCallables;
        std::vector<FieldAttribute> attributes;
        int                       updateOrder = 0;

        /// <summary>The code in Udon's serialized big-endian byte order.</summary>
        [[nodiscard]] std::vector<uint8_t> ByteCode() const;
    };

    /// <summary>A readable listing of the program, one instruction per line.</summary>
    std::string Disassemble(const Program& program);

    /// <summary>Operand words the opcode is followed by.</summary>
    constexpr int OperandCount(OpCode op) {
        switch (op) {
            case OpCode::Push:
            case OpCode::JumpIfFalse:
            case OpCode::Jump:
            case OpCode::Extern:
            case OpCode::Annotation:
            case OpCode::JumpIndirect:
                return 1;
            default:
                return 0;
        }
    }

} // namespace UdonLuau
