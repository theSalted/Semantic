using System;
using System.IO;
using Semantic;

internal static class Program
{
    private static void MustFail<T>(Action action) where T : Exception
    {
        try { action(); }
        catch (T) { return; }
        throw new Exception("Expected " + typeof(T).Name);
    }

    private static int Main(string[] args)
    {
        try
        {
            if (args.Length == 0)
            {
                if (string.IsNullOrWhiteSpace(Native.BackendInfo)) throw new Exception("No backend information.");
                Console.WriteLine("Native backend smoke passed. Pass model.gguf bank.tsv [input] to match phrases.");
                return 0;
            }
            if (args.Length < 2) throw new ArgumentException("Usage: model.gguf bank.tsv [input]");
            using var matcher = new PhraseMatcher(args[0], args[1]);
            if (args.Length > 2)
            {
                var result = matcher.Match(string.Join(" ", args, 2, args.Length - 2));
                Console.WriteLine($"{result.Response} (cosine {result.Similarity:F6})");
                return 0;
            }
            foreach (var pair in new[] {
                ("how are you", "I am fine thank you"),
                ("How are you doing today?", "I am fine thank you"),
                ("What should I call you?", "You can call me Semantic."),
                ("bonjour", "Bonjour ! Comment ça va ?") })
            {
                var result = matcher.Match(pair.Item1);
                if (result.Response != pair.Item2 || float.IsNaN(result.Similarity))
                    throw new Exception("Unexpected match: " + result.Response);
                Console.WriteLine($"C#: {pair.Item1} -> {result.Response} ({result.Similarity:F6})");
            }
            MustFail<ArgumentException>(() => matcher.Match(" "));
            MustFail<ArgumentException>(() => matcher.Match("hello\0ignored"));
            MustFail<InvalidOperationException>(() => { using var missing = new PhraseMatcher(args[0] + ".missing", args[1]); });
            MustFail<InvalidOperationException>(() => { using var missing = new PhraseMatcher(args[0], args[1] + ".missing"); });
            string temporary = Path.GetTempFileName();
            try
            {
                foreach (string badBank in new[] { "prompt\tresponse\n", "prompt\tresponse\nonly one field\n", "prompt\tresponse\nhello\t\n" })
                {
                    File.WriteAllText(temporary, badBank);
                    MustFail<InvalidOperationException>(() => { using var invalid = new PhraseMatcher(args[0], temporary); });
                }
            }
            finally { File.Delete(temporary); }
            using var a = matcher.CreateSession();
            using var b = matcher.CreateSession();
            var first = a.Preview("how are you");
            a.Commit("how are you", first.Index);
            a.Preview("What should I call you?");
            a.Preview("What should I call you?", ContextMode.Blend);
            if (a.History().Length != 1 || b.History().Length != 0 ||
                a.History()[0].Character != first.Response || matcher.TokenLimit(true) < matcher.TokenLimit())
                throw new Exception("Session isolation, preview purity or history failed.");
            MustFail<InvalidOperationException>(() => a.Commit("hello", -1));
            if (a.History().Length != 1) throw new Exception("Failed commit changed history.");
            var retrieved = a.Retrieve("What should I call you?", 3);
            var isolated = b.Retrieve("What should I call you?", 3);
            if (retrieved.Length != 3 || retrieved[0].Index != a.Preview("What should I call you?").Index)
                throw new Exception("Top-k did not agree with input-only top-1.");
            for (int i = 0; i < retrieved.Length; ++i)
                if (retrieved[i].Index != isolated[i].Index || retrieved[i].Similarity != isolated[i].Similarity ||
                    retrieved[i].Exchanges != 0 || retrieved[i].ContextText != "What should I call you?")
                    throw new Exception("Latest-input retrieval depends on history.");
            if (a.History().Length != 1 || b.History().Length != 0) throw new Exception("Retrieval committed history.");
            MustFail<ArgumentOutOfRangeException>(() => a.Retrieve("hello", 129));
            b.CommitRendered("A new player turn", "A genuinely generated test reply.");
            if (b.History()[0].Character != "A genuinely generated test reply.")
                throw new Exception("Rendered history did not preserve the actual reply.");
            MustFail<ArgumentException>(() => b.CommitRendered("hello", "invalid\0text"));
            if (b.History().Length != 1) throw new Exception("Invalid rendered commit mutated history.");
            a.Reset();
            if (a.History().Length != 0) throw new Exception("Reset failed.");
            matcher.Dispose();
            // Native sessions retain their bank/model, and keep the same serialization gate.
            if (a.Preview("how are you").Response != "I am fine thank you") throw new Exception("Session ownership failed.");
            a.Dispose();
            MustFail<ObjectDisposedException>(() => a.Preview("hello"));
            MustFail<ObjectDisposedException>(() => matcher.Match("hello"));
            Console.WriteLine("C# phrase checks passed, including UTF-8, file errors, and disposal.");
            return 0;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine(e.Message);
            return 1;
        }
    }
}
