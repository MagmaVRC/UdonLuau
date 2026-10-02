using System;
using System.Collections.Concurrent;

namespace Magma.VRC.UdonLuau
{
    /// <summary>Udon's names for .NET types, as used in extern signatures.</summary>
    internal static class UdonTypeNames
    {
        private static readonly ConcurrentDictionary<Type, string> Cache = new ConcurrentDictionary<Type, string>();

        /// <summary>Removes the characters Udon strips from type names and spells array and by-ref markers out.</summary>
        public static string Sanitize(string typeName) =>
            typeName.Replace(",", "").Replace(".", "").Replace("[]", "Array").Replace("&", "Ref").Replace("+", "");

        /// <summary>Returns the Udon name of a type, such as UnityEngineTransformArray for Transform[].</summary>
        public static string Get(Type type) => Cache.GetOrAdd(type, Compute);

        private static string Compute(Type type)
        {
            string name = type.Name;
            if (type.IsGenericType)
            {
                int tick = name.IndexOf('`');
                if (tick >= 0) name = name.Substring(0, tick);
            }

            Type element = type;
            while (element.IsArray || element.IsByRef) element = element.GetElementType();

            string space = element.Namespace;
            if (element.DeclaringType != null)
            {
                string declaring = "";
                for (Type outer = element.DeclaringType; outer != null; outer = outer.DeclaringType) declaring = $"{outer.Name}.{declaring}";
                space += $".{declaring}";
            }

            if (name == "T" || name == "T[]") space = "";

            string full = Sanitize($"{space}.{name}");
            foreach (Type argument in element.GetGenericArguments()) full += Get(argument);

            return full switch
            {
                "SystemCollectionsGenericListT" => "ListT",
                "SystemCollectionsGenericIEnumerableT" => "IEnumerableT",
                _ => full,
            };
        }
    }
}
