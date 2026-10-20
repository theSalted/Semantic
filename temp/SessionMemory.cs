using System.Text.RegularExpressions;

record PlayerFact(string Key, string Value, string Evidence)
{
    public string Id => "player:" + Key;
}

// Bounded, explicit declarations only. No inference from assistant text, quotes, or third parties.
static class SessionMemory
{
    public const int MaxFacts = 7;
    static readonly RegexOptions Flags = RegexOptions.IgnoreCase | RegexOptions.CultureInvariant;
    static readonly (string Key, string Pattern)[] Declarations = [
        ("name", @"^(?:my name is|my name['’]s|I(?: am|['’]m) called|(?:please )?call me|you can call me) (?<value>.+)$"),
        ("favorite_drink", @"^my favou?rite drink is (?<value>.+)$"),
        ("favorite_food", @"^my favou?rite food is (?<value>.+)$"),
        ("favorite_color", @"^my favou?rite colou?r is (?<value>.+)$"),
        ("home", @"^I live in (?<value>.+)$"),
        ("origin", @"^I(?: am|['’]m) from (?<value>.+)$"),
        ("lost_item", @"^I lost (?<value>.+)$")
    ];
    static readonly Dictionary<string, string> Questions = new(StringComparer.OrdinalIgnoreCase) {
        ["what is my name"] = "name", ["what's my name"] = "name", ["what was my name"] = "name",
        ["do you remember my name"] = "name", ["tell me my name"] = "name", ["what did I say my name was"] = "name",
        ["what is my favorite drink"] = "favorite_drink", ["what's my favorite drink"] = "favorite_drink",
        ["what is my favorite food"] = "favorite_food", ["what's my favorite food"] = "favorite_food",
        ["what is my favorite color"] = "favorite_color", ["what's my favorite color"] = "favorite_color",
        ["where do I live"] = "home", ["where am I from"] = "origin", ["what did I lose"] = "lost_item"
    };
    public static PlayerFact[] Advance(PlayerFact[] current, string input)
    {
        // Quoted roleplay, embedded dialogue and hypothetical statements are not evidence of identity.
        if (input.IndexOfAny(['"', '“', '”', '«', '»', '`', '\n', '\r']) >= 0) return current;
        var facts = current.ToDictionary(f => f.Key, StringComparer.Ordinal);
        foreach (var raw in Regex.Split(input, @"[.!?;]+"))
        {
            string clause = raw.Trim();
            clause = Regex.Replace(clause, @"^(?:hi|hello|hey)[,!]?\s+", "", Flags);
            clause = Regex.Replace(clause, @"^(?:actually|correction|to correct that)[,:]?\s+", "", Flags);
            if (Regex.IsMatch(clause, @"^(?:please )?(?:forget my name|do not remember my name|don't remember my name)$", Flags))
            { facts.Remove("name"); continue; }
            var denial = Regex.Match(clause, @"^(?:my name (?:is not|isn't)|I(?: am not|'m not)) (?<old>.+)$", Flags);
            if (denial.Success)
            {
                if (facts.TryGetValue("name", out var known) && string.Equals(known.Value, denial.Groups["old"].Value.Trim(), StringComparison.OrdinalIgnoreCase)) facts.Remove("name");
                continue;
            }
            bool recognized = false;
            foreach (var (key, pattern) in Declarations)
            {
                var match = Regex.Match(clause, pattern, Flags);
                if (!match.Success) continue;
                recognized = true;
                string value = match.Groups["value"].Value.Trim();
                if (value.Length == 0 || value.Length > 120 || Regex.IsMatch(value, @"\b(?:if|unless|maybe|perhaps|hypothetically|suppose|not|isn't|wasn't|and|but|because|while|when)\b", Flags)) continue;
                if (key == "name" && (value.Length > 64 || !Regex.IsMatch(value, @"^[\p{L}\p{M}]+(?:[ '\u2019-][\p{L}\p{M}]+){0,5}$") ||
                    Regex.IsMatch(value, @"\b(?:a|an|the|and|but|secret|private|unknown|none|definitely|am|is|are|was|were|it|this|that|you|me|my|here|there|Dr|Mr|Mrs|Ms)\b", Flags))) continue;
                facts[key] = new(key, value, raw.Trim());
                break;
            }
            // Do not extract an apparent first-person statement after an attribution/context sentence.
            if (!recognized && clause.Length > 0 && !Regex.IsMatch(clause, @"^(?:hello|hi|hey)$", Flags)) break;
        }
        // Fixed allowlist bounds cardinality; name is always retained and ordered first.
        return facts.Values.OrderBy(f => f.Key == "name" ? "" : f.Key, StringComparer.Ordinal).Take(MaxFacts).ToArray();
    }
    public static string? NameAcknowledgement(string input, PlayerFact[] before, PlayerFact[] after)
    {
        var prior = before.SingleOrDefault(f => f.Key == "name");
        var current = after.SingleOrDefault(f => f.Key == "name");
        if (current is not null && prior?.Value != current.Value &&
            string.Equals(input.Trim().TrimEnd('.', '!'), current.Evidence.Trim().TrimEnd('.', '!'), StringComparison.Ordinal))
            return "All right, I'll call you " + current.Value + ".";
        if (prior is not null && current is null) return "All right, I won't use that name.";
        return null;
    }
    public static (string Key, bool Known, string Response)? Recall(string input, PlayerFact[] facts)
    {
        string query = input.Trim().TrimEnd('?', '.', '!').Replace('’', '\'');
        query = Regex.Replace(query, @"\s+", " ").Replace("favourite", "favorite", StringComparison.OrdinalIgnoreCase).Replace("colour", "color", StringComparison.OrdinalIgnoreCase);
        if (!Questions.TryGetValue(query, out string? key)) return null;
        var fact = facts.SingleOrDefault(f => f.Key == key);
        string label = key switch { "name" => "name", "favorite_drink" => "favorite drink", "favorite_food" => "favorite food", "favorite_color" => "favorite color", "home" => "where you live", "origin" => "where you're from", _ => "what you lost" };
        if (fact is null) return (key, false, "You haven't told me " + (key is "name" or "favorite_drink" or "favorite_food" or "favorite_color" ? "your " : "") + label + " yet.");
        string reply = key switch {
            "home" => "You told me you live in " + fact.Value + ".",
            "origin" => "You told me you're from " + fact.Value + ".",
            "lost_item" => "You told me you lost " + fact.Value + ".",
            _ => "You told me your " + label + " is " + fact.Value + "."
        };
        return (key, true, reply);
    }
}
