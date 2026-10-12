using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace Semantic
{
    public readonly struct PhraseMatch
    {
        public string Response { get; }
        public float Similarity { get; }
        internal PhraseMatch(string response, float similarity)
        {
            Response = response;
            Similarity = similarity;
        }
    }

    public sealed class PhraseMatcher : IDisposable
    {
        private readonly MatcherHandle handle;
        private readonly object gate = new object();

        public PhraseMatcher(string modelPath, string bankPath)
        {
            ValidateText(modelPath, nameof(modelPath));
            ValidateText(bankPath, nameof(bankPath));
            IntPtr pointer = Create(modelPath, bankPath);
            if (pointer == IntPtr.Zero) throw NativeError();
            handle = new MatcherHandle(pointer);
        }

        public PhraseMatch Match(string input)
        {
            ValidateText(input, nameof(input));
            lock (gate)
            {
                if (handle.IsClosed) throw new ObjectDisposedException(nameof(PhraseMatcher));
                IntPtr response = MatchNative(handle, input, out float score);
                if (response == IntPtr.Zero) throw NativeError();
                return new PhraseMatch(Marshal.PtrToStringUTF8(response)!, score);
            }
        }

        public void Dispose()
        {
            lock (gate) handle.Dispose();
        }

        private static void ValidateText(string text, string parameter)
        {
            if (string.IsNullOrWhiteSpace(text) || text.IndexOf('\0') >= 0)
                throw new ArgumentException("Provide nonempty text without null characters.", parameter);
        }

        private static Exception NativeError() => new InvalidOperationException(
            Marshal.PtrToStringUTF8(LastError()) ?? "Native matcher failed.");

        private sealed class MatcherHandle : SafeHandleZeroOrMinusOneIsInvalid
        {
            internal MatcherHandle(IntPtr pointer) : base(true) { SetHandle(pointer); }
            protected override bool ReleaseHandle() { Destroy(handle); return true; }
        }

        [DllImport("Semantic.Native", EntryPoint = "Semantic_CreateMatcher", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern IntPtr Create(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string model,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string bank);

        [DllImport("Semantic.Native", EntryPoint = "Semantic_Match", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern IntPtr MatchNative(MatcherHandle matcher,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string input, out float score);

        [DllImport("Semantic.Native", EntryPoint = "Semantic_DestroyMatcher", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern void Destroy(IntPtr matcher);

        [DllImport("Semantic.Native", EntryPoint = "Semantic_LastError", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern IntPtr LastError();
    }
}
