using System.Net;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Semantic;

var root = Path.GetFullPath(args[0]);
if (args.Length > 1 && args[1] == "--config-only") { await ModelConfigChecks.Run(root); return; }
NativeLibrary.SetDllImportResolver(typeof(PhraseMatcher).Assembly, (name, _, _) => name == "Semantic.Native" ? NativeLibrary.Load(Path.Combine(root, "build/libSemantic.Native.dylib")) : IntPtr.Zero);
var destination = Path.Combine(root, "artifacts/llm_methods"); Directory.CreateDirectory(destination);
var bankPath = Path.Combine(destination, "contract-bank.tsv");
const string bankText = "prompt\tresponse\nhello\tHello there.\nWhat color is the book?\tThe book is blue.\nWhat is your name?\tI am the librarian.\n";
File.WriteAllText(bankPath, bankText);
var llm = new FakeLlm(); var paper = new FakePaper();
using var methods = new LocalMethods(new("Librarian", "Concise", "A library", "Today", [new("book", "The book is blue.")]), Path.Combine(root, "models/Qwen3-14B-Q4_K_M.gguf"), llm, paper);
using var bank = new PhraseBank(Path.Combine(root, "models/embeddinggemma-2-Q8_0.gguf"), bankPath);
var checks = new List<string>();
JsonElement Element(object value) => JsonSerializer.SerializeToElement(value, LocalMethods.Json);
string Session(string method) => Element(bank.NewSession(method)).GetProperty("sessionId").GetString()!;
void Check(bool condition, string name) { if (!condition) throw new Exception(name); checks.Add(name); }
async Task Fails(Func<Task<object>> task, string name) { try { await task(); } catch (Exception e) when (e is InvalidOperationException or ArgumentException or OperationCanceledException or JsonException) { checks.Add(name); return; } throw new Exception("Expected failure: " + name); }
Task<object> Reply(string session, string method, string input = "hello", bool preview = false, string? request = null) => bank.Respond(input, session, method, request ?? Guid.NewGuid().ToString(), methods, preview, CancellationToken.None);
int History(JsonElement result) => result.GetProperty("history").GetArrayLength();

var embedding = Session("input"); var peer = Session("input");
var id = Guid.NewGuid().ToString();
var first = Element(await Reply(embedding, "input", request: id));
Check(History(first) == 1 && first.GetProperty("response").GetString() == "Hello there.", "authored result commits exact text");
Check(History(Element(await Reply(embedding, "input", request: id))) == 1, "duplicate latest request does not commit twice");
await Fails(() => Reply(embedding, "input", "different", request: id), "duplicate ID with changed input rejected");
Check(History(Element(await Reply(peer, "input", preview: true))) == 0, "caller isolation and preview purity");
await Fails(() => Reply(embedding, "history"), "cross-method session reuse rejected");
bank.Reset(embedding);
Check(History(Element(await Reply(embedding, "input", preview: true))) == 0, "reset clears history");

var authored = Session("llm_id");
llm.Decision = new { selected_id = "0" };
Check(History(Element(await Reply(authored, "llm_id", preview: true))) == 0, "LLM preview does not commit");
var chosen = Element(await Reply(authored, "llm_id"));
Check(chosen.GetProperty("selectedId").GetString() == "0" && History(chosen) == 1, "LLM ID uses stored bank response");
llm.Decision = new { selected_id = "999" };
await Fails(() => Reply(authored, "llm_id"), "out-of-shortlist ID rejected");
llm.Decision = new { selected_id = "0" }; llm.Truncate = true;
var trimmed = Element(await Reply(authored, "llm_id", preview: true));
Check(trimmed.GetProperty("metadata").GetProperty("historyExchangesDropped").GetInt32() == 1 && History(trimmed) == 1, "prompt overflow removes complete oldest exchanges without committing");
llm.Truncate = false;
var hybrid = Session("hybrid");
llm.Decision = new { decision = "generate", selected_id = "", response = "Could you clarify which shelf?", used_fact_ids = Array.Empty<string>() };
var generated = Element(await Reply(hybrid, "hybrid"));
Check(generated.GetProperty("provenance").GetString() == "generated" && generated.GetProperty("history")[0].GetProperty("character").GetString() == "Could you clarify which shelf?", "generated history contains actual rendered text");
llm.Decision = new { decision = "generate", selected_id = "", response = "Unsupported.", used_fact_ids = new[] { "invented" } };
await Fails(() => Reply(hybrid, "hybrid"), "unknown grounding fact ID rejected");
llm.Decision = new { decision = "select", selected_id = "0", response = "Modified reply", used_fact_ids = Array.Empty<string>() };
await Fails(() => Reply(hybrid, "hybrid"), "authored text modification rejected");
llm.Decision = new { decision = "generate", selected_id = "", response = "Hello there.", used_fact_ids = Array.Empty<string>() };
Check(Element(await Reply(hybrid, "hybrid", preview: true)).GetProperty("provenance").GetString() == "selected", "exact authored copy reclassified honestly");
var rewrite = Session("rewrite"); llm.Decision = new { query = "What color is the book?" };
var rewritten = Element(await Reply(rewrite, "rewrite", "What color is it?"));
Check(rewritten.GetProperty("response").GetString() == "The book is blue." && rewritten.GetProperty("history")[0].GetProperty("user").GetString() == "What color is it?", "rewrite retrieval commits original user input");
var rewritePayload = JsonSerializer.Deserialize<JsonElement>(llm.LastRequest.GetProperty("messages")[1].GetProperty("content").GetString()!);
Check(!rewritePayload.TryGetProperty("candidates", out _), "rewriter sees no authored answers");

var remote = Session("light_biranker");
paper.Selected = "0";
Check(History(Element(await Reply(remote, "light_biranker"))) == 1, "paper adapter selection commits exact bank text");
var paperPreview = Element(await Reply(remote, "light_biranker", preview: true));
Check(paper.Last.GetProperty("candidates").GetArrayLength() == 3 && paper.Last.GetProperty("history")[0].GetString() == "hello" && paper.Last.GetProperty("history")[1].GetString() == "Hello there.", "paper receives full responses and alternating actual utterances");
paper.Selected = "999"; await Fails(() => Reply(remote, "light_biranker"), "paper out-of-bank ID rejected");
paper.Selected = "0"; paper.Text = "tampered"; await Fails(() => Reply(remote, "light_biranker"), "paper text mismatch rejected"); paper.Text = null;

paper.Block = new(TaskCreationOptions.RunContinuationsAsynchronously);
paper.Started = new(TaskCreationOptions.RunContinuationsAsynchronously);
var pending = Reply(remote, "light_biranker"); await paper.Started.Task;
await Fails(() => Reply(remote, "light_biranker"), "second overlapping turn rejected");
bank.Reset(remote); paper.Block.SetResult(); paper.Block = null;
await Fails(() => pending, "reset cancels stale commit");
Check(History(Element(await Reply(remote, "light_biranker", preview: true))) == 0, "cancelled reply is absent after reset");
paper.Block = new(TaskCreationOptions.RunContinuationsAsynchronously); paper.Started = new(TaskCreationOptions.RunContinuationsAsynchronously);
pending = Reply(remote, "light_biranker"); await paper.Started.Task;
llm.Decision = new { selected_id = "0" }; var calls = llm.Calls;
var queuedLlm = Reply(authored, "llm_id", preview: true);
await Task.Delay(50);
Check(!queuedLlm.IsCompleted && llm.Calls == calls, "Qwen waits while paper adapter holds heavy-inference gate");
paper.Block.SetResult(); paper.Block = null; await pending; await queuedLlm; bank.Reset(remote);
var closed = Session("input"); bank.Close(closed);
await Fails(() => Reply(closed, "input"), "closed session is unavailable and disposed");
paper.Block = new(TaskCreationOptions.RunContinuationsAsynchronously); paper.Started = new(TaskCreationOptions.RunContinuationsAsynchronously);
pending = Reply(remote, "light_biranker"); await paper.Started.Task;
File.WriteAllText(bankPath, bankText + "bye\tFarewell.\n"); paper.Block.SetResult(); paper.Block = null;
await Fails(() => pending, "external bank change prevents stale response commit"); File.WriteAllText(bankPath, bankText);
Check(History(Element(await Reply(remote, "light_biranker", preview: true))) == 0, "external edit failure leaves history intact");

var result = new { kind = "contract tests with fake adapters and real embedding model; no quality comparison", count = checks.Count, passed = checks, at = DateTime.UtcNow };
File.WriteAllText(Path.Combine(destination, "contract-checks.json"), JsonSerializer.Serialize(result, new JsonSerializerOptions(LocalMethods.Json) { WriteIndented = true }));
Console.WriteLine($"{checks.Count} method/session contract checks passed.");
await CatalogChecks.Run(root);

sealed class FakeLlm : HttpMessageHandler
{
    public object Decision = new { selected_id = "0" };
    public JsonElement LastRequest;
    public bool Truncate;
    public int Calls;
    private bool hasHistory;
    protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken ct)
    {
        object result;
        switch (request.RequestUri!.AbsolutePath)
        {
            case "/v1/models": result = new { data = new[] { new { id = LocalModel.FourteenB.Alias } } }; break;
            case "/props": result = new { model_path = "Qwen3-14B-Q4_K_M.gguf", default_generation_settings = new { n_ctx = 1024 }, build_info = "contract fake" }; break;
            case "/apply-template":
                LastRequest = JsonSerializer.Deserialize<JsonElement>(await request.Content!.ReadAsStringAsync(ct));
                var payload = JsonSerializer.Deserialize<JsonElement>(LastRequest.GetProperty("messages")[1].GetProperty("content").GetString()!);
                hasHistory = payload.GetProperty("history").GetArrayLength() > 0;
                result = new { prompt = "contract-test prompt" }; break;
            case "/tokenize": result = new { tokens = Enumerable.Range(0, Truncate && hasHistory ? 900 : 100).ToArray() }; break;
            default: Calls++; result = new { choices = new[] { new { finish_reason = "stop", message = new { content = JsonSerializer.Serialize(Decision) } } }, usage = new { prompt_tokens = 100, completion_tokens = 10 } }; break;
        }
        return new(HttpStatusCode.OK) { Content = new StringContent(JsonSerializer.Serialize(result), Encoding.UTF8, "application/json") };
    }
}
sealed class FakePaper : HttpMessageHandler
{
    public string Selected = "0";
    public string? Text;
    public JsonElement Last;
    public TaskCompletionSource? Block, Started;
    protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken ct)
    {
        if (request.Content!.Headers.ContentLength is not > 0)
            return new(HttpStatusCode.RequestEntityTooLarge) { Content = new StringContent("Content-Length required") };
        Last = JsonSerializer.Deserialize<JsonElement>(await request.Content!.ReadAsStringAsync(ct));
        Started?.TrySetResult();
        if (Block is not null) await Block.Task; // Deliberately ignore cancellation to exercise the commit guard.
        var result = new Dictionary<string, object?> { ["selected_id"] = Selected, ["metadata"] = new { implementation = "contract fake" } };
        if (Text is not null) result["selected_text"] = Text;
        return new(HttpStatusCode.OK) { Content = new StringContent(JsonSerializer.Serialize(result), Encoding.UTF8, "application/json") };
    }
}
