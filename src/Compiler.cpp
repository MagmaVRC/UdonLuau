#include "UdonLuau/Compiler.hpp"

#include "Emitter.hpp"
#include "Polyfills.hpp"
#include "TypeSystem.hpp"

#include "Luau/Ast.h"
#include "Luau/Parser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

LUAU_FASTFLAG(LuauExportValueSyntax)

namespace UdonLuau {

    namespace {

        using namespace Luau;
        using namespace Detail;

        struct CompileError {
            Location    location;
            std::string message;
        };

        [[noreturn]] void Fail(const Location& location, std::string message) {
            throw CompileError{ location, std::move(message) };
        }

        struct Variable {
            uint32_t    slot = 0;
            const Type* type = nullptr;
        };

        enum class Reentry : uint8_t { Ignore, Restart, Overlap };

        struct Function {
            std::string              name;
            AstExprFunction*         node = nullptr;
            Location                 location;
            bool                     exported = false;
            std::string              entryName;
            const EventInfo*         event = nullptr;
            std::vector<Variable>    parameters;
            std::vector<const Type*> returns;
            std::vector<uint32_t>    returnSlots;
            Label                    exportedEntry;
            Label                    entry;
            Label                    epilogue;
            std::vector<Function*>   callees;
            int                      calls = 0;
            size_t                   size = 0;
            bool                     forceInline = false;
            bool                     noInline = false;
            bool                     inlined = false;
            bool                     trampoline = true;
            bool                     delayTarget = false;
            bool                     networkCallable = false;
            int                      maxEventsPerSecond = 0;
            bool                     async = false;
            bool                     entered = false;
            bool                     cancelTarget = false;
            Reentry                  reentry = Reentry::Ignore;
            uint32_t                 freeSlot = 0;
            uint32_t                 lineSlot = 0;
            uint32_t                 cancelReturn = 0;
            Label                    cancelRoutine;
            std::vector<uint32_t>    waitFlags;

            [[nodiscard]] bool IsRoot() const { return async && (exported || entered); }
            [[nodiscard]] bool Cancellable() const { return reentry == Reentry::Restart || cancelTarget; }
        };

        constexpr size_t kInlineBudget = 40;
        constexpr size_t kJumpTableMinimum = 8;

        struct InlineFrame {
            Function*               function = nullptr;
            std::vector<Variable>   results;
            Label                   end;
            AstStat*                tail = nullptr;
        };

        class Analysis : public AstVisitor {
        public:
            std::unordered_set<AstLocal*>&                     assigned;
            std::unordered_set<AstLocal*>&                     memberAssigned;
            std::function<void(AstExpr*)>                      onCall;

            Analysis(std::unordered_set<AstLocal*>& assigned, std::unordered_set<AstLocal*>& memberAssigned, std::function<void(AstExpr*)> onCall)
                : assigned(assigned), memberAssigned(memberAssigned), onCall(std::move(onCall)) {}

            void Target(AstExpr* e) {
                while (auto* g = e->as<AstExprGroup>()) e = g->expr;
                if (auto* l = e->as<AstExprLocal>()) assigned.insert(l->local);
                AstExpr* owner = nullptr;
                if (auto* n = e->as<AstExprIndexName>()) owner = n->expr;
                if (auto* x = e->as<AstExprIndexExpr>()) owner = x->expr;
                while (owner && owner->is<AstExprGroup>()) owner = owner->as<AstExprGroup>()->expr;
                if (owner)
                    if (auto* l = owner->as<AstExprLocal>()) memberAssigned.insert(l->local);
            }

            bool visit(AstStatAssign* s) override {
                for (AstExpr* v : s->vars) Target(v);
                return true;
            }
            bool visit(AstStatCompoundAssign* s) override {
                Target(s->var);
                return true;
            }
            bool visit(AstExprCall* c) override {
                AstExpr* func = c->func;
                while (true) {
                    if (auto* g = func->as<AstExprGroup>()) func = g->expr;
                    else if (auto* i = func->as<AstExprInstantiate>()) func = i->expr;
                    else break;
                }
                onCall(func);
                if (auto* index = func->as<AstExprIndexName>(); index && !c->self && c->args.size > 0) {
                    auto* library = index->expr->as<AstExprGlobal>();
                    std::string_view member = index->index.value;
                    if (library && std::string_view(library->name.value) == "table" && (member == "insert" || member == "remove")) Target(c->args.data[0]);
                }
                if (auto* index = func->as<AstExprIndexName>(); index && c->self) {
                    AstExpr* owner = index->expr;
                    while (auto* g = owner->as<AstExprGroup>()) owner = g->expr;
                    if (auto* l = owner->as<AstExprLocal>()) memberAssigned.insert(l->local);
                }
                return true;
            }
        };

        class CallFinder : public AstVisitor {
        public:
            explicit CallFinder(const std::function<bool(AstExprCall*)>& harmless) : harmless(harmless) {}
            const std::function<bool(AstExprCall*)>& harmless;
            bool found = false;
            bool visit(AstExprCall* c) override {
                if (harmless(c)) return true;
                found = true;
                return false;
            }
        };

        AstExpr* CallTarget(AstExprCall* c) {
            AstExpr* func = c->func;
            while (true) {
                if (auto* g = func->as<AstExprGroup>()) func = g->expr;
                else if (auto* i = func->as<AstExprInstantiate>()) func = i->expr;
                else return func;
            }
        }

        bool IsPureStaticCall(AstExprCall* c) {
            static const std::set<std::string, std::less<>> types{ "Mathf", "math", "string", "Vector2", "Vector3", "Vector4", "Quaternion", "Color", "Vector2Int", "Vector3Int" };
            if (c->self) return false;
            auto* index = CallTarget(c)->as<AstExprIndexName>();
            if (!index) return false;
            auto* global = index->expr->as<AstExprGlobal>();
            return global && types.contains(global->name.value) && std::string_view(index->index.value) != "OrthoNormalize";
        }

        class PurityScan : public AstVisitor {
        public:
            explicit PurityScan(std::function<bool(AstExprCall*)> pureCall) : pureCall(std::move(pureCall)) {}
            std::function<bool(AstExprCall*)> pureCall;
            bool pure = true;

            void Target(AstExpr* e) {
                while (auto* g = e->as<AstExprGroup>()) e = g->expr;
                auto* l = e->as<AstExprLocal>();
                if (!l || l->local->functionDepth == 0) pure = false;
            }
            bool visit(AstStatAssign* s) override {
                for (AstExpr* v : s->vars) Target(v);
                return pure;
            }
            bool visit(AstStatCompoundAssign* s) override {
                Target(s->var);
                return pure;
            }
            bool visit(AstExprCall* c) override {
                if (!pureCall(c)) pure = false;
                return pure;
            }
        };

        class NodeCounter : public AstVisitor {
        public:
            size_t count = 0;
            bool visit(AstNode*) override {
                ++count;
                return true;
            }
        };

        struct DelaySpec;

        struct Value {
            enum class Kind { None, Slot, Integer, Real, Boolean, String, Nil, TypeRef, Namespace, Function, Network, Delayed };

            Kind        kind = Kind::None;
            const Type* type = nullptr;
            uint32_t    slot = 0;
            int64_t     integer = 0;
            double      real = 0.0;
            bool        boolean = false;
            std::string text;
            Function*   function = nullptr;
            const Type* targetType = nullptr;
            std::shared_ptr<const DelaySpec> delay;

            static Value OfSlot(uint32_t slot, const Type* type) {
                Value v;
                v.kind = Kind::Slot;
                v.slot = slot;
                v.type = type;
                return v;
            }
            static Value OfInteger(int64_t value, const Type* enumType = nullptr) {
                Value v;
                v.kind = Kind::Integer;
                v.integer = value;
                v.type = enumType;
                return v;
            }
            static Value OfReal(double value) {
                Value v;
                v.kind = Kind::Real;
                v.real = value;
                return v;
            }
            static Value OfBoolean(bool value) {
                Value v;
                v.kind = Kind::Boolean;
                v.boolean = value;
                return v;
            }
            static Value OfString(std::string value) {
                Value v;
                v.kind = Kind::String;
                v.text = std::move(value);
                return v;
            }
            static Value OfNil() {
                Value v;
                v.kind = Kind::Nil;
                return v;
            }
            static Value OfType(const Type* type) {
                Value v;
                v.kind = Kind::TypeRef;
                v.type = type;
                return v;
            }
            static Value OfNamespace(std::string path) {
                Value v;
                v.kind = Kind::Namespace;
                v.text = std::move(path);
                return v;
            }
            static Value OfFunction(Function* f) {
                Value v;
                v.kind = Kind::Function;
                v.function = f;
                return v;
            }

            [[nodiscard]] bool IsLiteral() const {
                return kind == Kind::Integer || kind == Kind::Real || kind == Kind::Boolean || kind == Kind::String || kind == Kind::Nil;
            }
            [[nodiscard]] bool IsNumberLiteral() const { return (kind == Kind::Integer && !type) || kind == Kind::Real; }
        };

        struct DelaySpec {
            Value                amount;
            bool                 frames = false;
            std::optional<Value> timing;
            Value                target;
        };

        struct ChangeHandler {
            std::string field;
            Variable    variable;
            std::string function;
            Location    location;
            Function*   target = nullptr;
        };

        struct DelaySite {
            std::string          entry;
            Label                label;
            Location             location;
            Value                target;
            std::string          method;
            bool                 queuedTarget = false;
            std::vector<Value>   queues;
            std::optional<Value> due;
            Function*            own = nullptr;
            const ScriptMethod*  scriptMethod = nullptr;
        };

        struct ListenSite {
            Function*             root = nullptr;
            uint32_t              waiting = 0;
            uint32_t              taken = 0;
            Label                 direct;
            std::vector<Variable> results;
        };

        struct SignalInfo {
            std::string                       name;
            std::vector<const Type*>          types;
            bool                              exported = false;
            std::vector<ListenSite*>          sites;
            std::map<Function*, uint32_t>     connections;
        };

        struct FireSite {
            SignalInfo*        signal = nullptr;
            Label              block;
            Label              back;
            std::vector<Value> values;
            Location           location;
        };

        struct EventListeners {
            const EventInfo*         event = nullptr;
            Location                 location;
            Function*                handler = nullptr;
            Label                    dispatch;
            std::vector<ListenSite*> sites;
            std::vector<Variable>    values;
        };

        struct Annotations {
            bool                        synced = false;
            SyncInterpolation           interpolation = SyncInterpolation::None;
            std::vector<FieldAttribute> attributes;
        };

        std::vector<std::string> SplitArguments(std::string_view text) {
            std::vector<std::string> out;
            std::string current;
            char quote = 0;
            bool quoted = false;
            auto flush = [&] {
                size_t b = current.find_first_not_of(" \t");
                size_t e = current.find_last_not_of(" \t");
                std::string item = b == std::string::npos ? std::string() : current.substr(b, e - b + 1);
                if (!item.empty() || quoted) out.push_back(std::move(item));
                current.clear();
                quoted = false;
            };
            for (size_t i = 0; i < text.size(); ++i) {
                char c = text[i];
                if (quote) {
                    if (c == '\\' && i + 1 < text.size()) current += text[++i];
                    else if (c == quote) quote = 0;
                    else current += c;
                } else if (c == '"' || c == '\'') {
                    quote = c;
                    quoted = true;
                } else if (c == ',') {
                    flush();
                } else {
                    current += c;
                }
            }
            flush();
            return out;
        }

        std::vector<FieldAttribute> ParseAttributes(std::string_view text) {
            std::vector<FieldAttribute> out;
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] != '@' || (i > 0 && !std::isspace(static_cast<unsigned char>(text[i - 1])))) continue;
                size_t start = ++i;
                while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_' || text[i] == '.')) ++i;
                if (i == start) continue;
                FieldAttribute attribute{ std::string(text.substr(start, i - start)), {} };
                size_t open = i;
                while (open < text.size() && (text[open] == ' ' || text[open] == '\t')) ++open;
                if (open < text.size() && text[open] == '(') {
                    char quote = 0;
                    size_t close = open + 1;
                    for (; close < text.size(); ++close) {
                        char c = text[close];
                        if (quote) {
                            if (c == '\\') ++close;
                            else if (c == quote) quote = 0;
                        } else if (c == '"' || c == '\'') {
                            quote = c;
                        } else if (c == ')') {
                            break;
                        }
                    }
                    attribute.arguments = SplitArguments(text.substr(open + 1, close - open - 1));
                    i = close;
                }
                out.push_back(std::move(attribute));
            }
            return out;
        }

        struct LoopLabels {
            Label exit;
            Label next;
        };

        int NumericRank(Numeric n) {
            switch (n) {
                case Numeric::SByte: return 1;
                case Numeric::Byte: return 2;
                case Numeric::Int16: return 3;
                case Numeric::UInt16:
                case Numeric::Char: return 4;
                case Numeric::Int32: return 5;
                case Numeric::UInt32: return 6;
                case Numeric::Int64: return 7;
                case Numeric::UInt64: return 8;
                case Numeric::Single: return 9;
                case Numeric::Double: return 10;
                default: return 0;
            }
        }

        bool FitsIntegral(int64_t v, Numeric n) {
            switch (n) {
                case Numeric::SByte: return v >= INT8_MIN && v <= INT8_MAX;
                case Numeric::Byte: return v >= 0 && v <= UINT8_MAX;
                case Numeric::Int16: return v >= INT16_MIN && v <= INT16_MAX;
                case Numeric::UInt16:
                case Numeric::Char: return v >= 0 && v <= UINT16_MAX;
                case Numeric::Int32: return v >= INT32_MIN && v <= INT32_MAX;
                case Numeric::UInt32: return v >= 0 && v <= UINT32_MAX;
                case Numeric::Int64: return true;
                case Numeric::UInt64: return v >= 0;
                default: return false;
            }
        }

        bool IsUnsigned(Numeric n) {
            return n == Numeric::Byte || n == Numeric::UInt16 || n == Numeric::UInt32 || n == Numeric::UInt64;
        }

        std::string LowerFirst(std::string s) {
            if (!s.empty()) s[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
            return s;
        }

        std::string UpperFirst(std::string s) {
            if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
            return s;
        }

        std::string_view OperatorMethod(AstExprBinary::Op op) {
            switch (op) {
                case AstExprBinary::Add: return "op_Addition";
                case AstExprBinary::Sub: return "op_Subtraction";
                case AstExprBinary::Mul: return "op_Multiply";
                case AstExprBinary::Div:
                case AstExprBinary::FloorDiv: return "op_Division";
                case AstExprBinary::Mod: return "op_Modulus";
                case AstExprBinary::CompareEq: return "op_Equality";
                case AstExprBinary::CompareNe: return "op_Inequality";
                case AstExprBinary::CompareLt: return "op_LessThan";
                case AstExprBinary::CompareLe: return "op_LessThanOrEqual";
                case AstExprBinary::CompareGt: return "op_GreaterThan";
                case AstExprBinary::CompareGe: return "op_GreaterThanOrEqual";
                default: return {};
            }
        }

        AstExprBinary::Op NegateComparison(AstExprBinary::Op op) {
            switch (op) {
                case AstExprBinary::CompareEq: return AstExprBinary::CompareNe;
                case AstExprBinary::CompareNe: return AstExprBinary::CompareEq;
                case AstExprBinary::CompareLt: return AstExprBinary::CompareGe;
                case AstExprBinary::CompareLe: return AstExprBinary::CompareGt;
                case AstExprBinary::CompareGt: return AstExprBinary::CompareLe;
                case AstExprBinary::CompareGe: return AstExprBinary::CompareLt;
                default: return op;
            }
        }

        bool IsConstantGetter(const ExternInfo& e) {
            static const std::set<std::string, std::less<>> vectors{ "get_zero", "get_one", "get_up", "get_down", "get_left", "get_right", "get_forward", "get_back",
                                                                     "get_positiveInfinity", "get_negativeInfinity" };
            static const std::set<std::string, std::less<>> colors{ "get_red", "get_green", "get_blue", "get_white", "get_black", "get_yellow", "get_cyan",
                                                                    "get_magenta", "get_gray", "get_grey", "get_clear" };
            static const std::set<std::string, std::less<>> vectorTypes{ "UnityEngineVector2", "UnityEngineVector3", "UnityEngineVector4", "UnityEngineVector2Int",
                                                                         "UnityEngineVector3Int" };
            if (!e.isStatic || !e.parameters.empty() || !e.method.starts_with("get_") || e.returnType != e.module) return false;
            if (vectorTypes.contains(e.module)) return vectors.contains(e.method);
            if (e.module == "UnityEngineColor") return colors.contains(e.method);
            return e.module == "UnityEngineQuaternion" && e.method == "get_identity";
        }

        bool IsFoldableCall(const ExternInfo& e) {
            static const std::set<std::string, std::less<>> math{ "Abs", "Max", "Min", "Clamp", "Clamp01", "Sqrt", "Sin", "Cos", "Tan", "Asin", "Acos", "Atan", "Atan2", "Pow",
                                                                  "Exp", "Log", "Log10", "Floor", "Ceil", "Round", "FloorToInt", "CeilToInt", "RoundToInt", "Sign", "Lerp",
                                                                  "LerpUnclamped", "InverseLerp", "Repeat", "PingPong", "DeltaAngle", "MoveTowards", "SmoothStep",
                                                                  "ClosestPowerOfTwo", "IsPowerOfTwo", "NextPowerOfTwo", "Truncate" };
            if (!e.isStatic || e.isGeneric) return false;
            if (e.module == "UnityEngineMathf" || e.module == "SystemMath") return math.contains(e.method);
            return e.module == "UnityEngineQuaternion" && e.method == "Euler";
        }

        bool HasNoSideEffects(const ExternInfo& e, bool hasReceiver) {
            static const std::set<std::string, std::less<>> modules{ "UnityEngineMathf", "SystemMath", "SystemConvert", "UnityEngineVector2", "UnityEngineVector3",
                                                                     "UnityEngineVector4", "UnityEngineQuaternion", "UnityEngineColor", "UnityEngineVector2Int",
                                                                     "UnityEngineVector3Int" };
            if (e.method.starts_with("get_") || e.method.starts_with("op_")) return true;
            return !hasReceiver && !e.isGeneric && modules.contains(e.module) && e.method != "OrthoNormalize";
        }

        bool IsStableRead(const ExternInfo& e, const Type* receiver) {
            static const std::set<std::string, std::less<>> transform{ "get_position", "get_rotation", "get_localPosition", "get_localRotation", "get_localScale",
                                                                       "get_eulerAngles", "get_localEulerAngles", "get_forward", "get_right", "get_up",
                                                                       "get_lossyScale", "get_parent", "get_root", "get_childCount", "get_transform",
                                                                       "get_gameObject" };
            if (!receiver || !e.method.starts_with("get_") || !e.parameters.empty()) return false;
            if (receiver->IsValueType()) return true;
            return (e.module == "UnityEngineTransform" || e.module == "UnityEngineGameObject" || e.module == "UnityEngineComponent") && transform.contains(e.method);
        }

        std::optional<double> MathfConstant(std::string_view name) {
            constexpr float pi = 3.14159265358979f;
            constexpr float deg2rad = pi * 2.0f / 360.0f;
            if (name == "PI") return pi;
            if (name == "Deg2Rad") return deg2rad;
            if (name == "Rad2Deg") return 1.0f / deg2rad;
            if (name == "Infinity") return std::numeric_limits<float>::infinity();
            if (name == "NegativeInfinity") return -std::numeric_limits<float>::infinity();
            return std::nullopt;
        }

        std::vector<std::string> SplitChars(std::string_view text) {
            std::vector<std::string> chars;
            for (size_t i = 0; i < text.size();) {
                auto lead = static_cast<unsigned char>(text[i]);
                size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
                chars.emplace_back(text.substr(i, length));
                i += length;
            }
            return chars;
        }

        bool IsSingleChar(std::string_view text) {
            std::vector<std::string> chars = SplitChars(text);
            return chars.size() == 1 && chars[0].size() < 4;
        }

        bool IsComparison(AstExprBinary::Op op) {
            return op == AstExprBinary::CompareEq || op == AstExprBinary::CompareNe || op == AstExprBinary::CompareLt ||
                   op == AstExprBinary::CompareLe || op == AstExprBinary::CompareGt || op == AstExprBinary::CompareGe;
        }

        AstExpr* Unwrap(AstExpr* e) {
            while (auto* g = e->as<AstExprGroup>()) e = g->expr;
            return e;
        }

        class Compiler {
        public:
            Compiler(const Catalog& catalog, std::string_view source, const CompileOptions& options, bool interfaceOnly)
                : catalog_(catalog), types_(catalog), source_(source), options_(options), interfaceOnly_(interfaceOnly) {}

            CompileResult Run();

        private:
            void IndexSource(const std::vector<Comment>& comments);
            Annotations AnnotationsFor(const Location& location) const;
            std::vector<FieldAttribute> ModuleAttributes(AstStatBlock* root) const;
            std::string_view TextAt(const Location& location) const;
            void Report(const Location& location, std::string message, Severity severity = Severity::Error);

            void DeclareModule(AstStatBlock* root);
            void DeclareFields(AstStatLocal* stat, bool aliasesOnly = false);
            void DeclareFunction(std::string name, AstExprFunction* node, const Location& location, AstLocal* local);
            void CheckUserName(std::string_view name, const Location& location);
            void CompileFunction(Function& f);
            void CheckRecursion();

            void CompileBlock(AstStatBlock* block);
            void CompileStatementGuarded(AstStat* stat);
            void CompileStatement(AstStat* stat);
            void CompileLocal(AstStatLocal* stat);
            void CompileAssign(AstStatAssign* stat);
            void CompileCompoundAssign(AstStatCompoundAssign* stat);
            void CompileIf(AstStatIf* stat);
            void CompileWhile(AstStatWhile* stat);
            void CompileRepeat(AstStatRepeat* stat);
            void CompileFor(AstStatFor* stat);
            void CompileForIn(AstStatForIn* stat);
            void CompileReturn(AstStatReturn* stat);
            void CompileLoopBody(AstStatBlock* body, LoopLabels labels);

            std::vector<Value> EvaluateList(const AstArray<AstExpr*>& exprs, size_t want, const std::vector<const Type*>& expected);
            void AssignTo(AstExpr* target, const Value& value);

            Value CompileExpr(AstExpr* expr, const Type* expected = nullptr, const Variable* into = nullptr);
            Value CompileConstantNumber(AstExprConstantNumber* expr);
            Value CompileGlobal(AstExprGlobal* expr);
            Value CompileIndexName(AstExprIndexName* expr);
            Value CompileIndexExpr(AstExprIndexExpr* expr);
            Value ReadMember(const Value& owner, std::string_view name, const Location& location);
            void WriteMember(const Value& owner, std::string_view name, const Value& value, const Location& location);
            Value ReadIndex(const Value& owner, const Value& key, const Location& location);
            void WriteIndex(const Value& owner, const Value& key, const Value& value, const Location& location);
            Value CompileUnary(AstExprUnary* expr);
            Value CompileBinary(AstExprBinary* expr, const Type* expected);
            Value CompileArithmetic(AstExprBinary::Op op, Value left, Value right, const Location& location);
            Value CompileComparison(AstExprBinary::Op op, AstExpr* left, AstExpr* right, const Location& location);
            Value CompareValues(AstExprBinary::Op op, const Value& left, const Value& right, const Location& location);
            Value CompileConcat(AstExpr* expr);
            Value CompileInterpolated(AstExprInterpString* expr);
            Value CompileIfElse(AstExprIfElse* expr, const Type* expected, const Variable* into);
            Value CompileTable(AstExprTable* expr, const Type* expected);
            Value BooleanFromCondition(AstExpr* expr, const Variable* into = nullptr);
            Value ConcatValues(std::vector<Value> parts, const Location& location);
            Value ToStringValue(const Value& value, const Location& location);
            std::optional<Value> FoldArithmetic(AstExprBinary::Op op, const Value& left, const Value& right);

            void CompileCondition(AstExpr* expr, Label whenFalse, bool negate = false);
            uint32_t TruthySlot(const Value& value, const Location& location);
            uint32_t NotSlot(uint32_t slot, const Location& location);

            std::vector<Value> CompileCall(AstExprCall* call, size_t want, const Variable* into = nullptr);
            std::vector<Value> CallFunction(Function* f, std::vector<Value> args, size_t want, const Location& location, const Variable* into = nullptr);
            std::vector<Value> CallBuiltin(std::string_view name, std::vector<Value> args, const Location& location, bool& handled);
            std::vector<Value> CallMember(const Value& receiver, std::string_view name, std::vector<Value> args, const Type* typeArg, const Location& location);
            std::vector<Value> CallStatic(const Type* owner, std::string_view name, std::vector<Value> args, const Type* typeArg, const Location& location);

            struct Resolution {
                const Method* method = nullptr;
                const Type*   typeArgument = nullptr;
            };
            std::optional<Resolution> TryResolve(const std::vector<const Method*>& methods, const std::vector<Value>& args, const Type* typeArg, bool& ambiguous);
            Resolution Resolve(const std::vector<const Method*>& methods, const std::vector<Value>& args, const Type* typeArg, const Location& location, std::string_view what);
            std::vector<Value> Invoke(const Resolution& r, const std::optional<Value>& receiver, const std::vector<Value>& args, const Location& location);
            std::vector<Value> InvokeOperator(std::string_view opName, const std::vector<const Type*>& owners, std::vector<Value> args, const Location& location);
            std::vector<const Method*> StaticMethods(const std::vector<const Type*>& owners, std::string_view name);
            std::vector<const Method*> MembersWithRenames(const Type* owner, std::string_view name, bool wantStatic);

            int Cost(const Value& value, const Type* to) const;
            const Type* NaturalType(const Value& value) const;
            const Type* InputType(const Parameter& p, const Type* typeArg);
            std::string Describe(const Value& value) const;
            std::string Describe(const Method& m) const;

            uint32_t Materialize(const Value& value, const Type* to, const Location& location);
            uint32_t Convert(uint32_t slot, const Type* from, const Type* to, const Location& location, std::optional<uint32_t> destination = std::nullopt);
            HeapValue Literal(const Value& value, const Type* type, const Location& location) const;
            void Store(const Value& value, const Variable& destination, const Location& location);
            Value Evaluate(const Value& value);
            Function* Callee(AstExpr* func) const;
            const ScriptMethod* PickScriptMethod(const ScriptInfo* script, std::string_view name, const std::vector<Value>& args, const Location& location);
            struct ListInit {
                bool                   ok = false;
                std::vector<AstExpr*>  items;
                AstExpr*               capacity = nullptr;
            };
            ListInit ParseListInit(AstExpr* init) const;
            Variable DeclareListStorage(const std::string& name, const Type* type, bool field, HeapValue array, int64_t count);
            void InitList(const Value& list, AstExpr* init, const Location& location);
            Value ListArray(const Value& list) const { return Value::OfSlot(list.slot, list.type->listArray); }
            Value Narrow(Value item, const Type* element) const {
                if (item.kind == Value::Kind::Slot && element && item.type != element && types_.ReferenceDistance(element, item.type) >= 0) item.type = element;
                return item;
            }
            Variable ListCount(const Value& list) const { return { list.slot + 1, types_.Int32 }; }
            Variable ListCapacity(const Value& list) const { return { list.slot + 2, types_.Int32 }; }
            void GrowList(const Value& list, const Value& needed, const Location& location);
            void CheckListIndex(const Value& list, const Value& index, const Location& location);
            Value ListIndexOf(const Value& list, const Value& item, const Location& location);
            void ListRemoveAt(const Value& list, const Value& index, const Location& location);
            std::vector<Value> CallList(const Value& list, std::string_view name, std::vector<Value> args, const Location& location);
            std::vector<Value> TableOnList(std::string_view name, std::vector<Value> args, const Location& location);
            const ScriptMethod* NetworkMethod(const Value& receiver, std::string_view name, size_t argc, const Location& location);
            std::vector<Value> SendNetwork(const Value& receiver, std::string_view name, std::vector<Value> args, const Location& location);
            std::vector<Value> CallDelay(std::string_view name, std::vector<Value> args, const Location& location);
            std::vector<Value> CallDelayed(const Value& receiver, std::string_view name, std::vector<Value> args, const Location& location);
            void CompileDelaySite(const DelaySite& site);
            void EmitReturn();
            void EmitYield();
            [[nodiscard]] bool DebugBuild() const;
            void AnalyzeCoroutines(AstStatBlock* root);
            std::vector<Value> CallTask(AstExprCall* call, std::string_view name, size_t want);
            std::vector<Value> EmitWait(bool frames, const Value& amount, const Value& timing, size_t want, const Location& location);
            void EnterRoot(Function* f, std::vector<Value> args, const Location& location);
            void CallCancel(Function& f);
            void EmitRootPrologue(Function& f, Label reject);
            void EmitRootReject(Function& f, Label reject);
            void EmitCancelRoutine(Function& f);
            void EmitAliveCheck();
            bool DeclareSignal(AstLocal* var, AstExpr* init, bool exported);
            SignalInfo* SignalOf(AstExpr* expr);
            const EventInfo* EventOf(AstExpr* expr) const;
            std::optional<std::vector<Value>> CallListenable(AstExprCall* call, size_t want);
            std::vector<Value> EmitListen(const std::vector<const Type*>& types, std::vector<ListenSite*>& registry, size_t want, const Location& location);
            void EmitDispatch(const std::vector<ListenSite*>& sites, const std::vector<Value>& values, const std::map<Function*, uint32_t>& connections, const Location& location);
            void EmitFireBlock(const FireSite& fire);
            void EmitEventDispatch(EventListeners& listeners);
            void DeclareConstTable(AstLocal* var, AstExprTable* table);
            void SortWith(const Value& array, const Value& length, const Value& comparer, const Location& location);
            Value FindBehaviour(const Value& receiver, std::string_view getter, const Type* wanted, const Location& location);
            void RecordChangeHandler(const Annotations& annotations, const std::string& field, const Variable& variable, const Location& location);
            void CompileChangeHandler(const ChangeHandler& handler);
            void BeginSyntheticEntry(const std::string& name);
            void FindSingletonUses(AstStatBlock* root);
            Value StaticCompanion(const Location& location);
            void CheckInstanceUse(AstLocal* local, const Location& location) const;
            void EmitEntryPrologue(const Function& f);
            void EmitSingletonResolver();
            std::optional<std::string_view> LibraryName(AstExpr* expr);
            std::vector<Value> CallLibrary(std::string_view library, std::string_view name, AstExprCall* call, std::vector<Value> args, const Type* typeArg, const Variable* into);
            std::vector<Value> CallMath(std::string_view name, std::vector<Value> args, const Location& location);
            std::vector<Value> CallString(std::string_view name, std::vector<Value> args, const Location& location);
            std::vector<Value> CallBit32(std::string_view name, std::vector<Value> args, const Location& location);
            std::vector<Value> CallUtf8(std::string_view name, std::vector<Value> args, const Location& location);
            std::vector<Value> CallTable(std::string_view name, AstExprCall* call, std::vector<Value> args, const Type* typeArg, const Variable* into);
            std::string TranslateFormat(std::string_view format, size_t count, const Location& location);
            Value ArrayArgument(const std::vector<Value>& args, size_t index, std::string_view function, const Location& location);
            Value NewArray(const Type* arrayType, const Value& length, const Location& location);
            void CopyRange(const Value& source, const Value& sourceIndex, const Value& destination, const Value& destinationIndex, const Value& length, const Location& location);
            bool CannotWrite(AstExprCall* call) const;
            bool HasCall(AstExpr* expr) const;

            const Type* ResolveType(AstType* type);
            const Type* ResolveTypeName(std::string_view name, const Location& location);
            const Type* PreferredType(const std::vector<const Type*>& matches) const;
            const Type* ShortType(std::string_view name, const Location& location);
            void DeclareTypeAlias(AstStatTypeAlias* alias);
            Value SelfValue(std::string_view name);
            Variable LookupLocal(AstLocal* local, const Location& location);

            const Type* VariableType(const ScriptVariable& v);
            ScriptVariable MakeVariable(std::string name, const Type* type, std::string symbol);
            ScriptInfo BuildInterface();
            std::vector<Value> CallScriptMethod(const Value& receiver, const ScriptMethod& method, const std::vector<Value>& args, size_t want, const Location& location);
            std::optional<Value> TryNetworkTarget(AstExprCall* call);
            Value ReadScriptField(const Value& receiver, const ScriptVariable& field, const Location& location);

            void Analyze(AstStatBlock* root);
            void DeclareDefines(AstStatBlock* root);
            void DeclareSyncMode(AstStatBlock* root);
            void CheckSynced(const std::string& name, const Type* type, SyncInterpolation interpolation, const Location& location);
            std::optional<Value> TryConstant(AstExpr* expr);
            bool IsTruthy(const Value& value) const { return value.kind != Value::Kind::Nil && !(value.kind == Value::Kind::Boolean && !value.boolean); }
            bool NegationIsFree(AstExpr* expr);
            void CompileBody(AstStatBlock* body, AstStat*& tail);
            std::vector<Value> Inline(Function* f, std::vector<Value> args, size_t want, const Location& location, const Variable* into);
            bool TryJumpTable(AstStatIf* stat);
            void CompileAssert(AstExprCall* call);
            Value BindLocal(AstLocal* var, const Value& value, const Type* annotated, bool isConst, bool hasValue);
            Value CoerceLiteral(const Value& value, const Type* type, const Location& location);
            std::string ExportId(const std::string& id) {
                int& count = exportIds_[id];
                return std::format("__{}_{}", count++, id);
            }
            std::unordered_map<std::string, int> exportIds_;

            const Catalog&                                 catalog_;
            TypeTable                                      types_;
            Emitter                                        emit_;
            std::string_view                               source_;
            const CompileOptions&                          options_;
            std::unordered_map<std::string, Value>         defines_;
            std::unordered_set<AstLocal*>                  assigned_;
            std::unordered_set<const Function*>            pure_;
            std::vector<DelaySite>                         delaySites_;
            std::unordered_set<AstLocal*>                  memberAssigned_;
            bool Immutable(AstLocal* local, const Type* type) const {
                return !assigned_.contains(local) && (!memberAssigned_.contains(local) || (type && type->IsReference()));
            }
            std::unordered_set<uint32_t>                   fieldSlots_;
            std::vector<InlineFrame>                       inlineStack_;
            std::vector<std::pair<std::string, const Type*>> exportedFields_;
            bool                                           interfaceOnly_ = false;
            BehaviourSyncMode                              syncMode_ = BehaviourSyncMode::Any;
            struct SingletonUse {
                const ScriptInfo* script = nullptr;
                uint32_t          slot = 0;
                bool              methods = false;
            };
            bool                                           singleton_ = false;
            bool                                           hasStatics_ = false;
            uint32_t                                       eventReturnSlot_ = 0;
            std::vector<ChangeHandler>                     changeHandlers_;
            std::unordered_map<AstLocal*, std::map<std::string, Value, std::less<>>> constTables_;
            std::set<std::string, std::less<>>             entryNames_;
            std::unordered_map<AstLocal*, std::string>     staticFields_;
            std::unordered_map<AstLocal*, std::string>     staticFunctions_;
            std::unordered_set<AstLocal*>                  instanceLocals_;
            bool                                           singletonStarted_ = false;
            uint32_t                                       startedSlot_ = 0;
            std::map<std::string, SingletonUse, std::less<>> singletons_;
            uint32_t                                       singletonsReadySlot_ = 0;
            uint32_t                                       resolveReturnSlot_ = 0;
            Label                                          resolveRoutine_;
            std::vector<std::pair<Label, Label>>           resolveStubs_;
            AstStat*                                       tail_ = nullptr;
            std::vector<size_t>                            lineStarts_;
            std::map<unsigned, std::vector<std::pair<FieldAttribute, Location>>> commentAttributes_;
            std::set<unsigned>                             commentLines_;
            std::unordered_map<AstLocal*, Value>           aliases_;
            std::unordered_map<std::string, const Type*>   typeAliases_;
            std::vector<Diagnostic>                        diagnostics_;

            std::unordered_map<AstLocal*, Variable>        variables_;
            std::unordered_map<AstLocal*, Function*>       localFunctions_;
            std::unordered_map<std::string, Function*>     globalFunctions_;
            std::vector<std::unique_ptr<Function>>         functions_;
            std::unordered_set<std::string>                userSymbols_;
            std::unordered_map<std::string, uint32_t>      selfSlots_;
            std::vector<SyncVariable>                      sync_;
            std::vector<EntryPoint>                        entries_;
            std::vector<LoopLabels>                        loops_;
            Function*                                      current_ = nullptr;
            Function*                                      root_ = nullptr;
            int                                            waitSites_ = 0;
            std::deque<ListenSite>                         listenSites_;
            std::unordered_map<AstLocal*, SignalInfo>      signals_;
            std::deque<FireSite>                           fireSites_;
            std::map<std::string, EventListeners, std::less<>> eventListeners_;
            uint32_t                                       haltSlot_ = 0;
            uint32_t                                       returnJumpSlot_ = 0;
        };

        CompileResult Compiler::Run() {
            static std::once_flag exportSyntax;
            std::call_once(exportSyntax, [] { FFlag::LuauExportValueSyntax.value = true; });
            Allocator allocator;
            AstNameTable names(allocator);
            ParseOptions options;
            options.captureComments = true;
            ParseResult parsed = Parser::parse(source_.data(), source_.size(), names, allocator, options);

            CompileResult result;
            for (const ParseError& e : parsed.errors) Report(e.getLocation(), e.getMessage());
            if (!parsed.errors.empty()) {
                result.diagnostics = std::move(diagnostics_);
                return result;
            }

            IndexSource(parsed.commentLocations);
            DeclareDefines(parsed.root);
            try {
                DeclareSyncMode(parsed.root);
            } catch (const CompileError& e) {
                Report(e.location, e.message);
            }

            HeapValue halt;
            halt.kind = ValueKind::Unsigned;
            halt.unsignedInteger = 0xFFFFFFFFu;
            haltSlot_ = emit_.Constant(types_.UInt32, halt);
            returnJumpSlot_ = emit_.AddSlot("__intnl_returnJump_SystemUInt32_0", types_.UInt32);
            singleton_ = options_.staticPart || std::ranges::any_of(ModuleAttributes(parsed.root), [](const FieldAttribute& a) { return a.name == "singleton"; });
            if (singleton_) {
                HeapValue no;
                no.kind = ValueKind::Boolean;
                startedSlot_ = emit_.AddSlot("__started", types_.Boolean, no, true);
                emit_.Slot(startedSlot_).attributes.push_back({ "hideininspector", {} });
            }

            DeclareModule(parsed.root);
            bool declared = std::ranges::none_of(diagnostics_, [](const Diagnostic& d) { return d.severity == Severity::Error; });
            if (declared) result.scriptInterface = BuildInterface();
            result.hasStatics = hasStatics_;
            if (interfaceOnly_) {
                result.diagnostics = std::move(diagnostics_);
                return result;
            }
            try {
                FindSingletonUses(parsed.root);
            } catch (const CompileError& e) {
                Report(e.location, e.message);
            }
            Analyze(parsed.root);
            for (auto& f : functions_) {
                try {
                    CompileFunction(*f);
                } catch (const CompileError& e) {
                    Report(e.location, e.message);
                }
            }
            auto guarded = [&](auto&& compile) {
                try {
                    compile();
                } catch (const CompileError& e) {
                    Report(e.location, e.message);
                }
            };
            for (const ChangeHandler& handler : changeHandlers_) guarded([&] { CompileChangeHandler(handler); });
            for (auto& [name, listeners] : eventListeners_) guarded([&] { EmitEventDispatch(listeners); });
            for (size_t delays = 0, fires = 0; delays < delaySites_.size() || fires < fireSites_.size();) {
                if (delays < delaySites_.size()) {
                    guarded([&] { CompileDelaySite(DelaySite(delaySites_[delays])); });
                    ++delays;
                } else {
                    guarded([&] { EmitFireBlock(fireSites_[fires]); });
                    ++fires;
                }
            }
            if (singleton_ && !singletonStarted_) {
                Label start = emit_.NewLabel();
                emit_.Bind(start);
                entries_.push_back({ "_start", *emit_.AddressOf(start) });
                if (options_.compatibleExitReturn) emit_.Push(haltSlot_);
                HeapValue yes;
                yes.kind = ValueKind::Boolean;
                yes.boolean = true;
                emit_.Copy(emit_.Constant(types_.Boolean, yes), startedSlot_);
                EmitReturn();
            }
            try {
                EmitSingletonResolver();
            } catch (const CompileError& e) {
                Report(e.location, e.message);
            }
            CheckRecursion();

            bool failed = std::ranges::any_of(diagnostics_, [](const Diagnostic& d) { return d.severity == Severity::Error; });
            if (!failed) {
                result.program = emit_.Finish(std::move(entries_), std::move(sync_));
                result.program->attributes = ModuleAttributes(parsed.root);
                result.program->syncMode = syncMode_;
                if (singleton_) result.program->updateOrder = std::numeric_limits<int>::min() / 2;
                for (const auto& f : functions_) {
                    if (!f->networkCallable) continue;
                    NetworkCallable n{ f->entryName, f->maxEventsPerSecond, {} };
                    for (const Variable& p : f->parameters) n.parameters.push_back({ result.program->heap[p.slot].symbol, p.type->udonName });
                    result.program->networkCallables.push_back(std::move(n));
                }
            }
            result.diagnostics = std::move(diagnostics_);
            return result;
        }

        void Compiler::Report(const Location& location, std::string message, Severity severity) {
            for (const Diagnostic& existing : diagnostics_)
                if (existing.message == message && existing.line == static_cast<int>(location.begin.line) && existing.column == static_cast<int>(location.begin.column)) return;
            Diagnostic d;
            d.severity = severity;
            d.message = std::move(message);
            d.line = static_cast<int>(location.begin.line);
            d.column = static_cast<int>(location.begin.column);
            d.endLine = static_cast<int>(location.end.line);
            d.endColumn = static_cast<int>(location.end.column);
            diagnostics_.push_back(std::move(d));
        }

        void Compiler::IndexSource(const std::vector<Comment>& comments) {
            lineStarts_.push_back(0);
            for (size_t i = 0; i < source_.size(); ++i)
                if (source_[i] == '\n') lineStarts_.push_back(i + 1);

            for (const Comment& c : comments) {
                if (c.type != Lexeme::Comment) continue;
                commentLines_.insert(c.location.begin.line);
                std::string_view text = TextAt(c.location);
                while (text.starts_with('-')) text.remove_prefix(1);
                auto& attributes = commentAttributes_[c.location.begin.line];
                for (FieldAttribute& a : ParseAttributes(text)) attributes.push_back({ std::move(a), c.location });
            }
        }

        std::string_view Compiler::TextAt(const Location& location) const {
            auto offset = [&](const Position& p) {
                size_t line = std::min<size_t>(p.line, lineStarts_.size() - 1);
                return std::min(source_.size(), lineStarts_[line] + p.column);
            };
            size_t begin = offset(location.begin);
            size_t end = offset(location.end);
            return end > begin ? source_.substr(begin, end - begin) : std::string_view{};
        }

        Annotations Compiler::AnnotationsFor(const Location& location) const {
            std::vector<const std::pair<FieldAttribute, Location>*> found;
            for (unsigned line = location.begin.line; line > 0 && commentLines_.contains(line - 1);) {
                auto it = commentAttributes_.find(--line);
                if (it == commentAttributes_.end()) continue;
                for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) found.insert(found.begin(), &*rit);
            }
            if (auto it = commentAttributes_.find(location.begin.line); it != commentAttributes_.end())
                for (const auto& entry : it->second) found.push_back(&entry);

            Annotations a;
            for (const auto* entry : found) {
                const FieldAttribute& attribute = entry->first;
                if (attribute.name != "sync") {
                    a.attributes.push_back(attribute);
                    continue;
                }
                a.synced = true;
                std::string_view mode = attribute.arguments.empty() ? "none" : std::string_view(attribute.arguments[0]);
                if (mode == "linear") a.interpolation = SyncInterpolation::Linear;
                else if (mode == "smooth") a.interpolation = SyncInterpolation::Smooth;
                else if (mode != "none") Fail(entry->second, std::format("unknown sync mode '{}'; use none, linear or smooth", mode));
            }
            return a;
        }

        std::vector<FieldAttribute> Compiler::ModuleAttributes(AstStatBlock* root) const {
            unsigned first = root->body.size ? root->body.data[0]->location.begin.line : std::numeric_limits<unsigned>::max();
            unsigned attached = first;
            while (attached > 0 && commentLines_.contains(attached - 1)) --attached;
            if (!root->body.size) attached = first;

            static const std::set<std::string, std::less<>> moduleOnly{ "singleton", "syncmode", "define" };
            std::vector<FieldAttribute> out;
            for (const auto& [line, entries] : commentAttributes_) {
                if (line >= first) break;
                for (const auto& entry : entries)
                    if (line < attached || moduleOnly.contains(entry.first.name)) out.push_back(entry.first);
            }
            return out;
        }

        const Type* Compiler::PreferredType(const std::vector<const Type*>& matches) const {
            for (const std::string& space : catalog_.PreferredNamespaces()) {
                const Type* found = nullptr;
                int count = 0;
                for (const Type* t : matches) {
                    std::string_view full = t->fullName;
                    size_t dot = full.rfind('.');
                    if ((dot == std::string_view::npos ? std::string_view{} : full.substr(0, dot)) != space) continue;
                    found = t;
                    ++count;
                }
                if (count == 1) return found;
            }
            return nullptr;
        }

        const Type* Compiler::ShortType(std::string_view name, const Location& location) {
            std::vector<const Type*> matches = types_.FindByShortName(name);
            if (matches.size() == 1) return matches[0];
            if (matches.empty()) return nullptr;
            if (const Type* preferred = PreferredType(matches)) return preferred;
            std::string list;
            for (const Type* t : matches) list += (list.empty() ? "" : ", ") + t->fullName;
            Fail(location, std::format("'{}' is ambiguous: {}; qualify it or add an alias such as 'type {} = Namespace.{}'", name, list, name, name));
        }

        void Compiler::DeclareTypeAlias(AstStatTypeAlias* alias) {
            if (alias->generics.size || alias->genericPacks.size) Fail(alias->location, "generic type aliases are not supported");
            typeAliases_[alias->name.value] = ResolveType(alias->type);
        }

        const Type* Compiler::VariableType(const ScriptVariable& v) {
            if (v.script.empty()) return types_.Get(v.type);
            const ScriptInfo* script = catalog_.FindScript(v.script);
            const Type* element = script ? types_.Script(script) : types_.Behaviour();
            return v.type.ends_with("Array") ? types_.ArrayOf(element) : element;
        }

        ScriptVariable Compiler::MakeVariable(std::string name, const Type* type, std::string symbol) {
            ScriptVariable v;
            v.name = std::move(name);
            v.symbol = std::move(symbol);
            v.type = type->udonName;
            const Type* element = type->kind == TypeKind::Array ? type->element : type;
            if (element && element->script) v.script = element->script->name;
            return v;
        }

        ScriptInfo Compiler::BuildInterface() {
            ScriptInfo info;
            info.singleton = singleton_;
            for (const auto& [name, type] : exportedFields_) info.fields.push_back(MakeVariable(name, type, name));
            for (const auto& f : functions_) {
                if (!f->exported || f->event) continue;
                ScriptMethod m;
                m.name = f->name;
                m.entryPoint = f->entryName;
                m.networkCallable = f->networkCallable;
                for (size_t i = 0; i < f->parameters.size(); ++i)
                    m.parameters.push_back(MakeVariable(f->node->args.data[i]->name.value, f->parameters[i].type, emit_.Slot(f->parameters[i].slot).symbol));
                for (size_t i = 0; i < f->returns.size(); ++i)
                    m.returns.push_back(MakeVariable(i ? std::format("result{}", i) : "result", f->returns[i], emit_.Slot(f->returnSlots[i]).symbol));
                info.methods.push_back(std::move(m));
            }
            return info;
        }

        const ScriptMethod* Compiler::PickScriptMethod(const ScriptInfo* script, std::string_view name, const std::vector<Value>& args, const Location& location) {
            const ScriptMethod* best = nullptr;
            int bestCost = std::numeric_limits<int>::max();
            int candidates = 0;
            bool ambiguous = false;
            for (const ScriptMethod& m : script->methods) {
                if (m.name != name) continue;
                ++candidates;
                if (m.parameters.size() != args.size()) continue;
                int total = 0;
                for (size_t i = 0; i < args.size() && total >= 0; ++i) {
                    int c = Cost(args[i], VariableType(m.parameters[i]));
                    total = c < 0 ? -1 : total + c;
                }
                if (total < 0) continue;
                if (total < bestCost) {
                    best = &m;
                    bestCost = total;
                    ambiguous = false;
                } else if (total == bestCost) {
                    ambiguous = true;
                }
            }
            if (candidates <= 1) return candidates ? &*std::ranges::find(script->methods, name, &ScriptMethod::name) : nullptr;
            if (!best) Fail(location, std::format("no overload of {}.{} takes these {} argument(s)", script->name, name, args.size()));
            if (ambiguous) Fail(location, std::format("call to {}.{} is ambiguous between overloads", script->name, name));
            return best;
        }

        std::vector<Value> Compiler::CallScriptMethod(const Value& receiver, const ScriptMethod& method, const std::vector<Value>& args, size_t want, const Location& location) {
            Value self = Value::OfSlot(receiver.slot, types_.Behaviour());
            if (args.size() != method.parameters.size())
                Fail(location, std::format("'{}' takes {} argument(s), got {}", method.name, method.parameters.size(), args.size()));
            for (size_t i = 0; i < args.size(); ++i) {
                const Type* type = VariableType(method.parameters[i]);
                if (Cost(args[i], type) < 0)
                    Fail(location, std::format("argument {} of '{}' must be {}, got {}", i + 1, method.name, type->displayName, Describe(args[i])));
                Value arg = args[i].IsLiteral() ? Value::OfSlot(Materialize(args[i], type, location), type) : args[i];
                if (arg.kind == Value::Kind::Slot && arg.type != type && arg.type->IsNumeric() && type->IsNumeric())
                    arg = Value::OfSlot(Convert(arg.slot, arg.type, type, location), type);
                CallMember(self, "SetProgramVariable", { Value::OfString(method.parameters[i].symbol), arg }, nullptr, location);
            }
            CallMember(self, "SendCustomEvent", { Value::OfString(method.entryPoint) }, nullptr, location);
            std::vector<Value> results;
            for (size_t i = 0; i < std::min(want, method.returns.size()); ++i) {
                Value raw = CallMember(self, "GetProgramVariable", { Value::OfString(method.returns[i].symbol) }, nullptr, location).at(0);
                results.push_back(Value::OfSlot(raw.slot, VariableType(method.returns[i])));
            }
            return results;
        }

        Value Compiler::ReadScriptField(const Value& receiver, const ScriptVariable& field, const Location& location) {
            Value self = Value::OfSlot(receiver.slot, types_.Behaviour());
            Value raw = CallMember(self, "GetProgramVariable", { Value::OfString(field.symbol) }, nullptr, location).at(0);
            return Value::OfSlot(raw.slot, VariableType(field));
        }

        std::optional<Value> Compiler::TryNetworkTarget(AstExprCall* call) {
            auto* index = Unwrap(call->func)->as<AstExprIndexName>();
            if (!index || call->self || call->args.size != 1) return std::nullopt;
            auto* global = Unwrap(index->expr)->as<AstExprGlobal>();
            if (!global || global->name != "Network" || globalFunctions_.contains("Network") || defines_.contains("Network") || !types_.FindByShortName("Network").empty())
                return std::nullopt;
            std::vector<const Type*> targets = types_.FindByShortName("NetworkEventTarget");
            const Type* target = targets.empty() ? nullptr : PreferredType(targets) ? PreferredType(targets) : targets[0];
            if (!target || !target->info) Fail(call->location, "VRC.Udon.Common.Interfaces.NetworkEventTarget is not in the catalog");
            const EnumMember* member = nullptr;
            for (const EnumMember& m : target->info->enumMembers)
                if (m.name == index->index.value) member = &m;
            if (!member) {
                std::string names;
                for (const EnumMember& m : target->info->enumMembers) names += (names.empty() ? "" : ", ") + m.name;
                Fail(index->location, std::format("unknown network target '{}'; use one of: {}", index->index.value, names));
            }
            Value receiver = CompileExpr(call->args.data[0]);
            if (receiver.kind != Value::Kind::Slot || types_.ReferenceDistance(receiver.type, types_.Behaviour()) < 0)
                Fail(call->args.data[0]->location, std::format("Network.{} needs an UdonBehaviour, got {}", member->name, Describe(receiver)));
            Value v = Value::OfSlot(receiver.slot, receiver.type);
            v.kind = Value::Kind::Network;
            v.integer = member->value;
            v.targetType = target;
            return v;
        }

        void Compiler::DeclareSyncMode(AstStatBlock* root) {
            std::vector<FieldAttribute> module = ModuleAttributes(root);
            auto attribute = std::ranges::find(module, "syncmode", &FieldAttribute::name);
            if (attribute == module.end()) return;
            Location location;
            for (const auto& [line, entries] : commentAttributes_)
                for (const auto& entry : entries)
                    if (entry.first.name == "syncmode") location = entry.second;

            std::string mode = attribute->arguments.empty() ? std::string() : attribute->arguments[0];
            std::ranges::transform(mode, mode.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (mode == "any") syncMode_ = BehaviourSyncMode::Any;
            else if (mode == "none") syncMode_ = BehaviourSyncMode::None;
            else if (mode == "novariablesync") syncMode_ = BehaviourSyncMode::NoVariableSync;
            else if (mode == "continuous") syncMode_ = BehaviourSyncMode::Continuous;
            else if (mode == "manual") syncMode_ = BehaviourSyncMode::Manual;
            else Fail(location, std::format("unknown sync mode '{}'; use any, none, novariablesync, continuous or manual", mode));
        }

        void Compiler::CheckSynced(const std::string& name, const Type* type, SyncInterpolation interpolation, const Location& location) {
            if (syncMode_ == BehaviourSyncMode::None || syncMode_ == BehaviourSyncMode::NoVariableSync)
                Fail(location, std::format("'{}' cannot be synced: the script's sync mode is {}", name, syncMode_ == BehaviourSyncMode::None ? "none" : "novariablesync"));
            if (syncMode_ == BehaviourSyncMode::Continuous && type->kind == TypeKind::Array)
                Fail(location, std::format("'{}' is an array, which continuous sync does not support; use '-- @syncmode(manual)'", name));
            if (syncMode_ == BehaviourSyncMode::Manual && interpolation != SyncInterpolation::None)
                Fail(location, std::format("'{}' uses {} interpolation, which manual sync does not support; use '-- @syncmode(continuous)'", name,
                    interpolation == SyncInterpolation::Linear ? "linear" : "smooth"));
            if (!catalog_.HasSyncableTypes()) return;
            const Catalog::SyncSupport* support = type->script ? nullptr : catalog_.FindSyncableType(type->udonName);
            if (!support) Fail(location, std::format("'{}' cannot be synced: Udon does not sync {}", name, type->displayName));
            if (interpolation == SyncInterpolation::Linear && !support->linear)
                Fail(location, std::format("'{}' cannot use linear interpolation: {} does not support it", name, type->displayName));
            if (interpolation == SyncInterpolation::Smooth && !support->smooth)
                Fail(location, std::format("'{}' cannot use smooth interpolation: {} does not support it", name, type->displayName));
        }

        void Compiler::DeclareDefines(AstStatBlock* root) {
            auto parse = [](std::string_view text) {
                if (text == "true" || text == "false") return Value::OfBoolean(text == "true");
                if (text == "nil") return Value::OfNil();
                if (!text.empty()) {
                    char* end = nullptr;
                    std::string s(text);
                    long long i = std::strtoll(s.c_str(), &end, 0);
                    if (end && *end == '\0') return Value::OfInteger(i);
                    double d = std::strtod(s.c_str(), &end);
                    if (end && *end == '\0') return Value::OfReal(d);
                }
                return Value::OfString(std::string(text));
            };
            for (const FieldAttribute& a : ModuleAttributes(root))
                if (a.name == "define" && !a.arguments.empty()) defines_[a.arguments[0]] = a.arguments.size() > 1 ? parse(a.arguments[1]) : Value::OfBoolean(true);
            for (const auto& [name, value] : options_.defines) defines_[name] = parse(value);
        }

        Function* Compiler::Callee(AstExpr* func) const {
            while (auto* g = func->as<AstExprGroup>()) func = g->expr;
            if (auto* l = func->as<AstExprLocal>()) {
                if (auto it = localFunctions_.find(l->local); it != localFunctions_.end()) return it->second;
            } else if (auto* g = func->as<AstExprGlobal>()) {
                if (auto it = globalFunctions_.find(g->name.value); it != globalFunctions_.end()) return it->second;
            }
            return nullptr;
        }

        bool Compiler::CannotWrite(AstExprCall* call) const {
            if (IsPureStaticCall(call)) return true;
            Function* f = call->self ? nullptr : Callee(CallTarget(call));
            return f && pure_.contains(f);
        }

        bool Compiler::HasCall(AstExpr* expr) const {
            std::function<bool(AstExprCall*)> harmless = [this](AstExprCall* c) { return CannotWrite(c); };
            CallFinder finder(harmless);
            expr->visit(&finder);
            return finder.found;
        }

        void Compiler::Analyze(AstStatBlock* root) {
            auto record = [&](AstExpr* func) {
                if (Function* f = Callee(func)) ++f->calls;
            };
            Analysis analysis(assigned_, memberAssigned_, record);
            root->visit(&analysis);

            class DelayTargets : public AstVisitor {
            public:
                std::set<std::string, std::less<>> names;
                bool visit(AstExprCall* c) override {
                    auto* index = c->self ? Unwrap(c->func)->as<AstExprIndexName>() : nullptr;
                    auto* inner = index ? Unwrap(index->expr)->as<AstExprCall>() : nullptr;
                    auto* library = inner && inner->args.size >= 2 ? Unwrap(inner->func)->as<AstExprIndexName>() : nullptr;
                    auto* owner = library ? Unwrap(library->expr)->as<AstExprGlobal>() : nullptr;
                    auto* target = owner && std::string_view(owner->name.value) == "Delay" ? Unwrap(inner->args.data[1])->as<AstExprGlobal>() : nullptr;
                    if (target && std::string_view(target->name.value) == "this") names.insert(index->index.value);
                    return true;
                }
            };
            DelayTargets delayed;
            root->visit(&delayed);
            for (auto& f : functions_) {
                if (f->event || !delayed.names.contains(f->name)) continue;
                f->delayTarget = true;
                ++f->calls;
            }

            for (bool changed = true; changed;) {
                changed = false;
                for (auto& f : functions_) {
                    if (f->event || f->exported || pure_.contains(f.get())) continue;
                    PurityScan scan([this](AstExprCall* c) { return CannotWrite(c); });
                    f->node->body->visit(&scan);
                    if (scan.pure) {
                        pure_.insert(f.get());
                        changed = true;
                    }
                }
            }

            for (ChangeHandler& h : changeHandlers_) {
                for (const auto& f : functions_)
                    if (f->name == h.function && !f->event) h.target = f.get();
                if (h.target) ++h.target->calls;
            }
            AnalyzeCoroutines(root);

            for (auto& f : functions_) {
                NodeCounter counter;
                f->node->body->visit(&counter);
                f->size = counter.count;
                for (const FieldAttribute& a : AnnotationsFor(f->location).attributes) {
                    if (a.name == "inline") f->forceInline = true;
                    if (a.name == "noinline") f->noInline = true;
                }
                if (f->forceInline && f->noInline) Report(f->location, std::format("'{}' cannot be both @inline and @noinline", f->name));
                if (f->async && f->noInline) Report(f->location, std::format("'{}' waits, so it is inlined into every caller and cannot be @noinline", f->name));
                f->inlined = f->async || (!f->noInline && !f->delayTarget && f->calls > 0 && (f->forceInline || f->size <= kInlineBudget || (f->calls == 1 && !f->exported)));
                f->trampoline = options_.compatibleExitReturn || f->async || !f->exported || (f->calls > 0 && !f->inlined) || (f->event && eventListeners_.contains(f->event->name));
            }
        }

        void Compiler::AnalyzeCoroutines(AstStatBlock*) {
            class CallScan : public AstVisitor {
            public:
                std::function<void(AstExprCall*)> onCall;
                bool visit(AstExprCall* c) override {
                    onCall(c);
                    return true;
                }
            };
            std::unordered_map<Function*, std::vector<Function*>> callees;
            for (auto& f : functions_) {
                Function* self = f.get();
                CallScan scan;
                scan.onCall = [&](AstExprCall* c) {
                    if (c->self) {
                        auto* member = Unwrap(c->func)->as<AstExprIndexName>();
                        std::string_view method = member ? member->index.value : "";
                        if (SignalInfo* signal = member ? SignalOf(member->expr) : nullptr) {
                            if (method == "Wait") self->async = true;
                            Function* fn = method == "Connect" && c->args.size == 1 ? Callee(c->args.data[0]) : nullptr;
                            if (fn) {
                                fn->entered = true;
                                if (!signal->connections.contains(fn)) signal->connections.emplace(fn, emit_.Hidden(types_.Boolean));
                            }
                        } else if (const EventInfo* event = member ? EventOf(member->expr) : nullptr; event && method == "Wait") {
                            self->async = true;
                            if (event->name != "Update" && event->name != "LateUpdate" && event->name != "FixedUpdate" && event->name != "PostLateUpdate") {
                                auto [listeners, added] = eventListeners_.try_emplace(event->name);
                                listeners->second.event = event;
                                if (added) listeners->second.location = c->location;
                            }
                        }
                        return;
                    }
                    auto* index = CallTarget(c)->as<AstExprIndexName>();
                    std::optional<std::string_view> library = index ? LibraryName(index->expr) : std::nullopt;
                    if (library && *library == "task") {
                        std::string_view name = index->index.value;
                        if (name == "wait" || name == "waitFrames" || name == "waitUntil") {
                            self->async = true;
                            return;
                        }
                        size_t at = name == "delay" ? 1 : 0;
                        if (c->args.size <= at) return;
                        if (Function* target = Callee(c->args.data[at])) {
                            if (name == "cancel") target->cancelTarget = true;
                            else target->entered = true;
                        }
                        return;
                    }
                    if (Function* g = c->self ? nullptr : Callee(CallTarget(c))) callees[self].push_back(g);
                };
                f->node->body->visit(&scan);
            }
            for (bool changed = true; changed;) {
                changed = false;
                for (auto& [f, called] : callees) {
                    if (f->async || std::ranges::none_of(called, &Function::async)) continue;
                    f->async = true;
                    changed = true;
                }
            }
            for (ChangeHandler& h : changeHandlers_)
                if (h.target) h.target->entered = true;
            for (auto& [name, listeners] : eventListeners_) {
                listeners.dispatch = emit_.NewLabel();
                for (const auto& f : functions_)
                    if (f->event == listeners.event) listeners.handler = f.get();
                if (listeners.handler) {
                    listeners.values = listeners.handler->parameters;
                    continue;
                }
                if (name == "Interact" || name == "OnAnimatorMove" || name == "OnRenderObject" || name.starts_with("OnCollision"))
                    Report(listeners.location, std::format("Events.{}:Wait() needs a handler for {} in this script: declaring the event changes how VRChat treats the object", name, name));
                for (const EventParameter& p : listeners.event->parameters) {
                    const Type* type = types_.Get(p.type);
                    listeners.values.push_back({ emit_.AddSlot(LowerFirst(name) + UpperFirst(p.name), type), type });
                }
            }
            for (auto& f : functions_) {
                if (f->delayTarget) f->entered = true;
                for (const FieldAttribute& a : AnnotationsFor(f->location).attributes) {
                    if (a.name != "reentry") continue;
                    std::string_view mode = a.arguments.size() == 1 ? std::string_view(a.arguments[0]) : std::string_view{};
                    if (mode == "ignore") f->reentry = Reentry::Ignore;
                    else if (mode == "restart") f->reentry = Reentry::Restart;
                    else if (mode == "overlap") f->reentry = Reentry::Overlap;
                    else Report(f->location, "@reentry takes ignore, restart or overlap");
                    if (!f->async) Report(f->location, std::format("@reentry has no effect: '{}' never waits", f->name), Severity::Warning);
                }
                if (f->async && f->exported && !f->returns.empty())
                    Report(f->location, std::format("'{}' waits, so it cannot return values: callers get control back at its first wait", f->name));
                if (!f->IsRoot()) continue;
                HeapValue yes;
                yes.kind = ValueKind::Boolean;
                yes.boolean = true;
                f->freeSlot = emit_.AddSlot(std::format("__co_{}_free", f->name), types_.Boolean, yes);
                if (DebugBuild()) f->lineSlot = emit_.AddSlot(std::format("__co_{}_line", f->name), types_.Int32);
                if (f->Cancellable()) {
                    f->cancelRoutine = emit_.NewLabel();
                    f->cancelReturn = emit_.Hidden(types_.UInt32);
                }
            }
        }

        void Compiler::CheckUserName(std::string_view name, const Location& location) {
            if (name.starts_with("__")) Fail(location, std::format("'{}': names starting with '__' are reserved", name));
            if (name == "this" || name == "gameObject" || name == "transform") Fail(location, std::format("'{}' is reserved", name));
        }

        void Compiler::DeclareModule(AstStatBlock* root) {
            for (AstStat* stat : root->body) {
                try {
                    if (auto* alias = stat->as<AstStatTypeAlias>()) DeclareTypeAlias(alias);
                } catch (const CompileError& e) {
                    Report(e.location, e.message);
                }
            }
            for (AstStat* stat : root->body) {
                try {
                    auto* asLocal = stat->as<AstStatLocal>();
                    bool splittable = (asLocal && !asLocal->isConst) || stat->is<AstStatLocalFunction>() || stat->is<AstStatFunction>();
                    bool isStatic = splittable && std::ranges::any_of(AnnotationsFor(stat->location).attributes, [](const FieldAttribute& a) { return a.name == "static"; });
                    if (isStatic) hasStatics_ = true;
                    if (splittable && isStatic != options_.staticPart) {
                        if (asLocal && options_.staticPart) {
                            DeclareFields(asLocal, true);
                        } else if (asLocal) {
                            for (AstLocal* var : asLocal->vars) staticFields_[var] = var->name.value;
                        } else if (auto* lf = stat->as<AstStatLocalFunction>()) {
                            if (options_.staticPart) instanceLocals_.insert(lf->name);
                            else staticFunctions_[lf->name] = lf->name->name.value;
                        }
                        continue;
                    }
                    if (auto* local = stat->as<AstStatLocal>()) {
                        DeclareFields(local);
                    } else if (auto* lf = stat->as<AstStatLocalFunction>()) {
                        DeclareFunction(lf->name->name.value, lf->func, lf->location, lf->name);
                    } else if (auto* gf = stat->as<AstStatFunction>()) {
                        auto* global = gf->name->as<AstExprGlobal>();
                        if (!global) Fail(gf->name->location, "only plain global functions can be declared; methods on tables are not supported");
                        if (!catalog_.FindEvent(global->name.value))
                            Fail(gf->location, std::format("'{}' is not an Udon event; declare it 'export function {}' to make it public or 'local function {}' to keep it private",
                                global->name.value, global->name.value, global->name.value));
                        DeclareFunction(global->name.value, gf->func, gf->location, nullptr);
                    } else if (stat->is<AstStatTypeAlias>() || stat->is<AstStatTypeFunction>()) {
                    } else {
                        Fail(stat->location, "only variable and function declarations are allowed at module level");
                    }
                } catch (const CompileError& e) {
                    Report(e.location, e.message);
                }
            }
        }

        void Compiler::DeclareFields(AstStatLocal* stat, bool aliasesOnly) {
            Annotations annotations = AnnotationsFor(stat->location);
            const bool exported = stat->isExported || options_.staticPart;
            for (size_t i = 0; i < stat->vars.size; ++i) {
                AstLocal* var = stat->vars.data[i];
                std::string name = var->name.value;
                CheckUserName(name, var->location);
                if (!userSymbols_.insert(name).second) Fail(var->location, std::format("'{}' is already declared", name));

                if (aliasesOnly) {
                    AstExpr* init = i < stat->values.size ? Unwrap(stat->values.data[i]) : nullptr;
                    if (!var->annotation && init && (init->is<AstExprGlobal>() || init->is<AstExprIndexName>())) {
                        Value v = CompileExpr(init);
                        if (v.kind == Value::Kind::TypeRef || v.kind == Value::Kind::Namespace) {
                            aliases_[var] = v;
                            continue;
                        }
                    }
                    instanceLocals_.insert(var);
                    continue;
                }

                if (stat->isConst && i < stat->values.size)
                    if (auto* table = Unwrap(stat->values.data[i])->as<AstExprTable>()) {
                        DeclareConstTable(var, table);
                        continue;
                    }
                if (DeclareSignal(var, i < stat->values.size ? stat->values.data[i] : nullptr, exported)) continue;
                const Type* type = var->annotation ? ResolveType(var->annotation) : nullptr;
                if (type && type->IsList()) {
                    if (options_.staticPart) Fail(var->location, "a List cannot be static; use an array");
                    if (exported) Fail(var->location, "a List cannot be exported; export an array instead");
                    if (stat->isConst) Fail(var->location, "a List cannot be a constant");
                    if (annotations.synced) Fail(var->location, "a List cannot be synced; sync an array instead");
                    ListInit parsed = ParseListInit(i < stat->values.size ? stat->values.data[i] : nullptr);
                    if (!parsed.ok) Fail(var->location, "initialize a List field with {} or constant items, e.g. {1, 2, 3}");
                    HeapValue array;
                    array.kind = ValueKind::Array;
                    array.text = type->element->udonName;
                    for (AstExpr* item : parsed.items) {
                        Value v = CompileExpr(item, type->element);
                        if (!v.IsLiteral()) Fail(item->location, "field initializers must be constants; assign other values in Start");
                        array.arguments.push_back(Literal(v, type->element, item->location));
                    }
                    variables_[var] = DeclareListStorage(name, type, true, std::move(array), static_cast<int64_t>(parsed.items.size()));
                    continue;
                }
                HeapValue initial;
                if (i < stat->values.size) {
                    AstExpr* init = stat->values.data[i];
                    Value v = CompileExpr(init, type);
                    if (!type && (v.kind == Value::Kind::TypeRef || v.kind == Value::Kind::Namespace)) {
                        if (stat->isExported) Fail(var->location, "type and namespace aliases cannot be exported");
                        aliases_[var] = v;
                        continue;
                    }
                    if (stat->isConst) {
                        if (stat->isExported) Fail(var->location, "constants cannot be exported; use 'export local'");
                        bool constant = v.IsLiteral() || (v.kind == Value::Kind::Slot && emit_.IsConstant(v.slot));
                        if (!constant) Fail(init->location, "module constants need a value known at compile time");
                        if (type && Cost(v, type) < 0) Fail(init->location, std::format("cannot initialize {} '{}' with {}", type->displayName, name, Describe(v)));
                        aliases_[var] = type && v.IsLiteral() ? CoerceLiteral(v, type, init->location) : v;
                        continue;
                    }
                    if (v.kind == Value::Kind::Slot && emit_.IsConstant(v.slot) && (!type || type == v.type)) {
                        type = v.type;
                        initial = emit_.Slot(v.slot).value;
                        uint32_t slot = emit_.AddSlot(name, type, initial, exported);
                        fieldSlots_.insert(slot);
                        if (exported) exportedFields_.emplace_back(name, type);
                        emit_.Slot(slot).attributes = annotations.attributes;
                        variables_[var] = { slot, type };
                        RecordChangeHandler(annotations, name, { slot, type }, var->location);
                        if (annotations.synced) {
                            CheckSynced(name, type, annotations.interpolation, var->location);
                            sync_.push_back({ name, annotations.interpolation });
                        }
                        continue;
                    }
                    if (!v.IsLiteral()) Fail(init->location, "field initializers must be constants; assign other values in Start");
                    if (!type) type = NaturalType(v);
                    if (!type) Fail(var->location, std::format("'{}' needs a type annotation", name));
                    if (Cost(v, type) < 0) Fail(init->location, std::format("cannot initialize {} '{}' with {}", type->displayName, name, Describe(v)));
                    initial = Literal(v, type, init->location);
                } else if (!type) {
                    Fail(var->location, std::format("'{}' needs a type annotation or an initializer", name));
                }

                if (stat->isConst) Fail(var->location, "constants need a value");
                uint32_t slot = emit_.AddSlot(name, type, initial, exported);
                fieldSlots_.insert(slot);
                if (exported) exportedFields_.emplace_back(name, type);
                emit_.Slot(slot).attributes = annotations.attributes;
                variables_[var] = { slot, type };
                RecordChangeHandler(annotations, name, { slot, type }, var->location);
                if (annotations.synced) {
                    CheckSynced(name, type, annotations.interpolation, var->location);
                    sync_.push_back({ name, annotations.interpolation });
                }
            }
        }

        void Compiler::DeclareFunction(std::string name, AstExprFunction* node, const Location& location, AstLocal* local) {
            CheckUserName(name, location);
            if (node->vararg) Fail(node->varargLocation, "variadic functions are not supported");
            if (node->generics.size || node->genericPacks.size) Fail(location, "generic functions are not supported");

            auto f = std::make_unique<Function>();
            f->name = name;
            f->node = node;
            f->location = location;
            f->exportedEntry = emit_.NewLabel();
            f->entry = emit_.NewLabel();
            f->epilogue = emit_.NewLabel();

            if (!local || local->isExported || options_.staticPart) {
                if (globalFunctions_.contains(name) || std::ranges::any_of(functions_, [&](const auto& g) { return g->exported && g->name == name; }))
                    Fail(location, std::format("function '{}' is already defined", name));
                f->exported = true;
                f->event = catalog_.FindEvent(name);
            }

            for (const FieldAttribute& a : AnnotationsFor(location).attributes) {
                if (a.name != "networkcallable") continue;
                if (!f->exported || f->event) Fail(location, "only 'export function' methods can be @networkcallable");
                f->networkCallable = true;
                if (!a.arguments.empty()) {
                    char* end = nullptr;
                    long rate = std::strtol(a.arguments[0].c_str(), &end, 10);
                    if (!end || *end || rate <= 0) Fail(location, "@networkcallable takes the maximum events per second as a positive integer");
                    f->maxEventsPerSecond = static_cast<int>(rate);
                }
            }
            std::optional<std::string> entryOverride;
            for (const FieldAttribute& a : AnnotationsFor(location).attributes) {
                if (a.name != "entry") continue;
                if (!f->exported || f->event) Fail(location, "only 'export function' methods can set their entry name with @entry");
                if (a.arguments.size() != 1 || a.arguments[0].empty()) Fail(location, "@entry takes the entry point name, e.g. @entry(\"OnDataUpdated\")");
                const std::string& entry = a.arguments[0];
                if (!std::ranges::all_of(entry, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }))
                    Fail(location, std::format("'{}' is not a valid entry point name", entry));
                if (f->networkCallable && entry.starts_with('_')) Fail(location, "network callable entry points cannot start with '_'");
                entryOverride = entry;
            }
            if (f->exported) {
                if (f->event) f->entryName = "_" + LowerFirst(name);
                else if (entryOverride) f->entryName = *entryOverride;
                else if (f->networkCallable) f->entryName = name;
                else f->entryName = node->args.size ? ExportId("_" + name) : "_" + name;
                if (!entryNames_.insert(f->entryName).second) Fail(location, std::format("entry point '{}' is already used by another function", f->entryName));
                for (const EventInfo* e : catalog_.Events())
                    if (!f->event && "_" + LowerFirst(e->name) == f->entryName)
                        Fail(location, std::format("'{}' would share its entry point with the {} event; rename it", name, e->name));
            }
            if (f->networkCallable) {
                if (syncMode_ == BehaviourSyncMode::None)
                    Fail(location, "sync mode none disables network events; use '-- @syncmode(novariablesync)' to keep them without synced variables");
                if (node->args.size > 8) Fail(location, "network callable methods take at most 8 parameters");
                if (node->returnAnnotation) Fail(node->returnAnnotation->location, "network callable methods cannot return values");
            }

            for (size_t i = 0; i < node->args.size; ++i) {
                AstLocal* arg = node->args.data[i];
                const Type* type = nullptr;
                uint32_t slot = 0;
                if (f->event) {
                    if (node->args.size != f->event->parameters.size())
                        Fail(location, std::format("event '{}' takes {} parameter(s)", name, f->event->parameters.size()));
                    const EventParameter& p = f->event->parameters[i];
                    type = types_.Get(p.type);
                    if (arg->annotation && ResolveType(arg->annotation) != type)
                        Fail(arg->location, std::format("parameter '{}' of event '{}' is {}", arg->name.value, name, type->displayName));
                    slot = emit_.AddSlot(LowerFirst(name) + UpperFirst(p.name), type);
                } else {
                    if (!arg->annotation) Fail(arg->location, std::format("parameter '{}' needs a type annotation", arg->name.value));
                    type = ResolveType(arg->annotation);
                    if (type->IsList()) {
                        if (f->exported) Fail(arg->location, "public methods cannot take a List; take an array instead");
                        variables_[arg] = { 0, type };
                        f->parameters.push_back({ 0, type });
                        continue;
                    }
                    slot = f->exported ? emit_.AddSlot(ExportId(std::string(arg->name.value) + "__param"), type) : emit_.Local(arg->name.value, type);
                }
                variables_[arg] = { slot, type };
                f->parameters.push_back({ slot, type });
            }

            if (auto* pack = node->returnAnnotation ? node->returnAnnotation->as<AstTypePackExplicit>() : nullptr) {
                if (pack->typeList.tailType) Fail(pack->location, "variadic returns are not supported");
                for (AstType* t : pack->typeList.types) {
                    const Type* type = ResolveType(t);
                    if (type->IsList()) Fail(t->location, "functions cannot return a List; return list:ToArray() instead");
                    if (f->event) {
                        if (!f->returns.empty()) Fail(t->location, "events return at most one value");
                        if (eventReturnSlot_ == 0) eventReturnSlot_ = emit_.AddSlot("__returnValue", types_.Object, {}, false);
                        f->returns.push_back(type);
                        f->returnSlots.push_back(eventReturnSlot_);
                        continue;
                    }
                    std::string symbol = f->exported ? ExportId(f->entryName + (f->returns.empty() ? std::string("__ret") : std::format("__ret{}", f->returns.size()))) : std::string();
                    f->returns.push_back(type);
                    f->returnSlots.push_back(f->exported ? emit_.AddSlot(symbol, type) : emit_.Hidden(type));
                }
            } else if (node->returnAnnotation) {
                Fail(node->returnAnnotation->location, "unsupported return annotation");
            }

            Function* raw = f.get();
            functions_.push_back(std::move(f));
            if (local) localFunctions_[local] = raw;
            else globalFunctions_[name] = raw;
        }

        void Compiler::CompileFunction(Function& f) {
            bool root = f.IsRoot();
            if (!f.exported && !root && (f.inlined || f.calls == 0)) return;
            for (const Variable& p : f.parameters)
                if (p.type->IsList()) Fail(f.location, std::format("'{}' takes a List, so it must be inlined; mark it '-- @inline'", f.name));
            current_ = &f;
            root_ = root ? &f : nullptr;
            emit_.ResetTemps();
            for (size_t i = 0; i < f.parameters.size(); ++i) {
                AstLocal* arg = f.node->args.data[i];
                aliases_.erase(arg);
                variables_[arg] = f.parameters[i];
            }
            if (f.exported) {
                emit_.Bind(f.exportedEntry);
                entries_.push_back({ f.entryName, *emit_.AddressOf(f.exportedEntry) });
                if (f.trampoline) {
                    auto listening = f.event ? eventListeners_.find(f.event->name) : eventListeners_.end();
                    emit_.Push(listening != eventListeners_.end() ? emit_.AddressConstant(listening->second.dispatch) : haltSlot_);
                }
                EmitEntryPrologue(f);
            }
            emit_.Bind(f.entry);
            Label reject = emit_.NewLabel();
            if (root) EmitRootPrologue(f, reject);
            AstStat* tail = nullptr;
            CompileBody(f.node->body, tail);
            emit_.Bind(f.epilogue);
            if (root) emit_.Copy(TruthySlot(Value::OfBoolean(true), f.location), f.freeSlot);
            if (f.trampoline) EmitYield();
            else emit_.JumpTo(0xFFFFFFFFu);
            if (root) {
                EmitRootReject(f, reject);
                if (f.Cancellable()) EmitCancelRoutine(f);
            }
            current_ = nullptr;
            root_ = nullptr;
        }

        void Compiler::EmitRootPrologue(Function& f, Label reject) {
            emit_.JumpIfFalse(f.freeSlot, reject);
            if (f.reentry == Reentry::Restart) CallCancel(f);
            emit_.Copy(TruthySlot(Value::OfBoolean(false), f.location), f.freeSlot);
            for (size_t i = 0; i < f.parameters.size(); ++i) {
                AstLocal* arg = f.node->args.data[i];
                Variable own{ emit_.Local(arg->name.value, f.parameters[i].type), f.parameters[i].type };
                emit_.Copy(f.parameters[i].slot, own.slot);
                variables_[arg] = own;
            }
        }

        void Compiler::EmitRootReject(Function& f, Label reject) {
            emit_.Bind(reject);
            if (DebugBuild()) {
                std::string who = options_.scriptName.empty() ? f.name : options_.scriptName + "." + f.name;
                Value message = f.reentry == Reentry::Ignore
                    ? ConcatValues({ Value::OfString(std::format("[UdonLuau] {} ignored: still waiting at line ", who)), Value::OfSlot(f.lineSlot, types_.Int32),
                                       Value::OfString(" (@reentry(ignore))") }, f.location)
                    : Value::OfString(std::format("[UdonLuau] {} ignored: it was triggered again while running", who));
                CallStatic(types_.Get("UnityEngineDebug"), "LogWarning", { message }, nullptr, f.location);
            }
            EmitYield();
        }

        void Compiler::CallCancel(Function& f) {
            Label back = emit_.NewLabel();
            emit_.Copy(emit_.AddressConstant(back), f.cancelReturn);
            emit_.Jump(f.cancelRoutine);
            emit_.Bind(back);
        }

        void Compiler::EmitCancelRoutine(Function& f) {
            emit_.Bind(f.cancelRoutine);
            uint32_t yes = TruthySlot(Value::OfBoolean(true), f.location);
            uint32_t no = TruthySlot(Value::OfBoolean(false), f.location);
            emit_.Copy(yes, f.freeSlot);
            for (uint32_t flag : f.waitFlags) emit_.Copy(no, flag);
            emit_.JumpIndirect(f.cancelReturn);
        }

        std::vector<Value> Compiler::CallTask(AstExprCall* call, std::string_view name, size_t want) {
            const Location& location = call->location;
            if (!FindPolyfill("task", name)) Fail(location, std::format("task.{} is not supported; available: {}", name, PolyfillNames("task")));
            size_t count = call->args.size;
            auto arg = [&](size_t i) { return CompileExpr(call->args.data[i]); };
            Value update = Value::OfInteger(0, types_.Get("VRCUdonCommonEnumsEventTiming"));

            if (name == "wait" || name == "waitFrames") {
                if (count > 2) Fail(location, std::format("task.{} takes the amount and an optional EventTiming", name));
                if (name == "waitFrames" && count == 0) Fail(location, "task.waitFrames takes the number of frames");
                bool frames = name == "waitFrames" || count == 0;
                Value amount = count ? arg(0) : Value::OfInteger(1);
                Value timing = count > 1 ? arg(1) : update;
                return EmitWait(frames, amount, timing, want, location);
            }
            if (name == "waitUntil") {
                if (count != 1) Fail(location, "task.waitUntil takes one condition");
                Label test = emit_.NewLabel();
                Label pause = emit_.NewLabel();
                Label done = emit_.NewLabel();
                emit_.Bind(test);
                CompileCondition(call->args.data[0], pause);
                emit_.Jump(done);
                emit_.Bind(pause);
                EmitWait(true, Value::OfInteger(1), update, 0, location);
                emit_.Jump(test);
                emit_.Bind(done);
                return {};
            }

            size_t at = name == "delay" ? 1 : 0;
            Function* target = count > at ? Callee(call->args.data[at]) : nullptr;
            if (!target) Fail(location, std::format("task.{} takes a function of this script", name));
            if (name == "cancel") {
                if (count != 1) Fail(location, "task.cancel takes one function");
                if (!target->IsRoot()) Fail(location, std::format("task.cancel: '{}' never waits, so there is nothing to cancel", target->name));
                CallCancel(*target);
                if (root_ == target) EmitYield();
                return {};
            }
            std::vector<Value> rest;
            for (size_t i = at + 1; i < count; ++i) rest.push_back(Evaluate(arg(i)));
            if (name == "spawn") {
                EnterRoot(target, std::move(rest), location);
                return {};
            }
            Value amount = name == "delay" ? arg(0) : Value::OfInteger(1);
            Value delayed = CallDelay(name == "delay" ? "Seconds" : "Frames", { amount, SelfValue("this") }, location).at(0);
            CallDelayed(delayed, target->name, std::move(rest), location);
            return {};
        }

        std::vector<Value> Compiler::EmitWait(bool frames, const Value& requested, const Value& timing, size_t want, const Location& location) {
            Function* root = root_;
            if (!root) Fail(location, "waits can only be used in events, public methods and functions they call or start");
            if (!timing.IsLiteral()) Fail(location, "the EventTiming of a wait must be a constant, such as EventTiming.LateUpdate");
            const Type* amountType = frames ? types_.Int32 : types_.Single;
            if (Cost(requested, amountType) < 0) Fail(location, std::format("the wait must be {}, got {}", amountType->displayName, Describe(requested)));
            Value amount = requested.IsLiteral() ? CoerceLiteral(requested, amountType, location) : requested;
            if (amount.kind == Value::Kind::Slot && amount.type != amountType) amount = Value::OfSlot(Convert(amount.slot, amount.type, amountType, location), amountType);

            const Type* clock = frames ? types_.Int32 : types_.Double;
            auto now = [&] { return CallStatic(types_.Get("UnityEngineTime"), frames ? "get_frameCount" : "get_timeAsDouble", {}, nullptr, location).at(0); };
            auto flag = [&](bool value) { return TruthySlot(Value::OfBoolean(value), location); };

            std::string entry = std::format("__co{}", waitSites_++);
            std::optional<Variable> start;
            if (want > 0) {
                start = Variable{ emit_.Hidden(types_.Double), types_.Double };
                Store(CallStatic(types_.Get("UnityEngineTime"), "get_timeAsDouble", {}, nullptr, location).at(0), *start, location);
            }
            bool cancellable = root->Cancellable();
            Variable waiting{}, pending{}, due{};
            if (cancellable) {
                waiting = { emit_.Hidden(types_.Boolean), types_.Boolean };
                pending = { emit_.Hidden(types_.Int32), types_.Int32 };
                due = { emit_.Hidden(clock), clock };
                root->waitFlags.push_back(waiting.slot);
                emit_.Copy(flag(true), waiting.slot);
                Store(CompileArithmetic(AstExprBinary::Add, Value::OfSlot(pending.slot, types_.Int32), Value::OfInteger(1), location), pending, location);
                Value span = amount;
                if (!frames) {
                    constexpr double kEarly = 0.0005;
                    span = amount.IsLiteral() ? Value::OfSlot(Materialize(Value::OfReal((amount.kind == Value::Kind::Real ? amount.real : static_cast<double>(amount.integer)) - kEarly), types_.Double, location), types_.Double)
                                              : CompileArithmetic(AstExprBinary::Sub, Value::OfSlot(Convert(amount.slot, types_.Single, types_.Double, location), types_.Double),
                                                    Value::OfSlot(Materialize(Value::OfReal(kEarly), types_.Double, location), types_.Double), location);
                }
                Store(CompileArithmetic(AstExprBinary::Add, now(), span, location), due, location);
            }
            CallMember(SelfValue("this"), frames ? "SendCustomEventDelayedFrames" : "SendCustomEventDelayedSeconds", { Value::OfString(entry), amount, timing }, nullptr, location);
            if (root->lineSlot) emit_.Copy(Materialize(Value::OfInteger(location.begin.line + 1), types_.Int32, location), root->lineSlot);
            if (root->reentry != Reentry::Ignore) emit_.Copy(flag(true), root->freeSlot);
            EmitYield();

            Label resume = emit_.NewLabel();
            emit_.Bind(resume);
            entries_.push_back({ entry, *emit_.AddressOf(resume) });
            emit_.Push(haltSlot_);
            if (cancellable) {
                Label stale = emit_.NewLabel();
                Label fresh = emit_.NewLabel();
                Label go = emit_.NewLabel();
                Store(CompileArithmetic(AstExprBinary::Sub, Value::OfSlot(pending.slot, types_.Int32), Value::OfInteger(1), location), pending, location);
                emit_.JumpIfFalse(waiting.slot, stale);
                Value more = CompareValues(AstExprBinary::CompareGt, Value::OfSlot(pending.slot, types_.Int32), Value::OfInteger(0), location);
                emit_.JumpIfFalse(TruthySlot(more, location), fresh);
                Value reached = CompareValues(AstExprBinary::CompareGe, now(), Value::OfSlot(due.slot, clock), location);
                emit_.JumpIfFalse(TruthySlot(reached, location), stale);
                emit_.Bind(fresh);
                emit_.Copy(flag(false), waiting.slot);
                emit_.Jump(go);
                emit_.Bind(stale);
                EmitYield();
                emit_.Bind(go);
            }
            if (root->reentry != Reentry::Ignore) emit_.Copy(flag(false), root->freeSlot);
            if (!start) return {};
            Value elapsed = CompileArithmetic(AstExprBinary::Sub, CallStatic(types_.Get("UnityEngineTime"), "get_timeAsDouble", {}, nullptr, location).at(0), Value::OfSlot(start->slot, types_.Double), location);
            return { Value::OfSlot(Convert(elapsed.slot, types_.Double, types_.Single, location), types_.Single) };
        }

        void Compiler::EnterRoot(Function* f, std::vector<Value> args, const Location& location) {
            if (!f->IsRoot()) {
                CallFunction(f, std::move(args), 0, location);
                return;
            }
            if (args.size() != f->parameters.size()) Fail(location, std::format("'{}' takes {} argument(s), got {}", f->name, f->parameters.size(), args.size()));
            std::vector<uint32_t> slots;
            for (size_t i = 0; i < args.size(); ++i) {
                if (Cost(args[i], f->parameters[i].type) < 0)
                    Fail(location, std::format("argument {} of '{}' must be {}, got {}", i + 1, f->name, f->parameters[i].type->displayName, Describe(args[i])));
                slots.push_back(Materialize(args[i], f->parameters[i].type, location));
            }
            for (size_t i = 0; i < slots.size(); ++i) emit_.Copy(slots[i], f->parameters[i].slot);
            if (current_) current_->callees.push_back(f);
            Label back = emit_.NewLabel();
            emit_.Push(emit_.AddressConstant(back));
            emit_.Jump(f->entry);
            emit_.Bind(back);
            EmitAliveCheck();
        }

        bool Compiler::DeclareSignal(AstLocal* var, AstExpr* init, bool exported) {
            auto* ref = var->annotation ? var->annotation->as<AstTypeReference>() : nullptr;
            bool annotated = ref && !ref->prefix && std::string_view(ref->name.value) == "Signal" && !typeAliases_.contains("Signal");
            auto* call = init ? Unwrap(init)->as<AstExprCall>() : nullptr;
            auto* ctor = call ? Unwrap(call->func)->as<AstExprGlobal>() : nullptr;
            bool constructed = ctor && std::string_view(ctor->name.value) == "Signal" && !globalFunctions_.contains("Signal");
            if (!annotated && !constructed) return false;
            if (!constructed || call->args.size) Fail(var->location, "create a signal with Signal(), e.g. 'local hit: Signal<VRCPlayerApi> = Signal()'");
            if (var->annotation && !annotated) Fail(var->annotation->location, "a signal's type is Signal<...>");
            if (options_.staticPart) Fail(var->location, "signals cannot be static");
            SignalInfo info;
            info.name = var->name.value;
            info.exported = exported;
            if (annotated) {
                for (const AstTypeOrPack& p : ref->parameters) {
                    if (!p.type) Fail(ref->location, "a signal's values are listed as types, e.g. Signal<VRCPlayerApi, number>");
                    const Type* type = ResolveType(p.type);
                    if (type->IsList()) Fail(p.type->location, "a signal cannot carry a List; pass an array");
                    info.types.push_back(type);
                }
            }
            signals_[var] = std::move(info);
            return true;
        }

        SignalInfo* Compiler::SignalOf(AstExpr* expr) {
            auto* l = Unwrap(expr)->as<AstExprLocal>();
            if (!l) return nullptr;
            auto it = signals_.find(l->local);
            return it == signals_.end() ? nullptr : &it->second;
        }

        const EventInfo* Compiler::EventOf(AstExpr* expr) const {
            auto* index = Unwrap(expr)->as<AstExprIndexName>();
            auto* owner = index ? Unwrap(index->expr)->as<AstExprGlobal>() : nullptr;
            if (!owner || std::string_view(owner->name.value) != "Events" || globalFunctions_.contains("Events") || defines_.contains("Events")) return nullptr;
            return catalog_.FindEvent(index->index.value);
        }

        std::optional<std::vector<Value>> Compiler::CallListenable(AstExprCall* call, size_t want) {
            auto* index = call->self ? Unwrap(call->func)->as<AstExprIndexName>() : nullptr;
            if (!index) return std::nullopt;
            std::string_view method = index->index.value;
            const Location& location = call->location;
            if (SignalInfo* signal = SignalOf(index->expr)) {
                if (method == "Wait") {
                    if (call->args.size) Fail(location, "Wait takes no arguments");
                    return EmitListen(signal->types, signal->sites, want, location);
                }
                if (method == "Fire") {
                    if (call->args.size != signal->types.size()) Fail(location, std::format("'{}' carries {} value(s), got {}", signal->name, signal->types.size(), call->args.size));
                    FireSite fire;
                    fire.signal = signal;
                    fire.block = emit_.NewLabel();
                    fire.back = emit_.NewLabel();
                    fire.location = location;
                    for (size_t i = 0; i < call->args.size; ++i) {
                        const Type* type = signal->types[i];
                        Value v = CompileExpr(call->args.data[i], type);
                        if (Cost(v, type) < 0) Fail(call->args.data[i]->location, std::format("value {} of '{}' must be {}, got {}", i + 1, signal->name, type->displayName, Describe(v)));
                        Variable held{ emit_.Temp(type), type };
                        Store(v, held, location);
                        fire.values.push_back(Value::OfSlot(held.slot, type));
                    }
                    emit_.Jump(fire.block);
                    emit_.Bind(fire.back);
                    fireSites_.push_back(std::move(fire));
                    EmitAliveCheck();
                    return std::vector<Value>{};
                }
                if (method == "Connect" || method == "Disconnect") {
                    Function* fn = call->args.size == 1 ? Callee(call->args.data[0]) : nullptr;
                    if (!fn) Fail(location, std::format("{} takes a function of this script", method));
                    if (fn->parameters.size() != signal->types.size() ||
                        !std::ranges::equal(fn->parameters, signal->types, [](const Variable& p, const Type* t) { return p.type == t; }))
                        Fail(location, std::format("'{}' must take the values '{}' carries", fn->name, signal->name));
                    auto connection = signal->connections.find(fn);
                    if (connection == signal->connections.end()) {
                        if (method == "Disconnect") return std::vector<Value>{};
                        connection = signal->connections.emplace(fn, emit_.Hidden(types_.Boolean)).first;
                    }
                    emit_.Copy(TruthySlot(Value::OfBoolean(method == "Connect"), location), connection->second);
                    return std::vector<Value>{};
                }
                Fail(location, std::format("signals have Wait, Fire, Connect and Disconnect, not '{}'", method));
            }
            if (const EventInfo* event = EventOf(index->expr)) {
                if (method != "Wait") Fail(location, std::format("Events.{} only has Wait", event->name));
                if (call->args.size) Fail(location, "Wait takes no arguments");
                for (std::string_view frameEvent : { "Update", "LateUpdate", "FixedUpdate", "PostLateUpdate" }) {
                    if (event->name != frameEvent) continue;
                    const Type* timing = types_.Get("VRCUdonCommonEnumsEventTiming");
                    const EnumMember* member = nullptr;
                    if (timing && timing->info)
                        for (const EnumMember& m : timing->info->enumMembers)
                            if (m.name == frameEvent) member = &m;
                    if (!member) Fail(location, std::format("this SDK has no EventTiming.{}", frameEvent));
                    return EmitWait(true, Value::OfInteger(1), Value::OfInteger(member->value, timing), 0, location);
                }
                EventListeners& listeners = eventListeners_.at(event->name);
                std::vector<const Type*> types;
                for (const Variable& v : listeners.values) types.push_back(v.type);
                return EmitListen(types, listeners.sites, want, location);
            }
            return std::nullopt;
        }

        std::vector<Value> Compiler::EmitListen(const std::vector<const Type*>& types, std::vector<ListenSite*>& registry, size_t want, const Location& location) {
            Function* root = root_;
            if (!root) Fail(location, "waits can only be used in events, public methods and functions they call or start");
            ListenSite& site = listenSites_.emplace_back();
            site.root = root;
            site.waiting = emit_.Hidden(types_.Boolean);
            site.taken = emit_.Hidden(types_.Boolean);
            site.direct = emit_.NewLabel();
            for (const Type* t : types) site.results.push_back({ emit_.Hidden(t), t });
            root->waitFlags.push_back(site.waiting);
            root->waitFlags.push_back(site.taken);
            registry.push_back(&site);

            emit_.Copy(TruthySlot(Value::OfBoolean(true), location), site.waiting);
            if (root->lineSlot) emit_.Copy(Materialize(Value::OfInteger(location.begin.line + 1), types_.Int32, location), root->lineSlot);
            if (root->reentry != Reentry::Ignore) emit_.Copy(TruthySlot(Value::OfBoolean(true), location), root->freeSlot);
            EmitYield();
            emit_.Bind(site.direct);
            if (root->reentry != Reentry::Ignore) emit_.Copy(TruthySlot(Value::OfBoolean(false), location), root->freeSlot);
            std::vector<Value> out;
            for (size_t i = 0; i < std::min(want, site.results.size()); ++i) out.push_back(Value::OfSlot(site.results[i].slot, site.results[i].type));
            return out;
        }

        void Compiler::EmitDispatch(const std::vector<ListenSite*>& sites, const std::vector<Value>& values, const std::map<Function*, uint32_t>& connections, const Location& location) {
            uint32_t yes = TruthySlot(Value::OfBoolean(true), location);
            uint32_t no = TruthySlot(Value::OfBoolean(false), location);
            uint32_t busy = emit_.Hidden(types_.Boolean);
            Label start = emit_.NewLabel();
            Label done = emit_.NewLabel();
            emit_.JumpIfFalse(busy, start);
            if (DebugBuild()) CallStatic(types_.Get("UnityEngineDebug"), "LogWarning", { Value::OfString(std::format("[UdonLuau] line {}: fired again while it was still being handled; ignored", location.begin.line + 1)) }, nullptr, location);
            emit_.Jump(done);
            emit_.Bind(start);
            emit_.Copy(yes, busy);
            std::vector<uint32_t> snapshots;
            for (const ListenSite* site : sites) {
                uint32_t snapshot = emit_.Hidden(types_.Boolean);
                snapshots.push_back(snapshot);
                Label idle = emit_.NewLabel();
                Label next = emit_.NewLabel();
                emit_.JumpIfFalse(site->waiting, idle);
                emit_.Copy(no, site->waiting);
                emit_.Copy(yes, site->taken);
                emit_.Copy(yes, snapshot);
                emit_.Jump(next);
                emit_.Bind(idle);
                emit_.Copy(no, snapshot);
                emit_.Bind(next);
            }
            for (size_t i = 0; i < sites.size(); ++i) {
                const ListenSite* site = sites[i];
                Label next = emit_.NewLabel();
                Label back = emit_.NewLabel();
                emit_.JumpIfFalse(snapshots[i], next);
                emit_.JumpIfFalse(site->taken, next);
                emit_.Copy(no, site->taken);
                for (size_t j = 0; j < site->results.size(); ++j) emit_.Copy(values[j].slot, site->results[j].slot);
                emit_.Push(emit_.AddressConstant(back));
                emit_.Jump(site->direct);
                emit_.Bind(back);
                emit_.Bind(next);
            }
            for (const auto& [fn, flag] : connections) {
                Label next = emit_.NewLabel();
                emit_.JumpIfFalse(flag, next);
                EnterRoot(fn, values, location);
                emit_.Bind(next);
            }
            emit_.Copy(no, busy);
            emit_.Bind(done);
        }

        void Compiler::EmitFireBlock(const FireSite& fire) {
            current_ = nullptr;
            root_ = nullptr;
            emit_.ResetTemps();
            emit_.Bind(fire.block);
            EmitDispatch(fire.signal->sites, fire.values, fire.signal->connections, fire.location);
            emit_.Jump(fire.back);
        }

        void Compiler::EmitEventDispatch(EventListeners& listeners) {
            current_ = nullptr;
            root_ = nullptr;
            emit_.ResetTemps();
            Location location;
            if (listeners.handler) {
                location = listeners.handler->location;
                for (size_t i = 0; i < listeners.handler->parameters.size(); ++i)
                    if (!listeners.handler->async && assigned_.contains(listeners.handler->node->args.data[i]))
                        Fail(location, std::format("Events.{}:Wait() passes on {}'s parameters, so the handler cannot assign them; copy into a local instead", listeners.event->name, listeners.event->name));
            } else {
                Label entry = emit_.NewLabel();
                emit_.Bind(entry);
                entries_.push_back({ "_" + LowerFirst(listeners.event->name), *emit_.AddressOf(entry) });
            }
            emit_.Bind(listeners.dispatch);
            std::vector<Value> values;
            for (const Variable& v : listeners.values) values.push_back(Value::OfSlot(v.slot, v.type));
            EmitDispatch(listeners.sites, values, {}, location);
            emit_.JumpTo(0xFFFFFFFFu);
        }

        void Compiler::EmitAliveCheck() {
            if (!root_ || !root_->Cancellable()) return;
            Label alive = emit_.NewLabel();
            emit_.JumpIfFalse(root_->freeSlot, alive);
            EmitYield();
            emit_.Bind(alive);
        }

        void Compiler::EmitYield() {
            emit_.Push(returnJumpSlot_);
            emit_.CopyFromStack();
            emit_.JumpIndirect(returnJumpSlot_);
        }

        bool Compiler::DebugBuild() const {
            auto debug = defines_.find("DEBUG");
            return debug != defines_.end() && IsTruthy(debug->second);
        }

        void Compiler::CompileBody(AstStatBlock* body, AstStat*& tail) {
            tail = body->body.size && body->body.data[body->body.size - 1]->is<AstStatReturn>() ? body->body.data[body->body.size - 1] : nullptr;
            AstStat* saved = tail_;
            tail_ = tail;
            CompileBlock(body);
            tail_ = saved;
        }

        void Compiler::CheckRecursion() {
            std::unordered_map<Function*, int> state;
            std::vector<Function*> path;
            std::function<bool(Function*)> visit = [&](Function* f) {
                int& s = state[f];
                if (s == 2) return false;
                if (s == 1) {
                    std::string chain;
                    auto start = std::ranges::find(path, f);
                    for (auto it = start; it != path.end(); ++it) chain += (*it)->name + " -> ";
                    Report(f->location, std::format("recursion is not supported: {}{}", chain, f->name));
                    return true;
                }
                s = 1;
                path.push_back(f);
                for (Function* c : f->callees)
                    if (visit(c)) return true;
                path.pop_back();
                s = 2;
                return false;
            };
            for (auto& f : functions_)
                if (visit(f.get())) return;
        }

        void Compiler::CompileBlock(AstStatBlock* block) {
            for (AstStat* stat : block->body) CompileStatementGuarded(stat);
        }

        void Compiler::CompileStatementGuarded(AstStat* stat) {
            size_t mark = emit_.TempMark();
            if (!stat->is<AstStatBlock>()) emit_.MarkLine(static_cast<int>(stat->location.begin.line));
            try {
                CompileStatement(stat);
            } catch (const CompileError& e) {
                Report(e.location, e.message);
            }
            emit_.ReleaseTemps(mark);
        }

        void Compiler::CompileStatement(AstStat* stat) {
            if (auto* s = stat->as<AstStatBlock>()) return CompileBlock(s);
            if (auto* s = stat->as<AstStatLocal>()) return CompileLocal(s);
            if (auto* s = stat->as<AstStatAssign>()) return CompileAssign(s);
            if (auto* s = stat->as<AstStatCompoundAssign>()) return CompileCompoundAssign(s);
            if (auto* s = stat->as<AstStatIf>()) return CompileIf(s);
            if (auto* s = stat->as<AstStatWhile>()) return CompileWhile(s);
            if (auto* s = stat->as<AstStatRepeat>()) return CompileRepeat(s);
            if (auto* s = stat->as<AstStatFor>()) return CompileFor(s);
            if (auto* s = stat->as<AstStatForIn>()) return CompileForIn(s);
            if (auto* s = stat->as<AstStatReturn>()) return CompileReturn(s);
            if (auto* s = stat->as<AstStatExpr>()) {
                auto* call = Unwrap(s->expr)->as<AstExprCall>();
                if (!call) Fail(s->location, "only calls can be used as statements");
                CompileCall(call, 0);
                return;
            }
            if (stat->is<AstStatBreak>()) {
                if (loops_.empty()) Fail(stat->location, "'break' outside a loop");
                emit_.Jump(loops_.back().exit);
                return;
            }
            if (stat->is<AstStatContinue>()) {
                if (loops_.empty()) Fail(stat->location, "'continue' outside a loop");
                emit_.Jump(loops_.back().next);
                return;
            }
            if (stat->is<AstStatLocalFunction>() || stat->is<AstStatFunction>()) Fail(stat->location, "functions can only be declared at module level");
            if (auto* s = stat->as<AstStatTypeAlias>()) return DeclareTypeAlias(s);
            Fail(stat->location, "unsupported statement");
        }

        void Compiler::CompileLocal(AstStatLocal* stat) {
            if (stat->isConst && stat->vars.size == 1 && stat->values.size == 1)
                if (auto* table = Unwrap(stat->values.data[0])->as<AstExprTable>()) {
                    DeclareConstTable(stat->vars.data[0], table);
                    return;
                }
            std::vector<const Type*> expected;
            for (AstLocal* var : stat->vars) expected.push_back(var->annotation ? ResolveType(var->annotation) : nullptr);
            if (std::ranges::any_of(expected, [](const Type* t) { return t && t->IsList(); })) {
                if (stat->vars.size != 1 || stat->values.size > 1) Fail(stat->location, "declare one List per 'local'");
                if (stat->isConst) Fail(stat->location, "a List cannot be a constant");
                AstLocal* var = stat->vars.data[0];
                CheckUserName(var->name.value, var->location);
                aliases_.erase(var);
                Variable list = DeclareListStorage(var->name.value, expected[0], false, {}, 0);
                InitList(Value::OfSlot(list.slot, list.type), stat->values.size ? stat->values.data[0] : nullptr, stat->location);
                variables_[var] = list;
                return;
            }

            std::vector<Value> values = EvaluateList(stat->values, stat->vars.size, expected);
            for (size_t i = 0; i < stat->vars.size; ++i) {
                AstLocal* var = stat->vars.data[i];
                CheckUserName(var->name.value, var->location);
                BindLocal(var, values[i], expected[i], stat->isConst, stat->values.size > 0);
            }
        }

        Value Compiler::CoerceLiteral(const Value& value, const Type* type, const Location& location) {
            if (NaturalType(value) == type || value.kind == Value::Kind::Nil) return value;
            if (value.kind == Value::Kind::Integer && !value.type && type == types_.Single) return Value::OfReal(static_cast<double>(value.integer));
            return Value::OfSlot(Materialize(value, type, location), type);
        }

        Value Compiler::BindLocal(AstLocal* var, const Value& value, const Type* annotated, bool isConst, bool hasValue) {
            aliases_.erase(var);
            variables_.erase(var);
            if (!annotated && (value.kind == Value::Kind::TypeRef || value.kind == Value::Kind::Namespace)) {
                aliases_[var] = value;
                return value;
            }
            const Type* type = annotated ? annotated : NaturalType(value);
            if (!type) Fail(var->location, std::format("cannot infer the type of '{}'; add a type annotation", var->name.value));
            if (hasValue && Cost(value, type) < 0) Fail(var->location, std::format("cannot assign {} to {}", Describe(value), type->displayName));

            bool immutable = isConst || Immutable(var, type);
            if (hasValue && immutable) {
                if (value.IsLiteral()) {
                    Value bound = CoerceLiteral(value, type, var->location);
                    aliases_[var] = bound;
                    return bound;
                }
                if (value.kind == Value::Kind::Slot && value.type == type && emit_.IsConstant(value.slot)) {
                    aliases_[var] = value;
                    return value;
                }
            }
            if (isConst && hasValue && value.kind == Value::Kind::Slot && value.type == type && !emit_.IsTemp(value.slot)) {
                Variable v{ emit_.Local(var->name.value, type), type };
                Store(value, v, var->location);
                variables_[var] = v;
                return Value::OfSlot(v.slot, type);
            }
            if (hasValue && value.kind == Value::Kind::Slot && value.type == type && emit_.IsTemp(value.slot)) {
                emit_.Promote(value.slot);
                variables_[var] = { value.slot, type };
                return value;
            }
            Variable v{ emit_.Local(var->name.value, type), type };
            if (hasValue) {
                Store(value, v, var->location);
            } else {
                HeapValue initial;
                initial.kind = type->IsValueType() ? ValueKind::Default : ValueKind::Null;
                emit_.Copy(emit_.Constant(type, initial), v.slot);
            }
            variables_[var] = v;
            return Value::OfSlot(v.slot, type);
        }

        std::vector<Value> Compiler::EvaluateList(const AstArray<AstExpr*>& exprs, size_t want, const std::vector<const Type*>& expected) {
            std::vector<Value> values;
            if (exprs.size == 1 && want > 1) {
                if (auto* call = Unwrap(exprs.data[0])->as<AstExprCall>()) values = CompileCall(call, want);
                else values.push_back(CompileExpr(exprs.data[0], expected.empty() ? nullptr : expected[0]));
            } else {
                for (size_t i = 0; i < exprs.size; ++i) {
                    const Type* hint = i < expected.size() ? expected[i] : nullptr;
                    Value v = CompileExpr(exprs.data[i], hint);
                    if (i < want) values.push_back(want > 1 ? Evaluate(v) : v);
                }
            }
            while (values.size() < want) values.push_back(Value::OfNil());
            values.resize(want);
            return values;
        }

        Value Compiler::Evaluate(const Value& value) {
            if (value.kind != Value::Kind::Slot || value.type->IsList() || emit_.IsTemp(value.slot) || emit_.IsConstant(value.slot)) return value;
            uint32_t t = emit_.Temp(value.type);
            emit_.Copy(value.slot, t);
            return Value::OfSlot(t, value.type);
        }

        void Compiler::CompileAssign(AstStatAssign* stat) {
            if (stat->vars.size == 1 && stat->values.size == 1)
                if (auto* local = Unwrap(stat->vars.data[0])->as<AstExprLocal>())
                    if (auto it = variables_.find(local->local); it != variables_.end() && it->second.type->IsList()) {
                        InitList(Value::OfSlot(it->second.slot, it->second.type), stat->values.data[0], stat->location);
                        return;
                    }
            std::vector<Value> values;
            if (stat->vars.size == 1 && stat->values.size == 1) {
                const Variable* into = nullptr;
                if (auto* local = Unwrap(stat->vars.data[0])->as<AstExprLocal>())
                    if (auto it = variables_.find(local->local); it != variables_.end()) into = &it->second;
                Variable target = into ? *into : Variable{};
                values.push_back(CompileExpr(stat->values.data[0], into ? target.type : nullptr, into ? &target : nullptr));
            } else {
                values = EvaluateList(stat->values, stat->vars.size, {});
            }
            for (size_t i = 0; i < stat->vars.size; ++i) AssignTo(stat->vars.data[i], values[i]);
        }

        void Compiler::AssignTo(AstExpr* target, const Value& value) {
            target = Unwrap(target);
            if (auto* local = target->as<AstExprLocal>()) {
                if (auto it = staticFields_.find(local->local); it != staticFields_.end()) {
                    WriteMember(StaticCompanion(target->location), it->second, value, target->location);
                    return;
                }
                CheckInstanceUse(local->local, target->location);
                if (aliases_.contains(local->local)) Fail(target->location, std::format("'{}' is a constant", local->local->name.value));
                Store(value, LookupLocal(local->local, local->location), target->location);
                return;
            }
            if (auto* index = target->as<AstExprIndexName>()) {
                WriteMember(CompileExpr(index->expr), index->index.value, value, target->location);
                return;
            }
            if (auto* index = target->as<AstExprIndexExpr>()) {
                Value owner = CompileExpr(index->expr);
                WriteIndex(owner, CompileExpr(index->index), value, target->location);
                return;
            }
            if (auto* global = target->as<AstExprGlobal>())
                Fail(target->location, std::format("unknown variable '{}'; declare it with 'local'", global->name.value));
            Fail(target->location, "cannot assign to this expression");
        }

        void Compiler::WriteMember(const Value& owner, std::string_view name, const Value& value, const Location& location) {
            std::string setter = "set_" + std::string(name);
            if (owner.kind == Value::Kind::TypeRef) {
                CallStatic(owner.type, setter, { value }, nullptr, location);
                return;
            }
            if (owner.kind != Value::Kind::Slot || !owner.type) Fail(location, std::format("cannot assign to a member of {}", Describe(owner)));
            if (owner.type->IsList()) Fail(location, std::format("List.{} is read-only", name));
            if (const ScriptInfo* script = owner.type->script) {
                auto field = std::ranges::find(script->fields, name, &ScriptVariable::name);
                if (field != script->fields.end()) {
                    const Type* type = VariableType(*field);
                    if (Cost(value, type) < 0) Fail(location, std::format("cannot assign {} to {}.{} ({})", Describe(value), script->name, field->name, type->displayName));
                    Value v = value.IsLiteral() ? Value::OfSlot(Materialize(value, type, location), type) : value;
                    if (v.kind == Value::Kind::Slot && v.type != type && v.type->IsNumeric() && type->IsNumeric())
                        v = Value::OfSlot(Convert(v.slot, v.type, type, location), type);
                    CallMember(Value::OfSlot(owner.slot, types_.Behaviour()), "SetProgramVariable", { Value::OfString(field->symbol), v }, nullptr, location);
                    return;
                }
                auto propertySetter = std::ranges::find(script->methods, "set_" + std::string(name), &ScriptMethod::name);
                if (propertySetter != script->methods.end() && propertySetter->parameters.size() == 1) {
                    CallScriptMethod(owner, *propertySetter, { value }, 0, location);
                    return;
                }
            }
            if (owner.type->IsValueType() && emit_.IsConstant(owner.slot))
                Fail(location, std::format("cannot modify '{}' of a constant {}", name, owner.type->displayName));
            if (owner.type->IsValueType() && emit_.IsTemp(owner.slot))
                Fail(location, std::format("cannot modify '{}' of a {} copy; store it in a local, change it, then assign it back", name, owner.type->displayName));
            CallMember(owner, setter, { value }, nullptr, location);
        }

        Value Compiler::ReadIndex(const Value& owner, const Value& key, const Location& location) {
            if (owner.kind != Value::Kind::Slot || !owner.type) Fail(location, std::format("cannot index {}", Describe(owner)));
            if (owner.type->IsList()) return CallList(owner, "Get", { key }, location).at(0);
            std::string_view method = owner.type->kind == TypeKind::Array ? "Get" : "get_Item";
            Value item = CallMember(owner, method, { key }, nullptr, location).at(0);
            if (owner.type->kind == TypeKind::Array) item = Narrow(item, owner.type->element);
            return item;
        }

        void Compiler::WriteIndex(const Value& owner, const Value& key, const Value& value, const Location& location) {
            if (owner.kind != Value::Kind::Slot || !owner.type) Fail(location, std::format("cannot index {}", Describe(owner)));
            if (owner.type->IsList()) {
                CallList(owner, "Set", { key, value }, location);
                return;
            }
            std::string_view method = owner.type->kind == TypeKind::Array ? "Set" : "set_Item";
            CallMember(owner, method, { key, value }, nullptr, location);
        }

        void Compiler::CompileCompoundAssign(AstStatCompoundAssign* stat) {
            AstExpr* target = Unwrap(stat->var);
            if (auto* local = target->as<AstExprLocal>()) {
                AstExprBinary synthetic(stat->location, stat->op, stat->var, stat->value);
                CheckInstanceUse(local->local, local->location);
                const Type* type = staticFields_.contains(local->local) ? nullptr : LookupLocal(local->local, local->location).type;
                AssignTo(stat->var, CompileBinary(&synthetic, type));
                return;
            }
            bool valueCall = HasCall(stat->value);
            auto combine = [&](const Value& current) {
                Value right = CompileExpr(stat->value, NaturalType(current));
                if (stat->op == AstExprBinary::Concat) return ConcatValues({ current, right }, stat->location);
                return CompileArithmetic(stat->op, current, right, stat->location);
            };
            if (auto* index = target->as<AstExprIndexName>()) {
                Value owner = CompileExpr(index->expr);
                if (valueCall) owner = Evaluate(owner);
                Value current = Evaluate(ReadMember(owner, index->index.value, stat->location));
                WriteMember(owner, index->index.value, combine(current), stat->location);
                return;
            }
            if (auto* index = target->as<AstExprIndexExpr>()) {
                Value owner = CompileExpr(index->expr);
                if (valueCall || HasCall(index->index)) owner = Evaluate(owner);
                Value key = CompileExpr(index->index);
                if (valueCall) key = Evaluate(key);
                Value current = Evaluate(ReadIndex(owner, key, stat->location));
                WriteIndex(owner, key, combine(current), stat->location);
                return;
            }
            Fail(stat->location, "cannot assign to this expression");
        }

        void Compiler::CompileIf(AstStatIf* stat) {
            if (auto c = TryConstant(stat->condition)) {
                if (IsTruthy(*c)) CompileBlock(stat->thenbody);
                else if (stat->elsebody) CompileStatementGuarded(stat->elsebody);
                return;
            }
            if (TryJumpTable(stat)) return;
            Label otherwise = emit_.NewLabel();
            Label end = emit_.NewLabel();
            CompileCondition(stat->condition, otherwise);
            CompileBlock(stat->thenbody);
            if (stat->elsebody) emit_.Jump(end);
            emit_.Bind(otherwise);
            if (stat->elsebody) CompileStatementGuarded(stat->elsebody);
            emit_.Bind(end);
        }

        void Compiler::CompileLoopBody(AstStatBlock* body, LoopLabels labels) {
            loops_.push_back(labels);
            CompileBlock(body);
            loops_.pop_back();
        }

        void Compiler::CompileWhile(AstStatWhile* stat) {
            Label top = emit_.NewLabel();
            Label test = emit_.NewLabel();
            Label end = emit_.NewLabel();
            std::optional<Value> constant = TryConstant(stat->condition);
            if (constant && !IsTruthy(*constant)) return;
            if (constant) {
                emit_.Bind(top);
                CompileLoopBody(stat->body, { end, top });
                emit_.Jump(top);
                emit_.Bind(end);
                return;
            }
            if (!NegationIsFree(stat->condition)) {
                emit_.Bind(top);
                CompileCondition(stat->condition, end);
                CompileLoopBody(stat->body, { end, top });
                emit_.Jump(top);
                emit_.Bind(end);
                return;
            }
            emit_.Jump(test);
            emit_.Bind(top);
            CompileLoopBody(stat->body, { end, test });
            emit_.Bind(test);
            CompileCondition(stat->condition, top, true);
            emit_.Bind(end);
        }

        std::optional<Value> Compiler::TryConstant(AstExpr* expr) {
            expr = Unwrap(expr);
            if (expr->is<AstExprConstantNil>()) return Value::OfNil();
            if (auto* e = expr->as<AstExprConstantBool>()) return Value::OfBoolean(e->value);
            if (auto* e = expr->as<AstExprConstantNumber>()) return CompileConstantNumber(e);
            if (auto* e = expr->as<AstExprConstantInteger>()) return Value::OfInteger(e->value);
            if (auto* e = expr->as<AstExprConstantString>()) return Value::OfString(std::string(e->value.data, e->value.size));
            if (auto* e = expr->as<AstExprLocal>()) {
                if (auto it = aliases_.find(e->local); it != aliases_.end() && it->second.IsLiteral()) return it->second;
                return std::nullopt;
            }
            if (auto* e = expr->as<AstExprGlobal>()) {
                if (globalFunctions_.contains(e->name.value)) return std::nullopt;
                if (auto it = defines_.find(e->name.value); it != defines_.end()) return it->second;
                return std::nullopt;
            }
            if (auto* e = expr->as<AstExprIndexName>())
                if (auto* owner = Unwrap(e->expr)->as<AstExprLocal>())
                    if (auto it = constTables_.find(owner->local); it != constTables_.end())
                        if (auto member = it->second.find(e->index.value); member != it->second.end()) return member->second;
            if (auto* e = expr->as<AstExprIfElse>()) {
                auto c = e->hasElse ? TryConstant(e->condition) : std::nullopt;
                if (!c) return std::nullopt;
                return TryConstant(IsTruthy(*c) ? e->trueExpr : e->falseExpr);
            }
            if (auto* e = expr->as<AstExprUnary>()) {
                auto v = TryConstant(e->expr);
                if (!v) return std::nullopt;
                switch (e->op) {
                    case AstExprUnary::Op::Not: return Value::OfBoolean(!IsTruthy(*v));
                    case AstExprUnary::Op::Minus:
                        if (v->kind == Value::Kind::Integer && !v->type) return Value::OfInteger(-v->integer);
                        if (v->kind == Value::Kind::Real) return Value::OfReal(-v->real);
                        return std::nullopt;
                    case AstExprUnary::Op::Len:
                        if (v->kind == Value::Kind::String) return Value::OfInteger(static_cast<int64_t>(v->text.size()));
                        return std::nullopt;
                }
                return std::nullopt;
            }
            auto* b = expr->as<AstExprBinary>();
            if (!b) return std::nullopt;

            auto left = TryConstant(b->left);
            if (b->op == AstExprBinary::And || b->op == AstExprBinary::Or) {
                if (!left) return std::nullopt;
                bool l = IsTruthy(*left);
                if (b->op == AstExprBinary::And && !l) return Value::OfBoolean(false);
                if (b->op == AstExprBinary::Or && l) return Value::OfBoolean(true);
                auto right = TryConstant(b->right);
                if (!right) return std::nullopt;
                return Value::OfBoolean(IsTruthy(*right));
            }
            auto right = TryConstant(b->right);
            if (!left || !right) return std::nullopt;

            if (b->op == AstExprBinary::Concat) {
                auto text = [](const Value& v) -> std::optional<std::string> {
                    if (v.kind == Value::Kind::String) return v.text;
                    if (v.kind == Value::Kind::Integer && !v.type) return std::to_string(v.integer);
                    if (v.kind == Value::Kind::Real) return std::format("{}", static_cast<float>(v.real));
                    return std::nullopt;
                };
                auto l = text(*left);
                auto r = text(*right);
                if (!l || !r) return std::nullopt;
                return Value::OfString(*l + *r);
            }
            if (IsComparison(b->op)) {
                if (left->IsNumberLiteral() && right->IsNumberLiteral()) {
                    double l = left->kind == Value::Kind::Integer ? static_cast<double>(left->integer) : left->real;
                    double r = right->kind == Value::Kind::Integer ? static_cast<double>(right->integer) : right->real;
                    switch (b->op) {
                        case AstExprBinary::CompareEq: return Value::OfBoolean(l == r);
                        case AstExprBinary::CompareNe: return Value::OfBoolean(l != r);
                        case AstExprBinary::CompareLt: return Value::OfBoolean(l < r);
                        case AstExprBinary::CompareLe: return Value::OfBoolean(l <= r);
                        case AstExprBinary::CompareGt: return Value::OfBoolean(l > r);
                        default: return Value::OfBoolean(l >= r);
                    }
                }
                if (left->kind != right->kind || !left->IsLiteral()) return std::nullopt;
                if (b->op != AstExprBinary::CompareEq && b->op != AstExprBinary::CompareNe) return std::nullopt;
                bool same = left->kind == Value::Kind::Nil || (left->kind == Value::Kind::Boolean && left->boolean == right->boolean) ||
                            (left->kind == Value::Kind::String && left->text == right->text) ||
                            (left->kind == Value::Kind::Integer && left->integer == right->integer && left->type == right->type);
                return Value::OfBoolean(b->op == AstExprBinary::CompareEq ? same : !same);
            }
            return FoldArithmetic(b->op, *left, *right);
        }

        bool Compiler::TryJumpTable(AstStatIf* stat) {
            struct Case {
                int64_t        value;
                AstStatBlock*  body;
            };
            AstLocal* subject = nullptr;
            std::vector<Case> cases;
            AstStat* otherwiseBody = nullptr;
            for (AstStatIf* s = stat; s;) {
                auto* b = Unwrap(s->condition)->as<AstExprBinary>();
                if (!b || b->op != AstExprBinary::CompareEq) return false;
                AstExpr* l = Unwrap(b->left);
                AstExpr* r = Unwrap(b->right);
                auto* local = l->as<AstExprLocal>();
                AstExpr* other = r;
                if (!local) {
                    local = r->as<AstExprLocal>();
                    other = l;
                }
                if (!local || (subject && local->local != subject)) return false;
                subject = local->local;
                auto c = TryConstant(other);
                if (!c || c->kind != Value::Kind::Integer || c->type) return false;
                cases.push_back({ c->integer, s->thenbody });
                if (!s->elsebody) break;
                if (auto* next = s->elsebody->as<AstStatIf>()) {
                    s = next;
                    continue;
                }
                otherwiseBody = s->elsebody;
                break;
            }
            if (cases.size() < kJumpTableMinimum || aliases_.contains(subject)) return false;
            auto variable = variables_.find(subject);
            if (variable == variables_.end() || variable->second.type != types_.Int32) return false;

            if (std::ranges::any_of(cases, [](const Case& c) { return !FitsIntegral(c.value, Numeric::Int32); })) return false;
            auto [low, high] = std::ranges::minmax(cases, {}, &Case::value);
            int64_t span = high.value - low.value + 1;
            if (span > static_cast<int64_t>(cases.size()) * 3 || span > 1024) return false;
            const Type* arrayType = types_.ArrayOf(types_.UInt32);
            if (types_.Methods(arrayType, "Get", false, true).empty()) return false;

            Label otherwise = emit_.NewLabel();
            Label end = emit_.NewLabel();
            std::vector<Label> labels;
            std::vector<Label> table(static_cast<size_t>(span), otherwise);
            std::vector<bool> filled(static_cast<size_t>(span), false);
            for (const Case& c : cases) {
                labels.push_back(emit_.NewLabel());
                size_t index = static_cast<size_t>(c.value - low.value);
                if (!filled[index]) {
                    table[index] = labels.back();
                    filled[index] = true;
                }
            }

            Value x = Value::OfSlot(variable->second.slot, types_.Int32);
            Location where = stat->condition->location;
            emit_.JumpIfFalse(CompareValues(AstExprBinary::CompareGe, x, Value::OfInteger(low.value), where).slot, otherwise);
            emit_.JumpIfFalse(CompareValues(AstExprBinary::CompareLe, x, Value::OfInteger(high.value), where).slot, otherwise);
            Value index = low.value == 0 ? x : CompileArithmetic(AstExprBinary::Sub, x, Value::OfInteger(low.value), where);
            uint32_t tableSlot = emit_.JumpTable(arrayType, types_.UInt32, table);
            Value target = CallMember(Value::OfSlot(tableSlot, arrayType), "Get", { index }, nullptr, where).at(0);
            emit_.JumpIndirect(target.slot);
            for (size_t i = 0; i < cases.size(); ++i) {
                emit_.Bind(labels[i]);
                CompileBlock(cases[i].body);
                emit_.Jump(end);
            }
            emit_.Bind(otherwise);
            if (otherwiseBody) CompileStatementGuarded(otherwiseBody);
            emit_.Bind(end);
            return true;
        }

        bool Compiler::NegationIsFree(AstExpr* expr) {
            expr = Unwrap(expr);
            if (auto* b = expr->as<AstExprBinary>()) {
                if (IsComparison(b->op)) return true;
                if (b->op == AstExprBinary::And || b->op == AstExprBinary::Or) return NegationIsFree(b->left) && NegationIsFree(b->right);
                return false;
            }
            if (auto* u = expr->as<AstExprUnary>(); u && u->op == AstExprUnary::Op::Not) return true;
            return TryConstant(expr).has_value();
        }

        void Compiler::CompileRepeat(AstStatRepeat* stat) {
            Label top = emit_.NewLabel();
            Label next = emit_.NewLabel();
            Label end = emit_.NewLabel();
            emit_.Bind(top);
            CompileLoopBody(stat->body, { end, next });
            emit_.Bind(next);
            CompileCondition(stat->condition, top);
            emit_.Bind(end);
        }

        void Compiler::CompileFor(AstStatFor* stat) {
            Value from = CompileExpr(stat->from);
            Value to = CompileExpr(stat->to);
            Value step = stat->step ? CompileExpr(stat->step) : Value::OfInteger(1);
            if (!step.IsNumberLiteral()) Fail(stat->step->location, "the loop step must be a constant number");
            double stepValue = step.kind == Value::Kind::Integer ? static_cast<double>(step.integer) : step.real;
            if (stepValue == 0) Fail(stat->step->location, "the loop step cannot be zero");

            const Type* type = stat->var->annotation ? ResolveType(stat->var->annotation) : nullptr;
            if (!type) {
                auto integral = [&](const Value& v) {
                    return (v.kind == Value::Kind::Integer && !v.type) || (v.kind == Value::Kind::Slot && v.type->IsIntegral());
                };
                type = integral(from) && integral(to) && integral(step) ? types_.Int32 : types_.Single;
            }
            if (!type->IsNumeric()) Fail(stat->var->location, "loop variables must be numeric");
            for (auto [value, expr] : { std::pair{ &from, stat->from }, std::pair{ &to, stat->to }, std::pair{ &step, stat->step ? stat->step : stat->to } })
                if (Cost(*value, type) < 0)
                    Fail(expr->location, std::format("{} cannot be used as a {} loop bound or step", Describe(*value), type->displayName));

            bool knownBounds = from.IsNumberLiteral() && to.IsNumberLiteral();
            if (knownBounds) {
                double a = from.kind == Value::Kind::Integer ? static_cast<double>(from.integer) : from.real;
                double b = to.kind == Value::Kind::Integer ? static_cast<double>(to.integer) : to.real;
                if (stepValue > 0 ? a > b : a < b) return;
            }

            bool userWrites = assigned_.contains(stat->var);
            Variable counter{ userWrites ? emit_.Hidden(type) : emit_.Local(stat->var->name.value, type), type };
            Variable user = userWrites ? Variable{ emit_.Local(stat->var->name.value, type), type } : counter;
            variables_[stat->var] = user;
            Store(from, counter, stat->from->location);
            Variable limit{};
            if (to.IsLiteral()) {
                limit = { Materialize(to, type, stat->to->location), type };
            } else {
                limit = { emit_.Hidden(type), type };
                Store(to, limit, stat->to->location);
            }
            uint32_t stepSlot = Materialize(step, type, stat->location);

            Label top = emit_.NewLabel();
            Label next = emit_.NewLabel();
            Label end = emit_.NewLabel();
            Label increment = emit_.NewLabel();
            if (!knownBounds) emit_.Jump(next);
            emit_.Bind(top);
            if (userWrites) emit_.Copy(counter.slot, user.slot);
            CompileLoopBody(stat->body, { end, increment });
            emit_.Bind(increment);
            Value sum = CompileArithmetic(AstExprBinary::Add, Value::OfSlot(counter.slot, type), Value::OfSlot(stepSlot, type), stat->location);
            Store(sum, counter, stat->location);
            emit_.Bind(next);
            if (type->IsIntegral()) {
                Value done = CompareValues(stepValue > 0 ? AstExprBinary::CompareGt : AstExprBinary::CompareLt,
                    Value::OfSlot(counter.slot, type), Value::OfSlot(limit.slot, type), stat->location);
                emit_.JumpIfFalse(done.slot, top);
            } else {
                Value inside = CompareValues(stepValue > 0 ? AstExprBinary::CompareLe : AstExprBinary::CompareGe,
                    Value::OfSlot(counter.slot, type), Value::OfSlot(limit.slot, type), stat->location);
                emit_.JumpIfFalse(inside.slot, end);
                emit_.Jump(top);
            }
            emit_.Bind(end);
        }

        void Compiler::CompileForIn(AstStatForIn* stat) {
            if (stat->values.size != 1 || stat->vars.size < 1 || stat->vars.size > 2)
                Fail(stat->location, "use 'for i, value in array do'");
            AstExpr* source = Unwrap(stat->values.data[0]);
            if (auto* call = source->as<AstExprCall>())
                if (auto* g = Unwrap(call->func)->as<AstExprGlobal>(); g && (g->name == "ipairs" || g->name == "pairs"))
                    Fail(source->location, "iterate arrays directly: 'for i, value in array do' (indices start at 0)");

            Value collection = CompileExpr(source);
            bool isList = collection.kind == Value::Kind::Slot && collection.type->IsList();
            if (!isList && (collection.kind != Value::Kind::Slot || collection.type->kind != TypeKind::Array))
                Fail(source->location, std::format("cannot iterate {}", Describe(collection)));

            const Type* arrayType = isList ? collection.type->listArray : collection.type;
            Variable array{ emit_.Hidden(arrayType), arrayType };
            Store(isList ? ListArray(collection) : collection, array, source->location);
            Variable index{ emit_.Hidden(types_.Int32), types_.Int32 };
            Store(Value::OfInteger(0), index, stat->location);
            Variable length{ emit_.Hidden(types_.Int32), types_.Int32 };
            if (isList) {
                Store(Value::OfSlot(ListCount(collection).slot, types_.Int32), length, source->location);
            } else {
                std::vector<Value> len = CallMember(Value::OfSlot(array.slot, arrayType), "get_Length", {}, nullptr, source->location);
                Store(len.at(0), length, source->location);
            }

            AstLocal* k = stat->vars.data[0];
            bool keyIsIndex = Immutable(k, types_.Int32);
            Variable key = keyIsIndex ? index : Variable{ emit_.Local(k->name.value, types_.Int32), types_.Int32 };
            variables_[k] = key;
            Variable item{};
            if (stat->vars.size == 2) {
                AstLocal* v = stat->vars.data[1];
                const Type* itemType = arrayType->element;
                if (v->annotation && ResolveType(v->annotation) != itemType)
                    Fail(v->location, std::format("array elements are {}", itemType->displayName));
                item = { emit_.Local(v->name.value, itemType), itemType };
                variables_[v] = item;
            }

            Label top = emit_.NewLabel();
            Label increment = emit_.NewLabel();
            Label test = emit_.NewLabel();
            Label end = emit_.NewLabel();
            emit_.Jump(test);
            emit_.Bind(top);
            if (!keyIsIndex) emit_.Copy(index.slot, key.slot);
            if (item.type) {
                std::vector<Value> got = CallMember(Value::OfSlot(array.slot, arrayType), "Get", { Value::OfSlot(index.slot, types_.Int32) }, nullptr, stat->location);
                got.at(0) = Narrow(got.at(0), item.type);
                Store(got.at(0), item, stat->location);
            }
            CompileLoopBody(stat->body, { end, increment });
            emit_.Bind(increment);
            Value sum = CompileArithmetic(AstExprBinary::Add, Value::OfSlot(index.slot, types_.Int32), Value::OfInteger(1), stat->location);
            Store(sum, index, stat->location);
            emit_.Bind(test);
            Value done = CompareValues(AstExprBinary::CompareGe, Value::OfSlot(index.slot, types_.Int32), Value::OfSlot(length.slot, types_.Int32), stat->location);
            emit_.JumpIfFalse(done.slot, top);
            emit_.Bind(end);
        }

        void Compiler::CompileReturn(AstStatReturn* stat) {
            std::optional<InlineFrame> frame;
            if (!inlineStack_.empty()) frame = inlineStack_.back();
            Function& f = frame ? *frame->function : *current_;
            if (stat->list.size && f.returns.empty()) {
                Fail(stat->location, std::format("'{}' has no return type annotation", f.name));
            }
            if (stat->list.size && stat->list.size != f.returns.size() && !(stat->list.size == 1 && Unwrap(stat->list.data[0])->is<AstExprCall>()))
                Fail(stat->location, std::format("'{}' returns {} value(s)", f.name, f.returns.size()));

            if (stat->list.size) {
                std::vector<Variable> targets;
                for (size_t i = 0; i < f.returns.size(); ++i)
                    targets.push_back(frame ? (i < frame->results.size() ? frame->results[i] : Variable{}) : Variable{ f.returnSlots[i], f.returns[i] });
                if (stat->list.size == 1 && f.returns.size() == 1) {
                    Value v = CompileExpr(stat->list.data[0], f.returns[0], targets[0].type ? &targets[0] : nullptr);
                    if (targets[0].type) Store(v, targets[0], stat->location);
                } else {
                    std::vector<Value> values = EvaluateList(stat->list, f.returns.size(), f.returns);
                    for (size_t i = 0; i < f.returns.size(); ++i)
                        if (targets[i].type) Store(values[i], targets[i], stat->location);
                }
            }

            if (frame) {
                if (stat != frame->tail) emit_.Jump(frame->end);
                return;
            }
            if (stat == tail_) return;
            if (f.trampoline) emit_.Jump(f.epilogue);
            else emit_.JumpTo(0xFFFFFFFFu);
        }

        Variable Compiler::LookupLocal(AstLocal* local, const Location& location) {
            if (auto it = variables_.find(local); it != variables_.end()) return it->second;
            if (localFunctions_.contains(local)) Fail(location, std::format("'{}' is a function", local->name.value));
            Fail(location, std::format("'{}' is not available here", local->name.value));
        }

        Value Compiler::CompileExpr(AstExpr* expr, const Type* expected, const Variable* into) {
            expr = Unwrap(expr);
            if (expr->is<AstExprBinary>() || expr->is<AstExprUnary>())
                if (auto constant = TryConstant(expr)) return *constant;
            if (expr->is<AstExprConstantNil>()) return Value::OfNil();
            if (auto* e = expr->as<AstExprConstantBool>()) return Value::OfBoolean(e->value);
            if (auto* e = expr->as<AstExprConstantNumber>()) return CompileConstantNumber(e);
            if (auto* e = expr->as<AstExprConstantInteger>()) return Value::OfInteger(e->value);
            if (auto* e = expr->as<AstExprConstantString>()) return Value::OfString(std::string(e->value.data, e->value.size));
            if (auto* e = expr->as<AstExprLocal>()) {
                if (auto it = staticFields_.find(e->local); it != staticFields_.end()) return ReadMember(StaticCompanion(e->location), it->second, e->location);
                if (staticFunctions_.contains(e->local)) Fail(e->location, std::format("static function '{}' can only be called", e->local->name.value));
                CheckInstanceUse(e->local, e->location);
                if (auto it = aliases_.find(e->local); it != aliases_.end()) return it->second;
                if (auto it = localFunctions_.find(e->local); it != localFunctions_.end()) return Value::OfFunction(it->second);
                Variable v = LookupLocal(e->local, e->location);
                return Value::OfSlot(v.slot, v.type);
            }
            if (auto* e = expr->as<AstExprGlobal>()) return CompileGlobal(e);
            if (auto* e = expr->as<AstExprIndexName>()) return CompileIndexName(e);
            if (auto* e = expr->as<AstExprIndexExpr>()) return CompileIndexExpr(e);
            if (auto* e = expr->as<AstExprCall>()) {
                if (auto network = TryNetworkTarget(e)) return *network;
                std::vector<Value> results = CompileCall(e, 1, into);
                if (results.empty() || results[0].kind == Value::Kind::Nil) Fail(e->location, "this call does not return a value");
                return results[0];
            }
            if (auto* e = expr->as<AstExprUnary>()) return CompileUnary(e);
            if (auto* e = expr->as<AstExprBinary>()) {
                if (e->op == AstExprBinary::And || e->op == AstExprBinary::Or) return BooleanFromCondition(e, into);
                return CompileBinary(e, expected);
            }
            if (auto* e = expr->as<AstExprInterpString>()) return CompileInterpolated(e);
            if (auto* e = expr->as<AstExprIfElse>()) return CompileIfElse(e, expected, into);
            if (auto* e = expr->as<AstExprTable>()) return CompileTable(e, expected);
            if (auto* e = expr->as<AstExprTypeAssertion>()) {
                const Type* target = ResolveType(e->annotation);
                Value v = CompileExpr(e->expr, target);
                if (v.kind == Value::Kind::Slot && v.type != target) {
                    if (v.type->IsNumeric() && target->IsNumeric()) return Value::OfSlot(Convert(v.slot, v.type, target, e->location), target);
                    if (types_.ReferenceDistance(target, v.type) >= 0 || types_.ReferenceDistance(v.type, target) >= 0) {
                        uint32_t t = emit_.Temp(target);
                        emit_.Copy(v.slot, t);
                        return Value::OfSlot(t, target);
                    }
                    Fail(e->location, std::format("cannot convert {} to {}", v.type->displayName, target->displayName));
                }
                if (v.IsLiteral()) return Value::OfSlot(Materialize(v, target, e->location), target);
                return v;
            }
            if (expr->is<AstExprFunction>()) Fail(expr->location, "anonymous functions are not supported");
            if (expr->is<AstExprVarargs>()) Fail(expr->location, "'...' is not supported");
            Fail(expr->location, "unsupported expression");
        }

        Value Compiler::CompileConstantNumber(AstExprConstantNumber* expr) {
            std::string_view text = TextAt(expr->location);
            bool hex = text.size() > 1 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X' || text[1] == 'b' || text[1] == 'B');
            bool real = !hex && text.find_first_of(".eE") != std::string_view::npos;
            double v = expr->value;
            if (!real && std::isfinite(v) && v == std::floor(v) && std::fabs(v) <= 9.2e18) return Value::OfInteger(static_cast<int64_t>(v));
            return Value::OfReal(v);
        }

        Value Compiler::SelfValue(std::string_view name) {
            std::string typeName = name == "this" ? "VRCUdonUdonBehaviour" : name == "gameObject" ? "UnityEngineGameObject" : "UnityEngineTransform";
            const Type* type = types_.Get(typeName);
            auto it = selfSlots_.find(typeName);
            if (it == selfSlots_.end()) {
                HeapValue v;
                v.kind = ValueKind::This;
                it = selfSlots_.emplace(typeName, emit_.AddSlot("__this_" + typeName + "_0", type, v)).first;
                emit_.MarkConstant(it->second);
            }
            return Value::OfSlot(it->second, type);
        }

        Value Compiler::CompileGlobal(AstExprGlobal* expr) {
            std::string_view name = expr->name.value;
            if (name == "this" || name == "gameObject" || name == "transform") return SelfValue(name);
            if (auto it = globalFunctions_.find(std::string(name)); it != globalFunctions_.end()) return Value::OfFunction(it->second);
            if (auto it = defines_.find(std::string(name)); it != defines_.end()) return it->second;
            if (name == "List") Fail(expr->location, "declare lists with their type: 'local xs: List<int> = {}'");
            if (auto it = singletons_.find(name); it != singletons_.end()) return Value::OfSlot(it->second.slot, types_.Script(it->second.script));

            if (auto it = typeAliases_.find(std::string(name)); it != typeAliases_.end()) return Value::OfType(it->second);
            if (const ScriptInfo* script = catalog_.FindScript(name)) return Value::OfType(types_.Script(script));
            if (types_.IsNamespace(name) && types_.FindByShortName(name).empty()) return Value::OfNamespace(std::string(name));
            if (const Type* t = ShortType(name, expr->location)) return Value::OfType(t);
            if (types_.IsNamespace(name)) return Value::OfNamespace(std::string(name));
            Fail(expr->location, std::format("unknown name '{}'", name));
        }

        void Compiler::DeclareConstTable(AstLocal* var, AstExprTable* table) {
            std::map<std::string, Value, std::less<>> members;
            for (const AstExprTable::Item& item : table->items) {
                if (item.kind != AstExprTable::Item::Kind::Record) Fail(table->location, "a constant table needs named members, e.g. const State = {Idle = 0, Open = 1}");
                auto* key = item.key->as<AstExprConstantString>();
                std::string name(key->value.data, key->value.size);
                auto constant = TryConstant(item.value);
                if (!constant) Fail(item.value->location, std::format("'{}' needs a value known at compile time", name));
                if (!members.emplace(name, *constant).second) Fail(item.value->location, std::format("'{}' is declared twice", name));
            }
            aliases_.erase(var);
            constTables_[var] = std::move(members);
        }

        Value Compiler::CompileIndexName(AstExprIndexName* expr) {
            if (auto* local = Unwrap(expr->expr)->as<AstExprLocal>())
                if (auto it = constTables_.find(local->local); it != constTables_.end()) {
                    auto member = it->second.find(expr->index.value);
                    if (member == it->second.end()) Fail(expr->location, std::format("'{}' has no member '{}'", local->local->name.value, expr->index.value));
                    return member->second;
                }
            if (auto library = LibraryName(expr->expr)) {
                std::string_view name = expr->index.value;
                if (*library == "math" && name == "pi") return Value::OfReal(static_cast<double>(3.14159265358979f));
                if (*library == "math" && name == "huge") return Value::OfReal(std::numeric_limits<double>::infinity());
                Fail(expr->location, std::format("{}.{} must be called", *library, name));
            }
            return ReadMember(CompileExpr(expr->expr), expr->index.value, expr->location);
        }

        Value Compiler::ReadMember(const Value& owner, std::string_view name, const Location& location) {
            if (owner.kind == Value::Kind::Nil || owner.kind == Value::Kind::None)
                Fail(location, std::format("cannot read '{}' of {}", name, Describe(owner)));
            if (owner.kind == Value::Kind::Slot && owner.type->IsList()) {
                if (name == "Count") return Value::OfSlot(ListCount(owner).slot, types_.Int32);
                if (name == "Capacity") return Value::OfSlot(ListCapacity(owner).slot, types_.Int32);
                Fail(location, std::format("List has no property '{}'; call methods with ':'", name));
            }

            if (owner.kind == Value::Kind::Namespace) {
                std::string path = owner.text + "." + std::string(name);
                if (const Type* t = types_.FindByFullName(path)) return Value::OfType(t);
                if (types_.IsNamespace(path)) return Value::OfNamespace(path);
                Fail(location, std::format("'{}' does not exist", path));
            }

            if (owner.kind == Value::Kind::TypeRef) {
                const Type* t = owner.type;
                if (t->kind == TypeKind::Enum && t->info) {
                    for (const EnumMember& m : t->info->enumMembers)
                        if (m.name == name) return Value::OfInteger(m.value, t);
                }
                if (!t->fullName.empty())
                    if (const Type* nested = types_.FindByFullName(t->fullName + "+" + std::string(name))) return Value::OfType(nested);
                if (t->udonName == "UnityEngineMathf")
                    if (auto constant = MathfConstant(name)) return Value::OfReal(*constant);
                return CallStatic(t, "get_" + std::string(name), {}, nullptr, location).at(0);
            }

            if (owner.kind == Value::Kind::Function) Fail(location, "functions have no members");
            if (owner.kind == Value::Kind::Network) Fail(location, "use ':' to call a method through Network");
            if (owner.kind == Value::Kind::Slot && owner.type->script) {
                const ScriptInfo* script = owner.type->script;
                auto field = std::ranges::find(script->fields, name, &ScriptVariable::name);
                if (field != script->fields.end()) return ReadScriptField(owner, *field, location);
                auto getter = std::ranges::find(script->methods, "get_" + std::string(name), &ScriptMethod::name);
                if (getter != script->methods.end() && getter->parameters.empty() && getter->returns.size() == 1)
                    return CallScriptMethod(owner, *getter, {}, 1, location).at(0);
                if (std::ranges::find(script->methods, name, &ScriptMethod::name) != script->methods.end())
                    Fail(location, std::format("'{}' is a method; call it with 'value:{}()'", name, name));
            }
            Value receiver = owner.kind == Value::Kind::Slot ? owner : Value::OfSlot(Materialize(owner, nullptr, location), NaturalType(owner));
            return CallMember(receiver, "get_" + std::string(name), {}, nullptr, location).at(0);
        }

        Value Compiler::CompileIndexExpr(AstExprIndexExpr* expr) {
            Value owner = CompileExpr(expr->expr);
            if (HasCall(expr->index)) owner = Evaluate(owner);
            return ReadIndex(owner, CompileExpr(expr->index), expr->location);
        }

        Value Compiler::CompileUnary(AstExprUnary* expr) {
            switch (expr->op) {
                case AstExprUnary::Op::Not: {
                    Value v = CompileExpr(expr->expr);
                    return Value::OfSlot(NotSlot(TruthySlot(v, expr->location), expr->location), types_.Boolean);
                }
                case AstExprUnary::Op::Minus: {
                    Value v = CompileExpr(expr->expr);
                    if (v.kind == Value::Kind::Integer && !v.type) return Value::OfInteger(-v.integer);
                    if (v.kind == Value::Kind::Real) return Value::OfReal(-v.real);
                    if (v.kind != Value::Kind::Slot) Fail(expr->location, std::format("cannot negate {}", Describe(v)));
                    return InvokeOperator("op_UnaryNegation", { v.type }, { v }, expr->location).at(0);
                }
                case AstExprUnary::Op::Len: {
                    Value v = CompileExpr(expr->expr);
                    if (v.kind == Value::Kind::String) return Value::OfInteger(static_cast<int64_t>(v.text.size()));
                    if (v.kind != Value::Kind::Slot) Fail(expr->location, std::format("cannot take the length of {}", Describe(v)));
                    if (v.type->IsList()) return Value::OfSlot(ListCount(v).slot, types_.Int32);
                    bool hasLength = !types_.Methods(v.type, "get_Length", false, true).empty();
                    return CallMember(v, hasLength ? "get_Length" : "get_Count", {}, nullptr, expr->location).at(0);
                }
            }
            Fail(expr->location, "unsupported operator");
        }

        Value Compiler::CompileBinary(AstExprBinary* expr, const Type* expected) {
            switch (expr->op) {
                case AstExprBinary::And:
                case AstExprBinary::Or:
                    return BooleanFromCondition(expr);
                case AstExprBinary::Concat:
                    return CompileConcat(expr);
                default:
                    break;
            }
            if (IsComparison(expr->op)) return CompileComparison(expr->op, expr->left, expr->right, expr->location);

            Value left = CompileExpr(expr->left, expected);
            if (HasCall(expr->right)) left = Evaluate(left);
            Value right = CompileExpr(expr->right, expected);
            return CompileArithmetic(expr->op, std::move(left), std::move(right), expr->location);
        }

        std::optional<Value> Compiler::FoldArithmetic(AstExprBinary::Op op, const Value& l, const Value& r) {
            if (!l.IsNumberLiteral() || !r.IsNumberLiteral()) return std::nullopt;
            bool ints = l.kind == Value::Kind::Integer && r.kind == Value::Kind::Integer;
            double a = l.kind == Value::Kind::Integer ? static_cast<double>(l.integer) : l.real;
            double b = r.kind == Value::Kind::Integer ? static_cast<double>(r.integer) : r.real;
            switch (op) {
                case AstExprBinary::Add: return ints ? Value::OfInteger(l.integer + r.integer) : Value::OfReal(a + b);
                case AstExprBinary::Sub: return ints ? Value::OfInteger(l.integer - r.integer) : Value::OfReal(a - b);
                case AstExprBinary::Mul: return ints ? Value::OfInteger(l.integer * r.integer) : Value::OfReal(a * b);
                case AstExprBinary::Div: return b == 0 ? std::nullopt : std::optional(Value::OfReal(a / b));
                case AstExprBinary::FloorDiv:
                    if (b == 0) return std::nullopt;
                    return ints ? Value::OfInteger(l.integer / r.integer) : Value::OfReal(std::floor(a / b));
                case AstExprBinary::Mod:
                    if (b == 0) return std::nullopt;
                    return ints ? Value::OfInteger(l.integer % r.integer) : Value::OfReal(std::fmod(a, b));
                case AstExprBinary::Pow: return Value::OfReal(std::pow(a, b));
                default: return std::nullopt;
            }
        }

        Value Compiler::CompileArithmetic(AstExprBinary::Op op, Value left, Value right, const Location& location) {
            if (auto folded = FoldArithmetic(op, left, right)) return *folded;

            auto integral = [&](const Value& v) {
                return (v.kind == Value::Kind::Integer && !v.type) || (v.kind == Value::Kind::Slot && v.type->IsIntegral());
            };

            if (op == AstExprBinary::Pow) {
                std::vector<const Type*> owners;
                for (std::string_view n : { "UnityEngineMathf", "SystemMath" }) owners.push_back(types_.Get(n));
                std::vector<Value> args{ left, right };
                Resolution r = Resolve(StaticMethods(owners, "Pow"), args, nullptr, location, "'^'");
                return Invoke(r, std::nullopt, args, location).at(0);
            }

            if (op == AstExprBinary::Div && integral(left) && integral(right)) {
                left = Value::OfSlot(Materialize(left, types_.Single, location), types_.Single);
                right = Value::OfSlot(Materialize(right, types_.Single, location), types_.Single);
            }

            std::vector<const Type*> owners;
            if (const Type* t = NaturalType(left)) owners.push_back(t);
            if (const Type* t = NaturalType(right)) owners.push_back(t);
            const Type* lt = NaturalType(left);
            const Type* rt = NaturalType(right);
            if (lt && rt && lt->IsNumeric() && rt->IsNumeric())
                for (const Type* t : { types_.Int32, types_.Int64, types_.Single, types_.Double }) owners.push_back(t);

            Value result = InvokeOperator(OperatorMethod(op), owners, { left, right }, location).at(0);
            if (op == AstExprBinary::FloorDiv && result.type && !result.type->IsIntegral()) {
                const Type* math = types_.Get(result.type == types_.Double ? "SystemMath" : "UnityEngineMathf");
                result = CallStatic(math, "Floor", { result }, nullptr, location).at(0);
            }
            return result;
        }

        Value Compiler::CompileComparison(AstExprBinary::Op op, AstExpr* leftExpr, AstExpr* rightExpr, const Location& location) {
            Value left = CompileExpr(leftExpr);
            if (HasCall(rightExpr)) left = Evaluate(left);
            Value right = CompileExpr(rightExpr);
            return CompareValues(op, left, right, location);
        }

        Value Compiler::CompareValues(AstExprBinary::Op op, const Value& left, const Value& right, const Location& location) {
            bool equality = op == AstExprBinary::CompareEq || op == AstExprBinary::CompareNe;
            if (left.IsLiteral() && right.IsLiteral() && left.kind == right.kind && equality) {
                bool same = left.kind == Value::Kind::Nil || (left.kind == Value::Kind::Integer && left.integer == right.integer) ||
                            (left.kind == Value::Kind::Real && left.real == right.real) || (left.kind == Value::Kind::Boolean && left.boolean == right.boolean) ||
                            (left.kind == Value::Kind::String && left.text == right.text);
                return Value::OfBoolean(op == AstExprBinary::CompareEq ? same : !same);
            }

            std::vector<const Type*> owners;
            const Type* lt = NaturalType(left);
            const Type* rt = NaturalType(right);
            if (lt) owners.push_back(lt);
            if (rt) owners.push_back(rt);
            if (lt && rt && lt->IsNumeric() && rt->IsNumeric())
                for (const Type* t : { types_.Int32, types_.Int64, types_.Single, types_.Double }) owners.push_back(t);

            std::vector<Value> args{ left, right };
            auto methods = StaticMethods(owners, OperatorMethod(op));
            bool ambiguous = false;
            if (auto r = TryResolve(methods, args, nullptr, ambiguous)) return Invoke(*r, std::nullopt, args, location).at(0);
            if (!equality) {
                Resolve(methods, args, nullptr, location, std::format("'{}'", toString(op)));
            }

            auto equals = StaticMethods({ types_.Object }, "Equals");
            std::erase_if(equals, [](const Method* m) { return m->parameters.size() != 2; });
            Resolution r = Resolve(equals, args, nullptr, location, "'=='");
            Value same = Invoke(r, std::nullopt, args, location).at(0);
            if (op == AstExprBinary::CompareEq) return same;
            return Value::OfSlot(NotSlot(same.slot, location), types_.Boolean);
        }

        Value Compiler::CompileConcat(AstExpr* expr) {
            std::vector<AstExpr*> operands;
            std::function<void(AstExpr*)> flatten = [&](AstExpr* e) {
                e = Unwrap(e);
                if (auto* b = e->as<AstExprBinary>(); b && b->op == AstExprBinary::Concat) {
                    flatten(b->left);
                    flatten(b->right);
                } else {
                    operands.push_back(e);
                }
            };
            flatten(expr);
            std::vector<Value> parts;
            for (size_t i = 0; i < operands.size(); ++i) {
                Value part = CompileExpr(operands[i]);
                bool laterCall = std::any_of(operands.begin() + static_cast<ptrdiff_t>(i) + 1, operands.end(), [this](AstExpr* e) { return HasCall(e); });
                parts.push_back(laterCall ? Evaluate(part) : part);
            }
            return ConcatValues(std::move(parts), expr->location);
        }

        Value Compiler::CompileInterpolated(AstExprInterpString* expr) {
            std::vector<Value> parts;
            for (size_t i = 0; i < expr->strings.size; ++i) {
                const AstArray<char>& s = expr->strings.data[i];
                if (s.size) parts.push_back(Value::OfString(std::string(s.data, s.size)));
                if (i < expr->expressions.size) parts.push_back(CompileExpr(expr->expressions.data[i]));
            }
            if (parts.empty()) return Value::OfString("");
            return ConcatValues(std::move(parts), expr->location);
        }

        Value Compiler::ToStringValue(const Value& value, const Location& location) {
            switch (value.kind) {
                case Value::Kind::String: return value;
                case Value::Kind::Integer:
                    if (!value.type) return Value::OfString(std::to_string(value.integer));
                    break;
                case Value::Kind::Real: return Value::OfString(std::format("{}", static_cast<float>(value.real)));
                case Value::Kind::Boolean: return Value::OfString(value.boolean ? "True" : "False");
                case Value::Kind::Nil: return Value::OfString("");
                default: break;
            }
            Value slot = value.kind == Value::Kind::Slot ? value : Value::OfSlot(Materialize(value, nullptr, location), NaturalType(value));
            if (slot.type == types_.String) return slot;
            return CallMember(slot, "ToString", {}, nullptr, location).at(0);
        }

        Value Compiler::ConcatValues(std::vector<Value> parts, const Location& location) {
            std::vector<Value> strings;
            for (const Value& p : parts) {
                Value s = ToStringValue(p, location);
                if (s.kind == Value::Kind::String && !strings.empty() && strings.back().kind == Value::Kind::String) strings.back().text += s.text;
                else strings.push_back(std::move(s));
            }
            if (strings.size() == 1) return strings[0];

            auto concat = StaticMethods({ types_.String }, "Concat");
            auto arity = [&](size_t n) {
                for (const Method* m : concat)
                    if (m->parameters.size() == n && std::ranges::all_of(m->parameters, [&](const Parameter& p) { return p.type == types_.String; })) return m;
                return static_cast<const Method*>(nullptr);
            };
            size_t widest = 2;
            for (size_t n = 4; n > 2; --n)
                if (arity(n)) {
                    widest = n;
                    break;
                }
            const Method* two = arity(2);
            if (!two) Fail(location, "string concatenation needs System.String.Concat(string, string)");

            while (strings.size() > 1) {
                size_t take = std::min(widest, strings.size());
                while (!arity(take)) --take;
                const Method* m = arity(take);
                std::vector<Value> group(strings.begin(), strings.begin() + static_cast<ptrdiff_t>(take));
                Value joined = Invoke({ m, nullptr }, std::nullopt, group, location).at(0);
                strings.erase(strings.begin(), strings.begin() + static_cast<ptrdiff_t>(take));
                strings.insert(strings.begin(), joined);
            }
            return strings[0];
        }

        Value Compiler::CompileIfElse(AstExprIfElse* expr, const Type* expected, const Variable* into) {
            if (!expr->hasElse) Fail(expr->location, "if-expressions need an else branch");
            if (auto c = TryConstant(expr->condition)) return CompileExpr(IsTruthy(*c) ? expr->trueExpr : expr->falseExpr, expected, into);
            Label otherwise = emit_.NewLabel();
            Label end = emit_.NewLabel();
            CompileCondition(expr->condition, otherwise);
            const Type* hint = expected ? expected : into ? into->type : nullptr;
            Value a = CompileExpr(expr->trueExpr, hint, into);
            const Type* type = hint ? hint : NaturalType(a);
            if (!type) Fail(expr->trueExpr->location, "cannot infer the type of this if-expression; add a type assertion");
            Variable result = into && into->type == type ? *into : Variable{ emit_.Temp(type), type };
            Store(a, result, expr->trueExpr->location);
            emit_.Jump(end);
            emit_.Bind(otherwise);
            Store(CompileExpr(expr->falseExpr, type, &result), result, expr->falseExpr->location);
            emit_.Bind(end);
            return Value::OfSlot(result.slot, type);
        }

        Value Compiler::CompileTable(AstExprTable* expr, const Type* expected) {
            if (!expected || expected->kind != TypeKind::Array)
                Fail(expr->location, "table constructors need an array type, e.g. 'local xs: {int} = {1, 2, 3}'");
            for (const AstExprTable::Item& item : expr->items)
                if (item.kind != AstExprTable::Item::Kind::List) Fail(expr->location, "only array-style tables are supported");

            Value array = NewArray(expected, Value::OfInteger(static_cast<int64_t>(expr->items.size)), expr->location);
            for (size_t i = 0; i < expr->items.size; ++i) {
                Value element = CompileExpr(expr->items.data[i].value, expected->element);
                CallMember(array, "Set", { Value::OfInteger(static_cast<int64_t>(i)), element }, nullptr, expr->location);
            }
            return array;
        }

        Value Compiler::BooleanFromCondition(AstExpr* expr, const Variable* into) {
            if (auto c = TryConstant(expr)) return Value::OfBoolean(IsTruthy(*c));
            uint32_t result = into && into->type == types_.Boolean ? into->slot : emit_.Temp(types_.Boolean);
            Label otherwise = emit_.NewLabel();
            Label end = emit_.NewLabel();
            CompileCondition(expr, otherwise);
            HeapValue t;
            t.kind = ValueKind::Boolean;
            t.boolean = true;
            HeapValue f;
            f.kind = ValueKind::Boolean;
            emit_.Copy(emit_.Constant(types_.Boolean, t), result);
            emit_.Jump(end);
            emit_.Bind(otherwise);
            emit_.Copy(emit_.Constant(types_.Boolean, f), result);
            emit_.Bind(end);
            return Value::OfSlot(result, types_.Boolean);
        }

        void Compiler::CompileCondition(AstExpr* expr, Label whenFalse, bool negate) {
            expr = Unwrap(expr);
            if (auto* b = expr->as<AstExprBinary>()) {
                bool isAnd = b->op == AstExprBinary::And;
                bool isOr = b->op == AstExprBinary::Or;
                if ((isAnd && !negate) || (isOr && negate)) {
                    CompileCondition(b->left, whenFalse, negate);
                    CompileCondition(b->right, whenFalse, negate);
                    return;
                }
                if ((isOr && !negate) || (isAnd && negate)) {
                    Label right = emit_.NewLabel();
                    Label pass = emit_.NewLabel();
                    CompileCondition(b->left, right, negate);
                    emit_.Jump(pass);
                    emit_.Bind(right);
                    CompileCondition(b->right, whenFalse, negate);
                    emit_.Bind(pass);
                    return;
                }
                if (IsComparison(b->op)) {
                    Value left = CompileExpr(b->left);
                    if (HasCall(b->right)) left = Evaluate(left);
                    Value right = CompileExpr(b->right);
                    auto floating = [&](const Value& v) {
                        const Type* t = NaturalType(v);
                        return t && (t->numeric == Numeric::Single || t->numeric == Numeric::Double);
                    };
                    bool ordering = b->op != AstExprBinary::CompareEq && b->op != AstExprBinary::CompareNe;
                    if (negate && ordering && (floating(left) || floating(right))) {
                        Value test = CompareValues(b->op, left, right, b->location);
                        Label skip = emit_.NewLabel();
                        emit_.JumpIfFalse(TruthySlot(test, b->location), skip);
                        emit_.Jump(whenFalse);
                        emit_.Bind(skip);
                        return;
                    }
                    Value test = CompareValues(negate ? NegateComparison(b->op) : b->op, left, right, b->location);
                    emit_.JumpIfFalse(TruthySlot(test, b->location), whenFalse);
                    return;
                }
            }
            if (auto* u = expr->as<AstExprUnary>(); u && u->op == AstExprUnary::Op::Not) {
                CompileCondition(u->expr, whenFalse, !negate);
                return;
            }
            Value v = CompileExpr(expr);
            if (v.IsLiteral()) {
                bool truthy = v.kind != Value::Kind::Nil && !(v.kind == Value::Kind::Boolean && !v.boolean);
                if (truthy == negate) emit_.Jump(whenFalse);
                return;
            }
            uint32_t slot = TruthySlot(v, expr->location);
            if (!negate) {
                emit_.JumpIfFalse(slot, whenFalse);
                return;
            }
            Label skip = emit_.NewLabel();
            emit_.JumpIfFalse(slot, skip);
            emit_.Jump(whenFalse);
            emit_.Bind(skip);
        }

        uint32_t Compiler::TruthySlot(const Value& value, const Location& location) {
            HeapValue b;
            b.kind = ValueKind::Boolean;
            switch (value.kind) {
                case Value::Kind::Boolean:
                    b.boolean = value.boolean;
                    return emit_.Constant(types_.Boolean, b);
                case Value::Kind::Nil:
                    return emit_.Constant(types_.Boolean, b);
                case Value::Kind::Slot:
                    if (value.type == types_.Boolean) return value.slot;
                    if (value.type->IsReference()) return CompareValues(AstExprBinary::CompareNe, value, Value::OfNil(), location).slot;
                    Fail(location, std::format("{} cannot be used as a condition", value.type->displayName));
                default:
                    if (value.IsLiteral()) {
                        b.boolean = true;
                        return emit_.Constant(types_.Boolean, b);
                    }
                    Fail(location, std::format("{} cannot be used as a condition", Describe(value)));
            }
        }

        uint32_t Compiler::NotSlot(uint32_t slot, const Location& location) {
            Value v = Value::OfSlot(slot, types_.Boolean);
            auto methods = StaticMethods({ types_.Boolean }, "op_UnaryNegation");
            std::vector<Value> args{ v };
            bool ambiguous = false;
            if (auto r = TryResolve(methods, args, nullptr, ambiguous)) return Invoke(*r, std::nullopt, args, location).at(0).slot;
            return CompareValues(AstExprBinary::CompareEq, v, Value::OfBoolean(false), location).slot;
        }

        std::vector<Value> Compiler::CompileCall(AstExprCall* call, size_t want, const Variable* into) {
            AstExpr* func = Unwrap(call->func);
            if (auto* index = call->self ? nullptr : func->as<AstExprIndexName>()) {
                std::optional<std::string_view> library = LibraryName(index->expr);
                if (library && *library == "task") return CallTask(call, index->index.value, want);
            }
            if (auto listened = CallListenable(call, want)) return std::move(*listened);
            if (auto* g = func->as<AstExprGlobal>(); g && g->name == "assert" && !globalFunctions_.contains("assert")) {
                CompileAssert(call);
                return {};
            }
            const Type* typeArg = nullptr;
            if (call->typeArguments.size) {
                if (!call->typeArguments.data[0].type) Fail(call->location, "type packs are not supported");
                typeArg = ResolveType(call->typeArguments.data[0].type);
            }
            if (auto* inst = func->as<AstExprInstantiate>()) {
                if (inst->typeArguments.size && inst->typeArguments.data[0].type) typeArg = ResolveType(inst->typeArguments.data[0].type);
                func = Unwrap(inst->expr);
            }

            bool argsCall = std::any_of(call->args.begin(), call->args.end(), [this](AstExpr* a) { return HasCall(a); });
            std::optional<Value> selfReceiver;
            if (call->self) {
                selfReceiver = CompileExpr(func->as<AstExprIndexName>()->expr);
                if (argsCall) selfReceiver = Evaluate(*selfReceiver);
            }

            std::vector<Value> args;
            for (size_t i = 0; i < call->args.size; ++i) {
                Value arg = CompileExpr(call->args.data[i]);
                bool laterCall = std::any_of(call->args.begin() + i + 1, call->args.end(), [this](AstExpr* a) { return HasCall(a); });
                args.push_back(laterCall ? Evaluate(arg) : arg);
            }

            if (call->self) {
                auto* index = func->as<AstExprIndexName>();
                Value receiver = *selfReceiver;
                std::string_view name = index->index.value;
                if (receiver.kind == Value::Kind::Slot && receiver.type->IsList()) return CallList(receiver, name, std::move(args), call->location);
                if (receiver.kind == Value::Kind::Network) return SendNetwork(receiver, name, std::move(args), call->location);
                if (receiver.kind == Value::Kind::Delayed) return CallDelayed(receiver, name, std::move(args), call->location);
                if (receiver.kind == Value::Kind::Slot && receiver.type->script)
                    if (const ScriptMethod* method = PickScriptMethod(receiver.type->script, name, args, call->location))
                        return CallScriptMethod(receiver, *method, args, want, call->location);
                if (receiver.kind == Value::Kind::TypeRef || receiver.kind == Value::Kind::Namespace)
                    Fail(call->location, "use '.' to call static methods");
                if (receiver.kind == Value::Kind::Nil || receiver.kind == Value::Kind::Function || receiver.kind == Value::Kind::None)
                    Fail(call->location, std::format("cannot call '{}' on {}", name, Describe(receiver)));
                if (receiver.kind != Value::Kind::Slot) receiver = Value::OfSlot(Materialize(receiver, nullptr, call->location), NaturalType(receiver));
                return CallMember(receiver, index->index.value, std::move(args), typeArg, call->location);
            }

            if (auto* index = func->as<AstExprIndexName>()) {
                if (auto library = LibraryName(index->expr))
                    return CallLibrary(*library, index->index.value, call, std::move(args), typeArg, into);
                Value owner = CompileExpr(index->expr);
                if (owner.kind == Value::Kind::TypeRef) {
                    std::string_view name = index->index.value;
                    return CallStatic(owner.type, name == "new" ? "ctor" : name, std::move(args), typeArg, call->location);
                }
                if (owner.kind == Value::Kind::Slot)
                    Fail(call->location, std::format("use ':' to call methods on values: '{}:{}(...)'", "value", index->index.value));
                Fail(call->location, "this expression cannot be called");
            }

            if (auto* l = func->as<AstExprLocal>()) {
                if (auto it = staticFunctions_.find(l->local); it != staticFunctions_.end()) {
                    Value companion = StaticCompanion(call->location);
                    const ScriptInfo* script = companion.type->script;
                    auto method = std::ranges::find(script->methods, it->second, &ScriptMethod::name);
                    if (method == script->methods.end()) Fail(call->location, std::format("static function '{}' is not registered yet; recompile the script", it->second));
                    return CallScriptMethod(companion, *method, args, want, call->location);
                }
            }
            if (auto* global = func->as<AstExprGlobal>(); global && !globalFunctions_.contains(global->name.value)) {
                bool handled = false;
                std::vector<Value> results = CallBuiltin(global->name.value, args, call->location, handled);
                if (handled) return results;
            }

            Value callee = CompileExpr(func);
            if (callee.kind == Value::Kind::Function) return CallFunction(callee.function, std::move(args), want, call->location, into);
            Fail(call->location, "this expression cannot be called");
        }

        const ScriptMethod* Compiler::NetworkMethod(const Value& receiver, std::string_view name, size_t argc, const Location& location) {
            const ScriptInfo* script = receiver.type->script;
            if (!script) return nullptr;
            auto method = std::ranges::find(script->methods, name, &ScriptMethod::name);
            if (method == script->methods.end()) Fail(location, std::format("{} has no public method '{}'", script->name, name));
            if (!method->networkCallable) Fail(location, std::format("{}.{} is not network callable; mark it with '-- @networkcallable'", script->name, name));
            if (argc != method->parameters.size()) Fail(location, std::format("'{}' takes {} argument(s), got {}", name, method->parameters.size(), argc));
            return &*method;
        }

        std::vector<Value> Compiler::SendNetwork(const Value& receiver, std::string_view name, std::vector<Value> args, const Location& location) {
            std::vector<Value> sent{ Value::OfInteger(receiver.integer, receiver.targetType), Value::OfString(std::string(name)) };
            const Function* own = nullptr;
            if (receiver.slot == SelfValue("this").slot)
                for (const auto& f : functions_)
                    if (f->name == name && !f->event) own = f.get();
            if (own) {
                if (!own->networkCallable) Fail(location, std::format("'{}' is not network callable; mark it with '-- @networkcallable'", name));
                if (args.size() != own->parameters.size()) Fail(location, std::format("'{}' takes {} argument(s), got {}", name, own->parameters.size(), args.size()));
                sent[1] = Value::OfString(own->entryName);
                for (size_t i = 0; i < args.size(); ++i) {
                    const Type* type = own->parameters[i].type;
                    if (Cost(args[i], type) < 0) Fail(location, std::format("argument {} of '{}' must be {}, got {}", i + 1, name, type->displayName, Describe(args[i])));
                    Value arg = args[i].IsLiteral() ? Value::OfSlot(Materialize(args[i], type, location), type) : args[i];
                    if (arg.kind == Value::Kind::Slot && arg.type != type && arg.type->IsNumeric() && type->IsNumeric())
                        arg = Value::OfSlot(Convert(arg.slot, arg.type, type, location), type);
                    sent.push_back(arg);
                }
            } else if (const ScriptMethod* method = NetworkMethod(receiver, name, args.size(), location)) {
                sent[1] = Value::OfString(method->entryPoint);
                for (size_t i = 0; i < args.size(); ++i) {
                    const Type* type = VariableType(method->parameters[i]);
                    if (Cost(args[i], type) < 0) Fail(location, std::format("argument {} of '{}' must be {}, got {}", i + 1, name, type->displayName, Describe(args[i])));
                    Value arg = args[i].IsLiteral() ? Value::OfSlot(Materialize(args[i], type, location), type) : args[i];
                    if (arg.kind == Value::Kind::Slot && arg.type != type && arg.type->IsNumeric() && type->IsNumeric())
                        arg = Value::OfSlot(Convert(arg.slot, arg.type, type, location), type);
                    sent.push_back(arg);
                }
            } else {
                for (const Value& a : args) sent.push_back(a);
            }
            if (sent.size() > 10) Fail(location, "networked calls take at most 8 arguments");
            Value self = Value::OfSlot(receiver.slot, types_.Behaviour());
            return CallMember(self, "SendCustomNetworkEvent", sent, nullptr, location);
        }

        std::vector<Value> Compiler::CallDelay(std::string_view name, std::vector<Value> args, const Location& location) {
            if (name != "Seconds" && name != "Frames") Fail(location, std::format("Delay.{} does not exist; use Delay.Seconds or Delay.Frames", name));
            if (args.size() < 2 || args.size() > 3) Fail(location, std::format("Delay.{} takes the delay, the target and an optional EventTiming", name));
            auto spec = std::make_shared<DelaySpec>();
            spec->frames = name == "Frames";
            const Type* amountType = spec->frames ? types_.Int32 : types_.Single;
            if (Cost(args[0], amountType) < 0) Fail(location, std::format("the delay must be {}", amountType->displayName));
            spec->amount = args[0].IsLiteral() ? CoerceLiteral(args[0], amountType, location) : args[0];
            if (spec->amount.kind == Value::Kind::Slot && emit_.IsTemp(spec->amount.slot)) {
                Variable held{ emit_.Hidden(amountType), amountType };
                Store(spec->amount, held, location);
                spec->amount = Value::OfSlot(held.slot, amountType);
            }
            if (args.size() == 3) spec->timing = args[2];
            const Value& target = args[1];
            bool behaviour = target.kind == Value::Kind::Slot && types_.ReferenceDistance(target.type, types_.Behaviour()) >= 0;
            if (target.kind != Value::Kind::Network && !behaviour)
                Fail(location, std::format("Delay.{} needs a behaviour or a Network target, got {}", name, Describe(target)));
            spec->target = target;
            Value v;
            v.kind = Value::Kind::Delayed;
            v.delay = std::move(spec);
            return { v };
        }

        std::vector<Value> Compiler::CallDelayed(const Value& receiver, std::string_view name, std::vector<Value> args, const Location& location) {
            const DelaySpec& spec = *receiver.delay;
            const Value& target = spec.target;
            const Value self = SelfValue("this");
            bool network = target.kind == Value::Kind::Network;
            bool toSelf = !network && target.slot == self.slot;

            std::vector<const Type*> types;
            Function* own = nullptr;
            const ScriptMethod* scriptMethod = nullptr;
            std::string entry(name);
            if (network) {
                if (const ScriptMethod* method = NetworkMethod(target, name, args.size(), location)) {
                    for (const ScriptVariable& p : method->parameters) types.push_back(VariableType(p));
                } else {
                    for (const Value& a : args) types.push_back(NaturalType(a));
                }
            } else if (toSelf) {
                for (const auto& f : functions_)
                    if (f->name == name && !f->event) own = f.get();
                if (!own) Fail(location, std::format("this script has no function '{}'", name));
                if (args.size() != own->parameters.size()) Fail(location, std::format("'{}' takes {} argument(s), got {}", name, own->parameters.size(), args.size()));
                for (const Variable& p : own->parameters) types.push_back(p.type);
            } else if (const ScriptInfo* script = target.type->script) {
                scriptMethod = PickScriptMethod(script, name, args, location);
                if (!scriptMethod) Fail(location, std::format("{} has no public method '{}'", script->name, name));
                if (args.size() != scriptMethod->parameters.size()) Fail(location, std::format("'{}' takes {} argument(s), got {}", name, scriptMethod->parameters.size(), args.size()));
                for (const ScriptVariable& p : scriptMethod->parameters) types.push_back(VariableType(p));
                entry = scriptMethod->entryPoint;
            } else if (!args.empty()) {
                Fail(location, "a delayed call with arguments needs a typed script reference; a plain UdonBehaviour only takes delayed calls without arguments");
            }
            for (size_t i = 0; i < args.size(); ++i) {
                if (!types[i]) Fail(location, std::format("cannot tell the type of argument {}", i + 1));
                if (types[i]->IsList()) Fail(location, "a List cannot be passed to a delayed call");
                if (Cost(args[i], types[i]) < 0) Fail(location, std::format("argument {} of '{}' must be {}, got {}", i + 1, name, types[i]->displayName, Describe(args[i])));
            }

            std::vector<Value> schedule{ Value(), spec.amount, spec.timing ? *spec.timing : Value::OfInteger(0, types_.Get("VRCUdonCommonEnumsEventTiming")) };
            std::string_view scheduler = spec.frames ? "SendCustomEventDelayedFrames" : "SendCustomEventDelayedSeconds";
            if (own && own->exported && args.empty()) entry = own->entryName;
            if (!network && (!own || own->exported) && args.empty()) {
                schedule[0] = Value::OfString(entry);
                CallMember(Value::OfSlot(target.slot, types_.Behaviour()), scheduler, std::move(schedule), nullptr, location);
                return { Value::OfNil() };
            }

            if (spec.timing && !spec.timing->IsLiteral())
                Fail(location, "delayed calls with arguments need a constant EventTiming, such as EventTiming.LateUpdate");
            DelaySite site;
            site.entry = std::format("__delay{}", delaySites_.size());
            site.label = emit_.NewLabel();
            site.location = location;
            site.target = target;
            site.method = std::string(name);
            site.own = own;
            site.scriptMethod = scriptMethod;
            auto queue = [&](const Type* type) {
                std::string symbol = std::format("{}_{}", site.entry, site.queues.size());
                HeapValue empty;
                empty.kind = ValueKind::Array;
                empty.text = type->udonName;
                Variable list = DeclareListStorage(symbol, types_.ListOf(type), true, std::move(empty), 0);
                site.queues.push_back(Value::OfSlot(list.slot, list.type));
                return site.queues.back();
            };
            site.queuedTarget = !toSelf && !emit_.IsConstant(target.slot);
            if (site.queuedTarget) CallList(queue(target.type), "Add", { Value::OfSlot(target.slot, target.type) }, location);
            for (size_t i = 0; i < args.size(); ++i) CallList(queue(types[i]), "Add", { args[i] }, location);
            if (!spec.amount.IsLiteral() || (spec.timing && !spec.timing->IsLiteral())) {
                const Type* clock = spec.frames ? types_.Int32 : types_.Single;
                HeapValue empty;
                empty.kind = ValueKind::Array;
                empty.text = clock->udonName;
                Variable list = DeclareListStorage(site.entry + "_due", types_.ListOf(clock), true, std::move(empty), 0);
                site.due = Value::OfSlot(list.slot, list.type);
                Value now = CallStatic(types_.Get("UnityEngineTime"), spec.frames ? "get_frameCount" : "get_time", {}, nullptr, location).at(0);
                CallList(*site.due, "Add", { CompileArithmetic(AstExprBinary::Add, now, spec.amount, location) }, location);
            }
            schedule[0] = Value::OfString(site.entry);
            CallMember(self, scheduler, std::move(schedule), nullptr, location);
            delaySites_.push_back(std::move(site));
            return { Value::OfNil() };
        }

        void Compiler::CompileDelaySite(const DelaySite& site) {
            current_ = nullptr;
            emit_.ResetTemps();
            emit_.Bind(site.label);
            entries_.push_back({ site.entry, *emit_.AddressOf(site.label) });
            const Location& location = site.location;
            if (options_.compatibleExitReturn) emit_.Push(haltSlot_);
            if (!singletons_.empty()) {
                Label resolve = emit_.NewLabel();
                Label resolved = emit_.NewLabel();
                emit_.JumpIfFalse(singletonsReadySlot_, resolve);
                emit_.Bind(resolved);
                resolveStubs_.emplace_back(resolve, resolved);
            }

            Label finished = emit_.NewLabel();
            if (site.due || !site.queues.empty()) {
                const Value& first = site.due ? *site.due : site.queues.front();
                Label pending = emit_.NewLabel();
                Value empty = CompareValues(AstExprBinary::CompareLe, Value::OfSlot(ListCount(first).slot, types_.Int32), Value::OfInteger(0), location);
                emit_.JumpIfFalse(TruthySlot(empty, location), pending);
                emit_.Jump(finished);
                emit_.Bind(pending);
            }
            Variable pick{ emit_.Hidden(types_.Int32), types_.Int32 };
            Store(Value::OfInteger(0), pick, location);
            Value picked = Value::OfSlot(pick.slot, types_.Int32);
            if (site.due) {
                Variable i{ emit_.Hidden(types_.Int32), types_.Int32 };
                Store(Value::OfInteger(1), i, location);
                Value index = Value::OfSlot(i.slot, types_.Int32);
                Label test = emit_.NewLabel();
                Label next = emit_.NewLabel();
                Label done = emit_.NewLabel();
                emit_.Bind(test);
                emit_.JumpIfFalse(TruthySlot(CompareValues(AstExprBinary::CompareLt, index, Value::OfSlot(ListCount(*site.due).slot, types_.Int32), location), location), done);
                Value candidate = CallList(*site.due, "Get", { index }, location).at(0);
                Value best = CallList(*site.due, "Get", { picked }, location).at(0);
                emit_.JumpIfFalse(TruthySlot(CompareValues(AstExprBinary::CompareLt, candidate, best, location), location), next);
                Store(index, pick, location);
                emit_.Bind(next);
                Store(CompileArithmetic(AstExprBinary::Add, index, Value::OfInteger(1), location), i, location);
                emit_.Jump(test);
                emit_.Bind(done);
                ListRemoveAt(*site.due, picked, location);
            }

            std::vector<Value> values;
            for (const Value& q : site.queues) {
                const Type* type = q.type->element;
                Variable held{ emit_.Hidden(type), type };
                Store(CallList(q, "Get", { picked }, location).at(0), held, location);
                ListRemoveAt(q, picked, location);
                values.push_back(Value::OfSlot(held.slot, type));
            }
            Value target = site.target;
            if (site.queuedTarget) {
                target.slot = values.front().slot;
                values.erase(values.begin());
            }

            if (target.kind == Value::Kind::Network) {
                SendNetwork(target, site.method, std::move(values), location);
            } else if (site.own) {
                EnterRoot(site.own, std::move(values), location);
            } else {
                CallScriptMethod(target, *site.scriptMethod, values, 0, location);
            }
            emit_.Bind(finished);
            EmitReturn();
        }

        void Compiler::BeginSyntheticEntry(const std::string& name) {
            current_ = nullptr;
            emit_.ResetTemps();
            Label entry = emit_.NewLabel();
            emit_.Bind(entry);
            entries_.push_back({ name, *emit_.AddressOf(entry) });
            if (options_.compatibleExitReturn) emit_.Push(haltSlot_);
            if (!singletons_.empty()) {
                Label resolve = emit_.NewLabel();
                Label resolved = emit_.NewLabel();
                emit_.JumpIfFalse(singletonsReadySlot_, resolve);
                emit_.Bind(resolved);
                resolveStubs_.emplace_back(resolve, resolved);
            }
        }

        void Compiler::RecordChangeHandler(const Annotations& annotations, const std::string& field, const Variable& variable, const Location& location) {
            for (const FieldAttribute& a : annotations.attributes) {
                if (a.name != "onchange") continue;
                if (a.arguments.size() != 1 || a.arguments[0].empty()) Fail(location, "@onchange takes the name of a function, e.g. @onchange(OnScoreChanged)");
                changeHandlers_.push_back({ field, variable, a.arguments[0], location, nullptr });
            }
        }

        void Compiler::CompileChangeHandler(const ChangeHandler& handler) {
            if (!handler.target) Fail(handler.location, std::format("@onchange: '{}' is not a function of this script", handler.function));
            const Function& f = *handler.target;
            if (f.parameters.size() > 1 || (f.parameters.size() == 1 && f.parameters[0].type != handler.variable.type))
                Fail(handler.location, std::format("@onchange: '{}' must take no parameters or the old value as {}", handler.function, handler.variable.type->displayName));
            std::vector<Value> args;
            if (f.parameters.size() == 1) {
                uint32_t old = emit_.AddSlot("_old_" + handler.field, handler.variable.type);
                args.push_back(Value::OfSlot(old, handler.variable.type));
            }
            BeginSyntheticEntry("_onVarChange_" + handler.field);
            EnterRoot(handler.target, std::move(args), handler.location);
            EmitReturn();
        }

        void Compiler::EmitReturn() {
            if (!options_.compatibleExitReturn) {
                emit_.JumpTo(0xFFFFFFFFu);
                return;
            }
            emit_.Push(returnJumpSlot_);
            emit_.CopyFromStack();
            emit_.JumpIndirect(returnJumpSlot_);
        }

        void Compiler::FindSingletonUses(AstStatBlock* root) {
            class Finder : public AstVisitor {
            public:
                std::set<std::string, std::less<>> names;
                std::set<std::string, std::less<>> called;
                bool visit(AstExprGlobal* g) override {
                    names.insert(g->name.value);
                    return true;
                }
                bool visit(AstExprCall* c) override {
                    auto* index = c->self ? Unwrap(c->func)->as<AstExprIndexName>() : nullptr;
                    if (auto* g = index ? Unwrap(index->expr)->as<AstExprGlobal>() : nullptr) called.insert(g->name.value);
                    return true;
                }
            };
            Finder finder;
            root->visit(&finder);
            for (const std::string& name : finder.names) {
                const ScriptInfo* script = catalog_.FindScript(name);
                if (!script || !script->singleton || globalFunctions_.contains(name) || defines_.contains(name)) continue;
                HeapValue none;
                none.kind = ValueKind::Null;
                uint32_t slot = emit_.AddSlot("__singleton_" + name, types_.Script(script), none, true);
                emit_.Slot(slot).attributes.push_back({ "hideininspector", {} });
                singletons_[name] = { script, slot, finder.called.contains(name) };
            }
            if (!options_.staticPart && (!staticFields_.empty() || !staticFunctions_.empty())) {
                if (options_.scriptName.empty()) Fail(root->location, "statics need the script's name; the host must pass it when compiling");
                std::string name = StaticCompanionName(options_.scriptName);
                const ScriptInfo* script = catalog_.FindScript(name);
                if (!script) Fail(root->location, std::format("the static part of {} is not registered yet; recompile the script", options_.scriptName));
                HeapValue none;
                none.kind = ValueKind::Null;
                uint32_t slot = emit_.AddSlot("__singleton_" + name, types_.Script(script), none, true);
                emit_.Slot(slot).attributes.push_back({ "hideininspector", {} });
                singletons_[name] = { script, slot, !staticFunctions_.empty() };
            }
            if (singletons_.empty()) return;
            HeapValue no;
            no.kind = ValueKind::Boolean;
            singletonsReadySlot_ = emit_.AddSlot("__singletons_ready", types_.Boolean, no, true);
            emit_.Slot(singletonsReadySlot_).attributes.push_back({ "hideininspector", {} });
            resolveReturnSlot_ = emit_.Hidden(types_.UInt32);
            resolveRoutine_ = emit_.NewLabel();
        }

        Value Compiler::StaticCompanion(const Location& location) {
            auto it = singletons_.find(StaticCompanionName(options_.scriptName));
            if (it == singletons_.end()) Fail(location, "this script's statics are not available here");
            return Value::OfSlot(it->second.slot, types_.Script(it->second.script));
        }

        void Compiler::CheckInstanceUse(AstLocal* local, const Location& location) const {
            if (instanceLocals_.contains(local))
                Fail(location, std::format("'{}' is not static; static code can only use static fields and functions", local->name.value));
        }

        void Compiler::EmitEntryPrologue(const Function& f) {
            if (!singletons_.empty()) {
                Label resolve = emit_.NewLabel();
                Label resolved = emit_.NewLabel();
                emit_.JumpIfFalse(singletonsReadySlot_, resolve);
                emit_.Bind(resolved);
                resolveStubs_.emplace_back(resolve, resolved);
            }
            if (!f.event) return;
            if (singleton_ && f.name == "Start") {
                singletonStarted_ = true;
                emit_.Copy(TruthySlot(Value::OfBoolean(true), f.location), startedSlot_);
            }
            if (f.name != "Start" && f.name != "OnEnable") return;
            for (const auto& [name, use] : singletons_) {
                if (!use.methods) continue;
                Value started = CallMember(Value::OfSlot(use.slot, types_.Behaviour()), "GetProgramVariable", { Value::OfString("__started") }, nullptr, f.location).at(0);
                Label wait = emit_.NewLabel();
                Label go = emit_.NewLabel();
                emit_.JumpIfFalse(started.slot, wait);
                emit_.Jump(go);
                emit_.Bind(wait);
                CallMember(SelfValue("this"), "SendCustomEventDelayedFrames",
                    { Value::OfString(f.entryName), Value::OfInteger(1), Value::OfInteger(0, types_.Get("VRCUdonCommonEnumsEventTiming")) }, nullptr, f.location);
                emit_.Jump(f.epilogue);
                emit_.Bind(go);
            }
        }

        void Compiler::EmitSingletonResolver() {
            if (resolveStubs_.empty()) return;
            current_ = nullptr;
            emit_.ResetTemps();
            for (const auto& [stub, back] : resolveStubs_) {
                emit_.Bind(stub);
                emit_.Copy(emit_.AddressConstant(back), resolveReturnSlot_);
                emit_.Jump(resolveRoutine_);
            }
            emit_.Bind(resolveRoutine_);
            Location location;
            for (const auto& [name, use] : singletons_) {
                Value found = CallStatic(types_.Get("UnityEngineGameObject"), "Find", { Value::OfString("/__UdonLuauSingletons/" + name) }, nullptr, location).at(0);
                Label missing = emit_.NewLabel();
                Label next = emit_.NewLabel();
                emit_.JumpIfFalse(TruthySlot(found, location), missing);
                std::vector<const Method*> getters = types_.Methods(found.type, "GetComponent", false, true);
                std::erase_if(getters, [](const Method* m) { return m->ext->isGeneric; });
                std::vector<Value> args{ Value::OfType(types_.Behaviour()) };
                Value behaviour = Invoke(Resolve(getters, args, nullptr, location, "GameObject.GetComponent"), found, args, location).at(0);
                emit_.Copy(behaviour.slot, use.slot);
                emit_.Jump(next);
                emit_.Bind(missing);
                CallStatic(types_.Get("UnityEngineDebug"), "LogError", { Value::OfString(std::format("[UdonLuau] the {} singleton was not found", name)) }, nullptr, location);
                emit_.Bind(next);
            }
            emit_.Copy(TruthySlot(Value::OfBoolean(true), location), singletonsReadySlot_);
            emit_.JumpIndirect(resolveReturnSlot_);
        }

        void Compiler::CompileAssert(AstExprCall* call) {
            if (call->args.size < 1 || call->args.size > 2) Fail(call->location, "assert takes a condition and an optional message");
            AstExpr* condition = call->args.data[0];
            AstExpr* message = call->args.size > 1 ? call->args.data[1] : nullptr;
            if (auto c = TryConstant(condition)) {
                if (IsTruthy(*c)) return;
                std::optional<Value> text = message ? TryConstant(message) : std::nullopt;
                Fail(call->location, text && text->kind == Value::Kind::String ? text->text : "assertion failed");
            }
            auto debug = defines_.find("DEBUG");
            if (debug == defines_.end() || !IsTruthy(debug->second)) return;
            Label pass = emit_.NewLabel();
            CompileCondition(condition, pass, true);
            Value text = message ? CompileExpr(message) : Value::OfString(std::format("assertion failed at line {}", call->location.begin.line + 1));
            CallStatic(types_.Get("UnityEngineDebug"), "LogError", { text }, nullptr, call->location);
            emit_.Bind(pass);
        }

        std::vector<Value> Compiler::CallBuiltin(std::string_view name, std::vector<Value> args, const Location& location, bool& handled) {
            handled = true;
            if (name == "print" || name == "warn") {
                Value message = args.empty() ? Value::OfString("") : args[0];
                if (args.size() > 1) {
                    std::vector<Value> parts;
                    for (size_t i = 0; i < args.size(); ++i) {
                        if (i) parts.push_back(Value::OfString(" "));
                        parts.push_back(args[i]);
                    }
                    message = ConcatValues(std::move(parts), location);
                }
                return CallStatic(types_.Get("UnityEngineDebug"), name == "print" ? "Log" : "LogWarning", { message }, nullptr, location);
            }
            if (name == "tostring") {
                if (args.size() != 1) Fail(location, "tostring takes one argument");
                return { ToStringValue(args[0], location) };
            }
            handled = false;
            return {};
        }

        std::optional<std::string_view> Compiler::LibraryName(AstExpr* expr) {
            auto* g = Unwrap(expr)->as<AstExprGlobal>();
            if (!g || globalFunctions_.contains(g->name.value) || defines_.contains(g->name.value)) return std::nullopt;
            for (const Polyfill& p : kPolyfills) {
                if (p.library != g->name.value) continue;
                if (!IsStandardLibrary(p.library) && !types_.FindByShortName(p.library).empty()) return std::nullopt;
                return p.library;
            }
            return std::nullopt;
        }

        std::vector<Value> Compiler::CallLibrary(std::string_view library, std::string_view name, AstExprCall* call, std::vector<Value> args, const Type* typeArg, const Variable* into) {
            if (call->self) Fail(call->location, std::format("use '{}.{}(...)'", library, name));
            if (!FindPolyfill(library, name)) Fail(call->location, std::format("{}.{} is not supported; available: {}", library, name, PolyfillNames(library)));
            if (library == "math") return CallMath(name, std::move(args), call->location);
            if (library == "string") return CallString(name, std::move(args), call->location);
            if (library == "Delay") return CallDelay(name, std::move(args), call->location);
            if (library == "bit32") return CallBit32(name, std::move(args), call->location);
            if (library == "utf8") return CallUtf8(name, std::move(args), call->location);
            return CallTable(name, call, std::move(args), typeArg, into);
        }

        std::vector<Value> Compiler::CallMath(std::string_view name, std::vector<Value> args, const Location& location) {
            static const std::map<std::string_view, std::string_view> direct{
                { "abs", "Abs" }, { "sqrt", "Sqrt" }, { "sin", "Sin" }, { "cos", "Cos" }, { "tan", "Tan" }, { "asin", "Asin" }, { "acos", "Acos" },
                { "exp", "Exp" }, { "log10", "Log10" }, { "clamp", "Clamp" }, { "pow", "Pow" }, { "lerp", "LerpUnclamped" } };
            const Type* mathf = types_.Get("UnityEngineMathf");
            const Type* math = types_.Get("SystemMath");
            auto arity = [&](size_t low, size_t high) {
                if (args.size() < low || args.size() > high)
                    Fail(location, low == high ? std::format("math.{} takes {} argument(s)", name, low) : std::format("math.{} takes {} to {} arguments", name, low, high));
            };
            auto integral = [&](const Value& v) {
                const Type* t = NaturalType(v);
                return t && t->IsIntegral();
            };

            if (auto it = direct.find(name); it != direct.end()) {
                size_t count = name == "clamp" || name == "lerp" ? 3 : name == "pow" ? 2 : 1;
                arity(count, count);
                return CallStatic(mathf, it->second, std::move(args), nullptr, location);
            }
            if (name == "floor" || name == "ceil") {
                arity(1, 1);
                if (integral(args[0])) return { args[0] };
                return CallStatic(mathf, name == "floor" ? "Floor" : "Ceil", std::move(args), nullptr, location);
            }
            if (name == "min" || name == "max") {
                if (args.size() < 2) Fail(location, std::format("math.{} takes at least two arguments", name));
                Value result = args[0];
                for (size_t i = 1; i < args.size(); ++i) result = CallStatic(mathf, name == "min" ? "Min" : "Max", { result, args[i] }, nullptr, location).at(0);
                return { result };
            }
            if (name == "atan") {
                arity(1, 2);
                std::string_view method = args.size() == 2 ? "Atan2" : "Atan";
                return CallStatic(mathf, method, std::move(args), nullptr, location);
            }
            if (name == "log") {
                arity(1, 2);
                return CallStatic(mathf, "Log", std::move(args), nullptr, location);
            }
            if (name == "noise") {
                arity(1, 2);
                Value noise = args.size() == 2 ? CallStatic(mathf, "PerlinNoise", std::move(args), nullptr, location).at(0) : CallStatic(mathf, "PerlinNoise1D", std::move(args), nullptr, location).at(0);
                return { CompileArithmetic(AstExprBinary::Sub, CompileArithmetic(AstExprBinary::Mul, noise, Value::OfReal(2), location), Value::OfReal(1), location) };
            }
            if (name == "sign") {
                arity(1, 1);
                return CallStatic(math, "Sign", std::move(args), nullptr, location);
            }
            if (name == "fmod") {
                arity(2, 2);
                return { CompileArithmetic(AstExprBinary::Mod, args[0], args[1], location) };
            }
            if (name == "round") {
                arity(1, 1);
                if (integral(args[0])) return { args[0] };
                const Type* type = NaturalType(args[0]);
                Value rounded = CallStatic(math, "Round", { args[0], Value::OfInteger(1, types_.Get("SystemMidpointRounding")) }, nullptr, location).at(0);
                if (type == types_.Double) return { rounded };
                return { Value::OfSlot(Convert(rounded.slot, types_.Double, types_.Single, location), types_.Single) };
            }
            if (name == "map") {
                arity(5, 5);
                Value scaled = CompileArithmetic(AstExprBinary::Mul, CompileArithmetic(AstExprBinary::Sub, args[0], args[1], location),
                                                 CompileArithmetic(AstExprBinary::Sub, args[4], args[3], location), location);
                return { CompileArithmetic(AstExprBinary::Add, CompileArithmetic(AstExprBinary::Div, scaled, CompileArithmetic(AstExprBinary::Sub, args[2], args[1], location), location),
                                           args[3], location) };
            }
            if (name == "random") {
                arity(0, 2);
                const Type* random = types_.Get("UnityEngineRandom");
                if (args.empty()) return CallStatic(random, "get_value", {}, nullptr, location);
                Value low = args.size() == 2 ? args[0] : Value::OfInteger(1);
                Value high = args.back();
                if (!integral(low) || !integral(high)) Fail(location, "math.random takes integer bounds");
                return CallStatic(random, "Range", { low, CompileArithmetic(AstExprBinary::Add, high, Value::OfInteger(1), location) }, nullptr, location);
            }
            Fail(location, std::format("math.{} is not supported", name));
        }

        std::vector<Value> Compiler::CallString(std::string_view name, std::vector<Value> args, const Location& location) {
            auto text = [&](size_t i) {
                if (i >= args.size() || NaturalType(args[i]) != types_.String) Fail(location, std::format("argument {} of string.{} must be a string", i + 1, name));
                return args[i].kind == Value::Kind::Slot ? args[i] : Value::OfSlot(Materialize(args[i], types_.String, location), types_.String);
            };
            if (name == "len") return CallMember(text(0), "get_Length", {}, nullptr, location);
            if (name == "upper") return CallMember(text(0), "ToUpperInvariant", {}, nullptr, location);
            if (name == "lower") return CallMember(text(0), "ToLowerInvariant", {}, nullptr, location);
            if (name == "split") {
                Value separator = args.size() > 1 ? text(1) : Value::OfString(",");
                return CallMember(text(0), "Split", { separator, Value::OfInteger(0, types_.Get("SystemStringSplitOptions")) }, nullptr, location);
            }
            if (name == "rep") {
                if (args.size() != 2) Fail(location, "string.rep takes a string and a count");
                const Type* builder = types_.Get("SystemTextStringBuilder");
                Value sb = CallStatic(builder, "ctor", {}, nullptr, location).at(0);
                Value filled = CallMember(sb, "Insert", { Value::OfInteger(0), text(0), args[1] }, nullptr, location).at(0);
                return CallMember(filled, "ToString", {}, nullptr, location);
            }
            if (name == "format") {
                if (args.empty() || args[0].kind != Value::Kind::String) Fail(location, "string.format needs a literal format string");
                std::vector<Value> values(args.begin() + 1, args.end());
                Value format = Value::OfString(TranslateFormat(args[0].text, values.size(), location));
                const Type* string = types_.String;
                if (values.empty()) return { format };
                if (values.size() <= 3) {
                    std::vector<Value> all{ format };
                    all.insert(all.end(), values.begin(), values.end());
                    return CallStatic(string, "Format", std::move(all), nullptr, location);
                }
                const Type* objects = types_.ArrayOf(types_.Object);
                Value array = NewArray(objects, Value::OfInteger(static_cast<int64_t>(values.size())), location);
                for (size_t i = 0; i < values.size(); ++i) CallMember(array, "Set", { Value::OfInteger(static_cast<int64_t>(i)), values[i] }, nullptr, location);
                return CallStatic(string, "Format", { format, array }, nullptr, location);
            }
            const Type* stringComparison = types_.Get("SystemStringComparison");
            auto ordinal = [&] { return Value::OfInteger(4, stringComparison); };
            auto held = [&](const Value& v, const Type* type) {
                if (v.kind != Value::Kind::Slot || !emit_.IsTemp(v.slot)) return v;
                Variable h{ emit_.Hidden(type), type };
                Store(v, h, location);
                return Value::OfSlot(h.slot, type);
            };
            auto ascii = [](const std::string& s) { return std::ranges::all_of(s, [](char c) { return static_cast<unsigned char>(c) < 0x80; }); };
            if (name == "trim") return CallMember(text(0), "Trim", {}, nullptr, location);
            if (name == "startswith" || name == "endswith") {
                if (args.size() != 2) Fail(location, std::format("string.{} takes a string and the text to look for", name));
                return CallMember(text(0), name == "startswith" ? "StartsWith" : "EndsWith", { text(1), ordinal() }, nullptr, location);
            }
            if (name == "reverse") {
                if (args.size() == 1 && args[0].kind == Value::Kind::String && ascii(args[0].text)) return { Value::OfString(std::string(args[0].text.rbegin(), args[0].text.rend())) };
                Value chars = CallMember(text(0), "ToCharArray", {}, nullptr, location).at(0);
                CallStatic(types_.Get("SystemArray"), "Reverse", { chars }, nullptr, location);
                return CallStatic(types_.String, "ctor", { chars }, nullptr, location);
            }
            if (name == "byte") {
                if (args.empty() || args.size() > 2) Fail(location, "string.byte takes a string and an optional position");
                Value at = args.size() == 2 ? args[1] : Value::OfInteger(1);
                if (args[0].kind == Value::Kind::String && at.kind == Value::Kind::Integer && ascii(args[0].text) && at.integer >= 1 && at.integer <= static_cast<int64_t>(args[0].text.size()))
                    return { Value::OfInteger(static_cast<unsigned char>(args[0].text[static_cast<size_t>(at.integer - 1)])) };
                Value chars = CallMember(text(0), "ToCharArray", { CompileArithmetic(AstExprBinary::Sub, at, Value::OfInteger(1), location), Value::OfInteger(1) }, nullptr, location).at(0);
                Value c = CallMember(chars, "Get", { Value::OfInteger(0) }, nullptr, location).at(0);
                return CallStatic(types_.Get("SystemConvert"), "ToInt32", { c }, nullptr, location);
            }
            if (name == "char") return CallUtf8("char", std::move(args), location);
            if (name == "sub") {
                if (args.size() < 2 || args.size() > 3) Fail(location, "string.sub takes a string, a start and an optional end");
                Value s = held(text(0), types_.String);
                Value len = held(CallMember(s, "get_Length", {}, nullptr, location).at(0), types_.Int32);
                auto normalize = [&](const Value& index, bool start) {
                    Variable out{ emit_.Hidden(types_.Int32), types_.Int32 };
                    Label negative = emit_.NewLabel();
                    Label done = emit_.NewLabel();
                    emit_.JumpIfFalse(TruthySlot(CompareValues(AstExprBinary::CompareGe, index, Value::OfInteger(0), location), location), negative);
                    Store(index, out, location);
                    emit_.Jump(done);
                    emit_.Bind(negative);
                    Store(CompileArithmetic(AstExprBinary::Add, CompileArithmetic(AstExprBinary::Add, len, index, location), Value::OfInteger(1), location), out, location);
                    emit_.Bind(done);
                    Value v = Value::OfSlot(out.slot, types_.Int32);
                    const Type* mathf = types_.Get("UnityEngineMathf");
                    Value clamped = start ? CallStatic(mathf, "Max", { v, Value::OfInteger(1) }, nullptr, location).at(0) : CallStatic(mathf, "Min", { v, len }, nullptr, location).at(0);
                    Store(clamped, out, location);
                    return v;
                };
                Value first = normalize(args[1], true);
                Value last = args.size() == 3 ? normalize(args[2], false) : len;
                Variable result{ emit_.Hidden(types_.String), types_.String };
                Store(Value::OfString(""), result, location);
                Label empty = emit_.NewLabel();
                Value count = held(CompileArithmetic(AstExprBinary::Add, CompileArithmetic(AstExprBinary::Sub, last, first, location), Value::OfInteger(1), location), types_.Int32);
                emit_.JumpIfFalse(TruthySlot(CompareValues(AstExprBinary::CompareGt, count, Value::OfInteger(0), location), location), empty);
                Store(CallMember(s, "Substring", { CompileArithmetic(AstExprBinary::Sub, first, Value::OfInteger(1), location), count }, nullptr, location).at(0), result, location);
                emit_.Bind(empty);
                return { Value::OfSlot(result.slot, types_.String) };
            }
            if (name == "find") {
                if (args.size() < 2 || args.size() > 4) Fail(location, "string.find takes a string, the text to find, an optional start and plain");
                bool plain = args.size() == 4 && args[3].kind == Value::Kind::Boolean && args[3].boolean;
                if (!plain && (args[1].kind != Value::Kind::String || args[1].text.find_first_of("^$*+?.()[]%-") != std::string::npos))
                    Fail(location, "string.find only does plain text search; pass plain = true, e.g. string.find(s, \"a.b\", 1, true)");
                Value s = held(text(0), types_.String);
                Value needle = held(text(1), types_.String);
                Value from = args.size() >= 3 && args[2].kind != Value::Kind::Nil ? CompileArithmetic(AstExprBinary::Sub, args[2], Value::OfInteger(1), location) : Value::OfInteger(0);
                Value index = held(CallMember(s, "IndexOf", { needle, from, ordinal() }, nullptr, location).at(0), types_.Int32);
                Variable start{ emit_.Hidden(types_.Int32), types_.Int32 };
                Variable end{ emit_.Hidden(types_.Int32), types_.Int32 };
                Store(Value::OfInteger(0), start, location);
                Store(Value::OfInteger(0), end, location);
                Label missing = emit_.NewLabel();
                emit_.JumpIfFalse(TruthySlot(CompareValues(AstExprBinary::CompareGe, index, Value::OfInteger(0), location), location), missing);
                Store(CompileArithmetic(AstExprBinary::Add, index, Value::OfInteger(1), location), start, location);
                Store(CompileArithmetic(AstExprBinary::Add, index, CallMember(needle, "get_Length", {}, nullptr, location).at(0), location), end, location);
                emit_.Bind(missing);
                return { Value::OfSlot(start.slot, types_.Int32), Value::OfSlot(end.slot, types_.Int32) };
            }
            Fail(location, std::format("string.{} is not supported; use the string's own methods, e.g. s:Substring(0, 3)", name));
        }

        std::vector<Value> Compiler::CallBit32(std::string_view name, std::vector<Value> args, const Location& location) {
            auto integer = [&](const Value& v) {
                if (v.kind == Value::Kind::Integer && !v.type) return v;
                if (v.kind == Value::Kind::Slot && v.type == types_.Int32) return v;
                if (Cost(v, types_.Int32) < 0) Fail(location, std::format("bit32.{} works on int values, got {}", name, Describe(v)));
                return Value::OfSlot(Materialize(v, types_.Int32, location), types_.Int32);
            };
            auto op = [&](std::string_view method, const Value& a, const Value& b, int32_t (*fold)(int32_t, int32_t)) {
                if (a.kind == Value::Kind::Integer && b.kind == Value::Kind::Integer)
                    return Value::OfInteger(fold(static_cast<int32_t>(a.integer), static_cast<int32_t>(b.integer)));
                return InvokeOperator(method, { types_.Int32 }, { integer(a), integer(b) }, location).at(0);
            };
            auto chain = [&](std::string_view method, int32_t (*fold)(int32_t, int32_t)) {
                if (args.empty()) Fail(location, std::format("bit32.{} takes at least one value", name));
                Value result = integer(args[0]);
                for (size_t i = 1; i < args.size(); ++i) result = op(method, result, args[i], fold);
                return result;
            };
            int32_t (*band)(int32_t, int32_t) = [](int32_t a, int32_t b) { return a & b; };
            int32_t (*bor)(int32_t, int32_t) = [](int32_t a, int32_t b) { return a | b; };
            int32_t (*bxor)(int32_t, int32_t) = [](int32_t a, int32_t b) { return a ^ b; };
            int32_t (*shl)(int32_t, int32_t) = [](int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) << (b & 31)); };
            int32_t (*sar)(int32_t, int32_t) = [](int32_t a, int32_t b) { return a >> (b & 31); };
            if (name == "band") return { chain("op_LogicalAnd", band) };
            if (name == "bor") return { chain("op_LogicalOr", bor) };
            if (name == "bxor") return { chain("op_LogicalXor", bxor) };
            if (name == "btest") return { CompareValues(AstExprBinary::CompareNe, chain("op_LogicalAnd", band), Value::OfInteger(0), location) };
            if (name == "bnot") {
                if (args.size() != 1) Fail(location, "bit32.bnot takes one value");
                return { op("op_LogicalXor", args[0], Value::OfInteger(-1), bxor) };
            }
            if (args.size() != 2) Fail(location, std::format("bit32.{} takes a value and a shift", name));
            if (name == "lshift") return { op("op_LeftShift", args[0], args[1], shl) };
            if (name == "arshift") return { op("op_RightShift", args[0], args[1], sar) };
            if (name == "rshift") {
                if (args[0].kind == Value::Kind::Integer && args[1].kind == Value::Kind::Integer)
                    return { Value::OfInteger(static_cast<int32_t>(static_cast<uint32_t>(args[0].integer) >> (args[1].integer & 31))) };
                Value shifted = op("op_RightShift", args[0], args[1], sar);
                Value top = op("op_LeftShift", Value::OfInteger(-1), CompileArithmetic(AstExprBinary::Sub, Value::OfInteger(31), args[1], location), shl);
                Value mask = op("op_LogicalXor", op("op_LeftShift", top, Value::OfInteger(1), shl), Value::OfInteger(-1), bxor);
                return { op("op_LogicalAnd", shifted, mask, band) };
            }
            Fail(location, std::format("bit32.{} is not supported", name));
        }

        std::vector<Value> Compiler::CallUtf8(std::string_view name, std::vector<Value> args, const Location& location) {
            if (name == "char") {
                bool literals = std::ranges::all_of(args, [](const Value& v) { return v.kind == Value::Kind::Integer && v.integer >= 0 && v.integer <= 0x10FFFF; });
                if (literals) {
                    std::string out;
                    for (const Value& v : args) {
                        auto cp = static_cast<uint32_t>(v.integer);
                        if (cp < 0x80) out += static_cast<char>(cp);
                        else if (cp < 0x800) out += { static_cast<char>(0xC0 | (cp >> 6)), static_cast<char>(0x80 | (cp & 0x3F)) };
                        else if (cp < 0x10000) out += { static_cast<char>(0xE0 | (cp >> 12)), static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), static_cast<char>(0x80 | (cp & 0x3F)) };
                        else out += { static_cast<char>(0xF0 | (cp >> 18)), static_cast<char>(0x80 | ((cp >> 12) & 0x3F)), static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), static_cast<char>(0x80 | (cp & 0x3F)) };
                    }
                    return { Value::OfString(out) };
                }
                std::vector<Value> parts;
                for (const Value& v : args) parts.push_back(CallStatic(types_.Get("SystemChar"), "ConvertFromUtf32", { v }, nullptr, location).at(0));
                if (parts.empty()) return { Value::OfString("") };
                return { ConcatValues(std::move(parts), location) };
            }
            if (name == "len") {
                if (args.size() != 1 || NaturalType(args[0]) != types_.String) Fail(location, "utf8.len takes a string");
                if (args[0].kind == Value::Kind::String)
                    return { Value::OfInteger(static_cast<int64_t>(std::ranges::count_if(args[0].text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }))) };
                Variable chars{ emit_.Hidden(types_.ArrayOf(types_.Get("SystemChar"))), types_.ArrayOf(types_.Get("SystemChar")) };
                Store(CallMember(args[0], "ToCharArray", {}, nullptr, location).at(0), chars, location);
                Value array = Value::OfSlot(chars.slot, chars.type);
                Variable count{ emit_.Hidden(types_.Int32), types_.Int32 };
                Store(CallMember(array, "get_Length", {}, nullptr, location).at(0), count, location);
                Variable i{ emit_.Hidden(types_.Int32), types_.Int32 };
                Store(Value::OfInteger(0), i, location);
                Value index = Value::OfSlot(i.slot, types_.Int32);
                Value total = Value::OfSlot(count.slot, types_.Int32);
                Variable length{ emit_.Hidden(types_.Int32), types_.Int32 };
                Store(total, length, location);
                Label top = emit_.NewLabel();
                Label next = emit_.NewLabel();
                Label done = emit_.NewLabel();
                emit_.Bind(top);
                emit_.JumpIfFalse(TruthySlot(CompareValues(AstExprBinary::CompareLt, index, Value::OfSlot(length.slot, types_.Int32), location), location), done);
                Value low = CallStatic(types_.Get("SystemChar"), "IsLowSurrogate", { CallMember(array, "Get", { index }, nullptr, location).at(0) }, nullptr, location).at(0);
                emit_.JumpIfFalse(low.slot, next);
                Store(CompileArithmetic(AstExprBinary::Sub, total, Value::OfInteger(1), location), count, location);
                emit_.Bind(next);
                Store(CompileArithmetic(AstExprBinary::Add, index, Value::OfInteger(1), location), i, location);
                emit_.Jump(top);
                emit_.Bind(done);
                return { total };
            }
            Fail(location, std::format("utf8.{} is not supported", name));
        }

        std::string Compiler::TranslateFormat(std::string_view format, size_t count, const Location& location) {
            std::string out;
            size_t next = 0;
            for (size_t i = 0; i < format.size(); ++i) {
                char c = format[i];
                if (c == '{' || c == '}') {
                    out += std::string(2, c);
                    continue;
                }
                if (c != '%') {
                    out += c;
                    continue;
                }
                if (++i >= format.size()) Fail(location, "string.format: '%' at the end of the format");
                if (format[i] == '%') {
                    out += '%';
                    continue;
                }
                bool left = false;
                bool zero = false;
                for (; i < format.size() && (format[i] == '-' || format[i] == '0' || format[i] == '+' || format[i] == ' '); ++i) {
                    left |= format[i] == '-';
                    zero |= format[i] == '0';
                }
                int width = 0;
                for (; i < format.size() && std::isdigit(static_cast<unsigned char>(format[i])); ++i) width = width * 10 + (format[i] - '0');
                int precision = -1;
                if (i < format.size() && format[i] == '.') {
                    precision = 0;
                    for (++i; i < format.size() && std::isdigit(static_cast<unsigned char>(format[i])); ++i) precision = precision * 10 + (format[i] - '0');
                }
                if (i >= format.size()) Fail(location, "string.format: incomplete format specifier");
                if (next >= count) Fail(location, std::format("string.format: the format needs more than {} value(s)", count));
                std::string spec;
                bool padded = false;
                char conversion = format[i];
                switch (conversion) {
                    case 'd': case 'i': case 'x': case 'X':
                        padded = zero && width && !left;
                        if (conversion == 'd' || conversion == 'i') spec = padded ? std::format("D{}", width) : "";
                        else spec = padded ? std::format("{}{}", conversion, width) : std::string(1, conversion);
                        break;
                    case 's': break;
                    case 'f': spec = std::format("F{}", precision < 0 ? 6 : precision); break;
                    case 'g': spec = "G"; break;
                    case 'e': spec = precision < 0 ? "e6" : std::format("e{}", precision); break;
                    default: Fail(location, std::format("string.format: '%{}' is not supported", conversion));
                }
                out += std::format("{{{}", next++);
                if (width && !padded) out += std::format(",{}", left ? -width : width);
                if (!spec.empty()) out += ":" + spec;
                out += '}';
            }
            if (next != count) Fail(location, std::format("string.format: the format uses {} value(s) but {} were given", next, count));
            return out;
        }

        Value Compiler::ArrayArgument(const std::vector<Value>& args, size_t index, std::string_view function, const Location& location) {
            if (index >= args.size() || args[index].kind != Value::Kind::Slot || args[index].type->kind != TypeKind::Array)
                Fail(location, std::format("argument {} of table.{} must be an array", index + 1, function));
            return args[index];
        }

        Value Compiler::NewArray(const Type* arrayType, const Value& length, const Location& location) {
            bool wrapped = arrayType->base && arrayType->base->kind == TypeKind::Array;
            Value array = CallStatic(wrapped ? arrayType->base : arrayType, "ctor", { length }, nullptr, location).at(0);
            array.type = arrayType;
            return array;
        }

        void Compiler::CopyRange(const Value& source, const Value& sourceIndex, const Value& destination, const Value& destinationIndex, const Value& length, const Location& location) {
            CallStatic(types_.Get("SystemArray"), "Copy", { source, sourceIndex, destination, destinationIndex, length }, nullptr, location);
        }

        std::vector<Value> Compiler::CallTable(std::string_view name, AstExprCall* call, std::vector<Value> args, const Type* typeArg, const Variable* into) {
            const Location& location = call->location;
            if (!args.empty() && args[0].kind == Value::Kind::Slot && args[0].type->IsList()) return TableOnList(name, std::move(args), location);
            auto writable = [&](size_t i) {
                AstExpr* target = Unwrap(call->args.data[i]);
                if (!target->is<AstExprLocal>() && !target->is<AstExprIndexName>() && !target->is<AstExprIndexExpr>())
                    Fail(location, std::format("table.{} replaces the array, so its first argument must be a variable or field", name));
                return call->args.data[i];
            };
            auto add = [&](const Value& a, const Value& b) { return CompileArithmetic(AstExprBinary::Add, a, b, location); };
            auto sub = [&](const Value& a, const Value& b) { return CompileArithmetic(AstExprBinary::Sub, a, b, location); };
            auto hold = [&](const Value& v, const Type* type) {
                if (v.kind != Value::Kind::Slot || emit_.IsConstant(v.slot)) return v;
                Variable h{ emit_.Hidden(type), type };
                Store(v, h, location);
                return Value::OfSlot(h.slot, type);
            };
            auto length = [&](const Value& array) { return hold(CallMember(array, "get_Length", {}, nullptr, location).at(0), types_.Int32); };

            if (name == "insert") {
                if (args.size() != 2 && args.size() != 3) Fail(location, "table.insert takes an array, an optional position and a value");
                Value array = ArrayArgument(args, 0, name, location);
                AstExpr* target = writable(0);
                Value len = length(array);
                Value grown = hold(NewArray(array.type, add(len, Value::OfInteger(1)), location), array.type);
                if (args.size() == 2) {
                    CopyRange(array, Value::OfInteger(0), grown, Value::OfInteger(0), len, location);
                    CallMember(grown, "Set", { len, args[1] }, nullptr, location);
                } else {
                    Value at = hold(args[1], types_.Int32);
                    CopyRange(array, Value::OfInteger(0), grown, Value::OfInteger(0), at, location);
                    CopyRange(array, at, grown, add(at, Value::OfInteger(1)), sub(len, at), location);
                    CallMember(grown, "Set", { at, args[2] }, nullptr, location);
                }
                AssignTo(target, grown);
                return { Value::OfNil() };
            }
            if (name == "remove") {
                if (args.empty() || args.size() > 2) Fail(location, "table.remove takes an array and an optional position");
                Value array = ArrayArgument(args, 0, name, location);
                AstExpr* target = writable(0);
                Value last = hold(sub(length(array), Value::OfInteger(1)), types_.Int32);
                Value at = args.size() == 2 ? hold(args[1], types_.Int32) : last;
                Value removed = hold(Narrow(CallMember(array, "Get", { at }, nullptr, location).at(0), array.type->element), array.type->element);
                Value shrunk = hold(NewArray(array.type, last, location), array.type);
                if (args.size() == 2) {
                    CopyRange(array, Value::OfInteger(0), shrunk, Value::OfInteger(0), at, location);
                    CopyRange(array, add(at, Value::OfInteger(1)), shrunk, at, sub(last, at), location);
                } else {
                    CopyRange(array, Value::OfInteger(0), shrunk, Value::OfInteger(0), last, location);
                }
                AssignTo(target, shrunk);
                return { removed };
            }
            if (name == "create") {
                if (args.empty() || args.size() > 2) Fail(location, "table.create takes a length and an optional value");
                const Type* arrayType = nullptr;
                if (typeArg) arrayType = typeArg->kind == TypeKind::Array ? typeArg : types_.ArrayOf(typeArg);
                else if (into && into->type && into->type->kind == TypeKind::Array) arrayType = into->type;
                else if (args.size() == 2 && NaturalType(args[1])) arrayType = types_.ArrayOf(NaturalType(args[1]));
                if (!arrayType) Fail(location, "table.create needs the element type: annotate the variable ('local xs: {int} = table.create(8)') or pass a value");
                Value count = hold(args[0], types_.Int32);
                Value array = hold(NewArray(arrayType, count, location), arrayType);
                if (args.size() == 2) {
                    Label done = emit_.NewLabel();
                    Label top = emit_.NewLabel();
                    if (!(count.kind == Value::Kind::Integer && count.integer > 0))
                        emit_.JumpIfFalse(TruthySlot(CompareValues(AstExprBinary::CompareGt, count, Value::OfInteger(0), location), location), done);
                    CallMember(array, "Set", { Value::OfInteger(0), args[1] }, nullptr, location);
                    Variable filled{ emit_.Hidden(types_.Int32), types_.Int32 };
                    Store(Value::OfInteger(1), filled, location);
                    Value filledValue = Value::OfSlot(filled.slot, types_.Int32);
                    emit_.Bind(top);
                    emit_.JumpIfFalse(TruthySlot(CompareValues(AstExprBinary::CompareLt, filledValue, count, location), location), done);
                    Value chunk = CallStatic(types_.Get("UnityEngineMathf"), "Min", { filledValue, sub(count, filledValue) }, nullptr, location).at(0);
                    CopyRange(array, Value::OfInteger(0), array, filledValue, chunk, location);
                    Store(add(filledValue, filledValue), filled, location);
                    emit_.Jump(top);
                    emit_.Bind(done);
                }
                return { array };
            }
            if (name == "find") {
                if (args.size() < 2 || args.size() > 3) Fail(location, "table.find takes an array, a value and an optional start index");
                ArrayArgument(args, 0, name, location);
                std::vector<const Method*> methods = StaticMethods({ types_.Get("SystemArray") }, "IndexOf");
                std::erase_if(methods, [](const Method* m) { return m->ext->isGeneric; });
                Resolution r = Resolve(methods, args, nullptr, location, "table.find");
                return Invoke(r, std::nullopt, args, location);
            }
            if (name == "clear") {
                if (args.size() != 1) Fail(location, "table.clear takes an array");
                Value array = ArrayArgument(args, 0, name, location);
                CallStatic(types_.Get("SystemArray"), "Clear", { array, Value::OfInteger(0), length(array) }, nullptr, location);
                return { Value::OfNil() };
            }
            if (name == "clone") {
                if (args.size() != 1) Fail(location, "table.clone takes an array");
                Value array = ArrayArgument(args, 0, name, location);
                Value len = length(array);
                Value copy = hold(NewArray(array.type, len, location), array.type);
                CopyRange(array, Value::OfInteger(0), copy, Value::OfInteger(0), len, location);
                return { copy };
            }
            if (name == "move") {
                if (args.size() != 4 && args.size() != 5) Fail(location, "table.move takes a source, first and last index, a destination index and an optional destination");
                Value source = ArrayArgument(args, 0, name, location);
                Value destination = args.size() == 5 ? ArrayArgument(args, 4, name, location) : source;
                Value first = hold(args[1], types_.Int32);
                CopyRange(source, first, destination, args[3], add(sub(args[2], first), Value::OfInteger(1)), location);
                return { destination };
            }
            if (name == "concat") {
                if (args.empty() || args.size() > 2) Fail(location, "table.concat takes an array and an optional separator");
                Value array = ArrayArgument(args, 0, name, location);
                if (array.type->element != types_.String) Fail(location, "table.concat needs an array of strings");
                return CallStatic(types_.String, "Join", { args.size() == 2 ? args[1] : Value::OfString(""), array }, nullptr, location);
            }
            if (name == "sort") {
                if (args.empty() || args.size() > 2) Fail(location, "table.sort takes an array and an optional comparison function");
                Value array = ArrayArgument(args, 0, name, location);
                if (args.size() == 2) {
                    SortWith(array, CallMember(array, "get_Length", {}, nullptr, location).at(0), args[1], location);
                    return { Value::OfNil() };
                }
                CallStatic(types_.Get("SystemArray"), "Sort", { array }, nullptr, location);
                return { Value::OfNil() };
            }
            Fail(location, std::format("table.{} is not supported", name));
        }

        Compiler::ListInit Compiler::ParseListInit(AstExpr* init) const {
            ListInit result;
            if (!init) {
                result.ok = true;
                return result;
            }
            init = Unwrap(init);
            if (auto* table = init->as<AstExprTable>()) {
                for (const AstExprTable::Item& item : table->items) {
                    if (item.kind != AstExprTable::Item::Kind::List) return result;
                    result.items.push_back(item.value);
                }
                result.ok = true;
                return result;
            }
            auto* call = init->as<AstExprCall>();
            auto* index = call && !call->self ? Unwrap(call->func)->as<AstExprIndexName>() : nullptr;
            auto* owner = index ? Unwrap(index->expr)->as<AstExprGlobal>() : nullptr;
            if (!owner || std::string_view(owner->name.value) != "List" || std::string_view(index->index.value) != "new" || call->args.size > 1) return result;
            if (call->args.size) result.capacity = call->args.data[0];
            result.ok = true;
            return result;
        }

        Variable Compiler::DeclareListStorage(const std::string& name, const Type* type, bool field, HeapValue array, int64_t count) {
            HeapValue size;
            size.kind = ValueKind::Integer;
            size.integer = count;
            uint32_t slot = field ? emit_.AddSlot(name, type->listArray, std::move(array)) : emit_.Local(name, type->listArray);
            uint32_t countSlot = field ? emit_.AddSlot("__lstcount_" + name, types_.Int32, size) : emit_.Local(name + "Count", types_.Int32);
            uint32_t capacitySlot = field ? emit_.AddSlot("__lstcap_" + name, types_.Int32, size) : emit_.Local(name + "Capacity", types_.Int32);
            if (countSlot != slot + 1 || capacitySlot != slot + 2) throw std::logic_error("list slots are not adjacent");
            if (field) fieldSlots_.insert({ slot, countSlot, capacitySlot });
            return { slot, type };
        }

        void Compiler::InitList(const Value& list, AstExpr* init, const Location& location) {
            ListInit parsed = ParseListInit(init);
            if (!parsed.ok) Fail(init ? init->location : location, "set a List with {}, {a, b, c} or List.new(capacity); lists cannot be copied, use list:ToArray()");
            const Type* element = list.type->element;
            int64_t n = static_cast<int64_t>(parsed.items.size());
            Value capacity = parsed.capacity ? CompileExpr(parsed.capacity, types_.Int32) : Value::OfInteger(n ? n : 4);
            if (capacity.kind == Value::Kind::Slot && emit_.IsTemp(capacity.slot)) {
                Variable held{ emit_.Hidden(types_.Int32), types_.Int32 };
                Store(capacity, held, location);
                capacity = Value::OfSlot(held.slot, types_.Int32);
            }
            Store(NewArray(list.type->listArray, capacity, location), { list.slot, list.type->listArray }, location);
            for (size_t i = 0; i < parsed.items.size(); ++i)
                CallMember(ListArray(list), "Set", { Value::OfInteger(static_cast<int64_t>(i)), CompileExpr(parsed.items[i], element) }, nullptr, location);
            Store(Value::OfInteger(n), ListCount(list), location);
            Store(capacity, ListCapacity(list), location);
        }

        void Compiler::GrowList(const Value& list, const Value& needed, const Location& location) {
            Label enough = emit_.NewLabel();
            Value capacity = Value::OfSlot(ListCapacity(list).slot, types_.Int32);
            emit_.JumpIfFalse(TruthySlot(CompareValues(AstExprBinary::CompareGt, needed, capacity, location), location), enough);
            Value doubled = CompileArithmetic(AstExprBinary::Mul, capacity, Value::OfInteger(2), location);
            Value grown = CallStatic(types_.Get("UnityEngineMathf"), "Max", { doubled, Value::OfInteger(4) }, nullptr, location).at(0);
            Value array = NewArray(list.type->listArray, grown, location);
            CallStatic(types_.Get("SystemArray"), "Copy", { ListArray(list), array, Value::OfSlot(ListCount(list).slot, types_.Int32) }, nullptr, location);
            Store(array, { list.slot, list.type->listArray }, location);
            Store(grown, ListCapacity(list), location);
            emit_.Bind(enough);
        }

        void Compiler::CheckListIndex(const Value& list, const Value& index, const Location& location) {
            auto debug = defines_.find("DEBUG");
            if (debug == defines_.end() || !IsTruthy(debug->second)) return;
            Label fine = emit_.NewLabel();
            Value outside = CompareValues(AstExprBinary::CompareGe, index, Value::OfSlot(ListCount(list).slot, types_.Int32), location);
            emit_.JumpIfFalse(TruthySlot(outside, location), fine);
            CallStatic(types_.Get("UnityEngineDebug"), "LogError", { Value::OfString(std::format("List index out of range at line {}", location.begin.line + 1)) }, nullptr, location);
            emit_.Bind(fine);
        }

        Value Compiler::ListIndexOf(const Value& list, const Value& item, const Location& location) {
            std::vector<const Method*> methods = StaticMethods({ types_.Get("SystemArray") }, "IndexOf");
            std::erase_if(methods, [](const Method* m) { return m->ext->isGeneric; });
            std::vector<Value> args{ ListArray(list), item, Value::OfInteger(0), Value::OfSlot(ListCount(list).slot, types_.Int32) };
            Resolution r = Resolve(methods, args, nullptr, location, "List:IndexOf");
            return Invoke(r, std::nullopt, args, location).at(0);
        }

        void Compiler::ListRemoveAt(const Value& list, const Value& index, const Location& location) {
            Value count = Value::OfSlot(ListCount(list).slot, types_.Int32);
            Store(CompileArithmetic(AstExprBinary::Sub, count, Value::OfInteger(1), location), ListCount(list), location);
            Value moved = CompileArithmetic(AstExprBinary::Sub, count, index, location);
            CallStatic(types_.Get("SystemArray"), "Copy",
                { ListArray(list), CompileArithmetic(AstExprBinary::Add, index, Value::OfInteger(1), location), ListArray(list), index, moved }, nullptr, location);
            if (list.type->element->IsReference()) CallMember(ListArray(list), "Set", { count, Value::OfNil() }, nullptr, location);
        }

        std::vector<Value> Compiler::CallList(const Value& list, std::string_view name, std::vector<Value> args, const Location& location) {
            const Type* element = list.type->element;
            Value count = Value::OfSlot(ListCount(list).slot, types_.Int32);
            auto arity = [&](size_t n) {
                if (args.size() != n) Fail(location, std::format("List:{} takes {} argument(s)", name, n));
            };
            auto held = [&](const Value& v, const Type* type) {
                if (v.kind != Value::Kind::Slot || !emit_.IsTemp(v.slot)) return v;
                Variable h{ emit_.Hidden(type), type };
                Store(v, h, location);
                return Value::OfSlot(h.slot, type);
            };
            if (name == "Add") {
                arity(1);
                Value next = held(CompileArithmetic(AstExprBinary::Add, count, Value::OfInteger(1), location), types_.Int32);
                GrowList(list, next, location);
                CallMember(ListArray(list), "Set", { count, args[0] }, nullptr, location);
                Store(next, ListCount(list), location);
                return { Value::OfNil() };
            }
            if (name == "Insert") {
                arity(2);
                Value at = held(args[0], types_.Int32);
                Value next = held(CompileArithmetic(AstExprBinary::Add, count, Value::OfInteger(1), location), types_.Int32);
                GrowList(list, next, location);
                CallStatic(types_.Get("SystemArray"), "Copy",
                    { ListArray(list), at, ListArray(list), CompileArithmetic(AstExprBinary::Add, at, Value::OfInteger(1), location),
                      CompileArithmetic(AstExprBinary::Sub, count, at, location) }, nullptr, location);
                CallMember(ListArray(list), "Set", { at, args[1] }, nullptr, location);
                Store(next, ListCount(list), location);
                return { Value::OfNil() };
            }
            if (name == "RemoveAt") {
                arity(1);
                CheckListIndex(list, args[0], location);
                ListRemoveAt(list, held(args[0], types_.Int32), location);
                return { Value::OfNil() };
            }
            if (name == "Remove") {
                arity(1);
                Value index = held(ListIndexOf(list, args[0], location), types_.Int32);
                Variable found{ emit_.Hidden(types_.Boolean), types_.Boolean };
                Store(CompareValues(AstExprBinary::CompareGe, index, Value::OfInteger(0), location), found, location);
                Label skip = emit_.NewLabel();
                emit_.JumpIfFalse(found.slot, skip);
                ListRemoveAt(list, index, location);
                emit_.Bind(skip);
                return { Value::OfSlot(found.slot, types_.Boolean) };
            }
            if (name == "IndexOf") {
                arity(1);
                return { ListIndexOf(list, args[0], location) };
            }
            if (name == "Contains") {
                arity(1);
                return { CompareValues(AstExprBinary::CompareGe, ListIndexOf(list, args[0], location), Value::OfInteger(0), location) };
            }
            if (name == "Clear") {
                arity(0);
                if (element->IsReference()) CallStatic(types_.Get("SystemArray"), "Clear", { ListArray(list), Value::OfInteger(0), count }, nullptr, location);
                Store(Value::OfInteger(0), ListCount(list), location);
                return { Value::OfNil() };
            }
            if (name == "ToArray") {
                arity(0);
                Value copy = NewArray(list.type->listArray, count, location);
                CallStatic(types_.Get("SystemArray"), "Copy", { ListArray(list), copy, count }, nullptr, location);
                return { copy };
            }
            if (name == "Sort" || name == "Reverse") {
                arity(0);
                CallStatic(types_.Get("SystemArray"), name, { ListArray(list), Value::OfInteger(0), count }, nullptr, location);
                return { Value::OfNil() };
            }
            if (name == "Get") {
                arity(1);
                CheckListIndex(list, args[0], location);
                return { Narrow(CallMember(ListArray(list), "Get", { args[0] }, nullptr, location).at(0), element) };
            }
            if (name == "Set") {
                arity(2);
                CheckListIndex(list, args[0], location);
                CallMember(ListArray(list), "Set", { args[0], args[1] }, nullptr, location);
                return { Value::OfNil() };
            }
            Fail(location, std::format("List has no method '{}'; available: Add, Insert, Remove, RemoveAt, IndexOf, Contains, Clear, ToArray, Sort, Reverse, Get, Set", name));
        }

        void Compiler::SortWith(const Value& array, const Value& length, const Value& comparer, const Location& location) {
            if (comparer.kind != Value::Kind::Function) Fail(location, "the comparison function must be a named local function, e.g. table.sort(xs, byScore)");
            Function* less = comparer.function;
            const Type* element = array.type->element;
            if (less->parameters.size() != 2 || less->returns.size() != 1 || less->returns[0] != types_.Boolean)
                Fail(location, std::format("'{}' must take two {} values and return a boolean", less->name, element->displayName));

            Variable count{ emit_.Hidden(types_.Int32), types_.Int32 };
            Store(length, count, location);
            Variable i{ emit_.Hidden(types_.Int32), types_.Int32 };
            Variable j{ emit_.Hidden(types_.Int32), types_.Int32 };
            Variable key{ emit_.Hidden(element), element };
            Variable other{ emit_.Hidden(element), element };
            Value iv = Value::OfSlot(i.slot, types_.Int32);
            Value jv = Value::OfSlot(j.slot, types_.Int32);
            auto get = [&](const Value& index) { return Narrow(CallMember(array, "Get", { index }, nullptr, location).at(0), element); };

            Store(Value::OfInteger(1), i, location);
            Label outer = emit_.NewLabel();
            Label inner = emit_.NewLabel();
            Label place = emit_.NewLabel();
            Label finished = emit_.NewLabel();
            emit_.Bind(outer);
            emit_.JumpIfFalse(TruthySlot(CompareValues(AstExprBinary::CompareLt, iv, Value::OfSlot(count.slot, types_.Int32), location), location), finished);
            Store(get(iv), key, location);
            Store(CompileArithmetic(AstExprBinary::Sub, iv, Value::OfInteger(1), location), j, location);
            emit_.Bind(inner);
            emit_.JumpIfFalse(TruthySlot(CompareValues(AstExprBinary::CompareGe, jv, Value::OfInteger(0), location), location), place);
            Store(get(jv), other, location);
            Value before = CallFunction(less, { Value::OfSlot(key.slot, element), Value::OfSlot(other.slot, element) }, 1, location, nullptr).at(0);
            emit_.JumpIfFalse(TruthySlot(before, location), place);
            CallMember(array, "Set", { CompileArithmetic(AstExprBinary::Add, jv, Value::OfInteger(1), location), Value::OfSlot(other.slot, element) }, nullptr, location);
            Store(CompileArithmetic(AstExprBinary::Sub, jv, Value::OfInteger(1), location), j, location);
            emit_.Jump(inner);
            emit_.Bind(place);
            CallMember(array, "Set", { CompileArithmetic(AstExprBinary::Add, jv, Value::OfInteger(1), location), Value::OfSlot(key.slot, element) }, nullptr, location);
            Store(CompileArithmetic(AstExprBinary::Add, iv, Value::OfInteger(1), location), i, location);
            emit_.Jump(outer);
            emit_.Bind(finished);
        }

        std::vector<Value> Compiler::TableOnList(std::string_view name, std::vector<Value> args, const Location& location) {
            Value list = args[0];
            std::vector<Value> rest(args.begin() + 1, args.end());
            if (name == "insert") {
                std::string_view method = rest.size() == 1 ? "Add" : "Insert";
                return CallList(list, method, std::move(rest), location);
            }
            if (name == "remove") {
                if (rest.size() > 1) Fail(location, "table.remove takes a list and an optional position");
                Value index = rest.empty() ? CompileArithmetic(AstExprBinary::Sub, Value::OfSlot(ListCount(list).slot, types_.Int32), Value::OfInteger(1), location) : rest[0];
                if (index.kind == Value::Kind::Slot && emit_.IsTemp(index.slot)) {
                    Variable h{ emit_.Hidden(types_.Int32), types_.Int32 };
                    Store(index, h, location);
                    index = Value::OfSlot(h.slot, types_.Int32);
                }
                CheckListIndex(list, index, location);
                Value removed = Narrow(CallMember(ListArray(list), "Get", { index }, nullptr, location).at(0), list.type->element);
                Variable kept{ emit_.Hidden(list.type->element), list.type->element };
                Store(removed, kept, location);
                ListRemoveAt(list, index, location);
                return { Value::OfSlot(kept.slot, kept.type) };
            }
            if (name == "find") {
                if (rest.size() != 1) Fail(location, "table.find on a List takes the value to find");
                return { ListIndexOf(list, rest[0], location) };
            }
            if (name == "clear") return CallList(list, "Clear", std::move(rest), location);
            if (name == "sort") {
                if (rest.size() == 1) {
                    SortWith(ListArray(list), Value::OfSlot(ListCount(list).slot, types_.Int32), rest[0], location);
                    return { Value::OfNil() };
                }
                return CallList(list, "Sort", std::move(rest), location);
            }
            if (name == "concat") {
                if (list.type->element != types_.String) Fail(location, "table.concat needs a list of strings");
                Value separator = rest.empty() ? Value::OfString("") : rest[0];
                return CallStatic(types_.String, "Join", { separator, ListArray(list), Value::OfInteger(0), Value::OfSlot(ListCount(list).slot, types_.Int32) }, nullptr, location);
            }
            Fail(location, std::format("table.{} does not work on a List", name));
        }

        std::vector<Value> Compiler::Inline(Function* f, std::vector<Value> args, size_t want, const Location& location, const Variable* into) {
            for (const InlineFrame& frame : inlineStack_)
                if (frame.function == f) Fail(location, std::format("recursion is not supported: '{}' calls itself", f->name));
            if (current_) current_->callees.push_back(f);

            for (size_t i = 0; i < args.size(); ++i) {
                AstLocal* param = f->node->args.data[i];
                const Type* type = f->parameters[i].type;
                Value a = args[i];
                aliases_.erase(param);
                variables_.erase(param);
                if (type->IsList()) {
                    if (a.kind != Value::Kind::Slot || a.type != type) Fail(location, std::format("argument {} of '{}' must be {}", i + 1, f->name, type->displayName));
                    aliases_[param] = a;
                    continue;
                }

                bool callerOwned = a.kind != Value::Kind::Slot || !fieldSlots_.contains(a.slot);
                if (Immutable(param, type) && callerOwned) {
                    if (a.IsLiteral()) {
                        aliases_[param] = CoerceLiteral(a, type, location);
                        continue;
                    }
                    if (a.kind == Value::Kind::Slot) {
                        uint32_t slot = Materialize(a, type, location);
                        if (emit_.IsTemp(slot)) emit_.Promote(slot);
                        aliases_[param] = Value::OfSlot(slot, type);
                        continue;
                    }
                }
                if (a.kind == Value::Kind::Slot && a.type == type && emit_.IsTemp(a.slot)) {
                    emit_.Promote(a.slot);
                    variables_[param] = { a.slot, type };
                    continue;
                }
                Variable local{ emit_.Local(param->name.value, type), type };
                Store(a, local, location);
                variables_[param] = local;
            }

            InlineFrame frame;
            frame.function = f;
            frame.end = emit_.NewLabel();
            for (size_t i = 0; i < f->returns.size(); ++i) {
                if (i >= std::max<size_t>(want, 1)) break;
                if (i == 0 && into && into->type == f->returns[0]) frame.results.push_back(*into);
                else frame.results.push_back({ emit_.Temp(f->returns[i]), f->returns[i] });
            }

            std::vector<LoopLabels> savedLoops = std::move(loops_);
            loops_.clear();
            inlineStack_.push_back(frame);
            try {
                AstStat* tail = nullptr;
                AstStatBlock* body = f->node->body;
                tail = body->body.size && body->body.data[body->body.size - 1]->is<AstStatReturn>() ? body->body.data[body->body.size - 1] : nullptr;
                inlineStack_.back().tail = tail;
                CompileBlock(body);
            } catch (...) {
                inlineStack_.pop_back();
                loops_ = std::move(savedLoops);
                throw;
            }
            emit_.Bind(inlineStack_.back().end);
            std::vector<Variable> results = std::move(inlineStack_.back().results);
            inlineStack_.pop_back();
            loops_ = std::move(savedLoops);

            std::vector<Value> out;
            for (size_t i = 0; i < std::min(want, results.size()); ++i) out.push_back(Value::OfSlot(results[i].slot, results[i].type));
            return out;
        }

        std::vector<Value> Compiler::CallFunction(Function* f, std::vector<Value> args, size_t want, const Location& location, const Variable* into) {
            if (args.size() != f->parameters.size())
                Fail(location, std::format("'{}' takes {} argument(s), got {}", f->name, f->parameters.size(), args.size()));
            for (size_t i = 0; i < args.size(); ++i)
                if (Cost(args[i], f->parameters[i].type) < 0)
                    Fail(location, std::format("argument {} of '{}' must be {}, got {}", i + 1, f->name, f->parameters[i].type->displayName, Describe(args[i])));
            if (f->inlined || (f->calls == 0 && !f->exported)) return Inline(f, std::move(args), want, location, into);
            std::vector<uint32_t> slots;
            for (size_t i = 0; i < args.size(); ++i) {
                if (Cost(args[i], f->parameters[i].type) < 0)
                    Fail(location, std::format("argument {} of '{}' must be {}, got {}", i + 1, f->name, f->parameters[i].type->displayName, Describe(args[i])));
                slots.push_back(Materialize(args[i], f->parameters[i].type, location));
            }
            for (size_t i = 0; i < slots.size(); ++i) emit_.Copy(slots[i], f->parameters[i].slot);

            if (current_) current_->callees.push_back(f);
            Label back = emit_.NewLabel();
            emit_.Push(emit_.AddressConstant(back));
            emit_.Jump(f->entry);
            emit_.Bind(back);

            std::vector<Value> results;
            for (size_t i = 0; i < std::min(want, f->returns.size()); ++i) {
                uint32_t t = i == 0 && into && into->type == f->returns[0] ? into->slot : emit_.Temp(f->returns[i]);
                emit_.Copy(f->returnSlots[i], t);
                results.push_back(Value::OfSlot(t, f->returns[i]));
            }
            return results;
        }

        std::vector<const Method*> Compiler::MembersWithRenames(const Type* owner, std::string_view name, bool wantStatic) {
            auto methods = types_.Methods(owner, name, wantStatic, true);
            if (!methods.empty()) return methods;
            static constexpr std::pair<std::string_view, std::string_view> kUnity6Renames[] = {
                { "linearVelocity", "velocity" },
                { "linearVelocityX", "velocityX" },
                { "linearVelocityY", "velocityY" },
                { "linearDamping", "drag" },
                { "angularDamping", "angularDrag" },
            };
            std::string_view prefix;
            std::string_view member = name;
            if (member.starts_with("get_") || member.starts_with("set_")) {
                prefix = member.substr(0, 4);
                member.remove_prefix(4);
            }
            for (auto [unity6, unity2022] : kUnity6Renames) {
                std::string_view other = member == unity6 ? unity2022 : member == unity2022 ? unity6 : std::string_view{};
                if (other.empty()) continue;
                methods = types_.Methods(owner, std::string(prefix) + std::string(other), wantStatic, true);
                if (!methods.empty()) return methods;
            }
            return methods;
        }

        std::vector<const Method*> Compiler::StaticMethods(const std::vector<const Type*>& owners, std::string_view name) {
            static constexpr std::pair<std::string_view, std::string_view> kSpellings[] = {
                { "op_Multiply", "op_Multiplication" },
                { "op_Modulus", "op_Remainder" },
                { "op_UnaryNegation", "op_UnaryMinus" },
            };
            std::vector<std::string_view> names{ name };
            for (auto [a, b] : kSpellings) {
                if (name == a) names.push_back(b);
                if (name == b) names.push_back(a);
            }
            std::vector<const Method*> out;
            for (std::string_view spelling : names)
            for (const Type* owner : owners)
                for (const Method* m : types_.Methods(owner, spelling, true, true))
                    if (std::ranges::find(out, m) == out.end() &&
                        std::ranges::none_of(out, [&](const Method* o) { return o->ext->parameterText == m->ext->parameterText && o->ext->returnType == m->ext->returnType; }))
                        out.push_back(m);
            return out;
        }

        std::vector<Value> Compiler::CallMember(const Value& receiver, std::string_view name, std::vector<Value> args, const Type* typeArg, const Location& location) {
            if (receiver.kind != Value::Kind::Slot || !receiver.type) Fail(location, std::format("cannot call '{}' on {}", name, Describe(receiver)));
            if (name == "GetComponent" || name == "GetComponentInChildren" || name == "GetComponentInParent") {
                const Type* wanted = typeArg ? typeArg : args.size() == 1 && args[0].kind == Value::Kind::TypeRef ? args[0].type : nullptr;
                if (wanted && (wanted->script || wanted == types_.Behaviour())) return { FindBehaviour(receiver, name, wanted, location) };
            }
            bool delayed = name == "SendCustomEventDelayedSeconds" || name == "SendCustomEventDelayedFrames";
            if (delayed && args.size() == 2) args.push_back(Value::OfInteger(0, types_.Get("VRCUdonCommonEnumsEventTiming")));
            size_t eventArg = name == "SendCustomNetworkEvent" ? 1 : 0;
            if ((delayed || name == "SendCustomEvent" || name == "SendCustomNetworkEvent") && args.size() > eventArg && args[eventArg].kind == Value::Kind::String &&
                receiver.slot == SelfValue("this").slot) {
                const std::string& event = args[eventArg].text;
                for (const auto& f : functions_) {
                    if (f->name != event || f->event) continue;
                    if (!f->exported) Report(location, std::format("'{}' is a local function and has no entry point; export it, or call it with Delay", event), Severity::Warning);
                    else if (f->entryName != event) Report(location, std::format("'{}' runs as entry point '{}'; send that name, or call it with Delay or Network", event, f->entryName), Severity::Warning);
                }
            }
            auto methods = MembersWithRenames(receiver.type, name, false);
            if (methods.empty()) {
                std::string_view shown = name;
                if (shown.starts_with("get_") || shown.starts_with("set_")) shown.remove_prefix(4);
                Fail(location, std::format("{} has no member '{}'", receiver.type->displayName, shown));
            }
            if (!typeArg && !args.empty() && args.back().kind == Value::Kind::TypeRef) {
                std::vector<const Method*> generic;
                for (const Method* m : methods)
                    if (m->ext->isGeneric) generic.push_back(m);
                std::vector<Value> rest(args.begin(), args.end() - 1);
                bool ambiguous = false;
                if (auto r = TryResolve(generic, rest, args.back().type, ambiguous)) return Invoke(*r, receiver, rest, location);
            }
            Resolution r = Resolve(methods, args, typeArg, location, std::format("{}.{}", receiver.type->displayName, name));
            return Invoke(r, receiver, args, location);
        }

        Value Compiler::FindBehaviour(const Value& receiver, std::string_view getter, const Type* wanted, const Location& location) {
            std::string plural = "GetComponents" + std::string(getter.substr(std::string_view("GetComponent").size()));
            auto nonGeneric = [&](std::string_view method, const Value& owner) {
                std::vector<const Method*> methods = MembersWithRenames(owner.type, method, false);
                std::erase_if(methods, [](const Method* m) { return m->ext->isGeneric; });
                std::vector<Value> args{ Value::OfType(types_.Behaviour()) };
                return Invoke(Resolve(methods, args, nullptr, location, std::format("{}.{}", owner.type->displayName, method)), owner, args, location).at(0);
            };
            if (!wanted->script) {
                Value found = nonGeneric(getter, receiver);
                found.type = types_.Behaviour();
                return found;
            }
            const std::string& typeName = wanted->script->typeName;
            if (typeName.empty()) Fail(location, std::format("{} has no registered type name, so it cannot be found with {}", wanted->displayName, getter));

            Variable result{ emit_.Hidden(wanted), wanted };
            Store(Value::OfNil(), Variable{ result.slot, types_.Behaviour() }, location);
            Variable components{ emit_.Hidden(types_.ArrayOf(types_.Behaviour())), types_.ArrayOf(types_.Behaviour()) };
            Value all = nonGeneric(plural, receiver);
            emit_.Copy(all.slot, components.slot);
            Value list = Value::OfSlot(components.slot, components.type);
            Variable index{ emit_.Hidden(types_.Int32), types_.Int32 };
            Store(Value::OfInteger(0), index, location);
            Variable count{ emit_.Hidden(types_.Int32), types_.Int32 };
            Store(CallMember(list, "get_Length", {}, nullptr, location).at(0), count, location);

            Label top = emit_.NewLabel();
            Label next = emit_.NewLabel();
            Label done = emit_.NewLabel();
            Value i = Value::OfSlot(index.slot, types_.Int32);
            emit_.Bind(top);
            emit_.JumpIfFalse(TruthySlot(CompareValues(AstExprBinary::CompareLt, i, Value::OfSlot(count.slot, types_.Int32), location), location), done);
            Value candidate = CallMember(list, "Get", { i }, nullptr, location).at(0);
            candidate.type = types_.Behaviour();
            Value name = CallMember(candidate, "GetProgramVariable", { Value::OfString("__refl_typename") }, nullptr, location).at(0);
            name.type = types_.String;
            emit_.JumpIfFalse(TruthySlot(CompareValues(AstExprBinary::CompareEq, name, Value::OfString(typeName), location), location), next);
            emit_.Copy(candidate.slot, result.slot);
            emit_.Jump(done);
            emit_.Bind(next);
            Store(CompileArithmetic(AstExprBinary::Add, i, Value::OfInteger(1), location), index, location);
            emit_.Jump(top);
            emit_.Bind(done);
            return Value::OfSlot(result.slot, wanted);
        }

        std::vector<Value> Compiler::CallStatic(const Type* owner, std::string_view name, std::vector<Value> args, const Type* typeArg, const Location& location) {
            auto methods = name == "ctor" ? types_.Methods(owner, name, true, false) : MembersWithRenames(owner, name, true);
            if (methods.empty()) {
                if (name == "ctor") Fail(location, std::format("{} has no constructor", owner->displayName));
                std::string_view shown = name;
                if (shown.starts_with("get_") || shown.starts_with("set_")) shown.remove_prefix(4);
                Fail(location, std::format("{} has no static member '{}'", owner->displayName, shown));
            }
            if (!typeArg && !args.empty() && args.back().kind == Value::Kind::TypeRef) {
                std::vector<const Method*> generic;
                for (const Method* m : methods)
                    if (m->ext->isGeneric) generic.push_back(m);
                std::vector<Value> rest(args.begin(), args.end() - 1);
                bool ambiguous = false;
                if (auto r = TryResolve(generic, rest, args.back().type, ambiguous)) return Invoke(*r, std::nullopt, rest, location);
            }
            Resolution r = Resolve(methods, args, typeArg, location, std::format("{}.{}", owner->displayName, name == "ctor" ? "new" : name));
            return Invoke(r, std::nullopt, args, location);
        }

        std::vector<Value> Compiler::InvokeOperator(std::string_view opName, const std::vector<const Type*>& owners, std::vector<Value> args, const Location& location) {
            auto methods = StaticMethods(owners, opName);
            Resolution r = Resolve(methods, args, nullptr, location, std::format("operator {}", opName));
            return Invoke(r, std::nullopt, args, location);
        }

        const Type* Compiler::InputType(const Parameter& p, const Type* typeArg) {
            if (!p.generic) return p.type;
            if (!typeArg) return nullptr;
            return p.genericArray ? types_.ArrayOf(typeArg) : typeArg;
        }

        std::optional<Compiler::Resolution> Compiler::TryResolve(const std::vector<const Method*>& methods, const std::vector<Value>& args, const Type* typeArg, bool& ambiguous) {
            int bestCost = std::numeric_limits<int>::max();
            ambiguous = false;
            const Method* bestMethod = nullptr;
            const Type* bestTypeArg = nullptr;
            for (const Method* m : methods) {
                std::vector<const Parameter*> inputs;
                for (const Parameter& p : m->parameters)
                    if (!p.byRef) inputs.push_back(&p);
                if (inputs.size() != args.size()) continue;

                const Type* generic = m->ext->isGeneric ? typeArg : nullptr;
                if (m->ext->isGeneric && !generic) {
                    for (size_t i = 0; i < args.size() && !generic; ++i) {
                        if (!inputs[i]->generic) continue;
                        const Type* t = NaturalType(args[i]);
                        if (!t) continue;
                        if (!inputs[i]->genericArray) generic = t;
                        else if (t->kind == TypeKind::Array) generic = t->element;
                    }
                    if (!generic) continue;
                }

                int total = 0;
                bool ok = true;
                for (size_t i = 0; i < args.size() && ok; ++i) {
                    const Type* to = InputType(*inputs[i], generic);
                    int c = to ? Cost(args[i], to) : -1;
                    if (c < 0) ok = false;
                    else total += c;
                }
                if (!ok) continue;
                if (total < bestCost) {
                    bestMethod = m;
                    bestTypeArg = generic;
                    bestCost = total;
                    ambiguous = false;
                } else if (total == bestCost) {
                    ambiguous = true;
                }
            }
            if (!bestMethod || ambiguous) return std::nullopt;
            return Resolution{ bestMethod, bestTypeArg };
        }

        Compiler::Resolution Compiler::Resolve(const std::vector<const Method*>& methods, const std::vector<Value>& args, const Type* typeArg, const Location& location, std::string_view what) {
            bool ambiguous = false;
            if (auto r = TryResolve(methods, args, typeArg, ambiguous)) return *r;

            std::string given;
            for (const Value& v : args) given += (given.empty() ? "" : ", ") + Describe(v);
            std::string options;
            for (const Method* m : methods) options += "\n  " + Describe(*m);
            if (ambiguous) Fail(location, std::format("call to {}({}) is ambiguous; candidates:{}", what, given, options));
            if (methods.empty()) Fail(location, std::format("{} does not exist", what));
            Fail(location, std::format("no overload of {} accepts ({}); candidates:{}", what, given, options));
        }

        std::vector<Value> Compiler::Invoke(const Resolution& r, const std::optional<Value>& receiver, const std::vector<Value>& args, const Location& location) {
            const Method& m = *r.method;
            if (m.ext->isConstructor && !m.ext->isGeneric && !receiver) {
                const Type* constructed = types_.Get(m.ext->returnType);
                bool literals = constructed->IsValueType() && std::ranges::all_of(args, [](const Value& a) { return a.IsLiteral() && a.kind != Value::Kind::Nil; }) &&
                                std::ranges::none_of(m.parameters, [](const Parameter& p) { return p.byRef || p.generic; });
                if (literals) {
                    HeapValue value;
                    value.kind = ValueKind::Construct;
                    value.text = m.ext->signature;
                    for (size_t i = 0; i < args.size(); ++i) value.arguments.push_back(Literal(args[i], m.parameters[i].type, location));
                    return { Value::OfSlot(emit_.Constant(constructed, value), constructed) };
                }
            }
            bool foldable = !receiver && IsFoldableCall(*m.ext) && std::ranges::all_of(args, [](const Value& a) { return a.IsLiteral() && a.kind != Value::Kind::Nil; }) &&
                            std::ranges::none_of(m.parameters, [](const Parameter& p) { return p.byRef || p.generic; });
            if (foldable && m.ext->returnType != "SystemVoid") {
                const Type* t = types_.Get(m.ext->returnType);
                HeapValue value;
                value.kind = ValueKind::Construct;
                value.text = m.ext->signature;
                for (size_t i = 0; i < args.size(); ++i) value.arguments.push_back(Literal(args[i], m.parameters[i].type, location));
                return { Value::OfSlot(emit_.Constant(t, value), t) };
            }
            if (!receiver && args.empty() && IsConstantGetter(*m.ext)) {
                const Type* t = types_.Get(m.ext->returnType);
                HeapValue value;
                value.kind = ValueKind::Construct;
                value.text = m.ext->signature;
                return { Value::OfSlot(emit_.Constant(t, value), t) };
            }
            std::vector<uint32_t> inputs;
            size_t argIndex = 0;
            for (const Parameter& p : m.parameters) {
                if (p.byRef) continue;
                inputs.push_back(Materialize(args[argIndex++], InputType(p, r.typeArgument), location));
            }

            std::vector<uint32_t> pushes;
            std::vector<Value> refResults;
            if (receiver) {
                uint32_t self = receiver->slot;
                if (receiver->type && receiver->type->IsValueType() && emit_.IsConstant(self) && !m.ext->method.starts_with("get_")) {
                    uint32_t copy = emit_.Temp(receiver->type);
                    emit_.Copy(self, copy);
                    self = copy;
                }
                pushes.push_back(self);
            }
            size_t inputIndex = 0;
            for (const Parameter& p : m.parameters) {
                if (p.byRef) {
                    uint32_t t = emit_.Temp(p.type);
                    pushes.push_back(t);
                    refResults.push_back(Value::OfSlot(t, p.type));
                } else {
                    pushes.push_back(inputs[inputIndex++]);
                }
            }
            if (m.ext->hasTypeOperand) {
                HeapValue tv;
                tv.kind = ValueKind::Type;
                tv.text = r.typeArgument->udonName;
                pushes.push_back(emit_.Constant(types_.SystemType, tv));
            }

            std::vector<Value> results;
            const Type* returnType = m.returnType;
            if (m.returnGeneric) returnType = m.returnGenericArray ? types_.ArrayOf(r.typeArgument) : r.typeArgument;
            if (m.ext->isConstructor) returnType = types_.Get(m.ext->returnType);
            uint32_t externSlot = emit_.ExternSlot(m.ext->signature);
            bool stable = receiver && returnType && refResults.empty() && IsStableRead(*m.ext, receiver->type);
            if (stable)
                if (auto cached = emit_.CachedRead(externSlot, pushes.front())) return { Value::OfSlot(*cached, returnType) };
            if (returnType) {
                uint32_t t = emit_.Temp(returnType);
                pushes.push_back(t);
                results.push_back(Value::OfSlot(t, returnType));
            }

            for (uint32_t s : pushes) emit_.Push(s);
            emit_.Extern(externSlot, returnType != nullptr, refResults.empty() && HasNoSideEffects(*m.ext, receiver.has_value()));
            if (stable) emit_.RememberRead(externSlot, pushes.front(), results.front().slot);
            if (!returnType) results.push_back(Value::OfNil());
            for (Value& v : refResults) results.push_back(std::move(v));
            return results;
        }

        int Compiler::Cost(const Value& v, const Type* to) const {
            if (!to) return -1;
            const bool toObject = to == types_.Object;
            switch (v.kind) {
                case Value::Kind::Slot: {
                    const Type* from = v.type;
                    if (from->IsList() || to->IsList()) return from == to ? 0 : -1;
                    if (from == to) return 0;
                    if (from->IsNumeric() && to->IsNumeric())
                        return TypeTable::ImplicitNumeric(from->numeric, to->numeric) ? 10 + NumericRank(to->numeric) - NumericRank(from->numeric) : -1;
                    int d = types_.ReferenceDistance(from, to);
                    if (d < 0 && toObject) d = 16;
                    if (d < 0) return -1;
                    return (from->IsValueType() ? 40 : 20) + d;
                }
                case Value::Kind::Integer:
                    if (v.type) {
                        if (v.type == to) return 0;
                        return toObject ? 40 : -1;
                    }
                    if (to->IsIntegral()) {
                        if (!FitsIntegral(v.integer, to->numeric)) return -1;
                        return to == types_.Int32 ? 0 : 2 + std::abs(NumericRank(to->numeric) - NumericRank(Numeric::Int32));
                    }
                    if (to == types_.Single) return 3;
                    if (to == types_.Double) return 4;
                    return toObject ? 40 : -1;
                case Value::Kind::Real:
                    if (to == types_.Single) return 0;
                    if (to == types_.Double) return 1;
                    return toObject ? 40 : -1;
                case Value::Kind::Boolean:
                    if (to == types_.Boolean) return 0;
                    return toObject ? 40 : -1;
                case Value::Kind::String: {
                    if (to == types_.String) return 0;
                    if (to->numeric == Numeric::Char) return IsSingleChar(v.text) ? 30 : -1;
                    if (to->kind == TypeKind::Array && to->element && to->element->numeric == Numeric::Char) return 30;
                    int d = types_.ReferenceDistance(types_.String, to);
                    if (d < 0 && toObject) d = 16;
                    return d < 0 ? -1 : 20 + d;
                }
                case Value::Kind::Nil:
                    if (to->IsValueType()) return -1;
                    return toObject ? 2 : 1;
                case Value::Kind::TypeRef:
                    if (to == types_.SystemType) return 0;
                    return toObject ? 20 : -1;
                default:
                    return -1;
            }
        }

        const Type* Compiler::NaturalType(const Value& v) const {
            switch (v.kind) {
                case Value::Kind::Slot: return v.type;
                case Value::Kind::Integer:
                    if (v.type) return v.type;
                    return FitsIntegral(v.integer, Numeric::Int32) ? types_.Int32 : types_.Int64;
                case Value::Kind::Real: return types_.Single;
                case Value::Kind::Boolean: return types_.Boolean;
                case Value::Kind::String: return types_.String;
                case Value::Kind::TypeRef: return types_.SystemType;
                default: return nullptr;
            }
        }

        std::string Compiler::Describe(const Value& v) const {
            switch (v.kind) {
                case Value::Kind::Slot: return v.type->displayName;
                case Value::Kind::Integer: return v.type ? v.type->displayName : "integer literal";
                case Value::Kind::Real: return "number literal";
                case Value::Kind::Boolean: return "boolean";
                case Value::Kind::String: return "string";
                case Value::Kind::Nil: return "nil";
                case Value::Kind::TypeRef: return std::format("type {}", v.type->displayName);
                case Value::Kind::Namespace: return std::format("namespace {}", v.text);
                case Value::Kind::Function: return std::format("function {}", v.function->name);
                default: return "nothing";
            }
        }

        std::string Compiler::Describe(const Method& m) const {
            std::string params;
            for (const Parameter& p : m.parameters) {
                params += params.empty() ? "" : ", ";
                if (p.byRef) params += "out ";
                params += p.generic ? (p.genericArray ? "{T}" : "T") : p.type->displayName;
            }
            std::string ret = m.returnGeneric ? (m.returnGenericArray ? "{T}" : "T") : m.returnType ? m.returnType->displayName : "void";
            return std::format("{}({}): {}", m.ext->method, params, ret);
        }

        HeapValue Compiler::Literal(const Value& v, const Type* type, const Location& location) const {
            HeapValue h;
            switch (v.kind) {
                case Value::Kind::Nil:
                    h.kind = ValueKind::Null;
                    return h;
                case Value::Kind::Boolean:
                    h.kind = ValueKind::Boolean;
                    h.boolean = v.boolean;
                    return h;
                case Value::Kind::String:
                    if (type && type->kind == TypeKind::Array && type->element && type->element->numeric == Numeric::Char) {
                        h.kind = ValueKind::Array;
                        h.text = type->element->udonName;
                        for (const std::string& c : SplitChars(v.text)) {
                            HeapValue element;
                            element.kind = ValueKind::String;
                            element.text = c;
                            h.arguments.push_back(std::move(element));
                        }
                        return h;
                    }
                    h.kind = ValueKind::String;
                    h.text = v.text;
                    return h;
                case Value::Kind::TypeRef:
                    h.kind = ValueKind::Type;
                    h.text = v.type->udonName;
                    return h;
                case Value::Kind::Integer:
                case Value::Kind::Real: {
                    double real = v.kind == Value::Kind::Integer ? static_cast<double>(v.integer) : v.real;
                    if (type->kind == TypeKind::Enum || v.type) {
                        h.kind = ValueKind::Integer;
                        h.integer = v.integer;
                    } else if (type->numeric == Numeric::Single || type->numeric == Numeric::Double) {
                        h.kind = ValueKind::Real;
                        h.real = real;
                    } else if (IsUnsigned(type->numeric)) {
                        h.kind = ValueKind::Unsigned;
                        h.unsignedInteger = v.kind == Value::Kind::Real ? static_cast<uint64_t>(std::nearbyint(v.real)) : static_cast<uint64_t>(v.integer);
                    } else if (type->IsIntegral()) {
                        h.kind = ValueKind::Integer;
                        h.integer = v.kind == Value::Kind::Real ? static_cast<int64_t>(std::nearbyint(v.real)) : v.integer;
                    } else {
                        Fail(location, std::format("a number cannot be stored as {}", type->displayName));
                    }
                    return h;
                }
                default:
                    Fail(location, std::format("{} is not a constant", Describe(v)));
            }
        }

        uint32_t Compiler::Materialize(const Value& v, const Type* to, const Location& location) {
            const Type* natural = NaturalType(v);
            const Type* target = to ? to : natural;
            if ((v.kind == Value::Kind::Slot && v.type->IsList()) || (target && target->IsList()))
                Fail(location, "a List cannot be copied or passed as a value; use list:ToArray() for an array copy");
            if (v.kind == Value::Kind::Slot) {
                if (!target || v.type == target) return v.slot;
                if (v.type->IsNumeric() && target->IsNumeric() && v.type->numeric != target->numeric) return Convert(v.slot, v.type, target, location);
                return v.slot;
            }
            if (v.kind == Value::Kind::Nil) return emit_.Constant(target && target->IsReference() ? target : types_.Object, Literal(v, types_.Object, location));
            if (!target) Fail(location, std::format("{} has no value", Describe(v)));
            if (!v.IsLiteral() && v.kind != Value::Kind::TypeRef) Fail(location, std::format("{} is not a value", Describe(v)));

            const Type* storage = target;
            bool charText = v.kind == Value::Kind::String &&
                            (target->numeric == Numeric::Char || (target->kind == TypeKind::Array && target->element && target->element->numeric == Numeric::Char));
            bool literalFits = (v.kind == Value::Kind::Integer || v.kind == Value::Kind::Real) ? (target->IsNumeric() || target->kind == TypeKind::Enum)
                             : v.kind == Value::Kind::TypeRef ? target == types_.SystemType
                             : charText || target == natural;
            if (!literalFits) storage = natural;
            return emit_.Constant(storage, Literal(v, storage, location));
        }

        uint32_t Compiler::Convert(uint32_t slot, const Type* from, const Type* to, const Location& location, std::optional<uint32_t> destination) {
            std::string method = "To" + std::string(to->fullName.substr(to->fullName.find_last_of('.') + 1));
            const Type* convert = types_.Get("SystemConvert");
            const Method* chosen = nullptr;
            for (const Method* m : types_.Methods(convert, method, true, false)) {
                if (m->parameters.size() != 1 || m->returnType != to) continue;
                if (m->parameters[0].type == from) {
                    chosen = m;
                    break;
                }
                if (m->parameters[0].type == types_.Object) chosen = m;
            }
            if (!chosen) Fail(location, std::format("cannot convert {} to {}: System.Convert.{}({}) is not exposed", from->displayName, to->displayName, method, from->displayName));
            uint32_t out = destination ? *destination : emit_.Temp(to);
            emit_.Push(slot);
            emit_.Push(out);
            emit_.Extern(emit_.ExternSlot(chosen->ext->signature), true);
            return out;
        }

        void Compiler::Store(const Value& value, const Variable& destination, const Location& location) {
            if (value.kind == Value::Kind::Function || value.kind == Value::Kind::Namespace || value.kind == Value::Kind::None)
                Fail(location, std::format("cannot store {} in a variable", Describe(value)));
            if (destination.type->IsList() || (value.kind == Value::Kind::Slot && value.type->IsList()))
                Fail(location, "a List cannot be copied or assigned; reset it with 'list = {}' or use list:ToArray()");
            if (Cost(value, destination.type) < 0) {
                bool explicitNumeric = ((value.kind == Value::Kind::Slot && value.type->IsNumeric()) || value.IsNumberLiteral()) && destination.type->IsNumeric();
                Fail(location, std::format("cannot assign {} to {}{}", Describe(value), destination.type->displayName,
                    explicitNumeric ? std::format("; convert explicitly with '(value :: {})'", destination.type->displayName) : ""));
            }
            if (value.kind == Value::Kind::Slot) {
                if (value.slot == destination.slot) return;
                if (value.type->IsNumeric() && destination.type->IsNumeric() && value.type->numeric != destination.type->numeric) {
                    Convert(value.slot, value.type, destination.type, location, destination.slot);
                    return;
                }
                if (emit_.IsTemp(value.slot) && emit_.RetargetLastResult(value.slot, destination.slot)) return;
                emit_.Copy(value.slot, destination.slot);
                return;
            }
            emit_.Copy(Materialize(value, destination.type, location), destination.slot);
        }

        const Type* Compiler::ResolveTypeName(std::string_view name, const Location& location) {
            static const std::unordered_map<std::string_view, std::string_view> aliases = {
                { "number", "SystemSingle" }, { "float", "SystemSingle" }, { "double", "SystemDouble" },
                { "int", "SystemInt32" }, { "uint", "SystemUInt32" }, { "long", "SystemInt64" }, { "ulong", "SystemUInt64" },
                { "short", "SystemInt16" }, { "ushort", "SystemUInt16" }, { "byte", "SystemByte" }, { "sbyte", "SystemSByte" },
                { "char", "SystemChar" }, { "boolean", "SystemBoolean" }, { "bool", "SystemBoolean" },
                { "string", "SystemString" }, { "any", "SystemObject" }, { "object", "SystemObject" },
            };
            if (auto it = typeAliases_.find(std::string(name)); it != typeAliases_.end()) return it->second;
            if (auto it = aliases.find(name); it != aliases.end()) return types_.Get(it->second);
            if (const ScriptInfo* script = catalog_.FindScript(name)) return types_.Script(script);
            if (const Type* t = ShortType(name, location)) return t;
            Fail(location, std::format("unknown type '{}'", name));
        }

        const Type* Compiler::ResolveType(AstType* type) {
            if (auto* g = type->as<AstTypeGroup>()) return ResolveType(g->type);
            if (auto* ref = type->as<AstTypeReference>()) {
                if (ref->prefix) {
                    std::string path = std::string(ref->prefix->value) + "." + ref->name.value;
                    if (auto it = ref->prefixLocal ? aliases_.find(ref->prefixLocal) : aliases_.end(); it != aliases_.end()) {
                        const Value& alias = it->second;
                        if (alias.kind != Value::Kind::Namespace && alias.kind != Value::Kind::TypeRef)
                            Fail(ref->location, std::format("'{}' is not a type or namespace", ref->prefix->value));
                        path = alias.kind == Value::Kind::Namespace ? alias.text + "." + ref->name.value : alias.type->fullName + "+" + ref->name.value;
                    }
                    if (const Type* t = types_.FindByFullName(path)) return t;
                    Fail(ref->location, std::format("unknown type '{}'", path));
                }
                if (ref->hasParameterList && ref->name == "List") {
                    if (ref->parameters.size != 1 || !ref->parameters.data[0].type) Fail(ref->location, "List takes one element type: List<int>");
                    const Type* element = ResolveType(ref->parameters.data[0].type);
                    if (element->IsList()) Fail(ref->location, "lists of lists are not supported");
                    return types_.ListOf(element);
                }
                return ResolveTypeName(ref->name.value, ref->location);
            }
            if (auto* table = type->as<AstTypeTable>()) {
                if (table->props.size || !table->indexer) Fail(type->location, "only array types '{T}' are supported");
                return types_.ArrayOf(ResolveType(table->indexer->resultType));
            }
            if (auto* u = type->as<AstTypeUnion>()) {
                AstType* single = nullptr;
                for (AstType* t : u->types) {
                    if (t->is<AstTypeOptional>()) continue;
                    if (auto* r = t->as<AstTypeReference>(); r && r->name == "nil") continue;
                    if (single) Fail(type->location, "union types are not supported");
                    single = t;
                }
                if (!single) Fail(type->location, "unsupported type");
                return ResolveType(single);
            }
            Fail(type->location, "unsupported type annotation");
        }

    } // namespace

    CompileResult Compile(const Catalog& catalog, std::string_view source, const CompileOptions& options) {
        Compiler compiler(catalog, source, options, false);
        return compiler.Run();
    }

    CompileResult ExtractInterface(const Catalog& catalog, std::string_view source, const CompileOptions& options) {
        Compiler compiler(catalog, source, options, true);
        return compiler.Run();
    }

} // namespace UdonLuau
