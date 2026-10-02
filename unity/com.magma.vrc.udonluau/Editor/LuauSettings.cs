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

        /// <summary>Whether .lua files are imported and compiled as UdonLuau scripts.</summary>
        public bool TreatLuaAsLuau
        {
            get => treatLuaAsLuau;
            set
            {
                treatLuaAsLuau = value;
                Save(true);
            }
        }

        /// <summary>Whether a path is an UdonLuau script: .luau, or .lua when that is enabled.</summary>
        public static bool IsScriptPath(string path) =>
            path.EndsWith(".luau", StringComparison.OrdinalIgnoreCase) || instance.treatLuaAsLuau && IsLuaPath(path);

        private static bool IsLuaPath(string path) => path.EndsWith(".lua", StringComparison.OrdinalIgnoreCase) && path.StartsWith("Assets/", StringComparison.Ordinal);

        /// <summary>Points .lua files at the UdonLuau importer, or back at their default importer, to match the setting.</summary>
        /// <param name="paths">The paths to update, or null for every .lua file in the project.</param>
        public static void ApplyLuaImporterOverrides(IEnumerable<string> paths = null)
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
                bool lua = EditorGUILayout.Toggle(new GUIContent("Treat .lua files as UdonLuau scripts", "Imports .lua files with the UdonLuau importer and compiles them like .luau files"), instance.TreatLuaAsLuau);
                if (EditorGUI.EndChangeCheck())
                {
                    instance.TreatLuaAsLuau = lua;
                    ApplyLuaImporterOverrides();
                }
            },
        };
    }
}
