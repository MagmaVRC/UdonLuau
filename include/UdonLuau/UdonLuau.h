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
} ul_value_kind;

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

/// <summary>Compiles Luau source against the catalog. Always returns a result.</summary>
UDONLUAU_API ul_result* ul_compile(const ul_catalog* catalog, const char* source, size_t length);

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

UDONLUAU_API int32_t ul_result_entry_count(const ul_result* result);
UDONLUAU_API int32_t ul_result_entry(const ul_result* result, int32_t index, ul_entry_point* out);

UDONLUAU_API int32_t ul_result_sync_count(const ul_result* result);
UDONLUAU_API int32_t ul_result_sync(const ul_result* result, int32_t index, ul_sync_variable* out);

UDONLUAU_API int32_t ul_result_update_order(const ul_result* result);

/// <summary>A readable listing of the compiled program, or an empty string when compilation failed.</summary>
UDONLUAU_API const char* ul_result_disassembly(ul_result* result);

#ifdef __cplusplus
}
#endif
