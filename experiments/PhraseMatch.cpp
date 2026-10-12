#include "bindings/Semantic.Native.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "Usage: Semantic.PhraseMatch model.gguf bank.tsv [input ...]\n"
                     "With no inputs, checks the sample bank's exact and paraphrase matches.\n";
        return 2;
    }
    try
    {
        std::unique_ptr<Semantic_Matcher, decltype(&Semantic_DestroyMatcher)> matcher(
            Semantic_CreateMatcher(argv[1], argv[2]), Semantic_DestroyMatcher);
        if (!matcher) throw std::runtime_error(Semantic_LastError());
        auto match = [&](const std::string& input, const std::string& expected = "", bool exact = false) {
            float score = 0;
            const char* result = Semantic_Match(matcher.get(), input.c_str(), &score);
            if (!result) throw std::runtime_error(Semantic_LastError());
            std::cout << input << " -> " << result << " (cosine " << score << ")\n";
            if (!std::isfinite(score) || score < -1 || score > 1 ||
                (!expected.empty() && result != expected) || (exact && score < 0.9999f))
                throw std::runtime_error("Unexpected response or cosine similarity.");
        };
        if (argc > 3)
        {
            for (int i = 3; i < argc; ++i) match(argv[i]);
        }
        else
        {
            for (const auto& pair : std::vector<std::pair<std::string, std::string>>{
                {"how are you", "I am fine thank you"},
                {"what is your name", "You can call me Semantic."},
                {"goodbye", "See you later."},
                {"thank you", "You are welcome."},
                {"I am hungry", "Would you like something to eat?"},
                {"bonjour", "Bonjour ! Comment ça va ?"}})
                match(pair.first, pair.second, true);
            match("How are you doing today?", "I am fine thank you");
            match("What should I call you?", "You can call me Semantic.");
            match("See you later!", "See you later.");
            match("Thanks a lot!", "You are welcome.");
            match("I could really use some food.", "Would you like something to eat?");

            float score;
            if (Semantic_Match(matcher.get(), "", &score) != nullptr)
                throw std::runtime_error("Empty input should fail.");
            if (Semantic_Match(matcher.get(), std::string(12000, 'x').c_str(), &score) != nullptr)
                throw std::runtime_error("Overlong input should fail.");
            match("how are you", "I am fine thank you", true); // failures must not poison the context
            std::cout << "Native phrase checks passed (768 dimensions, mean pooling, CPU).\n";
        }
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
