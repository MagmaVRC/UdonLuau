#include "UdonLuau/Definitions.hpp"

#include "TypeSystem.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace UdonLuau {

    namespace {

        using namespace Detail;

        struct Signature {
            std::vector<std::string> parameters;
            std::string              returns;

            bool operator<(const Signature& o) const { return std::tie(parameters, returns) < std::tie(o.parameters, o.returns); }
        };

        struct Members {
            std::map<std::string, std::string>               properties;
            std::map<std::string, std::set<Signature>>       methods;
            std::map<std::string, std::set<Signature>>       metamethods;
        };

        bool IsKeyword(std::string_view name) {
            static const std::unordered_set<std::string_view> keywords = {
                "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in",
                "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while",
            };
            return keywords.contains(name);
        }

        bool IsIdentifier(std::string_view name) {
            if (name.empty() || IsKeyword(name) || std::isdigit(static_cast<unsigned char>(name[0]))) return false;
            return std::ranges::all_of(name, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
        }

        std::string_view Metamethod(std::string_view op) {
            static const std::unordered_map<std::string_view, std::string_view> map = {
                { "op_Addition", "__add" }, { "op_Subtraction", "__sub" }, { "op_Multiply", "__mul" }, { "op_Multiplication", "__mul" },
                { "op_Division", "__div" }, { "op_Modulus", "__mod" }, { "op_Remainder", "__mod" }, { "op_UnaryNegation", "__unm" },
                { "op_UnaryMinus", "__unm" }, { "op_Equality", "__eq" }, { "op_LessThan", "__lt" }, { "op_LessThanOrEqual", "__le" },
            };
            auto it = map.find(op);
            return it == map.end() ? std::string_view{} : it->second;
        }

        class Generator {
        public:
            explicit Generator(const Catalog& catalog) : catalog_(catalog), types_(catalog) {}

            std::string Run();

        private:
            std::string Name(const Type* t);
            std::string ClassName(const Type* t);
            bool HasShortName(const Type* t);
            bool IsBuiltin(const Type* t) const;
            void Collect();
            void Add(const Type* owner, const Method& m);
            std::string Function(const Signature& s, const std::string& self) const;
            std::string Statics(const Type* t);
            void Declare(const Type* t);

            const Catalog&                                  catalog_;
            TypeTable                                       types_;
            std::unordered_map<const Type*, Members>        instance_;
            std::unordered_map<const Type*, Members>        statics_;
            std::vector<const Type*>                        declared_;
            std::unordered_set<const Type*>                 seen_;
            std::string                                     out_;
        };

        bool Generator::IsBuiltin(const Type* t) const {
            return t->IsNumeric() || t == types_.Boolean || t == types_.String || t == types_.Object || t == types_.Void || t == types_.SystemType;
        }

        bool Generator::HasShortName(const Type* t) {
            if (t->fullName.empty()) return false;
            std::vector<const Type*> matches = types_.FindByShortName(t->displayName);
            if (matches.size() == 1) return matches[0] == t;
            for (const std::string& space : catalog_.PreferredNamespaces()) {
                const Type* found = nullptr;
                int count = 0;
                for (const Type* m : matches) {
                    size_t dot = m->fullName.rfind('.');
                    if ((dot == std::string::npos ? std::string() : m->fullName.substr(0, dot)) == space) {
                        found = m;
                        ++count;
                    }
                }
                if (count == 1) return found == t;
            }
            return false;
        }

        std::string Generator::ClassName(const Type* t) {
            if (t->script) return t->script->name;
            return HasShortName(t) && IsIdentifier(t->displayName) ? t->displayName : t->udonName;
        }

        std::string Generator::Name(const Type* t) {
            if (!t || t == types_.Void) return "()";
            if (t->kind == TypeKind::Array && t->element) return "{" + Name(t->element) + "}";
            switch (t->numeric) {
                case Numeric::Single: return "number";
                case Numeric::Double: return "double";
                case Numeric::Int32: return "int";
                case Numeric::UInt32: return "uint";
                case Numeric::Int64: return "long";
                case Numeric::UInt64: return "ulong";
                case Numeric::Int16: return "short";
                case Numeric::UInt16: return "ushort";
                case Numeric::Byte: return "byte";
                case Numeric::SByte: return "sbyte";
                case Numeric::Char: return "char";
                default: break;
            }
            if (t == types_.Boolean) return "boolean";
            if (t == types_.String) return "string";
            if (t == types_.Object || t == types_.SystemType) return "any";
            if (!t->script && seen_.insert(t).second) declared_.push_back(t);
            return ClassName(t);
        }

        void Generator::Add(const Type* owner, const Method& m) {
            const ExternInfo& e = *m.ext;
            Signature s;
            std::vector<std::string> outs;
            for (const Parameter& p : m.parameters) {
                std::string type = p.generic ? (p.genericArray ? "{any}" : "any") : Name(p.type);
                if (p.byRef) outs.push_back(type);
                else s.parameters.push_back(type);
            }
            if (e.hasTypeOperand) s.parameters.push_back("any");
            std::string ret = m.returnGeneric ? (m.returnGenericArray ? "{any}" : "any") : e.isConstructor ? Name(types_.Get(e.returnType)) : Name(m.returnType);
            std::vector<std::string> results;
            if (ret != "()") results.push_back(ret);
            results.insert(results.end(), outs.begin(), outs.end());
            s.returns = results.empty() ? "()" : results.size() == 1 ? results[0] : "(" + [&] {
                std::string joined;
                for (const std::string& r : results) joined += (joined.empty() ? "" : ", ") + r;
                return joined;
            }() + ")";

            std::string_view method = e.method;
            if (e.isConstructor) {
                statics_[owner].methods["new"].insert(s);
                return;
            }
            if (method.starts_with("op_")) {
                std::string_view meta = Metamethod(method);
                if (meta.empty() || s.parameters.empty() || IsBuiltin(owner) || s.parameters[0] != Name(owner)) return;
                Signature rest = s;
                rest.parameters.erase(rest.parameters.begin());
                instance_[owner].metamethods[std::string(meta)].insert(rest);
                return;
            }
            Members& members = m.isStatic ? statics_[owner] : instance_[owner];
            if (method.starts_with("get_") && s.parameters.empty() && outs.empty()) {
                std::string name(method.substr(4));
                if (IsIdentifier(name)) members.properties[name] = ret;
                return;
            }
            if (method.starts_with("set_") && s.parameters.size() == 1) {
                std::string name(method.substr(4));
                if (IsIdentifier(name) && !members.properties.contains(name)) members.properties[name] = s.parameters[0];
                return;
            }
            if (IsIdentifier(method)) members.methods[std::string(method)].insert(s);
        }

        void Generator::Collect() {
            for (const ExternInfo* e : catalog_.Externs()) {
                const Method& m = types_.Describe(*e);
                if (!m.supported) continue;
                Add(types_.Get(e->module), m);
            }
            for (const TypeInfo* info : catalog_.Types()) Name(types_.Get(info->udonName));
        }

        std::string Generator::Function(const Signature& s, const std::string& self) const {
            std::string params = self.empty() ? std::string() : "self: " + self;
            for (const std::string& p : s.parameters) params += (params.empty() ? "" : ", ") + p;
            return std::format("({}) -> {}", params, s.returns);
        }

        std::string Generator::Statics(const Type* t) {
            std::string body;
            if (t->kind == TypeKind::Enum && t->info)
                for (const EnumMember& m : t->info->enumMembers)
                    if (IsIdentifier(m.name)) body += std::format("    {}: {},\n", m.name, ClassName(t));
            auto it = statics_.find(t);
            if (it != statics_.end()) {
                for (const auto& [name, type] : it->second.properties) body += std::format("    {}: {},\n", name, type);
                for (const auto& [name, overloads] : it->second.methods) {
                    std::string joined;
                    for (const Signature& s : overloads) joined += (joined.empty() ? "" : " & ") + std::string("(") + Function(s, "") + ")";
                    body += std::format("    {}: {},\n", name, overloads.size() == 1 ? Function(*overloads.begin(), "") : joined);
                }
            }
            return "{\n" + body + "}";
        }

        void Generator::Declare(const Type* t) {
            std::string name = ClassName(t);
            std::string header = std::format("declare extern type {}", name);
            if (t->base && t->base != types_.Object && !IsBuiltin(t->base) && t->base->kind != TypeKind::Array) header += " extends " + ClassName(t->base);
            out_ += header + " with\n";

            std::set<std::string> written;
            std::function<void(const Type*)> members = [&](const Type* owner) {
                auto it = instance_.find(owner);
                if (it != instance_.end()) {
                    for (const auto& [prop, type] : it->second.properties)
                        if (written.insert(prop).second) out_ += std::format("    {}: {}\n", prop, type);
                    for (const auto& [method, overloads] : it->second.methods) {
                        if (!written.insert(method).second) continue;
                        if (overloads.size() == 1) {
                            const Signature& s = *overloads.begin();
                            std::string params = "self";
                            for (size_t i = 0; i < s.parameters.size(); ++i) params += std::format(", p{}: {}", i + 1, s.parameters[i]);
                            out_ += std::format("    function {}({}): {}\n", method, params, s.returns);
                        } else {
                            std::string joined;
                            for (const Signature& s : overloads) joined += (joined.empty() ? "" : " & ") + std::string("(") + Function(s, name) + ")";
                            out_ += std::format("    {}: {}\n", method, joined);
                        }
                    }
                    for (const auto& [meta, overloads] : it->second.metamethods) {
                        if (!written.insert(meta).second) continue;
                        const Signature& s = *overloads.begin();
                        std::string params = "self";
                        for (size_t i = 0; i < s.parameters.size(); ++i) params += std::format(", p{}: {}", i + 1, s.parameters[i]);
                        out_ += std::format("    function {}({}): {}\n", meta, params, s.returns);
                    }
                }
                for (const Type* i : owner->interfaces) members(i);
            };
            members(t);
            out_ += "end\n\n";
        }

        std::string Generator::Run() {
            out_ += "--!nocheck\n\n";
            for (std::string_view alias : { "int", "uint", "long", "ulong", "short", "ushort", "byte", "sbyte", "char", "float", "double" })
                out_ += std::format("export type {} = number\n", alias);
            out_ += "export type object = any\n\n";
            out_ += "export type List<T> = {\n"
                    "    Count: number,\n"
                    "    Capacity: number,\n"
                    "    Add: (self: List<T>, item: T) -> (),\n"
                    "    Insert: (self: List<T>, index: number, item: T) -> (),\n"
                    "    Remove: (self: List<T>, item: T) -> boolean,\n"
                    "    RemoveAt: (self: List<T>, index: number) -> (),\n"
                    "    IndexOf: (self: List<T>, item: T) -> number,\n"
                    "    Contains: (self: List<T>, item: T) -> boolean,\n"
                    "    Clear: (self: List<T>) -> (),\n"
                    "    ToArray: (self: List<T>) -> { T },\n"
                    "    Sort: (self: List<T>) -> (),\n"
                    "    Reverse: (self: List<T>) -> (),\n"
                    "    Get: (self: List<T>, index: number) -> T,\n"
                    "    Set: (self: List<T>, index: number, item: T) -> (),\n"
                    "    [number]: T,\n"
                    "}\n"
                    "declare List: { new: <T>(capacity: number?) -> List<T> }\n\n";

            Collect();
            const Type* behaviour = types_.Behaviour();
            Name(behaviour);
            Name(types_.Get("UnityEngineGameObject"));
            Name(types_.Get("UnityEngineTransform"));

            std::vector<const Type*> classes;
            for (size_t i = 0; i < declared_.size(); ++i) {
                const Type* t = declared_[i];
                if (t->base) Name(t->base);
                for (const Type* iface : t->interfaces) Name(iface);
            }
            for (const Type* t : declared_)
                if (!IsBuiltin(t) && t->kind != TypeKind::Array) classes.push_back(t);

            std::unordered_map<const Type*, int> depth;
            std::function<int(const Type*)> depthOf = [&](const Type* t) -> int {
                if (!t || IsBuiltin(t)) return 0;
                if (auto it = depth.find(t); it != depth.end()) return it->second;
                depth[t] = 0;
                return depth[t] = 1 + depthOf(t->base);
            };
            std::ranges::stable_sort(classes, [&](const Type* a, const Type* b) {
                int da = depthOf(a), db = depthOf(b);
                return da != db ? da < db : ClassName(a) < ClassName(b);
            });

            std::vector<const ScriptInfo*> scripts;
            std::set<std::string> scriptNames;
            for (const ScriptInfo* script : catalog_.Scripts()) {
                if (!IsIdentifier(script->name) || script->name == ClassName(behaviour)) continue;
                scripts.push_back(script);
                scriptNames.insert(script->name);
            }
            std::ranges::sort(scripts, {}, &ScriptInfo::name);

            std::set<std::string> names;
            for (const Type* t : classes)
                if (!scriptNames.contains(ClassName(t)) && names.insert(ClassName(t)).second) Declare(t);

            for (const ScriptInfo* script : scripts) {
                out_ += std::format("declare extern type {} extends {} with\n", script->name, ClassName(behaviour));
                auto variableType = [&](const ScriptVariable& v) {
                    if (v.script.empty()) return Name(types_.Get(v.type));
                    std::string target = scriptNames.contains(v.script) ? v.script : ClassName(behaviour);
                    return v.type.ends_with("Array") ? "{" + target + "}" : target;
                };
                for (const ScriptVariable& f : script->fields)
                    if (IsIdentifier(f.name)) out_ += std::format("    {}: {}\n", f.name, variableType(f));
                for (const ScriptMethod& m : script->methods) {
                    if (!IsIdentifier(m.name)) continue;
                    std::string params = "self";
                    for (const ScriptVariable& p : m.parameters) params += std::format(", {}: {}", IsIdentifier(p.name) ? p.name : "value", variableType(p));
                    std::string returns = "()";
                    if (m.returns.size() == 1) returns = variableType(m.returns[0]);
                    if (m.returns.size() > 1) {
                        returns = "(";
                        for (size_t i = 0; i < m.returns.size(); ++i) returns += (i ? ", " : "") + variableType(m.returns[i]);
                        returns += ")";
                    }
                    out_ += std::format("    function {}({}): {}\n", m.name, params, returns);
                }
                out_ += "end\n\n";
            }

            for (const Type* t : classes)
                if (HasShortName(t) && IsIdentifier(t->displayName) && !scriptNames.contains(t->displayName))
                    out_ += std::format("declare {}: {}\n\n", t->displayName, Statics(t));
            for (const ScriptInfo* script : scripts) out_ += std::format("declare {}: {{}}\n", script->name);

            std::map<std::string, std::vector<const Type*>> qualified;
            for (const Type* t : classes) {
                if (HasShortName(t) || t->fullName.empty()) continue;
                size_t dot = t->fullName.find('.');
                if (dot == std::string::npos) continue;
                qualified[t->fullName.substr(0, dot)].push_back(t);
            }
            for (const auto& [root, members] : qualified) {
                std::function<std::string(const std::string&, int)> table = [&](const std::string& prefix, int indent) {
                    std::map<std::string, std::string> entries;
                    for (const Type* t : members) {
                        if (!t->fullName.starts_with(prefix + ".")) continue;
                        std::string rest = t->fullName.substr(prefix.size() + 1);
                        size_t dot = rest.find('.');
                        std::string key = dot == std::string::npos ? rest : rest.substr(0, dot);
                        if (!IsIdentifier(key) || entries.contains(key)) continue;
                        entries[key] = dot == std::string::npos ? Statics(t) : table(prefix + "." + key, indent + 1);
                    }
                    std::string body = "{\n";
                    for (const auto& [key, value] : entries) body += std::string(static_cast<size_t>(indent + 1) * 4, ' ') + key + ": " + value + ",\n";
                    return body + std::string(static_cast<size_t>(indent) * 4, ' ') + "}";
                };
                if (IsIdentifier(root) && !names.contains(root)) out_ += std::format("declare {}: {}\n\n", root, table(root, 0));
            }

            out_ += std::format("declare this: {}\n", ClassName(behaviour));
            out_ += std::format("declare gameObject: {}\n", ClassName(types_.Get("UnityEngineGameObject")));
            out_ += std::format("declare transform: {}\n", ClassName(types_.Get("UnityEngineTransform")));

            std::vector<const Type*> targets = types_.FindByShortName("NetworkEventTarget");
            if (!targets.empty() && targets[0]->info) {
                std::string body;
                for (const EnumMember& m : targets[0]->info->enumMembers)
                    if (IsIdentifier(m.name)) body += std::format("    {}: (behaviour: {}) -> any,\n", m.name, ClassName(behaviour));
                out_ += std::format("declare Network: {{\n{}}}\n", body);
            }
            return std::move(out_);
        }

    } // namespace

    std::string GenerateDefinitions(const Catalog& catalog) {
        Generator generator(catalog);
        return generator.Run();
    }

} // namespace UdonLuau
