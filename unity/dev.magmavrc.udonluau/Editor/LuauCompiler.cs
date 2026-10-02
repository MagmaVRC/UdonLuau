using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using VRC.SDK3.UdonNetworkCalling;
using VRC.Udon.Common;
using VRC.Udon.Common.Interfaces;

namespace Magma.VRC.UdonLuau
{
    /// <summary>A compiler message with a one-based source position.</summary>
    [Serializable]
    public struct LuauDiagnostic
    {
        /// <summary>Whether the message is a warning rather than an error.</summary>
        public bool isWarning;

        /// <summary>The one-based line.</summary>
        public int line;

        /// <summary>The one-based column.</summary>
        public int column;

        /// <summary>The message text.</summary>
        public string message;
    }

    [Serializable]
    internal class LuauAnnotation
    {
        public string name;
        public string[] arguments;
    }

    [Serializable]
    internal class LuauFieldAnnotations
    {
        public string symbol;
        public List<LuauAnnotation> annotations = new List<LuauAnnotation>();
    }

    [Serializable]
    internal class LuauScriptReference
    {
        public string symbol;
        public string script;
    }

    internal sealed class LuauCompileResult
    {
        /// <summary>The program, or null when compilation or program construction failed.</summary>
        public IUdonProgram Program;

        public readonly List<LuauDiagnostic> Diagnostics = new List<LuauDiagnostic>();
        public readonly List<LuauFieldAnnotations> Fields = new List<LuauFieldAnnotations>();
        public readonly List<LuauAnnotation> ModuleAnnotations = new List<LuauAnnotation>();
        public readonly List<LuauScriptReference> ScriptReferences = new List<LuauScriptReference>();
        public readonly List<NetworkCallingEntrypointMetadata> NetworkCallables = new List<NetworkCallingEntrypointMetadata>();

        /// <summary>Max events per second declared with @networkcallable(n) by entry point; 0 means the SDK default.</summary>
        public readonly Dictionary<string, int> NetworkRates = new Dictionary<string, int>();

        /// <summary>The script's public fields and methods.</summary>
        public ScriptInterface Interface = new ScriptInterface();

        /// <summary>The sync mode the script declares with @syncmode.</summary>
        public UdonSharp.BehaviourSyncMode SyncMode = UdonSharp.BehaviourSyncMode.Any;

        /// <summary>The compiler's readable listing of the program.</summary>
        public string Disassembly = "";

        /// <summary>Whether a program was produced.</summary>
        public bool Succeeded => Program != null;

        internal void Error(string message) => Diagnostics.Add(new LuauDiagnostic { line = 1, column = 1, message = message });
    }

    internal static class LuauCompiler
    {
        private const int MinimumHeapSize = 512;
        private const string TypeIdSymbol = "__refl_typeid";
        private const string TypeNameSymbol = "__refl_typename";

        /// <summary>Compiles Luau source against the editor's catalog and builds the Udon program.</summary>
        /// <param name="source">The Luau source.</param>
        /// <param name="scriptName">The script's name; when given, the program carries UdonSharp's type identity for the script's generated class.</param>
        public static LuauCompileResult Compile(string source, string scriptName = null)
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

            output.Interface = ScriptRegistry.ReadInterface(result, scriptName);
            foreach (ScriptVariable field in output.Interface.Fields)
                if (!string.IsNullOrEmpty(field.Script)) output.ScriptReferences.Add(new LuauScriptReference { symbol = field.Symbol, script = field.Script });
            if (Native.ul_result_sync_mode != null) output.SyncMode = (UdonSharp.BehaviourSyncMode)Native.ul_result_sync_mode(result);
            output.ModuleAnnotations.AddRange(ReadAnnotations(result, -1));
            output.Disassembly = Native.Read(Native.ul_result_disassembly(result)) ?? "";

            try
            {
                ReadNetworkCallables(catalog, result, output);
                string typeName = scriptName == null ? null : ProxyGenerator.UdonSharpTypeName(scriptName);
                output.Program = new ProgramBuilder(catalog, result, output, typeName).Build();
            }
            catch (Exception e)
            {
                output.Program = null;
                output.Error($"failed to build the Udon program: {e.Message}");
            }
            return output;
        }

        private static ResultHandle Invoke(LuauCatalog catalog, byte[] source, string defines)
        {
            IntPtr result = Native.ul_compile_with_defines != null
                ? Native.ul_compile_with_defines(catalog.Handle, source, (UIntPtr)source.Length, Native.Utf8(defines ?? ""))
                : Native.ul_compile(catalog.Handle, source, (UIntPtr)source.Length);
            return new ResultHandle(result);
        }

        private static void ReadNetworkCallables(LuauCatalog catalog, ResultHandle result, LuauCompileResult output)
        {
            if (Native.ul_result_network_count == null) return;
            int count = Native.ul_result_network_count(result);
            for (int i = 0; i < count; i++)
            {
                if (Native.ul_result_network(result, i, out NativeNetworkCallable callable) == 0) continue;
                var parameters = new NetworkCallingParameterMetadata[callable.ParameterCount];
                for (int j = 0; j < parameters.Length; j++)
                {
                    if (Native.ul_result_network_parameter(result, i, j, out NativeNetworkParameter p) == 0) throw new InvalidOperationException($"network parameter {j} is missing");
                    string typeName = Native.Read(p.Type);
                    Type type = catalog.ResolveType(typeName) ?? throw new InvalidOperationException($"unknown network parameter type '{typeName}'");
                    parameters[j] = new NetworkCallingParameterMetadata(Native.Read(p.Symbol), type);
                }
                var attribute = callable.MaxEventsPerSecond > 0 ? new NetworkCallableAttribute(callable.MaxEventsPerSecond) : new NetworkCallableAttribute();
                string entry = Native.Read(callable.EntryPoint);
                output.NetworkCallables.Add(new NetworkCallingEntrypointMetadata(entry, attribute, parameters));
                output.NetworkRates[entry] = callable.MaxEventsPerSecond;
            }
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

            private readonly string _reflectionTypeName;

            public ProgramBuilder(LuauCatalog catalog, ResultHandle result, LuauCompileResult output, string reflectionTypeName)
            {
                _catalog = catalog;
                _result = result;
                _output = output;
                _reflectionTypeName = reflectionTypeName;
            }

            public IUdonProgram Build()
            {
                IntPtr code = Native.ul_result_bytecode(_result, out UIntPtr length);
                var byteCode = new byte[(int)length];
                if (code != IntPtr.Zero) Marshal.Copy(code, byteCode, 0, byteCode.Length);

                int slotCount = Native.ul_result_heap_count(_result);
                var heap = new UdonHeap((uint)Math.Max(slotCount + 2, MinimumHeapSize));
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

                if (_reflectionTypeName != null && symbols.All(s => s.Name != TypeIdSymbol && s.Name != TypeNameSymbol))
                {
                    heap.SetHeapVariable((uint)slotCount, UdonSharp.Internal.UdonSharpInternalUtility.GetTypeID(_reflectionTypeName), typeof(long));
                    symbols.Add(new UdonSymbol(TypeIdSymbol, typeof(long), (uint)slotCount));
                    heap.SetHeapVariable((uint)slotCount + 1, _reflectionTypeName, typeof(string));
                    symbols.Add(new UdonSymbol(TypeNameSymbol, typeof(string), (uint)slotCount + 1));
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
