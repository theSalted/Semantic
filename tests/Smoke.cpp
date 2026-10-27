// Real-model checks through the C API.
// Usage: Semantic.Smoke <embedding.gguf> <tagged-bank.tsv>
// Retrieval and preparation always run. Hybrid turns also run when SEMANTIC_HYBRID_CONFIG
// names a hybrid config and SEMANTIC_HYBRID_BANK its bank (the chat model may download first).
#include "../Semantic.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

using Clock = std::chrono::steady_clock;

static long long Since(Clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

static std::string Turn(Semantic_Matcher* matcher, const char* input, std::string* tag = nullptr)
{
    const auto start = Clock::now();
    float score = 0; const char* emote = nullptr;
    const char* reply = Semantic_MatchWithTag(matcher, input, &score, &emote);
    if (!reply) { std::cerr << "FAIL " << input << ": " << Semantic_LastError() << "\n"; std::exit(1); }
    std::cout << Since(start) << "ms  [" << emote << " " << score << "]  " << input << "  ->  " << reply << "\n";
    if (tag) *tag = emote;
    return reply;
}

// Polls a preparation to completion, printing stage changes; returns the final state.
static int AwaitPrepared(const char* model, const char* bank)
{
    std::string last;
    while (true)
    {
        float progress = 0; const char* stage = nullptr;
        const int state = Semantic_PrepareStatus(model, bank, &progress, &stage);
        const std::string now = std::string(stage) + (std::strcmp(stage, "downloading") == 0 ? " " + std::to_string(static_cast<int>(progress * 100)) + "%" : "");
        if (now != last) { std::cout << "  prepare: " << now << "\n"; last = now; }
        if (state != 1) return state;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

int main(int argc, char** argv)
{
    assert(argc == 3);
    const char* model = argv[1];
    const char* bank = argv[2];
    std::cout << Semantic_BackendInfo() << "\n";

    // Retrieval mode: each authored prompt returns its own row and tag.
    auto* retrieval = Semantic_CreateMatcher(model, bank);
    if (!retrieval) { std::cerr << Semantic_LastError() << "\n"; return 1; }
    std::ifstream rows(bank);
    std::string line;
    std::getline(rows, line);
    while (std::getline(rows, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto tab = line.find('\t'), tag_tab = line.find('\t', tab + 1);
        std::string tag;
        assert(Turn(retrieval, line.substr(0, tab).c_str(), &tag) == line.substr(tab + 1, tag_tab - tab - 1));
        assert(tag == line.substr(tag_tab + 1));
    }
    Semantic_DestroyMatcher(retrieval);

    // Preparation: not started, then ready, then the next matcher is handed over immediately.
    assert(Semantic_PrepareStatus(model, bank, nullptr, nullptr) == 0);
    assert(Semantic_Prepare(model, bank) == 1);
    assert(AwaitPrepared(model, bank) == 2);
    auto start = Clock::now();
    auto* prepared = Semantic_CreateMatcher(model, bank);
    assert(prepared && Since(start) < 100);
    std::cout << "prepared matcher in " << Since(start) << "ms\n";
    Turn(prepared, "how are you");
    Semantic_DestroyMatcher(prepared);
    assert(Semantic_Prepare("missing.hybrid.json", bank) == 1);
    assert(AwaitPrepared("missing.hybrid.json", bank) == -1);
    assert(!Semantic_CreateMatcher("missing.hybrid.json", bank) && std::strlen(Semantic_LastError()) > 0);

    const char* config = std::getenv("SEMANTIC_HYBRID_CONFIG");
    const char* hybrid_bank = std::getenv("SEMANTIC_HYBRID_BANK");
    if (!config || !hybrid_bank) { std::cout << "Retrieval smoke passed (hybrid skipped)\n"; return 0; }

    start = Clock::now();
    assert(Semantic_Prepare(config, hybrid_bank) == 1);
    if (AwaitPrepared(config, hybrid_bank) != 2)
    {
        const char* stage = nullptr;
        Semantic_PrepareStatus(config, hybrid_bank, nullptr, &stage);
        std::cerr << "FAIL prepare: " << stage << "\n";
        return 1;
    }
    std::cout << "hybrid prepared in " << Since(start) << "ms\n";
    start = Clock::now();
    auto* hybrid = Semantic_CreateMatcher(config, hybrid_bank);
    assert(hybrid && Since(start) < 100);
    std::string tag;
    Turn(hybrid, "Hello!");
    Turn(hybrid, "My name is Mira.");
    assert(Turn(hybrid, "What's my name?", &tag) == "You told me your name is Mira." && tag.empty());
    Turn(hybrid, "What do you think about the moon landing?");
    Turn(hybrid, "Why not?");

    // New conversation: the prepared chat model stays loaded.
    start = Clock::now();
    auto* fresh = Semantic_CreateMatcher(config, hybrid_bank);
    Semantic_DestroyMatcher(hybrid);
    std::cout << "new conversation in " << Since(start) << "ms\n";
    assert(Turn(fresh, "What's my name?") == "You haven't told me your name yet.");
    float score = 0; const char* emote = nullptr;
    assert(!Semantic_MatchWithTag(fresh, "   ", &score, &emote) && std::strlen(Semantic_LastError()) > 0);
    Semantic_DestroyMatcher(fresh);
    std::cout << "Hybrid smoke passed\n";
}
