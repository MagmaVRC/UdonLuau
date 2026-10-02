using System.Linq;
using UnityEditor;
using UnityEngine;
using VRC.Udon;

namespace Magma.VRC.UdonLuau
{
    /// <summary>Adds an UdonBehaviour running the script when a Luau script is dropped on a GameObject in the hierarchy, scene view or inspector.</summary>
    [InitializeOnLoad]
    internal static class LuauDragAndDrop
    {
        static LuauDragAndDrop()
        {
#if UNITY_6000_3_OR_NEWER
            DragAndDrop.AddDropHandlerV2(OnHierarchyDrop);
            DragAndDrop.AddDropHandlerV2(OnSceneDrop);
            DragAndDrop.AddDropHandlerV2(OnInspectorDrop);
#else
            DragAndDrop.AddDropHandler(OnHierarchyDrop);
            DragAndDrop.AddDropHandler(OnSceneDrop);
            DragAndDrop.AddDropHandler(OnInspectorDrop);
#endif
        }

#if UNITY_6000_3_OR_NEWER
        private static DragAndDropVisualMode OnHierarchyDrop(EntityId target, HierarchyDropFlags flags, Transform parent, bool perform) =>
            OnHierarchyDrop(EditorUtility.EntityIdToObject(target), flags, perform);
#else
        private static DragAndDropVisualMode OnHierarchyDrop(int target, HierarchyDropFlags flags, Transform parent, bool perform) =>
            OnHierarchyDrop(EditorUtility.InstanceIDToObject(target), flags, perform);
#endif

        private static DragAndDropVisualMode OnHierarchyDrop(Object target, HierarchyDropFlags flags, bool perform)
        {
            if ((flags & HierarchyDropFlags.DropUpon) == 0) return DragAndDropVisualMode.None;
            return Drop(target as GameObject, perform, false);
        }

        private static DragAndDropVisualMode OnSceneDrop(Object dropUpon, Vector3 worldPosition, Vector2 viewportPosition, Transform parent, bool perform) =>
            Drop(dropUpon as GameObject, perform, false);

        private static DragAndDropVisualMode OnInspectorDrop(Object[] targets, bool perform)
        {
            GameObject gameObject = targets.Select(t => t as GameObject ?? (t as Component)?.gameObject).FirstOrDefault(g => g != null);
            return Drop(gameObject, perform, true);
        }

        private static DragAndDropVisualMode Drop(GameObject target, bool perform, bool fillEmpty)
        {
            if (target == null || EditorUtility.IsPersistent(target) || Application.isPlaying) return DragAndDropVisualMode.None;

            TextAsset script = DraggedScript();
            LuauProgramAsset asset = DragAndDrop.objectReferences.OfType<LuauProgramAsset>().FirstOrDefault();
            if (script == null && asset == null) return DragAndDropVisualMode.None;

            if (!perform)
            {
                asset ??= LuauProgramAsset.ForScript(script);
                if (asset != null && script != null) DragAndDrop.objectReferences = new Object[] { asset };
                return DragAndDropVisualMode.Link;
            }

            asset ??= LuauEditorHooks.EnsureProgramAsset(script);
            if (asset == null) return DragAndDropVisualMode.Rejected;

            UdonBehaviour behaviour = fillEmpty ? target.GetComponents<UdonBehaviour>().FirstOrDefault(b => b.programSource == null) : null;
            if (behaviour == null && ProxyLinker.AddProxy(target, asset) != null)
            {
                DragAndDrop.AcceptDrag();
                Selection.activeGameObject = target;
                return DragAndDropVisualMode.Link;
            }

            if (behaviour == null) behaviour = Undo.AddComponent<UdonBehaviour>(target);
            else Undo.RecordObject(behaviour, "Assign UdonLuau Program");

            var serialized = new SerializedObject(behaviour);
            serialized.FindProperty("programSource").objectReferenceValue = asset;
            serialized.FindProperty("serializedProgramAsset").objectReferenceValue = asset.SerializedProgramAsset;
            serialized.ApplyModifiedProperties();
            SyncModes.Apply(new[] { behaviour });

            DragAndDrop.AcceptDrag();
            Selection.activeGameObject = target;
            return DragAndDropVisualMode.Link;
        }

        private static TextAsset DraggedScript()
        {
            foreach (Object reference in DragAndDrop.objectReferences)
            {
                if (!(reference is TextAsset text)) continue;
                string path = AssetDatabase.GetAssetPath(text);
                if (!string.IsNullOrEmpty(path) && LuauSettings.IsScriptPath(path)) return text;
            }
            return null;
        }
    }
}
