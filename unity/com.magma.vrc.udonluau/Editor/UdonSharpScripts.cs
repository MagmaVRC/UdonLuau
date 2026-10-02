using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using UdonSharp;
using UnityEditor;
using VRC.SDK3.UdonNetworkCalling;
using VRC.Udon;
using VRC.Udon.Common.Interfaces;
using VRC.Udon.Editor;
using VRC.Udon.Editor.ProgramSources;

namespace Magma.VRC.UdonLuau
{
    /// <summary>Describes UdonSharp behaviours as scripts, using the method layout UdonSharp compiled them with.</summary>
    public static class UdonSharpScripts
    {
        private const BindingFlags Declared = BindingFlags.DeclaredOnly | BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;

        private static readonly FieldInfo SerializedProgramField = typeof(UdonProgramAsset).GetField("serializedUdonProgramAsset", BindingFlags.Instance | BindingFlags.NonPublic);
        private static readonly FieldInfo NetworkMetadataField = typeof(UdonSharpProgramAsset).GetField("networkCallingMetadata", BindingFlags.Instance | BindingFlags.NonPublic);
        private static readonly Dictionary<Type, UdonSharpProgramAsset> Assets = new Dictionary<Type, UdonSharpProgramAsset>();
        private static readonly Dictionary<Type, (string key, ScriptInterface script)> Cache = new Dictionary<Type, (string, ScriptInterface)>();
        private static Dictionary<string, Type[]> _events;

        internal static HashSet<string> RegisteredNames { get; set; }

        internal static void ClearCache() => Cache.Clear();

        private sealed class Layout
        {
            public string ExportName;
            public string ReturnName;
            public string[] ParameterNames;
            public readonly List<string> Keys = new List<string>();
        }

        /// <summary>Every UdonSharp class with a program asset, by class name.</summary>
        public static List<KeyValuePair<string, Type>> FindAll()
        {
            Assets.Clear();
            var list = new List<KeyValuePair<string, Type>>();
            foreach (UdonSharpProgramAsset asset in UdonSharpProgramAsset.GetAllUdonSharpPrograms())
            {
                Type type = asset != null && asset.sourceCsScript != null ? asset.sourceCsScript.GetClass() : null;
                if (type == null || !typeof(UdonSharpBehaviour).IsAssignableFrom(type) || Assets.ContainsKey(type)) continue;
                Assets[type] = asset;
                list.Add(new KeyValuePair<string, Type>(type.Name, type));
            }
            return list;
        }

        /// <summary>Whether a program source is the UdonSharp program of the named class.</summary>
        public static bool IsProgramOf(AbstractUdonProgramSource source, string script) =>
            source is UdonSharpProgramAsset asset && asset.sourceCsScript != null && asset.sourceCsScript.GetClass()?.Name == script;

        /// <summary>Describes a class's public fields and methods. Methods whose compiled layout cannot be confirmed are left out.</summary>
        public static ScriptInterface Describe(string name, Type type)
        {
            if (!Assets.TryGetValue(type, out UdonSharpProgramAsset asset)) return null;
            var serialized = SerializedProgramField?.GetValue(asset) as AbstractSerializedUdonProgramAsset;
            string serializedPath = serialized != null ? AssetDatabase.GetAssetPath(serialized) : null;
            string key = serializedPath == null ? "" : File.GetLastWriteTimeUtc(serializedPath).Ticks.ToString();
            if (Cache.TryGetValue(type, out var cached) && cached.key == key) return cached.script;

            var script = new ScriptInterface { Name = name };
            IUdonProgram program = serialized != null ? serialized.RetrieveProgram() : null;
            if (program != null) Fill(script, type, program);

            var callable = new HashSet<string>((NetworkMetadataField?.GetValue(asset) as NetworkCallingEntrypointMetadata[] ?? Array.Empty<NetworkCallingEntrypointMetadata>()).Select(m => m.Name));
            foreach (ScriptMethod method in script.Methods) method.NetworkCallable = callable.Contains(method.EntryPoint);
            Cache[type] = (key, script);
            return script;
        }

        private static void Fill(ScriptInterface script, Type type, IUdonProgram program)
        {
            IUdonSymbolTable symbols = program.SymbolTable;
            var entries = new HashSet<string>(program.EntryPoints.GetExportedSymbols());

            var chain = new List<Type>();
            for (Type t = type; t != null && t != typeof(UdonSharpBehaviour); t = t.BaseType) chain.Insert(0, t);

            var counters = new Dictionary<string, int>(StringComparer.Ordinal);
            var keyUses = new Dictionary<string, int>(StringComparer.Ordinal);
            var layouts = new Dictionary<MethodInfo, Layout>();
            var produced = new HashSet<string>(StringComparer.Ordinal);

            foreach (Type declaring in chain)
            {
                foreach (MethodInfo method in declaring.GetMethods(Declared).OrderBy(m => m.MetadataToken))
                {
                    if (method.Name.IndexOf('<') >= 0 || method.IsDefined(typeof(CompilerGeneratedAttribute), false) && !method.IsSpecialName) continue;
                    MethodInfo overridden = Overridden(method);
                    if (overridden != null && IsUserType(overridden.DeclaringType)) continue;
                    Layout layout = BuildLayout(method, counters);
                    layouts[method] = layout;
                    foreach (string k in layout.Keys) keyUses[k] = keyUses.TryGetValue(k, out int n) ? n + 1 : 1;
                    foreach (string symbol in layout.ParameterNames.Append(layout.ReturnName))
                        if (symbol != null && symbol.StartsWith("__", StringComparison.Ordinal)) produced.Add(symbol);
                }
            }

            var compiled = new HashSet<string>(symbols.GetSymbols().Where(IsLayoutSymbol), StringComparer.Ordinal);
            bool exact = compiled.SetEquals(produced);

            foreach (FieldInfo field in type.GetFields(BindingFlags.Instance | BindingFlags.Public))
            {
                if (!IsUserType(field.DeclaringType) || !symbols.HasAddressForSymbol(field.Name)) continue;
                script.Fields.Add(Variable(field.Name, field.FieldType, symbols, field.Name));
            }

            var publicMethods = type.GetMethods(BindingFlags.Instance | BindingFlags.Public)
                .Where(m => IsUserType(m.DeclaringType) && !m.IsSpecialName && !m.IsGenericMethod && !IsEvent(m))
                .ToList();
            var overloaded = new HashSet<string>(publicMethods.GroupBy(m => m.Name).Where(g => g.Count() > 1).Select(g => g.Key));

            foreach (MethodInfo method in publicMethods)
            {
                if (overloaded.Contains(method.Name)) continue;
                MethodInfo root = method;
                for (MethodInfo up = Overridden(root); up != null && IsUserType(up.DeclaringType); up = Overridden(up)) root = up;
                if (!layouts.TryGetValue(root, out Layout layout)) continue;
                if (!exact && layout.Keys.Any(k => keyUses[k] != 1)) continue;
                if (!entries.Contains(layout.ExportName)) continue;

                ParameterInfo[] parameters = method.GetParameters();
                if (parameters.Any(p => p.ParameterType.IsByRef)) continue;
                if (layout.ParameterNames.Any(s => !symbols.HasAddressForSymbol(s))) continue;
                if (layout.ReturnName != null && !symbols.HasAddressForSymbol(layout.ReturnName)) continue;

                var described = new ScriptMethod { Name = method.Name, EntryPoint = layout.ExportName };
                for (int i = 0; i < parameters.Length; i++)
                    described.Parameters.Add(Variable(parameters[i].Name, parameters[i].ParameterType, symbols, layout.ParameterNames[i]));
                if (layout.ReturnName != null)
                    described.Returns.Add(Variable("result", method.ReturnType, symbols, layout.ReturnName));
                script.Methods.Add(described);
            }
        }

        private static Layout BuildLayout(MethodInfo method, Dictionary<string, int> counters)
        {
            ParameterInfo[] parameters = method.GetParameters();
            var layout = new Layout { ExportName = method.Name, ParameterNames = new string[parameters.Length] };

            string Unique(string id)
            {
                counters.TryGetValue(id, out int found);
                counters[id] = found + 1;
                layout.Keys.Add(id);
                return $"__{found}_{id}";
            }

            if (IsEvent(method))
            {
                string lowered = char.ToLowerInvariant(method.Name[0]) + method.Name.Substring(1);
                layout.ExportName = "_" + lowered;
                for (int i = 0; i < parameters.Length; i++) layout.ParameterNames[i] = null;
            }
            else
            {
                bool networkCallable = method.GetCustomAttributes(true).Any(a => a.GetType().Name == "NetworkCallableAttribute");
                if (!networkCallable && parameters.Length > 0) layout.ExportName = Unique(method.Name);
                for (int i = 0; i < parameters.Length; i++) layout.ParameterNames[i] = Unique(parameters[i].Name + "__param");
            }

            if (method.ReturnType != typeof(void)) layout.ReturnName = Unique(layout.ExportName + "__ret");
            return layout;
        }

        private static bool IsLayoutSymbol(string symbol) =>
            symbol.StartsWith("__", StringComparison.Ordinal) && (symbol.EndsWith("__param", StringComparison.Ordinal) || symbol.EndsWith("__ret", StringComparison.Ordinal))
            && symbol.Length > 3 && char.IsDigit(symbol[2]);

        private static ScriptVariable Variable(string name, Type type, IUdonSymbolTable symbols, string symbol)
        {
            Type element = type.IsArray ? type.GetElementType() : type;
            return new ScriptVariable
            {
                Name = name,
                UdonType = UdonTypeNames.Get(symbols.GetSymbolType(symbol)),
                Script = Assets.ContainsKey(element) && (RegisteredNames == null || RegisteredNames.Contains(element.Name)) ? element.Name : null,
                Symbol = symbol,
            };
        }

        private static bool IsUserType(Type type) => type != null && type != typeof(UdonSharpBehaviour) && typeof(UdonSharpBehaviour).IsAssignableFrom(type);

        private static MethodInfo Overridden(MethodInfo method)
        {
            if (!method.IsVirtual || method.GetBaseDefinition() == method) return null;
            Type[] types = method.GetParameters().Select(p => p.ParameterType).ToArray();
            for (Type t = method.DeclaringType.BaseType; t != null; t = t.BaseType)
            {
                MethodInfo candidate = t.GetMethod(method.Name, Declared, null, types, null);
                if (candidate != null && candidate.IsVirtual) return candidate;
            }
            return null;
        }

        private static bool IsEvent(MethodInfo method)
        {
            if (_events == null)
            {
                _events = new Dictionary<string, Type[]>(StringComparer.Ordinal);
                foreach (var definition in UdonEditorManager.Instance.GetNodeDefinitions("Event_"))
                {
                    if (definition.fullName == "Event_Custom") continue;
                    string eventName = definition.fullName.Substring(6);
                    if (!_events.ContainsKey(eventName)) _events[eventName] = definition.Outputs.Select(o => o.type).ToArray();
                }
            }

            if (!_events.TryGetValue(method.Name, out Type[] arguments)) return false;
            ParameterInfo[] parameters = method.GetParameters();
            if (parameters.Any(p => p.ParameterType.IsByRef)) return false;
            return parameters.Length == 0 || parameters.Select(p => p.ParameterType).SequenceEqual(arguments);
        }
    }
}
