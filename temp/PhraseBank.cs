using System.Text;
using System.Security.Cryptography;
using Semantic;

record Phrase(string? Prompt, string? Response);
record SaveResult(int Added, int Skipped);
record BankView(string Revision, IReadOnlyList<Phrase> Phrases);

sealed class PhraseBank : IDisposable
{
    private static readonly UTF8Encoding Utf8 = new(false, true);
    private readonly object gate = new();
    private readonly string modelPath;
    private readonly string bankPath;
    private PhraseMatcher? matcher;

    public PhraseBank(string modelPath, string bankPath)
    {
        this.modelPath = modelPath;
        this.bankPath = bankPath;
        if (Parse(File.ReadAllText(bankPath, Utf8), allowEmpty: true).Count > 0)
            matcher = new PhraseMatcher(modelPath, bankPath);
    }

    public string Match(string? input)
    {
        if (string.IsNullOrWhiteSpace(input)) throw new ArgumentException("Enter a phrase.");
        if (input.Length > 1000) throw new ArgumentException("Use at most 1,000 characters.");
        lock (gate) return matcher?.Match(input.Trim()).Response
            ?? throw new InvalidOperationException("Add a phrase before matching.");
    }

    public BankView View()
    {
        lock (gate)
        {
            var contents = File.ReadAllText(bankPath, Utf8);
            return new BankView(Revision(contents), Parse(contents, allowEmpty: true));
        }
    }

    public BankView Edit(int index, string? revision, Phrase phrase)
    {
        lock (gate)
        {
            var original = File.ReadAllText(bankPath, Utf8);
            var phrases = Editable(original, revision, index);
            var prompt = Validate(phrase.Prompt, "Prompt", 1000);
            var response = Validate(phrase.Response, "Response", 8000);
            if (!string.Equals(phrases[index].Prompt!.Trim(), prompt, StringComparison.OrdinalIgnoreCase) &&
                phrases.Where((_, i) => i != index).Any(p =>
                string.Equals(p.Prompt!.Trim(), prompt, StringComparison.OrdinalIgnoreCase)))
                throw new ArgumentException("That prompt already exists. Nothing was saved.");
            phrases[index] = new Phrase(prompt, response);
            return Save(original, phrases);
        }
    }

    public BankView Delete(int index, string? revision)
    {
        lock (gate)
        {
            var original = File.ReadAllText(bankPath, Utf8);
            var phrases = Editable(original, revision, index);
            phrases.RemoveAt(index);
            return Save(original, phrases);
        }
    }

    private static List<Phrase> Editable(string original, string? revision, int index)
    {
        if (revision != Revision(original))
            throw new InvalidOperationException("The bank changed. Reload the page before editing or deleting.");
        var phrases = Parse(original, allowEmpty: true);
        if (index < 0 || index >= phrases.Count) throw new ArgumentException("That phrase no longer exists.");
        return phrases;
    }

    private static string Revision(string contents) => Convert.ToHexString(SHA256.HashData(Utf8.GetBytes(contents)));

    public SaveResult Add(IReadOnlyList<Phrase> incoming)
    {
        if (incoming.Count == 0) throw new ArgumentException("No phrases to add.");
        lock (gate)
        {
            // Reread so additions also preserve edits made outside the playground.
            var original = File.ReadAllText(bankPath, Utf8);
            var combined = Parse(original, allowEmpty: true);
            var known = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var phrase in combined) known.TryAdd(phrase.Prompt!.Trim(), phrase.Response!);
            var added = 0;
            var skipped = 0;
            foreach (var phrase in incoming)
            {
                var prompt = Validate(phrase.Prompt, "Prompt", 1000);
                var response = Validate(phrase.Response, "Response", 8000);
                if (known.TryGetValue(prompt, out var existing))
                {
                    if (existing != response)
                        throw new ArgumentException($"“{prompt}” already has a different response. Nothing was saved.");
                    skipped++;
                    continue;
                }
                known.Add(prompt, response);
                combined.Add(new Phrase(prompt, response));
                added++;
            }

            Save(original, combined);
            return new SaveResult(added, skipped);
        }
    }

    private BankView Save(string original, IReadOnlyList<Phrase> phrases)
    {
        var contents = "prompt\tresponse\n" + string.Concat(phrases.Select(p => $"{p.Prompt}\t{p.Response}\n"));
        if (Utf8.GetByteCount(contents) > 1024 * 1024)
            throw new ArgumentException("The phrase bank would exceed 1 MB. Nothing was saved.");
        var candidate = bankPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
        PhraseMatcher? replacement = null;
        try
        {
            File.WriteAllText(candidate, contents, Utf8);
            // Validate and embed before replacing the bank. An empty bank has no matcher.
            if (phrases.Count > 0) replacement = new PhraseMatcher(modelPath, candidate);
            if (File.ReadAllText(bankPath, Utf8) != original)
                throw new InvalidOperationException("The phrase bank changed while saving. Try again.");
            File.Move(candidate, bankPath, overwrite: true);
            var previous = matcher;
            matcher = replacement;
            replacement = null;
            previous?.Dispose();
            return new BankView(Revision(contents), phrases);
        }
        finally
        {
            replacement?.Dispose();
            if (File.Exists(candidate)) File.Delete(candidate);
        }
    }

    public static List<Phrase> Parse(string? tsv, bool allowEmpty = false)
    {
        if (string.IsNullOrEmpty(tsv)) throw new ArgumentException("Choose a TSV file containing phrases.");
        if (Utf8.GetByteCount(tsv) > 1024 * 1024) throw new ArgumentException("Use a TSV file under 1 MB.");
        using var reader = new StringReader(tsv.TrimStart('\uFEFF'));
        if (reader.ReadLine() != "prompt\tresponse")
            throw new FormatException("The first TSV row must be prompt and response, separated by a tab.");
        var result = new List<Phrase>();
        var row = 1;
        while (reader.ReadLine() is { } line)
        {
            row++;
            if (string.IsNullOrWhiteSpace(line)) continue;
            var fields = line.Split('\t');
            if (fields.Length != 2) throw new FormatException($"Row {row} needs one prompt and one response, separated by a tab.");
            Validate(fields[0], $"Prompt on row {row}", 1000);
            Validate(fields[1], $"Response on row {row}", 8000);
            result.Add(new Phrase(fields[0], fields[1]));
        }
        if (!allowEmpty && result.Count == 0) throw new FormatException("The TSV file contains no phrases.");
        return result;
    }

    private static string Validate(string? value, string name, int maximum)
    {
        if (string.IsNullOrWhiteSpace(value)) throw new ArgumentException($"{name} is required.");
        if (value.IndexOfAny(['\t', '\r', '\n', '\0']) >= 0)
            throw new ArgumentException($"{name} cannot contain tabs, newlines, or null characters.");
        if (value.Length > maximum) throw new ArgumentException($"{name} must be at most {maximum:N0} characters.");
        return value.Trim();
    }

    public void Dispose() { lock (gate) matcher?.Dispose(); }
}
