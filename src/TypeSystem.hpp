#pragma once

#include "UdonLuau/Catalog.hpp"

#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace UdonLuau::Detail {

    enum class Numeric : uint8_t {
        None,
        SByte,
        Byte,
        Int16,
        UInt16,
        Char,
        Int32,
        UInt32,
        Int64,
        UInt64,
        Single,
        Double,
    };

    struct Type {
        std::string              udonName;
        std::string              fullName;
        std::string              displayName;
        TypeKind                 kind = TypeKind::Class;
        const Type*              base = nullptr;
        std::vector<const Type*> interfaces;
        const Type*              element = nullptr;
        const Type*              listArray = nullptr;
        const TypeInfo*          info = nullptr;
        const ScriptInfo*        script = nullptr;
        Numeric                  numeric = Numeric::None;

        [[nodiscard]] bool IsValueType() const { return kind == TypeKind::Struct || kind == TypeKind::Enum; }
        [[nodiscard]] bool IsReference() const { return !IsValueType(); }
        [[nodiscard]] bool IsNumeric() const { return numeric != Numeric::None; }
        [[nodiscard]] bool IsIntegral() const { return IsNumeric() && numeric != Numeric::Single && numeric != Numeric::Double; }
        [[nodiscard]] bool IsList() const { return listArray != nullptr; }
    };

    struct Parameter {
        const Type* type = nullptr;
        bool        byRef = false;
        bool        generic = false;
        bool        genericArray = false;
    };

    struct Method {
        const ExternInfo*      ext = nullptr;
        std::vector<Parameter> parameters;
        const Type*            returnType = nullptr;
        bool                   returnGeneric = false;
        bool                   returnGenericArray = false;
        bool                   isStatic = true;
        bool                   supported = true;
    };

    class TypeTable {
    public:
        explicit TypeTable(const Catalog& catalog);

        [[nodiscard]] const Type* Get(std::string_view udonName);
        [[nodiscard]] const Type* ArrayOf(const Type* element);
        [[nodiscard]] const Type* ListOf(const Type* element);
        [[nodiscard]] const Type* Script(const ScriptInfo* script);
        [[nodiscard]] const Type* Behaviour() { return Get("VRCUdonUdonBehaviour"); }
        [[nodiscard]] const Type* FindByFullName(std::string_view fullName);
        [[nodiscard]] std::vector<const Type*> FindByShortName(std::string_view shortName);
        [[nodiscard]] bool IsNamespace(std::string_view path) const { return catalog_.IsNamespace(path); }
        [[nodiscard]] bool IsKnown(std::string_view udonName) const;

        [[nodiscard]] const Method& Describe(const ExternInfo& ext);
        [[nodiscard]] std::vector<const Method*> Methods(const Type* owner, std::string_view name, bool wantStatic, bool inherit);

        [[nodiscard]] int ReferenceDistance(const Type* from, const Type* to) const;
        [[nodiscard]] static bool ImplicitNumeric(Numeric from, Numeric to);

        [[nodiscard]] const Catalog& GetCatalog() const { return catalog_; }

        const Type* Object;
        const Type* Void;
        const Type* Boolean;
        const Type* Int32;
        const Type* UInt32;
        const Type* Int64;
        const Type* Single;
        const Type* Double;
        const Type* String;
        const Type* SystemType;

    private:
        const Type* Build(std::string_view udonName, const TypeInfo* info);
        std::vector<std::string> SplitParameters(const ExternInfo& ext) const;

        const Catalog&                                     catalog_;
        std::deque<Type>                                   storage_;
        std::unordered_map<std::string, const Type*>       byName_;
        std::unordered_map<const ExternInfo*, Method>      methods_;
        std::unordered_map<const ScriptInfo*, const Type*> scripts_;
        std::unordered_map<const Type*, const Type*>       lists_;
    };

} // namespace UdonLuau::Detail
