namespace Semantic
{
    public readonly struct CatalogResponse
    {
        public string Id { get; }
        public string Text { get; }
        public CatalogResponse(string id, string text) { Id = id; Text = text; }
    }
    public readonly struct CatalogKey
    {
        public string Id { get; }
        public string ResponseId { get; }
        public string Text { get; }
        public CatalogKey(string id, string responseId, string text) { Id = id; ResponseId = responseId; Text = text; }
    }
    public readonly struct CatalogMatch
    {
        public string ResponseId { get; }
        public string Response { get; }
        public string KeyId { get; }
        public string KeyText { get; }
        public float Similarity { get; }
        public int Tokens { get; }
        public int Exchanges { get; }
        public string ContextText { get; }
        internal CatalogMatch(string id, string response, string keyId, string keyText, float score,
            int tokens, int exchanges, string context)
        {
            ResponseId = id; Response = response; KeyId = keyId; KeyText = keyText; Similarity = score;
            Tokens = tokens; Exchanges = exchanges; ContextText = context;
        }
    }
}
