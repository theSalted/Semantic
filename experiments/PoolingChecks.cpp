#include "Semantic.hpp"
#include "llama.h"
#include <iostream>
#include <memory>

// Independent pooling check: explicitly mark every token as an output, obtain
// unpooled token vectors, and average them in C++. No session/adapter code used.
int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) throw std::runtime_error("Pass the local model path.");
        struct Backend { Backend() { llama_backend_init(); } ~Backend() { llama_backend_free(); } } backend;
        auto mp = llama_model_default_params();
        ggml_backend_dev_t cpu_only[] = {nullptr};
        mp.devices = cpu_only; mp.n_gpu_layers = 0;
        std::unique_ptr<llama_model, decltype(&llama_model_free)> model(
            llama_model_load_from_file(argv[1], mp), llama_model_free);
        if (!model) throw std::runtime_error("Cannot load model.");
        auto cp = llama_context_default_params();
        cp.n_ctx = cp.n_batch = cp.n_ubatch = 512;
        cp.n_seq_max = 1; cp.n_threads = cp.n_threads_batch = 4;
        cp.embeddings = true; cp.attention_type = LLAMA_ATTENTION_TYPE_NON_CAUSAL;
        cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
        cp.type_k = cp.type_v = GGML_TYPE_F32;
        cp.offload_kqv = cp.op_offload = false;
        cp.pooling_type = LLAMA_POOLING_TYPE_MEAN;
        std::unique_ptr<llama_context, decltype(&llama_free)> mean(llama_init_from_model(model.get(), cp), llama_free);
        cp.pooling_type = LLAMA_POOLING_TYPE_NONE;
        std::unique_ptr<llama_context, decltype(&llama_free)> unpooled(llama_init_from_model(model.get(), cp), llama_free);
        if (!mean || !unpooled) throw std::runtime_error("Cannot create contexts.");
        const int dimensions = llama_model_n_embd_out(model.get());
        for (const std::string input : {
            "Which direction did you come from?",
            "User: Hello Fellow Traveler\nCharacter: How are you doing?\nUser: Which direction did you come from?",
            "你好, bonjour, café! What is the price of a bag of cinnamon?"})
        {
            const std::string text = "task: sentence similarity | query: " + input;
            std::vector<llama_token> tokens(512);
            int count = llama_tokenize(llama_model_get_vocab(model.get()), text.data(),
                                       static_cast<int>(text.size()), tokens.data(), 512, true, false);
            if (count <= 0) throw std::runtime_error("Tokenization failed.");
            struct Batch
            {
                llama_batch data;
                explicit Batch(int size) : data(llama_batch_init(size, 0, 1)) { data.n_tokens = size; }
                ~Batch() { llama_batch_free(data); }
            } batch(count);
            for (int i = 0; i < count; ++i)
            {
                batch.data.token[i] = tokens[i]; batch.data.pos[i] = i;
                batch.data.n_seq_id[i] = 1; batch.data.seq_id[i][0] = 0; batch.data.logits[i] = 1;
            }
            if (llama_encode(mean.get(), batch.data) || llama_encode(unpooled.get(), batch.data))
                throw std::runtime_error("Encoder failed.");
            const float* pooled = llama_get_embeddings_seq(mean.get(), 0);
            if (!pooled) throw std::runtime_error("Missing pooled output.");
            Semantic::Embedding expected(dimensions, 0);
            for (int i = 0; i < count; ++i)
            {
                const float* token = llama_get_embeddings_ith(unpooled.get(), i);
                if (!token) throw std::runtime_error("Missing token output.");
                for (int d = 0; d < dimensions; ++d) expected[d] += token[d] / count;
            }
            double error = 0, norm = 0;
            for (int d = 0; d < dimensions; ++d)
            { error += std::pow(expected[d] - pooled[d], 2); norm += std::pow(pooled[d], 2); }
            const double relative = std::sqrt(error / norm);
            if (!std::isfinite(relative) || relative > 1e-4)
                throw std::runtime_error("Mean pooling differs from the explicit token average.");
            std::cout << "Mean pooling equals explicit average: tokens=" << count
                      << " dimensions=" << dimensions << " relative_error=" << relative << '\n';
        }
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
