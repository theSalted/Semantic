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
        auto ranked = Semantic::TopMatches(tiny, {1, 0}, 8);
        if (ranked.size() != 2 || ranked[0].index != 1 || ranked[1].index != 0)
            throw std::runtime_error("Candidate order or count failed.");
        MustFail([&] { Semantic::TopMatches(tiny, {1, 0}, 0); });
        MustFail([&] { Semantic::TopMatches(tiny, {1}, 2); });
        tiny[0].embedding = tiny[1].embedding;
        if (Semantic::TopMatches(tiny, {1, 0}, 2)[0].index != 0)
            throw std::runtime_error("Candidate ties must preserve bank order.");
        int embeddings_seen = 0;
        MustFail([&] { Semantic::EmbedPrompts(tiny, [&](const std::string&) {
            return ++embeddings_seen == 1 ? Semantic::Embedding{1} : Semantic::Embedding{1, 0};
        }); });
        auto count = [](const std::string& text) { return text.size() + 5; };
        Semantic::Session a(100, 2), b(100, 2);
        a.Commit("first", "selected", count);
        auto before = a.History().size();
        auto preview = a.Prepare("next", Semantic::ContextMode::History, count);
        if (preview.text != "User: first\nCharacter: selected\nUser: next" ||
            preview.tokens > 100 || preview.exchanges != 1 || a.History().size() != before || !b.History().empty())
            throw std::runtime_error("History roles, purity or isolation failed.");
        if (a.Prepare("next", Semantic::ContextMode::InputOnly, count).text != "next")
            throw std::runtime_error("Input-only behavior changed.");
        a.Commit("second", "reply", count);
        a.Commit("third", "reply", count);
        if (a.History().size() != 2 || a.History()[0].user != "second")
            throw std::runtime_error("Exchange bound failed.");
        auto recent = a.Prepare("next", Semantic::ContextMode::Recent, count);
        if (recent.exchanges != 1 || recent.text.find("second") != std::string::npos)
            throw std::runtime_error("Recency selection failed.");
        auto over = a.Prepare(std::string(95, 'x'), Semantic::ContextMode::History, count);
        if (over.exchanges != 0 || over.tokens != 100) throw std::runtime_error("Budget boundary failed.");
        MustFail([&] { a.Prepare(std::string(96, 'x'), Semantic::ContextMode::History, count); });
        MustFail([&] { a.Prepare("next", static_cast<Semantic::ContextMode>(42), count); });
        MustFail([&] { a.Commit("valid", "reply", [](const std::string&) -> std::size_t { throw std::runtime_error("tokenizer"); }); });
        if (a.History().size() != 2) throw std::runtime_error("Failed commit mutated history.");
        a.Reset();
        a.Commit("你好\nUser: literal", "réponse", count);
        if (a.Prepare("再见", Semantic::ContextMode::History, count).text.find("你好\\nUser: literal") == std::string::npos)
            throw std::runtime_error("UTF-8 or role escaping failed.");
        a.Commit("short", std::string(100, 'r'), count);
        if (!a.History().empty()) throw std::runtime_error("Oversized exchange not evicted.");
        a.Reset();
        if (a.Prepare("next", Semantic::ContextMode::History, count).text != "next")
            throw std::runtime_error("Reset failed.");
        const std::string exact = " \t你好\r\nNo.  ";
        std::vector<Semantic::Response> responses{{"z", exact}, {"a", "Same text"}, {"b", "Same text"}, {"unkeyed", "Retained"}};
        std::vector<Semantic::TriggerKey> keys{
            {"k2", "z", "duplicate", {1, 0}}, {"k1", "z", "duplicate", {1, 0}},
            {"k3", "a", "duplicate", {1, 0}}, {"k4", "b", "another", {0.8f, 0.6f}}};
        Semantic::Catalog catalog(responses, keys);
        auto hits = catalog.Rank({1, 0}, 3);
        if (hits.size() != 3 || catalog.Responses()[hits[0].response].id != "a" ||
            catalog.Responses()[hits[1].response].id != "z" || catalog.Keys()[hits[1].key].id != "k1" ||
            catalog.Responses()[hits[2].response].id != "b" || catalog.Find("z").text != exact)
            throw std::runtime_error("Catalog max-key aggregation, dedup-before-k, exact text or ID ties failed.");
        if (catalog.Responses().size() != 4 || catalog.Find("unkeyed").text != "Retained")
            throw std::runtime_error("Unkeyed response was lost.");
        std::reverse(responses.begin(), responses.end()); std::reverse(keys.begin(), keys.end());
        Semantic::Catalog reordered(responses, keys);
        auto reordered_hits = reordered.Rank({1, 0}, 3);
        for (std::size_t i = 0; i < hits.size(); ++i)
            if (catalog.Responses()[hits[i].response].id != reordered.Responses()[reordered_hits[i].response].id ||
                catalog.Keys()[hits[i].key].id != reordered.Keys()[reordered_hits[i].key].id ||
                hits[i].score != reordered_hits[i].score)
                throw std::runtime_error("Catalog/key reordering changed response identity or ranking.");
        Semantic::Catalog no_keys(responses, {});
        if (!no_keys.Rank({1, 0}, 3).empty() || no_keys.Responses().size() != 4)
            throw std::runtime_error("Zero-key catalog must retain responses without fabricated retrieval.");
        MustFail([&] { catalog.Find("missing"); });
        MustFail([&] { Semantic::Catalog({{"same", "one"}, {"same", "two"}}, {}); });
        MustFail([&] { Semantic::Catalog(responses, {{"same", "a", "x", {}}, {"same", "b", "y", {}}}); });
        MustFail([&] { Semantic::Catalog(responses, {{"key", "missing", "x", {}}}); });
        MustFail([&] { catalog.Rank({1}, 3); });
        std::cout << "Core checks passed, including catalog identity, exact text, zero/multiple keys, stable ranking and session ownership.\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
