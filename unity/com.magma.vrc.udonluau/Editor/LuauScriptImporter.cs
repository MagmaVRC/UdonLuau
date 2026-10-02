using System.IO;
using UnityEditor.AssetImporters;
using UnityEngine;

namespace Magma.VRC.UdonLuau
{
    /// <summary>Imports .luau files as text assets.</summary>
    [ScriptedImporter(1, "luau")]
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
