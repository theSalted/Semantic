using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace Semantic
{
    public enum ContextMode { InputOnly, History, Recent, Blend }

    public readonly struct PhraseMatch
    {
        public string Response { get; }
        public float Similarity { get; }
        public int Index { get; }
        public int Tokens { get; }
        public int Exchanges { get; }
        public string ContextText { get; }
        internal PhraseMatch(string response, float similarity, int index = -1, int tokens = 0,
                             int exchanges = 0, string contextText = "")
        {
            Response = response;
            Similarity = similarity;
            Index = index; Tokens = tokens; Exchanges = exchanges; ContextText = contextText;
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

        public PhraseMatcher(string modelPath, IEnumerable<CatalogResponse> responses, IEnumerable<CatalogKey> keys)
        {
            ValidateText(modelPath, nameof(modelPath));
            if (responses == null || keys == null) throw new ArgumentNullException("Catalog and keys are required.");
            var entries = responses.ToArray(); var triggers = keys.ToArray();
            using var ids = new Utf8Array(entries.Select(x => x.Id));
            using var texts = new Utf8Array(entries.Select(x => x.Text));
            using var keyIds = new Utf8Array(triggers.Select(x => x.Id));
            using var responseIds = new Utf8Array(triggers.Select(x => x.ResponseId));
            using var keyTexts = new Utf8Array(triggers.Select(x => x.Text));
            var pointer = CreateCatalog(modelPath, ids.Values, texts.Values, entries.Length,
                keyIds.Values, responseIds.Values, keyTexts.Values, triggers.Length, 0);
            if (pointer == IntPtr.Zero) throw NativeError();
            handle = new MatcherHandle(pointer);
        }

        private sealed class Utf8Array : IDisposable
        {
            internal readonly IntPtr[] Values;
            internal Utf8Array(IEnumerable<string> items)
            {
                var texts = items.ToArray();
                Values = new IntPtr[texts.Length];
                try
                {
                    for (int i = 0; i < texts.Length; ++i)
                    { ValidateText(texts[i], "catalog field"); Values[i] = Marshal.StringToCoTaskMemUTF8(texts[i]); }
                }
                catch { Dispose(); throw; }
            }
            public void Dispose() { foreach (var p in Values) if (p != IntPtr.Zero) Marshal.FreeCoTaskMem(p); }
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

        public Session CreateSession(int tokenBudget = 0)
        {
            lock (gate)
            {
                if (handle.IsClosed) throw new ObjectDisposedException(nameof(PhraseMatcher));
                var pointer = CreateSessionNative(handle, tokenBudget);
                if (pointer == IntPtr.Zero) throw NativeError();
                return new Session(pointer, gate);
            }
        }

        public int TokenLimit(bool modelLimit = false)
        {
            lock (gate)
            {
                if (handle.IsClosed) throw new ObjectDisposedException(nameof(PhraseMatcher));
                return GetTokenLimit(handle, modelLimit ? 1 : 0);
            }
        }

        public sealed class Session : IDisposable
        {
            private readonly SessionHandle handle;
            private readonly object gate;
            internal Session(IntPtr pointer, object sharedGate)
            { handle = new SessionHandle(pointer); gate = sharedGate; }
            private void Check() { if (handle.IsClosed) throw new ObjectDisposedException(nameof(Session)); }

            public PhraseMatch Preview(string input, ContextMode mode = ContextMode.InputOnly)
            {
                ValidateText(input, nameof(input));
                lock (gate)
                {
                    Check();
                    if (PreviewNative(handle, input, mode, out var result) == 0) throw NativeError();
                    return new PhraseMatch(Marshal.PtrToStringUTF8(result.Response)!, result.Score,
                        result.Index, result.Tokens, result.Exchanges, Marshal.PtrToStringUTF8(result.Text)!);
                }
            }
            // Index must be from this session's immutable bank snapshot.
            public PhraseMatch[] Retrieve(string input, int count = 12)
            {
                ValidateText(input, nameof(input));
                if (count < 1 || count > 128) throw new ArgumentOutOfRangeException(nameof(count));
                lock (gate)
                {
                    Check();
                    var native = new NativeResult[count];
                    int written = RetrieveNative(handle, input, count, native);
                    if (written < 0) throw NativeError();
                    var result = new PhraseMatch[written];
                    for (int i = 0; i < written; ++i)
                        result[i] = new PhraseMatch(Marshal.PtrToStringUTF8(native[i].Response)!, native[i].Score,
                            native[i].Index, native[i].Tokens, 0, Marshal.PtrToStringUTF8(native[i].Text)!);
                    return result;
                }
            }
            public CatalogMatch[] RetrieveResponses(string input, int count = 12, ContextMode mode = ContextMode.InputOnly)
            {
                ValidateText(input, nameof(input));
                if (count < 1 || count > 128) throw new ArgumentOutOfRangeException(nameof(count));
                lock (gate)
                {
                    Check();
                    var native = new NativeCatalogResult[count];
                    var written = RetrieveResponsesNative(handle, input, mode, count, native);
                    if (written < 0) throw NativeError();
                    var result = new CatalogMatch[written];
                    for (int i = 0; i < written; ++i)
                        result[i] = new CatalogMatch(Marshal.PtrToStringUTF8(native[i].Id)!,
                            Marshal.PtrToStringUTF8(native[i].Response)!, Marshal.PtrToStringUTF8(native[i].KeyId)!,
                            Marshal.PtrToStringUTF8(native[i].KeyText)!, native[i].Score, native[i].Tokens,
                            native[i].Exchanges, Marshal.PtrToStringUTF8(native[i].Context)!);
                    return result;
                }
            }
            public void CommitResponse(string input, string responseId)
            {
                ValidateText(input, nameof(input)); ValidateText(responseId, nameof(responseId));
                lock (gate) { Check(); if (CommitResponseNative(handle, input, responseId) == 0) throw NativeError(); }
            }
            // Caller validates externally rendered text and labels generation separately.
            public void CommitRendered(string input, string response)
            {
                ValidateText(input, nameof(input)); ValidateText(response, nameof(response));
                lock (gate) { Check(); if (CommitRenderedNative(handle, input, response) == 0) throw NativeError(); }
            }
            public void Commit(string input, int selectedIndex)
            {
                ValidateText(input, nameof(input));
                lock (gate) { Check(); if (CommitNative(handle, input, selectedIndex) == 0) throw NativeError(); }
            }
            public void Reset() { lock (gate) { Check(); ResetNative(handle); } }
            public (string User, string Character)[] History()
            {
                lock (gate)
                {
                    Check();
                    var result = new (string User, string Character)[HistoryCount(handle)];
                    for (int i = 0; i < result.Length; ++i)
                        result[i] = (Marshal.PtrToStringUTF8(HistoryText(handle, i, 0))!,
                                     Marshal.PtrToStringUTF8(HistoryText(handle, i, 1))!);
                    return result;
                }
            }
            public void Dispose() { lock (gate) handle.Dispose(); }
        }
        private sealed class SessionHandle : SafeHandleZeroOrMinusOneIsInvalid
        {
            internal SessionHandle(IntPtr pointer) : base(true) { SetHandle(pointer); }
            protected override bool ReleaseHandle() { DestroySession(handle); return true; }
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct NativeResult
        {
            internal IntPtr Response;
            internal float Score;
            internal int Index, Tokens, Exchanges;
            internal IntPtr Text;
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct NativeCatalogResult
        {
            internal IntPtr Id, Response, KeyId, KeyText;
            internal float Score;
            internal int Tokens, Exchanges;
            internal IntPtr Context;
        }
        [DllImport("Semantic.Native", EntryPoint = "Semantic_CreateCatalogMatcher", CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr CreateCatalog([MarshalAs(UnmanagedType.LPUTF8Str)] string model,
            [In] IntPtr[] ids, [In] IntPtr[] texts, int responseCount, [In] IntPtr[] keyIds,
            [In] IntPtr[] keyResponseIds, [In] IntPtr[] keyTexts, int keyCount, int capacity);
        [DllImport("Semantic.Native", EntryPoint = "Semantic_RetrieveResponses", CallingConvention = CallingConvention.Cdecl)]
        private static extern int RetrieveResponsesNative(SessionHandle session, [MarshalAs(UnmanagedType.LPUTF8Str)] string input,
            ContextMode mode, int count, [Out] NativeCatalogResult[] results);
        [DllImport("Semantic.Native", EntryPoint = "Semantic_CommitResponse", CallingConvention = CallingConvention.Cdecl)]
        private static extern int CommitResponseNative(SessionHandle session, [MarshalAs(UnmanagedType.LPUTF8Str)] string input,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string responseId);
        [DllImport("Semantic.Native", EntryPoint = "Semantic_CreateSession", CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr CreateSessionNative(MatcherHandle matcher, int budget);
        [DllImport("Semantic.Native", EntryPoint = "Semantic_TokenLimit", CallingConvention = CallingConvention.Cdecl)]
        private static extern int GetTokenLimit(MatcherHandle matcher, int trained);
        [DllImport("Semantic.Native", EntryPoint = "Semantic_DestroySession", CallingConvention = CallingConvention.Cdecl)]
        private static extern void DestroySession(IntPtr session);
        [DllImport("Semantic.Native", EntryPoint = "Semantic_ResetSession", CallingConvention = CallingConvention.Cdecl)]
        private static extern void ResetNative(SessionHandle session);
        [DllImport("Semantic.Native", EntryPoint = "Semantic_Preview", CallingConvention = CallingConvention.Cdecl)]
        private static extern int PreviewNative(SessionHandle session, [MarshalAs(UnmanagedType.LPUTF8Str)] string input,
            ContextMode mode, out NativeResult result);
        [DllImport("Semantic.Native", EntryPoint = "Semantic_Commit", CallingConvention = CallingConvention.Cdecl)]
        private static extern int CommitNative(SessionHandle session, [MarshalAs(UnmanagedType.LPUTF8Str)] string input, int index);
        [DllImport("Semantic.Native", EntryPoint = "Semantic_Retrieve", CallingConvention = CallingConvention.Cdecl)]
        private static extern int RetrieveNative(SessionHandle session, [MarshalAs(UnmanagedType.LPUTF8Str)] string input,
            int count, [Out] NativeResult[] results);
        [DllImport("Semantic.Native", EntryPoint = "Semantic_CommitRendered", CallingConvention = CallingConvention.Cdecl)]
        private static extern int CommitRenderedNative(SessionHandle session, [MarshalAs(UnmanagedType.LPUTF8Str)] string input,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string response);
        [DllImport("Semantic.Native", EntryPoint = "Semantic_HistoryCount", CallingConvention = CallingConvention.Cdecl)]
        private static extern int HistoryCount(SessionHandle session);
        [DllImport("Semantic.Native", EntryPoint = "Semantic_HistoryText", CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr HistoryText(SessionHandle session, int index, int character);

        public void Dispose()
        {
            lock (gate) handle.Dispose();
        }

        private static void ValidateText(string text, string parameter)
        {
            if (string.IsNullOrWhiteSpace(text) || text.IndexOf('\0') >= 0)
                throw new ArgumentException("Provide nonempty text without null characters.", parameter);
            new System.Text.UTF8Encoding(false, true).GetByteCount(text);
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
