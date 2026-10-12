using System;
using System.Runtime.InteropServices;

namespace Semantic
{
    public static class Native
    {
        public static string? BackendInfo => Marshal.PtrToStringAnsi(GetBackendInfo());

        [DllImport("Semantic.Native", EntryPoint = "Semantic_BackendInfo",
            CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern IntPtr GetBackendInfo();
    }
}
