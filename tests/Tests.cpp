#include "UdonLuau/Compiler.hpp"
#include "UdonLuau/Definitions.hpp"

#include "Luau/Parser.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

using namespace UdonLuau;

namespace {

    struct Vec3 {
        float x = 0, y = 0, z = 0;
    };

    struct BehaviourRef {
        int id = 0;
    };

    using IntArray = std::shared_ptr<std::vector<int32_t>>;
    using RefArray = std::shared_ptr<std::vector<BehaviourRef>>;
    using Cell = std::variant<std::monostate, bool, int32_t, uint32_t, int64_t, float, double, std::string, Vec3, IntArray, std::vector<uint32_t>, BehaviourRef, RefArray>;
    using Impl = std::function<void(std::vector<Cell>&, const std::vector<uint32_t>&)>;

    void RunOn(const Cell& behaviour, const std::string& entry);
    void NetworkRunOn(const Cell& behaviour, const std::string& entry, const std::vector<Cell>& arguments);
    Cell& VarOn(const Cell& behaviour, const std::string& symbol);

    struct Fixture {
        Catalog                                catalog;
        std::unordered_map<std::string, Impl>  impls;
        std::vector<std::string>               log;

        void Type(std::string fullName, TypeKind kind, std::string base = {}, std::vector<std::string> interfaces = {}, std::vector<EnumMember> members = {}) {
            TypeInfo t;
            t.fullName = std::move(fullName);
            t.kind = kind;
            t.baseType = std::move(base);
            t.interfaces = std::move(interfaces);
            t.enumMembers = std::move(members);
            catalog.AddType(std::move(t));
        }

        void Extern(const std::string& signature, bool instance, Impl impl = {}) {
            ExternInfo info;
            ParseExternSignature(signature, info);
            int count = static_cast<int>(info.parameters.size()) + (info.returnType == "SystemVoid" ? 0 : 1) + (info.hasTypeOperand ? 1 : 0) + (instance ? 1 : 0);
            if (!catalog.AddExtern(signature, count)) std::printf("bad extern %s\n", signature.c_str());
            if (impl) impls[signature] = std::move(impl);
        }
    };

    template <typename T>
    T& As(std::vector<Cell>& h, uint32_t a) {
        return std::get<T>(h[a]);
    }

    template <typename T, typename F>
    void Binary(Fixture& f, const std::string& module, const std::string& op, const std::string& t, const std::string& ret, F fn) {
        f.Extern(std::format("{}.__{}__{}_{}__{}", module, op, t, t, ret), false, [fn](std::vector<Cell>& h, const std::vector<uint32_t>& p) {
            h[p[2]] = fn(As<T>(h, p[0]), As<T>(h, p[1]));
        });
    }

    template <typename T>
    void Numeric(Fixture& f, const std::string& name) {
        Binary<T>(f, name, "op_Addition", name, name, [](T a, T b) { return a + b; });
        Binary<T>(f, name, "op_Subtraction", name, name, [](T a, T b) { return a - b; });
        Binary<T>(f, name, "op_Multiplication", name, name, [](T a, T b) { return a * b; });
        Binary<T>(f, name, "op_Division", name, name, [](T a, T b) { return a / b; });
        Binary<T>(f, name, "op_LessThan", name, "SystemBoolean", [](T a, T b) { return a < b; });
        Binary<T>(f, name, "op_LessThanOrEqual", name, "SystemBoolean", [](T a, T b) { return a <= b; });
        Binary<T>(f, name, "op_GreaterThan", name, "SystemBoolean", [](T a, T b) { return a > b; });
        Binary<T>(f, name, "op_GreaterThanOrEqual", name, "SystemBoolean", [](T a, T b) { return a >= b; });
        Binary<T>(f, name, "op_Equality", name, "SystemBoolean", [](T a, T b) { return a == b; });
        Binary<T>(f, name, "op_Inequality", name, "SystemBoolean", [](T a, T b) { return a != b; });
        f.Extern(std::format("{}.__op_UnaryMinus__{}__{}", name, name, name), false, [](std::vector<Cell>& h, const std::vector<uint32_t>& p) { h[p[1]] = static_cast<T>(-As<T>(h, p[0])); });
        f.Extern(std::format("{}.__ToString__SystemString", name), true, [](std::vector<Cell>& h, const std::vector<uint32_t>& p) {
            h[p[1]] = std::format("{}", As<T>(h, p[0]));
        });
    }

    std::string Text(const Cell& c) {
        if (auto* s = std::get_if<std::string>(&c)) return *s;
        if (auto* i = std::get_if<int32_t>(&c)) return std::to_string(*i);
        if (auto* f = std::get_if<float>(&c)) return std::format("{}", *f);
        if (auto* b = std::get_if<bool>(&c)) return *b ? "True" : "False";
        return "?";
    }

    Fixture MakeFixture() {
        Fixture f;
        f.catalog.AddStandardEvents();
        for (auto [type, linear, smooth] : std::initializer_list<std::tuple<const char*, bool, bool>>{
                 { "SystemInt32", true, true }, { "SystemSingle", true, true }, { "SystemBoolean", false, false },
                 { "SystemString", false, false }, { "SystemInt32Array", false, false }, { "UnityEngineVector3", true, true } })
            f.catalog.AddSyncableType(type, linear, smooth);
        f.Type("UnityEngine.Object", TypeKind::Class);
        f.Type("UnityEngine.GameObject", TypeKind::Class, "UnityEngineObject");
        f.Type("UnityEngine.Component", TypeKind::Class, "UnityEngineObject");
        f.Type("UnityEngine.Transform", TypeKind::Class, "UnityEngineComponent");
        f.Type("UnityEngine.Behaviour", TypeKind::Class, "UnityEngineComponent");
        f.Type("UnityEngine.MonoBehaviour", TypeKind::Class, "UnityEngineBehaviour");
        f.Type("VRC.Udon.Common.Interfaces.IUdonEventReceiver", TypeKind::Interface);
        f.Type("VRC.Udon.UdonBehaviour", TypeKind::Class, "UnityEngineMonoBehaviour", { "VRCUdonCommonInterfacesIUdonEventReceiver" });
        f.Type("VRC.SDKBase.VRCPlayerApi", TypeKind::Class);
        f.Type("UnityEngine.Vector3", TypeKind::Struct);
        f.Type("UnityEngine.Debug", TypeKind::Class);
        f.Type("UnityEngine.Mathf", TypeKind::Struct);
        f.Type("System.Math", TypeKind::Class);
        f.Type("System.Convert", TypeKind::Class);
        f.Type("UnityEngine.KeyCode", TypeKind::Enum, {}, {}, { { "Space", 32 }, { "Return", 13 } });

        Numeric<int32_t>(f, "SystemInt32");
        Numeric<float>(f, "SystemSingle");
        f.Extern("SystemInt32.__op_Remainder__SystemInt32_SystemInt32__SystemInt32", false, [](auto& h, auto& p) { h[p[2]] = As<int32_t>(h, p[0]) % As<int32_t>(h, p[1]); });
        f.Extern("SystemBoolean.__op_UnaryNegation__SystemBoolean__SystemBoolean", false, [](auto& h, auto& p) { h[p[1]] = !As<bool>(h, p[0]); });
        f.Extern("SystemBoolean.__op_Equality__SystemBoolean_SystemBoolean__SystemBoolean", false, [](auto& h, auto& p) { h[p[2]] = As<bool>(h, p[0]) == As<bool>(h, p[1]); });
        f.Extern("SystemConvert.__ToSingle__SystemInt32__SystemSingle", false, [](auto& h, auto& p) { h[p[1]] = static_cast<float>(As<int32_t>(h, p[0])); });
        f.Extern("SystemConvert.__ToInt32__SystemSingle__SystemInt32", false, [](auto& h, auto& p) { h[p[1]] = static_cast<int32_t>(std::nearbyint(As<float>(h, p[0]))); });
        f.Extern("SystemString.__Concat__SystemString_SystemString__SystemString", false, [](auto& h, auto& p) { h[p[2]] = As<std::string>(h, p[0]) + As<std::string>(h, p[1]); });
        f.Extern("SystemString.__Concat__SystemString_SystemString_SystemString__SystemString", false, [](auto& h, auto& p) {
            h[p[3]] = As<std::string>(h, p[0]) + As<std::string>(h, p[1]) + As<std::string>(h, p[2]);
        });
        f.Extern("SystemString.__get_Length__SystemInt32", true, [](auto& h, auto& p) { h[p[1]] = static_cast<int32_t>(As<std::string>(h, p[0]).size()); });
        f.Extern("SystemObject.__Equals__SystemObject_SystemObject__SystemBoolean", false, [](auto& h, auto& p) { h[p[2]] = h[p[0]].index() == h[p[1]].index(); });
        f.Extern("SystemObject.__ToString__SystemString", true, [](auto& h, auto& p) { h[p[1]] = Text(h[p[0]]); });
        f.Extern("UnityEngineObject.__op_Equality__UnityEngineObject_UnityEngineObject__SystemBoolean", false);
        f.Extern("UnityEngineObject.__op_Inequality__UnityEngineObject_UnityEngineObject__SystemBoolean", false);
        f.Extern("UnityEngineDebug.__Log__SystemObject__SystemVoid", false, [&log = f.log](auto& h, auto& p) { log.push_back(Text(h[p[0]])); });
        f.Extern("UnityEngineDebug.__LogWarning__SystemObject__SystemVoid", false);
        f.Extern("UnityEngineDebug.__LogError__SystemObject__SystemVoid", false, [&log = f.log](auto& h, auto& p) { log.push_back("error: " + Text(h[p[0]])); });
        f.Extern("SystemUInt32Array.__Get__SystemInt32__SystemUInt32", true, [](auto& h, auto& p) {
            h[p[2]] = As<std::vector<uint32_t>>(h, p[0]).at(static_cast<size_t>(As<int32_t>(h, p[1])));
        });
        f.Extern("UnityEngineMathf.__Floor__SystemSingle__SystemSingle", false, [](auto& h, auto& p) { h[p[1]] = std::floor(As<float>(h, p[0])); });
        f.Extern("UnityEngineMathf.__Pow__SystemSingle_SystemSingle__SystemSingle", false, [](auto& h, auto& p) { h[p[2]] = std::pow(As<float>(h, p[0]), As<float>(h, p[1])); });
        f.Extern("SystemMath.__Pow__SystemDouble_SystemDouble__SystemDouble", false);
        f.Extern("SystemInt32Array.__ctor__SystemInt32__SystemInt32Array", false, [](auto& h, auto& p) { h[p[1]] = std::make_shared<std::vector<int32_t>>(static_cast<size_t>(As<int32_t>(h, p[0]))); });
        f.Extern("SystemInt32Array.__Get__SystemInt32__SystemInt32", true, [](auto& h, auto& p) { h[p[2]] = As<IntArray>(h, p[0])->at(static_cast<size_t>(As<int32_t>(h, p[1]))); });
        f.Extern("SystemInt32Array.__Set__SystemInt32_SystemInt32__SystemVoid", true, [](auto& h, auto& p) {
            As<IntArray>(h, p[0])->at(static_cast<size_t>(As<int32_t>(h, p[1]))) = As<int32_t>(h, p[2]);
        });
        f.Extern("SystemInt32Array.__get_Length__SystemInt32", true, [](auto& h, auto& p) { h[p[1]] = static_cast<int32_t>(As<IntArray>(h, p[0])->size()); });
        f.Extern("UnityEngineVector3.__ctor__SystemSingle_SystemSingle_SystemSingle__UnityEngineVector3", false, [](auto& h, auto& p) {
            h[p[3]] = Vec3{ As<float>(h, p[0]), As<float>(h, p[1]), As<float>(h, p[2]) };
        });
        f.Extern("UnityEngineVector3.__Set__SystemSingle_SystemSingle_SystemSingle__SystemVoid", true, [](auto& h, auto& p) {
            h[p[0]] = Vec3{ As<float>(h, p[1]), As<float>(h, p[2]), As<float>(h, p[3]) };
        });
        f.Extern("UnityEngineVector3.__get_up__UnityEngineVector3", false, [](auto& h, auto& p) { h[p[0]] = Vec3{ 0.0f, 1.0f, 0.0f }; });
        f.Extern("UnityEngineVector3.__get_y__SystemSingle", true, [](auto& h, auto& p) { h[p[1]] = As<Vec3>(h, p[0]).y; });
        f.Extern("UnityEngineVector3.__set_y__SystemSingle", true, [](auto& h, auto& p) { As<Vec3>(h, p[0]).y = As<float>(h, p[1]); });
        f.Extern("UnityEngineVector3.__op_Addition__UnityEngineVector3_UnityEngineVector3__UnityEngineVector3", false, [](auto& h, auto& p) {
            Vec3 a = As<Vec3>(h, p[0]), b = As<Vec3>(h, p[1]);
            h[p[2]] = Vec3{ a.x + b.x, a.y + b.y, a.z + b.z };
        });
        f.Extern("UnityEngineVector3.__op_Multiply__UnityEngineVector3_SystemSingle__UnityEngineVector3", false, [](auto& h, auto& p) {
            Vec3 a = As<Vec3>(h, p[0]);
            float s = As<float>(h, p[1]);
            h[p[2]] = Vec3{ a.x * s, a.y * s, a.z * s };
        });
        f.Extern("SystemArray.__IndexOf__TArray_T__SystemInt32", false, [](auto& h, auto& p) {
            auto& xs = *As<IntArray>(h, p[0]);
            auto it = std::ranges::find(xs, As<int32_t>(h, p[1]));
            h[p[2]] = it == xs.end() ? -1 : static_cast<int32_t>(it - xs.begin());
        });
        f.Type("System.Array", TypeKind::Class);
        f.Extern("SystemArray.__Copy__SystemArray_SystemInt32_SystemArray_SystemInt32_SystemInt32__SystemVoid", false, [](auto& h, auto& p) {
            int32_t s = As<int32_t>(h, p[1]), d = As<int32_t>(h, p[3]), n = As<int32_t>(h, p[4]);
            auto move = [&](auto& from, auto& to) {
                std::vector chunk(from.begin() + s, from.begin() + s + n);
                std::ranges::copy(chunk, to.begin() + d);
            };
            if (std::holds_alternative<RefArray>(h[p[0]])) move(*As<RefArray>(h, p[0]), *As<RefArray>(h, p[2]));
            else move(*As<IntArray>(h, p[0]), *As<IntArray>(h, p[2]));
        });
        f.Extern("SystemArray.__IndexOf__SystemArray_SystemObject__SystemInt32", false, [](auto& h, auto& p) {
            auto& xs = *As<IntArray>(h, p[0]);
            auto it = std::ranges::find(xs, As<int32_t>(h, p[1]));
            h[p[2]] = it == xs.end() ? -1 : static_cast<int32_t>(it - xs.begin());
        });
        f.Extern("SystemArray.__Copy__SystemArray_SystemArray_SystemInt32__SystemVoid", false, [](auto& h, auto& p) {
            if (std::holds_alternative<RefArray>(h[p[0]])) {
                std::copy_n(As<RefArray>(h, p[0])->begin(), As<int32_t>(h, p[2]), As<RefArray>(h, p[1])->begin());
                return;
            }
            std::copy_n(As<IntArray>(h, p[0])->begin(), As<int32_t>(h, p[2]), As<IntArray>(h, p[1])->begin());
        });
        f.Extern("UnityEngineComponentArray.__ctor__SystemInt32__UnityEngineComponentArray", false, [](auto& h, auto& p) {
            h[p[1]] = std::make_shared<std::vector<BehaviourRef>>(static_cast<size_t>(As<int32_t>(h, p[0])), BehaviourRef{ -1 });
        });
        f.Extern("UnityEngineComponentArray.__Get__SystemInt32__UnityEngineComponent", true, [](auto& h, auto& p) { h[p[2]] = As<RefArray>(h, p[0])->at(static_cast<size_t>(As<int32_t>(h, p[1]))); });
        f.Extern("UnityEngineComponentArray.__Set__SystemInt32_UnityEngineComponent__SystemVoid", true, [](auto& h, auto& p) {
            BehaviourRef value = std::holds_alternative<BehaviourRef>(h[p[2]]) ? std::get<BehaviourRef>(h[p[2]]) : BehaviourRef{ -1 };
            As<RefArray>(h, p[0])->at(static_cast<size_t>(As<int32_t>(h, p[1]))) = value;
        });
        f.Extern("UnityEngineComponentArray.__get_Length__SystemInt32", true, [](auto& h, auto& p) { h[p[1]] = static_cast<int32_t>(As<RefArray>(h, p[0])->size()); });
        f.Extern("UnityEngineTime.__get_frameCount__SystemInt32", false, [](auto& h, auto& p) { h[p[0]] = int32_t{ 100 }; });
        f.Extern("SystemArray.__IndexOf__SystemArray_SystemObject_SystemInt32_SystemInt32__SystemInt32", false, [](auto& h, auto& p) {
            auto& xs = *As<IntArray>(h, p[0]);
            int32_t start = As<int32_t>(h, p[2]), n = As<int32_t>(h, p[3]);
            auto end = xs.begin() + start + n;
            auto it = std::find(xs.begin() + start, end, As<int32_t>(h, p[1]));
            h[p[4]] = it == end ? -1 : static_cast<int32_t>(it - xs.begin());
        });
        f.Extern("UnityEngineMathf.__Max__SystemInt32_SystemInt32__SystemInt32", false, [](auto& h, auto& p) { h[p[2]] = std::max(As<int32_t>(h, p[0]), As<int32_t>(h, p[1])); });
        f.Extern("UnityEngineMathf.__Min__SystemInt32_SystemInt32__SystemInt32", false, [](auto& h, auto& p) { h[p[2]] = std::min(As<int32_t>(h, p[0]), As<int32_t>(h, p[1])); });
        f.Extern("SystemString.__Format__SystemString_SystemObject_SystemObject__SystemString", false);
        f.Extern("UnityEngineComponent.__GetComponent__T", true);
        f.Extern("UnityEngineComponent.__GetComponent__SystemType__UnityEngineComponent", true);
        f.Type("VRC.Udon.Common.Interfaces.NetworkEventTarget", TypeKind::Enum, {}, {}, { { "All", 0 }, { "Owner", 1 } });
        f.Extern("VRCUdonCommonInterfacesIUdonEventReceiver.__SendCustomEvent__SystemString__SystemVoid", true, [](auto& h, auto& p) {
            RunOn(h[p[0]], As<std::string>(h, p[1]));
        });
        f.Extern("VRCUdonCommonInterfacesIUdonEventReceiver.__SendCustomNetworkEvent__VRCUdonCommonInterfacesNetworkEventTarget_SystemString__SystemVoid", true,
            [&log = f.log](auto& h, auto& p) {
                log.push_back(std::format("net:{}:{}", As<int32_t>(h, p[1]), As<std::string>(h, p[2])));
                RunOn(h[p[0]], As<std::string>(h, p[2]));
            });
        f.Extern("VRCUdonCommonInterfacesIUdonEventReceiver.__SendCustomNetworkEvent__VRCUdonCommonInterfacesNetworkEventTarget_SystemString_SystemObject__SystemVoid", true,
            [&log = f.log](auto& h, auto& p) {
                log.push_back(std::format("net:{}:{}", As<int32_t>(h, p[1]), As<std::string>(h, p[2])));
                NetworkRunOn(h[p[0]], As<std::string>(h, p[2]), { h[p[3]] });
            });
        f.Type("VRC.Udon.Common.Enums.EventTiming", TypeKind::Enum, {}, {}, { { "Update", 0 }, { "LateUpdate", 1 } });
        f.Extern("VRCUdonCommonInterfacesIUdonEventReceiver.__SendCustomEventDelayedSeconds__SystemString_SystemSingle_VRCUdonCommonEnumsEventTiming__SystemVoid", true,
            [&log = f.log](auto& h, auto& p) { log.push_back(std::format("delay:{}:{}", As<std::string>(h, p[1]), As<float>(h, p[2]))); });
        f.Extern("VRCUdonCommonInterfacesIUdonEventReceiver.__SendCustomEventDelayedFrames__SystemString_SystemInt32_VRCUdonCommonEnumsEventTiming__SystemVoid", true,
            [&log = f.log](auto& h, auto& p) { log.push_back(std::format("frames:{}:{}", As<std::string>(h, p[1]), As<int32_t>(h, p[2]))); });
        f.Type("UnityEngine.Time", TypeKind::Class);
        f.Extern("UnityEngineTime.__get_time__SystemSingle", false, [](auto& h, auto& p) { h[p[0]] = 10.0f; });
        f.Extern("VRCUdonCommonInterfacesIUdonEventReceiver.__SetProgramVariable__SystemString_SystemObject__SystemVoid", true, [](auto& h, auto& p) {
            VarOn(h[p[0]], As<std::string>(h, p[1])) = h[p[2]];
        });
        f.Extern("VRCUdonCommonInterfacesIUdonEventReceiver.__GetProgramVariable__SystemString__SystemObject", true, [](auto& h, auto& p) {
            h[p[2]] = VarOn(h[p[0]], As<std::string>(h, p[1]));
        });
        return f;
    }

    Cell Initial(Fixture& f, const HeapValue& v, const std::string& t) {
        switch (v.kind) {
            case ValueKind::Construct: {
                ExternInfo info;
                ParseExternSignature(v.text, info);
                std::vector<Cell> scratch;
                std::vector<uint32_t> addresses;
                for (size_t i = 0; i < v.arguments.size(); ++i) {
                    scratch.push_back(Initial(f, v.arguments[i], info.parameters[i]));
                    addresses.push_back(static_cast<uint32_t>(i));
                }
                scratch.emplace_back();
                addresses.push_back(static_cast<uint32_t>(scratch.size() - 1));
                f.impls.at(v.text)(scratch, addresses);
                return scratch.back();
            }
            case ValueKind::Array: {
                if (t == "UnityEngineComponentArray") return std::make_shared<std::vector<BehaviourRef>>(v.arguments.size(), BehaviourRef{ -1 });
                if (t == "SystemInt32Array") {
                    auto ints = std::make_shared<std::vector<int32_t>>();
                    for (const HeapValue& e : v.arguments) ints->push_back(static_cast<int32_t>(e.integer));
                    return ints;
                }
                std::vector<uint32_t> elements;
                for (const HeapValue& e : v.arguments) elements.push_back(static_cast<uint32_t>(e.unsignedInteger));
                return elements;
            }
            case ValueKind::Boolean: return v.boolean;
            case ValueKind::String: return v.text;
            case ValueKind::Unsigned: return static_cast<uint32_t>(v.unsignedInteger);
            case ValueKind::Real: return t == "SystemDouble" ? Cell(v.real) : Cell(static_cast<float>(v.real));
            case ValueKind::Integer: return t == "SystemInt64" ? Cell(v.integer) : Cell(static_cast<int32_t>(v.integer));
            case ValueKind::Default:
                if (t == "SystemInt32") return int32_t{};
                if (t == "SystemSingle") return 0.0f;
                if (t == "SystemBoolean") return false;
                if (t == "SystemUInt32") return uint32_t{};
                if (t == "UnityEngineVector3") return Vec3{};
                return {};
            default: return {};
        }
    }

    struct Counters {
        int externs = 0;
        int copies = 0;
        int jumps = 0;
        int branches = 0;
        int indirect = 0;
        int instructions = 0;
    };

    struct Machine {
        Fixture&            fixture;
        const Program&      program;
        std::vector<Cell>   heap;
        Counters            counters;

        Machine(Fixture& f, const Program& p) : fixture(f), program(p) {
            for (const HeapSlot& s : p.heap) heap.push_back(Initial(f, s.value, s.type));
        }

        Cell& Var(std::string_view symbol) {
            for (size_t i = 0; i < program.heap.size(); ++i)
                if (program.heap[i].symbol == symbol) return heap[i];
            throw std::runtime_error(std::format("no symbol {}", symbol));
        }

        void Run(std::string_view entry) {
            uint32_t pc = 0xFFFFFFFFu;
            for (const EntryPoint& e : program.entryPoints)
                if (e.name == entry) pc = e.address;
            if (pc == 0xFFFFFFFFu) throw std::runtime_error(std::format("no entry {}", entry));

            counters = {};
            std::vector<uint32_t> stack;
            auto pop = [&] {
                if (stack.empty()) throw std::runtime_error("stack underflow");
                uint32_t v = stack.back();
                stack.pop_back();
                return v;
            };
            for (int steps = 0; pc != 0xFFFFFFFFu && pc / 4 < program.code.size(); ++steps) {
                if (steps > 1000000) throw std::runtime_error("step limit");
                auto op = static_cast<OpCode>(program.code[pc / 4]);
                uint32_t arg = OperandCount(op) ? program.code[pc / 4 + 1] : 0;
                uint32_t next = pc + (OperandCount(op) ? 8 : 4);
                ++counters.instructions;
                if (op == OpCode::Extern) ++counters.externs;
                if (op == OpCode::Copy) ++counters.copies;
                if (op == OpCode::Jump) ++counters.jumps;
                if (op == OpCode::JumpIfFalse) ++counters.branches;
                if (op == OpCode::JumpIndirect) ++counters.indirect;
                switch (op) {
                    case OpCode::Push: stack.push_back(arg); break;
                    case OpCode::Pop: pop(); break;
                    case OpCode::Copy: {
                        uint32_t dst = pop();
                        uint32_t src = pop();
                        heap[dst] = heap[src];
                        break;
                    }
                    case OpCode::JumpIfFalse:
                        if (!std::get<bool>(heap[pop()])) next = arg;
                        break;
                    case OpCode::Jump: next = arg; break;
                    case OpCode::JumpIndirect: next = std::get<uint32_t>(heap[arg]); break;
                    case OpCode::Nop:
                    case OpCode::Annotation: break;
                    case OpCode::Extern: {
                        const std::string& sig = program.heap[arg].value.text;
                        const ExternInfo* info = fixture.catalog.FindExtern(sig);
                        if (!info) throw std::runtime_error("unknown extern " + sig);
                        std::vector<uint32_t> params(static_cast<size_t>(info->parameterCount));
                        for (size_t i = params.size(); i-- > 0;) params[i] = pop();
                        auto it = fixture.impls.find(sig);
                        if (it == fixture.impls.end()) throw std::runtime_error("no implementation for " + sig);
                        it->second(heap, params);
                        break;
                    }
                    default: break;
                }
                pc = next;
            }
            if (!stack.empty()) throw std::runtime_error(std::format("stack not balanced: {} left", stack.size()));
        }
    };

    std::vector<Machine*> g_machines;

    void RunOn(const Cell& behaviour, const std::string& entry) {
        g_machines.at(static_cast<size_t>(std::get<BehaviourRef>(behaviour).id))->Run(entry);
    }

    Cell& VarOn(const Cell& behaviour, const std::string& symbol) {
        return g_machines.at(static_cast<size_t>(std::get<BehaviourRef>(behaviour).id))->Var(symbol);
    }

    void NetworkRunOn(const Cell& behaviour, const std::string& entry, const std::vector<Cell>& arguments) {
        Machine* target = g_machines.at(static_cast<size_t>(std::get<BehaviourRef>(behaviour).id));
        for (const NetworkCallable& n : target->program.networkCallables) {
            if (n.entryPoint != entry) continue;
            if (n.parameters.size() != arguments.size()) throw std::runtime_error("network argument count mismatch");
            for (size_t i = 0; i < arguments.size(); ++i) target->Var(n.parameters[i].symbol) = arguments[i];
            target->Run(entry);
            return;
        }
        throw std::runtime_error("entry is not network callable: " + entry);
    }

    int failures = 0;

    void Check(bool ok, std::string_view what) {
        std::printf("%s %.*s\n", ok ? "  ok  " : "  FAIL", static_cast<int>(what.size()), what.data());
        if (!ok) ++failures;
    }

    std::optional<Program> Build(Fixture& f, std::string_view source, const CompileOptions& options = {}) {
        CompileResult r = Compile(f.catalog, source, options);
        for (const Diagnostic& d : r.diagnostics) std::printf("    %d:%d %s\n", d.line + 1, d.column + 1, d.message.c_str());
        return r.program;
    }

    bool HasError(Fixture& f, std::string_view source, std::string_view fragment, const CompileOptions& options = {}) {
        CompileResult r = Compile(f.catalog, source, options);
        for (const Diagnostic& d : r.diagnostics)
            if (d.message.find(fragment) != std::string::npos) return !r.Succeeded();
        for (const Diagnostic& d : r.diagnostics) std::printf("    got: %s\n", d.message.c_str());
        return false;
    }

    template <typename F>
    void Case(std::string_view name, F body) {
        std::printf("%.*s\n", static_cast<int>(name.size()), name.data());
        try {
            body();
        } catch (const std::exception& e) {
            Check(false, std::format("threw: {}", e.what()));
        }
    }

} // namespace

int main() {
    Case("loops, fields and concatenation", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, R"(
export local count: int = 0
local total: number = 0
function Start()
    for i = 1, 10 do
        count += i
    end
    total = count / 4
    local s = "n=" .. count
    print(s)
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        m.Run("_start");
        Check(std::get<int32_t>(m.Var("count")) == 55, "count == 55");
        Check(std::get<float>(m.Var("total")) == 13.75f, "total == 13.75");
        Check(f.log.size() == 1 && f.log[0] == "n=55", "logged n=55");
        Check(std::ranges::any_of(p->heap, [](const HeapSlot& s) { return s.symbol == "count" && s.exported; }), "count exported");
        Check(std::ranges::any_of(p->heap, [](const HeapSlot& s) { return s.symbol == "total" && !s.exported; }), "total private");
    });

    Case("functions, branches and loops", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, R"(
local result = 0
local function add(a: int, b: int): int
    return a + b
end
local function clamp(v: int, lo: int, hi: int): int
    if v < lo then
        return lo
    elseif v > hi then
        return hi
    end
    return v
end
function Start()
    local x = add(2, 3)
    result = clamp(add(x, 100), 0, 50)
    local n = 0
    while true do
        n += 1
        if n >= 7 then break end
    end
    result += n
    repeat
        n -= 1
        if n % 2 == 0 then continue end
        result += 1
    until n <= 0
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        m.Run("_start");
        Check(std::get<int32_t>(m.Var("result")) == 60, std::format("result == 60 (got {})", std::get<int32_t>(m.Var("result"))));
    });

    Case("arrays and iteration", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, R"(
local sum = 0
local count = 0
function Start()
    local xs: {int} = {4, 5, 6}
    for i, v in xs do
        sum += v * i
    end
    xs[0] = 10
    count = #xs + xs[0] + Array.IndexOf(xs, 6) * 100
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        m.Run("_start");
        Check(std::get<int32_t>(m.Var("sum")) == 17, "sum == 17");
        Check(std::get<int32_t>(m.Var("count")) == 213, "count == 213 with inferred generic IndexOf");
        const ExternInfo* indexOf = f.catalog.FindExtern("SystemArray.__IndexOf__TArray_T__SystemInt32");
        const ExternInfo* getComponent = f.catalog.FindExtern("UnityEngineComponent.__GetComponent__T");
        Check(indexOf && !indexOf->hasTypeOperand && indexOf->isStatic, "inferable generic has no type operand");
        Check(getComponent && getComponent->hasTypeOperand && !getComponent->isStatic, "return-only generic takes a type operand");
    });

    Case("structs, interpolation and boolean logic", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, R"(
local y: number = 0
local ok = false
local floored = 0
function Start()
    local v = Vector3.new(1, 2, 3)
    v.y = 10
    local w = v + v * 2
    y = w.y
    ok = y > 20 and not (y > 100) or false
    floored = 7 // 2 + 2 * 4
    print(`y={y} ok={ok}`)
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        m.Run("_start");
        Check(std::get<float>(m.Var("y")) == 30.0f, "y == 30");
        Check(std::get<bool>(m.Var("ok")), "ok");
        Check(std::get<int32_t>(m.Var("floored")) == 11, "constant folding");
        Check(f.log.size() == 1 && f.log[0] == "y=30 ok=True", std::format("log '{}'", f.log.empty() ? "" : f.log[0]));
    });

    Case("events, enums and self references", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, R"(
local key = KeyCode.Space
local joined = 0
function OnPlayerJoined(player: VRCPlayerApi)
    joined += 1
end
export function Ping()
    this:SendCustomEvent("Pong")
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        bool hasEntry = std::ranges::any_of(p->entryPoints, [](const EntryPoint& e) { return e.name == "_onPlayerJoined"; });
        bool hasCustom = std::ranges::any_of(p->entryPoints, [](const EntryPoint& e) { return e.name == "_Ping"; });
        bool hasParam = std::ranges::any_of(p->heap, [](const HeapSlot& s) { return s.symbol == "onPlayerJoinedPlayer" && s.type == "VRCSDKBaseVRCPlayerApi"; });
        bool keyValue = std::ranges::any_of(p->heap, [](const HeapSlot& s) { return s.symbol == "key" && s.type == "UnityEngineKeyCode" && s.value.integer == 32; });
        Check(hasEntry && hasCustom, "entry points");
        Check(hasParam, "event parameter symbol");
        Check(keyValue, "enum constant");
        Machine m(f, *p);
        m.Run("_onPlayerJoined");
        m.Run("_onPlayerJoined");
        Check(std::get<int32_t>(m.Var("joined")) == 2, "event ran twice");
    });

    Case("names, aliases and annotations", [] {
        Fixture f = MakeFixture();
        f.Type("Foo.Thing", TypeKind::Class);
        f.Type("Bar.Thing", TypeKind::Class);
        auto p = Build(f, R"(-- @syncmode(continuous)

type UObject = UnityEngine.Object
local SDKBase = VRC.SDKBase
local V3 = Vector3

-- @header("Movement")
-- @range(0, 10)
-- @tooltip("Degrees, per second")
-- @sync(linear)
export local speed: number = 1
export local Object: Transform
export local anyObject: Object
export local owner: SDKBase.VRCPlayerApi
local raw: UObject

function Start()
    if not Object then
        Object = transform
    end
    local v = V3.new(1, 2, 3)
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        auto slot = [&](std::string_view name) -> const HeapSlot* {
            for (const HeapSlot& s : p->heap)
                if (s.symbol == name) return &s;
            return nullptr;
        };
        const HeapSlot* speed = slot("speed");
        bool attrs = speed && speed->attributes.size() == 3 &&
                     speed->attributes[0].name == "header" && speed->attributes[0].arguments == std::vector<std::string>{ "Movement" } &&
                     speed->attributes[1].name == "range" && speed->attributes[1].arguments == std::vector<std::string>{ "0", "10" } &&
                     speed->attributes[2].name == "tooltip" && speed->attributes[2].arguments == std::vector<std::string>{ "Degrees, per second" };
        Check(attrs, "attributes passed through in order with parsed arguments");
        Check(p->attributes.size() == 1 && p->attributes[0].name == "syncmode" && p->attributes[0].arguments == std::vector<std::string>{ "continuous" }, "module annotations");
        Check(p->sync.size() == 1 && p->sync[0].symbol == "speed" && p->sync[0].interpolation == SyncInterpolation::Linear, "sync(linear)");
        Check(slot("Object") && slot("Object")->type == "UnityEngineTransform", "variable named Object");
        Check(slot("anyObject") && slot("anyObject")->type == "UnityEngineObject", "Object prefers UnityEngine");
        Check(slot("owner") && slot("owner")->type == "VRCSDKBaseVRCPlayerApi", "namespace alias in annotation");
        Check(slot("raw") && slot("raw")->type == "UnityEngineObject", "type alias");
        Check(!slot("SDKBase") && !slot("V3"), "aliases take no heap slots");
        Check(HasError(f, "local t: Thing", "ambiguous"), "ambiguous name outside preferred namespaces");
        Check(HasError(f, "-- @sync(fast)\nlocal x = 1", "unknown sync mode"), "bad sync mode");
    });

    Case("inlining and constant folding", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, R"(
local result = 0
local function add(a: int, b: int): int
    return a + b
end
local function clamp(v: int, lo: int, hi: int): int
    if v < lo then
        return lo
    elseif v > hi then
        return hi
    end
    return v
end
function Start()
    result = add(2, 3)
end
export function Clamp()
    result = clamp(result + 100, 0, 50)
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        m.Run("_start");
        Check(std::get<int32_t>(m.Var("result")) == 5 && m.counters.externs == 0 && m.counters.indirect == 0,
            std::format("add(2, 3) folded to a constant ({} externs)", m.counters.externs));
        m.Run("_Clamp");
        Check(std::get<int32_t>(m.Var("result")) == 50 && m.counters.indirect == 0, "clamp inlined without call overhead");
    });

    Case("noinline keeps real calls", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, R"(
local n = 0
-- @noinline
local function bump(): int
    n += 1
    return n
end
function Start()
    local a = bump()
    local b = bump()
    n = a + b
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        m.Run("_start");
        Check(std::get<int32_t>(m.Var("n")) == 3, "results correct");
        Check(m.counters.indirect == 2, std::format("two returns through the trampoline (got {})", m.counters.indirect));
    });

    Case("events skip the return trampoline", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, R"(
local n = 0
-- @noinline
export function Reset()
    n = 0
end
function Update()
    n += 1
    if n > 100 then
        Reset()
    end
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        m.Run("_update");
        Check(m.counters.indirect == 0 && m.counters.copies == 0, std::format("Update: {} indirect, {} copies", m.counters.indirect, m.counters.copies));
        m.Run("_Reset");
        Check(m.counters.indirect == 1, "an event that is also called keeps its trampoline");
    });

    Case("rotated loops", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, R"(
local sum = 0
local k = 0
function Start()
    for i = 1, 10 do
        sum += i
    end
end
export function Count()
    while k < 5 do
        k += 1
    end
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        m.Run("_start");
        Check(std::get<int32_t>(m.Var("sum")) == 55, "for result");
        Check(m.counters.jumps == 1 && m.counters.branches == 10, std::format("for: {} jumps, {} branches", m.counters.jumps, m.counters.branches));
        m.Run("_Count");
        Check(std::get<int32_t>(m.Var("k")) == 5, "while result");
        Check(m.counters.jumps == 2 && m.counters.branches == 6, std::format("while: {} jumps, {} branches", m.counters.jumps, m.counters.branches));
    });

    Case("constant structs", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, R"(
local y: number = 0
local up = Vector3.new(0, 1, 0)
function Start()
    local v = Vector3.new(0, 2, 0)
    y = v.y + up.y
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        m.Run("_start");
        Check(std::get<float>(m.Var("y")) == 3.0f, "value");
        Check(m.counters.externs == 3, std::format("no constructor at run time ({} externs)", m.counters.externs));
        Check(std::ranges::any_of(p->heap, [](const HeapSlot& s) { return s.symbol == "up" && s.value.kind == ValueKind::Construct; }), "struct field initialized at build time");
    });

    Case("static constants are built into the heap", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, R"(
local y: number = 0
function Start()
    y = Vector3.up.y
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        m.Run("_start");
        Check(std::get<float>(m.Var("y")) == 1.0f, "value");
        Check(m.counters.externs == 1, std::format("Vector3.up read without an extern ({} externs)", m.counters.externs));
    });

    Case("pure calls with constant arguments are built into the heap", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, R"(
local y: number = 0
local z: number = 0
function Start()
    y = Mathf.Floor(2.5)
    z = Mathf.PI * 2
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        m.Run("_start");
        Check(std::get<float>(m.Var("y")) == 2.0f && std::get<float>(m.Var("z")) == 3.14159265358979f * 2.0f, "values");
        Check(m.counters.externs == 0, std::format("no externs at run time ({})", m.counters.externs));
    });

    Case("repeated reads reuse the first result until something could change it", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, R"(
local y: number = 0
local v = Vector3.new(1, 2, 3)
function Start()
    y = v.y + v.y
    v.y = 5
    y = y + v.y
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        m.Run("_start");
        Check(std::get<float>(m.Var("y")) == 9.0f, std::format("value ({})", std::get<float>(m.Var("y"))));
        Check(m.counters.externs == 5, std::format("v.y read once before the write ({} externs)", m.counters.externs));
    });

    Case("table, math and string library", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, R"(
local xs: {int}
local filled: {int}
local n = 0
local found = 0
local label = ""
function Start()
    xs = {1, 2}
    table.insert(xs, 3)
    table.insert(xs, 0, 9)
    local r = table.remove(xs, 1)
    local last = table.remove(xs)
    found = table.find(xs, 2)
    n = #xs * 100 + r * 10 + last
    filled = table.create(5, 7)
    n += filled[4] * 1000 + math.min(4, 2, 3) * 10000
end
export function Describe()
    label = string.format("%d items, %.2f%% {x}", n, 1.5)
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        m.Run("_start");
        Check(std::get<int32_t>(m.Var("n")) == 27213, std::format("insert, remove, create and min ({})", std::get<int32_t>(m.Var("n"))));
        Check(std::get<int32_t>(m.Var("found")) == 1, "find");
        Check(std::ranges::any_of(p->heap, [](const HeapSlot& s) { return s.value.text == "{0} items, {1:F2}% {{x}}"; }), "format string translated");
    });

    Case("List<T> is an array plus a count", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, R"(
local items: List<int> = {5, 6}
local total = 0
local count = 0
local found = 0
local removed = 0
local flag = false
local size = 0
-- @inline
local function sum(xs: List<int>): int
    local s = 0
    for _, v in xs do
        s += v
    end
    return s
end
function Start()
    for i = 1, 10 do
        items:Add(i)
    end
    items:Insert(0, 100)
    items:RemoveAt(1)
    removed = table.remove(items)
    table.insert(items, 7)
    found = items:IndexOf(6)
    items[2] += 1
    count = #items
    total = sum(items)
end
export function Local()
    local ys: List<int> = List.new(1)
    ys:Add(3)
    ys:Add(4)
    ys:Add(5)
    local ok = ys:Remove(4)
    flag = ok and ys:Contains(5) and not ys:Contains(4)
    size = #ys:ToArray()
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        m.Run("_start");
        Check(std::get<int32_t>(m.Var("count")) == 12 && std::get<int32_t>(m.Var("removed")) == 10 && std::get<int32_t>(m.Var("found")) == 1,
            std::format("count {} removed {} found {}", std::get<int32_t>(m.Var("count")), std::get<int32_t>(m.Var("removed")), std::get<int32_t>(m.Var("found"))));
        Check(std::get<int32_t>(m.Var("total")) == 159, std::format("sum through an inlined List parameter ({})", std::get<int32_t>(m.Var("total"))));
        m.Run("_Local");
        Check(std::get<bool>(m.Var("flag")) && std::get<int32_t>(m.Var("size")) == 2, "local list remove, contains and ToArray");

        Fixture g = MakeFixture();
        Check(!Build(g, "local xs: List<int> = {}\nlocal ys: List<int> = {}\nfunction Start()\n    ys = xs\nend\n").has_value(), "lists cannot be copied");
    });

    Case("singletons", [] {
        Fixture f = MakeFixture();
        f.Extern("UnityEngineGameObject.__Find__SystemString__UnityEngineGameObject", false, [&log = f.log](auto& h, auto& p) {
            log.push_back("find:" + As<std::string>(h, p[0]));
            h[p[1]] = BehaviourRef{ 0 };
        });
        f.Extern("UnityEngineGameObject.__GetComponent__T", true, [](auto& h, auto& p) { h[p[2]] = h[p[0]]; });
        f.impls["UnityEngineObject.__op_Inequality__UnityEngineObject_UnityEngineObject__SystemBoolean"] = [](auto& h, auto& p) {
            h[p[2]] = std::holds_alternative<BehaviourRef>(h[p[0]]) && std::get<BehaviourRef>(h[p[0]]).id >= 0;
        };
        const char* stateSource = R"(-- @singleton
export local score: int = 0
export function AddScore(n: int)
    score += n
end
)";
        CompileResult face = ExtractInterface(f.catalog, stateSource);
        Check(face.scriptInterface && face.scriptInterface->singleton, "the interface is marked singleton");
        if (!face.scriptInterface) return;
        ScriptInfo state = *face.scriptInterface;
        state.name = "GameState";
        f.catalog.AddScript(state);

        auto stateProgram = Build(f, stateSource);
        auto user = Build(f, R"(
local seen = 0
function Start()
    GameState:AddScore(5)
end
function Update()
    seen = GameState.score
end
)");
        auto bystander = Build(f, "local n = 0\nfunction Update()\n    n += 1\nend\n");
        Check(stateProgram && user && bystander, "compiles");
        if (!stateProgram || !user || !bystander) return;
        Check(stateProgram->updateOrder < -1000000, "singletons start before other scripts");
        Check(std::ranges::none_of(bystander->heap, [](const HeapSlot& s) { return s.symbol.starts_with("__singleton"); }), "scripts that never use a singleton pay nothing");

        Machine stateMachine(f, *stateProgram);
        Machine userMachine(f, *user);
        g_machines = { &stateMachine, &userMachine };
        userMachine.Run("_start");
        Check(f.log.size() == 2 && f.log[0] == "find:/__UdonLuauSingletons/GameState" && f.log[1] == "frames:_start:1",
            "resolved by its fixed root path once, and Start waits for the singleton to start");
        Check(std::get<int32_t>(stateMachine.Var("score")) == 0, "nothing was called before the singleton started");
        stateMachine.Run("_start");
        Check(std::get<bool>(stateMachine.Var("__started")), "a singleton without Start still marks itself started");
        f.log.clear();
        userMachine.Run("_start");
        Check(std::get<int32_t>(stateMachine.Var("score")) == 5 && f.log.empty(), "the retried Start reaches the singleton without resolving again");
        userMachine.Run("_update");
        Check(std::get<int32_t>(userMachine.Var("seen")) == 5, "fields read through the singleton");
    });

    Case("negated conditions branch without an extern", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, R"(
export local flag = false
local n = 0
function Start()
    if not flag then
        n += 1
    end
    while not flag do
        n += 1
        flag = n > 3
    end
    local more = true
    repeat
        n += 1
        more = n < 6
    until not more
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        m.Run("_start");
        Check(std::get<int32_t>(m.Var("n")) == 6, std::format("result ({})", std::get<int32_t>(m.Var("n"))));
        Check(std::ranges::none_of(p->heap, [](const HeapSlot& s) { return s.value.text.find("op_UnaryNegation") != std::string::npos; }), "no negation extern");
    });

    Case("calls that cannot write variables skip operand copies", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, R"(
local n = 2
local total = 0
local function square(x: int): int
    return x * x
end
-- @noinline
local function triple(): int
    local a = n * 3
    return a + 1
end
function Start()
    total += square(n)
    total = (n + 5) + triple()
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        m.Run("_start");
        Check(std::get<int32_t>(m.Var("total")) == 14, std::format("result ({})", std::get<int32_t>(m.Var("total"))));
    });

    Case("switch-style chains become jump tables", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, R"(
export local x: int = 0
local r = 0
export function Pick()
    if x == 0 then r = 10
    elseif x == 1 then r = 11
    elseif x == 2 then r = 12
    elseif x == 3 then r = 13
    elseif x == 4 then r = 14
    elseif x == 5 then r = 15
    elseif x == 6 then r = 16
    elseif x == 7 then r = 17
    else r = -1
    end
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        bool all = true;
        int maxExterns = 0;
        for (int32_t value : { 0, 3, 7, 8, -2 }) {
            m.Var("x") = value;
            m.Run("_Pick");
            int32_t expected = value >= 0 && value <= 7 ? 10 + value : -1;
            all = all && std::get<int32_t>(m.Var("r")) == expected;
            maxExterns = std::max(maxExterns, m.counters.externs);
        }
        Check(all, "every case dispatches correctly");
        Check(maxExterns <= 3, std::format("at most 3 externs per dispatch (got {})", maxExterns));
    });

    Case("compile-time defines, const and assert", [] {
        Fixture f = MakeFixture();
        CompileOptions options;
        options.defines = { { "DEBUG", "false" }, { "LEVEL", "3" } };
        auto p = Build(f, R"(-- @define(FEATURE, true)

const LIMIT = LEVEL * 2
local r = 0
function Start()
    if DEBUG then
        print("debug")
    end
    if FEATURE then
        r = LIMIT
    end
    assert(LIMIT == 6, "limit")
    assert(r >= 0)
end
)", options);
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        m.Run("_start");
        Check(std::get<int32_t>(m.Var("r")) == 6 && m.counters.externs == 0 && f.log.empty(), "dead branches and release asserts removed");
        Check(std::ranges::none_of(p->heap, [](const HeapSlot& s) { return s.symbol == "LIMIT"; }), "constants take no heap slot");
        Check(HasError(f, "const L = 2\nfunction Start()\n assert(L == 3, \"L must be 3\")\nend", "L must be 3"), "failed constant assert is a compile error");

        CompileOptions debug;
        debug.defines = { { "DEBUG", "true" } };
        auto d = Build(f, "local r = 0\nfunction Start()\n assert(r > 0, \"r positive\")\nend", debug);
        Check(d.has_value(), "debug build compiles");
        if (!d) return;
        Machine dm(f, *d);
        dm.Run("_start");
        Check(f.log.size() == 1 && f.log[0] == "error: r positive", "debug assert logs at run time");
    });

    Case("real operator spellings and underscored type names", [] {
        Fixture f = MakeFixture();
        f.Type("VRC.SDKBase.VRC_Pickup", TypeKind::Class, "UnityEngineComponent");
        f.Type("TMPro.TMP_Text", TypeKind::Class, "UnityEngineComponent");
        f.Type("TMPro.TMP_TextInfo", TypeKind::Class);
        Check(f.catalog.AddExtern("UnityEngineComponent.__Grab__VRCSDKBaseVRC_Pickup__SystemVoid", 2), "underscored parameter accepted");
        Check(f.catalog.AddExtern("TMProTMP_TextInfo.__ctor__TMProTMP_Text__TMProTMP_TextInfo", 2), "count that only fits after merging accepted");
        auto p = Build(f, R"(
export local speed: number = 2
export local a: int = 17
export local pickup: VRC_Pickup
export local label: TMP_Text
local out: number = 0
local m = 0
function Start()
    out = 10 * speed * speed - -speed
    m = a % 5
end
export function Other()
    transform:Grab(pickup)
    local info = TMP_TextInfo.new(label)
end
)");
        Check(p.has_value(), "compiles");
        if (!p) return;
        Machine m(f, *p);
        m.Run("_start");
        Check(std::get<float>(m.Var("out")) == 42.0f && std::get<int32_t>(m.Var("m")) == 2, "op_Multiplication, op_UnaryMinus and op_Remainder used");
    });

    Case("public methods and typed cross-behaviour calls", [] {
        Fixture f = MakeFixture();
        const char* doorSource = R"(
export local opened = 0
export local speed: number = 1
-- @networkcallable
export function Open()
    opened += 1
end
-- @networkcallable(10)
export function Hit(damage: int)
    opened += damage
end
export function Add(a: int, b: int): int
    return a + b
end
)";
        CompileResult face = ExtractInterface(f.catalog, doorSource);
        Check(face.scriptInterface.has_value() && !face.program, "interface extracted without compiling bodies");
        if (!face.scriptInterface) return;
        ScriptInfo door = *face.scriptInterface;
        door.name = "Door";
        auto add = std::ranges::find(door.methods, "Add", &ScriptMethod::name);
        auto open = std::ranges::find(door.methods, "Open", &ScriptMethod::name);
        Check(open != door.methods.end() && open->entryPoint == "Open" && open->networkCallable, "network callable method keeps its name");
        Check(add != door.methods.end() && !add->networkCallable && add->entryPoint == "__0__Add" && add->parameters.size() == 2 && add->parameters[0].symbol == "__0_a__param" &&
                  add->parameters[1].symbol == "__0_b__param" && add->returns.size() == 1 && add->returns[0].symbol == "__0___0__Add__ret" &&
                  add->returns[0].type == "SystemInt32",
            "method layout matches UdonSharp's naming");
        Check(door.fields.size() == 2 && door.fields[1].name == "speed" && door.fields[1].type == "SystemSingle", "public fields");
        f.catalog.AddScript(door);

        auto doorProgram = Build(f, doorSource);
        auto callerProgram = Build(f, R"(
export local door: Door
export local doors: {Door}
local result = 0
local speedSeen: number = 0
function Start()
    door:Open()
    result = door:Add(2, 40)
    door.speed = 5
    speedSeen = door.speed
    Network.All(door):Open()
    Network.Owner(door):Hit(7)
end
)");
        Check(doorProgram.has_value() && callerProgram.has_value(), "both compile");
        if (!doorProgram || !callerProgram) return;
        Machine doorMachine(f, *doorProgram);
        Machine caller(f, *callerProgram);
        g_machines = { &doorMachine, &caller };
        caller.Var("door") = BehaviourRef{ 0 };
        caller.Run("_start");
        Check(std::get<int32_t>(doorMachine.Var("opened")) == 9, "Open ran locally and through the network, Hit(7) delivered its argument");
        const NetworkCallable* hit = nullptr;
        for (const NetworkCallable& n : doorProgram->networkCallables)
            if (n.entryPoint == "Hit") hit = &n;
        Check(doorProgram->networkCallables.size() == 2 && hit && hit->maxEventsPerSecond == 10 && hit->parameters.size() == 1 &&
                  hit->parameters[0].symbol == "__0_damage__param" && hit->parameters[0].type == "SystemInt32",
            "network calling metadata");
        Check(std::get<int32_t>(caller.Var("result")) == 42, "arguments in, result out");
        Check(std::get<float>(doorMachine.Var("speed")) == 5.0f && std::get<float>(caller.Var("speedSeen")) == 5.0f, "fields written and read");
        Check(f.log.size() == 2 && f.log[0] == "net:0:Open" && f.log[1] == "net:1:Hit", "SendCustomNetworkEvent with NetworkEventTarget.All and .Owner");
        Check(HasError(f, "export local door: Door\nfunction Start()\n door:Close()\nend", "no member 'Close'"), "unknown method");
        Check(HasError(f, "export local door: Door\nfunction Start()\n Network.All(door):Add(1, 2)\nend", "not network callable"), "unmarked methods cannot be called over the network");

        auto delayed = Build(f, R"(
export local door: Door
local hits = 0
local waitFrames = 3
local function bump(n: int)
    hits += n
end
function Start()
    Delay.Seconds(1, this):bump(2)
    Delay.Seconds(1, this):bump(3)
    Delay.Frames(2, Network.All(door)):Hit(7)
    Delay.Frames(waitFrames, door):Add(1, 2)
    Delay.Seconds(0.5, door):Open()
end
)");
        Check(delayed.has_value(), "delayed calls compile");
        if (!delayed) return;
        doorMachine.Var("opened") = int32_t{ 0 };
        Machine later(f, *delayed);
        g_machines = { &doorMachine, &later };
        later.Var("door") = BehaviourRef{ 0 };
        f.log.clear();
        later.Run("_start");
        Check(f.log.size() == 5 && f.log[0] == "delay:__delay0:1" && f.log[1] == "delay:__delay1:1" && f.log[2] == "frames:__delay2:2" && f.log[3] == "frames:__delay3:3" &&
                  f.log[4] == "delay:Open:0.5",
            std::format("scheduled through the SDK ({})", [&] {
                std::string all;
                for (const std::string& line : f.log) all += line + " ";
                return all;
            }()));
        later.Run("__delay0");
        Check(std::get<int32_t>(later.Var("hits")) == 2, "first delayed call gets its own argument");
        later.Run("__delay1");
        Check(std::get<int32_t>(later.Var("hits")) == 5, "second delayed call gets its own argument");
        later.Run("__delay0");
        Check(std::get<int32_t>(later.Var("hits")) == 5, "a stub fired with an empty queue does nothing");
        later.Run("__delay2");
        Check(std::get<int32_t>(doorMachine.Var("opened")) == 7 && f.log.back() == "net:0:Hit", "delayed network call with an argument");
        later.Run("__delay3");
        Check(std::get<int32_t>(doorMachine.Var("__0___0__Add__ret")) == 3, "delayed call with a variable delay reaches the target");

        auto ticking = Build(f, R"(
local total = 0
local function tick(n: int)
    total += n
    if n < 3 then
        Delay.Seconds(1, this):tick(n + 1)
    end
end
function Start()
    Delay.Seconds(1, this):tick(1)
end
)");
        Check(ticking.has_value(), "a function that delays a call to itself compiles");
        if (ticking) {
            Machine ticker(f, *ticking);
            ticker.Run("_start");
            for (int i = 0; i < 3; ++i) ticker.Run(i == 0 ? "__delay1" : "__delay0");
            Check(std::get<int32_t>(ticker.Var("total")) == 6, std::format("repeating delayed call ({})", std::get<int32_t>(ticker.Var("total"))));
        }
        Check(HasError(f, "local function go(n: int, t: EventTiming)\n    Delay.Frames(1, this, t):go(n, t)\nend\nfunction Start()\n    go(1, EventTiming.Update)\nend\n",
                  "constant EventTiming"),
            "a variable EventTiming is rejected for calls with arguments");
        Check(HasError(f, "-- @networkcallable\nexport function Get(): int\n return 1\nend", "cannot return values"), "network callable methods cannot return");
        Check(HasError(f, "-- @networkcallable\nlocal function Hidden()\nend", "only 'export function'"), "network callable must be public");
        g_machines.clear();
    });

    Case("UdonSharp-compatible export layout", [] {
        Fixture f = MakeFixture();
        CompileResult r = ExtractInterface(f.catalog, R"(
export function Close(): int
    return 1
end
export function Twice(value: int): int
    return value * 2
end
export function Again(value: int, other: int)
end
-- @networkcallable
export function Ping(value: int)
end
)");
        Check(r.scriptInterface.has_value(), "interface");
        if (!r.scriptInterface) return;
        auto method = [&](std::string_view n) { return &*std::ranges::find(r.scriptInterface->methods, n, &ScriptMethod::name); };
        Check(method("Close")->entryPoint == "_Close" && method("Close")->returns[0].symbol == "__0__Close__ret", "no parameters: _Name, __0__Name__ret");
        Check(method("Twice")->entryPoint == "__0__Twice" && method("Twice")->parameters[0].symbol == "__0_value__param" &&
                  method("Twice")->returns[0].symbol == "__0___0__Twice__ret",
            "parameters: mangled entry");
        Check(method("Again")->entryPoint == "__0__Again" && method("Again")->parameters[0].symbol == "__1_value__param" &&
                  method("Again")->parameters[1].symbol == "__0_other__param",
            "parameter counters are shared across the class in declaration order");
        Check(method("Ping")->entryPoint == "Ping" && method("Ping")->parameters[0].symbol == "__2_value__param", "network callable keeps its name");
    });

    Case("luau-lsp definitions", [] {
        Fixture f = MakeFixture();
        ScriptInfo door;
        door.name = "Door";
        door.fields.push_back({ "speed", "SystemSingle", "", "speed" });
        door.methods.push_back({ "Add", "__0__Add", { { "a", "SystemInt32", "", "__0_a__param" } }, { { "result", "SystemInt32", "", "__0___0__Add__ret" } }, false });
        f.catalog.AddScript(door);
        f.Type("Foo.Thing", TypeKind::Class);
        f.Type("Bar.Thing", TypeKind::Class);
        std::string defs = GenerateDefinitions(f.catalog);

        Luau::Allocator allocator;
        Luau::AstNameTable names(allocator);
        Luau::ParseOptions options;
        options.allowDeclarationSyntax = true;
        Luau::ParseResult parsed = Luau::Parser::parse(defs.data(), defs.size(), names, allocator, options);
        for (const Luau::ParseError& e : parsed.errors) std::printf("    %u: %s\n", e.getLocation().begin.line + 1, e.getMessage().c_str());
        Check(parsed.errors.empty(), std::format("definitions parse as Luau ({} bytes)", defs.size()));

        auto has = [&](std::string_view text) { return defs.find(text) != std::string::npos; };
        Check(has("declare extern type Vector3 with\n") && has("    y: number\n") && has("    function __add(self, p1: Vector3): Vector3\n"), "struct with fields and operators");
        Check(has("declare Vector3: {\n") && has("\n    new: (number, number, number) -> Vector3,"), "constructor on the global");
        Check(has("declare extern type Transform extends Component with\n"), "inheritance");
        Check(has("declare extern type UdonBehaviour extends MonoBehaviour with\n") && has("function SendCustomEvent(self, p1: string): ()"), "interface members folded into the class");
        Check(has("declare KeyCode: {\n    Space: KeyCode,\n    Return: KeyCode,\n}"), "enum members");
        Check(has("declare extern type Door extends UdonBehaviour with\n    speed: number\n    function Add(self, a: int): int\n"), "behaviour scripts");
        Check(has("declare transform: Transform\n") && has("    All: (behaviour: UdonBehaviour) -> any,\n"), "globals and Network");
        Check(has("declare Foo: {\n    Thing: {") && has("declare Bar: {\n    Thing: {") && !has("declare Thing:"), "ambiguous types reachable by namespace");
    });

    Case("Unity 6 member names", [] {
        Fixture f = MakeFixture();
        f.Type("UnityEngine.Rigidbody", TypeKind::Class, "UnityEngineComponent");
        f.Extern("UnityEngineRigidbody.__get_velocity__UnityEngineVector3", true);
        f.Extern("UnityEngineRigidbody.__set_velocity__UnityEngineVector3__SystemVoid", true);
        auto p = Build(f, "export local rb: Rigidbody\nfunction Start()\n rb.linearVelocity = rb.linearVelocity\n rb.velocity = rb.velocity\nend");
        Check(p.has_value(), "linearVelocity resolves to the velocity externs Udon exposes");
        if (!p) return;
        std::string listing = Disassemble(*p);
        Check(listing.find("__get_velocity__") != std::string::npos && listing.find("linearVelocity") == std::string::npos, "emits the exposed signature");
    });

    Case("behaviour sync modes", [] {
        Fixture f = MakeFixture();
        auto mode = [&](std::string_view source) {
            CompileResult r = Compile(f.catalog, source);
            return r.program ? static_cast<int>(r.program->syncMode) : -1;
        };
        Check(mode("-- @sync(linear)\nexport local f: number = 0") == static_cast<int>(BehaviourSyncMode::Any), "no annotation means any");
        Check(mode("-- @syncmode(Manual)\n\n-- @sync\nlocal xs: {int} = nil") == static_cast<int>(BehaviourSyncMode::Manual), "manual allows synced arrays");
        Check(mode("-- @syncmode(novariablesync)\n\n-- @networkcallable\nexport function Ping()\nend") == static_cast<int>(BehaviourSyncMode::NoVariableSync),
            "novariablesync keeps network events");
        Check(HasError(f, "-- @syncmode(manual)\n\n-- @sync(linear)\nlocal f: number = 0", "manual sync does not support"), "manual rejects interpolation");
        Check(HasError(f, "-- @syncmode(continuous)\n\n-- @sync\nlocal xs: {int} = nil", "continuous sync does not support"), "continuous rejects arrays");
        Check(HasError(f, "-- @syncmode(none)\n\n-- @sync\nlocal x = 0", "sync mode is none"), "none rejects synced variables");
        Check(HasError(f, "-- @syncmode(novariablesync)\n\n-- @sync\nlocal x = 0", "sync mode is novariablesync"), "novariablesync rejects synced variables");
        Check(HasError(f, "-- @syncmode(none)\n\n-- @networkcallable\nexport function Ping()\nend", "disables network events"), "none rejects network callable methods");
        Check(HasError(f, "-- @syncmode(fast)\n\nlocal x = 0", "unknown sync mode"), "unknown mode");
        Check(HasError(f, "-- @sync(linear)\nlocal b = false", "cannot use linear"), "interpolation needs a type that supports it");
        Check(HasError(f, "-- @sync\nexport local t: Transform", "Udon does not sync"), "unsyncable type");
    });

    Case("review regressions", [] {
        Fixture f = MakeFixture();
        auto check =[&](std::string_view what, std::string_view source, auto verify) {
            auto p = Build(f, source);
            if (!p) {
                Check(false, std::format("{} (compile)", what));
                return;
            }
            Machine m(f, *p);
            m.Run("_start");
            Check(verify(m), what);
        };
        auto num = [](Machine& m, std::string_view v) { return std::get<float>(m.Var(v)); };
        auto integer = [](Machine& m, std::string_view v) { return std::get<int32_t>(m.Var(v)); };

        check("real calls keep the caller's temps", R"(
local k: number = 5
local out: number = 0
-- @noinline
local function g(): number
    return k * 2 + 1
end
function Start()
    out = (k + 1) + g()
end
)", [&](Machine& m) { return num(m, "out") == 17.0f; });

        check("inlined parameters bound to temps are not retargeted", R"(
local k: number = 4
local last: number = 0
local total: number = 0
local function f(x: number)
    last = x
    total = total + x
end
function Start()
    f(k + 1)
end
)", [&](Machine& m) { return num(m, "last") == 5.0f && num(m, "total") == 5.0f; });

        check("a local copied from a parameter does not change it", R"(
local v: number = 3
local out: number = 0
local function f(x: number): number
    local y = x
    y = y * 2
    return x + y
end
function Start()
    out = f(v + 0.5)
end
)", [&](Machine& m) { return num(m, "out") == 10.5f; });

        check("struct methods do not change shared constants", R"(
local a: number = 0
function Start()
    local v = Vector3.new(0, 0, 0)
    v:Set(1, 2, 3)
    local w = Vector3.new(0, 0, 0)
    a = w.y + v.y
end
)", [&](Machine& m) { return num(m, "a") == 2.0f; });

        check("decimal literals converted to int round", R"(
local a = 0
local b = 0
function Start()
    a = (2.5 :: int)
    b = (3.5 :: int)
end
)", [&](Machine& m) { return integer(m, "a") == 2 && integer(m, "b") == 4; });
        Check(HasError(f, "function Start()\n for i: int = 0, 10.5 do end\nend", "loop bound or step"), "fractional bounds for int loops are rejected");

        check("// and % agree between constants and run time", R"(
local n: int = -7
local d: int = 0
local r: int = 0
local fd: int = 0
local fr: int = 0
function Start()
    d = n // 2
    r = n % 3
    fd = -7 // 2
    fr = -7 % 3
end
)", [&](Machine& m) { return integer(m, "d") == integer(m, "fd") && integer(m, "r") == integer(m, "fr") && integer(m, "d") == -3 && integer(m, "r") == -1; });

        check("locals without a value reset each time", R"(
local out = 0
function Start()
    for i = 1, 3 do
        local x: int
        if i == 1 then
            x = 5
        end
        out += x
    end
end
)", [&](Machine& m) { return integer(m, "out") == 5; });

        check("operands are read before later calls run", R"(
local n: int = 1
local out = 0
local function bump(): int
    n += 10
    return 1
end
function Start()
    out = n + bump()
end
)", [&](Machine& m) { return integer(m, "out") == 2; });

        check("compound assignment evaluates its target once", R"(
local calls = 0
local out = 0
local function index(): int
    calls += 1
    return 0
end
function Start()
    local xs: {int} = {1, 2}
    xs[index()] += 5
    out = xs[0]
end
)", [&](Machine& m) { return integer(m, "calls") == 1 && integer(m, "out") == 6; });

        check("assigning the loop variable does not change the iteration", R"(
local count = 0
function Start()
    for i = 1, 5 do
        count += 1
        i = i + 10
    end
end
)", [&](Machine& m) { return integer(m, "count") == 5; });

        Check(HasError(f, "function Start()\n local x = (nil).y\nend", "cannot read 'y' of nil"), "member of nil is a diagnostic");
        Check(HasError(f, "const A = 5\nlocal x: A.B", "not a type or namespace"), "literal alias as a type prefix is a diagnostic");
        bool threw = false;
        try {
            Compile(f.catalog, "local function g(): number\n return 1\nend\nfunction Start()\n print(g<<number>>())\nend");
        } catch (...) {
            threw = true;
        }
        Check(!threw, "explicit type arguments on a local function do not crash");
    });

    Case("diagnostics", [] {
        Fixture f = MakeFixture();
        Check(HasError(f, "function Start() transform:Nope() end", "has no member 'Nope'"), "unknown member");
        Check(HasError(f, "export function A()\n A()\nend", "recursion"), "recursion");
        Check(HasError(f, "function Helper() end", "not an Udon event"), "plain global functions must be events");
        Check(HasError(f, "local x: int = \"s\"", "cannot initialize"), "type mismatch");
        Check(HasError(f, "function Start() undefinedThing = 1 end", "unknown variable"), "undeclared global");
        Check(HasError(f, "function Start() local v = transform.position end", "no member 'position'"), "missing property");
        Check(HasError(f, "function Start() local a = 1 local b: number = 2 a = b end", "convert explicitly"), "narrowing");
        Check(HasError(f, "function Start()\n export local x = 1\nend", "xport"), "export only at module level");
    });

    Case("disassembly", [] {
        Fixture f = MakeFixture();
        auto p = Build(f, "local n = 0\nfunction Update() n += 1 end");
        Check(p.has_value(), "compiles");
        if (!p) return;
        std::string text = Disassemble(*p);
        Check(text.find("_update:") != std::string::npos && text.find("op_Addition") != std::string::npos, "listing");
        Check(p->ByteCode().size() == p->code.size() * 4 && p->ByteCode()[3] == static_cast<uint8_t>(p->code[0]), "big-endian bytes");
    });

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
