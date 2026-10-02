using System.Collections.Generic;
using System.Linq;
using UnityEditor;
using UnityEngine;
using VRC.Udon;

namespace Magma.VRC.UdonLuau
{
    [CustomEditor(typeof(UdonLuauBehaviour), true)]
    internal sealed class LuauBehaviourEditor : Editor
    {
        private static bool _showUdon;
        private readonly Dictionary<UdonLuauBehaviour, UdonBehaviour> _backings = new Dictionary<UdonLuauBehaviour, UdonBehaviour>();

        private void OnEnable()
        {
            foreach (UdonLuauBehaviour proxy in targets.OfType<UdonLuauBehaviour>())
            {
                if (!Application.isPlaying) ProxyLinker.Setup(proxy, true);
                _backings[proxy] = proxy.BackingBehaviour;
            }
        }

        private void OnDestroy()
        {
            if (Application.isPlaying) return;
            foreach (var pair in _backings)
            {
                if (ReferenceEquals(pair.Key, null) || pair.Key != null || pair.Value == null) continue;
                if (ProxyLinker.ProxyOf(pair.Value) == null) Undo.DestroyObjectImmediate(pair.Value);
            }
        }

        public override void OnInspectorGUI()
        {
            var proxy = (UdonLuauBehaviour)target;
            UdonBehaviour backing = proxy.BackingBehaviour;
            if (backing == null || !(backing.programSource is LuauProgramAsset asset))
            {
                EditorGUILayout.HelpBox("This component is not linked to its UdonBehaviour yet.", MessageType.Info);
                return;
            }

            bool dirty = false;
            using (new EditorGUI.DisabledScope(targets.Length > 1))
                asset.DrawProxyGUI(backing, ref dirty);
            if (dirty) EditorUtility.SetDirty(backing);

            _showUdon = EditorGUILayout.Foldout(_showUdon, "Udon", true);
            if (!_showUdon) return;
            using (new EditorGUI.DisabledScope(true))
            using (new EditorGUI.IndentLevelScope())
            {
                EditorGUILayout.ObjectField("Udon Behaviour", backing, typeof(UdonBehaviour), true);
                EditorGUILayout.ObjectField("Program Source", asset, typeof(LuauProgramAsset), false);
                EditorGUILayout.ObjectField("Script", asset.SourceScript, typeof(TextAsset), false);
            }
        }
    }
}
