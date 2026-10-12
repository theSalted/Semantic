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
            matcher.Dispose();
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
