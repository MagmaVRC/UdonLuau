#pragma once

#include <string>
#include <string_view>

namespace UdonLuau::Detail {

    struct Polyfill {
        std::string_view library;
        std::string_view name;
        std::string_view signature;
    };

    inline constexpr Polyfill kPolyfills[] = {
        { "math", "abs", "(x: number) -> number" },
        { "math", "floor", "(x: number) -> number" },
        { "math", "ceil", "(x: number) -> number" },
        { "math", "sqrt", "(x: number) -> number" },
        { "math", "sin", "(x: number) -> number" },
        { "math", "cos", "(x: number) -> number" },
        { "math", "tan", "(x: number) -> number" },
        { "math", "asin", "(x: number) -> number" },
        { "math", "acos", "(x: number) -> number" },
        { "math", "atan", "(y: number, x: number?) -> number" },
        { "math", "exp", "(x: number) -> number" },
        { "math", "log", "(x: number, base: number?) -> number" },
        { "math", "log10", "(x: number) -> number" },
        { "math", "pow", "(x: number, y: number) -> number" },
        { "math", "min", "(x: number, ...number) -> number" },
        { "math", "max", "(x: number, ...number) -> number" },
        { "math", "clamp", "(x: number, min: number, max: number) -> number" },
        { "math", "sign", "(x: number) -> number" },
        { "math", "round", "(x: number) -> number" },
        { "math", "fmod", "(x: number, y: number) -> number" },
        { "math", "lerp", "(a: number, b: number, t: number) -> number" },
        { "math", "map", "(x: number, inMin: number, inMax: number, outMin: number, outMax: number) -> number" },
        { "math", "random", "(m: number?, n: number?) -> number" },
        { "math", "pi", "number" },
        { "math", "huge", "number" },
        { "string", "format", "(format: string, ...any) -> string" },
        { "string", "len", "(s: string) -> number" },
        { "string", "upper", "(s: string) -> string" },
        { "string", "lower", "(s: string) -> string" },
        { "string", "rep", "(s: string, n: number) -> string" },
        { "string", "split", "(s: string, separator: string?) -> { string }" },
        { "table", "insert", "<T>(t: { T }, pos: number | T, value: T?) -> ()" },
        { "table", "remove", "<T>(t: { T }, pos: number?) -> T" },
        { "table", "find", "<T>(t: { T }, value: T, init: number?) -> number" },
        { "table", "create", "<T>(count: number, value: T?) -> { T }" },
        { "table", "clone", "<T>(t: { T }) -> { T }" },
        { "table", "clear", "<T>(t: { T }) -> ()" },
        { "table", "concat", "(t: { string }, separator: string?) -> string" },
        { "table", "sort", "<T>(t: { T }) -> ()" },
        { "table", "move", "<T>(a1: { T }, f: number, e: number, t: number, a2: { T }?) -> { T }" },
        { "Delay", "Seconds", "<T>(seconds: number, target: T, timing: any?) -> T" },
        { "Delay", "Frames", "<T>(frames: number, target: T, timing: any?) -> T" },
    };

    [[nodiscard]] constexpr bool IsStandardLibrary(std::string_view library) {
        return library == "math" || library == "string" || library == "table";
    }

    [[nodiscard]] constexpr bool IsPolyfillLibrary(std::string_view library) {
        for (const Polyfill& p : kPolyfills)
            if (p.library == library) return true;
        return false;
    }

    [[nodiscard]] constexpr const Polyfill* FindPolyfill(std::string_view library, std::string_view name) {
        for (const Polyfill& p : kPolyfills)
            if (p.library == library && p.name == name) return &p;
        return nullptr;
    }

    [[nodiscard]] inline std::string PolyfillNames(std::string_view library) {
        std::string names;
        for (const Polyfill& p : kPolyfills)
            if (p.library == library) names += (names.empty() ? "" : ", ") + std::string(p.name);
        return names;
    }

} // namespace UdonLuau::Detail
