using System;
using UnityEngine;
using VRC.Udon;
using VRC.Udon.Common;
using VRC.Udon.Common.Interfaces;

namespace Magma.VRC.UdonLuau
{
    /// <summary>Marks a generated component class with the asset GUID of the Luau script it stands for.</summary>
    [AttributeUsage(AttributeTargets.Class, Inherited = false)]
    public sealed class UdonLuauScriptAttribute : Attribute
    {
        /// <summary>The asset GUID of the .lua or .luau script.</summary>
        public string ScriptGuid { get; }

        /// <param name="scriptGuid">The asset GUID of the .lua or .luau script.</param>
        public UdonLuauScriptAttribute(string scriptGuid) => ScriptGuid = scriptGuid;
    }

    /// <summary>Base of the generated components that stand for UdonLuau scripts. The script runs on a hidden UdonBehaviour linked to this component; the component itself does nothing at run time and is removed from uploaded worlds.</summary>
    public abstract class UdonLuauBehaviour : MonoBehaviour
    {
        [SerializeField, HideInInspector] private UdonBehaviour backingBehaviour;

        /// <summary>The hidden UdonBehaviour that runs the script.</summary>
        public UdonBehaviour BackingBehaviour
        {
            get => backingBehaviour;
            set => backingBehaviour = value;
        }

        /// <summary>Reads a variable of the script: the running value in play mode, the inspector value otherwise.</summary>
        protected T GetVariable<T>(string symbol)
        {
            if (backingBehaviour == null) return default;
            object value = Application.isPlaying
                ? backingBehaviour.GetProgramVariable(symbol)
                : backingBehaviour.publicVariables.TryGetVariableValue(symbol, out object stored) ? stored : null;
            return value is T typed ? typed : default;
        }

        /// <summary>Writes a variable of the script: the running value in play mode, the inspector value otherwise.</summary>
        protected void SetVariable<T>(string symbol, T value)
        {
            if (backingBehaviour == null) return;
            if (Application.isPlaying)
            {
                backingBehaviour.SetProgramVariable(symbol, value);
                return;
            }

            IUdonVariableTable table = backingBehaviour.publicVariables;
            if (!table.TrySetVariableValue(symbol, value)) table.TryAddVariable(new UdonVariable<T>(symbol, value));
        }

        /// <summary>Runs an entry point of the script.</summary>
        protected void SendEvent(string entryPoint)
        {
            if (backingBehaviour != null) backingBehaviour.SendCustomEvent(entryPoint);
        }

        /// <summary>Returns the component standing for the script that runs on a behaviour.</summary>
        protected static T ProxyOf<T>(UdonBehaviour behaviour) where T : UdonLuauBehaviour
        {
            if (behaviour == null) return null;
            foreach (T proxy in behaviour.GetComponents<T>())
                if (proxy.backingBehaviour == behaviour) return proxy;
            return null;
        }
    }
}
