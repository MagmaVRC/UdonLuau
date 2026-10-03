using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using UnityEditor;

namespace Magma.VRC.UdonLuau
{
    internal sealed class ScriptVariable
    {
        public string Name;
        public string UdonType;
        public string Script;
        public string Symbol;
    }

    internal sealed class ScriptMethod
    {
        public string Name;
        public string EntryPoint;
        public bool NetworkCallable;
        public readonly List<ScriptVariable> Parameters = new List<ScriptVariable>();
        public readonly List<ScriptVariable> Returns = new List<ScriptVariable>();
    }

    internal sealed class ScriptInterface
    {
        public string Name;
        public bool Singleton;
        public readonly List<ScriptVariable> Fields = new List<ScriptVariable>();
        public readonly List<ScriptMethod> Methods = new List<ScriptMethod>();

        /// <summary>A stable text form used to detect interface changes.</summary>
        public override string ToString()
        {
            var text = new StringBuilder(Name).Append(Singleton ? "!" : "").Append('{');
            foreach (ScriptVariable f in Fields) Append(text.Append("f:"), f);
            foreach (ScriptMethod m in Methods)
            {
                text.Append("m:").Append(m.Name).Append('=').Append(m.EntryPoint).Append(m.NetworkCallable ? "!" : "").Append('(');
                foreach (ScriptVariable p in m.Parameters) Append(text, p);
                text.Append(")->(");
                foreach (ScriptVariable r in m.Returns) Append(text, r);
                text.Append(')');
            }
            return text.Append('}').ToString();
        }

        private static void Append(StringBuilder text, ScriptVariable v) =>
            text.Append(v.Name).Append(':').Append(v.UdonType).Append(':').Append(v.Script).Append(':').Append(v.Symbol).Append(';');
    }

    internal static class ScriptRegistry
    {
        private sealed class CachedInterface
        {
            public string Key;
            public ScriptInterface Interface;
            public ScriptInterface Statics;
        }

        private static readonly Dictionary<string, CachedInterface> LuauCache = new Dictionary<string, CachedInterface>();
        private static readonly Dictionary<LuauProgramAsset, string> ConflictMessages = new Dictionary<LuauProgramAsset, string>();
        private static string _fingerprint;
        private static int _batchDepth;
        private static bool _batchRefreshed;
        private static bool _recompileScheduled;

        internal static bool CompilingAll { get; set; }

        /// <summary>The script name of a Luau source: its file name without extension.</summary>
        public static string ScriptName(UnityEngine.Object source) =>
            source == null ? null : Path.GetFileNameWithoutExtension(AssetDatabase.GetAssetPath(source));

        /// <summary>The name of the companion singleton holding a script's static fields and functions.</summary>
        public static string StaticName(string scriptName) => scriptName + ".Static";

        /// <summary>The public interface last read from a program's script, even when its function bodies do not compile, or null.</summary>
        public static ScriptInterface InterfaceFor(LuauProgramAsset asset)
        {
            if (asset == null || asset.SourceScript == null) return null;
            if (!LuauCache.TryGetValue(AssetDatabase.GetAssetPath(asset.SourceScript), out CachedInterface cached)) return null;
            return asset.StaticPart ? cached.Statics : cached.Interface;
        }

        /// <summary>Returns why the program's script name cannot be used, or null.</summary>
        public static string Conflict(LuauProgramAsset asset) => ConflictMessages.TryGetValue(asset, out string message) ? message : null;

        /// <summary>Runs an action with the registry refreshed once for all compilations inside it.</summary>
        public static void Batch(Action action)
        {
            _batchDepth++;
            try
            {
                action();
            }
            finally
            {
                if (--_batchDepth == 0) _batchRefreshed = false;
            }
        }

        /// <summary>Refreshes the registry unless a batch already did.</summary>
        public static void EnsureCurrent()
        {
            if (_batchDepth > 0 && _batchRefreshed) return;
            Refresh();
            if (_batchDepth > 0) _batchRefreshed = true;
        }

        /// <summary>Forgets cached interfaces; used when the native compiler is unloaded.</summary>
        public static void Reset()
        {
            LuauCache.Clear();
            ConflictMessages.Clear();
            _fingerprint = null;
            _batchRefreshed = false;
        }

        /// <summary>Re-registers every script in the catalog.</summary>
        /// <returns>Whether any script's interface changed since the last refresh.</returns>
        public static bool Refresh()
        {
            LuauCatalog catalog = LuauCatalog.Current;
            if (catalog == null || Native.ul_catalog_add_script == null) return false;

            var luau = LuauProgramAsset.FindAll()
                .Where(a => a.SourceScript != null)
                .GroupBy(a => AssetDatabase.GetAssetPath(a.SourceScript))
                .ToList();
            var sharp = UdonSharpScripts.FindAll();

            var owners = new Dictionary<string, List<string>>(StringComparer.Ordinal);
            void Own(string name, string owner)
            {
                if (!owners.TryGetValue(name, out var list)) owners[name] = list = new List<string>();
                list.Add(owner);
            }
            foreach (var group in luau) Own(Path.GetFileNameWithoutExtension(group.Key), group.Key);
            foreach (var entry in sharp) Own(entry.Key, $"UdonSharp class {entry.Value.FullName}");

            ConflictMessages.Clear();
            foreach (var group in luau)
            {
                string name = Path.GetFileNameWithoutExtension(group.Key);
                if (owners[name].Count < 2) continue;
                string message = $"script name '{name}' is used by more than one script: {string.Join(", ", owners[name])}";
                foreach (LuauProgramAsset asset in group) ConflictMessages[asset] = message;
            }

            var names = owners.Where(o => o.Value.Count == 1).Select(o => o.Key).OrderBy(n => n, StringComparer.Ordinal).ToList();
            foreach (string name in names) Native.ul_catalog_add_script(catalog.Handle, Native.Utf8(name));
            var registered = new HashSet<string>(names, StringComparer.Ordinal);
            if (UdonSharpScripts.RegisteredNames == null || !UdonSharpScripts.RegisteredNames.SetEquals(registered)) UdonSharpScripts.ClearCache();
            UdonSharpScripts.RegisteredNames = registered;

            string defines = LuauSettings.instance.Defines ?? "";
            string context = defines + "|" + string.Join(",", names);
            var interfaces = new List<ScriptInterface>();

            foreach (var group in luau)
            {
                string name = Path.GetFileNameWithoutExtension(group.Key);
                if (owners[name].Count != 1) continue;
                string source = group.First().SourceScript.text;
                string key = Hash(source) + "|" + context;
                if (!LuauCache.TryGetValue(group.Key, out CachedInterface cached) || cached.Key != key)
                {
                    var (main, statics) = Extract(catalog, name, source, defines);
                    cached = new CachedInterface { Key = key, Interface = main, Statics = statics };
                    LuauCache[group.Key] = cached;
                }
                interfaces.Add(cached.Interface);
                if (cached.Statics != null) interfaces.Add(cached.Statics);
            }

            foreach (var entry in sharp)
            {
                if (owners[entry.Key].Count != 1) continue;
                ScriptInterface description = UdonSharpScripts.Describe(entry.Key, entry.Value);
                if (description != null) interfaces.Add(description);
            }

            foreach (ScriptInterface script in interfaces) Register(catalog, script);

            string fingerprint = string.Join("\n", interfaces.Select(i => i.ToString()).OrderBy(s => s, StringComparer.Ordinal));
            bool changed = _fingerprint != null && _fingerprint != fingerprint;
            bool first = _fingerprint == null;
            _fingerprint = fingerprint;
            if (first || changed) EditorSetup.Write();
            if (changed && !CompilingAll && !_recompileScheduled)
            {
                _recompileScheduled = true;
                EditorApplication.delayCall += () =>
                {
                    _recompileScheduled = false;
                    if (!EditorApplication.isPlayingOrWillChangePlaymode) LuauEditorHooks.CompileAll();
                };
            }
            return changed;
        }

        private static (ScriptInterface main, ScriptInterface statics) Extract(LuauCatalog catalog, string name, string source, string defines)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(source);
            if (Native.ul_extract_interface_part == null)
            {
                using var plain = new ResultHandle(Native.ul_extract_interface(catalog.Handle, bytes, (UIntPtr)bytes.Length, Native.Utf8(defines)));
                return (ReadInterface(plain, name), null);
            }

            using var result = new ResultHandle(Native.ul_extract_interface_part(catalog.Handle, bytes, (UIntPtr)bytes.Length, Native.Utf8(defines), Native.Utf8(name), 0));
            ScriptInterface main = ReadInterface(result, name);
            if (Native.ul_result_has_statics == null || Native.ul_result_has_statics(result) == 0) return (main, null);
            using var part = new ResultHandle(Native.ul_extract_interface_part(catalog.Handle, bytes, (UIntPtr)bytes.Length, Native.Utf8(defines), Native.Utf8(name), 1));
            return (main, ReadInterface(part, StaticName(name)));
        }

        internal static ScriptInterface ReadInterface(ResultHandle result, string name)
        {
            var script = new ScriptInterface { Name = name };
            if (result.IsInvalid || Native.ul_result_has_interface == null || Native.ul_result_has_interface(result) == 0) return script;
            script.Singleton = Native.ul_result_interface_singleton != null && Native.ul_result_interface_singleton(result) != 0;

            int fieldCount = Native.ul_result_interface_field_count(result);
            for (int i = 0; i < fieldCount; i++)
                if (Native.ul_result_interface_field(result, i, out NativeScriptVariable field) != 0) script.Fields.Add(Read(field));

            int methodCount = Native.ul_result_interface_method_count(result);
            for (int i = 0; i < methodCount; i++)
            {
                if (Native.ul_result_interface_method(result, i, out NativeScriptMethod m) == 0) continue;
                var method = new ScriptMethod
                {
                    Name = Native.Read(m.Name),
                    EntryPoint = Native.Read(m.EntryPoint),
                    NetworkCallable = Native.ul_result_interface_method_network_callable != null && Native.ul_result_interface_method_network_callable(result, i) != 0,
                };
                for (int j = 0; j < m.ParameterCount; j++)
                    if (Native.ul_result_interface_method_value(result, i, 0, j, out NativeScriptVariable v) != 0) method.Parameters.Add(Read(v));
                for (int j = 0; j < m.ReturnCount; j++)
                    if (Native.ul_result_interface_method_value(result, i, 1, j, out NativeScriptVariable v) != 0) method.Returns.Add(Read(v));
                script.Methods.Add(method);
            }
            return script;
        }

        private static ScriptVariable Read(NativeScriptVariable v) => new ScriptVariable
        {
            Name = Native.Read(v.Name),
            UdonType = Native.Read(v.Type),
            Script = Native.Read(v.Script),
            Symbol = Native.Read(v.Symbol),
        };

        private static void Register(LuauCatalog catalog, ScriptInterface script)
        {
            byte[] name = Native.Utf8(script.Name);
            Native.ul_catalog_add_script(catalog.Handle, name);
            if (script.Singleton) Native.ul_catalog_set_script_singleton?.Invoke(catalog.Handle, name, 1);
            foreach (ScriptVariable f in script.Fields)
                Native.ul_catalog_add_script_field(catalog.Handle, name, Native.Utf8(f.Name), Native.Utf8(f.UdonType), Native.Utf8(f.Script), Native.Utf8(f.Symbol));
            foreach (ScriptMethod m in script.Methods)
            {
                byte[] method = Native.Utf8(m.Name);
                Native.ul_catalog_add_script_method(catalog.Handle, name, method, Native.Utf8(m.EntryPoint));
                foreach (ScriptVariable p in m.Parameters)
                    Native.ul_catalog_add_script_method_value(catalog.Handle, name, method, 0, Native.Utf8(p.Name), Native.Utf8(p.UdonType), Native.Utf8(p.Script), Native.Utf8(p.Symbol));
                foreach (ScriptVariable r in m.Returns)
                    Native.ul_catalog_add_script_method_value(catalog.Handle, name, method, 1, Native.Utf8(r.Name), Native.Utf8(r.UdonType), Native.Utf8(r.Script), Native.Utf8(r.Symbol));
                if (m.NetworkCallable) Native.ul_catalog_set_script_method_network_callable?.Invoke(catalog.Handle, name, method, 1);
            }
        }

        private static string Hash(string text)
        {
            using var sha = SHA256.Create();
            return Convert.ToBase64String(sha.ComputeHash(Encoding.UTF8.GetBytes(text)));
        }
    }
}
