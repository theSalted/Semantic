#ifndef SEMANTIC_HPP
#define SEMANTIC_HPP

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Semantic
{
    using Embedding = std::vector<float>;

    struct Phrase
    {
        std::string prompt;
        std::string response;
        Embedding embedding;
    };

    inline bool IsBlank(const std::string& text)
    {
        return text.find_first_not_of(" \t\r\n") == std::string::npos;
    }

    // UTF-8 TSV: a prompt<TAB>response header, then one pair per line.
    // Tabs and newlines inside fields are deliberately not supported.
    inline std::vector<Phrase> LoadPhraseBank(const std::string& filename)
    {
        std::ifstream file(std::filesystem::u8path(filename));
        if (!file) throw std::runtime_error("Cannot open phrase bank: " + filename);
        std::string line;
        auto next_line = [&]() {
            if (!std::getline(file, line)) return false;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        };
        if (!next_line() || line != "prompt\tresponse")
            throw std::runtime_error("Phrase bank must start with prompt<TAB>response.");

        std::vector<Phrase> phrases;
        std::size_t line_number = 1;
        while (next_line())
        {
            ++line_number;
            if (IsBlank(line)) continue;
            const auto tab = line.find('\t');
            if (tab == std::string::npos || line.find('\t', tab + 1) != std::string::npos ||
                line.find('\0') != std::string::npos)
                throw std::runtime_error("Invalid phrase bank row " + std::to_string(line_number));
            Phrase phrase{line.substr(0, tab), line.substr(tab + 1), {}};
            if (IsBlank(phrase.prompt) || IsBlank(phrase.response))
                throw std::runtime_error("Empty prompt or response on row " + std::to_string(line_number));
            phrases.push_back(std::move(phrase));
        }
        if (file.bad()) throw std::runtime_error("Could not read phrase bank: " + filename);
        if (phrases.empty()) throw std::runtime_error("Phrase bank is empty.");
        return phrases;
    }

    inline Embedding Normalize(Embedding values)
    {
        if (values.empty()) throw std::runtime_error("Embedding is empty.");
        double squared_norm = 0;
        for (float value : values)
        {
            if (!std::isfinite(value)) throw std::runtime_error("Embedding contains a non-finite value.");
            squared_norm += static_cast<double>(value) * value;
        }
        if (!(squared_norm > 0) || !std::isfinite(squared_norm))
            throw std::runtime_error("Embedding has an invalid norm.");
        const double norm = std::sqrt(squared_norm);
        for (float& value : values) value = static_cast<float>(value / norm);
        return values;
    }

    template<class Embed>
    inline void EmbedPrompts(std::vector<Phrase>& bank, Embed embed)
    {
        if (bank.empty()) throw std::runtime_error("Phrase bank is empty.");
        std::size_t dimensions = 0;
        for (auto& phrase : bank)
        {
            phrase.embedding = Normalize(embed(phrase.prompt));
            if (dimensions && phrase.embedding.size() != dimensions)
                throw std::runtime_error("Embedding dimensions do not match.");
            dimensions = phrase.embedding.size();
        }
    }

    // Always returns the nearest stored phrase; the score is not a probability.
    inline const Phrase& BestMatch(const std::vector<Phrase>& bank, Embedding query, float& score)
    {
        if (bank.empty()) throw std::runtime_error("Phrase bank is empty.");
        query = Normalize(std::move(query));
        const Phrase* best = nullptr;
        double best_score = -std::numeric_limits<double>::infinity();
        for (const auto& phrase : bank)
        {
            if (phrase.embedding.size() != query.size())
                throw std::runtime_error("Embedding dimensions do not match; embed the bank first.");
            const auto normalized = Normalize(phrase.embedding);
            double similarity = 0;
            for (std::size_t i = 0; i < query.size(); ++i)
                similarity += static_cast<double>(normalized[i]) * query[i];
            if (similarity > best_score)
            {
                best_score = similarity;
                best = &phrase;
            }
        }
        score = static_cast<float>(std::clamp(best_score, -1.0, 1.0));
        return *best;
    }
}

#endif
