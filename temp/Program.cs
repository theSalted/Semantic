using System.Net;
using System.Runtime.InteropServices;
using Semantic;

var builder = WebApplication.CreateBuilder(args);
builder.WebHost.ConfigureKestrel(options =>
{
    options.Listen(IPAddress.Loopback, builder.Configuration.GetValue("port", 5187));
    options.Limits.MaxRequestBodySize = 2 * 1024 * 1024;
    options.AddServerHeader = false;
});
builder.Configuration["AllowedHosts"] = "localhost;127.0.0.1";

var root = Path.GetFullPath("..", builder.Environment.ContentRootPath);
var libraryPath = Path.Combine(root, "build", "libSemantic.Native.dylib");
var modelPath = Path.Combine(root, "models", "embeddinggemma-2-Q8_0.gguf");
var bankPath = Path.GetFullPath(builder.Configuration["bank"] ?? Path.Combine(root, "experiments", "phrases.tsv"));
foreach (var required in new[] { libraryPath, modelPath, bankPath })
    if (!File.Exists(required)) throw new FileNotFoundException("Required matcher file is missing.", required);

NativeLibrary.SetDllImportResolver(typeof(PhraseMatcher).Assembly, (name, _, _) =>
    name == "Semantic.Native" ? NativeLibrary.Load(libraryPath) : IntPtr.Zero);

using var bank = new PhraseBank(modelPath, bankPath);
var app = builder.Build();
app.Use(async (context, next) =>
{
    context.Response.Headers.CacheControl = "no-store";
    var origin = context.Request.Headers.Origin.ToString();
    if (context.Request.Method is not ("GET" or "HEAD" or "OPTIONS") && origin.Length > 0 &&
        origin != $"{context.Request.Scheme}://{context.Request.Host}")
    {
        context.Response.StatusCode = StatusCodes.Status403Forbidden;
        await context.Response.WriteAsJsonAsync(new { error = "Open the local playground to save phrases." });
        return;
    }
    await next(context);
});
app.UseDefaultFiles();
app.UseStaticFiles();

app.MapPost("/api/match", (MatchRequest request) => Run(() => new { response = bank.Match(request.Input) }));
app.MapGet("/api/phrases", () => Run(() => bank.View()));
app.MapPost("/api/phrases", (Phrase request) => Run(() => bank.Add([request])));
app.MapPut("/api/phrases/{index:int}", (int index, EditRequest request) =>
    Run(() => bank.Edit(index, request.Revision, new Phrase(request.Prompt, request.Response))));
app.MapDelete("/api/phrases/{index:int}", (int index, [Microsoft.AspNetCore.Mvc.FromBody] DeleteRequest request) =>
    Run(() => bank.Delete(index, request.Revision)));
app.MapPost("/api/import", (ImportRequest request) => Run(() => bank.Add(PhraseBank.Parse(request.Tsv))));
app.Run();

static IResult Run(Func<object> action)
{
    try { return Results.Ok(action()); }
    catch (Exception error) when (error is ArgumentException or InvalidOperationException or FormatException)
    { return Results.BadRequest(new { error = error.Message }); }
    catch (Exception error) when (error is IOException or UnauthorizedAccessException)
    { return Results.Json(new { error = "Could not read or save the phrase bank. " + error.Message }, statusCode: 500); }
}

record MatchRequest(string? Input);
record ImportRequest(string? Tsv);
record EditRequest(string? Prompt, string? Response, string? Revision);
record DeleteRequest(string? Revision);
