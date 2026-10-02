using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using UnityEditor;
using VRC.Udon;
using VRC.Udon.Common.Interfaces;
using VRC.Udon.Editor;
using Debug = UnityEngine.Debug;

namespace Magma.VRC.UdonLuau
{
    /// <summary>The extern, type and event catalog built from the Udon wrapper modules loaded in the editor.</summary>
    internal sealed class LuauCatalog
    {
        private const BindingFlags InstanceFields = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;

        private static LuauCatalog _current;
        private static bool _failed;

        private readonly Dictionary<string, Type> _types = new Dictionary<string, Type>();
        private readonly HashSet<string> _unresolved = new HashSet<string>();
        private readonly HashSet<string> _added = new HashSet<string>();

        internal CatalogHandle Handle { get; private set; }

        /// <summary>The wrapper whose extern delegates the editor uses.</summary>
        public IUdonWrapper Wrapper { get; private set; }

        /// <summary>Number of extern signatures added.</summary>
        public int ExternCount { get; private set; }

        /// <summary>Number of types added.</summary>
        public int TypeCount => _added.Count;

        /// <summary>Number of events read from the node definitions.</summary>
        public int EventCount { get; private set; }

        /// <summary>Milliseconds the build took.</summary>
        public long BuildMilliseconds { get; private set; }

        /// <summary>Returns the catalog for this editor session, building it on first use.</summary>
        /// <returns>Null when the native library could not be loaded.</returns>
        public static LuauCatalog Current
        {
            get
            {
                if (_current != null || _failed) return _current;
                try
                {
                    Native.Load();
                    _current = Build();
                }
                catch (Exception e) when (e is DllNotFoundException || e is EntryPointNotFoundException || e is BadImageFormatException || e is IOException)
                {
                    _failed = true;
                    Debug.LogError($"[UdonLuau] The native compiler could not be loaded: {e.Message}. UdonLuau scripts will not compile. Reinstall the UdonLuau package (or put UdonLuau.dll back in its Editor/Plugins/x86_64 folder), then use Tools > UdonLuau > Reload Native Compiler.");
                }
                return _current;
            }
        }

        /// <summary>Frees the native compiler and the catalog, then loads the package's UdonLuau.dll again.</summary>
        [MenuItem("Tools/UdonLuau/Reload Native Compiler", false, 20)]
        public static void Reload()
        {
            Native.Unload();
            _failed = false;
            if (Current != null) LuauEditorHooks.CompileAll();
        }

        static LuauCatalog()
        {
            Native.Unloading += () =>
            {
                ScriptRegistry.Reset();
                _current?.Handle.Dispose();
                _current = null;
            };
        }

        /// <summary>Resolves an Udon type name, such as UnityEngineTransform, to its System.Type.</summary>
        /// <returns>Null when no loaded type has that name.</returns>
        public Type ResolveType(string udonName)
        {
            if (string.IsNullOrEmpty(udonName)) return null;
            if (_types.TryGetValue(udonName, out Type type)) return type;
            if (_unresolved.Contains(udonName)) return null;

            try
            {
                type = UdonEditorManager.Instance.GetTypeFromTypeString(udonName);
            }
            catch
            {
                type = null;
            }

            if (type == null && udonName.EndsWith("Array", StringComparison.Ordinal))
                type = ResolveType(udonName.Substring(0, udonName.Length - 5))?.MakeArrayType();

            if (type == null)
            {
                _unresolved.Add(udonName);
                return null;
            }

            _types[udonName] = type;
            return type;
        }

        /// <summary>Splits the parameter or return part of an extern signature into Udon type names, rejoining names that contain underscores.</summary>
        public List<string> SplitTypeList(string text)
        {
            var names = new List<string>();
            string[] tokens = text.Split('_');
            for (int i = 0; i < tokens.Length;)
            {
                if (tokens[i].Length == 0)
                {
                    i++;
                    continue;
                }

                int end = i;
                string candidate = tokens[i];
                string found = null;
                for (int j = i; j < tokens.Length; j++)
                {
                    if (j > i) candidate += "_" + tokens[j];
                    if (ResolveType(candidate) == null) continue;
                    found = candidate;
                    end = j;
                    break;
                }

                names.Add(found ?? tokens[i]);
                i = end + 1;
            }
            return names;
        }

        /// <summary>Returns the parameter type names of an extern signature.</summary>
        public List<string> ParameterTypes(string signature)
        {
            string[] parts = MethodParts(signature);
            return parts.Length >= 4 ? SplitTypeList(parts[2]) : new List<string>();
        }

        private static string[] MethodParts(string signature)
        {
            int dot = signature.IndexOf('.');
            return signature.Substring(dot + 1).Split(new[] { "__" }, StringSplitOptions.None);
        }

        private static LuauCatalog Build()
        {
            var stopwatch = Stopwatch.StartNew();
            var catalog = new LuauCatalog { Handle = new CatalogHandle(Native.ul_catalog_create()) };
            catalog.Wrapper = UdonEditorManager.Instance.GetWrapper();

            Native.ul_catalog_add_standard_events(catalog.Handle);

            foreach (KeyValuePair<string, int> entry in EnumerateExterns(catalog.Wrapper))
            {
                if (Native.ul_catalog_add_extern(catalog.Handle, Native.Utf8(entry.Key), entry.Value) != 0) catalog.ExternCount++;
                catalog.AddSignatureTypes(entry.Key);
            }

            catalog.AddEvents();
            catalog.AddSyncableTypes();

            foreach (string name in new[] { "SystemObject", "SystemVoid", "SystemString", "SystemType", "SystemBoolean", "SystemInt32", "SystemSingle", "VRCUdonUdonBehaviour", "VRCUdonUdonBehaviourArray" })
                catalog.AddType(name);

            stopwatch.Stop();
            catalog.BuildMilliseconds = stopwatch.ElapsedMilliseconds;
            Debug.Log($"[UdonLuau] Catalog built: {catalog.ExternCount} externs, {catalog.TypeCount} types, {catalog.EventCount} events in {catalog.BuildMilliseconds} ms.");
            return catalog;
        }

        private static IEnumerable<KeyValuePair<string, int>> EnumerateExterns(IUdonWrapper wrapper)
        {
            if (wrapper.GetType().GetField("_wrapperModulesByName", InstanceFields)?.GetValue(wrapper) is IDictionary modules)
            {
                foreach (DictionaryEntry module in modules)
                {
                    if (!(ParameterCounts(module.Value) is Dictionary<string, int> counts)) continue;
                    string prefix = (string)module.Key + ".";
                    foreach (KeyValuePair<string, int> count in counts) yield return new KeyValuePair<string, int>(prefix + count.Key, count.Value);
                }
                yield break;
            }

            foreach (var definition in UdonEditorManager.Instance.GetNodeDefinitions())
            {
                string signature = definition.fullName;
                if (signature == null || !signature.Contains(".__")) continue;
                int count;
                try
                {
                    count = wrapper.GetExternFunctionParameterCount(signature);
                }
                catch
                {
                    continue;
                }
                yield return new KeyValuePair<string, int>(signature, count);
            }
        }

        private static Dictionary<string, int> ParameterCounts(object module)
        {
            for (Type type = module.GetType(); type != null; type = type.BaseType)
            {
                FieldInfo field = type.GetField("_parameterCounts", InstanceFields | BindingFlags.DeclaredOnly);
                if (field == null) continue;
                object value = field.GetValue(module);
                return value is Lazy<Dictionary<string, int>> lazy ? lazy.Value : value as Dictionary<string, int>;
            }
            return null;
        }

        private void AddSignatureTypes(string signature)
        {
            int dot = signature.IndexOf('.');
            if (dot <= 0) return;
            AddType(signature.Substring(0, dot));

            string[] parts = MethodParts(signature);
            for (int i = 2; i < parts.Length; i++)
            foreach (string name in SplitTypeList(parts[i]))
                AddType(name);
        }

        private void AddType(string udonName)
        {
            if (_added.Contains(udonName)) return;
            Type type = ResolveType(udonName);
            if (type != null) AddType(type, udonName);
        }

        private string AddType(Type type, string udonName = null)
        {
            if (type.IsByRef) return AddType(type.GetElementType());
            if (type.IsGenericParameter) return null;

            string canonical = UdonTypeNames.Get(type);
            udonName ??= canonical;
            if (!_added.Add(udonName)) return udonName;
            if (!_types.ContainsKey(udonName)) _types[udonName] = type;
            if (udonName != canonical) AddType(type);

            TypeKind kind = type.IsArray ? TypeKind.Array
                : type.IsEnum ? TypeKind.Enum
                : type.IsInterface ? TypeKind.Interface
                : type.IsValueType ? TypeKind.Struct
                : TypeKind.Class;

            string baseName = type.BaseType != null ? AddType(type.BaseType) : null;
            string element = type.IsArray ? AddType(type.GetElementType()) : null;

            var interfaces = new List<string>();
            foreach (Type contract in type.GetInterfaces())
            {
                string name = AddType(contract);
                if (name != null) interfaces.Add(name);
            }

            string fullName = type.IsGenericType || udonName != canonical ? null : type.FullName;
            Native.ul_catalog_add_type(Handle, Native.Utf8(udonName), Native.Utf8(fullName), (int)kind, Native.Utf8(baseName), Native.Utf8(string.Join(";", interfaces)), Native.Utf8(element));

            if (type.IsEnum)
            {
                bool unsigned = Enum.GetUnderlyingType(type) == typeof(ulong);
                byte[] enumName = Native.Utf8(udonName);
                foreach (FieldInfo field in type.GetFields(BindingFlags.Public | BindingFlags.Static))
                {
                    object raw = field.GetRawConstantValue();
                    long value = unsigned ? unchecked((long)Convert.ToUInt64(raw)) : Convert.ToInt64(raw);
                    Native.ul_catalog_add_enum_member(Handle, enumName, Native.Utf8(field.Name), value);
                }
            }

            return udonName;
        }

        private void AddSyncableTypes()
        {
            if (Native.ul_catalog_add_syncable_type == null) return;
            var field = typeof(UdonNetworkTypes).GetField("_syncTypes", BindingFlags.Static | BindingFlags.NonPublic);
            if (!(field?.GetValue(null) is IEnumerable<Type> types))
            {
                Debug.LogWarning("[UdonLuau] Could not read the SDK's syncable types; synced variable types will not be checked.");
                return;
            }

            foreach (Type type in types)
            {
                string name = AddType(type);
                if (name != null) Native.ul_catalog_add_syncable_type(Handle, Native.Utf8(name), UdonNetworkTypes.CanSyncLinear(type) ? 1 : 0, UdonNetworkTypes.CanSyncSmooth(type) ? 1 : 0);
            }
        }

        private void AddEvents()
        {
            foreach (var definition in UdonEditorManager.Instance.GetNodeDefinitions("Event_"))
            {
                string name = definition.fullName.Substring("Event_".Length);
                if (name.StartsWith("Custom", StringComparison.Ordinal) || name == "OnVariableChange") continue;

                var outputs = definition.Outputs;
                var names = new IntPtr[outputs.Count];
                var types = new IntPtr[outputs.Count];
                try
                {
                    for (int i = 0; i < outputs.Count; i++)
                    {
                        Type type = outputs[i].type ?? typeof(object);
                        names[i] = Marshal.StringToCoTaskMemUTF8(outputs[i].name ?? $"parameter{i}");
                        types[i] = Marshal.StringToCoTaskMemUTF8(AddType(type) ?? UdonTypeNames.Get(type));
                    }
                    Native.ul_catalog_add_event(Handle, Native.Utf8(name), names, types, outputs.Count);
                    EventCount++;
                }
                finally
                {
                    foreach (IntPtr p in names) Marshal.FreeCoTaskMem(p);
                    foreach (IntPtr p in types) Marshal.FreeCoTaskMem(p);
                }
            }
        }
    }
}
