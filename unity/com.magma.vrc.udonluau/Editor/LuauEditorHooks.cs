using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;
using VRC.SDKBase.Editor.BuildPipeline;
using VRC.Udon.Editor.ProgramSources;

namespace Magma.VRC.UdonLuau
{
    [CustomEditor(typeof(LuauProgramAsset))]
    internal sealed class LuauProgramAssetEditor : UdonProgramAssetEditor
    {
    }

    [InitializeOnLoad]
    internal sealed class LuauEditorHooks : AssetPostprocessor
    {
        private const string Template =
            "export local speed: number = 1\n" +
            "\n" +
            "function Start()\n" +
            "    print(\"Hello from UdonLuau\")\n" +
            "end\n" +
            "\n" +
            "function Update()\n" +
            "    transform:Rotate(Vector3.up, speed)\n" +
            "end\n";

        static LuauEditorHooks()
        {
            EditorApplication.delayCall += () =>
            {
                if (EditorApplication.isPlayingOrWillChangePlaymode) return;
                CompileAll();
            };
        }

        /// <summary>Compiles every UdonLuau program in the project.</summary>
        /// <returns>Whether all of them compiled.</returns>
        public static bool CompileAll()
        {
            bool succeeded = true;
            foreach (LuauProgramAsset asset in LuauProgramAsset.FindAll())
            {
                if (asset.SourceScript == null) continue;
                asset.RefreshProgram();
                succeeded &= !asset.HasErrors;
            }
            LuauProgramAsset.ApplySyncModesInOpenScenes();
            return succeeded;
        }

        private static void OnPostprocessAllAssets(string[] imported, string[] deleted, string[] moved, string[] movedFrom)
        {
            var changed = new HashSet<string>(imported.Where(IsLuau), StringComparer.OrdinalIgnoreCase);
            if (changed.Count == 0) return;

            EditorApplication.delayCall += () =>
            {
                if (EditorApplication.isPlayingOrWillChangePlaymode) return;
                foreach (LuauProgramAsset asset in LuauProgramAsset.FindAll())
                {
                    if (asset.SourceScript != null && changed.Contains(AssetDatabase.GetAssetPath(asset.SourceScript))) asset.RefreshProgram();
                }
                LuauProgramAsset.ApplySyncModesInOpenScenes();
            };
        }

        private static bool IsLuau(string path) => path.EndsWith(".luau", StringComparison.OrdinalIgnoreCase);

        [MenuItem("Assets/Create/VRChat/UdonLuau Script", false, 100)]
        private static void CreateScript()
        {
            string folder = "Assets";
            if (Selection.activeObject != null)
            {
                string selected = AssetDatabase.GetAssetPath(Selection.activeObject);
                if (!string.IsNullOrEmpty(selected)) folder = AssetDatabase.IsValidFolder(selected) ? selected : Path.GetDirectoryName(selected).Replace('\\', '/');
            }

            string scriptPath = AssetDatabase.GenerateUniqueAssetPath($"{folder}/NewLuauScript.luau");
            File.WriteAllText(scriptPath, Template);
            AssetDatabase.ImportAsset(scriptPath);

            var asset = ScriptableObject.CreateInstance<LuauProgramAsset>();
            asset.SourceScript = AssetDatabase.LoadAssetAtPath<TextAsset>(scriptPath);
            string assetPath = AssetDatabase.GenerateUniqueAssetPath(Path.ChangeExtension(scriptPath, ".asset"));
            AssetDatabase.CreateAsset(asset, assetPath);
            asset.RefreshProgram();
            AssetDatabase.SaveAssets();

            Selection.activeObject = asset.SourceScript;
            EditorGUIUtility.PingObject(asset);
        }
    }

    internal sealed class LuauBuildCompile : IVRCSDKBuildRequestedCallback
    {
        public int callbackOrder => 99;

        public bool OnBuildRequested(VRCSDKRequestedBuildType requestedBuildType)
        {
            if (requestedBuildType == VRCSDKRequestedBuildType.Avatar) return true;
            if (LuauEditorHooks.CompileAll())
            {
                AssetDatabase.SaveAssets();
                return true;
            }

            Debug.LogError("[UdonLuau] Some UdonLuau scripts failed to compile; fix the errors above before building.");
            return false;
        }
    }
}
