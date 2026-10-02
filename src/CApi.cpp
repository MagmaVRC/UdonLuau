#include "UdonLuau/UdonLuau.h"

#include "UdonLuau/Compiler.hpp"

#include <algorithm>
#include <new>
#include <optional>
#include <string>
#include <vector>

struct ul_catalog {
    UdonLuau::Catalog catalog;
};

struct ul_result {
    UdonLuau::CompileResult    result;
    std::vector<uint8_t>       bytes;
    std::optional<std::string> disassembly;
};

namespace {

    std::string Str(const char* s) { return s ? std::string(s) : std::string(); }

    std::vector<std::string> SplitList(const char* s) {
        std::vector<std::string> out;
        std::string_view text = s ? s : "";
        while (!text.empty()) {
            size_t sep = text.find(';');
            std::string_view item = text.substr(0, sep);
            if (!item.empty()) out.emplace_back(item);
            if (sep == std::string_view::npos) break;
            text.remove_prefix(sep + 1);
        }
        return out;
    }

    bool InRange(int32_t index, size_t size) { return index >= 0 && static_cast<size_t>(index) < size; }

    UdonLuau::ScriptVariable MakeVariable(const char* name, const char* type, const char* script, const char* symbol) {
        return { Str(name), Str(type), Str(script), Str(symbol) };
    }

    void Fill(const UdonLuau::ScriptVariable& v, ul_script_variable* out) {
        *out = { v.name.c_str(), v.type.c_str(), v.script.c_str(), v.symbol.c_str() };
    }

} // namespace

extern "C" {

ul_catalog* ul_catalog_create(void) {
    return new (std::nothrow) ul_catalog();
}

void ul_catalog_destroy(ul_catalog* catalog) {
    delete catalog;
}

void ul_catalog_add_type(ul_catalog* catalog, const char* udon_name, const char* full_name, int32_t kind, const char* base_type, const char* interfaces, const char* element_type) {
    if (!catalog) return;
    UdonLuau::TypeInfo t;
    t.udonName = Str(udon_name);
    t.fullName = Str(full_name);
    t.kind = static_cast<UdonLuau::TypeKind>(kind);
    t.baseType = Str(base_type);
    t.interfaces = SplitList(interfaces);
    t.elementType = Str(element_type);
    catalog->catalog.AddType(std::move(t));
}

int32_t ul_catalog_add_enum_member(ul_catalog* catalog, const char* udon_name, const char* member, int64_t value) {
    if (!catalog || !udon_name || !member) return 0;
    const UdonLuau::TypeInfo* existing = catalog->catalog.FindType(udon_name);
    if (!existing) return 0;
    UdonLuau::TypeInfo copy = *existing;
    copy.enumMembers.push_back({ member, value });
    catalog->catalog.AddType(std::move(copy));
    return 1;
}

int32_t ul_catalog_add_extern(ul_catalog* catalog, const char* signature, int32_t parameter_count) {
    if (!catalog || !signature) return 0;
    return catalog->catalog.AddExtern(signature, parameter_count) ? 1 : 0;
}

void ul_catalog_add_event(ul_catalog* catalog, const char* name, const char* const* parameter_names, const char* const* parameter_types, int32_t parameter_count) {
    if (!catalog || !name) return;
    UdonLuau::EventInfo e;
    e.name = name;
    for (int32_t i = 0; i < parameter_count; ++i) e.parameters.push_back({ Str(parameter_names[i]), Str(parameter_types[i]) });
    catalog->catalog.AddEvent(std::move(e));
}

void ul_catalog_add_standard_events(ul_catalog* catalog) {
    if (catalog) catalog->catalog.AddStandardEvents();
}

void ul_catalog_add_script(ul_catalog* catalog, const char* name) {
    if (!catalog || !name || !*name) return;
    UdonLuau::ScriptInfo script;
    script.name = name;
    catalog->catalog.AddScript(std::move(script));
}

int32_t ul_catalog_add_script_field(ul_catalog* catalog, const char* script, const char* name, const char* udon_type, const char* script_type, const char* symbol) {
    if (!catalog || !script) return 0;
    const UdonLuau::ScriptInfo* existing = catalog->catalog.FindScript(script);
    if (!existing) return 0;
    UdonLuau::ScriptInfo copy = *existing;
    copy.fields.push_back(MakeVariable(name, udon_type, script_type, symbol));
    catalog->catalog.AddScript(std::move(copy));
    return 1;
}

int32_t ul_catalog_add_script_method(ul_catalog* catalog, const char* script, const char* name, const char* entry_point) {
    if (!catalog || !script || !name) return 0;
    const UdonLuau::ScriptInfo* existing = catalog->catalog.FindScript(script);
    if (!existing) return 0;
    UdonLuau::ScriptInfo copy = *existing;
    copy.methods.push_back({ name, entry_point ? entry_point : name, {}, {} });
    catalog->catalog.AddScript(std::move(copy));
    return 1;
}

int32_t ul_catalog_add_script_method_value(ul_catalog* catalog, const char* script, const char* method, int32_t is_return, const char* name, const char* udon_type, const char* script_type, const char* symbol) {
    if (!catalog || !script || !method) return 0;
    const UdonLuau::ScriptInfo* existing = catalog->catalog.FindScript(script);
    if (!existing) return 0;
    UdonLuau::ScriptInfo copy = *existing;
    auto it = std::find_if(copy.methods.begin(), copy.methods.end(), [&](const UdonLuau::ScriptMethod& m) { return m.name == method; });
    if (it == copy.methods.end()) return 0;
    (is_return ? it->returns : it->parameters).push_back(MakeVariable(name, udon_type, script_type, symbol));
    catalog->catalog.AddScript(std::move(copy));
    return 1;
}

ul_result* ul_extract_interface(const ul_catalog* catalog, const char* source, size_t length, const char* defines) {
    auto* r = new (std::nothrow) ul_result();
    if (!r) return nullptr;
    if (!catalog) {
        r->result.diagnostics.push_back({ UdonLuau::Severity::Error, "no catalog" });
        return r;
    }
    UdonLuau::CompileOptions options;
    for (const std::string& pair : SplitList(defines)) {
        size_t eq = pair.find('=');
        options.defines[pair.substr(0, eq)] = eq == std::string::npos ? "true" : pair.substr(eq + 1);
    }
    try {
        r->result = UdonLuau::ExtractInterface(catalog->catalog, std::string_view(source ? source : "", source ? length : 0), options);
    } catch (const std::exception& e) {
        r->result = {};
        r->result.diagnostics.push_back({ UdonLuau::Severity::Error, std::string("internal compiler error: ") + e.what() });
    }
    return r;
}

void ul_catalog_set_preferred_namespaces(ul_catalog* catalog, const char* namespaces) {
    if (catalog) catalog->catalog.SetPreferredNamespaces(SplitList(namespaces));
}

ul_result* ul_compile(const ul_catalog* catalog, const char* source, size_t length) {
    return ul_compile_with_defines(catalog, source, length, nullptr);
}

ul_result* ul_compile_with_defines(const ul_catalog* catalog, const char* source, size_t length, const char* defines) {
    UdonLuau::CompileOptions options;
    for (const std::string& pair : SplitList(defines)) {
        size_t eq = pair.find('=');
        options.defines[pair.substr(0, eq)] = eq == std::string::npos ? "true" : pair.substr(eq + 1);
    }
    auto* r = new (std::nothrow) ul_result();
    if (!r) return nullptr;
    if (!catalog) {
        r->result.diagnostics.push_back({ UdonLuau::Severity::Error, "no catalog" });
        return r;
    }
    try {
        r->result = UdonLuau::Compile(catalog->catalog, std::string_view(source ? source : "", source ? length : 0), options);
    } catch (const std::exception& e) {
        r->result = {};
        r->result.diagnostics.push_back({ UdonLuau::Severity::Error, std::string("internal compiler error: ") + e.what() });
    }
    if (r->result.program) r->bytes = r->result.program->ByteCode();
    return r;
}

void ul_result_destroy(ul_result* result) {
    delete result;
}

int32_t ul_result_succeeded(const ul_result* result) {
    return result && result->result.Succeeded() ? 1 : 0;
}

int32_t ul_result_diagnostic_count(const ul_result* result) {
    return result ? static_cast<int32_t>(result->result.diagnostics.size()) : 0;
}

int32_t ul_result_diagnostic(const ul_result* result, int32_t index, ul_diagnostic* out) {
    if (!result || !out || !InRange(index, result->result.diagnostics.size())) return 0;
    const UdonLuau::Diagnostic& d = result->result.diagnostics[static_cast<size_t>(index)];
    *out = { d.severity == UdonLuau::Severity::Warning, d.line, d.column, d.endLine, d.endColumn, d.message.c_str() };
    return 1;
}

const uint8_t* ul_result_bytecode(const ul_result* result, size_t* length) {
    if (length) *length = result ? result->bytes.size() : 0;
    return result && !result->bytes.empty() ? result->bytes.data() : nullptr;
}

int32_t ul_result_heap_count(const ul_result* result) {
    return result && result->result.program ? static_cast<int32_t>(result->result.program->heap.size()) : 0;
}

int32_t ul_result_heap_slot(const ul_result* result, int32_t address, ul_heap_slot* out) {
    if (!result || !out || !result->result.program || !InRange(address, result->result.program->heap.size())) return 0;
    const UdonLuau::HeapSlot& s = result->result.program->heap[static_cast<size_t>(address)];
    const UdonLuau::HeapValue& v = s.value;
    *out = { s.symbol.c_str(), s.type.c_str(), s.exported, static_cast<int32_t>(v.kind), v.boolean, v.integer, v.unsignedInteger, v.real, v.text.c_str() };
    return 1;
}

namespace {

    const UdonLuau::HeapSlot* SlotAt(const ul_result* result, int32_t address) {
        if (!result || !result->result.program || !InRange(address, result->result.program->heap.size())) return nullptr;
        return &result->result.program->heap[static_cast<size_t>(address)];
    }

    const std::vector<UdonLuau::FieldAttribute>* AttributesAt(const ul_result* result, int32_t address) {
        if (address == -1) return result && result->result.program ? &result->result.program->attributes : nullptr;
        const UdonLuau::HeapSlot* slot = SlotAt(result, address);
        return slot ? &slot->attributes : nullptr;
    }

} // namespace

int32_t ul_result_heap_argument_count(const ul_result* result, int32_t address) {
    const UdonLuau::HeapSlot* slot = SlotAt(result, address);
    return slot ? static_cast<int32_t>(slot->value.arguments.size()) : 0;
}

int32_t ul_result_heap_argument(const ul_result* result, int32_t address, int32_t index, ul_heap_value* out) {
    const UdonLuau::HeapSlot* slot = SlotAt(result, address);
    if (!slot || !out || !InRange(index, slot->value.arguments.size())) return 0;
    const UdonLuau::HeapValue& v = slot->value.arguments[static_cast<size_t>(index)];
    *out = { static_cast<int32_t>(v.kind), v.boolean, v.integer, v.unsignedInteger, v.real, v.text.c_str(), static_cast<int32_t>(v.arguments.size()) };
    return 1;
}

int32_t ul_result_attribute_count(const ul_result* result, int32_t address) {
    const auto* attributes = AttributesAt(result, address);
    return attributes ? static_cast<int32_t>(attributes->size()) : 0;
}

int32_t ul_result_attribute(const ul_result* result, int32_t address, int32_t index, ul_attribute* out) {
    const auto* attributes = AttributesAt(result, address);
    if (!attributes || !out || !InRange(index, attributes->size())) return 0;
    const UdonLuau::FieldAttribute& a = (*attributes)[static_cast<size_t>(index)];
    *out = { a.name.c_str(), static_cast<int32_t>(a.arguments.size()) };
    return 1;
}

const char* ul_result_attribute_argument(const ul_result* result, int32_t address, int32_t index, int32_t argument) {
    const auto* attributes = AttributesAt(result, address);
    if (!attributes || !InRange(index, attributes->size())) return nullptr;
    const UdonLuau::FieldAttribute& a = (*attributes)[static_cast<size_t>(index)];
    return InRange(argument, a.arguments.size()) ? a.arguments[static_cast<size_t>(argument)].c_str() : nullptr;
}

int32_t ul_result_entry_count(const ul_result* result) {
    return result && result->result.program ? static_cast<int32_t>(result->result.program->entryPoints.size()) : 0;
}

int32_t ul_result_entry(const ul_result* result, int32_t index, ul_entry_point* out) {
    if (!result || !out || !result->result.program || !InRange(index, result->result.program->entryPoints.size())) return 0;
    const UdonLuau::EntryPoint& e = result->result.program->entryPoints[static_cast<size_t>(index)];
    *out = { e.name.c_str(), e.address };
    return 1;
}

int32_t ul_result_sync_count(const ul_result* result) {
    return result && result->result.program ? static_cast<int32_t>(result->result.program->sync.size()) : 0;
}

int32_t ul_result_sync(const ul_result* result, int32_t index, ul_sync_variable* out) {
    if (!result || !out || !result->result.program || !InRange(index, result->result.program->sync.size())) return 0;
    const UdonLuau::SyncVariable& s = result->result.program->sync[static_cast<size_t>(index)];
    *out = { s.symbol.c_str(), static_cast<int32_t>(s.interpolation) };
    return 1;
}

int32_t ul_result_update_order(const ul_result* result) {
    return result && result->result.program ? result->result.program->updateOrder : 0;
}

int32_t ul_result_has_interface(const ul_result* result) {
    return result && result->result.scriptInterface ? 1 : 0;
}

int32_t ul_result_interface_field_count(const ul_result* result) {
    return ul_result_has_interface(result) ? static_cast<int32_t>(result->result.scriptInterface->fields.size()) : 0;
}

int32_t ul_result_interface_field(const ul_result* result, int32_t index, ul_script_variable* out) {
    if (!ul_result_has_interface(result) || !out || !InRange(index, result->result.scriptInterface->fields.size())) return 0;
    Fill(result->result.scriptInterface->fields[static_cast<size_t>(index)], out);
    return 1;
}

int32_t ul_result_interface_method_count(const ul_result* result) {
    return ul_result_has_interface(result) ? static_cast<int32_t>(result->result.scriptInterface->methods.size()) : 0;
}

int32_t ul_result_interface_method(const ul_result* result, int32_t index, ul_script_method* out) {
    if (!ul_result_has_interface(result) || !out || !InRange(index, result->result.scriptInterface->methods.size())) return 0;
    const UdonLuau::ScriptMethod& m = result->result.scriptInterface->methods[static_cast<size_t>(index)];
    *out = { m.name.c_str(), m.entryPoint.c_str(), static_cast<int32_t>(m.parameters.size()), static_cast<int32_t>(m.returns.size()) };
    return 1;
}

int32_t ul_result_interface_method_value(const ul_result* result, int32_t method, int32_t is_return, int32_t index, ul_script_variable* out) {
    if (!ul_result_has_interface(result) || !out || !InRange(method, result->result.scriptInterface->methods.size())) return 0;
    const UdonLuau::ScriptMethod& m = result->result.scriptInterface->methods[static_cast<size_t>(method)];
    const auto& values = is_return ? m.returns : m.parameters;
    if (!InRange(index, values.size())) return 0;
    Fill(values[static_cast<size_t>(index)], out);
    return 1;
}

const char* ul_result_disassembly(ul_result* result) {
    if (!result) return "";
    if (!result->disassembly) result->disassembly = result->result.program ? UdonLuau::Disassemble(*result->result.program) : std::string();
    return result->disassembly->c_str();
}

}
