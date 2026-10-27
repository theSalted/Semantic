#include "Semantic.Native.h"
#include "Semantic.hpp"
#include "SemanticHybrid.hpp"
#include "SemanticDownload.hpp"
#include "llama.h"
#include "ggml-backend.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace
{
    thread_local std::string last_error;

#if defined(SEMANTIC_BACKEND_DL)
    // Directory of this plugin; GPU and CPU-variant backends ship beside it.
    std::string PluginDirectory()
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&PluginDirectory), &module))
            throw std::runtime_error("Cannot locate the Semantic plugin.");
        std::wstring path(MAX_PATH, L'\0');
        DWORD length;
        while ((length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()))) == path.size()) path.resize(path.size() * 2);
        path.resize(length);
        return std::filesystem::path(path).parent_path().u8string();
    }
#endif

    struct BackendLifetime
    {
        BackendLifetime()
        {
#if defined(SEMANTIC_BACKEND_DL)
            // Registers every backend this machine can run (CUDA, Vulkan, best CPU variant);
            // the chat model is then offloaded to the best available GPU, else runs on CPU.
            ggml_backend_load_all_from_path(PluginDirectory().c_str());
            if (ggml_backend_dev_count() == 0) throw std::runtime_error("No compute backends were found beside Semantic.Native.");
#endif
            llama_backend_init();
        }
        ~BackendLifetime() { llama_backend_free(); }
    };

    void InitializeBackend()
    {
        static BackendLifetime lifetime;
    }

    bool EndsWith(const std::string& text, const std::string& suffix)
    {
        return text.size() >= suffix.size() && Semantic::Hybrid::Lower(text.substr(text.size() - suffix.size())) == suffix;
    }

    std::vector<llama_token> TokenizeText(const llama_vocab* vocab, const std::string& text, bool parse_special)
    {
        if (text.size() > static_cast<std::size_t>(INT32_MAX)) throw std::invalid_argument("Text is too large.");
        const int count = -llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), nullptr, 0, false, parse_special);
        std::vector<llama_token> tokens(std::max(count, 0));
        if (count > 0 && llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), tokens.data(), count, false, parse_special) != count)
            throw std::runtime_error("Tokenization failed.");
        return tokens;
    }

    class LlmEngine;

    // GPU backends abort at process exit if buffers are still alive, and engines may still be
    // held then (a prepared model is kept warm; a host may skip disposal). This registry is
    // created after the backends, so it is destroyed first and releases any live engine.
    class EngineRegistry
    {
    public:
        static EngineRegistry& Instance()
        {
            InitializeBackend();
            ggml_backend_dev_count(); // Construct ggml's device registry before this object.
            static EngineRegistry registry;
            return registry;
        }
        void Add(LlmEngine* engine) { std::lock_guard<std::mutex> lock(gate); live.insert(engine); }
        void Remove(LlmEngine* engine) { std::lock_guard<std::mutex> lock(gate); live.erase(engine); }
        ~EngineRegistry();

    private:
        std::mutex gate;
        std::set<LlmEngine*> live;
    };

    // One loaded chat model per path, shared by every hybrid matcher in the process.
    // A new conversation is a new matcher; while another matcher holds the engine, no reload happens.
    class LlmEngine
    {
    public:
        // Downloads the model on first use when it is neither bundled nor cached.
        static std::shared_ptr<LlmEngine> Acquire(const std::string& bundled, const std::string& url, int gpu_layers,
            const Semantic::Download::Progress& progress)
        {
            static std::mutex cache_gate;
            static std::map<std::string, std::weak_ptr<LlmEngine>> cache;
            std::lock_guard<std::mutex> lock(cache_gate);
            const auto path = Semantic::Download::Ensure(std::filesystem::u8path(bundled), url, progress);
            if (auto engine = cache[path].lock()) return engine;
            auto engine = std::make_shared<LlmEngine>(path, gpu_layers);
            cache[path] = engine;
            return engine;
        }

        LlmEngine(const std::string& path, int gpu_layers)
        {
            if (!std::ifstream(std::filesystem::u8path(path))) throw std::runtime_error("Cannot open LLM model: " + path);
            InitializeBackend();
            auto model_params = llama_model_default_params();
            model_params.n_gpu_layers = gpu_layers;
            model.reset(llama_model_load_from_file(path.c_str(), model_params));
            if (!model) throw std::runtime_error("Could not load the LLM model.");
            const char* chat_template = llama_model_chat_template(model.get(), nullptr);
            if (!chat_template || std::string(chat_template).find("<|im_start|>") == std::string::npos)
                throw std::runtime_error("Hybrid mode expects a ChatML chat model such as Qwen3.");
            vocab = llama_model_get_vocab(model.get());

            auto params = llama_context_default_params();
            params.n_ctx = static_cast<uint32_t>(std::min(8192, llama_model_n_ctx_train(model.get())));
            params.n_batch = params.n_ubatch = 512;
            params.n_seq_max = 1;
            params.n_threads = params.n_threads_batch = 8;
            context.reset(llama_init_from_model(model.get(), params));
            if (!context) throw std::runtime_error("Could not create the LLM context.");
            context_limit = static_cast<int>(llama_n_ctx(context.get()));
            EngineRegistry::Instance().Add(this);
        }

        ~LlmEngine() { EngineRegistry::Instance().Remove(this); }

        // Frees model memory at process exit. A generation still running then keeps its resources.
        void Release()
        {
            std::unique_lock<std::mutex> lock(gate, std::try_to_lock);
            if (!lock) return;
            context.reset();
            model.reset();
        }

        std::vector<llama_token> Prompt(const std::string& system, const std::string& user) const
        {
            // Scaffolding is parsed for special tokens; dialogue content never is.
            std::vector<llama_token> tokens;
            for (const auto& [text, special] : std::vector<std::pair<std::string, bool>>{
                {"<|im_start|>system\n", true}, {system, false}, {"<|im_end|>\n<|im_start|>user\n", true}, {user, false},
                {"<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n", true}})
            {
                const auto part = TokenizeText(vocab, text, special);
                tokens.insert(tokens.end(), part.begin(), part.end());
            }
            return tokens;
        }

        // Greedy, grammar-constrained decoding. Throws unless the model reaches end-of-generation.
        std::string Generate(const std::vector<llama_token>& prompt, const std::string& grammar, int max_tokens)
        {
            std::lock_guard<std::mutex> lock(gate);
            std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)> sampler(llama_sampler_chain_init(llama_sampler_chain_default_params()), llama_sampler_free);
            auto* constrained = llama_sampler_init_grammar(vocab, grammar.c_str(), "root");
            if (!constrained) throw std::runtime_error("Invalid hybrid decoding grammar.");
            llama_sampler_chain_add(sampler.get(), constrained);
            llama_sampler_chain_add(sampler.get(), llama_sampler_init_greedy());

            // Reuse the KV cache for the shared prefix (system text and profile are stable across turns).
            std::size_t reuse = 0;
            while (reuse < cached.size() && reuse < prompt.size() && cached[reuse] == prompt[reuse]) ++reuse;
            reuse = std::min(reuse, prompt.size() - 1);
            auto* memory = llama_get_memory(context.get());
            if (!llama_memory_seq_rm(memory, 0, static_cast<llama_pos>(reuse), -1)) { llama_memory_clear(memory, true); reuse = 0; }
            cached.assign(prompt.begin(), prompt.begin() + reuse);
            for (std::size_t at = reuse; at < prompt.size(); at += 512)
            {
                std::vector<llama_token> chunk(prompt.begin() + at, prompt.begin() + std::min(prompt.size(), at + 512));
                Decode(chunk);
            }

            std::string output;
            for (int produced = 0; produced < max_tokens; ++produced)
            {
                const llama_token token = llama_sampler_sample(sampler.get(), context.get(), -1);
                if (llama_vocab_is_eog(vocab, token)) return output;
                char piece[256];
                const int length = llama_token_to_piece(vocab, token, piece, sizeof piece, 0, false);
                if (length < 0) throw std::runtime_error("Could not render a generated token.");
                output.append(piece, length);
                std::vector<llama_token> next{token};
                Decode(next);
            }
            throw std::runtime_error("The model did not finish a complete decision. Nothing was committed.");
        }

        int ContextLimit() const { return context_limit; }

    private:
        void Decode(std::vector<llama_token>& tokens)
        {
            if (llama_decode(context.get(), llama_batch_get_one(tokens.data(), static_cast<int32_t>(tokens.size()))) != 0)
            {
                cached.clear();
                llama_memory_clear(llama_get_memory(context.get()), true);
                throw std::runtime_error("LLM evaluation failed.");
            }
            cached.insert(cached.end(), tokens.begin(), tokens.end());
        }

        std::unique_ptr<llama_model, decltype(&llama_model_free)> model{nullptr, llama_model_free};
        std::unique_ptr<llama_context, decltype(&llama_free)> context{nullptr, llama_free};
        const llama_vocab* vocab = nullptr;
        int context_limit = 0;
        std::mutex gate;
        std::vector<llama_token> cached;
    };

    EngineRegistry::~EngineRegistry()
    {
        std::lock_guard<std::mutex> lock(gate);
        for (auto* engine : live) engine->Release();
    }

    // Conversation state lives inside a hybrid matcher; destroying the matcher ends the conversation.
    struct HybridState
    {
        Semantic::Hybrid::Config config;
        std::shared_ptr<LlmEngine> llm;
        std::unique_ptr<Semantic::Hybrid::FastPath> fast_path;
        std::vector<Semantic::Exchange> history;
        std::vector<Semantic::Hybrid::PlayerFact> memory;
        std::string response; // Generated text is owned here until the next match.
    };
}

struct MatcherState
{
    std::vector<Semantic::Phrase> phrases;
    std::unique_ptr<Semantic::Catalog> catalog;
    std::unique_ptr<llama_model, decltype(&llama_model_free)> model{nullptr, llama_model_free};
    std::unique_ptr<llama_context, decltype(&llama_free)> context{nullptr, llama_free};
    std::unique_ptr<HybridState> hybrid;
    int max_tokens;
    int model_tokens;
    static constexpr int reserved_output_tokens = 512;

    // `model_path` is the embedding GGUF (retrieval mode) or a hybrid-mode JSON config.
    MatcherState(const std::string& model_path, const std::string& bank_path, int budget,
                 std::unique_ptr<Semantic::Catalog> entries = nullptr, const Semantic::Download::Progress& progress = {})
        : phrases(entries ? std::vector<Semantic::Phrase>{} : Semantic::LoadPhraseBank(bank_path)),
          catalog(std::move(entries))
    {
        std::string embedding_path = model_path;
        if (EndsWith(model_path, ".json"))
        {
            if (catalog) throw std::invalid_argument("Hybrid mode uses a phrase bank, not a catalog.");
            hybrid = std::make_unique<HybridState>();
            hybrid->config = Semantic::Hybrid::LoadConfig(model_path);
            embedding_path = hybrid->config.embedding_model;
            hybrid->fast_path = std::make_unique<Semantic::Hybrid::FastPath>(hybrid->config.fast_path, phrases);
        }
        if (!std::ifstream(std::filesystem::u8path(embedding_path)))
            throw std::runtime_error("Cannot open model: " + embedding_path);
        InitializeBackend();
        auto model_params = llama_model_default_params();
        ggml_backend_dev_t cpu_only[] = {nullptr};
        model_params.devices = cpu_only;
        model_params.n_gpu_layers = 0; // CPU first: no float16 activation path for EmbeddingGemma2.
        model.reset(llama_model_load_from_file(embedding_path.c_str(), model_params));
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
        if (hybrid) hybrid->llm = LlmEngine::Acquire(hybrid->config.llm_model, hybrid->config.llm_url, hybrid->config.gpu_layers, progress);
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

    // Retrieval shortlist, then fast path, memory constraint or LLM select-or-generate.
    // The turn is committed to history only after a valid reply exists.
    const Semantic::Phrase* HybridMatch(const std::string& raw_input, float& score)
    {
        namespace H = Semantic::Hybrid;
        auto& state = *hybrid;
        const auto input = H::Trim(raw_input);
        H::ValidateText(input, 1000, "Input");
        H::Request request{input, state.history, H::Shortlist(phrases, Embed(input),
            static_cast<std::size_t>(state.config.candidates)), std::nullopt, std::nullopt};
        auto memory = state.memory;
        if (state.config.session_memory)
        {
            memory = H::AdvanceMemory(state.memory, input);
            request.memory = memory;
            request.required = H::Recall(input, memory);
            if (!request.required) request.required = H::NameAcknowledgement(input, state.memory, memory);
        }

        H::Decision decision;
        const auto fast = request.required ? H::FastPathDecision{false, "memory", {}}
            : state.fast_path->Evaluate(input, state.history, request.candidates, phrases);
        if (fast.accepted) decision = {phrases[*fast.index].response, fast.index};
        else
        {
            const auto system = H::SystemInstruction(request.memory.has_value());
            std::vector<llama_token> prompt;
            while (true)
            {
                prompt = state.llm->Prompt(system, H::UserContent(state.config.profile, request));
                if (static_cast<int>(prompt.size()) + reserved_output_tokens <= state.llm->ContextLimit()) break;
                if (!request.history.empty()) request.history.erase(request.history.begin());
                else if (request.candidates.size() > 1) request.candidates.pop_back();
                else throw std::runtime_error("Current query, profile and minimum candidate do not fit the LLM context. Nothing was committed.");
            }
            const auto output = state.llm->Generate(prompt, H::Grammar(state.config.profile, request), reserved_output_tokens);
            decision = H::Validate(state.config.profile, request, output);
        }

        score = 0;
        if (decision.index)
            for (const auto& candidate : request.candidates)
                if (candidate.index == *decision.index) score = candidate.similarity;
        state.history.push_back({input, decision.response});
        TrimHistory(state.history);
        state.memory = memory;
        state.response = decision.response;
        return decision.index ? &phrases[*decision.index] : nullptr;
    }

    // Retained history shares the embedding window's token budget, oldest exchanges first out.
    void TrimHistory(std::vector<Semantic::Exchange>& history) const
    {
        const auto* vocab = llama_model_get_vocab(model.get());
        auto cost = [&](const Semantic::Exchange& turn) { return TokenizeText(vocab, turn.user, false).size() + TokenizeText(vocab, turn.character, false).size(); };
        std::size_t total = 0;
        for (const auto& turn : history) total += cost(turn);
        while (history.size() > 1 && total > static_cast<std::size_t>(max_tokens)) { total -= cost(history.front()); history.erase(history.begin()); }
    }
};

namespace
{
    std::string PathKey(const char* path)
    {
        return std::filesystem::absolute(std::filesystem::u8path(path)).lexically_normal().u8string();
    }

    // Background preparations keyed by (model, bank). Leaked on purpose: detached workers may
    // still be downloading during static destruction at process exit.
    class Preparations
    {
    public:
        static Preparations& Instance() { static auto* instance = new Preparations; return *instance; }

        void Start(const std::string& model, const std::string& bank)
        {
            std::lock_guard<std::mutex> lock(gate);
            auto& entry = entries[{model, bank}];
            if (entry.state == Running || entry.state == Ready) return;
            entry = {Running, 0, "loading", nullptr, nullptr};
            std::thread([this, model, bank] { Run(model, bank); }).detach();
        }

        int Status(const std::string& model, const std::string& bank, float& progress, std::string& stage)
        {
            std::lock_guard<std::mutex> lock(gate);
            const auto found = entries.find({model, bank});
            if (found == entries.end()) { progress = 0; stage.clear(); return NotStarted; }
            progress = found->second.progress;
            stage = found->second.stage;
            return found->second.state;
        }

        // Waits for a running preparation; returns its matcher once, or null to build normally.
        std::shared_ptr<MatcherState> Take(const std::string& model, const std::string& bank)
        {
            std::unique_lock<std::mutex> lock(gate);
            const auto found = entries.find({model, bank});
            if (found == entries.end()) return nullptr;
            done.wait(lock, [&] { return found->second.state != Running; });
            return std::move(found->second.parked);
        }

    private:
        enum { Failed = -1, NotStarted = 0, Running = 1, Ready = 2 };
        struct Entry
        {
            int state;
            float progress;
            std::string stage;
            std::shared_ptr<MatcherState> parked; // Fresh conversation for the next CreateMatcher.
            std::shared_ptr<void> warm;           // Keeps a prepared chat model loaded.
        };

        void Run(const std::string& model, const std::string& bank)
        {
            const std::pair<std::string, std::string> key{model, bank};
            try
            {
                auto state = std::make_shared<MatcherState>(model, bank, 0, nullptr,
                    [this, key](unsigned long long received, unsigned long long total) {
                        std::lock_guard<std::mutex> lock(gate);
                        auto& entry = entries[key];
                        const bool finished = total && received >= total;
                        entry.stage = finished ? "loading" : "downloading";
                        entry.progress = total ? static_cast<float>(static_cast<double>(received) / total) : 0;
                    });
                std::lock_guard<std::mutex> lock(gate);
                entries[key] = {Ready, 1, "ready", state, state->hybrid ? std::shared_ptr<void>(state->hybrid->llm) : nullptr};
            }
            catch (const std::exception& e)
            {
                std::lock_guard<std::mutex> lock(gate);
                entries[key] = {Failed, 0, e.what(), nullptr, nullptr};
            }
            done.notify_all();
        }

        std::mutex gate;
        std::condition_variable done;
        std::map<std::pair<std::string, std::string>, Entry> entries;
    };
}

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
        if (capacity == 0)
            if (auto prepared = Preparations::Instance().Take(PathKey(model), PathKey(bank)))
                return new Semantic_Matcher{std::move(prepared)};
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
    const char* tag = nullptr;
    return Semantic_MatchWithTag(matcher, input, score, &tag);
}

const char* SEMANTIC_CALL Semantic_MatchWithTag(Semantic_Matcher* matcher, const char* input,
    float* score, const char** emote_tag)
{
    last_error.clear();
    if (score) *score = 0;
    if (emote_tag) *emote_tag = nullptr;
    try
    {
        if (!matcher || !input || !score || !emote_tag)
            throw std::invalid_argument("Matcher, input, score, and emote tag output are required.");
        auto& state = *matcher->state;
        if (state.hybrid)
        {
            const auto* phrase = state.HybridMatch(input, *score);
            *emote_tag = phrase ? phrase->emote_tag.c_str() : "";
            return phrase ? phrase->response.c_str() : state.hybrid->response.c_str();
        }
        const auto& phrase = Semantic::BestMatch(state.phrases, state.Embed(input), *score);
        *emote_tag = phrase.emote_tag.c_str();
        return phrase.response.c_str();
    }
    catch (const std::exception& e) { last_error = e.what(); }
    catch (...) { last_error = "Could not match input."; }
    return nullptr;
}

int SEMANTIC_CALL Semantic_Prepare(const char* model, const char* bank)
{
    last_error.clear();
    try
    {
        if (!model || !bank) throw std::invalid_argument("Model and bank paths are required.");
        Preparations::Instance().Start(PathKey(model), PathKey(bank));
        return 1;
    }
    catch (const std::exception& e) { last_error = e.what(); }
    catch (...) { last_error = "Could not start preparation."; }
    return 0;
}

int SEMANTIC_CALL Semantic_PrepareStatus(const char* model, const char* bank, float* progress, const char** stage)
{
    thread_local std::string stage_text;
    float fraction = 0;
    int state = 0;
    try
    {
        if (model && bank) state = Preparations::Instance().Status(PathKey(model), PathKey(bank), fraction, stage_text);
        else stage_text.clear();
    }
    catch (const std::exception& e) { state = -1; stage_text = e.what(); }
    if (progress) *progress = fraction;
    if (stage) *stage = stage_text.c_str();
    return state;
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
