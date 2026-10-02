using System.Collections.Generic;
using System.Linq;
using UdonSharp;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using VRC.SDK3.Components;
using VRC.SDKBase;
using VRC.Udon;
using VRC.Udon.Common.Interfaces;

namespace Magma.VRC.UdonLuau
{
    /// <summary>Applies UdonLuau sync modes to UdonBehaviours with UdonSharp's rules.</summary>
    internal static class SyncModes
    {
        private static readonly Networking.SyncType[] PickableMethods = { Networking.SyncType.None, Networking.SyncType.Continuous, Networking.SyncType.Manual };
        private static readonly string[] PickableNames = { "None", "Continuous", "Manual" };

        /// <summary>Every UdonBehaviour in the open scenes.</summary>
        public static IEnumerable<UdonBehaviour> SceneBehaviours() =>
            Resources.FindObjectsOfTypeAll<UdonBehaviour>().Where(b => !EditorUtility.IsPersistent(b) && b.gameObject.scene.IsValid());

        /// <summary>The sync mode a behaviour's program requires, or null for programs that are neither UdonLuau nor UdonSharp.</summary>
        public static BehaviourSyncMode? ModeOf(UdonBehaviour behaviour) => behaviour.programSource switch
        {
            LuauProgramAsset luau => luau.SyncMode,
            UdonSharpProgramAsset sharp => sharp.behaviourSyncMode,
            _ => null,
        };

        /// <summary>Forces None, Continuous and Manual modes and lets NoVariableSync behaviours follow the others on their GameObject.</summary>
        /// <param name="behaviours">The behaviours to update; others on their GameObjects are taken into account.</param>
        /// <param name="log">Whether to log conflicts between behaviours on the same GameObject.</param>
        public static void Apply(IEnumerable<UdonBehaviour> behaviours, bool log = true)
        {
            int modified = 0;
            var gameObjects = new HashSet<GameObject>();

            foreach (UdonBehaviour behaviour in behaviours)
            {
                if (behaviour == null || !(behaviour.programSource is LuauProgramAsset asset)) continue;
                gameObjects.Add(behaviour.gameObject);

                Networking.SyncType? forced = asset.SyncMode switch
                {
                    BehaviourSyncMode.None => Networking.SyncType.None,
                    BehaviourSyncMode.Continuous => Networking.SyncType.Continuous,
                    BehaviourSyncMode.Manual => Networking.SyncType.Manual,
                    _ => null,
                };
                if (forced is Networking.SyncType method && behaviour.SyncMethod != method)
                {
                    Set(behaviour, method);
                    modified++;
                }
            }

            foreach (GameObject gameObject in gameObjects)
            {
                UdonBehaviour[] onObject = gameObject.GetComponents<UdonBehaviour>();
                bool hasManual = false, hasContinuous = false, hasPositionSync = false, hasNoSync = false;

                foreach (UdonBehaviour other in onObject)
                {
                    if (ModeOf(other) == BehaviourSyncMode.NoVariableSync)
                    {
                        hasNoSync = true;
                        continue;
                    }

                    if (other.SyncMethod == Networking.SyncType.Manual) hasManual = true;
                    else if (other.SyncMethod == Networking.SyncType.Continuous) hasContinuous = true;
#pragma warning disable CS0618
                    if (other.SynchronizePosition) hasPositionSync = true;
#pragma warning restore CS0618
                }

                bool hasObjectSync = gameObject.GetComponent<VRCObjectSync>();
                if (hasManual && log)
                {
                    if (hasContinuous) Debug.LogWarning($"[UdonLuau] UdonBehaviours on GameObject '{gameObject.name}' have conflicting synchronization methods, this can cause sync to work unexpectedly.", gameObject);
                    if (hasObjectSync) Debug.LogWarning($"[UdonLuau] UdonBehaviours on GameObject '{gameObject.name}' are using manual sync while VRCObjectSync is on the GameObject, this can cause sync to work unexpectedly.", gameObject);
                    if (hasPositionSync) Debug.LogWarning($"[UdonLuau] UdonBehaviours on GameObject '{gameObject.name}' are using manual sync while position sync is enabled on an UdonBehaviour on the GameObject, this can cause sync to work unexpectedly.", gameObject);
                }

                if (!hasNoSync) continue;
                if (hasManual && (hasContinuous || hasPositionSync || hasObjectSync))
                {
                    if (log) Debug.LogWarning($"[UdonLuau] Cannot update sync mode on UdonBehaviour with NoVariableSync on '{gameObject.name}' because there are conflicting sync types on the GameObject", gameObject);
                    continue;
                }

                foreach (UdonBehaviour behaviour in onObject)
                {
                    if (!(behaviour.programSource is LuauProgramAsset asset) || asset.SyncMode != BehaviourSyncMode.NoVariableSync) continue;
                    if (hasManual && behaviour.SyncMethod != Networking.SyncType.Manual)
                    {
                        Set(behaviour, Networking.SyncType.Manual);
                        modified++;
                    }
                    else if (!hasManual && behaviour.SyncMethod == Networking.SyncType.Manual)
                    {
                        Set(behaviour, Networking.SyncType.Continuous);
                        modified++;
                    }
                }
            }

            if (modified > 0 && !Application.isPlaying) EditorSceneManager.MarkAllScenesDirty();
        }

        /// <summary>Draws the sync method of a behaviour: a picker for scripts with sync mode Any, the enforced method otherwise.</summary>
        public static void DrawGUI(LuauProgramAsset asset, UdonBehaviour behaviour, ref bool dirty)
        {
            if (behaviour == null)
            {
                EditorGUILayout.LabelField("Sync Mode", asset.SyncMode.ToString());
                return;
            }

            if (!Application.isPlaying) Apply(new[] { behaviour }, false);

            DrawObjectWarnings(behaviour);

            if (asset.SyncMode != BehaviourSyncMode.Any)
            {
                using (new EditorGUI.DisabledScope(true))
                    EditorGUILayout.TextField(new GUIContent("Synchronization Method", $"Set by -- @syncmode({asset.SyncMode.ToString().ToLowerInvariant()}) in the script"), Describe(behaviour, asset.SyncMode));
                return;
            }

            int index = System.Array.IndexOf(PickableMethods, behaviour.SyncMethod);
            using (new EditorGUI.DisabledScope(Application.isPlaying))
            {
                EditorGUI.BeginChangeCheck();
                int picked = EditorGUILayout.Popup("Synchronization Method", index, PickableNames);
                if (EditorGUI.EndChangeCheck() && picked >= 0)
                {
                    Set(behaviour, PickableMethods[picked]);
                    dirty = true;
                }
            }

            IUdonProgram program = asset.Program;
            if (program == null) return;
            var synced = program.SyncMetadataTable.GetAllSyncMetadata().ToList();
            bool interpolated = synced.Any(m => m.Properties.Any(p => p.InterpolationAlgorithm != UdonSyncInterpolationMethod.None));
            bool arrays = synced.Any(m => program.SymbolTable.GetSymbolType(m.Name)?.IsArray == true);

            switch (behaviour.SyncMethod)
            {
                case Networking.SyncType.Manual when interpolated:
                    EditorGUILayout.HelpBox("Manual sync ignores linear and smooth interpolation on synced variables.", MessageType.Warning);
                    break;
                case Networking.SyncType.Continuous when arrays:
                    EditorGUILayout.HelpBox("Synced arrays are not supported with Continuous sync; use Manual.", MessageType.Warning);
                    break;
                case Networking.SyncType.None when synced.Count > 0:
                    EditorGUILayout.HelpBox("This script has synced variables, which do not sync with sync method None.", MessageType.Warning);
                    break;
            }
        }

        private static string Describe(UdonBehaviour behaviour, BehaviourSyncMode mode) =>
            mode == BehaviourSyncMode.NoVariableSync ? $"No Variable Sync (currently {behaviour.SyncMethod})" : behaviour.SyncMethod.ToString();

        private static void DrawObjectWarnings(UdonBehaviour behaviour)
        {
            UdonBehaviour[] onObject = behaviour.GetComponents<UdonBehaviour>();
            if (onObject.Length > 1 && ModeOf(behaviour) != BehaviourSyncMode.NoVariableSync)
            {
                var others = onObject.Where(b => ModeOf(b) != BehaviourSyncMode.NoVariableSync).ToList();
                if (others.Any(b => b.SyncMethod == Networking.SyncType.Manual) && others.Any(b => b.SyncMethod != Networking.SyncType.Manual))
                    EditorGUILayout.HelpBox("You are mixing sync methods between UdonBehaviours on the same game object, this will cause all behaviours to use the sync method of the last component on the game object.", MessageType.Error);
            }

            if (behaviour.SyncMethod != Networking.SyncType.Manual) return;
#pragma warning disable CS0618
            if (behaviour.SynchronizePosition)
                EditorGUILayout.HelpBox("Manual sync cannot be used on GameObjects with Position Sync", MessageType.Error);
#pragma warning restore CS0618
            else if (behaviour.GetComponent<VRCObjectSync>())
                EditorGUILayout.HelpBox("Manual sync cannot be used on GameObjects with VRC Object Sync", MessageType.Error);
        }

        private static void Set(UdonBehaviour behaviour, Networking.SyncType method)
        {
            Undo.RecordObject(behaviour, "Apply UdonLuau Sync Mode");
            behaviour.SyncMethod = method;
            EditorUtility.SetDirty(behaviour);
            if (PrefabUtility.IsPartOfPrefabInstance(behaviour)) PrefabUtility.RecordPrefabInstancePropertyModifications(behaviour);
        }
    }
}
