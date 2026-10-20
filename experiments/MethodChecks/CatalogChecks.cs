using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Semantic;

static class CatalogChecks
{
    public static async Task Run(string root)
    {
        var checks = new List<string>();
        void Check(bool value, string name) { if (!value) throw new Exception(name); checks.Add(name); }
        void Reject(Action action, string name)
        {
            try { action(); } catch (Exception e) when (e is FormatException or ArgumentException or InvalidOperationException or JsonException)
            { checks.Add(name); return; }
            throw new Exception("Expected rejection: " + name);
        }
        async Task RejectAsync(Func<Task<object>> action, string name)
        {
            try { await action(); } catch (Exception e) when (e is FormatException or ArgumentException or InvalidOperationException)
            { checks.Add(name); return; }
            throw new Exception("Expected rejection: " + name);
        }
        var output = Path.Combine(root, "artifacts/llm_methods/catalog-checks");
        var work = Path.Combine(output, Guid.NewGuid().ToString("N")); Directory.CreateDirectory(work);
        string Hash(string p) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(p)));
        void Manifest(string dir) => File.WriteAllText(Path.Combine(dir, "manifest.json"), JsonSerializer.Serialize(new {
            schema_version = CatalogData.Schema,
            responses = new { path = "responses.jsonl", sha256 = Hash(Path.Combine(dir, "responses.jsonl")) },
            keys = new { path = "keys.jsonl", sha256 = Hash(Path.Combine(dir, "keys.jsonl")) },
            target_speaker = "Test character", status = "preparation"
        }));
        string Bundle(string name, object[] responses, object[] keys)
        {
            var dir = Path.Combine(work, name); Directory.CreateDirectory(dir);
            File.WriteAllText(Path.Combine(dir, "responses.jsonl"), string.Concat(responses.Select(x => JsonSerializer.Serialize(x) + "\n")), new UTF8Encoding(false));
            File.WriteAllText(Path.Combine(dir, "keys.jsonl"), string.Concat(keys.Select(x => JsonSerializer.Serialize(x) + "\n")), new UTF8Encoding(false));
            Manifest(dir); return Path.Combine(dir, "manifest.json");
        }
        const string exact = " \t你好,\r\nNo.  ";
        object[] responses = [
            new { response_id = "c.unkeyed", text = exact },
            new { response_id = "c.a", text = "Hello there." },
            new { response_id = "c.b", text = "Hello there." }
        ];
        object[] keys = [
            new { key_id = "k.z", response_id = "c.a", text = "hello" },
            new { key_id = "k.a", response_id = "c.a", text = "hello" },
            new { key_id = "k.other", response_id = "c.a", text = "What is your name?" },
            new { key_id = "k.b", response_id = "c.b", text = "hello" }
        ];
        var path = Bundle("multiple", responses, keys);
        var data = CatalogData.Load(path);
        Check(data.Responses.Length == 3 && data.Keys.Length == 4, "Catalog retains zero-or-more keys without duplicating responses");
        Check(data.ById["c.unkeyed"] == exact, "JSONL preserves Unicode, tabs, CRLF and surrounding whitespace exactly");
        Check(data.ById["c.a"] == data.ById["c.b"], "Identical reply text under different stable IDs remains separate");
        var reordered = CatalogData.Load(Bundle("reordered", responses.Reverse().ToArray(), keys.Reverse().ToArray()));
        Check(data.Responses.Select(x => x.Id).SequenceEqual(reordered.Responses.Select(x => x.Id)) &&
            data.Keys.Select(x => x.Id).SequenceEqual(reordered.Keys.Select(x => x.Id)), "Interchange reordering preserves stable response and key identity");
        Check(data.Revision != reordered.Revision, "Immutable snapshot revision detects byte-level file reorder");
        Reject(() => CatalogData.Load(Bundle("duplicate-response", [responses[0], responses[0]], [])), "Duplicate response IDs rejected");
        Reject(() => CatalogData.Load(Bundle("duplicate-key", responses, [keys[0], keys[0]])), "Duplicate key IDs rejected even when contents match");
        Reject(() => CatalogData.Load(Bundle("dangling", responses, [new { key_id = "bad", response_id = "missing", text = "hello" }])), "Unknown key response IDs rejected");
        Reject(() => CatalogData.Load(Bundle("blank-key", responses, [new { key_id = "bad", response_id = "c.a", text = " \t" }])), "Blank trigger is rejected, never used as a fallback");
        var tamper = Bundle("tamper", responses, keys);
        File.AppendAllText(Path.Combine(Path.GetDirectoryName(tamper)!, "keys.jsonl"), "\n");
        Reject(() => CatalogData.Load(tamper), "Manifest SHA256 mismatch rejected");
        var oldKeys = File.ReadAllText(Path.Combine(Path.GetDirectoryName(path)!, "keys.jsonl"));
        File.AppendAllText(Path.Combine(Path.GetDirectoryName(path)!, "keys.jsonl"), "\n");
        Reject(data.EnsureCurrent, "Loaded catalog detects subsequent external file changes");
        File.WriteAllText(Path.Combine(Path.GetDirectoryName(path)!, "keys.jsonl"), oldKeys);
        data.EnsureCurrent();
        var invalid = Bundle("invalid", responses, keys);
        File.WriteAllText(invalid, File.ReadAllText(invalid).Replace("\"keys.jsonl\"", "\"../keys.jsonl\""));
        Reject(() => CatalogData.Load(invalid), "Manifest paths cannot escape artifact directory");
        var empty = Bundle("zero", responses, []);
        var noKeys = CatalogData.Load(empty);
        Check(noKeys.Keys.Length == 0 && noKeys.Responses.Length == 3, "Empty key file retains full catalog");

        var model = Path.Combine(root, "models/embeddinggemma-2-Q8_0.gguf");
        CatalogMatch[] ranked;
        using (var native = new PhraseMatcher(model, data.Responses, data.Keys))
        using (var session = native.CreateSession())
        {
            ranked = session.RetrieveResponses("hello", 2);
            Check(ranked.Length == 2 && ranked[0].ResponseId == "c.a" && ranked[1].ResponseId == "c.b",
                "Real embedding retrieval deduplicates responses before top-k");
            Check(ranked[0].KeyId == "k.a" && ranked[0].KeyText == "hello", "Winning key provenance and stable key tie-break returned through C ABI");
            Check(session.History().Length == 0, "Catalog retrieval does not accumulate history");
            session.CommitResponse("hello", "c.unkeyed");
            Check(session.History()[0].Character == exact, "Stable-ID commit supports unkeyed responses and preserves exact text");
            Reject(() => session.CommitResponse("hello", "missing"), "Unknown stable-ID commit rejected");
            Check(session.History().Length == 1, "Rejected stable-ID commit leaves history unchanged");
            session.Reset(); Check(session.History().Length == 0, "Catalog reset clears history");
        }
        using (var native = new PhraseMatcher(model, reordered.Responses, reordered.Keys))
        {
            using var session = native.CreateSession(); native.Dispose();
            var again = session.RetrieveResponses("hello", 2);
            Check(again.Select(x => (x.ResponseId, x.KeyId)).SequenceEqual(ranked.Select(x => (x.ResponseId, x.KeyId))),
                "Reordered catalog/key files produce same stable selection and provenance");
            Check(again.Select(x => x.Similarity).Zip(ranked.Select(x => x.Similarity)).All(x => Math.Abs(x.First - x.Second) < 1e-6),
                "Repeated keys and reorder preserve response scores");
            Check(session.History().Length == 0, "Catalog session retains ownership after matcher disposal");
        }
        var fake = new FakePaper { Selected = "c.unkeyed", Text = exact };
        var llm = new FakeLlm { Decision = new { selected_id = "c.a" } };
        using var methods = new LocalMethods(new("Test", "Concise", "", "", []),
            Path.Combine(root, "models/Qwen3-14B-Q4_K_M.gguf"), llm, fake);
        JsonElement Element(object o) => JsonSerializer.SerializeToElement(o, LocalMethods.Json);
        using (var bank = new PhraseBank(model, path, true))
        {
            var id = Element(bank.NewSession("light_biranker")).GetProperty("sessionId").GetString();
            var result = Element(await bank.Respond("hello", id, "light_biranker", Guid.NewGuid().ToString(), methods, false, default));
            var supplied = fake.Last.GetProperty("candidates");
            Check(supplied.GetArrayLength() == 3 && supplied.EnumerateArray().Select(x => x.GetProperty("id").GetString()).Distinct().Count() == 3,
                "Full-response adapter receives entire unique catalog including unkeyed reply");
            Check(result.GetProperty("selectedId").GetString() == "c.unkeyed" && result.GetProperty("response").GetString() == exact &&
                result.GetProperty("history")[0].GetProperty("character").GetString() == exact, "Common adapter validates opaque stable ID and commits exact authored text");
            fake.Text = "tampered";
            await RejectAsync(() => bank.Respond("hello", id, "light_biranker", Guid.NewGuid().ToString(), methods, true, default), "Catalog rejects paper-selected text modification");
            fake.Text = exact; fake.Selected = "0";
            await RejectAsync(() => bank.Respond("hello", id, "light_biranker", Guid.NewGuid().ToString(), methods, true, default), "Old row index cannot impersonate a catalog ID");
            var qwen = Element(bank.NewSession("llm_id")).GetProperty("sessionId").GetString();
            var selected = Element(await bank.Respond("hello", qwen, "llm_id", Guid.NewGuid().ToString(), methods, true, default));
            var payload = JsonSerializer.Deserialize<JsonElement>(llm.LastRequest.GetProperty("messages")[1].GetProperty("content").GetString()!);
            Check(payload.GetProperty("candidates").GetArrayLength() == 2, "LLM shortlist contains unique response IDs");
            Check(selected.GetProperty("metadata").GetProperty("matchedKey").GetProperty("keyId").GetString() == "k.a",
                "Selected shortlist response includes matched-key provenance");
            Reject(() => bank.Add([new("fallback", "text")]), "Catalog mode rejects legacy mutation and fabricated fallback import");
            Check(Element(bank.View()).GetProperty("readOnly").GetBoolean(), "Catalog is exposed separately as read-only");
        }
        fake.Selected = "c.unkeyed";
        using (var bank = new PhraseBank(model, empty, true))
        {
            var id = Element(bank.NewSession("light_biranker")).GetProperty("sessionId").GetString();
            var result = Element(await bank.Respond("hello", id, "light_biranker", Guid.NewGuid().ToString(), methods, false, default));
            Check(result.GetProperty("selectedId").GetString() == "c.unkeyed" && fake.Last.GetProperty("candidates").GetArrayLength() == 3,
                "All-unkeyed catalog supports full-response selection without embedding any fallback");
            var nativeId = Element(bank.NewSession("input")).GetProperty("sessionId").GetString();
            var calls = llm.Calls;
            await RejectAsync(() => bank.Respond("hello", nativeId, "input", Guid.NewGuid().ToString(), methods, false, default), "Zero-key retrieval fails explicitly instead of shrinking or fabricating keys");
            Check(llm.Calls == calls, "Zero-key failure dispatches no model rewrite or generation");
            var available = bank.AvailableMethods([new("input", "Input", true, ""), new("light_biranker", "Paper", true, "")]);
            Check(!available[0].Available && available[1].Available, "Discovery distinguishes key coverage from full-catalog availability");
        }
        var projection = Path.Combine(root, "artifacts/chandler/fixtures-v2/candidate-catalog-projection.jsonl");
        if (File.Exists(projection))
        {
            var fullDir = Path.Combine(work, "full-catalog"); Directory.CreateDirectory(fullDir);
            File.Copy(projection, Path.Combine(fullDir, "responses.jsonl")); File.WriteAllText(Path.Combine(fullDir, "keys.jsonl"), "");
            Manifest(fullDir); var full = CatalogData.Load(Path.Combine(fullDir, "manifest.json"));
            Check(full.Responses.Length == 7470 && full.Keys.Length == 0 && Hash(projection) == Hash(Path.Combine(fullDir, "responses.jsonl")),
                "All 7470 Chandler catalog IDs/text retained from exact projection with zero keys; offline load only");
        }
        File.WriteAllText(Path.Combine(output, "results.json"), JsonSerializer.Serialize(new {
            scope = "focused unit/contract checks; tiny real embedding fixtures plus fake remote adapters; no quality comparison",
            count = checks.Count, passed = checks, artifacts = work, at = DateTime.UtcNow
        }, new JsonSerializerOptions(LocalMethods.Json) { WriteIndented = true }));
        Console.WriteLine($"{checks.Count} catalog/key checks passed.");
    }
}
