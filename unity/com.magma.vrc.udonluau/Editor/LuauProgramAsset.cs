using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Reflection;
using UnityEditor;
using UnityEngine;
using VRC.SDKBase;
using VRC.Udon;
using VRC.Udon.Common;
using VRC.Udon.Editor.ProgramSources;
using VRC.Udon.Editor.ProgramSources.Attributes;
using Magma.VRC.UdonLuau;

[assembly: UdonProgramSourceNewMenu(typeof(LuauProgramAsset), "UdonLuau Program Asset")]

namespace Magma.VRC.UdonLuau
{
    /// <summary>An Udon program source compiled from a .luau script.</summary>
    [CreateAssetMenu(menuName = "VRChat/Udon/UdonLuau Program Asset", fileName = "New UdonLuau Program Asset")]
    public class LuauProgramAsset : UdonProgramAsset
    {
        private static readonly Type[] FileLogParameters = { typeof(string), typeof(string), typeof(int), typeof(int) };
        private static readonly MethodInfo LogFileError = typeof(Debug).GetMethod("LogPlayerBuildError", BindingFlags.NonPublic | BindingFlags.Static, null, FileLogParameters, null);
        private static readonly MethodInfo LogFileWarning = typeof(Debug).GetMethod("LogCompilerWarning", BindingFlags.NonPublic | BindingFlags.Static, null, FileLogParameters, null);

        [SerializeField] private TextAsset sourceScript;
        [SerializeField] private List<LuauDiagnostic> diagnostics = new List<LuauDiagnostic>();
        [SerializeField] private List<LuauFieldAnnotations> fieldAnnotations = new List<LuauFieldAnnotations>();
        [SerializeField] private string syncMode = "";
        [SerializeField] private string disassembly = "";

        [NonSerialized] private bool _showDisassembly;

        /// <summary>The .luau script this program is compiled from.</summary>
        public TextAsset SourceScript
        {
            get => sourceScript;
            set => sourceScript = value;
        }

        /// <summary>Messages from the last compilation.</summary>
        public IReadOnlyList<LuauDiagnostic> Diagnostics => diagnostics;

        /// <summary>Whether the last compilation failed.</summary>
        public bool HasErrors => diagnostics.Any(d => !d.isWarning);

        /// <summary>The sync method declared with @syncmode, or null when the script leaves it to the behaviour.</summary>
        public Networking.SyncType? SyncMode => syncMode switch
        {
            "none" => Networking.SyncType.None,
            "manual" => Networking.SyncType.Manual,
            "continuous" => Networking.SyncType.Continuous,
            _ => (Networking.SyncType?)null,
        };

        /// <summary>Returns every program asset in the project.</summary>
        public static IEnumerable<LuauProgramAsset> FindAll() =>
            AssetDatabase.FindAssets($"t:{nameof(LuauProgramAsset)}")
                .Select(guid => AssetDatabase.LoadAssetAtPath<LuauProgramAsset>(AssetDatabase.GUIDToAssetPath(guid)))
                .Where(asset => asset != null);

        /// <summary>Sets the behaviour's sync method to the one its program declares.</summary>
        /// <returns>Whether the behaviour was changed.</returns>
        public static bool ApplySyncMode(UdonBehaviour behaviour)
        {
            if (behaviour == null || !(behaviour.programSource is LuauProgramAsset asset) || !(asset.SyncMode is Networking.SyncType mode)) return false;
            if (behaviour.SyncMethod == mode) return false;

            Undo.RecordObject(behaviour, "Apply UdonLuau Sync Mode");
            behaviour.SyncMethod = mode;
            EditorUtility.SetDirty(behaviour);
            if (PrefabUtility.IsPartOfPrefabInstance(behaviour)) PrefabUtility.RecordPrefabInstancePropertyModifications(behaviour);
            return true;
        }

        /// <summary>Applies sync modes to every UdonBehaviour in the open scenes that uses a UdonLuau program.</summary>
        public static void ApplySyncModesInOpenScenes()
        {
            foreach (UdonBehaviour behaviour in Resources.FindObjectsOfTypeAll<UdonBehaviour>())
            {
                if (EditorUtility.IsPersistent(behaviour) || !behaviour.gameObject.scene.IsValid()) continue;
                ApplySyncMode(behaviour);
            }
        }

        protected override void RefreshProgramImpl()
        {
            diagnostics.Clear();
            fieldAnnotations.Clear();
            syncMode = "";
            disassembly = "";

            if (sourceScript == null)
            {
                program = null;
                return;
            }

            LuauCompileResult result = LuauCompiler.Compile(sourceScript.text);
            program = result.Program;
            diagnostics.AddRange(result.Diagnostics);
            fieldAnnotations.AddRange(result.Fields);
            disassembly = result.Disassembly;

            LuauAnnotation sync = result.ModuleAnnotations.LastOrDefault(a => a.name == "syncmode");
            if (sync != null)
            {
                string mode = sync.arguments.Length > 0 ? sync.arguments[0].Trim().ToLowerInvariant() : "";
                if (mode == "none" || mode == "manual" || mode == "continuous") syncMode = mode;
                else diagnostics.Add(new LuauDiagnostic { isWarning = true, line = 1, column = 1, message = $"unknown sync mode '{mode}'; use none, manual or continuous" });
            }

            LogDiagnostics();
        }

        private void LogDiagnostics()
        {
            string path = AssetDatabase.GetAssetPath(sourceScript);
            foreach (LuauDiagnostic d in diagnostics)
            {
                string text = $"[UdonLuau] {path}({d.line},{d.column}): {(d.isWarning ? "warning" : "error")}: {d.message}";
                MethodInfo log = d.isWarning ? LogFileWarning : LogFileError;
                if (log != null)
                    log.Invoke(null, new object[] { text, path, d.line, d.column });
                else if (d.isWarning)
                    Debug.LogWarning(text, this);
                else
                    Debug.LogError(text, this);
            }
        }

        protected override void DrawProgramSourceGUI(UdonBehaviour udonBehaviour, ref bool dirty)
        {
            if (udonBehaviour == null) DrawSourceField(ref dirty);

            foreach (LuauDiagnostic d in diagnostics)
                EditorGUILayout.HelpBox($"{d.line}:{d.column}: {d.message}", d.isWarning ? MessageType.Warning : MessageType.Error);

            if (udonBehaviour != null && ApplySyncMode(udonBehaviour)) dirty = true;
            if (SyncMode is Networking.SyncType mode) EditorGUILayout.LabelField("Sync Mode", mode.ToString());

            DrawInteractionArea(udonBehaviour);
            DrawPublicVariables(udonBehaviour, ref dirty);

            if (udonBehaviour != null) return;
            _showDisassembly = EditorGUILayout.Foldout(_showDisassembly, "Compiled Program", true);
            if (!_showDisassembly) return;
            using (new EditorGUI.DisabledScope(true))
                EditorGUILayout.TextArea(disassembly);
            DrawProgramDisassembly();
        }

        private void DrawSourceField(ref bool dirty)
        {
            EditorGUI.BeginChangeCheck();
            var script = (TextAsset)EditorGUILayout.ObjectField("Source Script", sourceScript, typeof(TextAsset), false);
            if (EditorGUI.EndChangeCheck())
            {
                Undo.RecordObject(this, "Change UdonLuau Source");
                sourceScript = script;
                dirty = true;
                RefreshProgram();
            }

            using (new EditorGUI.DisabledScope(sourceScript == null || Application.isPlaying))
            {
                if (GUILayout.Button("Compile")) RefreshProgram();
            }
        }

        protected override object GetPublicVariableDefaultValue(string symbol, Type type)
        {
            if (program?.SymbolTable == null || !program.SymbolTable.TryGetAddressFromSymbol(symbol, out uint address)) return null;
            if (!program.Heap.TryGetHeapVariable(address, out object value) || value is UdonBaseHeapReference) return null;
            return type == null || type.IsInstanceOfType(value) ? value : null;
        }

        protected override object DrawPublicVariableField(string symbol, object variableValue, Type variableType, ref bool dirty, bool enabled)
        {
            List<LuauAnnotation> annotations = fieldAnnotations.FirstOrDefault(f => f.symbol == symbol)?.annotations;
            if (annotations == null || annotations.Count == 0) return base.DrawPublicVariableField(symbol, variableValue, variableType, ref dirty, enabled);
            if (Find(annotations, "hideininspector") != null) return variableValue;

            LuauAnnotation header = Find(annotations, "header");
            if (header != null)
            {
                EditorGUILayout.Space();
                EditorGUILayout.LabelField(Argument(header, 0), EditorStyles.boldLabel);
            }

            LuauAnnotation space = Find(annotations, "space");
            if (space != null) EditorGUILayout.Space(Number(Argument(space, 0), 8f));

            var label = new GUIContent(symbol, Argument(Find(annotations, "tooltip"), 0));
            LuauAnnotation range = Find(annotations, "range");
            LuauAnnotation multiline = Find(annotations, "multiline") ?? Find(annotations, "textarea");

            if (range != null && IsSliderType(variableType))
                return DrawSlider(label, variableValue, variableType, Number(Argument(range, 0), 0f), Number(Argument(range, 1), 1f), ref dirty, enabled);
            if (multiline != null && variableType == typeof(string))
                return DrawTextArea(label, variableValue as string, ref dirty, enabled);

            object result = base.DrawPublicVariableField(symbol, variableValue, variableType, ref dirty, enabled);
            if (!string.IsNullOrEmpty(label.tooltip) && Event.current.type == EventType.Repaint)
            {
                Rect row = GUILayoutUtility.GetLastRect();
                GUI.Label(new Rect(row.x, row.y, EditorGUIUtility.labelWidth, EditorGUIUtility.singleLineHeight), new GUIContent("", label.tooltip));
            }
            return result;
        }

        private static bool IsSliderType(Type type) => type == typeof(float) || type == typeof(double) || type == typeof(int);

        private static object DrawSlider(GUIContent label, object value, Type type, float min, float max, ref bool dirty, bool enabled)
        {
            using (new EditorGUI.DisabledScope(!enabled))
            {
                EditorGUI.BeginChangeCheck();
                object result;
                if (type == typeof(int))
                    result = EditorGUILayout.IntSlider(label, value is int i ? i : 0, (int)min, (int)max);
                else if (type == typeof(double))
                    result = (double)EditorGUILayout.Slider(label, value is double d ? (float)d : 0f, min, max);
                else
                    result = EditorGUILayout.Slider(label, value is float f ? f : 0f, min, max);
                if (EditorGUI.EndChangeCheck()) dirty = true;
                return result;
            }
        }

        private static object DrawTextArea(GUIContent label, string value, ref bool dirty, bool enabled)
        {
            using (new EditorGUI.DisabledScope(!enabled))
            {
                EditorGUILayout.LabelField(label);
                EditorGUI.BeginChangeCheck();
                var style = new GUIStyle(EditorStyles.textArea) { wordWrap = true };
                string result = EditorGUILayout.TextArea(value ?? "", style, GUILayout.MinHeight(EditorGUIUtility.singleLineHeight * 3));
                if (EditorGUI.EndChangeCheck()) dirty = true;
                return result;
            }
        }

        private static LuauAnnotation Find(List<LuauAnnotation> annotations, string name) =>
            annotations.LastOrDefault(a => string.Equals(a.name, name, StringComparison.OrdinalIgnoreCase));

        private static string Argument(LuauAnnotation annotation, int index) =>
            annotation?.arguments != null && index < annotation.arguments.Length ? annotation.arguments[index] : null;

        private static float Number(string text, float fallback) =>
            float.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out float value) ? value : fallback;
    }
}
