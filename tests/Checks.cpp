// Model-free checks of the retrieval core and hybrid-mode logic (no models are loaded).
// Usage: Semantic.Checks <phrases.tsv> <tagged-phrases.tsv>
#include "../Semantic.cpp"
#include <cassert>
#include <iostream>

using namespace Semantic::Hybrid;

template<class F> void MustFail(F operation)
{
    try { operation(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("Expected failure was not reported.");
}

static std::string Value(const std::vector<PlayerFact>& facts, const std::string& key)
{
    const auto* fact = FindFact(facts, key);
    return fact ? fact->value : "";
}

int main(int argc, char** argv)
{
    assert(argc == 3);
    // ---- Retrieval core
    {
        auto bank = Semantic::LoadPhraseBank(argv[1]);
        assert(bank.size() == 6 && bank[0].prompt == "how are you" && bank[0].response == "I am fine thank you" && bank[0].emote_tag.empty());
        auto tagged = Semantic::LoadPhraseBank(argv[2]); // Optional third column: each row keeps its own tag.
        assert(tagged.size() == 5 && tagged[0].emote_tag == "Happy" && tagged[2].emote_tag == "Calm");
        MustFail([] { Semantic::LoadPhraseBank("__missing_semantic_bank__.tsv"); });
        MustFail([] { Semantic::Normalize({}); });
        MustFail([] { Semantic::Normalize({0, 0}); });
        MustFail([] { Semantic::Normalize({std::numeric_limits<float>::quiet_NaN()}); });
        MustFail([] { Semantic::Normalize({std::numeric_limits<float>::infinity()}); });
        float score;
        MustFail([&] { Semantic::BestMatch({}, {1}, score); });
        MustFail([&] { Semantic::BestMatch({{"a", "A", {1, 0}}}, {1}, score); });
        std::vector<Semantic::Phrase> tiny{{"a", "A", {100, 0}}, {"b", "B", {0, 3}}};
        assert(Semantic::BestMatch(tiny, {0, 200}, score).response == "B" && std::abs(score - 1) < 0.00001); // Cosine normalization.
        tiny[0].embedding = {-1, 0};
        tiny[1].embedding = {-1, 1};
        assert(Semantic::BestMatch(tiny, {1, 0}, score).response == "B" && score < 0); // Negative similarities still rank.
        auto ranked = Semantic::TopMatches(tiny, {1, 0}, 8);
        assert(ranked.size() == 2 && ranked[0].index == 1 && ranked[1].index == 0);
        MustFail([&] { Semantic::TopMatches(tiny, {1, 0}, 0); });
        MustFail([&] { Semantic::TopMatches(tiny, {1}, 2); });
        tiny[0].embedding = tiny[1].embedding;
        assert(Semantic::TopMatches(tiny, {1, 0}, 2)[0].index == 0); // Ties preserve bank order.
        int embeddings_seen = 0;
        MustFail([&] { Semantic::EmbedPrompts(tiny, [&](const std::string&) {
            return ++embeddings_seen == 1 ? Semantic::Embedding{1} : Semantic::Embedding{1, 0};
        }); });
    }

    // ---- Hybrid mode

    // Explicit declarations, corrections and removal.
    auto memory = AdvanceMemory({}, "Hello! My name is Mira.");
    assert(Value(memory, "name") == "Mira");
    assert(NameAcknowledgement("Hello! My name is Mira.", {}, memory) == std::nullopt); // Evidence clause differs from whole input.
    assert(*NameAcknowledgement("My name is Mira.", {}, AdvanceMemory({}, "My name is Mira.")) == "All right, I'll call you Mira.");
    auto renamed = AdvanceMemory(memory, "Actually, call me Jun.");
    assert(Value(renamed, "name") == "Jun");
    assert(*NameAcknowledgement("Actually, call me Jun.", memory, renamed) == "All right, I'll call you Jun.");
    auto curly = AdvanceMemory({}, "I\xE2\x80\x99m called Ana-Lu\xC3\xADsa");
    assert(Value(curly, "name") == "Ana-Lu\xC3\xADsa");
    auto forgotten = AdvanceMemory(renamed, "Forget my name.");
    assert(Value(forgotten, "name").empty() && *NameAcknowledgement("Forget my name.", renamed, forgotten) == "All right, I won't use that name.");
    assert(Value(AdvanceMemory(renamed, "My name isn't Jun"), "name").empty());
    assert(Value(AdvanceMemory(renamed, "My name isn't Bob"), "name") == "Jun");

    // Quotes, hedges, third-party context and non-names are not identity evidence.
    assert(AdvanceMemory({}, "She said \"my name is Mira\"").empty());
    assert(AdvanceMemory({}, "My name is not important").empty());
    assert(AdvanceMemory({}, "My friend arrived. My name is Mira.").empty());
    assert(AdvanceMemory({}, "Call me the stranger").empty());
    assert(AdvanceMemory({}, "Call me R2D2").empty());

    auto many = AdvanceMemory({}, "My favourite drink is tea. My favorite food is bread. My favorite colour is red. I live in Dale. I'm from the coast. I lost my hat. My name is Mira.");
    assert(many.size() == 7 && many.front().key == "name" && Value(many, "favorite_drink") == "tea");

    // Supported recall questions only.
    assert(*Recall("What's my name?", many) == "You told me your name is Mira.");
    assert(*Recall("where do I live", many) == "You told me you live in Dale.");
    assert(*Recall("What is my favourite colour?", many) == "You told me your favorite color is red.");
    assert(*Recall("What is my name?", {}) == "You haven't told me your name yet.");
    assert(*Recall("What did I lose?", {}) == "You haven't told me what you lost yet.");
    assert(!Recall("Who am I really?", many));

    // Fast path: reviewed exact trigger, distinct-reply margin, conflicting rows and contextual history.
    std::vector<Semantic::Phrase> rows = {
        {"I'll have whiskey", "Whiskey it is.", {}, "Happy"},
        {"I'll have wine", "We have wine.", {}, "Calm"},
        {"Twice", "One.", {}, "A"}, {"Twice", "Two.", {}, "B"}};
    FastPath fast({true, 0.99, 0.06, {"I'll have whiskey", "Twice", "I'll have wine"}}, rows);
    std::vector<Candidate> clear = {{0, "Whiskey it is.", 0.995f}, {0, "Whiskey it is.", 0.99f}, {1, "We have wine.", 0.90f}};
    assert(fast.Evaluate("I\xE2\x80\x99ll have whiskey!", {}, clear, rows).accepted);
    assert(*fast.Evaluate("i'll HAVE whiskey", {}, clear, rows).index == 0);
    assert(!fast.Evaluate("I'll have whiskey, not wine", {}, clear, rows).accepted);
    assert(!fast.Evaluate("I'll have whiskey", {{"I'm allergic to rye", "Noted."}}, clear, rows).accepted);
    std::vector<Candidate> close = {{0, "Whiskey it is.", 0.995f}, {1, "We have wine.", 0.96f}};
    assert(fast.Evaluate("I'll have whiskey", {}, close, rows).reason == "distinct reply margin too small");
    assert(fast.Evaluate("Twice", {}, {{2, "One.", 1.0f}, {1, "We have wine.", 0.1f}}, rows).reason == "conflicting authored mappings or tags");
    assert(!FastPath({false, 0.99, 0.06, {"I'll have whiskey"}}, rows).Evaluate("I'll have whiskey", {}, clear, rows).accepted);

    // Shortlist ranking is stable and keeps row identity and text.
    std::vector<Semantic::Phrase> bank = {{"a", "A", {1, 0}, "x"}, {"b", "B", {0, 1}, "y"}, {"c", "C", {1, 0}, "z"}};
    auto top = Shortlist(bank, {1, 0}, 2);
    assert(top.size() == 2 && top[0].index == 0 && top[1].index == 2 && top[1].text == "C" && top[0].similarity == 1);

    // Prompt, grammar and validation contract.
    Profile profile{"Pell", "Gruff", "Bar", "None", {{"identity", "Owns the bar."}}};
    Request request{"Hi", {{"Hello", "What?"}}, {{3, "Welcome.", 0.8f}, {7, "Bar's open.", 0.7f}}, std::vector<PlayerFact>{{"name", "Mira", "My name is Mira"}}, std::nullopt};
    const auto content = Json::parse(UserContent(profile, request));
    assert(content.begin().key() == "profile" && content["player_memory"][0]["id"] == "player:name" && content["candidates"][1]["id"] == "7");
    const auto grammar = Grammar(profile, request);
    assert(grammar.find("\"3\" | \"7\"") != std::string::npos && grammar.find("\"\\\"player:name\\\"\"") != std::string::npos);
    assert(Validate(profile, request, R"({"selected_id":"7"})").index == 7u);
    assert(!Validate(profile, request, R"({"response":"Aye, Mira.","used_fact_ids":["player:name"]})").index);
    assert(Validate(profile, request, R"({"response":"Welcome.","used_fact_ids":[]})").index == 3u); // Exact copy keeps authorship.
    bool threw = false;
    try { Validate(profile, request, R"({"selected_id":"5"})"); } catch (const std::runtime_error&) { threw = true; }
    assert(threw);
    threw = false;
    try { Validate(profile, request, R"({"response":"Hi","used_fact_ids":["made_up"]})"); } catch (const std::runtime_error&) { threw = true; }
    assert(threw);

    request.required = "You told me your name is \"Mira\".";
    const auto constrained = Grammar(profile, request);
    assert(constrained.find("select") == std::string::npos && constrained.find("\\\\\\\"Mira\\\\\\\"") != std::string::npos);
    threw = false;
    try { Validate(profile, request, R"({"response":"Your name is Bob.","used_fact_ids":[]})"); } catch (const std::runtime_error&) { threw = true; }
    assert(threw);

    std::cout << "Checks passed\n";
}
