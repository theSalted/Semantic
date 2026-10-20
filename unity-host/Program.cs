using System.Net;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Semantic;

var builder = WebApplication.CreateBuilder(args);
builder.WebHost.ConfigureKestrel(o =>
{
    o.Listen(IPAddress.Loopback, builder.Configuration.GetValue("port", 5192));
    o.Limits.MaxRequestBodySize = 8192;
    o.AddServerHeader = false;
});
builder.Configuration["AllowedHosts"] = "localhost;127.0.0.1";
var root = Path.GetFullPath(builder.Configuration["semantic-root"] ?? "..");
var bankPath = Path.GetFullPath(builder.Configuration["bank"] ?? throw new ArgumentException("--bank is required."));
var profilePath = builder.Configuration["character-profile"] ?? throw new ArgumentException("--character-profile is required.");
var libraryPath = Path.GetFullPath(builder.Configuration["native-library"] ?? Path.Combine(root, "build", RuntimeInformation.IsOSPlatform(OSPlatform.Windows) ? "Semantic.Native.dll" : "libSemantic.Native.dylib"));
NativeLibrary.SetDllImportResolver(typeof(PhraseMatcher).Assembly, (name, _, _) => name == "Semantic.Native" ? NativeLibrary.Load(libraryPath) : IntPtr.Zero);
using var source = new TaggedBank(bankPath);
var fastSettings = RetrievalFastPath.Load(builder.Configuration["fast-path-config"] ?? Path.Combine(root, "unity-host", "fast-path.json"));
var fastPath = new RetrievalFastPath(fastSettings, source.Rows.GroupBy(r => RetrievalFastPath.Normalize(r[0])).Where(g => g.Select(r => (r[1], r[2])).Distinct().Count() > 1).Select(g => g.Key));
using var bank = new PhraseBank(Path.Combine(root, "models", "embeddinggemma-2-Q8_0.gguf"), source.Projection, sessionMemory: true, fastPath: fastPath);
using var methods = new LocalMethods(CharacterProfile.Load(profilePath), Path.Combine(root, "models", "Qwen3-14B-Q4_K_M.gguf"), compactHybrid: true);
var app = builder.Build();
app.Use(async (context, next) =>
{
    context.Response.Headers.CacheControl = "no-store";
    // This bridge is for a local Unity client, never browser-origin requests or bank editing.
    if (context.Request.Headers.Origin.Count > 0) { context.Response.StatusCode = 403; return; }
    await next(context);
});
app.MapGet("/health", () => Execute(() => new { ready = true, protocol = 1, bankSha256 = source.Check(), rows = source.Rows.Length, methodId = "hybrid" }));
app.MapPost("/api/session", (SessionRequest request) => Execute(() =>
{
    source.Check(request.BankSha256);
    return new { sessionId = JsonSerializer.SerializeToElement(bank.NewSession("hybrid"), LocalMethods.Json).GetProperty("sessionId").GetString(), bankSha256 = source.Hash };
}));
app.MapPost("/api/reset", (SessionRequest request) => Execute(() => bank.Reset(request.SessionId)));
app.MapPost("/api/close", (SessionRequest request) => Execute(() => bank.Close(request.SessionId)));
app.MapPost("/api/respond", async (RespondRequest request, CancellationToken ct) =>
{
    try
    {
        source.Check(request.BankSha256);
        LocalMethods.ValidateText(request.Input, 400, "Input");
        var result = JsonSerializer.SerializeToElement(await bank.Respond(request.Input, request.SessionId, "hybrid", request.RequestId, methods, false, ct), LocalMethods.Json);
        source.Check(request.BankSha256);
        var provenance = result.GetProperty("provenance").GetString();
        var selectedId = result.GetProperty("selectedId").GetString();
        string? tag = null;
        if (provenance == "selected")
        {
            if (!int.TryParse(selectedId, out int index) || index < 0 || index >= source.Rows.Length || source.Rows[index][1] != result.GetProperty("response").GetString())
                throw new InvalidOperationException("Authored reply does not match the Unity bank.");
            tag = source.Rows[index][2];
        }
        return Results.Ok(new { response = result.GetProperty("response"), provenance, selectedId, emoteTag = tag, bankSha256 = source.Hash,
            requestId = request.RequestId, history = result.GetProperty("history"), metadata = result.GetProperty("metadata") });
    }
    catch (OperationCanceledException) { return Results.Json(new { error = "Turn cancelled or reset." }, statusCode: 409); }
    catch (Exception e) when (e is ArgumentException or InvalidOperationException or FormatException or IOException or HttpRequestException or JsonException or KeyNotFoundException)
    { return Results.Json(new { error = e.Message }, statusCode: 503); }
});
app.Run();

static IResult Execute(Func<object> action)
{
    try { return Results.Ok(action()); }
    catch (Exception e) when (e is ArgumentException or InvalidOperationException or IOException or FormatException)
    { return Results.Json(new { error = e.Message }, statusCode: 409); }
}
record SessionRequest(string? SessionId, string? BankSha256);
record RespondRequest(string? SessionId, string? Input, string? RequestId, string? BankSha256);

sealed class TaggedBank : IDisposable
{
    readonly string path;
    public readonly string Hash;
    public readonly string[][] Rows;
    public readonly string Projection;
    public TaggedBank(string path)
    {
        this.path = path;
        var bytes = File.ReadAllBytes(path);
        Hash = Convert.ToHexString(SHA256.HashData(bytes));
        using var reader = new StringReader(new UTF8Encoding(false, true).GetString(bytes));
        if (reader.ReadLine() != "prompt\tresponse\temote_tag") throw new FormatException("Expected the existing three-column Unity bank.");
        var rows = new List<string[]>();
        while (reader.ReadLine() is { } line)
        {
            if (string.IsNullOrWhiteSpace(line)) continue;
            var fields = line.Split('\t');
            if (fields.Length != 3 || fields.Any(string.IsNullOrWhiteSpace)) throw new FormatException("Invalid tagged row.");
            rows.Add(fields);
        }
        if (rows.Count == 0) throw new FormatException("The Unity bank is empty.");
        Rows = rows.ToArray();
        Projection = Path.Combine(Path.GetTempPath(), "semantic-unity-" + Guid.NewGuid().ToString("N") + ".tsv");
        File.WriteAllText(Projection, "prompt\tresponse\n" + string.Concat(Rows.Select(r => r[0] + "\t" + r[1] + "\n")), new UTF8Encoding(false));
    }
    public string Check(string? expected = null)
    {
        if ((expected is not null && !Hash.Equals(expected, StringComparison.OrdinalIgnoreCase)) || Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))) != Hash)
            throw new InvalidOperationException("Unity bank changed or differs from the bridge. Restart the bridge and conversation.");
        return Hash;
    }
    public void Dispose() { if (File.Exists(Projection)) File.Delete(Projection); }
}
