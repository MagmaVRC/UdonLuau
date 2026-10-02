#include "UdonLuau/Compiler.hpp"

#include "Emitter.hpp"
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
            bool                     networkCallable = false;
            int                      maxEventsPerSecond = 0;
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
                onCall(c->func);
                return true;
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

        struct Value {
            enum class Kind { None, Slot, Integer, Real, Boolean, String, Nil, TypeRef, Namespace, Function, Network };

            Kind        kind = Kind::None;
            const Type* type = nullptr;
            uint32_t    slot = 0;
            int64_t     integer = 0;
            double      real = 0.0;
            bool        boolean = false;
            std::string text;
            Function*   function = nullptr;
            const Type* targetType = nullptr;

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
            void DeclareFields(AstStatLocal* stat);
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

            const Catalog&                                 catalog_;
            TypeTable                                      types_;
            Emitter                                        emit_;
            std::string_view                               source_;
            const CompileOptions&                          options_;
            std::unordered_map<std::string, Value>         defines_;
            std::unordered_set<AstLocal*>                  assigned_;
            std::unordered_set<AstLocal*>                  memberAssigned_;
            bool Immutable(AstLocal* local, const Type* type) const {
                return !assigned_.contains(local) && (!memberAssigned_.contains(local) || (type && type->IsReference()));
            }
            std::unordered_set<uint32_t>                   fieldSlots_;
            std::vector<InlineFrame>                       inlineStack_;
            std::vector<std::pair<std::string, const Type*>> exportedFields_;
            bool                                           interfaceOnly_ = false;
            BehaviourSyncMode                              syncMode_ = BehaviourSyncMode::Any;
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
            uint32_t                                       haltSlot_ = 0;
            uint32_t                                       returnJumpSlot_ = 0;
        };

        CompileResult Compiler::Run() {
            FFlag::LuauExportValueSyntax.value = true;
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

            DeclareModule(parsed.root);
            bool declared = std::ranges::none_of(diagnostics_, [](const Diagnostic& d) { return d.severity == Severity::Error; });
            if (declared) result.scriptInterface = BuildInterface();
            if (interfaceOnly_) {
                result.diagnostics = std::move(diagnostics_);
                return result;
            }
            Analyze(parsed.root);
            for (auto& f : functions_) {
                try {
                    CompileFunction(*f);
                } catch (const CompileError& e) {
                    Report(e.location, e.message);
                }
            }
            CheckRecursion();

            bool failed = std::ranges::any_of(diagnostics_, [](const Diagnostic& d) { return d.severity == Severity::Error; });
            if (!failed) {
                result.program = emit_.Finish(std::move(entries_), std::move(sync_));
                result.program->attributes = ModuleAttributes(parsed.root);
                result.program->syncMode = syncMode_;
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

            std::vector<FieldAttribute> out;
            for (const auto& [line, entries] : commentAttributes_) {
                if (line >= attached) break;
                for (const auto& entry : entries) out.push_back(entry.first);
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

        void Compiler::Analyze(AstStatBlock* root) {
            auto record = [&](AstExpr* func) {
                while (auto* g = func->as<AstExprGroup>()) func = g->expr;
                Function* f = nullptr;
                if (auto* l = func->as<AstExprLocal>()) {
                    if (auto it = localFunctions_.find(l->local); it != localFunctions_.end()) f = it->second;
                } else if (auto* g = func->as<AstExprGlobal>()) {
                    if (auto it = globalFunctions_.find(g->name.value); it != globalFunctions_.end()) f = it->second;
                }
                if (f) ++f->calls;
            };
            Analysis analysis(assigned_, memberAssigned_, record);
            root->visit(&analysis);

            for (auto& f : functions_) {
                NodeCounter counter;
                f->node->body->visit(&counter);
                f->size = counter.count;
                for (const FieldAttribute& a : AnnotationsFor(f->location).attributes) {
                    if (a.name == "inline") f->forceInline = true;
                    if (a.name == "noinline") f->noInline = true;
                }
                if (f->forceInline && f->noInline) Report(f->location, std::format("'{}' cannot be both @inline and @noinline", f->name));
                f->inlined = !f->noInline && f->calls > 0 && (f->forceInline || f->size <= kInlineBudget || (f->calls == 1 && !f->exported));
                f->trampoline = !f->exported || (f->calls > 0 && !f->inlined);
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

        void Compiler::DeclareFields(AstStatLocal* stat) {
            Annotations annotations = AnnotationsFor(stat->location);
            for (size_t i = 0; i < stat->vars.size; ++i) {
                AstLocal* var = stat->vars.data[i];
                std::string name = var->name.value;
                CheckUserName(name, var->location);
                if (!userSymbols_.insert(name).second) Fail(var->location, std::format("'{}' is already declared", name));

                const Type* type = var->annotation ? ResolveType(var->annotation) : nullptr;
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
                        uint32_t slot = emit_.AddSlot(name, type, initial, stat->isExported);
                        fieldSlots_.insert(slot);
                        if (stat->isExported) exportedFields_.emplace_back(name, type);
                        emit_.Slot(slot).attributes = annotations.attributes;
                        variables_[var] = { slot, type };
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
                uint32_t slot = emit_.AddSlot(name, type, initial, stat->isExported);
                fieldSlots_.insert(slot);
                if (stat->isExported) exportedFields_.emplace_back(name, type);
                emit_.Slot(slot).attributes = annotations.attributes;
                variables_[var] = { slot, type };
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

            if (!local || local->isExported) {
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
            if (f->exported) {
                f->entryName = f->event ? "_" + LowerFirst(name) : f->networkCallable ? name : "_" + name;
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
                    slot = f->exported ? emit_.AddSlot(std::format("__{}_{}__param", name, arg->name.value), type) : emit_.Local(arg->name.value, type);
                }
                variables_[arg] = { slot, type };
                f->parameters.push_back({ slot, type });
            }

            if (auto* pack = node->returnAnnotation ? node->returnAnnotation->as<AstTypePackExplicit>() : nullptr) {
                if (pack->typeList.tailType) Fail(pack->location, "variadic returns are not supported");
                for (AstType* t : pack->typeList.types) {
                    const Type* type = ResolveType(t);
                    std::string symbol = f->returns.empty() ? std::format("__{}__ret", name) : std::format("__{}__ret{}", name, f->returns.size());
                    f->returns.push_back(type);
                    f->returnSlots.push_back(f->exported ? emit_.AddSlot(symbol, type) : emit_.Hidden(type));
                }
            } else if (node->returnAnnotation) {
                Fail(node->returnAnnotation->location, "unsupported return annotation");
            }
            if (f->event && !f->returns.empty()) Fail(location, "events cannot return values");

            Function* raw = f.get();
            functions_.push_back(std::move(f));
            if (local) localFunctions_[local] = raw;
            else globalFunctions_[name] = raw;
        }

        void Compiler::CompileFunction(Function& f) {
            if (!f.exported && (f.inlined || f.calls == 0)) return;
            current_ = &f;
            for (size_t i = 0; i < f.parameters.size(); ++i) {
                AstLocal* arg = f.node->args.data[i];
                aliases_.erase(arg);
                variables_[arg] = f.parameters[i];
            }
            if (f.exported) {
                emit_.Bind(f.exportedEntry);
                entries_.push_back({ f.entryName, *emit_.AddressOf(f.exportedEntry) });
                if (f.trampoline) emit_.Push(haltSlot_);
            }
            emit_.Bind(f.entry);
            AstStat* tail = nullptr;
            CompileBody(f.node->body, tail);
            emit_.Bind(f.epilogue);
            if (f.trampoline) {
                emit_.Push(returnJumpSlot_);
                emit_.CopyFromStack();
                emit_.JumpIndirect(returnJumpSlot_);
            } else {
                emit_.JumpTo(0xFFFFFFFFu);
            }
            current_ = nullptr;
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
            std::vector<const Type*> expected;
            for (AstLocal* var : stat->vars) expected.push_back(var->annotation ? ResolveType(var->annotation) : nullptr);

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
            if (hasValue) Store(value, v, var->location);
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
            if (value.kind != Value::Kind::Slot || emit_.IsTemp(value.slot)) return value;
            uint32_t t = emit_.Temp(value.type);
            emit_.Copy(value.slot, t);
            return Value::OfSlot(t, value.type);
        }

        void Compiler::CompileAssign(AstStatAssign* stat) {
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
                if (aliases_.contains(local->local)) Fail(target->location, std::format("'{}' is a constant", local->local->name.value));
                Store(value, LookupLocal(local->local, local->location), target->location);
                return;
            }
            if (auto* index = target->as<AstExprIndexName>()) {
                Value owner = CompileExpr(index->expr);
                std::string setter = std::string("set_") + index->index.value;
                if (owner.kind == Value::Kind::TypeRef) {
                    CallStatic(owner.type, setter, { value }, nullptr, target->location);
                    return;
                }
                if (owner.kind != Value::Kind::Slot) Fail(target->location, "cannot assign to this expression");
                if (const ScriptInfo* script = owner.type->script) {
                    auto field = std::ranges::find(script->fields, std::string_view(index->index.value), &ScriptVariable::name);
                    if (field != script->fields.end()) {
                        const Type* type = VariableType(*field);
                        if (Cost(value, type) < 0) Fail(target->location, std::format("cannot assign {} to {}.{} ({})", Describe(value), script->name, field->name, type->displayName));
                        Value v = value.IsLiteral() ? Value::OfSlot(Materialize(value, type, target->location), type) : value;
                        if (v.kind == Value::Kind::Slot && v.type != type && v.type->IsNumeric() && type->IsNumeric())
                            v = Value::OfSlot(Convert(v.slot, v.type, type, target->location), type);
                        CallMember(Value::OfSlot(owner.slot, types_.Behaviour()), "SetProgramVariable", { Value::OfString(field->symbol), v }, nullptr, target->location);
                        return;
                    }
                }
                if (owner.type->IsValueType() && emit_.IsConstant(owner.slot))
                    Fail(target->location, std::format("cannot modify '{}' of a constant {}", index->index.value, owner.type->displayName));
                if (owner.type->IsValueType() && emit_.IsTemp(owner.slot))
                    Fail(target->location, std::format("cannot modify '{}' of a {} copy; store it in a local, change it, then assign it back", index->index.value, owner.type->displayName));
                CallMember(owner, setter, { value }, nullptr, target->location);
                return;
            }
            if (auto* index = target->as<AstExprIndexExpr>()) {
                Value owner = CompileExpr(index->expr);
                if (owner.kind != Value::Kind::Slot) Fail(target->location, "cannot index this expression");
                Value key = CompileExpr(index->index);
                std::string_view method = owner.type->kind == TypeKind::Array ? "Set" : "set_Item";
                CallMember(owner, method, { key, value }, nullptr, target->location);
                return;
            }
            if (auto* global = target->as<AstExprGlobal>())
                Fail(target->location, std::format("unknown variable '{}'; declare it with 'local'", global->name.value));
            Fail(target->location, "cannot assign to this expression");
        }

        void Compiler::CompileCompoundAssign(AstStatCompoundAssign* stat) {
            AstExprBinary synthetic(stat->location, stat->op, stat->var, stat->value);
            const Type* hint = nullptr;
            if (auto* local = Unwrap(stat->var)->as<AstExprLocal>()) hint = LookupLocal(local->local, local->location).type;
            AssignTo(stat->var, CompileBinary(&synthetic, hint));
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

            bool knownBounds = from.IsNumberLiteral() && to.IsNumberLiteral();
            if (knownBounds) {
                double a = from.kind == Value::Kind::Integer ? static_cast<double>(from.integer) : from.real;
                double b = to.kind == Value::Kind::Integer ? static_cast<double>(to.integer) : to.real;
                if (stepValue > 0 ? a > b : a < b) return;
            }

            Variable counter{ emit_.Local(stat->var->name.value, type), type };
            variables_[stat->var] = counter;
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
            CompileLoopBody(stat->body, { end, increment });
            emit_.Bind(increment);
            Value sum = CompileArithmetic(AstExprBinary::Add, Value::OfSlot(counter.slot, type), Value::OfSlot(stepSlot, type), stat->location);
            Store(sum, counter, stat->location);
            emit_.Bind(next);
            Value done = CompareValues(stepValue > 0 ? AstExprBinary::CompareGt : AstExprBinary::CompareLt,
                Value::OfSlot(counter.slot, type), Value::OfSlot(limit.slot, type), stat->location);
            emit_.JumpIfFalse(done.slot, top);
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
            if (collection.kind != Value::Kind::Slot || collection.type->kind != TypeKind::Array)
                Fail(source->location, std::format("cannot iterate {}", Describe(collection)));

            const Type* arrayType = collection.type;
            Variable array{ emit_.Hidden(arrayType), arrayType };
            Store(collection, array, source->location);
            Variable index{ emit_.Hidden(types_.Int32), types_.Int32 };
            Store(Value::OfInteger(0), index, stat->location);
            Variable length{ emit_.Hidden(types_.Int32), types_.Int32 };
            std::vector<Value> len = CallMember(Value::OfSlot(array.slot, arrayType), "get_Length", {}, nullptr, source->location);
            Store(len.at(0), length, source->location);

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
                if (item.type->script) got.at(0).type = item.type;
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
            InlineFrame* frame = inlineStack_.empty() ? nullptr : &inlineStack_.back();
            Function& f = frame ? *frame->function : *current_;
            if (stat->list.size && f.returns.empty()) {
                if (f.event) Fail(stat->location, "events cannot return values");
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

            if (auto it = typeAliases_.find(std::string(name)); it != typeAliases_.end()) return Value::OfType(it->second);
            if (const ScriptInfo* script = catalog_.FindScript(name)) return Value::OfType(types_.Script(script));
            if (types_.IsNamespace(name) && types_.FindByShortName(name).empty()) return Value::OfNamespace(std::string(name));
            if (const Type* t = ShortType(name, expr->location)) return Value::OfType(t);
            if (types_.IsNamespace(name)) return Value::OfNamespace(std::string(name));
            Fail(expr->location, std::format("unknown name '{}'", name));
        }

        Value Compiler::CompileIndexName(AstExprIndexName* expr) {
            Value owner = CompileExpr(expr->expr);
            std::string_view name = expr->index.value;

            if (owner.kind == Value::Kind::Namespace) {
                std::string path = owner.text + "." + std::string(name);
                if (const Type* t = types_.FindByFullName(path)) return Value::OfType(t);
                if (types_.IsNamespace(path)) return Value::OfNamespace(path);
                Fail(expr->location, std::format("'{}' does not exist", path));
            }

            if (owner.kind == Value::Kind::TypeRef) {
                const Type* t = owner.type;
                if (t->kind == TypeKind::Enum && t->info) {
                    for (const EnumMember& m : t->info->enumMembers)
                        if (m.name == name) return Value::OfInteger(m.value, t);
                }
                if (!t->fullName.empty())
                    if (const Type* nested = types_.FindByFullName(t->fullName + "+" + std::string(name))) return Value::OfType(nested);
                return CallStatic(t, "get_" + std::string(name), {}, nullptr, expr->location).at(0);
            }

            if (owner.kind == Value::Kind::Function) Fail(expr->location, "functions have no members");
            if (owner.kind == Value::Kind::Network) Fail(expr->location, "use ':' to call a method through Network");
            if (owner.kind == Value::Kind::Slot && owner.type->script) {
                const ScriptInfo* script = owner.type->script;
                auto field = std::ranges::find(script->fields, name, &ScriptVariable::name);
                if (field != script->fields.end()) return ReadScriptField(owner, *field, expr->location);
                if (std::ranges::find(script->methods, name, &ScriptMethod::name) != script->methods.end())
                    Fail(expr->location, std::format("'{}' is a method; call it with '{}:{}()'", name, "value", name));
            }
            Value receiver = owner.kind == Value::Kind::Slot ? owner : Value::OfSlot(Materialize(owner, nullptr, expr->location), NaturalType(owner));
            return CallMember(receiver, "get_" + std::string(name), {}, nullptr, expr->location).at(0);
        }

        Value Compiler::CompileIndexExpr(AstExprIndexExpr* expr) {
            Value owner = CompileExpr(expr->expr);
            if (owner.kind != Value::Kind::Slot) Fail(expr->location, std::format("cannot index {}", Describe(owner)));
            Value key = CompileExpr(expr->index);
            std::string_view method = owner.type->kind == TypeKind::Array ? "Get" : "get_Item";
            Value item = CallMember(owner, method, { key }, nullptr, expr->location).at(0);
            if (owner.type->kind == TypeKind::Array && owner.type->element && owner.type->element->script) item.type = owner.type->element;
            return item;
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
                    return ints ? Value::OfInteger(static_cast<int64_t>(std::floor(a / b))) : Value::OfReal(std::floor(a / b));
                case AstExprBinary::Mod:
                    if (b == 0) return std::nullopt;
                    return ints ? Value::OfInteger(l.integer - static_cast<int64_t>(std::floor(a / b)) * r.integer) : Value::OfReal(a - std::floor(a / b) * b);
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
            for (AstExpr* e : operands) parts.push_back(CompileExpr(e));
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

            std::vector<Value> ctorArgs{ Value::OfInteger(static_cast<int64_t>(expr->items.size)) };
            Value array = CallStatic(expected, "ctor", ctorArgs, nullptr, expr->location).at(0);
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
                    Value test = CompileComparison(negate ? NegateComparison(b->op) : b->op, b->left, b->right, b->location);
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
            if (negate) slot = NotSlot(slot, expr->location);
            emit_.JumpIfFalse(slot, whenFalse);
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

            std::vector<Value> args;
            for (AstExpr* a : call->args) args.push_back(CompileExpr(a));

            if (call->self) {
                auto* index = func->as<AstExprIndexName>();
                Value receiver = CompileExpr(index->expr);
                std::string_view name = index->index.value;
                if (receiver.kind == Value::Kind::Network) {
                    std::string entry(name);
                    std::vector<Value> sent{ Value::OfInteger(receiver.integer, receiver.targetType), Value::OfString(entry) };
                    if (const ScriptInfo* script = receiver.type->script) {
                        auto method = std::ranges::find(script->methods, name, &ScriptMethod::name);
                        if (method == script->methods.end()) Fail(call->location, std::format("{} has no public method '{}'", script->name, name));
                        if (!method->networkCallable)
                            Fail(call->location, std::format("{}.{} is not network callable; mark it with '-- @networkcallable'", script->name, name));
                        if (args.size() != method->parameters.size())
                            Fail(call->location, std::format("'{}' takes {} argument(s), got {}", name, method->parameters.size(), args.size()));
                        sent[1] = Value::OfString(method->entryPoint);
                        for (size_t i = 0; i < args.size(); ++i) {
                            const Type* type = VariableType(method->parameters[i]);
                            if (Cost(args[i], type) < 0)
                                Fail(call->location, std::format("argument {} of '{}' must be {}, got {}", i + 1, name, type->displayName, Describe(args[i])));
                            Value arg = args[i].IsLiteral() ? Value::OfSlot(Materialize(args[i], type, call->location), type) : args[i];
                            if (arg.kind == Value::Kind::Slot && arg.type != type && arg.type->IsNumeric() && type->IsNumeric())
                                arg = Value::OfSlot(Convert(arg.slot, arg.type, type, call->location), type);
                            sent.push_back(arg);
                        }
                    } else {
                        for (const Value& a : args) sent.push_back(a);
                    }
                    if (sent.size() > 10) Fail(call->location, "networked calls take at most 8 arguments");
                    Value self = Value::OfSlot(receiver.slot, types_.Behaviour());
                    return CallMember(self, "SendCustomNetworkEvent", sent, nullptr, call->location);
                }
                if (receiver.kind == Value::Kind::Slot && receiver.type->script) {
                    const ScriptInfo* script = receiver.type->script;
                    auto method = std::ranges::find(script->methods, name, &ScriptMethod::name);
                    if (method != script->methods.end()) return CallScriptMethod(receiver, *method, args, want, call->location);
                }
                if (receiver.kind == Value::Kind::TypeRef || receiver.kind == Value::Kind::Namespace)
                    Fail(call->location, "use '.' to call static methods");
                if (receiver.kind != Value::Kind::Slot) receiver = Value::OfSlot(Materialize(receiver, nullptr, call->location), NaturalType(receiver));
                return CallMember(receiver, index->index.value, std::move(args), typeArg, call->location);
            }

            if (auto* index = func->as<AstExprIndexName>()) {
                Value owner = CompileExpr(index->expr);
                if (owner.kind == Value::Kind::TypeRef) {
                    std::string_view name = index->index.value;
                    return CallStatic(owner.type, name == "new" ? "ctor" : name, std::move(args), typeArg, call->location);
                }
                if (owner.kind == Value::Kind::Slot)
                    Fail(call->location, std::format("use ':' to call methods on values: '{}:{}(...)'", "value", index->index.value));
                Fail(call->location, "this expression cannot be called");
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

                bool callerOwned = a.kind != Value::Kind::Slot || !fieldSlots_.contains(a.slot);
                if (Immutable(param, type) && callerOwned) {
                    if (a.IsLiteral()) {
                        aliases_[param] = CoerceLiteral(a, type, location);
                        continue;
                    }
                    if (a.kind == Value::Kind::Slot) {
                        aliases_[param] = Value::OfSlot(Materialize(a, type, location), type);
                        continue;
                    }
                }
                if (a.kind == Value::Kind::Slot && a.type == type && emit_.IsTemp(a.slot)) {
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
            if (f->inlined) return Inline(f, std::move(args), want, location, into);
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
            auto methods = types_.Methods(receiver.type, name, false, true);
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

        std::vector<Value> Compiler::CallStatic(const Type* owner, std::string_view name, std::vector<Value> args, const Type* typeArg, const Location& location) {
            auto methods = types_.Methods(owner, name, true, name != "ctor");
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
            std::vector<uint32_t> inputs;
            size_t argIndex = 0;
            for (const Parameter& p : m.parameters) {
                if (p.byRef) continue;
                inputs.push_back(Materialize(args[argIndex++], InputType(p, r.typeArgument), location));
            }

            std::vector<uint32_t> pushes;
            std::vector<Value> refResults;
            if (receiver) pushes.push_back(receiver->slot);
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
            if (returnType) {
                uint32_t t = emit_.Temp(returnType);
                pushes.push_back(t);
                results.push_back(Value::OfSlot(t, returnType));
            }

            for (uint32_t s : pushes) emit_.Push(s);
            emit_.Extern(emit_.ExternSlot(m.ext->signature), returnType != nullptr);
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
                        h.unsignedInteger = static_cast<uint64_t>(v.integer);
                    } else if (type->IsIntegral()) {
                        h.kind = ValueKind::Integer;
                        h.integer = v.integer;
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
            if (v.kind == Value::Kind::Slot) {
                if (!target || v.type == target) return v.slot;
                if (v.type->IsNumeric() && target->IsNumeric() && v.type->numeric != target->numeric) return Convert(v.slot, v.type, target, location);
                return v.slot;
            }
            if (v.kind == Value::Kind::Nil) return emit_.Constant(target && target->IsReference() ? target : types_.Object, Literal(v, types_.Object, location));
            if (!target) Fail(location, std::format("{} has no value", Describe(v)));
            if (!v.IsLiteral() && v.kind != Value::Kind::TypeRef) Fail(location, std::format("{} is not a value", Describe(v)));

            const Type* storage = target;
            bool literalFits = (v.kind == Value::Kind::Integer || v.kind == Value::Kind::Real) ? (target->IsNumeric() || target->kind == TypeKind::Enum)
                             : v.kind == Value::Kind::TypeRef ? target == types_.SystemType
                             : target == natural;
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
                        path = alias.kind == Value::Kind::Namespace ? alias.text + "." + ref->name.value : alias.type->fullName + "+" + ref->name.value;
                    }
                    if (const Type* t = types_.FindByFullName(path)) return t;
                    Fail(ref->location, std::format("unknown type '{}'", path));
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
