#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(UDONLUAU_SHARED)
#if defined(UDONLUAU_EXPORTS)
#define UDONLUAU_API __declspec(dllexport)
#else
#define UDONLUAU_API __declspec(dllimport)
#endif
#else
#define UDONLUAU_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ul_catalog ul_catalog;
typedef struct ul_result ul_result;

typedef enum ul_type_kind {
    UL_TYPE_CLASS = 0,
    UL_TYPE_STRUCT = 1,
    UL_TYPE_ENUM = 2,
    UL_TYPE_INTERFACE = 3,
    UL_TYPE_ARRAY = 4,
} ul_type_kind;

typedef enum ul_value_kind {
    UL_VALUE_DEFAULT = 0,
    UL_VALUE_NULL = 1,
    UL_VALUE_BOOLEAN = 2,
    UL_VALUE_INTEGER = 3,
    UL_VALUE_UNSIGNED = 4,
    UL_VALUE_REAL = 5,
    UL_VALUE_STRING = 6,
    UL_VALUE_THIS = 7,
    UL_VALUE_TYPE = 8,
    UL_VALUE_CONSTRUCT = 9,
    UL_VALUE_ARRAY = 10,
} ul_value_kind;

typedef struct ul_heap_value {
    int32_t     kind;
    int32_t     boolean;
    int64_t     integer;
    uint64_t    unsigned_integer;
    double      real;
    const char* text;
    int32_t     argument_count;
} ul_heap_value;

/// <summary>A compiler message. Lines and columns are zero-based; the end is exclusive.</summary>
typedef struct ul_diagnostic {
    int32_t     is_warning;
    int32_t     line;
    int32_t     column;
    int32_t     end_line;
    int32_t     end_column;
    const char* message;
} ul_diagnostic;

typedef struct ul_heap_slot {
    const char* symbol;
    const char* type;
    int32_t     exported;
    int32_t     kind;
    int32_t     boolean;
    int64_t     integer;
    uint64_t    unsigned_integer;
    double      real;
    const char* text;
} ul_heap_slot;

typedef struct ul_attribute {
    const char* name;
    int32_t     argument_count;
} ul_attribute;

typedef struct ul_script_variable {
    const char* name;
    const char* type;
    const char* script;
    const char* symbol;
} ul_script_variable;

typedef struct ul_script_method {
    const char* name;
    const char* entry_point;
    int32_t     parameter_count;
    int32_t     return_count;
} ul_script_method;

typedef struct ul_network_callable {
    const char* entry_point;
    int32_t     max_events_per_second;
    int32_t     parameter_count;
} ul_network_callable;

typedef struct ul_network_parameter {
    const char* symbol;
    const char* type;
} ul_network_parameter;

typedef struct ul_entry_point {
    const char* name;
    uint32_t    address;
} ul_entry_point;

typedef struct ul_sync_variable {
    const char* symbol;
    int32_t     interpolation;
} ul_sync_variable;

/// <summary>Creates an empty catalog.</summary>
UDONLUAU_API ul_catalog* ul_catalog_create(void);

/// <summary>Destroys a catalog.</summary>
UDONLUAU_API void ul_catalog_destroy(ul_catalog* catalog);

/// <summary>Adds or replaces a type. Interfaces are separated by ';'; any name may be null.</summary>
UDONLUAU_API void ul_catalog_add_type(ul_catalog* catalog, const char* udon_name, const char* full_name, int32_t kind, const char* base_type, const char* interfaces, const char* element_type);

/// <summary>Adds a named value to an enum type added earlier.</summary>
/// <returns>0 when the type is unknown.</returns>
UDONLUAU_API int32_t ul_catalog_add_enum_member(ul_catalog* catalog, const char* udon_name, const char* member, int64_t value);

/// <summary>Adds an extern signature with the operand count its wrapper module reports.</summary>
/// <returns>0 when the signature is malformed.</returns>
UDONLUAU_API int32_t ul_catalog_add_extern(ul_catalog* catalog, const char* signature, int32_t parameter_count);

/// <summary>Adds or replaces an event. Parameter names and Udon type names are parallel arrays.</summary>
UDONLUAU_API void ul_catalog_add_event(ul_catalog* catalog, const char* name, const char* const* parameter_names, const char* const* parameter_types, int32_t parameter_count);

/// <summary>Adds the events VRChat dispatches to every UdonBehaviour.</summary>
UDONLUAU_API void ul_catalog_add_standard_events(ul_catalog* catalog);

/// <summary>Sets the ';'-separated namespaces that decide an ambiguous short type name, highest priority first.</summary>
UDONLUAU_API void ul_catalog_set_preferred_namespaces(ul_catalog* catalog, const char* namespaces);

/// <summary>Declares a type Udon can sync and whether it supports linear and smooth interpolation.
/// When none are declared, synced variable types are not checked.</summary>
UDONLUAU_API void ul_catalog_add_syncable_type(ul_catalog* catalog, const char* udon_name, int32_t linear, int32_t smooth);

/// <summary>Adds a behaviour script that Luau code can hold typed references to, replacing any earlier one with the same name.</summary>
UDONLUAU_API void ul_catalog_add_script(ul_catalog* catalog, const char* name);

/// <summary>Marks a script added earlier as a singleton (singleton 1) or a regular script (singleton 0).</summary>
/// <returns>0 when the script is unknown.</returns>
UDONLUAU_API int32_t ul_catalog_set_script_singleton(ul_catalog* catalog, const char* script, int32_t singleton);

/// <summary>Adds a public field to a script added earlier. script_type names a behaviour script when the field holds a typed reference to one.</summary>
/// <returns>0 when the script is unknown.</returns>
UDONLUAU_API int32_t ul_catalog_add_script_field(ul_catalog* catalog, const char* script, const char* name, const char* udon_type, const char* script_type, const char* symbol);

/// <summary>Adds a public method to a script added earlier.</summary>
/// <returns>0 when the script is unknown.</returns>
UDONLUAU_API int32_t ul_catalog_add_script_method(ul_catalog* catalog, const char* script, const char* name, const char* entry_point);

/// <summary>Appends a parameter (is_return 0) or a return value (is_return 1) to a method added earlier.</summary>
/// <returns>0 when the script or method is unknown.</returns>
UDONLUAU_API int32_t ul_catalog_add_script_method_value(ul_catalog* catalog, const char* script, const char* method, int32_t is_return, const char* name, const char* udon_type, const char* script_type, const char* symbol);

/// <summary>Marks a method added earlier as callable by other clients over the network.</summary>
/// <returns>0 when the script or method is unknown.</returns>
UDONLUAU_API int32_t ul_catalog_set_script_method_network_callable(ul_catalog* catalog, const char* script, const char* method, int32_t network_callable);

/// <summary>Reads a module's public methods and fields without compiling function bodies. The result has no program.</summary>
UDONLUAU_API ul_result* ul_extract_interface(const ul_catalog* catalog, const char* source, size_t length, const char* defines);

/// <summary>A Luau definitions file (.d.luau) describing everything the catalog exposes, for luau-lsp and other Luau tooling.
/// The string stays valid until the next call or until the catalog is destroyed.</summary>
UDONLUAU_API const char* ul_catalog_definitions(ul_catalog* catalog);

/// <summary>Compiles Luau source against the catalog. Always returns a result.</summary>
UDONLUAU_API ul_result* ul_compile(const ul_catalog* catalog, const char* source, size_t length);

/// <summary>Compiles with compile-time defines given as ';'-separated NAME=value pairs, for example "DEBUG=true;LEVEL=3".</summary>
UDONLUAU_API ul_result* ul_compile_with_defines(const ul_catalog* catalog, const char* source, size_t length, const char* defines);

/// <summary>Compiles one part of a module: its per-instance program (static_part 0) or the companion singleton holding its static fields and functions (static_part 1). script_name is the script's name; statics live in a companion singleton named script_name + ".Static".</summary>
UDONLUAU_API ul_result* ul_compile_part(const ul_catalog* catalog, const char* source, size_t length, const char* defines, const char* script_name, int32_t static_part);

/// <summary>Reads the public interface of one part of a module, as ul_compile_part would compile it.</summary>
UDONLUAU_API ul_result* ul_extract_interface_part(const ul_catalog* catalog, const char* source, size_t length, const char* defines, const char* script_name, int32_t static_part);

/// <summary>Whether the module declares static fields or functions, so its companion singleton must also be compiled and placed.</summary>
UDONLUAU_API int32_t ul_result_has_statics(const ul_result* result);

/// <summary>Destroys a result and every string it returned.</summary>
UDONLUAU_API void ul_result_destroy(ul_result* result);

/// <summary>Whether compilation produced a program.</summary>
UDONLUAU_API int32_t ul_result_succeeded(const ul_result* result);

UDONLUAU_API int32_t ul_result_diagnostic_count(const ul_result* result);
UDONLUAU_API int32_t ul_result_diagnostic(const ul_result* result, int32_t index, ul_diagnostic* out);

/// <summary>The program's code in Udon's big-endian byte order.</summary>
UDONLUAU_API const uint8_t* ul_result_bytecode(const ul_result* result, size_t* length);

UDONLUAU_API int32_t ul_result_heap_count(const ul_result* result);
UDONLUAU_API int32_t ul_result_heap_slot(const ul_result* result, int32_t address, ul_heap_slot* out);

/// <summary>Constant arguments of a UL_VALUE_CONSTRUCT slot in constructor parameter order, or the
/// elements of a UL_VALUE_ARRAY slot.</summary>
UDONLUAU_API int32_t ul_result_heap_argument_count(const ul_result* result, int32_t address);
UDONLUAU_API int32_t ul_result_heap_argument(const ul_result* result, int32_t address, int32_t index, ul_heap_value* out);

/// <summary>Annotations written above the variable at a heap address, such as @range(0, 10).
/// Address -1 reads the annotations at the top of the file.</summary>
UDONLUAU_API int32_t ul_result_attribute_count(const ul_result* result, int32_t address);
UDONLUAU_API int32_t ul_result_attribute(const ul_result* result, int32_t address, int32_t index, ul_attribute* out);
UDONLUAU_API const char* ul_result_attribute_argument(const ul_result* result, int32_t address, int32_t index, int32_t argument);

UDONLUAU_API int32_t ul_result_entry_count(const ul_result* result);
UDONLUAU_API int32_t ul_result_entry(const ul_result* result, int32_t index, ul_entry_point* out);

UDONLUAU_API int32_t ul_result_sync_count(const ul_result* result);
UDONLUAU_API int32_t ul_result_sync(const ul_result* result, int32_t index, ul_sync_variable* out);

UDONLUAU_API int32_t ul_result_update_order(const ul_result* result);

/// <summary>The sync mode the script requires: 0 any (the user picks on the behaviour), 1 none,
/// 2 no variable sync, 3 continuous, 4 manual. Matches UdonSharp's BehaviourSyncMode.</summary>
UDONLUAU_API int32_t ul_result_sync_mode(const ul_result* result);

/// <summary>Whether the result carries the module's public interface, which it does whenever its declarations were valid.</summary>
UDONLUAU_API int32_t ul_result_has_interface(const ul_result* result);
UDONLUAU_API int32_t ul_result_interface_field_count(const ul_result* result);
UDONLUAU_API int32_t ul_result_interface_field(const ul_result* result, int32_t index, ul_script_variable* out);
UDONLUAU_API int32_t ul_result_interface_method_count(const ul_result* result);
UDONLUAU_API int32_t ul_result_interface_method(const ul_result* result, int32_t index, ul_script_method* out);

/// <summary>Whether the module is marked @singleton.</summary>
UDONLUAU_API int32_t ul_result_interface_singleton(const ul_result* result);

/// <summary>Whether a public method is marked @networkcallable.</summary>
UDONLUAU_API int32_t ul_result_interface_method_network_callable(const ul_result* result, int32_t index);

/// <summary>Entry points other clients may run over the network, with the heap symbols their arguments are written to.
/// max_events_per_second is 0 for the SDK default.</summary>
UDONLUAU_API int32_t ul_result_network_count(const ul_result* result);
UDONLUAU_API int32_t ul_result_network(const ul_result* result, int32_t index, ul_network_callable* out);
UDONLUAU_API int32_t ul_result_network_parameter(const ul_result* result, int32_t index, int32_t parameter, ul_network_parameter* out);

/// <summary>A parameter (is_return 0) or return value (is_return 1) of a public method.</summary>
UDONLUAU_API int32_t ul_result_interface_method_value(const ul_result* result, int32_t method, int32_t is_return, int32_t index, ul_script_variable* out);

/// <summary>A readable listing of the compiled program, or an empty string when compilation failed.</summary>
UDONLUAU_API const char* ul_result_disassembly(ul_result* result);

#ifdef __cplusplus
}
#endif
