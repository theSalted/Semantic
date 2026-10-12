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

struct Semantic_Matcher
{
    std::vector<Semantic::Phrase> phrases;
    std::unique_ptr<llama_model, decltype(&llama_model_free)> model{nullptr, llama_model_free};
    std::unique_ptr<llama_context, decltype(&llama_free)> context{nullptr, llama_free};
    static constexpr int max_tokens = 512;

    Semantic_Matcher(const std::string& model_path, const std::string& bank_path)
        : phrases(Semantic::LoadPhraseBank(bank_path))
    {
        if (!std::ifstream(std::filesystem::u8path(model_path)))
            throw std::runtime_error("Cannot open model: " + model_path);
        InitializeBackend();
        auto model_params = llama_model_default_params();
        model_params.n_gpu_layers = 0; // CPU first: no float16 activation path for EmbeddingGemma2.
        model.reset(llama_model_load_from_file(model_path.c_str(), model_params));
        if (!model) throw std::runtime_error("Could not load the embedding model.");
        char architecture[64]{};
        llama_model_meta_val_str(model.get(), "general.architecture", architecture, sizeof architecture);
        if (std::string(architecture) != "gemma-embedding2" || llama_model_n_embd_out(model.get()) != 768)
            throw std::runtime_error("This experiment expects EmbeddingGemma2 with 768 output dimensions.");

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
        Semantic::EmbedPrompts(phrases, [this](const std::string& text) { return Embed(text); });
    }

    Semantic::Embedding Embed(const std::string& input)
    {
        if (Semantic::IsBlank(input)) throw std::invalid_argument("Input must not be empty.");
        // Official symmetric task prefix, applied to prompts AND user input:
        // https://huggingface.co/google/embeddinggemma-2#1-task-instruction-prefixes
        const std::string text = "task: sentence similarity | query: " + input;
        if (text.size() > static_cast<std::size_t>(INT32_MAX))
            throw std::invalid_argument("Input is too large.");
        std::vector<llama_token> tokens(max_tokens);
        // GGUF supplies its tokenizer and BOS/EOS policy. Treat user special-token text literally.
        const int count = llama_tokenize(llama_model_get_vocab(model.get()), text.data(),
            static_cast<int32_t>(text.size()), tokens.data(), max_tokens, true, false);
        if (count <= 0) throw std::invalid_argument("Input exceeds 512 tokens, including prefix and special tokens.");
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

Semantic_Matcher* SEMANTIC_CALL Semantic_CreateMatcher(const char* model, const char* bank)
{
    last_error.clear();
    try
    {
        if (!model || !bank) throw std::invalid_argument("Model and bank paths are required.");
        return new Semantic_Matcher(model, bank);
    }
    catch (const std::exception& e) { last_error = e.what(); }
    catch (...) { last_error = "Could not create matcher."; }
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
        return Semantic::BestMatch(matcher->phrases, matcher->Embed(input), *score).response.c_str();
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
