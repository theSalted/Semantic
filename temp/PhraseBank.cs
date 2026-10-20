using System.Text;
using System.Diagnostics;
using System.Security.Cryptography;
using Semantic;

record Phrase(string? Prompt, string? Response);
record SaveResult(int Added, int Skipped);
record BankView(string Revision, IReadOnlyList<Phrase> Phrases, object? Catalog = null, bool ReadOnly = false);

sealed class PhraseBank : IDisposable
{
    private static readonly UTF8Encoding Utf8 = new(false, true);
    private readonly object gate = new();
    private readonly string modelPath;
    private readonly string bankPath;
    private readonly CatalogData? catalog;
    private readonly bool sessionMemory;
    private readonly RetrievalFastPath? fastPath;
    private PhraseMatcher? matcher;
    private sealed class Conversation(PhraseMatcher.Session native, string? method)
    {
        public readonly PhraseMatcher.Session Native = native;
        public readonly string? Method = method;
        public DateTime Seen = DateTime.UtcNow;
        public int Generation;
        public bool HasTurns;
        public PlayerFact[] Memory = [];
        public CancellationTokenSource? Flight;
        public string? LastRequest, LastInput;
        public object? LastResult;
    }
    private readonly Dictionary<string, Conversation> sessions = new();
    private BankView snapshot;
    private void ClearSessions()
    {
        foreach (var entry in sessions.Values) { entry.Flight?.Cancel(); entry.Native.Dispose(); }
        sessions.Clear();
    }
    private void ExpireSessions()
    {
        foreach (var key in sessions.Where(p => p.Value.Flight is null && p.Value.Seen < DateTime.UtcNow.AddMinutes(-30)).Select(p => p.Key).ToArray())
        { sessions[key].Native.Dispose(); sessions.Remove(key); }
    }
    private void EnsureBankCurrent()
    {
        if (catalog is not null) { catalog.EnsureCurrent(); return; }
        if (Revision(File.ReadAllText(bankPath, Utf8)) != snapshot.Revision)
            throw new InvalidOperationException("The bank changed on disk. Restart the playground to reload it before continuing.");
    }
    public MethodInfo[] AvailableMethods(MethodInfo[] methods) => catalog?.Keys.Length == 0
        ? methods.Select(m => !LocalMethods.IsPaper(m.Id)
            ? m with { Available = false, Note = "No reviewed keys are indexed. The full response catalog is retained." } : m).ToArray()
        : methods;
    private void RequireEditable()
    {
        if (catalog is not null) throw new InvalidOperationException("Catalog imports are read-only. Edit and revalidate the source artifact, then restart.");
    }
    public object NewSession(string? methodId = null)
    {
        lock (gate)
        {
            ExpireSessions(); EnsureBankCurrent();
            if (methodId is not null && !LocalMethods.IsKnown(methodId)) throw new ArgumentException("Unknown method.");
            if (matcher is null) throw new InvalidOperationException("Add a phrase before starting a conversation.");
            if (sessions.Count >= 128) throw new InvalidOperationException("Too many active conversations. Try again later.");
            var id = Guid.NewGuid().ToString("N");
            var limit = 512; // Shared framework history budget; adapters apply their own prompt limits too.
            sessions.Add(id, new Conversation(matcher.CreateSession(limit), methodId));
            return new { sessionId = id, tokenLimit = limit, modelTokenLimit = matcher.TokenLimit(true), methodId };
        }
    }
    private Conversation GetSession(string? id)
    {
        ExpireSessions();
        if (id is null || !sessions.TryGetValue(id, out var entry))
            throw new ArgumentException("Conversation expired or the bank changed. Start a new conversation.");
        entry.Seen = DateTime.UtcNow;
        return entry;
    }
    public object Reset(string? id)
    {
        lock (gate)
        {
            var entry = GetSession(id); entry.Generation++; entry.Flight?.Cancel(); entry.Flight = null;
            entry.Native.Reset(); entry.Memory = []; entry.HasTurns = false; entry.LastRequest = null; entry.LastInput = null; entry.LastResult = null;
            return new { reset = true };
        }
    }
    public object Close(string? id)
    {
        lock (gate)
        {
            var entry = GetSession(id); entry.Generation++; entry.Flight?.Cancel();
            sessions.Remove(id!); entry.Native.Dispose();
            return new { closed = true };
        }
    }
    public object Match(string? input, string? id, int mode, bool preview)
    {
        if (catalog is not null) throw new InvalidOperationException("Catalog sessions use /api/respond with stable response IDs.");
        LocalMethods.ValidateText(input, 1000, "Input");
        if (mode < 0 || mode > 3) throw new ArgumentException("Choose a valid context mode.");
        lock (gate)
        {
            EnsureBankCurrent(); var entry = GetSession(id);
            if (entry.Method is not null || entry.Flight is not null) throw new InvalidOperationException("This session belongs to a comparison method. Use /api/respond.");
            var session = entry.Native; input = input!.Trim();
            var baseline = session.Preview(input, ContextMode.InputOnly);
            var selected = mode == 0 ? baseline : session.Preview(input, (ContextMode)mode);
            if (!preview) { session.Commit(input, selected.Index); entry.HasTurns = true; }
            return new { selected, baseline, preview, history = History(session) };
        }
    }
    private static DialogueTurn[] History(PhraseMatcher.Session session) => session.History().Select(t => new DialogueTurn(t.User, t.Character)).ToArray();
    private record RankedReplies(ReplyCandidate[] Candidates, int Tokens, int Exchanges, string ContextText);
    private RankedReplies Retrieve(PhraseMatcher.Session session, string input, ContextMode mode, int count)
    {
        if (catalog is not null)
        {
            var rows = session.RetrieveResponses(input, count, mode);
            if (rows.Length == 0) throw new InvalidOperationException("No reviewed keys are indexed; response-only methods can still use the full catalog.");
            return new(rows.Select(r => new ReplyCandidate(r.ResponseId, r.Response, r.Similarity, new(r.KeyId, r.KeyText))).ToArray(),
                rows[0].Tokens, rows[0].Exchanges, rows[0].ContextText);
        }
        PhraseMatch[] matches = count == 1 ? [session.Preview(input, mode)] : session.Retrieve(input, count);
        return new(matches.Select(r => new ReplyCandidate(r.Index.ToString(System.Globalization.CultureInfo.InvariantCulture), r.Response, r.Similarity)).ToArray(),
            matches[0].Tokens, matches[0].Exchanges, matches[0].ContextText);
    }
    public async Task<object> Respond(string? input, string? id, string? method, string? requestId, LocalMethods methods, bool preview, CancellationToken ct)
    {
        LocalMethods.ValidateText(input, 1000, "Input"); input = input!.Trim();
        if (method is null || !LocalMethods.IsKnown(method)) throw new ArgumentException("Unknown method.");
        if (catalog?.Keys.Length == 0 && !LocalMethods.IsPaper(method))
            throw new InvalidOperationException("No reviewed keys are indexed; response-only methods retain the full catalog.");
        if (!Guid.TryParse(requestId, out _)) throw new ArgumentException("A unique requestId is required.");
        var preparation = Stopwatch.StartNew();
        Conversation entry; int epoch; CancellationTokenSource flight; MethodInput prepared; MethodReply? reply = null; PlayerFact[]? memory = null; string? requiredMemoryResponse = null; FastPathDecision? fastDecision = null;
        lock (gate)
        {
            EnsureBankCurrent(); entry = GetSession(id);
            if (entry.Method != method) throw new ArgumentException("Session is bound to another method. Create one session per method.");
            if (entry.LastRequest == requestId)
            {
                if (entry.LastInput != input || preview) throw new ArgumentException("Request ID was already used with different content.");
                return entry.LastResult!;
            }
            if (entry.Flight is not null) throw new InvalidOperationException("This method already has a pending turn.");
            var history = History(entry.Native);
            ReplyCandidate[] candidates = [];
            if (method is "input" or "history" or "recent" or "blend")
            {
                var mode = method switch { "history" => ContextMode.History, "recent" => ContextMode.Recent, "blend" => ContextMode.Blend, _ => ContextMode.InputOnly };
                var result = Retrieve(entry.Native, input, mode, 1);
                var selected = result.Candidates[0];
                reply = new(selected.Text, "selected", selected.Id, new() {
                    ["contextTokens"] = result.Tokens, ["historyExchangesUsed"] = result.Exchanges,
                    ["similarity"] = selected.Similarity, ["contextText"] = result.ContextText, ["contextLimit"] = 512,
                    ["matchedKey"] = selected.MatchedKey });
            }
            else if (method is "hybrid" or "llm_id") candidates = Retrieve(entry.Native, input, ContextMode.InputOnly, method == "hybrid" ? 12 : 32).Candidates;
            else if (LocalMethods.IsPaper(method)) candidates = catalog is not null
                ? catalog.Responses.Select(r => new ReplyCandidate(r.Id, r.Text)).ToArray()
                : snapshot.Phrases.Select((p, i) => new ReplyCandidate(i.ToString(System.Globalization.CultureInfo.InvariantCulture), p.Response!)).ToArray();
            if (sessionMemory && method == "hybrid")
            {
                memory = SessionMemory.Advance(entry.Memory, input);
                var recall = SessionMemory.Recall(input, memory);
                if (recall is { } fact) requiredMemoryResponse = fact.Response;
                else requiredMemoryResponse = SessionMemory.NameAcknowledgement(input, entry.Memory, memory);
            }
            if (method == "hybrid" && fastPath is not null && catalog is null && requiredMemoryResponse is null)
            {
                fastDecision = fastPath.Evaluate(input, history, candidates, snapshot.Phrases);
                if (fastDecision.Accepted)
                {
                    var selected = candidates.Single(c => c.Id == fastDecision.SelectedId);
                    reply = new(selected.Text, "selected", selected.Id, new() {
                        ["path"] = "retrieval-fast", ["modelInvoked"] = false,
                        ["historyUsed"] = history, ["historyExchangesUsed"] = history.Length,
                        ["candidateScope"] = "latest-input retrieval shortlist", ["candidates"] = candidates, ["elapsedMs"] = 0 });
                }
            }
            prepared = new(method, input, snapshot.Revision, history, candidates, memory, requiredMemoryResponse);
            epoch = entry.Generation;
            flight = CancellationTokenSource.CreateLinkedTokenSource(ct);
            entry.Flight = flight;
        }
        try
        {
            double retrievalAndSnapshotMs = preparation.Elapsed.TotalMilliseconds;
            reply ??= await methods.Run(prepared, flight.Token);
            reply.Metadata["retrievalAndSnapshotMs"] = retrievalAndSnapshotMs;
            if (fastDecision is not null) reply.Metadata["fastPath"] = fastDecision;
            var commitTimer = Stopwatch.StartNew();
            lock (gate)
            {
                flight.Token.ThrowIfCancellationRequested();
                if (entry.Generation != epoch || entry.Flight != flight || !sessions.TryGetValue(id!, out var current) || current != entry)
                    throw new InvalidOperationException("Conversation was reset while this request was pending. No reply was committed.");
                EnsureBankCurrent();
                if (reply.RewrittenQuery is not null)
                {
                    var result = Retrieve(entry.Native, reply.RewrittenQuery, ContextMode.InputOnly, 1).Candidates[0];
                    reply.Metadata["retrievalSimilarity"] = result.Similarity;
                    reply.Metadata["matchedKey"] = result.MatchedKey;
                    reply = reply with { Response = result.Text, SelectedId = result.Id };
                }
                int selected = -1;
                if (reply.Provenance == "selected")
                {
                    var valid = catalog is not null
                        ? reply.SelectedId is not null && catalog.ById.TryGetValue(reply.SelectedId, out var exact) && exact == reply.Response
                        : int.TryParse(reply.SelectedId, out selected) && selected >= 0 && selected < snapshot.Phrases.Count && snapshot.Phrases[selected].Response == reply.Response;
                    if (!valid)
                        throw new InvalidOperationException("Adapter returned an invalid authored response.");
                }
                else if (reply.Provenance != "generated" || method != "hybrid") throw new InvalidOperationException("Only select-or-generate can return generated text.");
                if (catalog is not null)
                {
                    reply.Metadata["catalog"] = new { schemaVersion = CatalogData.Schema, catalog.Status, catalog.TargetSpeaker,
                        responseCount = catalog.Responses.Length, keyCount = catalog.Keys.Length, aggregation = CatalogData.Aggregation };
                    var matched = prepared.Candidates.FirstOrDefault(c => c.Id == reply.SelectedId)?.MatchedKey;
                    if (matched is not null) reply.Metadata["matchedKey"] = matched;
                }
                LocalMethods.ValidateText(reply.Response, 8000, "Response");
                if (!preview)
                {
                    if (catalog is not null && reply.Provenance == "selected") entry.Native.CommitResponse(input, reply.SelectedId!);
                    else if (selected >= 0) entry.Native.Commit(input, selected);
                    else entry.Native.CommitRendered(input, reply.Response);
                    entry.HasTurns = true;
                    if (memory is not null) entry.Memory = memory;
                }
                if (memory is not null) reply.Metadata["sessionMemory"] = memory;
                reply.Metadata["commitMs"] = commitTimer.Elapsed.TotalMilliseconds;
                var resultObject = new { methodId = method, requestId, response = reply.Response, provenance = reply.Provenance, selectedId = reply.SelectedId,
                    metadata = reply.Metadata, bankRevision = snapshot.Revision, preview, history = History(entry.Native) };
                if (!preview) { entry.LastRequest = requestId; entry.LastInput = input; entry.LastResult = resultObject; }
                return resultObject;
            }
        }
        finally
        {
            lock (gate) { if (entry.Flight == flight) entry.Flight = null; entry.Seen = DateTime.UtcNow; }
            flight.Dispose();
        }
    }

    public PhraseBank(string modelPath, string bankPath, bool catalogMode = false, bool sessionMemory = false, RetrievalFastPath? fastPath = null)
    {
        this.modelPath = modelPath; this.bankPath = bankPath; this.sessionMemory = sessionMemory; this.fastPath = fastPath;
        if (catalogMode)
        {
            catalog = CatalogData.Load(bankPath);
            snapshot = new BankView(catalog.Revision, [], catalog.View, true);
            matcher = new PhraseMatcher(modelPath, catalog.Responses, catalog.Keys);
        }
        else
        {
            var contents = File.ReadAllText(bankPath, Utf8);
            snapshot = new BankView(Revision(contents), Parse(contents, allowEmpty: true));
            if (snapshot.Phrases.Count > 0) matcher = new PhraseMatcher(modelPath, bankPath);
        }
    }
    public BankView View() { lock (gate) { EnsureBankCurrent(); return snapshot; } }

    public BankView Edit(int index, string? revision, Phrase phrase)
    {
        RequireEditable();
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
        RequireEditable();
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
        RequireEditable();
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
        ExpireSessions();
        if (sessions.Values.Any(entry => entry.HasTurns || entry.Flight is not null))
            throw new InvalidOperationException("Reset active conversations before changing the phrase bank.");
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
            ClearSessions();
            var previous = matcher;
            matcher = replacement;
            replacement = null;
            previous?.Dispose();
            snapshot = new BankView(Revision(contents), phrases);
            return snapshot;
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

    public void Dispose() { lock (gate) { ClearSessions(); matcher?.Dispose(); } }
}
