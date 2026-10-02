using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using UdonSharp;
using UnityEditor;
using UnityEngine;
using VRC.Udon.Common.Interfaces;

namespace Magma.VRC.UdonLuau
{
    /// <summary>Generates the C# component class that stands for a Luau script on GameObjects, and the abstract UdonSharp class that lets UdonSharp code call it.</summary>
    public static class ProxyGenerator
    {
        /// <summary>The folder generated classes are written to.</summary>
        public const string Folder = "Assets/UdonLuau/Generated";

        /// <summary>The namespace of the generated components.</summary>
        public const string ProxyNamespace = "UdonLuau.Generated";

        /// <summary>The namespace of the generated UdonSharp classes.</summary>
        public const string UdonSharpNamespace = "UdonLuau.Scripts";

        private const string StubSuffix = ".UdonSharp.cs";

        private static HashSet<string> _luauScripts = new HashSet<string>();
        private static Dictionary<string, Type> _sharpScripts = new Dictionary<string, Type>();

        private static readonly HashSet<string> Keywords = new HashSet<string>
        {
            "abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked", "class", "const", "continue", "decimal", "default", "delegate", "do", "double",
            "else", "enum", "event", "explicit", "extern", "false", "finally", "fixed", "float", "for", "foreach", "goto", "if", "implicit", "in", "int", "interface", "internal",
            "is", "lock", "long", "namespace", "new", "null", "object", "operator", "out", "override", "params", "private", "protected", "public", "readonly", "ref", "return",
            "sbyte", "sealed", "short", "sizeof", "stackalloc", "static", "string", "struct", "switch", "this", "throw", "true", "try", "typeof", "uint", "ulong", "unchecked",
            "unsafe", "ushort", "using", "virtual", "void", "volatile", "while",
        };

        /// <summary>The C# class name generated for a script name.</summary>
        public static string ClassName(string scriptName)
        {
            var name = new StringBuilder();
            foreach (char c in scriptName ?? "") name.Append(char.IsLetterOrDigit(c) || c == '_' ? c : '_');
            if (name.Length == 0 || char.IsDigit(name[0])) name.Insert(0, '_');
            string result = name.ToString();
            return Keywords.Contains(result) ? "_" + result : result;
        }

        /// <summary>The full name of the UdonSharp class generated for a script, which is also the type identity its program reports to UdonSharp.</summary>
        public static string UdonSharpTypeName(string scriptName) => $"{UdonSharpNamespace}.{ClassName(scriptName)}";

        /// <summary>The generated component class for a program asset, or null before it has been generated and compiled.</summary>
        public static Type ProxyType(LuauProgramAsset asset)
        {
            if (asset == null || asset.SourceScript == null) return null;
            string guid = AssetDatabase.AssetPathToGUID(AssetDatabase.GetAssetPath(asset.SourceScript));
            foreach (Type type in TypeCache.GetTypesWithAttribute<UdonLuauScriptAttribute>())
            {
                var attribute = (UdonLuauScriptAttribute)Attribute.GetCustomAttribute(type, typeof(UdonLuauScriptAttribute));
                if (attribute?.ScriptGuid == guid && typeof(UdonLuauBehaviour).IsAssignableFrom(type) && !type.IsAbstract) return type;
            }
            return null;
        }

        /// <summary>The program asset of a generated component class.</summary>
        public static LuauProgramAsset ProgramAsset(Type proxyType)
        {
            var attribute = (UdonLuauScriptAttribute)Attribute.GetCustomAttribute(proxyType, typeof(UdonLuauScriptAttribute));
            if (attribute == null) return null;
            var script = AssetDatabase.LoadAssetAtPath<TextAsset>(AssetDatabase.GUIDToAssetPath(attribute.ScriptGuid));
            return script == null ? null : LuauEditorHooks.EnsureProgramAsset(script);
        }

        /// <summary>Writes the generated classes of compiled scripts whose text changed, importing them in one batch.</summary>
        /// <returns>Whether a file was written, which triggers a script compilation.</returns>
        public static bool Generate(IEnumerable<LuauProgramAsset> assets)
        {
            var compiled = assets.Where(a => a != null && a.SourceScript != null && a.LastInterface != null && a.Program != null).ToList();
            if (compiled.Count == 0) return false;

            _luauScripts = new HashSet<string>(compiled.Select(a => a.ScriptName));
            foreach (LuauProgramAsset other in LuauProgramAsset.FindAll())
                if (other.ScriptName != null && File.Exists($"{Folder}/{ClassName(other.ScriptName)}.cs")) _luauScripts.Add(other.ScriptName);
            _sharpScripts = new Dictionary<string, Type>();
            foreach (var entry in UdonSharpScripts.FindAll()) _sharpScripts[entry.Key] = entry.Value;

            bool written = false;
            AssetDatabase.StartAssetEditing();
            try
            {
                foreach (LuauProgramAsset asset in compiled) written |= Generate(asset, asset.LastInterface, asset.Program, asset.NetworkRates);
            }
            finally
            {
                AssetDatabase.StopAssetEditing();
            }
            return written;
        }

        /// <summary>Deletes generated classes whose script no longer exists.</summary>
        public static void Prune()
        {
            if (!Directory.Exists(Folder)) return;
            foreach (string file in Directory.GetFiles(Folder, "*.cs"))
            {
                if (file.EndsWith(StubSuffix, StringComparison.OrdinalIgnoreCase)) continue;
                string guid = GuidIn(File.ReadAllText(file));
                if (guid == null || !string.IsNullOrEmpty(AssetDatabase.GUIDToAssetPath(guid)) && AssetDatabase.LoadAssetAtPath<TextAsset>(AssetDatabase.GUIDToAssetPath(guid)) != null) continue;
                DeleteProxy(file.Replace('\\', '/'));
            }
        }

        private static string GuidIn(string source)
        {
            const string marker = "UdonLuauScript(\"";
            int start = source.IndexOf(marker, StringComparison.Ordinal);
            if (start < 0) return null;
            start += marker.Length;
            int end = source.IndexOf('"', start);
            return end > start ? source.Substring(start, end - start) : null;
        }

        private static bool Generate(LuauProgramAsset asset, ScriptInterface script, IUdonProgram program, IReadOnlyDictionary<string, int> networkRates)
        {
            string scriptPath = AssetDatabase.GetAssetPath(asset.SourceScript);
            string guid = AssetDatabase.AssetPathToGUID(scriptPath);
            string className = ClassName(Path.GetFileNameWithoutExtension(scriptPath));

            string proxyPath = $"{Folder}/{className}.cs";
            string existing = FindProxyFile(guid);
            if (existing != null && existing != proxyPath && !File.Exists(proxyPath))
            {
                AssetDatabase.MoveAsset(existing, proxyPath);
                string oldStub = existing.Substring(0, existing.Length - 3) + StubSuffix;
                if (File.Exists(oldStub)) AssetDatabase.DeleteAsset(oldStub);
            }

            bool written = Write(proxyPath, ProxySource(className, guid, Path.GetFileNameWithoutExtension(scriptPath), script));
            written |= Write($"{Folder}/{className}{StubSuffix}", StubSource(className, script, program, networkRates));
            return written;
        }

        private static void DeleteProxy(string proxy)
        {
            AssetDatabase.DeleteAsset(proxy);
            string stub = proxy.Substring(0, proxy.Length - 3) + StubSuffix;
            if (File.Exists(stub)) AssetDatabase.DeleteAsset(stub);
        }

        private static string FindProxyFile(string guid)
        {
            if (!Directory.Exists(Folder)) return null;
            string marker = $"UdonLuauScript(\"{guid}\")";
            foreach (string file in Directory.GetFiles(Folder, "*.cs"))
            {
                if (file.EndsWith(StubSuffix, StringComparison.OrdinalIgnoreCase)) continue;
                if (File.ReadAllText(file).Contains(marker)) return file.Replace('\\', '/');
            }
            return null;
        }

        private static bool Write(string path, string text)
        {
            if (File.Exists(path) && File.ReadAllText(path) == text) return false;
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            File.WriteAllText(path, text);
            AssetDatabase.ImportAsset(path);
            return true;
        }

        private static string ProxySource(string className, string guid, string scriptName, ScriptInterface script)
        {
            var text = new StringBuilder();
            text.Append("namespace ").Append(ProxyNamespace).Append("\n{\n");
            text.Append("    [global::Magma.VRC.UdonLuau.UdonLuauScript(\"").Append(guid).Append("\")]\n");
            text.Append("    [global::UnityEngine.AddComponentMenu(\"UdonLuau/").Append(scriptName.Replace("\"", "")).Append("\")]\n");
            text.Append("    public sealed class ").Append(className).Append(" : global::Magma.VRC.UdonLuau.UdonLuauBehaviour\n    {\n");

            var used = new HashSet<string> { className };
            foreach (ScriptVariable field in script.Fields)
            {
                string type = ProxyTypeName(field);
                if (type == null || !used.Add(field.Name)) continue;
                string member = Identifier(field.Name);
                if (!string.IsNullOrEmpty(field.Script) && type.StartsWith("global::" + ProxyNamespace, StringComparison.Ordinal))
                {
                    text.Append($"        public {Hides(field.Name)}{type} {member}\n        {{\n");
                    text.Append($"            get => ProxyOf<{type}>(GetVariable<global::VRC.Udon.UdonBehaviour>(\"{field.Symbol}\"));\n");
                    text.Append($"            set => SetVariable<global::VRC.Udon.UdonBehaviour>(\"{field.Symbol}\", value != null ? value.BackingBehaviour : null);\n        }}\n\n");
                }
                else
                {
                    text.Append($"        public {Hides(field.Name)}{type} {member}\n        {{\n");
                    text.Append($"            get => GetVariable<{type}>(\"{field.Symbol}\");\n");
                    text.Append($"            set => SetVariable<{type}>(\"{field.Symbol}\", value);\n        }}\n\n");
                }
            }

            foreach (ScriptMethod method in script.Methods)
            {
                if (method.Returns.Count > 1 || !used.Add(method.Name)) continue;
                string returnType = method.Returns.Count == 0 ? "void" : PlainTypeName(method.Returns[0].UdonType);
                var parameters = method.Parameters.Select(p => (p, type: PlainTypeName(p.UdonType))).ToList();
                if (returnType == null || parameters.Any(p => p.type == null)) continue;

                text.Append($"        public {Hides(method.Name)}{returnType} {Identifier(method.Name)}(");
                text.Append(string.Join(", ", parameters.Select(p => $"{p.type} {Identifier(p.p.Name)}")));
                text.Append(")\n        {\n");
                foreach (var (p, type) in parameters) text.Append($"            SetVariable<{type}>(\"{p.Symbol}\", {Identifier(p.Name)});\n");
                text.Append($"            SendEvent(\"{method.EntryPoint}\");\n");
                if (method.Returns.Count == 1) text.Append($"            return GetVariable<{returnType}>(\"{method.Returns[0].Symbol}\");\n");
                text.Append("        }\n\n");
            }

            TrimTrailingBlankLine(text);
            text.Append("    }\n}\n");
            return text.ToString();
        }

        private sealed class StubMethod
        {
            public ScriptMethod Method;
            public string Name;
            public string Entry;
            public string[] Parameters;
            public string Return;
        }

        private static string StubSource(string className, ScriptInterface script, IUdonProgram program, IReadOnlyDictionary<string, int> networkRates)
        {
            var text = new StringBuilder();
            text.Append("namespace ").Append(UdonSharpNamespace).Append("\n{\n");
            text.Append("    public abstract class ").Append(className).Append(" : global::UdonSharp.UdonSharpBehaviour\n    {\n");

            var reserved = new HashSet<string>(typeof(UdonSharpBehaviour).GetMembers().Select(m => m.Name)) { className };
            var used = new HashSet<string>();
            foreach (ScriptVariable field in script.Fields)
            {
                string type = StubTypeName(field);
                if (type == null || reserved.Contains(field.Name) || !used.Add(field.Name) || !program.SymbolTable.HasAddressForSymbol(field.Symbol) || field.Symbol != field.Name) continue;
                text.Append($"        public {type} {Identifier(field.Name)};\n");
            }
            if (used.Count > 0) text.Append('\n');

            var candidates = new List<StubMethod>();
            foreach (ScriptMethod method in script.Methods)
            {
                if (method.Returns.Count > 1 || method.NetworkCallable && method.Returns.Count > 0) continue;
                string name = method.NetworkCallable ? method.Name : "_" + method.Name;
                if (reserved.Contains(name) || used.Contains(name)) continue;
                if (method.Parameters.Any(p => StubTypeName(p) == null) || method.Returns.Any(r => StubTypeName(r) == null)) continue;
                candidates.Add(new StubMethod { Method = method, Name = name });
            }

            var entries = new HashSet<string>(program.EntryPoints.GetExportedSymbols());
            while (true)
            {
                Layout(candidates);
                var unmatched = candidates.Where(c => !entries.Contains(c.Entry) || c.Entry != c.Method.EntryPoint
                    || c.Parameters.Where((s, i) => s != c.Method.Parameters[i].Symbol).Any()
                    || c.Return != null && c.Return != c.Method.Returns[0].Symbol).ToList();
                if (unmatched.Count == 0) break;
                foreach (StubMethod m in unmatched) candidates.Remove(m);
            }

            foreach (StubMethod stub in candidates)
            {
                ScriptMethod method = stub.Method;
                if (method.NetworkCallable)
                {
                    int rate = networkRates != null && networkRates.TryGetValue(method.EntryPoint, out int r) ? r : 0;
                    text.Append(rate > 0 ? $"        [global::VRC.SDK3.UdonNetworkCalling.NetworkCallable({rate})]\n" : "        [global::VRC.SDK3.UdonNetworkCalling.NetworkCallable]\n");
                }
                string returnType = method.Returns.Count == 0 ? "void" : StubTypeName(method.Returns[0]);
                text.Append($"        public {returnType} {Identifier(stub.Name)}(");
                text.Append(string.Join(", ", method.Parameters.Select(p => $"{StubTypeName(p)} {Identifier(p.Name)}")));
                text.Append(method.Returns.Count == 0 ? ") { }\n" : ") { return default; }\n");
            }

            TrimTrailingBlankLine(text);
            text.Append("    }\n}\n");
            return text.ToString();
        }

        private static void Layout(List<StubMethod> methods)
        {
            var counters = new Dictionary<string, int>(StringComparer.Ordinal);
            string Unique(string id)
            {
                counters.TryGetValue(id, out int found);
                counters[id] = found + 1;
                return $"__{found}_{id}";
            }

            foreach (StubMethod stub in methods)
            {
                ScriptMethod method = stub.Method;
                stub.Entry = !method.NetworkCallable && method.Parameters.Count > 0 ? Unique(stub.Name) : stub.Name;
                stub.Parameters = method.Parameters.Select(p => Unique(p.Name + "__param")).ToArray();
                stub.Return = method.Returns.Count == 1 ? Unique(stub.Entry + "__ret") : null;
            }
        }

        private static void TrimTrailingBlankLine(StringBuilder text)
        {
            if (text.Length >= 2 && text[text.Length - 1] == '\n' && text[text.Length - 2] == '\n') text.Length--;
        }

        private static string Hides(string member) => typeof(UdonLuauBehaviour).GetMember(member).Length > 0 ? "new " : "";

        private static string Identifier(string name) => Keywords.Contains(name) ? "@" + name : name;

        private static string ProxyTypeName(ScriptVariable variable)
        {
            if (!string.IsNullOrEmpty(variable.Script) && !variable.UdonType.EndsWith("Array", StringComparison.Ordinal) && _luauScripts.Contains(variable.Script))
                return $"global::{ProxyNamespace}.{ClassName(variable.Script)}";
            return PlainTypeName(variable.UdonType);
        }

        private static string StubTypeName(ScriptVariable variable)
        {
            if (string.IsNullOrEmpty(variable.Script)) return PlainTypeName(variable.UdonType);
            bool array = variable.UdonType.EndsWith("Array", StringComparison.Ordinal);
            string element;
            if (_luauScripts.Contains(variable.Script))
                element = $"global::{UdonSharpNamespace}.{ClassName(variable.Script)}";
            else if (_sharpScripts.TryGetValue(variable.Script, out Type sharp))
                element = CSharpName(sharp);
            else
                return null;
            return array ? element + "[]" : element;
        }

        private static string PlainTypeName(string udonType)
        {
            Type type = LuauCatalog.Current?.ResolveType(udonType);
            return type == null ? null : CSharpName(type);
        }

        private static string CSharpName(Type type)
        {
            if (type.IsArray)
            {
                string element = CSharpName(type.GetElementType());
                return element == null ? null : element + "[]";
            }
            if (type.IsGenericType || type.IsGenericParameter || type.IsByRef || type.IsPointer) return null;
            if (type == typeof(void)) return "void";
            return "global::" + type.FullName.Replace('+', '.');
        }
    }
}
