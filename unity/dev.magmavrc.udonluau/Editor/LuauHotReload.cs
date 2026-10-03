using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using UnityEditor;
using VRC.Udon;
using VRC.Udon.Common.Interfaces;
using VRC.Udon.ProgramSources;
using Debug = UnityEngine.Debug;
using Object = UnityEngine.Object;

namespace Magma.VRC.UdonLuau
{
    /// <summary>Swaps edited Luau programs into running UdonBehaviours in play mode, keeping their variables, when the script's public interface is unchanged.</summary>
    [InitializeOnLoad]
    internal static class LuauHotReload
    {
        private const BindingFlags Instance = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;

        private static readonly FieldInfo ProgramField = typeof(UdonBehaviour).GetField("_program", Instance);
        private static readonly FieldInfo VmField = typeof(UdonBehaviour).GetField("_udonVM", Instance);
        private static readonly FieldInfo ManagerField = typeof(UdonBehaviour).GetField("_udonManager", Instance);
        private static readonly FieldInfo InitializedField = typeof(UdonBehaviour).GetField("_initialized", Instance);
        private static readonly MethodInfo ResolveReferences = typeof(UdonBehaviour).GetMethod("ResolveUdonHeapReferences", Instance);
        private static readonly MethodInfo ProcessEntryPoints = typeof(UdonBehaviour).GetMethod("ProcessEntryPoints", Instance);

        private static readonly ConcurrentQueue<(string path, long ticks)> Changes = new ConcurrentQueue<(string, long)>();
        private static readonly List<FileSystemWatcher> Watchers = new List<FileSystemWatcher>();

        static LuauHotReload()
        {
            EditorApplication.playModeStateChanged += state =>
            {
                if (state == PlayModeStateChange.EnteredPlayMode) Start();
                else if (state == PlayModeStateChange.ExitingPlayMode) Stop();
            };
            AssemblyReloadEvents.beforeAssemblyReload += Stop;
            EditorApplication.update += Pump;
            if (EditorApplication.isPlaying) Start();
        }

        /// <summary>Whether the reflection hooks into UdonBehaviour this feature needs were found.</summary>
        public static bool Supported => ProgramField != null && VmField != null && ManagerField != null && InitializedField != null && ResolveReferences != null && ProcessEntryPoints != null;

        private static void Start()
        {
            Stop();
            foreach (string filter in new[] { "*.luau", "*.lua" })
            {
                var watcher = new FileSystemWatcher(Path.GetFullPath("Assets"), filter) { IncludeSubdirectories = true, NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.FileName };
                watcher.Changed += (_, e) => Changes.Enqueue((e.FullPath, Stopwatch.GetTimestamp()));
                watcher.Created += (_, e) => Changes.Enqueue((e.FullPath, Stopwatch.GetTimestamp()));
                watcher.Renamed += (_, e) => Changes.Enqueue((e.FullPath, Stopwatch.GetTimestamp()));
                watcher.EnableRaisingEvents = true;
                Watchers.Add(watcher);
            }
        }

        private static void Stop()
        {
            foreach (FileSystemWatcher watcher in Watchers) watcher.Dispose();
            Watchers.Clear();
            while (Changes.TryDequeue(out _))
            {
            }
        }

        private static void Pump()
        {
            if (Changes.IsEmpty || !EditorApplication.isPlaying) return;
            var latest = new Dictionary<string, long>(StringComparer.OrdinalIgnoreCase);
            while (Changes.TryDequeue(out var change)) latest[change.path] = Math.Min(latest.TryGetValue(change.path, out long t) ? t : long.MaxValue, change.ticks);

            string root = Path.GetFullPath(".").Replace('\\', '/').TrimEnd('/') + "/";
            foreach (var pair in latest)
            {
                string path = pair.Key.Replace('\\', '/');
                if (path.StartsWith(root, StringComparison.OrdinalIgnoreCase)) path = path.Substring(root.Length);
                if (!LuauSettings.IsScriptPath(path)) continue;
                var script = AssetDatabase.LoadAssetAtPath<UnityEngine.TextAsset>(path);
                LuauProgramAsset asset = LuauProgramAsset.ForScript(script);
                if (asset == null) continue;

                string source;
                try
                {
                    source = File.ReadAllText(path);
                }
                catch (IOException)
                {
                    Changes.Enqueue((pair.Key, pair.Value));
                    continue;
                }
                LuauProgramAsset statics = LuauProgramAsset.StaticsForScript(script);
                if (statics != null) Reload(statics, source, pair.Value);
                Reload(asset, source, pair.Value);
            }
        }

        /// <summary>Compiles new source for a script and swaps it into every running UdonBehaviour that uses it.</summary>
        /// <returns>The number of behaviours updated, or -1 when the change needs a restart of play mode.</returns>
        public static int Reload(LuauProgramAsset asset, string source, long changedAt = 0)
        {
            if (!Supported)
            {
                Debug.LogWarning("[UdonLuau] Hot reload is not available with this VRChat SDK.");
                return -1;
            }

            LuauCompileResult result = asset.CompileForHotReload(source);
            string path = AssetDatabase.GetAssetPath(asset.SourceScript);
            if (!result.Succeeded)
            {
                foreach (LuauDiagnostic d in result.Diagnostics.Where(d => !d.isWarning))
                    Debug.LogError($"[UdonLuau] {path}({d.line},{d.column}): error: {d.message}");
                return -1;
            }

            if (result.Interface.ToString() != asset.InterfaceText)
            {
                Debug.LogWarning($"[UdonLuau] {path}: the script's public variables or methods changed; exit and re-enter play mode to apply it.");
                return -1;
            }

            var holder = UnityEngine.ScriptableObject.CreateInstance<SerializedUdonProgramAsset>();
            holder.StoreProgram(result.Program, result.NetworkCallables.Count > 0 ? result.NetworkCallables.ToArray() : null);

            int count = 0;
            foreach (UdonBehaviour behaviour in Object.FindObjectsByType<UdonBehaviour>(UnityEngine.FindObjectsInactive.Include, UnityEngine.FindObjectsSortMode.None))
            {
                if (behaviour.programSource != asset || !(bool)InitializedField.GetValue(behaviour)) continue;
                if (Swap(behaviour, holder.RetrieveProgram())) count++;
            }
            Object.DestroyImmediate(holder);

            double ms = changedAt == 0 ? 0 : (Stopwatch.GetTimestamp() - changedAt) * 1000.0 / Stopwatch.Frequency;
            Debug.Log($"[UdonLuau] Hot reloaded {path} into {count} behaviour(s)" + (changedAt == 0 ? "." : $" {ms:0} ms after the file changed."));
            return count;
        }

        private static bool Swap(UdonBehaviour behaviour, IUdonProgram program)
        {
            var old = (IUdonProgram)ProgramField.GetValue(behaviour);
            var vm = (IUdonVM)VmField.GetValue(behaviour);
            if (old == null || vm == null || program == null) return false;

            foreach (string symbol in program.SymbolTable.GetSymbols())
            {
                if (symbol.StartsWith("__", StringComparison.Ordinal) || !old.SymbolTable.HasAddressForSymbol(symbol)) continue;
                Type type = program.SymbolTable.GetSymbolType(symbol);
                if (old.SymbolTable.GetSymbolType(symbol) != type) continue;
                object value = old.Heap.GetHeapVariable(old.SymbolTable.GetAddressFromSymbol(symbol));
                if (value != null && !type.IsInstanceOfType(value)) continue;
                program.Heap.SetHeapVariable(program.SymbolTable.GetAddressFromSymbol(symbol), value, type);
            }

            object manager = ManagerField.GetValue(behaviour);
            manager?.GetType().GetMethod("ProcessUdonProgram", BindingFlags.Instance | BindingFlags.Public)?.Invoke(manager, new object[] { program });
            if (!(bool)ResolveReferences.Invoke(behaviour, new object[] { program.SymbolTable, program.Heap })) return false;

            ProgramField.SetValue(behaviour, program);
            vm.LoadProgram(program);
            ProcessEntryPoints.Invoke(behaviour, null);
            return true;
        }
    }
}
