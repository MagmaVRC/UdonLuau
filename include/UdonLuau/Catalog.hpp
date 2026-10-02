#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace UdonLuau {

    enum class TypeKind : uint8_t {
        Class,
        Struct,
        Enum,
        Interface,
        Array,
    };

    struct EnumMember {
        std::string name;
        int64_t     value = 0;
    };

    /// <summary>A .NET type as Udon names it.</summary>
    struct TypeInfo {
        std::string             udonName;
        std::string             fullName;
        TypeKind                kind = TypeKind::Class;
        std::string             baseType;
        std::vector<std::string> interfaces;
        std::string             elementType;
        std::string             enumUnderlying;
        std::vector<EnumMember> enumMembers;
    };

    /// <summary>One extern signature, split into its parts.</summary>
    struct ExternInfo {
        std::string              signature;
        std::string              module;
        std::string              method;
        std::string              parameterText;
        std::vector<std::string> parameters;
        std::string              returnType;
        int                      parameterCount = 0;
        bool                     isStatic = true;
        bool                     isGeneric = false;
        bool                     hasTypeOperand = false;
        bool                     isConstructor = false;
    };

    struct EventParameter {
        std::string name;
        std::string type;
    };

    /// <summary>A built-in Udon event such as OnPlayerJoined.</summary>
    struct EventInfo {
        std::string                 name;
        std::vector<EventParameter> parameters;
    };

    /// <summary>A variable of another behaviour's program: a public field, a method parameter or a
    /// return value. type is the Udon type name; script names the behaviour script when the value
    /// is a typed reference to one.</summary>
    struct ScriptVariable {
        std::string name;
        std::string type;
        std::string script;
        std::string symbol;
    };

    /// <summary>A public method of a behaviour script: the entry point SendCustomEvent runs, the
    /// heap symbols its arguments and results are passed through, and whether other clients may
    /// call it over the network.</summary>
    struct ScriptMethod {
        std::string                 name;
        std::string                 entryPoint;
        std::vector<ScriptVariable> parameters;
        std::vector<ScriptVariable> returns;
        bool                        networkCallable = false;
    };

    /// <summary>The public surface of a behaviour script, Luau or UdonSharp.</summary>
    struct ScriptInfo {
        std::string                 name;
        std::vector<ScriptMethod>   methods;
        std::vector<ScriptVariable> fields;
    };

    /// <summary>Everything the host exposes to Udon: types, extern signatures and events.
    /// Filled by the host from the live wrapper modules, so newly exposed externs need no
    /// compiler change.</summary>
    class Catalog {
    public:
        Catalog();
        ~Catalog();
        Catalog(Catalog&&) noexcept;
        Catalog& operator=(Catalog&&) noexcept;
        Catalog(const Catalog&) = delete;
        Catalog& operator=(const Catalog&) = delete;

        /// <summary>Adds or replaces a type.</summary>
        void AddType(TypeInfo type);

        /// <summary>Adds an extern signature with the operand count its wrapper reports.</summary>
        /// <returns>False when the signature is malformed or the count cannot match it.</returns>
        bool AddExtern(std::string_view signature, int parameterCount);

        /// <summary>Adds or replaces a built-in event.</summary>
        void AddEvent(EventInfo event);

        /// <summary>Adds the events VRChat dispatches to every UdonBehaviour, for hosts that
        /// cannot enumerate them.</summary>
        void AddStandardEvents();

        /// <summary>Declares a type Udon can sync, and whether it supports linear and smooth
        /// interpolation. When no syncable types are declared, synced variable types are not checked.</summary>
        void AddSyncableType(std::string_view udonName, bool linear, bool smooth);

        /// <summary>Adds or replaces a behaviour script that Luau code can hold typed references to.</summary>
        void AddScript(ScriptInfo script);
        [[nodiscard]] const ScriptInfo* FindScript(std::string_view name) const;

        struct SyncSupport {
            bool linear = false;
            bool smooth = false;
        };
        [[nodiscard]] bool HasSyncableTypes() const;
        [[nodiscard]] const SyncSupport* FindSyncableType(std::string_view udonName) const;

        /// <summary>Sets the namespaces that decide an ambiguous short type name, highest
        /// priority first.</summary>
        void SetPreferredNamespaces(std::vector<std::string> namespaces);
        [[nodiscard]] std::span<const std::string> PreferredNamespaces() const;

        [[nodiscard]] const TypeInfo* FindType(std::string_view udonName) const;
        [[nodiscard]] const TypeInfo* FindTypeByFullName(std::string_view fullName) const;
        [[nodiscard]] std::vector<const TypeInfo*> FindTypesByShortName(std::string_view shortName) const;
        [[nodiscard]] bool IsNamespace(std::string_view path) const;

        [[nodiscard]] const ExternInfo* FindExtern(std::string_view signature) const;
        [[nodiscard]] std::span<const ExternInfo* const> FindMethods(std::string_view module, std::string_view method) const;

        [[nodiscard]] const EventInfo* FindEvent(std::string_view name) const;

        [[nodiscard]] std::vector<const TypeInfo*> Types() const;
        [[nodiscard]] std::vector<const ExternInfo*> Externs() const;
        [[nodiscard]] std::vector<const EventInfo*> Events() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    /// <summary>Splits an extern signature into module, method, parameter and return names.</summary>
    /// <returns>False when the text is not an extern signature.</returns>
    bool ParseExternSignature(std::string_view signature, ExternInfo& out);

    /// <summary>The Udon name of a .NET full type name: dots, plus signs and commas removed,
    /// "[]" spelled Array and "&amp;" spelled Ref.</summary>
    std::string UdonTypeName(std::string_view fullName);

} // namespace UdonLuau
