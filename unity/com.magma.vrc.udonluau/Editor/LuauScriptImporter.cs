using System.IO;
using UnityEditor.AssetImporters;
using UnityEngine;

namespace Magma.VRC.UdonLuau
{
    /// <summary>Imports .luau files, and .lua files when enabled in Project Settings, as text assets.</summary>
    [ScriptedImporter(2, new[] { "luau" }, new[] { "lua" })]
    public sealed class LuauScriptImporter : ScriptedImporter
    {
        /// <summary>Reads the file into a TextAsset.</summary>
        public override void OnImportAsset(AssetImportContext ctx)
        {
            var source = new TextAsset(File.ReadAllText(ctx.assetPath));
            ctx.AddObjectToAsset("source", source);
            ctx.SetMainObject(source);
        }
    }
}
