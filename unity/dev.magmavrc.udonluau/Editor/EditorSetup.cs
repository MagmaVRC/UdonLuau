using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using UnityEditor;
using Debug = UnityEngine.Debug;

namespace Magma.VRC.UdonLuau
{
    /// <summary>Writes the Luau definitions file and the VS Code settings that make luau-lsp understand UdonLuau scripts.</summary>
    internal static class EditorSetup
    {
        /// <summary>The definitions file, relative to the project root.</summary>
        public const string DefinitionsFile = "UdonLuau.d.luau";

        private const string DefinitionsPackage = "@udon";

        /// <summary>Regenerates the definitions and VS Code settings.</summary>
        [MenuItem("Tools/UdonLuau/Regenerate Editor Definitions", false, 1)]
        public static void Regenerate()
        {
            ScriptRegistry.Refresh();
            Write(true);
        }

        /// <summary>Writes the definitions and VS Code settings when the setting is on and their content changed.</summary>
        /// <param name="log">Whether to log what was written and how long it took.</param>
        public static void Write(bool log = false)
        {
            if (!LuauSettings.instance.GenerateEditorSetup) return;
            LuauCatalog catalog = LuauCatalog.Current;
            if (catalog == null || Native.ul_catalog_definitions == null) return;

            var stopwatch = Stopwatch.StartNew();
            string definitions = Native.Read(Native.ul_catalog_definitions(catalog.Handle)) ?? "";
            long generated = stopwatch.ElapsedMilliseconds;
            bool changed = WriteIfChanged(DefinitionsFile, definitions);
            bool settings = WriteVsCodeSettings();
            stopwatch.Stop();

            if (log)
                Debug.Log($"[UdonLuau] {(changed ? "Wrote" : "Checked")} {DefinitionsFile} ({Encoding.UTF8.GetByteCount(definitions) / 1024} KB, generated in {generated} ms, {stopwatch.ElapsedMilliseconds} ms total){(settings ? "; updated .vscode settings" : "")}.");
        }

        private static bool WriteVsCodeSettings()
        {
            bool changed = false;
            Directory.CreateDirectory(".vscode");

            JObject settings = Load(".vscode/settings.json");
            JObject definitionFiles = settings["luau-lsp.types.definitionFiles"] as JObject;
            if (settings["luau-lsp.types.definitionFiles"] is JArray legacy)
            {
                if (!legacy.Values<string>().Contains(DefinitionsFile)) legacy.Add(DefinitionsFile);
            }
            else
            {
                definitionFiles ??= new JObject();
                definitionFiles[DefinitionsPackage] = DefinitionsFile;
                settings["luau-lsp.types.definitionFiles"] = definitionFiles;
            }

            settings["luau-lsp.platform.type"] = "standard";
            settings["luau-lsp.sourcemap.enabled"] = false;
            Child(settings, "luau-lsp.fflags.override")["LuauExportValueSyntax"] = "true";
            JObject associations = Child(settings, "files.associations");
            associations["*.luau"] = "luau";
            if (LuauSettings.instance.TreatLuaAsLuau) associations["*.lua"] = "luau";
            else if ((string)associations["*.lua"] == "luau") associations.Remove("*.lua");
            settings["Lua.diagnostics.enable"] = false;
            string merged = settings.ToString(Formatting.Indented) + "\n";
            if (File.Exists(".vscode/settings.json"))
            {
                string original = File.ReadAllText(".vscode/settings.json");
                if (original != merged && HasComments(original))
                {
                    File.Copy(".vscode/settings.json", ".vscode/settings.json.bak", true);
                    Debug.LogWarning("[UdonLuau] .vscode/settings.json had comments, which are not kept when its settings are merged; the original was saved as .vscode/settings.json.bak.");
                }
            }
            changed |= WriteIfChanged(".vscode/settings.json", merged);

            JObject luaurc = Load(".luaurc");
            Child(luaurc, "lint")["FunctionUnused"] = false;
            changed |= WriteIfChanged(".luaurc", luaurc.ToString(Formatting.Indented) + "\n");

            JObject extensions = Load(".vscode/extensions.json");
            JArray recommendations = extensions["recommendations"] as JArray ?? new JArray();
            if (!recommendations.Values<string>().Contains("johnnymorganz.luau-lsp", StringComparer.OrdinalIgnoreCase)) recommendations.Add("johnnymorganz.luau-lsp");
            extensions["recommendations"] = recommendations;
            changed |= WriteIfChanged(".vscode/extensions.json", extensions.ToString(Formatting.Indented) + "\n");
            return changed;
        }

        private static bool HasComments(string json)
        {
            bool inString = false;
            for (int i = 0; i < json.Length - 1; i++)
            {
                char c = json[i];
                if (inString)
                {
                    if (c == '\\') i++;
                    else if (c == '"') inString = false;
                }
                else if (c == '"') inString = true;
                else if (c == '/' && (json[i + 1] == '/' || json[i + 1] == '*')) return true;
            }
            return false;
        }

        private static JObject Child(JObject parent, string key)
        {
            if (parent[key] is JObject child) return child;
            child = new JObject();
            parent[key] = child;
            return child;
        }

        private static JObject Load(string path)
        {
            if (!File.Exists(path)) return new JObject();
            string text = File.ReadAllText(path);
            if (string.IsNullOrWhiteSpace(text)) return new JObject();
            try
            {
                using var reader = new JsonTextReader(new StringReader(text));
                return JObject.Load(reader, new JsonLoadSettings { CommentHandling = CommentHandling.Load, DuplicatePropertyNameHandling = DuplicatePropertyNameHandling.Replace });
            }
            catch (JsonException e)
            {
                string backup = path + ".bak";
                File.Copy(path, backup, true);
                Debug.LogWarning($"[UdonLuau] {path} could not be parsed ({e.Message}); it was saved as {backup} and rewritten.");
                return new JObject();
            }
        }

        private static bool WriteIfChanged(string path, string text)
        {
            if (File.Exists(path) && File.ReadAllText(path) == text) return false;
            File.WriteAllText(path, text);
            return true;
        }
    }
}
