using UdonLuau.Scripts;
using UdonSharp;
using UnityEngine;
using VRC.Udon.Common.Interfaces;

namespace UdonLuau.Samples
{
    /// <summary>Calls the InteropCounter Luau script when a player interacts with this object.</summary>
    public class InteropSharpCaller : UdonSharpBehaviour
    {
        /// <summary>The GameObject that has the InteropCounter component.</summary>
        public GameObject counterObject;

        public override void Interact()
        {
            InteropCounter counter = counterObject.GetComponent<InteropCounter>();
            if (counter == null)
            {
                Debug.LogWarning("InteropSharpCaller: no InteropCounter on the assigned object");
                return;
            }

            int total = counter._Add(1);
            Debug.Log($"InteropSharpCaller: Add returned {total}, count field reads {counter.count}");

            if (total >= 5)
                counter.SendCustomNetworkEvent(NetworkEventTarget.All, nameof(InteropCounter.ResetCount));
        }
    }
}
