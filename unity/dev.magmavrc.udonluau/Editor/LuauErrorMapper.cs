using System;
using System.Collections.Generic;
using System.Reflection;
using UnityEditor;
using UnityEngine;
using VRC.Udon;
using VRC.Udon.Common.Interfaces;

namespace Magma.VRC.UdonLuau
{
    /// <summary>Logs the Luau source line where an Udon exception halted a behaviour running a UdonLuau program.</summary>
    [InitializeOnLoad]
    internal static class LuauErrorMapper
    {
        private const string ExceptionText = "An exception occurred during Udon execution";
        private const string ErroredText = "Udon VM execution errored";

        private static readonly FieldInfo VmField = typeof(UdonBehaviour).GetField("_udonVM", BindingFlags.Instance | BindingFlags.NonPublic);
        private static readonly HashSet<int> Reported = new HashSet<int>();
        private static bool _logging;

        static LuauErrorMapper()
        {
            Application.logMessageReceived += OnLog;
            EditorApplication.playModeStateChanged += _ => Reported.Clear();
            AssemblyReloadEvents.beforeAssemblyReload += () => Application.logMessageReceived -= OnLog;
        }

        private static void OnLog(string condition, string stackTrace, LogType type)
        {
            if (_logging || (type != LogType.Error && type != LogType.Exception) || !Application.isPlaying) return;
            if (condition == null || (condition.IndexOf(ExceptionText, StringComparison.Ordinal) < 0 && condition.IndexOf(ErroredText, StringComparison.Ordinal) < 0)) return;

            _logging = true;
            try
            {
                Report(condition);
            }
            finally
            {
                _logging = false;
            }
        }

        private static void Report(string condition)
        {
            if (!UdonManager.HasInstance) return;
            UdonBehaviour behaviour = UdonManager.Instance.currentlyExecuting;
            if (behaviour == null || !(behaviour.programSource is LuauProgramAsset asset) || asset.SourceScript == null) return;
            if (!(VmField?.GetValue(behaviour) is IUdonVM vm)) return;
            if (!Reported.Add(behaviour.GetInstanceID())) return;

            int line = asset.SourceLine(vm.GetProgramCounter());
            string path = AssetDatabase.GetAssetPath(asset.SourceScript);
            if (line < 0 || string.IsNullOrEmpty(path)) return;

            LuauProgramAsset.LogAt(path, line + 1, 1, false, Summary(condition), behaviour);
        }

        private static string Summary(string condition)
        {
            string[] lines = condition.Split('\n');
            string externLine = null;
            string cause = null;
            foreach (string raw in lines)
            {
                string text = raw.Trim();
                if (externLine == null && text.IndexOf("EXTERN to", StringComparison.Ordinal) >= 0) externLine = text;
                int inner = text.LastIndexOf("---> ", StringComparison.Ordinal);
                if (inner >= 0) cause = text.Substring(inner + 5);
            }
            string first = lines[0].TrimEnd('\r');
            if (externLine == null) return cause == null ? first : $"{first} {cause}";
            int at = externLine.IndexOf("An exception occurred during EXTERN", StringComparison.Ordinal);
            string detail = at >= 0 ? externLine.Substring(at) : externLine;
            return cause == null ? detail : $"{detail} {cause}";
        }
    }
}
