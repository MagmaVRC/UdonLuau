using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using UnityEditor;
using UnityEditor.Callbacks;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;
using VRC.Udon;
using Object = UnityEngine.Object;

namespace Magma.VRC.UdonLuau
{
    /// <summary>Links each UdonLuau component to the hidden UdonBehaviour that runs its script, upgrades plain UdonBehaviours, and removes the components from uploaded worlds.</summary>
    [InitializeOnLoad]
    internal static class ProxyLinker
    {
        private static readonly MethodInfo MoveComponent = Type.GetType("UnityEditorInternal.ComponentUtility, UnityEditor")
            ?.GetMethod("MoveComponentRelativeToComponent", BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public, null, new[] { typeof(Component), typeof(Component), typeof(bool) }, null);

        private static bool _sanitizeQueued;

        static ProxyLinker()
        {
            EditorSceneManager.sceneOpened += (_, __) => QueueSanitize();
            EditorApplication.hierarchyChanged += QueueSanitize;
        }

        /// <summary>Returns the component standing for the script a behaviour runs, or null.</summary>
        public static UdonLuauBehaviour ProxyOf(UdonBehaviour behaviour) =>
            behaviour == null ? null : behaviour.GetComponents<UdonLuauBehaviour>().FirstOrDefault(p => p.BackingBehaviour == behaviour);

        /// <summary>Adds the component standing for a script to a GameObject, with its hidden UdonBehaviour.</summary>
        /// <returns>The component, or null when the script's class has not been generated and compiled yet.</returns>
        public static UdonLuauBehaviour AddProxy(GameObject gameObject, LuauProgramAsset asset)
        {
            Type type = ProxyGenerator.ProxyType(asset);
            if (type == null) return null;
            var proxy = (UdonLuauBehaviour)Undo.AddComponent(gameObject, type);
            Setup(proxy, true);
            return proxy;
        }

        /// <summary>Creates or repairs the hidden UdonBehaviour of a component.</summary>
        public static void Setup(UdonLuauBehaviour proxy, bool withUndo)
        {
            if (proxy == null || PrefabUtility.IsPartOfPrefabInstance(proxy)) return;
            LuauProgramAsset asset = ProxyGenerator.ProgramAsset(proxy.GetType());
            if (asset == null) return;

            UdonBehaviour backing = proxy.BackingBehaviour;
            if (backing != null && backing.gameObject != proxy.gameObject) backing = null;
            if (backing != null && proxy.GetComponents<UdonLuauBehaviour>().Any(p => p != proxy && p.BackingBehaviour == backing)) backing = null;

            if (backing == null)
            {
                backing = withUndo ? Undo.AddComponent<UdonBehaviour>(proxy.gameObject) : proxy.gameObject.AddComponent<UdonBehaviour>();
                Move(backing, proxy, false);
                if (withUndo) Undo.RecordObject(proxy, "Link UdonLuau Behaviour");
                proxy.BackingBehaviour = backing;
                EditorUtility.SetDirty(proxy);
            }

            Link(backing, asset, withUndo);
            if (backing.enabled != proxy.enabled) backing.enabled = proxy.enabled;
            Hide(backing);
            SyncModes.Apply(new[] { backing }, false);
        }

        private static void Link(UdonBehaviour backing, LuauProgramAsset asset, bool withUndo)
        {
            var serialized = new SerializedObject(backing);
            SerializedProperty source = serialized.FindProperty("programSource");
            SerializedProperty program = serialized.FindProperty("serializedProgramAsset");
            if (source.objectReferenceValue != asset) source.objectReferenceValue = asset;
            if (program.objectReferenceValue != asset.SerializedProgramAsset) program.objectReferenceValue = asset.SerializedProgramAsset;
            if (withUndo) serialized.ApplyModifiedProperties();
            else serialized.ApplyModifiedPropertiesWithoutUndo();
        }

        private static void Hide(UdonBehaviour backing)
        {
            if ((backing.hideFlags & HideFlags.HideInInspector) != 0) return;
            EditorApplication.delayCall += () =>
            {
                if (backing == null) return;
                backing.hideFlags |= HideFlags.HideInInspector;
                EditorUtility.SetDirty(backing);
            };
        }

        private static void Move(Component component, Component relativeTo, bool above)
        {
            try
            {
                MoveComponent?.Invoke(null, new object[] { component, relativeTo, above });
            }
            catch (TargetInvocationException)
            {
            }
        }

        private static void QueueSanitize()
        {
            if (_sanitizeQueued || EditorApplication.isPlayingOrWillChangePlaymode) return;
            _sanitizeQueued = true;
            EditorApplication.delayCall += () =>
            {
                _sanitizeQueued = false;
                if (!EditorApplication.isPlayingOrWillChangePlaymode) SanitizeOpenScenes();
            };
        }

        /// <summary>Links every component in the open scenes, removes hidden UdonBehaviours whose component was removed, and upgrades plain UdonBehaviours that run UdonLuau programs.</summary>
        public static void SanitizeOpenScenes()
        {
            foreach (UdonLuauBehaviour proxy in Resources.FindObjectsOfTypeAll<UdonLuauBehaviour>())
            {
                if (EditorUtility.IsPersistent(proxy) || !proxy.gameObject.scene.IsValid()) continue;
                UdonBehaviour backing = proxy.BackingBehaviour;
                if (backing == null || backing.gameObject != proxy.gameObject || !(backing.programSource is LuauProgramAsset) || (backing.hideFlags & HideFlags.HideInInspector) == 0)
                    Setup(proxy, false);
            }

            foreach (UdonBehaviour behaviour in SyncModes.SceneBehaviours().ToList())
            {
                if (behaviour == null || !(behaviour.programSource is LuauProgramAsset asset) || ProxyOf(behaviour) != null) continue;

                if ((behaviour.hideFlags & HideFlags.HideInInspector) != 0)
                {
                    Undo.DestroyObjectImmediate(behaviour);
                    continue;
                }

                Upgrade(behaviour, asset);
            }
        }

        private static void Upgrade(UdonBehaviour behaviour, LuauProgramAsset asset)
        {
            Type type = ProxyGenerator.ProxyType(asset);
            if (type == null || PrefabUtility.IsPartOfPrefabInstance(behaviour)) return;

            var proxy = (UdonLuauBehaviour)Undo.AddComponent(behaviour.gameObject, type);
            proxy.BackingBehaviour = behaviour;
            proxy.enabled = behaviour.enabled;
            Move(proxy, behaviour, true);
            EditorUtility.SetDirty(proxy);
            Undo.RecordObject(behaviour, "Upgrade UdonLuau Behaviour");
            Hide(behaviour);
            Debug.Log($"[UdonLuau] Upgraded the UdonBehaviour on '{behaviour.gameObject.name}' to a {type.Name} component.", behaviour.gameObject);
        }

        [PostProcessScene(-10)]
        private static void OnPostProcessScene()
        {
            if (!EditorApplication.isPlayingOrWillChangePlaymode) StripScene(SceneManager.GetActiveScene());
        }

        /// <summary>Removes the UdonLuau components from a scene being built, leaving the UdonBehaviours that run the scripts.</summary>
        public static void StripScene(Scene scene)
        {
            if (!scene.IsValid() || !scene.isLoaded) return;
            var proxies = new List<UdonLuauBehaviour>();
            foreach (GameObject root in scene.GetRootGameObjects()) proxies.AddRange(root.GetComponentsInChildren<UdonLuauBehaviour>(true));
            foreach (UdonLuauBehaviour proxy in proxies)
            {
                if (proxy.BackingBehaviour != null) proxy.BackingBehaviour.hideFlags &= ~HideFlags.HideInInspector;
                Object.DestroyImmediate(proxy);
            }
        }
    }
}
