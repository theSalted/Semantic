#include "Semantic.Native.h"
#include "Semantic.hpp"
#include "llama.h"

#include <cstdint>
#include <memory>

namespace
{
    thread_local std::string last_error;

    struct BackendLifetime
    {
        BackendLifetime() { llama_backend_init(); }
        ~BackendLifetime() { llama_backend_free(); }
    };

    void InitializeBackend()
    {
        static BackendLifetime lifetime;
    }
}

struct MatcherState
{
    std::vector<Semantic::Phrase> phrases;
    std::unique_ptr<Semantic::Catalog> catalog;
    std::unique_ptr<llama_model, decltype(&llama_model_free)> model{nullptr, llama_model_free};
    std::unique_ptr<llama_context, decltype(&llama_free)> context{nullptr, llama_free};
    int max_tokens;
    int model_tokens;

    MatcherState(const std::string& model_path, const std::string& bank_path, int budget,
                 std::unique_ptr<Semantic::Catalog> entries = nullptr)
        : phrases(entries ? std::vector<Semantic::Phrase>{} : Semantic::LoadPhraseBank(bank_path)),
          catalog(std::move(entries))
    {
        if (!std::ifstream(std::filesystem::u8path(model_path)))
            throw std::runtime_error("Cannot open model: " + model_path);
        InitializeBackend();
        auto model_params = llama_model_default_params();
        ggml_backend_dev_t cpu_only[] = {nullptr};
        model_params.devices = cpu_only;
        model_params.n_gpu_layers = 0; // CPU first: no float16 activation path for EmbeddingGemma2.
        model.reset(llama_model_load_from_file(model_path.c_str(), model_params));
        if (!model) throw std::runtime_error("Could not load the embedding model.");
        char architecture[64]{};
        llama_model_meta_val_str(model.get(), "general.architecture", architecture, sizeof architecture);
        if (std::string(architecture) != "gemma-embedding2" || llama_model_n_embd_out(model.get()) != 768)
            throw std::runtime_error("This experiment expects EmbeddingGemma2 with 768 output dimensions.");

        // This GGUF advertises 262144 positional capacity; Google's model card
        // specifies an 8192-token embedding window. Respect BOTH limits.
        model_tokens = std::min(8192, llama_model_n_ctx_train(model.get()));
        max_tokens = budget == 0 ? std::min(512, model_tokens) : budget;
        if (max_tokens < 32 || max_tokens > model_tokens)
            throw std::invalid_argument("Context capacity must be 32..model token limit.");
        auto params = llama_context_default_params();
        params.n_ctx = params.n_batch = params.n_ubatch = max_tokens;
        params.n_seq_max = 1;
        params.n_threads = params.n_threads_batch = 4;
        params.embeddings = true;
        params.pooling_type = LLAMA_POOLING_TYPE_MEAN;
        params.attention_type = LLAMA_ATTENTION_TYPE_NON_CAUSAL;
        params.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
        params.type_k = params.type_v = GGML_TYPE_F32;
        params.offload_kqv = params.op_offload = false;
        context.reset(llama_init_from_model(model.get(), params));
        if (!context) throw std::runtime_error("Could not create the embedding context.");
        if (catalog) catalog->EmbedKeys([this](const std::string& text) { return Embed(text); });
        else Semantic::EmbedPrompts(phrases, [this](const std::string& text) { return Embed(text); });
    }

    std::vector<llama_token> Tokenize(const std::string& input)
    {
        if (Semantic::IsBlank(input)) throw std::invalid_argument("Input must not be empty.");
        // Symmetric phrase-to-phrase similarity. Retrieval would instead require
        // search-result queries AND title/text document prefixes on the bank.
        const std::string text = "task: sentence similarity | query: " + input;
        if (text.size() > static_cast<std::size_t>(INT32_MAX))
            throw std::invalid_argument("Input is too large.");
        auto vocab = llama_model_get_vocab(model.get());
        const int needed = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()),
                                          nullptr, 0, true, false);
        if (needed == INT32_MIN) throw std::invalid_argument("Input is too large.");
        std::vector<llama_token> tokens(static_cast<std::size_t>(std::abs(needed)));
        const int count = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()),
                                         tokens.data(), static_cast<int32_t>(tokens.size()), true, false);
        if (count <= 0) throw std::runtime_error("Tokenization failed.");
        tokens.resize(count);
        return tokens;
    }

    std::size_t Count(const std::string& text) { return Tokenize(text).size(); }

    Semantic::Embedding Embed(const std::string& input)
    {
        auto tokens = Tokenize(input);
        const int count = static_cast<int>(tokens.size());
        if (count > max_tokens)
            throw std::invalid_argument("Input exceeds context capacity, including prefix and special tokens.");
        if (auto memory = llama_get_memory(context.get())) llama_memory_clear(memory, true);
        llama_batch batch{};
        batch.n_tokens = count;
        batch.token = tokens.data(); // Null positions/sequence IDs use position 0..n-1, sequence 0.
        if (llama_decode(context.get(), batch) != 0)
            throw std::runtime_error("Embedding evaluation failed.");
        const float* values = llama_get_embeddings_seq(context.get(), 0);
        if (!values) throw std::runtime_error("Model returned no pooled embedding.");
        return {values, values + llama_model_n_embd_out(model.get())};
    }
};

struct Semantic_Matcher { std::shared_ptr<MatcherState> state; };
struct Semantic_Session
{
    std::shared_ptr<MatcherState> state; // Sessions keep the model/bank alive.
    Semantic::Session history;
    std::string preview_text;
    Semantic_Session(std::shared_ptr<MatcherState> owner, int budget)
        : state(std::move(owner)), history(budget) {}
};

Semantic_Matcher* SEMANTIC_CALL Semantic_CreateMatcher(const char* model, const char* bank)
{
    return Semantic_CreateMatcherWithCapacity(model, bank, 0);
}

Semantic_Matcher* SEMANTIC_CALL Semantic_CreateMatcherWithCapacity(const char* model, const char* bank, int capacity)
{
    last_error.clear();
    try
    {
        if (!model || !bank) throw std::invalid_argument("Model and bank paths are required.");
        return new Semantic_Matcher{std::make_shared<MatcherState>(model, bank, capacity)};
    }
    catch (const std::exception& e) { last_error = e.what(); }
    catch (...) { last_error = "Could not create matcher."; }
    return nullptr;
}

Semantic_Matcher* SEMANTIC_CALL Semantic_CreateCatalogMatcher(
    const char* model, const char* const* response_ids, const char* const* response_texts, int response_count,
    const char* const* key_ids, const char* const* key_response_ids, const char* const* key_texts, int key_count, int capacity)
{
    last_error.clear();
    try
    {
        if (!model || response_count < 1 || response_count > 20000 || key_count < 0 || key_count > 200000 ||
            !response_ids || !response_texts || (key_count && (!key_ids || !key_response_ids || !key_texts)))
            throw std::invalid_argument("Valid model, response arrays and key arrays are required.");
        std::vector<Semantic::Response> responses;
        std::vector<Semantic::TriggerKey> keys;
        for (int i = 0; i < response_count; ++i)
        {
            if (!response_ids[i] || !response_texts[i]) throw std::invalid_argument("Null response field.");
            responses.push_back({response_ids[i], response_texts[i]});
        }
        for (int i = 0; i < key_count; ++i)
        {
            if (!key_ids[i] || !key_response_ids[i] || !key_texts[i]) throw std::invalid_argument("Null key field.");
            keys.push_back({key_ids[i], key_response_ids[i], key_texts[i], {}});
        }
        auto catalog = std::make_unique<Semantic::Catalog>(std::move(responses), std::move(keys));
        return new Semantic_Matcher{std::make_shared<MatcherState>(model, "", capacity, std::move(catalog))};
    }
    catch (const std::exception& e) { last_error = e.what(); }
    catch (...) { last_error = "Could not create catalog matcher."; }
    return nullptr;
}

void SEMANTIC_CALL Semantic_DestroyMatcher(Semantic_Matcher* matcher)
{
    delete matcher;
}

const char* SEMANTIC_CALL Semantic_Match(Semantic_Matcher* matcher, const char* input, float* score)
{
    last_error.clear();
    if (score) *score = 0;
    try
    {
        if (!matcher || !input || !score) throw std::invalid_argument("Matcher, input, and score are required.");
        return Semantic::BestMatch(matcher->state->phrases, matcher->state->Embed(input), *score).response.c_str();
    }
    catch (const std::exception& e) { last_error = e.what(); }
    catch (...) { last_error = "Could not match input."; }
    return nullptr;
}

const char* SEMANTIC_CALL Semantic_LastError()
{
    return last_error.c_str();
}

const char* SEMANTIC_CALL Semantic_BackendInfo()
{
    try { InitializeBackend(); return llama_print_system_info(); }
    catch (...) { return nullptr; }
}

int SEMANTIC_CALL Semantic_TokenLimit(Semantic_Matcher* matcher, int trained)
{
    return matcher ? (trained ? matcher->state->model_tokens : matcher->state->max_tokens) : 0;
}

Semantic_Session* SEMANTIC_CALL Semantic_CreateSession(Semantic_Matcher* matcher, int budget)
{
    last_error.clear();
    try
    {
        if (!matcher) throw std::invalid_argument("Matcher is required.");
        if (!budget) budget = matcher->state->max_tokens;
        if (budget < 32 || budget > matcher->state->max_tokens)
            throw std::invalid_argument("Session budget must be 32..matcher capacity.");
        return new Semantic_Session(matcher->state, budget);
    }
    catch (const std::exception& e) { last_error = e.what(); }
    catch (...) { last_error = "Could not create session."; }
    return nullptr;
}

void SEMANTIC_CALL Semantic_DestroySession(Semantic_Session* session) { delete session; }
void SEMANTIC_CALL Semantic_ResetSession(Semantic_Session* session) { if (session) session->history.Reset(); }

int SEMANTIC_CALL Semantic_Preview(Semantic_Session* session, const char* input, int mode, Semantic_Result* result)
{
    last_error.clear();
    if (result) *result = {};
    try
    {
        if (!session || !input || !result) throw std::invalid_argument("Session, input and result are required.");
        auto& state = *session->state;
        auto prepared = session->history.Prepare(input, static_cast<Semantic::ContextMode>(mode),
                                                 [&](const auto& text) { return state.Count(text); });
        auto embedding = Semantic::Normalize(state.Embed(prepared.text));
        if (mode == static_cast<int>(Semantic::ContextMode::Blend) && prepared.exchanges)
        {
            auto current = Semantic::Normalize(state.Embed(input));
            for (std::size_t i = 0; i < embedding.size(); ++i)
                embedding[i] = 0.3f * embedding[i] + 0.7f * current[i];
        }
        const auto& phrase = Semantic::BestMatch(state.phrases, std::move(embedding), result->score);
        session->preview_text = std::move(prepared.text);
        result->context_text = session->preview_text.c_str();
        result->response = phrase.response.c_str();
        result->index = static_cast<int>(&phrase - state.phrases.data());
        result->tokens = static_cast<int>(prepared.tokens);
        result->exchanges = static_cast<int>(prepared.exchanges);
        return 1;
    }
    catch (const std::exception& e) { last_error = e.what(); }
    catch (...) { last_error = "Could not preview input."; }
    return 0;
}

int SEMANTIC_CALL Semantic_Retrieve(Semantic_Session* session, const char* input, int k, Semantic_Result* results)
{
    last_error.clear();
    try
    {
        if (!session || !input || !results || k < 1 || k > 128)
            throw std::invalid_argument("Session, input, result buffer and k in 1..128 are required.");
        auto& state = *session->state;
        auto prepared = session->history.Prepare(input, Semantic::ContextMode::InputOnly,
                                                [&](const auto& text) { return state.Count(text); });
        auto candidates = Semantic::TopMatches(state.phrases, state.Embed(prepared.text), k);
        session->preview_text = std::move(prepared.text);
        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            const auto& c = candidates[i];
            results[i] = {state.phrases[c.index].response.c_str(), c.score, static_cast<int>(c.index),
                          static_cast<int>(prepared.tokens), 0, session->preview_text.c_str()};
        }
        return static_cast<int>(candidates.size());
    }
    catch (const std::exception& e) { last_error = e.what(); }
    catch (...) { last_error = "Could not retrieve candidates."; }
    return -1;
}

int SEMANTIC_CALL Semantic_RetrieveResponses(Semantic_Session* session, const char* input,
                                           int mode, int k, Semantic_CatalogResult* results)
{
    last_error.clear();
    try
    {
        if (!session || !session->state->catalog || !input || !results || k < 1 || k > 128)
            throw std::invalid_argument("Catalog session, input, result buffer and k in 1..128 are required.");
        auto& state = *session->state;
        auto prepared = session->history.Prepare(input, static_cast<Semantic::ContextMode>(mode),
                                                [&](const auto& text) { return state.Count(text); });
        if (state.catalog->Keys().empty()) return 0;
        auto embedding = Semantic::Normalize(state.Embed(prepared.text));
        if (mode == static_cast<int>(Semantic::ContextMode::Blend) && prepared.exchanges)
        {
            auto current = Semantic::Normalize(state.Embed(input));
            for (std::size_t i = 0; i < embedding.size(); ++i) embedding[i] = 0.3f * embedding[i] + 0.7f * current[i];
        }
        auto ranked = state.catalog->Rank(std::move(embedding), k);
        session->preview_text = std::move(prepared.text);
        for (std::size_t i = 0; i < ranked.size(); ++i)
        {
            const auto& match = ranked[i];
            const auto& response = state.catalog->Responses()[match.response];
            const auto& key = state.catalog->Keys()[match.key];
            results[i] = {response.id.c_str(), response.text.c_str(), key.id.c_str(), key.text.c_str(),
                match.score, static_cast<int>(prepared.tokens), static_cast<int>(prepared.exchanges), session->preview_text.c_str()};
        }
        return static_cast<int>(ranked.size());
    }
    catch (const std::exception& e) { last_error = e.what(); }
    catch (...) { last_error = "Could not retrieve catalog responses."; }
    return -1;
}

int SEMANTIC_CALL Semantic_CommitResponse(Semantic_Session* session, const char* input, const char* response_id)
{
    last_error.clear();
    try
    {
        if (!session || !session->state->catalog || !input || !response_id)
            throw std::invalid_argument("Catalog session, input and response ID are required.");
        const auto& response = session->state->catalog->Find(response_id);
        session->history.Commit(input, response.text, [&](const auto& text) { return session->state->Count(text); });
        return 1;
    }
    catch (const std::exception& e) { last_error = e.what(); }
    catch (...) { last_error = "Could not commit catalog response."; }
    return 0;
}

int SEMANTIC_CALL Semantic_CommitRendered(Semantic_Session* session, const char* input, const char* response)
{
    last_error.clear();
    try
    {
        if (!session || !input || !response) throw std::invalid_argument("Session, input and rendered response are required.");
        session->history.Commit(input, response, [&](const auto& text) { return session->state->Count(text); });
        return 1;
    }
    catch (const std::exception& e) { last_error = e.what(); }
    catch (...) { last_error = "Could not commit rendered exchange."; }
    return 0;
}

int SEMANTIC_CALL Semantic_Commit(Semantic_Session* session, const char* input, int index)
{
    last_error.clear();
    try
    {
        if (!session || !input || index < 0 || static_cast<std::size_t>(index) >= session->state->phrases.size())
            throw std::invalid_argument("Session, input and a valid selected phrase index are required.");
        session->history.Commit(input, session->state->phrases[index].response,
                               [&](const auto& text) { return session->state->Count(text); });
        return 1;
    }
    catch (const std::exception& e) { last_error = e.what(); }
    catch (...) { last_error = "Could not commit exchange."; }
    return 0;
}

int SEMANTIC_CALL Semantic_HistoryCount(Semantic_Session* session)
{
    return session ? static_cast<int>(session->history.History().size()) : 0;
}
const char* SEMANTIC_CALL Semantic_HistoryText(Semantic_Session* session, int index, int character)
{
    if (!session || index < 0 || static_cast<std::size_t>(index) >= session->history.History().size()) return nullptr;
    const auto& exchange = session->history.History()[index];
    return (character ? exchange.character : exchange.user).c_str();
}
