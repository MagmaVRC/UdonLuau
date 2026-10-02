using System.Collections.Generic;
using UnityEditor;
using UnityEngine;

namespace Magma.VRC.UdonLuau
{
    /// <summary>Project-wide UdonLuau settings, stored in ProjectSettings/UdonLuauSettings.asset.</summary>
    [FilePath("ProjectSettings/UdonLuauSettings.asset", FilePathAttribute.Location.ProjectFolder)]
    public sealed class LuauSettings : ScriptableSingleton<LuauSettings>
    {
        [SerializeField] private string defines = "DEBUG=false";

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

        [SettingsProvider]
        private static SettingsProvider CreateProvider() => new SettingsProvider("Project/UdonLuau", SettingsScope.Project, new HashSet<string> { "Luau", "Udon", "defines" })
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
            },
        };
    }
}
