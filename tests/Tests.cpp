#include "UdonLuau/Compiler.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <functional>
#include <map>
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

    using Cell = std::variant<std::monostate, bool, int32_t, uint32_t, int64_t, float, double, std::string, Vec3, std::vector<int32_t>, std::vector<uint32_t>, BehaviourRef>;
    using Impl = std::function<void(std::vector<Cell>&, const std::vector<uint32_t>&)>;

    void RunOn(const Cell& behaviour, const std::string& entry);
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
        f.Extern("SystemInt32Array.__ctor__SystemInt32__SystemInt32Array", false, [](auto& h, auto& p) { h[p[1]] = std::vector<int32_t>(static_cast<size_t>(As<int32_t>(h, p[0]))); });
        f.Extern("SystemInt32Array.__Get__SystemInt32__SystemInt32", true, [](auto& h, auto& p) { h[p[2]] = As<std::vector<int32_t>>(h, p[0]).at(static_cast<size_t>(As<int32_t>(h, p[1]))); });
        f.Extern("SystemInt32Array.__Set__SystemInt32_SystemInt32__SystemVoid", true, [](auto& h, auto& p) {
            As<std::vector<int32_t>>(h, p[0]).at(static_cast<size_t>(As<int32_t>(h, p[1]))) = As<int32_t>(h, p[2]);
        });
        f.Extern("SystemInt32Array.__get_Length__SystemInt32", true, [](auto& h, auto& p) { h[p[1]] = static_cast<int32_t>(As<std::vector<int32_t>>(h, p[0]).size()); });
        f.Extern("UnityEngineVector3.__ctor__SystemSingle_SystemSingle_SystemSingle__UnityEngineVector3", false, [](auto& h, auto& p) {
            h[p[3]] = Vec3{ As<float>(h, p[0]), As<float>(h, p[1]), As<float>(h, p[2]) };
        });
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
            auto& xs = As<std::vector<int32_t>>(h, p[0]);
            auto it = std::ranges::find(xs, As<int32_t>(h, p[1]));
            h[p[2]] = it == xs.end() ? -1 : static_cast<int32_t>(it - xs.begin());
        });
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
        bool hasCustom = std::ranges::any_of(p->entryPoints, [](const EntryPoint& e) { return e.name == "Ping"; });
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
        auto p = Build(f, R"(-- @syncmode(manual)

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
        Check(p->attributes.size() == 1 && p->attributes[0].name == "syncmode" && p->attributes[0].arguments == std::vector<std::string>{ "manual" }, "module annotations");
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
        m.Run("Clamp");
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
        m.Run("Reset");
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
        m.Run("Count");
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
            m.Run("Pick");
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
export function Open()
    opened += 1
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
        Check(add != door.methods.end() && add->entryPoint == "Add" && add->parameters.size() == 2 && add->parameters[0].symbol == "__Add_a__param" &&
                  add->returns.size() == 1 && add->returns[0].symbol == "__Add__ret" && add->returns[0].type == "SystemInt32",
            "method layout");
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
end
)");
        Check(doorProgram.has_value() && callerProgram.has_value(), "both compile");
        if (!doorProgram || !callerProgram) return;
        Machine doorMachine(f, *doorProgram);
        Machine caller(f, *callerProgram);
        g_machines = { &doorMachine, &caller };
        caller.Var("door") = BehaviourRef{ 0 };
        caller.Run("_start");
        Check(std::get<int32_t>(doorMachine.Var("opened")) == 2, "Open ran locally and through the network");
        Check(std::get<int32_t>(caller.Var("result")) == 42, "arguments in, result out");
        Check(std::get<float>(doorMachine.Var("speed")) == 5.0f && std::get<float>(caller.Var("speedSeen")) == 5.0f, "fields written and read");
        Check(f.log.size() == 1 && f.log[0] == "net:0:Open", "SendCustomNetworkEvent with NetworkEventTarget.All");
        Check(HasError(f, "export local door: Door\nfunction Start()\n door:Close()\nend", "no member 'Close'"), "unknown method");
        Check(HasError(f, "export local door: Door\nfunction Start()\n Network.All(door):Add(1, 2)\nend", "cannot pass arguments"), "networked calls take no arguments");
        g_machines.clear();
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
