using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

record FastPathSettings(bool Enabled, double MinSimilarity, double MinMargin, string[] EligiblePrompts);
record FastPathDecision(bool Accepted, string Reason, float? Similarity = null, double? Margin = null, string? SelectedId = null);

sealed class RetrievalFastPath
{
    readonly FastPathSettings settings;
    readonly HashSet<string> eligible;
    readonly HashSet<string> ambiguousTags;
    public RetrievalFastPath(FastPathSettings settings, IEnumerable<string>? ambiguousTags = null)
    {
        if (!double.IsFinite(settings.MinSimilarity) || settings.MinSimilarity < 0 || settings.MinSimilarity > 1 ||
            !double.IsFinite(settings.MinMargin) || settings.MinMargin < 0 || settings.MinMargin > 1 || settings.EligiblePrompts is null)
            throw new ArgumentException("Fast path needs finite similarity/margin in [0,1] and reviewed eligible prompts.");
        this.settings = settings;
        eligible = settings.EligiblePrompts.Select(Normalize).ToHashSet(StringComparer.Ordinal);
        this.ambiguousTags = (ambiguousTags ?? []).Select(Normalize).ToHashSet(StringComparer.Ordinal);
    }
    public static FastPathSettings Load(string path) => JsonSerializer.Deserialize<FastPathSettings>(File.ReadAllText(path), LocalMethods.Json) ?? throw new ArgumentException("Empty fast-path configuration.");
    public static string Normalize(string input) => Regex.Replace(input.Normalize(NormalizationForm.FormC).Trim().TrimEnd('.', '!', '?').Replace('’', '\'').ToLowerInvariant(), @"\s+", " ");
    public FastPathDecision Evaluate(string input, DialogueTurn[] history, ReplyCandidate[] candidates, IReadOnlyList<Phrase> rows)
    {
        if (!settings.Enabled) return new(false, "disabled");
        string normalized = Normalize(input);
        if (Regex.IsMatch(input, @"\b(?:not|no|never|without|instead|except|but|actually|don't|won't|can't|isn't|it|that|this|those|them|same|again|name|remember)\b", RegexOptions.IgnoreCase) || input.IndexOfAny(['"','“','”','`','\n','\r']) >= 0)
            return new(false, "negation, memory, reference, quote or correction");
        if (!eligible.Contains(normalized)) return new(false, "not a reviewed standalone trigger");
        if (history.Any(t => Regex.IsMatch(t.User, @"\b(?:not|no|never|without|instead|except|but|actually|don't|won't|can't|allergic|allergy)\b", RegexOptions.IgnoreCase)))
            return new(false, "retained user history needs contextual judgment");
        var exact = rows.Select((row, index) => (row, index)).Where(x => Normalize(x.row.Prompt!) == normalized).ToArray();
        if (exact.Length == 0) return new(false, "not an exact normalized authored trigger");
        if (ambiguousTags.Contains(normalized) || exact.Select(x => x.row.Response).Distinct(StringComparer.Ordinal).Count() != 1)
            return new(false, "conflicting authored mappings or tags");
        var distinct = candidates.GroupBy(c => c.Text, StringComparer.Ordinal).Select(g => g.First()).ToArray();
        if (distinct.Length < 2 || distinct[0].Similarity is not { } score || distinct[1].Similarity is not { } runnerUp || !float.IsFinite(score) || !float.IsFinite(runnerUp))
            return new(false, "insufficient scored distinct replies");
        double margin = score - runnerUp;
        if (!exact.Any(x => x.index.ToString(System.Globalization.CultureInfo.InvariantCulture) == distinct[0].Id))
            return new(false, "retrieval winner differs from exact trigger", score, margin);
        if (score < settings.MinSimilarity) return new(false, "below absolute similarity gate", score, margin);
        if (margin < settings.MinMargin) return new(false, "distinct reply margin too small", score, margin);
        return new(true, "exact reviewed trigger and both score gates passed", score, margin, distinct[0].Id);
    }
}
