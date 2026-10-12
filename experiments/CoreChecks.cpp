#include "Semantic.hpp"
#include <iostream>

template<class F> void MustFail(F operation)
{
    try { operation(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("Expected failure was not reported.");
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) throw std::runtime_error("Pass the sample bank path.");
        auto bank = Semantic::LoadPhraseBank(argv[1]);
        if (bank.size() != 6 || bank[0].prompt != "how are you" || bank[0].response != "I am fine thank you")
            throw std::runtime_error("Sample bank was not loaded correctly.");
        MustFail([] { Semantic::LoadPhraseBank("__missing_semantic_bank__.tsv"); });
        MustFail([] { Semantic::Normalize({}); });
        MustFail([] { Semantic::Normalize({0, 0}); });
        MustFail([] { Semantic::Normalize({std::numeric_limits<float>::quiet_NaN()}); });
        MustFail([] { Semantic::Normalize({std::numeric_limits<float>::infinity()}); });
        float score;
        MustFail([&] { Semantic::BestMatch({}, {1}, score); });
        MustFail([&] { Semantic::BestMatch({{"a", "A", {1, 0}}}, {1}, score); });
        std::vector<Semantic::Phrase> tiny{{"a", "A", {100, 0}}, {"b", "B", {0, 3}}};
        if (Semantic::BestMatch(tiny, {0, 200}, score).response != "B" || std::abs(score - 1) > 0.00001)
            throw std::runtime_error("Cosine normalization failed.");
        tiny[0].embedding = {-1, 0};
        tiny[1].embedding = {-1, 1};
        if (Semantic::BestMatch(tiny, {1, 0}, score).response != "B" || !(score < 0))
            throw std::runtime_error("Negative similarity ranking failed.");
        int count = 0;
        MustFail([&] { Semantic::EmbedPrompts(tiny, [&](const std::string&) {
            return ++count == 1 ? Semantic::Embedding{1} : Semantic::Embedding{1, 0};
        }); });
        std::cout << "Core checks passed.\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
