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
        { "math", "noise", "(x: number, y: number?) -> number" },
        { "math", "pi", "number" },
        { "math", "huge", "number" },
        { "string", "format", "(format: string, ...any) -> string" },
        { "string", "len", "(s: string) -> number" },
        { "string", "upper", "(s: string) -> string" },
        { "string", "lower", "(s: string) -> string" },
        { "string", "rep", "(s: string, n: number) -> string" },
        { "string", "split", "(s: string, separator: string?) -> { string }" },
        { "string", "sub", "(s: string, i: number, j: number?) -> string" },
        { "string", "find", "(s: string, pattern: string, init: number?, plain: boolean?) -> (number, number)" },
        { "string", "byte", "(s: string, i: number?) -> number" },
        { "string", "char", "(...number) -> string" },
        { "string", "reverse", "(s: string) -> string" },
        { "string", "trim", "(s: string) -> string" },
        { "string", "startswith", "(s: string, prefix: string) -> boolean" },
        { "string", "endswith", "(s: string, suffix: string) -> boolean" },
        { "bit32", "band", "(...number) -> number" },
        { "bit32", "bor", "(...number) -> number" },
        { "bit32", "bxor", "(...number) -> number" },
        { "bit32", "bnot", "(x: number) -> number" },
        { "bit32", "lshift", "(x: number, disp: number) -> number" },
        { "bit32", "rshift", "(x: number, disp: number) -> number" },
        { "bit32", "arshift", "(x: number, disp: number) -> number" },
        { "bit32", "btest", "(...number) -> boolean" },
        { "utf8", "len", "(s: string) -> number" },
        { "utf8", "char", "(...number) -> string" },
        { "table", "insert", "<T>(t: { T }, pos: number | T, value: T?) -> ()" },
        { "table", "remove", "<T>(t: { T }, pos: number?) -> T" },
        { "table", "find", "<T>(t: { T }, value: T, init: number?) -> number" },
        { "table", "create", "<T>(count: number, value: T?) -> { T }" },
        { "table", "clone", "<T>(t: { T }) -> { T }" },
        { "table", "clear", "<T>(t: { T }) -> ()" },
        { "table", "concat", "(t: { string }, separator: string?) -> string" },
        { "table", "sort", "<T>(t: { T }) -> ()" },
        { "table", "move", "<T>(a1: { T }, f: number, e: number, t: number, a2: { T }?) -> { T }" },
        { "task", "wait", "(seconds: number?, timing: any?) -> number" },
        { "task", "waitFrames", "(frames: number, timing: any?) -> number" },
        { "task", "waitUntil", "(condition: boolean) -> ()" },
        { "task", "spawn", "<A...>(fn: (A...) -> ...any, A...) -> ()" },
        { "task", "defer", "<A...>(fn: (A...) -> ...any, A...) -> ()" },
        { "task", "delay", "<A...>(seconds: number, fn: (A...) -> ...any, A...) -> ()" },
        { "task", "cancel", "(fn: (...any) -> ...any) -> ()" },
        { "Delay", "Seconds", "<T>(seconds: number, target: T, timing: any?) -> T" },
        { "Delay", "Frames", "<T>(frames: number, target: T, timing: any?) -> T" },
    };

    [[nodiscard]] constexpr bool IsStandardLibrary(std::string_view library) {
        return library == "math" || library == "string" || library == "table" || library == "bit32" || library == "utf8";
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
