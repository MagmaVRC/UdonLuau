#include "TypeSystem.hpp"

#include <algorithm>
#include <array>
#include <queue>
#include <unordered_set>

namespace UdonLuau::Detail {

    namespace {

        struct Builtin {
            std::string_view udonName;
            std::string_view fullName;
            TypeKind         kind;
            Numeric          numeric;
        };

        constexpr std::array kBuiltins = {
            Builtin{ "SystemObject", "System.Object", TypeKind::Class, Numeric::None },
            Builtin{ "SystemVoid", "System.Void", TypeKind::Struct, Numeric::None },
            Builtin{ "SystemBoolean", "System.Boolean", TypeKind::Struct, Numeric::None },
            Builtin{ "SystemSByte", "System.SByte", TypeKind::Struct, Numeric::SByte },
            Builtin{ "SystemByte", "System.Byte", TypeKind::Struct, Numeric::Byte },
            Builtin{ "SystemInt16", "System.Int16", TypeKind::Struct, Numeric::Int16 },
            Builtin{ "SystemUInt16", "System.UInt16", TypeKind::Struct, Numeric::UInt16 },
            Builtin{ "SystemChar", "System.Char", TypeKind::Struct, Numeric::Char },
            Builtin{ "SystemInt32", "System.Int32", TypeKind::Struct, Numeric::Int32 },
            Builtin{ "SystemUInt32", "System.UInt32", TypeKind::Struct, Numeric::UInt32 },
            Builtin{ "SystemInt64", "System.Int64", TypeKind::Struct, Numeric::Int64 },
            Builtin{ "SystemUInt64", "System.UInt64", TypeKind::Struct, Numeric::UInt64 },
            Builtin{ "SystemSingle", "System.Single", TypeKind::Struct, Numeric::Single },
            Builtin{ "SystemDouble", "System.Double", TypeKind::Struct, Numeric::Double },
            Builtin{ "SystemString", "System.String", TypeKind::Class, Numeric::None },
            Builtin{ "SystemType", "System.Type", TypeKind::Class, Numeric::None },
            Builtin{ "SystemArray", "System.Array", TypeKind::Class, Numeric::None },
        };

        const Builtin* FindBuiltin(std::string_view udonName) {
            for (const Builtin& b : kBuiltins)
                if (b.udonName == udonName) return &b;
            return nullptr;
        }

        bool IsGenericMarker(std::string_view name) {
            return name == "T" || name == "TArray" || name == "ListT";
        }

        std::string_view TrailingName(std::string_view fullName) {
            size_t cut = fullName.find_last_of(".+");
            return cut == std::string_view::npos ? fullName : fullName.substr(cut + 1);
        }

        constexpr std::string_view kArraySuffix = "Array";
        constexpr std::string_view kRefSuffix = "Ref";

    } // namespace

    TypeTable::TypeTable(const Catalog& catalog) : catalog_(catalog) {
        Object = Get("SystemObject");
        Void = Get("SystemVoid");
        Boolean = Get("SystemBoolean");
        Int32 = Get("SystemInt32");
        UInt32 = Get("SystemUInt32");
        Int64 = Get("SystemInt64");
        Single = Get("SystemSingle");
        Double = Get("SystemDouble");
        String = Get("SystemString");
        SystemType = Get("SystemType");
    }

    bool TypeTable::IsKnown(std::string_view udonName) const {
        if (udonName.empty()) return false;
        if (catalog_.FindType(udonName) || FindBuiltin(udonName) || IsGenericMarker(udonName)) return true;
        if (udonName.size() > kArraySuffix.size() && udonName.ends_with(kArraySuffix))
            return IsKnown(udonName.substr(0, udonName.size() - kArraySuffix.size()));
        return false;
    }

    const Type* TypeTable::Get(std::string_view udonName) {
        if (auto it = byName_.find(std::string(udonName)); it != byName_.end()) return it->second;
        return Build(udonName, catalog_.FindType(udonName));
    }

    const Type* TypeTable::Build(std::string_view udonName, const TypeInfo* info) {
        Type& type = storage_.emplace_back();
        type.udonName = std::string(udonName);
        type.info = info;
        byName_[type.udonName] = &type;

        const Builtin* builtin = FindBuiltin(udonName);
        if (builtin) {
            type.fullName = std::string(builtin->fullName);
            type.kind = builtin->kind;
            type.numeric = builtin->numeric;
        }

        if (info) {
            if (!info->fullName.empty()) type.fullName = info->fullName;
            type.kind = info->kind;
            if (type.kind == TypeKind::Enum) type.numeric = Numeric::None;
        }

        bool isArray = type.kind == TypeKind::Array;
        std::string elementName = info ? info->elementType : std::string();
        if (!info && !builtin && udonName.size() > kArraySuffix.size() && udonName.ends_with(kArraySuffix)) {
            std::string_view prefix = udonName.substr(0, udonName.size() - kArraySuffix.size());
            if (IsKnown(prefix)) {
                isArray = true;
                elementName = std::string(prefix);
            }
        }
        if (isArray) {
            type.kind = TypeKind::Array;
            if (elementName.empty() && udonName.ends_with(kArraySuffix))
                elementName = std::string(udonName.substr(0, udonName.size() - kArraySuffix.size()));
            type.element = Get(elementName);
            if (type.fullName.empty() && !type.element->fullName.empty()) type.fullName = type.element->fullName + "[]";
        }

        if (type.udonName != "SystemObject" && type.kind != TypeKind::Interface) {
            std::string baseName = info ? info->baseType : std::string();
            if (baseName.empty()) baseName = isArray ? "SystemArray" : "SystemObject";
            if (baseName != type.udonName) type.base = Get(baseName);
        }
        if (info)
            for (const std::string& name : info->interfaces) type.interfaces.push_back(Get(name));

        if (isArray && type.element) type.displayName = "{" + type.element->displayName + "}";
        else if (!type.fullName.empty()) type.displayName = std::string(TrailingName(type.fullName));
        else type.displayName = type.udonName;
        return &type;
    }

    const Type* TypeTable::ArrayOf(const Type* element) {
        return Get(element->udonName + std::string(kArraySuffix));
    }

    const Type* TypeTable::FindByFullName(std::string_view fullName) {
        if (const TypeInfo* info = catalog_.FindTypeByFullName(fullName)) return Get(info->udonName);
        for (const Builtin& b : kBuiltins)
            if (b.fullName == fullName) return Get(b.udonName);
        return nullptr;
    }

    std::vector<const Type*> TypeTable::FindByShortName(std::string_view shortName) {
        std::vector<const Type*> out;
        for (const TypeInfo* info : catalog_.FindTypesByShortName(shortName)) out.push_back(Get(info->udonName));
        for (const Builtin& b : kBuiltins) {
            if (TrailingName(b.fullName) != shortName) continue;
            const Type* t = Get(b.udonName);
            if (std::ranges::find(out, t) == out.end()) out.push_back(t);
        }
        return out;
    }

    std::vector<std::string> TypeTable::SplitParameters(const ExternInfo& ext) const {
        const std::vector<std::string>& pieces = ext.parameters;
        auto known = [&](std::string_view name) {
            if (IsKnown(name)) return true;
            return name.size() > kRefSuffix.size() && name.ends_with(kRefSuffix) && IsKnown(name.substr(0, name.size() - kRefSuffix.size()));
        };
        if (std::ranges::all_of(pieces, [&](const std::string& p) { return known(p); })) return pieces;

        std::vector<std::string> out;
        for (size_t i = 0; i < pieces.size();) {
            size_t take = 1;
            std::string joined = pieces[i];
            std::string candidate = joined;
            for (size_t j = i + 1; j < pieces.size(); ++j) {
                candidate += "_" + pieces[j];
                if (known(candidate)) {
                    take = j - i + 1;
                    joined = candidate;
                }
            }
            out.push_back(joined);
            i += take;
        }
        return out;
    }

    const Method& TypeTable::Describe(const ExternInfo& ext) {
        if (auto it = methods_.find(&ext); it != methods_.end()) return it->second;

        Method method;
        method.ext = &ext;
        for (const std::string& raw : SplitParameters(ext)) {
            Parameter p;
            std::string_view name = raw;
            if (name == "ListT") method.supported = false;
            if (name == "T" || name == "TArray") {
                p.generic = true;
                p.genericArray = name == "TArray";
            } else {
                if (!IsKnown(name) && name.size() > kRefSuffix.size() && name.ends_with(kRefSuffix)) {
                    p.byRef = true;
                    name.remove_suffix(kRefSuffix.size());
                }
                p.type = Get(name);
            }
            method.parameters.push_back(p);
        }
        if (ext.returnType == "ListT") method.supported = false;
        if (ext.returnType == "T" || ext.returnType == "TArray") {
            method.returnGeneric = true;
            method.returnGenericArray = ext.returnType == "TArray";
        } else if (ext.returnType != "SystemVoid") {
            method.returnType = Get(ext.returnType);
        }
        return methods_.emplace(&ext, std::move(method)).first->second;
    }

    std::vector<const Method*> TypeTable::Methods(const Type* owner, std::string_view name, bool wantStatic, bool inherit) {
        std::vector<const Method*> out;
        std::unordered_set<std::string> seen;
        std::unordered_set<const Type*> visited;
        std::queue<const Type*> pending;
        pending.push(owner);

        while (!pending.empty()) {
            const Type* t = pending.front();
            pending.pop();
            if (!t || !visited.insert(t).second) continue;

            for (const ExternInfo* ext : catalog_.FindMethods(t->udonName, name)) {
                if (ext->isStatic != wantStatic) continue;
                if (!seen.insert(ext->parameterText + "|" + ext->returnType).second) continue;
                const Method& m = Describe(*ext);
                if (m.supported) out.push_back(&m);
            }

            if (!inherit) break;
            pending.push(t->base);
            for (const Type* i : t->interfaces) pending.push(i);
            if (!t->base && t != Object) pending.push(Object);
        }
        return out;
    }

    int TypeTable::ReferenceDistance(const Type* from, const Type* to) const {
        if (!from || !to) return -1;
        if (from == to) return 0;

        if (from->kind == TypeKind::Array && to->kind == TypeKind::Array && from->element && to->element &&
            from->element->IsReference() && to->element->IsReference()) {
            int d = ReferenceDistance(from->element, to->element);
            if (d >= 0) return d;
        }

        std::unordered_set<const Type*> visited;
        std::vector<const Type*> frontier = { from };
        for (int depth = 1; !frontier.empty(); ++depth) {
            std::vector<const Type*> next;
            for (const Type* t : frontier) {
                auto visit = [&](const Type* n) {
                    if (n && visited.insert(n).second) next.push_back(n);
                };
                visit(t->base);
                for (const Type* i : t->interfaces) visit(i);
            }
            for (const Type* n : next)
                if (n == to) return depth;
            frontier = std::move(next);
        }
        return to == Object ? 16 : -1;
    }

    bool TypeTable::ImplicitNumeric(Numeric from, Numeric to) {
        if (from == to) return true;
        using N = Numeric;
        switch (from) {
            case N::SByte: return to == N::Int16 || to == N::Int32 || to == N::Int64 || to == N::Single || to == N::Double;
            case N::Byte: return to == N::Int16 || to == N::UInt16 || to == N::Int32 || to == N::UInt32 || to == N::Int64 || to == N::UInt64 || to == N::Single || to == N::Double;
            case N::Int16: return to == N::Int32 || to == N::Int64 || to == N::Single || to == N::Double;
            case N::UInt16:
            case N::Char: return to == N::Int32 || to == N::UInt32 || to == N::Int64 || to == N::UInt64 || to == N::Single || to == N::Double || (from == N::Char && to == N::UInt16);
            case N::Int32: return to == N::Int64 || to == N::Single || to == N::Double;
            case N::UInt32: return to == N::Int64 || to == N::UInt64 || to == N::Single || to == N::Double;
            case N::Int64:
            case N::UInt64: return to == N::Single || to == N::Double;
            case N::Single: return to == N::Double;
            default: return false;
        }
    }

} // namespace UdonLuau::Detail
