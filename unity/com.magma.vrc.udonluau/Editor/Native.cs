using System;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

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

    internal sealed class CatalogHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
        public CatalogHandle() : base(true) { }

        protected override bool ReleaseHandle()
        {
            Native.ul_catalog_destroy(handle);
            return true;
        }
    }

    internal sealed class ResultHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
        public ResultHandle() : base(true) { }

        protected override bool ReleaseHandle()
        {
            Native.ul_result_destroy(handle);
            return true;
        }
    }

    internal static class Native
    {
        private const string Library = "UdonLuau";

        public static byte[] Utf8(string text) => text == null ? null : Encoding.UTF8.GetBytes(text + "\0");

        public static string Read(IntPtr text) => text == IntPtr.Zero ? null : Marshal.PtrToStringUTF8(text);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern CatalogHandle ul_catalog_create();

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern void ul_catalog_destroy(IntPtr catalog);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern void ul_catalog_add_type(CatalogHandle catalog, byte[] udonName, byte[] fullName, int kind, byte[] baseType, byte[] interfaces, byte[] elementType);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ul_catalog_add_enum_member(CatalogHandle catalog, byte[] udonName, byte[] member, long value);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ul_catalog_add_extern(CatalogHandle catalog, byte[] signature, int parameterCount);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern void ul_catalog_add_event(CatalogHandle catalog, byte[] name, IntPtr[] parameterNames, IntPtr[] parameterTypes, int parameterCount);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern void ul_catalog_add_standard_events(CatalogHandle catalog);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern void ul_catalog_set_preferred_namespaces(CatalogHandle catalog, byte[] namespaces);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern ResultHandle ul_compile(CatalogHandle catalog, byte[] source, UIntPtr length);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern ResultHandle ul_compile_with_defines(CatalogHandle catalog, byte[] source, UIntPtr length, byte[] defines);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern void ul_result_destroy(IntPtr result);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ul_result_succeeded(ResultHandle result);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ul_result_diagnostic_count(ResultHandle result);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ul_result_diagnostic(ResultHandle result, int index, out NativeDiagnostic diagnostic);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr ul_result_bytecode(ResultHandle result, out UIntPtr length);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ul_result_heap_count(ResultHandle result);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ul_result_heap_slot(ResultHandle result, int address, out NativeHeapSlot slot);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ul_result_heap_argument_count(ResultHandle result, int address);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ul_result_heap_argument(ResultHandle result, int address, int index, out NativeHeapValue value);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ul_result_attribute_count(ResultHandle result, int address);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ul_result_attribute(ResultHandle result, int address, int index, out NativeAttribute attribute);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr ul_result_attribute_argument(ResultHandle result, int address, int index, int argument);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ul_result_entry_count(ResultHandle result);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ul_result_entry(ResultHandle result, int index, out NativeEntryPoint entry);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ul_result_sync_count(ResultHandle result);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ul_result_sync(ResultHandle result, int index, out NativeSyncVariable variable);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern int ul_result_update_order(ResultHandle result);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr ul_result_disassembly(ResultHandle result);
    }
}
