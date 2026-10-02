using System.IO;
using UnityEditor.AssetImporters;
using UnityEngine;

namespace Magma.VRC.UdonLuau
{
    /// <summary>Imports UdonLuau scripts (.lua, unless turned off in Project Settings, and .luau) as text assets.</summary>
    [ScriptedImporter(3, new[] { "luau" }, new[] { "lua" })]
    public sealed class LuauScriptImporter : ScriptedImporter
    {
        private static Texture2D _icon;

        /// <summary>Reads the file into a TextAsset.</summary>
        public override void OnImportAsset(AssetImportContext ctx)
        {
            var source = new TextAsset(File.ReadAllText(ctx.assetPath));
            ctx.AddObjectToAsset("source", source, Icon());
            ctx.SetMainObject(source);
        }

        private static Texture2D Icon()
        {
            if (_icon != null) return _icon;
            const int size = 32;
            var planet = new Color32(0, 0, 128, 255);
            var moon = new Color32(255, 255, 255, 255);
            var pixels = new Color32[size * size];
            for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
            {
                float px = x + 0.5f, py = y + 0.5f;
                bool inPlanet = (px - 13f) * (px - 13f) + (py - 13f) * (py - 13f) <= 12f * 12f;
                bool inHole = (px - 18f) * (px - 18f) + (py - 18f) * (py - 18f) <= 3.5f * 3.5f;
                bool inMoon = (px - 27f) * (px - 27f) + (py - 27f) * (py - 27f) <= 3.5f * 3.5f;
                pixels[y * size + x] = inMoon || inPlanet && inHole ? moon : inPlanet ? planet : new Color32(0, 0, 0, 0);
            }

            _icon = new Texture2D(size, size, TextureFormat.RGBA32, false) { hideFlags = HideFlags.HideAndDontSave, filterMode = FilterMode.Bilinear };
            _icon.SetPixels32(pixels);
            _icon.Apply();
            return _icon;
        }
    }
}
