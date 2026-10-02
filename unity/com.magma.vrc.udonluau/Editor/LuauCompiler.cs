using System;
using System.Collections.Generic;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using VRC.Udon.Common;
using VRC.Udon.Common.Interfaces;

namespace Magma.VRC.UdonLuau
{
    /// <summary>A compiler message with a one-based source position.</summary>
    [Serializable]
    public struct LuauDiagnostic
    {
        public bool isWarning;
        public int line;
        public int column;
        public string message;
    }

    /// <summary>An annotation such as @range(0, 10), with unquoted arguments.</summary>
    [Serializable]
    public class LuauAnnotation
    {
        public string name;
        public string[] arguments;
    }

    /// <summary>The annotations written above one variable.</summary>
    [Serializable]
    public class LuauFieldAnnotations
    {
        public string symbol;
        public List<LuauAnnotation> annotations = new List<LuauAnnotation>();
    }

    /// <summary>Everything a compilation produced.</summary>
    public sealed class LuauCompileResult
    {
        /// <summary>The program, or null when compilation or program construction failed.</summary>
        public IUdonProgram Program;

        public readonly List<LuauDiagnostic> Diagnostics = new List<LuauDiagnostic>();
        public readonly List<LuauFieldAnnotations> Fields = new List<LuauFieldAnnotations>();
        public readonly List<LuauAnnotation> ModuleAnnotations = new List<LuauAnnotation>();

        /// <summary>The compiler's readable listing of the program.</summary>
        public string Disassembly = "";

        /// <summary>Whether a program was produced.</summary>
        public bool Succeeded => Program != null;

        internal void Error(string message) => Diagnostics.Add(new LuauDiagnostic { line = 1, column = 1, message = message });
    }

    /// <summary>Compiles Luau source into Udon programs.</summary>
    public static class LuauCompiler
    {
        private const int MinimumHeapSize = 512;

        /// <summary>Compiles Luau source against the editor's catalog and builds the Udon program.</summary>
        public static LuauCompileResult Compile(string source)
        {
            var output = new LuauCompileResult();
            LuauCatalog catalog = LuauCatalog.Current;
            if (catalog == null)
            {
                output.Error("the UdonLuau native compiler is not available");
                return output;
            }

            byte[] bytes = Encoding.UTF8.GetBytes(source ?? "");
            using ResultHandle result = Invoke(catalog, bytes, LuauSettings.instance.Defines);
            if (result.IsInvalid)
            {
                output.Error("the compiler returned no result");
                return output;
            }

            int diagnosticCount = Native.ul_result_diagnostic_count(result);
            for (int i = 0; i < diagnosticCount; i++)
            {
                if (Native.ul_result_diagnostic(result, i, out NativeDiagnostic d) == 0) continue;
                output.Diagnostics.Add(new LuauDiagnostic { isWarning = d.IsWarning != 0, line = d.Line + 1, column = d.Column + 1, message = Native.Read(d.Message) });
            }

            if (Native.ul_result_succeeded(result) == 0) return output;

            output.ModuleAnnotations.AddRange(ReadAnnotations(result, -1));
            output.Disassembly = Native.Read(Native.ul_result_disassembly(result)) ?? "";

            try
            {
                output.Program = new ProgramBuilder(catalog, result, output).Build();
            }
            catch (Exception e)
            {
                output.Program = null;
                output.Error($"failed to build the Udon program: {e.Message}");
            }
            return output;
        }

        private static bool _definesUnsupported;

        private static ResultHandle Invoke(LuauCatalog catalog, byte[] source, string defines)
        {
            if (!_definesUnsupported)
            {
                try
                {
                    return Native.ul_compile_with_defines(catalog.Handle, source, (UIntPtr)source.Length, Native.Utf8(defines ?? ""));
                }
                catch (EntryPointNotFoundException)
                {
                    _definesUnsupported = true;
                    UnityEngine.Debug.LogWarning("[UdonLuau] The loaded native compiler does not support defines; restart the editor after updating UdonLuau.dll.");
                }
            }
            return Native.ul_compile(catalog.Handle, source, (UIntPtr)source.Length);
        }

        internal static List<LuauAnnotation> ReadAnnotations(ResultHandle result, int address)
        {
            var list = new List<LuauAnnotation>();
            int count = Native.ul_result_attribute_count(result, address);
            for (int i = 0; i < count; i++)
            {
                if (Native.ul_result_attribute(result, address, i, out NativeAttribute a) == 0) continue;
                var arguments = new string[a.ArgumentCount];
                for (int j = 0; j < arguments.Length; j++) arguments[j] = Native.Read(Native.ul_result_attribute_argument(result, address, i, j)) ?? "";
                list.Add(new LuauAnnotation { name = Native.Read(a.Name), arguments = arguments });
            }
            return list;
        }

        private sealed class ProgramBuilder
        {
            private readonly LuauCatalog _catalog;
            private readonly ResultHandle _result;
            private readonly LuauCompileResult _output;

            public ProgramBuilder(LuauCatalog catalog, ResultHandle result, LuauCompileResult output)
            {
                _catalog = catalog;
                _result = result;
                _output = output;
            }

            public IUdonProgram Build()
            {
                IntPtr code = Native.ul_result_bytecode(_result, out UIntPtr length);
                var byteCode = new byte[(int)length];
                if (code != IntPtr.Zero) Marshal.Copy(code, byteCode, 0, byteCode.Length);

                int slotCount = Native.ul_result_heap_count(_result);
                var heap = new UdonHeap((uint)Math.Max(slotCount, MinimumHeapSize));
                var symbols = new List<IUdonSymbol>(slotCount);
                var exported = new List<string>();

                for (int address = 0; address < slotCount; address++)
                {
                    if (Native.ul_result_heap_slot(_result, address, out NativeHeapSlot slot) == 0) throw new InvalidOperationException($"heap slot {address} is missing");

                    string symbol = Native.Read(slot.Symbol);
                    string typeName = Native.Read(slot.Type);
                    Type type = _catalog.ResolveType(typeName) ?? throw new InvalidOperationException($"unknown type '{typeName}' for '{symbol}'");

                    Initialize(heap, (uint)address, slot, type);
                    symbols.Add(new UdonSymbol(symbol, type, (uint)address));
                    if (slot.Exported != 0) exported.Add(symbol);

                    var annotations = ReadAnnotations(_result, address);
                    if (annotations.Count > 0) _output.Fields.Add(new LuauFieldAnnotations { symbol = symbol, annotations = annotations });
                }

                var entries = new List<IUdonSymbol>();
                var entryNames = new List<string>();
                int entryCount = Native.ul_result_entry_count(_result);
                for (int i = 0; i < entryCount; i++)
                {
                    if (Native.ul_result_entry(_result, i, out NativeEntryPoint entry) == 0) continue;
                    string name = Native.Read(entry.Name);
                    entries.Add(new UdonSymbol(name, null, entry.Address));
                    entryNames.Add(name);
                }

                var sync = new List<IUdonSyncMetadata>();
                int syncCount = Native.ul_result_sync_count(_result);
                for (int i = 0; i < syncCount; i++)
                {
                    if (Native.ul_result_sync(_result, i, out NativeSyncVariable variable) == 0) continue;
                    var property = new UdonSyncProperty("this", (UdonSyncInterpolationMethod)variable.Interpolation);
                    sync.Add(new UdonSyncMetadata(Native.Read(variable.Symbol), new List<IUdonSyncProperty> { property }));
                }

                return new UdonProgram(
                    "UDON",
                    1,
                    byteCode,
                    heap,
                    new UdonSymbolTable(entries, entryNames),
                    new UdonSymbolTable(symbols, exported),
                    new UdonSyncMetadataTable(sync),
                    Native.ul_result_update_order(_result));
            }

            private void Initialize(UdonHeap heap, uint address, NativeHeapSlot slot, Type type)
            {
                string text = Native.Read(slot.Text);
                switch ((ValueKind)slot.Kind)
                {
                    case ValueKind.Default:
                        heap.InitializeHeapVariable(address, type);
                        break;
                    case ValueKind.This:
                        heap.SetHeapVariable(address, new UdonGameObjectComponentHeapReference(type));
                        break;
                    case ValueKind.Construct:
                        heap.SetHeapVariable(address, Construct(text, (int)address, type), type);
                        break;
                    case ValueKind.Array:
                        heap.SetHeapVariable(address, BuildArray(text, (int)address), type);
                        break;
                    default:
                        var value = new NativeHeapValue { Kind = slot.Kind, Boolean = slot.Boolean, Integer = slot.Integer, Unsigned = slot.Unsigned, Real = slot.Real, Text = slot.Text };
                        heap.SetHeapVariable(address, Convert(value, type), type);
                        break;
                }
            }

            private object Convert(NativeHeapValue value, Type type)
            {
                string text = Native.Read(value.Text);
                switch ((ValueKind)value.Kind)
                {
                    case ValueKind.Null:
                    case ValueKind.Default when !type.IsValueType:
                        return null;
                    case ValueKind.Default:
                        return Activator.CreateInstance(type);
                    case ValueKind.Type:
                        return _catalog.ResolveType(text) ?? throw new InvalidOperationException($"unknown type '{text}'");
                    case ValueKind.Boolean:
                        return ToType(value.Boolean != 0, type);
                    case ValueKind.Integer:
                        return type.IsEnum ? Enum.ToObject(type, value.Integer) : ToType(Natural(value.Integer), type);
                    case ValueKind.Unsigned:
                        return type.IsEnum ? Enum.ToObject(type, value.Unsigned) : ToType(value.Unsigned <= uint.MaxValue ? (object)(uint)value.Unsigned : value.Unsigned, type);
                    case ValueKind.Real:
                        return ToType(value.Real, type);
                    case ValueKind.String:
                        return type == typeof(char) && !string.IsNullOrEmpty(text) ? text[0] : ToType(text, type);
                    default:
                        throw new InvalidOperationException($"value kind {value.Kind} is not a constant");
                }
            }

            private static object Natural(long value) => value >= int.MinValue && value <= int.MaxValue ? (object)(int)value : value;

            private static object ToType(object value, Type type)
            {
                if (type == typeof(object) || type.IsInstanceOfType(value)) return value;
                return System.Convert.ChangeType(value, type, CultureInfo.InvariantCulture);
            }

            private Array BuildArray(string elementName, int address)
            {
                Type element = _catalog.ResolveType(elementName) ?? throw new InvalidOperationException($"unknown array element type '{elementName}'");
                int count = Native.ul_result_heap_argument_count(_result, address);
                Array array = Array.CreateInstance(element, count);
                for (int i = 0; i < count; i++)
                {
                    if (Native.ul_result_heap_argument(_result, address, i, out NativeHeapValue value) == 0) throw new InvalidOperationException($"array element {i} is missing");
                    array.SetValue(Convert(value, element), i);
                }
                return array;
            }

            private object Construct(string signature, int address, Type type)
            {
                List<string> parameterNames = _catalog.ParameterTypes(signature);
                int count = Native.ul_result_heap_argument_count(_result, address);
                if (count != parameterNames.Count) throw new InvalidOperationException($"'{signature}' takes {parameterNames.Count} arguments but {count} were given");

                var scratch = new UdonHeap((uint)count + 1);
                var addresses = new uint[count + 1];
                for (int i = 0; i < count; i++)
                {
                    if (Native.ul_result_heap_argument(_result, address, i, out NativeHeapValue value) == 0) throw new InvalidOperationException($"argument {i} of '{signature}' is missing");
                    if (value.ArgumentCount > 0) throw new InvalidOperationException($"nested constant construction in '{signature}' is not supported");
                    Type parameterType = _catalog.ResolveType(parameterNames[i]) ?? throw new InvalidOperationException($"unknown parameter type '{parameterNames[i]}'");
                    scratch.SetHeapVariable((uint)i, Convert(value, parameterType), parameterType);
                    addresses[i] = (uint)i;
                }
                scratch.InitializeHeapVariable((uint)count, type);
                addresses[count] = (uint)count;

                _catalog.Wrapper.GetExternFunctionDelegate(signature)(scratch, addresses);
                return scratch.GetHeapVariable((uint)count);
            }
        }
    }
}
