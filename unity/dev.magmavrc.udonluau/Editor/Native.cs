using System;
using System.ComponentModel;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using UnityEditor;

namespace Magma.VRC.UdonLuau
{
    internal enum ValueKind
    {
        Default = 0,
        Null = 1,
        Boolean = 2,
        Integer = 3,
        Unsigned = 4,
        Real = 5,
        String = 6,
        This = 7,
        Type = 8,
        Construct = 9,
        Array = 10,
    }

    internal enum TypeKind
    {
        Class = 0,
        Struct = 1,
        Enum = 2,
        Interface = 3,
        Array = 4,
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeDiagnostic
    {
        public int IsWarning;
        public int Line;
        public int Column;
        public int EndLine;
        public int EndColumn;
        public IntPtr Message;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeHeapSlot
    {
        public IntPtr Symbol;
        public IntPtr Type;
        public int Exported;
        public int Kind;
        public int Boolean;
        public long Integer;
        public ulong Unsigned;
        public double Real;
        public IntPtr Text;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeHeapValue
    {
        public int Kind;
        public int Boolean;
        public long Integer;
        public ulong Unsigned;
        public double Real;
        public IntPtr Text;
        public int ArgumentCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeAttribute
    {
        public IntPtr Name;
        public int ArgumentCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeEntryPoint
    {
        public IntPtr Name;
        public uint Address;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeSyncVariable
    {
        public IntPtr Symbol;
        public int Interpolation;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeScriptVariable
    {
        public IntPtr Name;
        public IntPtr Type;
        public IntPtr Script;
        public IntPtr Symbol;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeScriptMethod
    {
        public IntPtr Name;
        public IntPtr EntryPoint;
        public int ParameterCount;
        public int ReturnCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeNetworkCallable
    {
        public IntPtr EntryPoint;
        public int MaxEventsPerSecond;
        public int ParameterCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct NativeNetworkParameter
    {
        public IntPtr Symbol;
        public IntPtr Type;
    }

    internal abstract class NativeHandle : IDisposable
    {
        private readonly int _generation;

        protected NativeHandle(IntPtr ptr)
        {
            Ptr = ptr;
            _generation = Native.Generation;
        }

        public IntPtr Ptr { get; private set; }

        public bool IsInvalid => Ptr == IntPtr.Zero;

        public static implicit operator IntPtr(NativeHandle handle) => handle?.Ptr ?? IntPtr.Zero;

        public void Dispose()
        {
            if (Ptr != IntPtr.Zero && Native.IsLoaded && Native.Generation == _generation) Release(Ptr);
            Ptr = IntPtr.Zero;
        }

        protected abstract void Release(IntPtr ptr);
    }

    internal sealed class CatalogHandle : NativeHandle
    {
        public CatalogHandle(IntPtr ptr) : base(ptr) { }

        protected override void Release(IntPtr ptr) => Native.ul_catalog_destroy(ptr);
    }

    internal sealed class ResultHandle : NativeHandle
    {
        public ResultHandle(IntPtr ptr) : base(ptr) { }

        protected override void Release(IntPtr ptr) => Native.ul_result_destroy(ptr);
    }

    /// <summary>Loads a private copy of UdonLuau.dll so a new build can replace the package's file while the editor runs.</summary>
    [InitializeOnLoad]
    internal static class Native
    {
        private const string PluginGuid = "4963dd02733f4afaa9b4be0e79713c81";

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr Create();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Destroy(IntPtr handle);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AddType(IntPtr catalog, byte[] udonName, byte[] fullName, int kind, byte[] baseType, byte[] interfaces, byte[] elementType);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int AddEnumMember(IntPtr catalog, byte[] udonName, byte[] member, long value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int AddExtern(IntPtr catalog, byte[] signature, int parameterCount);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AddEvent(IntPtr catalog, byte[] name, IntPtr[] parameterNames, IntPtr[] parameterTypes, int parameterCount);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void CatalogText(IntPtr catalog, byte[] text);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int AddScriptField(IntPtr catalog, byte[] script, byte[] name, byte[] udonType, byte[] scriptType, byte[] symbol);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int AddScriptMethod(IntPtr catalog, byte[] script, byte[] name, byte[] entryPoint);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int AddScriptMethodValue(IntPtr catalog, byte[] script, byte[] method, int isReturn, byte[] name, byte[] udonType, byte[] scriptType, byte[] symbol);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AddSyncableType(IntPtr catalog, byte[] udonName, int linear, int smooth);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int SetScriptMethodFlag(IntPtr catalog, byte[] script, byte[] method, int value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int SetScriptFlag(IntPtr catalog, byte[] script, int value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetNetworkCallable(IntPtr result, int index, out NativeNetworkCallable callable);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetNetworkParameter(IntPtr result, int index, int parameter, out NativeNetworkParameter value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr CompileSource(IntPtr catalog, byte[] source, UIntPtr length);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr CompileWithDefines(IntPtr catalog, byte[] source, UIntPtr length, byte[] defines);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int ResultInt(IntPtr result);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int ResultAt(IntPtr result, int index);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr ResultText(IntPtr result);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetDiagnostic(IntPtr result, int index, out NativeDiagnostic diagnostic);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr GetBytecode(IntPtr result, out UIntPtr length);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetHeapSlot(IntPtr result, int address, out NativeHeapSlot slot);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetHeapArgument(IntPtr result, int address, int index, out NativeHeapValue value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetAttribute(IntPtr result, int address, int index, out NativeAttribute attribute);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr GetAttributeArgument(IntPtr result, int address, int index, int argument);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetEntry(IntPtr result, int index, out NativeEntryPoint entry);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetSync(IntPtr result, int index, out NativeSyncVariable variable);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetInterfaceField(IntPtr result, int index, out NativeScriptVariable variable);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetInterfaceMethod(IntPtr result, int index, out NativeScriptMethod method);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetInterfaceMethodValue(IntPtr result, int method, int isReturn, int index, out NativeScriptVariable variable);

        public static Create ul_catalog_create;
        public static Destroy ul_catalog_destroy;
        public static AddType ul_catalog_add_type;
        public static AddEnumMember ul_catalog_add_enum_member;
        public static AddExtern ul_catalog_add_extern;
        public static AddEvent ul_catalog_add_event;
        public static Destroy ul_catalog_add_standard_events;
        public static CatalogText ul_catalog_set_preferred_namespaces;
        public static CatalogText ul_catalog_add_script;
        public static AddScriptField ul_catalog_add_script_field;
        public static AddScriptMethod ul_catalog_add_script_method;
        public static AddScriptMethodValue ul_catalog_add_script_method_value;
        public static SetScriptMethodFlag ul_catalog_set_script_method_network_callable;
        public static SetScriptFlag ul_catalog_set_script_singleton;
        public static ResultInt ul_result_interface_singleton;
        public static AddSyncableType ul_catalog_add_syncable_type;
        public static ResultText ul_catalog_definitions;
        public static ResultInt ul_result_sync_mode;
        public static ResultAt ul_result_interface_method_network_callable;
        public static ResultInt ul_result_network_count;
        public static GetNetworkCallable ul_result_network;
        public static GetNetworkParameter ul_result_network_parameter;
        public static CompileWithDefines ul_extract_interface;
        public static CompileSource ul_compile;
        public static CompileWithDefines ul_compile_with_defines;
        public static Destroy ul_result_destroy;
        public static ResultInt ul_result_succeeded;
        public static ResultInt ul_result_diagnostic_count;
        public static GetDiagnostic ul_result_diagnostic;
        public static GetBytecode ul_result_bytecode;
        public static ResultInt ul_result_heap_count;
        public static GetHeapSlot ul_result_heap_slot;
        public static ResultAt ul_result_heap_argument_count;
        public static GetHeapArgument ul_result_heap_argument;
        public static ResultAt ul_result_attribute_count;
        public static GetAttribute ul_result_attribute;
        public static GetAttributeArgument ul_result_attribute_argument;
        public static ResultInt ul_result_entry_count;
        public static GetEntry ul_result_entry;
        public static ResultInt ul_result_sync_count;
        public static GetSync ul_result_sync;
        public static ResultInt ul_result_update_order;
        public static ResultInt ul_result_has_interface;
        public static ResultInt ul_result_interface_field_count;
        public static GetInterfaceField ul_result_interface_field;
        public static ResultInt ul_result_interface_method_count;
        public static GetInterfaceMethod ul_result_interface_method;
        public static GetInterfaceMethodValue ul_result_interface_method_value;
        public static ResultText ul_result_disassembly;

        private static IntPtr _module;

        static Native()
        {
            AssemblyReloadEvents.beforeAssemblyReload += Unload;
            EditorApplication.quitting += Unload;
        }

        public static int Generation { get; private set; }

        public static bool IsLoaded => _module != IntPtr.Zero;

        /// <summary>The private copy that is loaded, or null.</summary>
        public static string LoadedPath { get; private set; }

        public static event Action Unloading;

        public static byte[] Utf8(string text) => text == null ? null : Encoding.UTF8.GetBytes(text + "\0");

        public static string Read(IntPtr text) => text == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(text);

        /// <summary>Copies the package's UdonLuau.dll under Library/UdonLuau and binds its exports.</summary>
        /// <exception cref="DllNotFoundException">The library is missing, cannot be loaded or lacks a required export.</exception>
        public static void Load()
        {
            if (IsLoaded) return;

            string source = LocateLibrary();
            if (source == null || !File.Exists(source)) throw new DllNotFoundException($"UdonLuau.dll was not found at {source ?? "the package's Editor/Plugins/x86_64 folder"}");

            byte[] bytes = File.ReadAllBytes(source);
            string hash;
            using (var sha = SHA256.Create()) hash = BitConverter.ToString(sha.ComputeHash(bytes), 0, 8).Replace("-", "");

            string folder = Path.GetFullPath(Path.Combine("Library", "UdonLuau"));
            Directory.CreateDirectory(folder);
            string copy = Path.Combine(folder, $"UdonLuau-{hash}.dll");
            if (!File.Exists(copy))
            {
                string partial = copy + ".tmp";
                File.WriteAllBytes(partial, bytes);
                File.Move(partial, copy);
            }
            DeleteStaleCopies(folder, copy);

            IntPtr module = LoadLibraryW(copy);
            if (module == IntPtr.Zero) throw new DllNotFoundException($"{copy} could not be loaded: {new Win32Exception(Marshal.GetLastWin32Error()).Message}");

            foreach (FieldInfo field in typeof(Native).GetFields(BindingFlags.Public | BindingFlags.Static))
            {
                if (!field.Name.StartsWith("ul_", StringComparison.Ordinal)) continue;
                IntPtr export = GetProcAddress(module, field.Name);
                field.SetValue(null, export == IntPtr.Zero ? null : Marshal.GetDelegateForFunctionPointer(export, field.FieldType));
            }

            if (ul_catalog_create == null || ul_compile == null || ul_result_destroy == null)
            {
                FreeLibrary(module);
                throw new DllNotFoundException($"{source} does not export the UdonLuau C API");
            }

            _module = module;
            LoadedPath = copy;
            Generation++;
        }

        /// <summary>Releases native objects and frees the loaded copy.</summary>
        public static void Unload()
        {
            if (!IsLoaded) return;
            Unloading?.Invoke();
            foreach (FieldInfo field in typeof(Native).GetFields(BindingFlags.Public | BindingFlags.Static))
                if (field.Name.StartsWith("ul_", StringComparison.Ordinal)) field.SetValue(null, null);
            FreeLibrary(_module);
            _module = IntPtr.Zero;
            LoadedPath = null;
        }

        private static string LocateLibrary()
        {
            var package = UnityEditor.PackageManager.PackageInfo.FindForAssembly(typeof(Native).Assembly);
            if (package != null) return Path.Combine(package.resolvedPath, "Editor", "Plugins", "x86_64", "UdonLuau.dll");

            string asset = AssetDatabase.GUIDToAssetPath(PluginGuid);
            return string.IsNullOrEmpty(asset) ? null : Path.GetFullPath(asset);
        }

        private static void DeleteStaleCopies(string folder, string keep)
        {
            foreach (string file in Directory.GetFiles(folder, "UdonLuau-*.dll"))
            {
                if (string.Equals(file, keep, StringComparison.OrdinalIgnoreCase)) continue;
                try
                {
                    File.Delete(file);
                }
                catch (IOException)
                {
                }
                catch (UnauthorizedAccessException)
                {
                }
            }
        }

        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryW(string path);

        [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)]
        private static extern IntPtr GetProcAddress(IntPtr module, string name);

        [DllImport("kernel32")]
        private static extern bool FreeLibrary(IntPtr module);
    }
}
