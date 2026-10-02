using UdonSharp;
using UnityEngine;

namespace UdonLuau.Samples
{
    /// <summary>An UdonSharp behaviour that the InteropLuauCaller Luau script calls.</summary>
    public class InteropSharpTarget : UdonSharpBehaviour
    {
        /// <summary>How many times Greet has run.</summary>
        public int greetings;

        /// <summary>Logs a greeting and returns its length.</summary>
        public int Greet(string playerName)
        {
            greetings++;
            string text = $"Hello, {playerName}!";
            Debug.Log($"InteropSharpTarget: {text} ({greetings})");
            return text.Length;
        }
    }
}
