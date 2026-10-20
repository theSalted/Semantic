using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Semantic;

// Immutable, opt-in interchange. Review evidence stays outside model input.
sealed class CatalogData
{
    public const string Schema = "semantic.catalog-keys.v1";
    public const string Aggregation = "maximum key similarity per response; UTF-8 bytewise response-ID ties, key-ID ties; deduplicate before top-k";
    private static readonly UTF8Encoding Utf8 = new(false, true);
    private readonly Dictionary<string, string> fileHashes = new(StringComparer.Ordinal);
    public string Revision { get; private set; } = "";
    public string Status { get; private set; } = "";
    public string TargetSpeaker { get; private set; } = "";
    public CatalogResponse[] Responses { get; private set; } = [];
    public CatalogKey[] Keys { get; private set; } = [];
    public Dictionary<string, string> ById { get; private set; } = new(StringComparer.Ordinal);
    public object View => new {
        schemaVersion = Schema, Status, TargetSpeaker, responseCount = Responses.Length, keyCount = Keys.Length,
        keyedResponseCount = Keys.Select(k => k.ResponseId).Distinct(StringComparer.Ordinal).Count(),
        aggregation = Aggregation,
        responses = Responses.Select(r => new { responseId = r.Id, text = r.Text }),
        keys = Keys.Select(k => new { keyId = k.Id, responseId = k.ResponseId, text = k.Text })
    };

    public static CatalogData Load(string manifestPath)
    {
        var data = new CatalogData();
        manifestPath = Path.GetFullPath(manifestPath);
        var bytes = Read(manifestPath);
        using var doc = JsonDocument.Parse(Utf8.GetString(bytes));
        var m = doc.RootElement;
        Fields(m, "schema_version", "responses", "keys", "target_speaker", "status");
        if (Text(m, "schema_version") != Schema) throw new FormatException("Unsupported catalog schema_version.");
        data.Status = Text(m, "status");
        if (data.Status is not ("preparation" or "released")) throw new FormatException("Catalog status must be preparation or released.");
        data.TargetSpeaker = Text(m, "target_speaker");
        data.fileHashes[manifestPath] = Hash(bytes);
        byte[] Component(string field)
        {
            var item = m.GetProperty(field); Fields(item, "path", "sha256");
            var relative = Text(item, "path");
            var directory = Path.GetDirectoryName(manifestPath)! + Path.DirectorySeparatorChar;
            var path = Path.GetFullPath(Path.Combine(directory, relative));
            if (Path.IsPathRooted(relative) || !path.StartsWith(directory, StringComparison.Ordinal) || path == manifestPath)
                throw new FormatException("Catalog file paths must resolve inside the manifest directory.");
            if (data.fileHashes.ContainsKey(path)) throw new FormatException("Catalog components must be separate files.");
            var content = Read(path); var hash = Hash(content);
            if (!string.Equals(hash, Text(item, "sha256"), StringComparison.OrdinalIgnoreCase))
                throw new FormatException($"SHA256 mismatch for catalog {field} file.");
            data.fileHashes[path] = hash;
            return content;
        }
        var responseRows = Rows(Component("responses"), 20000);
        var keyRows = Rows(Component("keys"), 200000);
        data.Responses = responseRows.Select(r => {
            Fields(r, "response_id", "text");
            var id = Text(r, "response_id", 256); var text = Text(r, "text", 8000);
            if (!data.ById.TryAdd(id, text)) throw new FormatException("Duplicate response_id: " + id);
            return new CatalogResponse(id, text);
        }).OrderBy(r => r.Id, StringComparer.Ordinal).ToArray();
        if (data.Responses.Length == 0) throw new FormatException("Response catalog must not be empty.");
        var keyIds = new HashSet<string>(StringComparer.Ordinal);
        data.Keys = keyRows.Select(r => {
            Fields(r, "key_id", "response_id", "text");
            var id = Text(r, "key_id", 256); var responseId = Text(r, "response_id", 256);
            if (!keyIds.Add(id)) throw new FormatException("Duplicate key_id: " + id);
            if (!data.ById.ContainsKey(responseId)) throw new FormatException("Key references unknown response_id: " + responseId);
            return new CatalogKey(id, responseId, Text(r, "text", 65536));
        }).OrderBy(k => k.Id, StringComparer.Ordinal).ToArray();
        // Byte-bound revision intentionally changes on reordering; stable IDs do not.
        data.Revision = Hash(Utf8.GetBytes(string.Join("\n", data.fileHashes.Values)));
        return data;
    }
    public void EnsureCurrent()
    {
        foreach (var (path, expected) in fileHashes)
            if (Hash(Read(path)) != expected)
                throw new InvalidOperationException("Catalog files changed. Restart to load the new immutable snapshot.");
    }
    private static byte[] Read(string path)
    {
        if (new FileInfo(path).Length > 64 * 1024 * 1024) throw new FormatException("Catalog component exceeds 64 MB.");
        return File.ReadAllBytes(path);
    }
    private static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes));
    private static List<JsonElement> Rows(byte[] bytes, int limit)
    {
        var result = new List<JsonElement>();
        using var reader = new StringReader(Utf8.GetString(bytes));
        while (reader.ReadLine() is { } line)
        {
            if (string.IsNullOrWhiteSpace(line)) continue;
            using var doc = JsonDocument.Parse(line);
            result.Add(doc.RootElement.Clone());
            if (result.Count > limit) throw new FormatException($"Catalog exceeds {limit} records.");
        }
        return result;
    }
    private static void Fields(JsonElement row, params string[] names)
    {
        if (row.ValueKind != JsonValueKind.Object || row.EnumerateObject().Count() != names.Length ||
            row.EnumerateObject().Any(p => !names.Contains(p.Name)) || names.Any(n => !row.TryGetProperty(n, out _)))
            throw new FormatException("Catalog object requires exactly: " + string.Join(", ", names));
    }
    private static string Text(JsonElement row, string field, int max = 1024)
    {
        var value = row.GetProperty(field);
        if (value.ValueKind != JsonValueKind.String) throw new FormatException(field + " must be a string.");
        var text = value.GetString()!;
        if (string.IsNullOrWhiteSpace(text) || text.Length > max || text.Contains('\0'))
            throw new FormatException(field + " is blank, too long, or contains a null character.");
        Utf8.GetByteCount(text); // Reject malformed Unicode; do not trim or normalize.
        return text;
    }
}
