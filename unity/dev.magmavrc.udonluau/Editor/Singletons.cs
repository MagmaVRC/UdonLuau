using System.Collections.Generic;
using System.Linq;
using UnityEditor;
using UnityEditor.Callbacks;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;
using VRC.Udon;
using VRC.Udon.Common;
using VRC.Udon.Common.Interfaces;

namespace Magma.VRC.UdonLuau
{
    /// <summary>Keeps one object for each singleton script the scene uses under a fixed root, and wires every script that uses a singleton to it when the scene is built or played.</summary>
    internal static class Singletons
    {
        /// <summary>The name of the root object holding the singletons. Scripts spawned at run time find a singleton by the path "/" + RootName + "/" + script name.</summary>
        public const string RootName = "__UdonLuauSingletons";

        private const string ReferencePrefix = "__singleton_";
        private const string ReadySymbol = "__singletons_ready";
        private const string StartedSymbol = "__started";

        /// <summary>The names of the singleton scripts a program uses.</summary>
        public static IEnumerable<string> UsedBy(IUdonProgram program) =>
            program?.SymbolTable == null
                ? Enumerable.Empty<string>()
                : program.SymbolTable.GetExportedSymbols().Where(s => s.StartsWith(ReferencePrefix)).Select(s => s.Substring(ReferencePrefix.Length));

        /// <summary>Whether a program asset is a singleton script.</summary>
        public static bool IsSingleton(LuauProgramAsset asset) => ProgramOf(asset)?.SymbolTable?.HasExportedSymbol(StartedSymbol) == true;

        /// <summary>Creates the objects for the singletons the open scenes use, and warns about singleton scripts placed elsewhere.</summary>
        public static void EnsureOpenScenes()
        {
            for (int i = 0; i < SceneManager.sceneCount; i++) Ensure(SceneManager.GetSceneAt(i));
        }

        private static void Ensure(Scene scene)
        {
            if (!scene.IsValid() || !scene.isLoaded) return;
            List<UdonBehaviour> behaviours = Behaviours(scene);
            var needed = new SortedSet<string>(behaviours.SelectMany(b => UsedBy(ProgramOf(b))));
            if (needed.Count == 0) return;

            Transform root = Root(scene, true);
            List<LuauProgramAsset> assets = LuauProgramAsset.FindAll().ToList();
            foreach (string name in needed)
            {
                LuauProgramAsset asset = assets.FirstOrDefault(a => a.ScriptName == name && IsSingleton(a));
                if (asset == null) continue;
                Transform holder = root.Find(name);
                if (holder == null)
                {
                    var created = new GameObject(name);
                    Undo.RegisterCreatedObjectUndo(created, "Create UdonLuau Singleton");
                    created.transform.SetParent(root, false);
                    holder = created.transform;
                }
                if (!holder.GetComponents<UdonBehaviour>().Any(b => b.programSource == asset)) Attach(holder.gameObject, asset);
            }

            foreach (UdonBehaviour behaviour in behaviours)
            {
                if (!(behaviour.programSource is LuauProgramAsset asset) || !IsSingleton(asset)) continue;
                if (behaviour.transform.parent == root && behaviour.gameObject.name == asset.ScriptName) continue;
                Debug.LogWarning($"[UdonLuau] '{behaviour.gameObject.name}' runs the {asset.ScriptName} singleton outside {RootName}; other scripts only use the one under {RootName}.", behaviour);
            }
        }

        private static void Attach(GameObject gameObject, LuauProgramAsset asset)
        {
            if (ProxyLinker.AddProxy(gameObject, asset) != null) return;
            var behaviour = Undo.AddComponent<UdonBehaviour>(gameObject);
            var serialized = new SerializedObject(behaviour);
            serialized.FindProperty("programSource").objectReferenceValue = asset;
            serialized.FindProperty("serializedProgramAsset").objectReferenceValue = asset.SerializedProgramAsset;
            serialized.ApplyModifiedProperties();
        }

        private static Transform Root(Scene scene, bool create)
        {
            GameObject[] roots = scene.GetRootGameObjects().Where(g => g.name == RootName).ToArray();
            if (roots.Length > 1)
                Debug.LogWarning($"[UdonLuau] the scene has {roots.Length} root objects named {RootName}; singletons are looked up in the first one.", roots[1]);
            GameObject root = roots.FirstOrDefault();
            if (root == null)
            {
                if (!create) return null;
                root = new GameObject(RootName);
                Undo.RegisterCreatedObjectUndo(root, "Create UdonLuau Singletons");
                SceneManager.MoveGameObjectToScene(root, scene);
            }
            if (!root.activeSelf)
            {
                Undo.RecordObject(root, "Activate UdonLuau Singletons");
                root.SetActive(true);
                Debug.LogWarning($"[UdonLuau] {RootName} must stay active so its singletons start; it was reactivated.", root);
            }
            return root.transform;
        }

        [PostProcessScene(-20)]
        private static void OnPostProcessScene() => Wire(SceneManager.GetActiveScene());

        /// <summary>Points every script in a scene that uses a singleton at the singleton's object, so it needs no lookup at run time.</summary>
        public static void Wire(Scene scene)
        {
            if (!scene.IsValid() || !scene.isLoaded) return;
            Transform root = Root(scene, false);
            foreach (UdonBehaviour behaviour in Behaviours(scene))
            {
                List<string> names = UsedBy(ProgramOf(behaviour)).ToList();
                if (names.Count == 0) continue;
                bool all = true;
                foreach (string name in names)
                {
                    UdonBehaviour target = root == null ? null : root.Find(name)?.GetComponents<UdonBehaviour>()
                        .FirstOrDefault(b => b.programSource is LuauProgramAsset asset && asset.ScriptName == name);
                    if (target == null)
                    {
                        all = false;
                        Debug.LogError($"[UdonLuau] '{behaviour.gameObject.name}' uses the {name} singleton, which is not in the scene.", behaviour);
                        continue;
                    }
                    Set(behaviour.publicVariables, ReferencePrefix + name, target);
                }
                Set(behaviour.publicVariables, ReadySymbol, all);
            }
        }

        private static void Set<T>(IUdonVariableTable table, string symbol, T value)
        {
            if (!table.TrySetVariableValue(symbol, value)) table.TryAddVariable(new UdonVariable<T>(symbol, value));
        }

        private static List<UdonBehaviour> Behaviours(Scene scene)
        {
            var list = new List<UdonBehaviour>();
            foreach (GameObject root in scene.GetRootGameObjects()) list.AddRange(root.GetComponentsInChildren<UdonBehaviour>(true));
            return list;
        }

        private static IUdonProgram ProgramOf(UdonBehaviour behaviour) => behaviour.programSource is LuauProgramAsset asset ? ProgramOf(asset) : null;

        private static IUdonProgram ProgramOf(LuauProgramAsset asset) =>
            asset == null ? null : asset.Program ?? asset.SerializedProgramAsset?.RetrieveProgram();
    }
}
