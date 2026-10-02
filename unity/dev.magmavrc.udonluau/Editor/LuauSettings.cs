using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;

namespace Magma.VRC.UdonLuau
{
    /// <summary>Project-wide UdonLuau settings, stored in ProjectSettings/UdonLuauSettings.asset.</summary>
    [FilePath("ProjectSettings/UdonLuauSettings.asset", FilePathAttribute.Location.ProjectFolder)]
    public sealed class LuauSettings : ScriptableSingleton<LuauSettings>
    {
        [SerializeField] private string defines = "DEBUG=false";
        [SerializeField] private bool treatLuaAsLuau = true;
        [SerializeField] private bool generateEditorSetup = true;

        /// <summary>Whether UdonLuau.d.luau and the VS Code luau-lsp settings are written to the project root.</summary>
        public bool GenerateEditorSetup
        {
            get => generateEditorSetup;
            set
            {
                generateEditorSetup = value;
                Save(true);
            }
        }

        /// <summary>Compile-time defines as ';'-separated NAME=value pairs.</summary>
        public string Defines
        {
            get => defines;
            set
            {
                defines = value;
                Save(true);
            }
        }

        /// <summary>Whether .lua files are UdonLuau scripts; on by default, off only for projects with unrelated .lua text files.</summary>
        public bool TreatLuaAsLuau
        {
            get => treatLuaAsLuau;
            set
            {
                treatLuaAsLuau = value;
                Save(true);
            }
        }

        /// <summary>Whether a path is an UdonLuau script: .lua (unless turned off) or .luau.</summary>
        public static bool IsScriptPath(string path) =>
            path.EndsWith(".luau", StringComparison.OrdinalIgnoreCase) || instance.treatLuaAsLuau && IsLuaPath(path);

        private static bool IsLuaPath(string path) => path.EndsWith(".lua", StringComparison.OrdinalIgnoreCase) && path.StartsWith("Assets/", StringComparison.Ordinal);

        internal static void ApplyLuaImporterOverrides(IEnumerable<string> paths = null)
        {
            paths ??= Directory.GetFiles("Assets", "*.lua", SearchOption.AllDirectories).Select(p => p.Replace('\\', '/'));
            foreach (string path in paths.Where(IsLuaPath).Distinct())
            {
                if (string.IsNullOrEmpty(AssetDatabase.AssetPathToGUID(path))) continue;
                bool ours = AssetDatabase.GetImporterOverride(path) == typeof(LuauScriptImporter);
                if (instance.treatLuaAsLuau && !ours) AssetDatabase.SetImporterOverride<LuauScriptImporter>(path);
                else if (!instance.treatLuaAsLuau && ours) AssetDatabase.ClearImporterOverride(path);
            }
        }

        [MenuItem("Tools/UdonLuau/Project Settings", false, 40)]
        private static void OpenSettings() => SettingsService.OpenProjectSettings("Project/UdonLuau");

        [SettingsProvider]
        private static SettingsProvider CreateProvider() => new SettingsProvider("Project/UdonLuau", SettingsScope.Project, new HashSet<string> { "Luau", "Lua", "Udon", "defines" })
        {
            guiHandler = _ =>
            {
                EditorGUI.BeginChangeCheck();
                string value = EditorGUILayout.DelayedTextField(new GUIContent("Defines", "';'-separated NAME=value pairs, such as DEBUG=true"), instance.Defines);
                if (EditorGUI.EndChangeCheck())
                {
                    instance.Defines = value;
                    LuauEditorHooks.CompileAll();
                }

                EditorGUI.BeginChangeCheck();
                bool lua = EditorGUILayout.Toggle(new GUIContent("Treat .lua files as UdonLuau scripts", "UdonLuau scripts are .lua files (.luau is also accepted). Turn this off only if the project contains unrelated .lua text files; .luau files stay UdonLuau scripts."), instance.TreatLuaAsLuau);
                if (EditorGUI.EndChangeCheck())
                {
                    instance.TreatLuaAsLuau = lua;
                    ApplyLuaImporterOverrides();
                    EditorSetup.Write();
                }

                EditorGUI.BeginChangeCheck();
                bool setup = EditorGUILayout.Toggle(new GUIContent("Generate VS Code / luau-lsp setup", $"Writes {EditorSetup.DefinitionsFile} and .vscode settings for the luau-lsp extension"), instance.GenerateEditorSetup);
                if (EditorGUI.EndChangeCheck())
                {
                    instance.GenerateEditorSetup = setup;
                    if (setup) EditorSetup.Regenerate();
                }
            },
        };
    }
}
