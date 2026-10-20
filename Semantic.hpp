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
#include <unordered_map>
#include <unordered_set>

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
    enum class ContextMode { InputOnly = 0, History = 1, Recent = 2, Blend = 3 };

    struct Candidate { std::size_t index; float score; };

    // Stable IDs are bank row indexes. Scores are similarities, not probabilities.
    inline std::vector<Candidate> TopMatches(const std::vector<Phrase>& bank, Embedding query, std::size_t k)
    {
        if (bank.empty() || k == 0) throw std::invalid_argument("A bank and positive candidate count are required.");
        query = Normalize(std::move(query));
        struct Ranked { std::size_t index; double score; };
        std::vector<Ranked> ranked;
        for (std::size_t row = 0; row < bank.size(); ++row)
        {
            if (bank[row].embedding.size() != query.size())
                throw std::runtime_error("Embedding dimensions do not match; embed the bank first.");
            auto values = Normalize(bank[row].embedding);
            double score = 0;
            for (std::size_t i = 0; i < values.size(); ++i) score += static_cast<double>(values[i]) * query[i];
            ranked.push_back({row, score});
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
        std::vector<Candidate> candidates;
        for (std::size_t i = 0; i < std::min(k, ranked.size()); ++i)
            candidates.push_back({ranked[i].index, static_cast<float>(std::clamp(ranked[i].score, -1.0, 1.0))});
        return candidates;
    }

    // Response identity is independent of retrieval keys. Empty key sets are valid.
    struct Response { std::string id, text; };
    struct TriggerKey { std::string id, response_id, text; Embedding embedding; };
    struct ResponseMatch { std::size_t response, key; float score; };

    class Catalog
    {
        std::unordered_map<std::string, std::size_t> lookup_;
        std::vector<std::size_t> key_responses_;
        std::vector<Response> responses;
        std::vector<TriggerKey> keys;
    public:
        const std::vector<Response>& Responses() const { return responses; }
        const std::vector<TriggerKey>& Keys() const { return keys; }
        Catalog(std::vector<Response> entries, std::vector<TriggerKey> triggers)
            : responses(std::move(entries)), keys(std::move(triggers))
        {
            if (responses.empty()) throw std::invalid_argument("Response catalog must not be empty.");
            auto valid = [](const std::string& s) { return !IsBlank(s) && s.find('\0') == std::string::npos; };
            for (std::size_t i = 0; i < responses.size(); ++i)
                if (!valid(responses[i].id) || !valid(responses[i].text) ||
                    !lookup_.emplace(responses[i].id, i).second)
                    throw std::invalid_argument("Response IDs must be unique; IDs and texts must be nonblank.");
            std::unordered_set<std::string> ids;
            for (const auto& key : keys)
            {
                if (!valid(key.id) || !valid(key.text) || !ids.insert(key.id).second ||
                    !lookup_.count(key.response_id))
                    throw std::invalid_argument("Keys require unique IDs, nonblank text and an existing response ID.");
                key_responses_.push_back(lookup_.at(key.response_id));
            }
        }
        const Response& Find(const std::string& id) const
        {
            const auto found = lookup_.find(id);
            if (found == lookup_.end()) throw std::invalid_argument("Unknown catalog response ID.");
            return responses[found->second];
        }
        template<class Embed> void EmbedKeys(Embed embed)
        {
            for (auto& key : keys) key.embedding = Normalize(embed(key.text));
        }
        // Max over keys, not a sum: adding identical keys cannot increase a score.
        // Ties use UTF-8 bytewise IDs, independent of either file's row order.
        std::vector<ResponseMatch> Rank(Embedding query, std::size_t k) const
        {
            if (!k) throw std::invalid_argument("A positive candidate count is required.");
            if (keys.empty()) return {};
            query = Normalize(std::move(query));
            const auto missing = keys.size();
            std::vector<std::size_t> winner(responses.size(), missing);
            std::vector<double> best(responses.size(), -std::numeric_limits<double>::infinity());
            for (std::size_t i = 0; i < keys.size(); ++i)
            {
                auto values = Normalize(keys[i].embedding);
                if (values.size() != query.size()) throw std::runtime_error("Embedding dimensions do not match.");
                double score = 0;
                for (std::size_t j = 0; j < values.size(); ++j) score += static_cast<double>(values[j]) * query[j];
                const auto r = key_responses_[i];
                if (score > best[r] || (score == best[r] && (winner[r] == missing || keys[i].id < keys[winner[r]].id)))
                { best[r] = score; winner[r] = i; }
            }
            std::vector<std::size_t> order;
            for (std::size_t r = 0; r < responses.size(); ++r) if (winner[r] != missing) order.push_back(r);
            std::sort(order.begin(), order.end(), [&](auto a, auto b) {
                return best[a] != best[b] ? best[a] > best[b] : responses[a].id < responses[b].id;
            });
            std::vector<ResponseMatch> result;
            for (std::size_t i = 0; i < std::min(k, order.size()); ++i)
            {
                const auto r = order[i];
                result.push_back({r, winner[r], static_cast<float>(std::clamp(best[r], -1.0, 1.0))});
            }
            return result;
        }
    };

    struct Exchange { std::string user; std::string character; };
    struct ContextInput
    {
        std::string text;
        std::size_t tokens = 0;
        std::size_t exchanges = 0;
    };

    // Plain role labels are data for an embedding model, never a chat template.
    inline std::string RoleText(const std::string& text)
    {
        std::string result;
        for (char c : text)
        {
            if (c == '\n') result += "\\n";
            else if (c == '\r') result += "\\r";
            else if (c == '\\') result += "\\\\";
            else if (c == '\0') throw std::invalid_argument("Text contains a null character.");
            else result += c;
        }
        return result;
    }

    // Engine-independent, owned per caller. No model state and no global history.
    // CountTokens must include the backend's prefix and special tokens.
    class Session
    {
        std::vector<Exchange> history_;
        std::size_t token_budget_;
        std::size_t max_exchanges_;

        std::string Render(std::size_t first) const
        {
            std::string text;
            for (std::size_t i = first; i < history_.size(); ++i)
                text += "User: " + RoleText(history_[i].user) +
                        "\nCharacter: " + RoleText(history_[i].character) + "\n";
            return text;
        }
    public:
        explicit Session(std::size_t token_budget = 512, std::size_t max_exchanges = 16)
            : token_budget_(token_budget), max_exchanges_(max_exchanges)
        {
            if (!token_budget || !max_exchanges || max_exchanges > 256)
                throw std::invalid_argument("Use a positive token budget and 1..256 exchanges.");
        }
        const std::vector<Exchange>& History() const { return history_; }
        std::size_t TokenBudget() const { return token_budget_; }
        void Reset() { history_.clear(); }

        template<class CountTokens>
        ContextInput Prepare(const std::string& input, ContextMode mode, CountTokens count) const
        {
            if (IsBlank(input) || input.size() > 65536)
                throw std::invalid_argument("Use nonempty input of at most 65536 UTF-8 bytes.");
            RoleText(input); // Validate even on the unlabelled input-only path.
            const auto input_tokens = count(input);
            if (input_tokens > token_budget_)
                throw std::invalid_argument("Current input exceeds the session token budget.");
            if (mode < ContextMode::InputOnly || mode > ContextMode::Blend)
                throw std::invalid_argument("Unknown context mode.");
            if (mode == ContextMode::InputOnly || history_.empty()) return {input, input_tokens, 0};
            std::size_t first = (mode == ContextMode::Recent || mode == ContextMode::Blend)
                ? history_.size() - 1 : 0;
            for (; first < history_.size(); ++first)
            {
                auto text = Render(first) + "User: " + RoleText(input);
                auto tokens = count(text);
                if (tokens <= token_budget_) return {std::move(text), tokens, history_.size() - first};
            }
            return {input, input_tokens, 0};
        }

        // Call only AFTER a response is selected/delivered. Previews never commit.
        // Complete exchanges are evicted oldest first. An oversized exchange is
        // omitted rather than splitting UTF-8 or silently truncating user text.
        template<class CountTokens>
        void Commit(const std::string& input, const std::string& selected, CountTokens count)
        {
            Prepare(input, ContextMode::InputOnly, count);
            if (IsBlank(selected) || selected.size() > 65536)
                throw std::invalid_argument("Selected response must contain 1..65536 UTF-8 bytes.");
            RoleText(selected);
            Session next = *this; // Strong exception guarantee, including tokenizer errors.
            next.history_.push_back({input, selected});
            while (!next.history_.empty() && (next.history_.size() > max_exchanges_ ||
                   count(next.Render(0)) > token_budget_))
                next.history_.erase(next.history_.begin());
            history_.swap(next.history_);
        }
    };

}

#endif
