#pragma once

#include "mwb/core/error.hpp"
#include "mwb/search/query.hpp"
#include "mwb/search/sql.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/// Compact in-memory browse index of specification section 19.4: evaluates the same query AST
/// with the same semantics as the SQL compiler, pages by keyset, and measures its own memory so
/// the caller can fall back to SQL paging when the budget is exceeded.
namespace mwb::search {

/// Track facts used by `channel:` and `program:`.
struct TrackFacts {
    std::uint16_t channel_mask = 0;      ///< Bit c for zero-based channel c.
    std::vector<std::uint8_t> programs;  ///< Zero-based program numbers (program_json[*].program).
};

/// One current-head sequence summary (mirrors sequence_summary/track_summary columns).
struct SequenceFacts {
    std::optional<double> tempo_representative;
    std::optional<double> tempo_min;
    std::optional<double> tempo_max;
    std::vector<double> tempos;               ///< diagnostics_json.tempos
    std::string tempo_origin = "unknown";
    std::optional<int> key_root;
    std::optional<std::string> key_mode;
    std::string key_origin = "unknown";
    std::optional<double> key_confidence;
    std::optional<double> quarter_length;
    double duration_seconds = 0.0;
    std::vector<std::string> meters;          ///< diagnostics_json.meter_names ("n/d")
    std::int64_t track_count = 0;
    std::int64_t note_count = 0;
    std::optional<int> min_pitch;
    std::optional<int> max_pitch;
    bool has_drums = false;
    std::vector<std::string> event_classes;   ///< diagnostics_json.event_classes
    std::vector<TrackFacts> tracks;
};

/// Analysis state of the current head, as reported by `status:`.
enum class AnalysisState : std::uint8_t { indexing, ready, warning, error };

/// Number of full-text columns (asset_fts: title, source_names, tags, description, lyrics).
inline constexpr std::size_t fts_column_count = 5;

/// Input row for the index: one active or trashed asset with its current-head facts.
struct BrowseRow {
    std::int64_t id = 0;
    std::string sort_title;
    std::int64_t modified_ms = 0;
    std::int64_t created_ms = 0;
    bool deleted = false;
    std::string role;
    bool favorite = false;
    int rating = 0;
    std::vector<std::string> tags;                 ///< Normalized tag names.
    std::vector<std::int64_t> collections;         ///< Manual collection IDs.
    AnalysisState analysis = AnalysisState::indexing;
    bool source_missing = false;
    std::array<std::string, fts_column_count> text; ///< Exact asset_fts column text.
    std::vector<SequenceFacts> sequences;
};

/// Three-valued evaluation result: `unknown` when the index cannot decide exactly what SQLite
/// would (non-ASCII text whose FTS tokenization only SQLite defines).
enum class Truth : std::uint8_t { no, yes, unknown };

/// One page served by the index.
struct BrowsePage {
    std::vector<std::int64_t> ids;
    std::optional<PageCursor> next;
};

/// The in-memory browse index.
class BrowseIndex {
public:
    /// Default memory budget at L100K (section 19.4).
    static constexpr std::size_t default_budget_bytes = 64u * 1024u * 1024u;

    explicit BrowseIndex(std::size_t budget_bytes = default_budget_bytes) : budget_bytes_(budget_bytes) {}

    /// Inserts or replaces a row (change outbox application).
    void upsert(const BrowseRow& row);
    /// Removes a row (purge).
    void remove(std::int64_t id);
    /// Removes everything (full reload).
    void clear();

    [[nodiscard]] std::size_t size() const { return rows_.size(); }
    /// The stored row, or null (page rows carry their sort keys from here).
    [[nodiscard]] const BrowseRow* find(std::int64_t id) const {
        const auto found = rows_.find(id);
        return found == rows_.end() ? nullptr : &found->second.row;
    }
    /// Measured bytes held by rows, dictionary and sort orders.
    [[nodiscard]] std::size_t memory_bytes() const { return memory_bytes_ + dictionary_bytes_; }
    [[nodiscard]] bool over_budget() const { return memory_bytes() > budget_bytes_; }

    /// Evaluates the query for one active row.
    [[nodiscard]] Truth evaluate(std::int64_t id, const Query& query, const SearchOptions& options) const;

    /// All active matching IDs in ascending ID order. Fails with `unsupported` when some row cannot
    /// be decided exactly (the caller then falls back to SQL).
    [[nodiscard]] Result<std::vector<std::int64_t>> matching_ids(const Query& query, const SearchOptions& options) const;

    /// Keyset page in title/modified/created order, identical to the SQL page query. Relevance order
    /// is not served by the index (`unsupported`).
    [[nodiscard]] Result<BrowsePage> page(const Query& query, const SearchOptions& options, const SortOrder& order,
                                          const std::optional<PageCursor>& after, std::size_t limit) const;

private:
    struct StoredRow {
        BrowseRow row;
        std::array<std::vector<std::uint32_t>, fts_column_count> tokens;
        bool text_exact = true;
        std::size_t bytes = 0;
    };

    /// Query text tokens resolved to dictionary ids once per call (nullopt: not ASCII-exact),
    /// not once per row.
    struct ResolvedText {
        std::vector<std::uint32_t> ids;
        std::vector<bool> last_prefix; ///< Indexed by token id: matches the prefix (prefix terms only).
    };
    using TokenCache = std::unordered_map<const Predicate*, std::optional<ResolvedText>>;

    [[nodiscard]] Truth evaluate_row(const StoredRow& row, const Query& query, const SearchOptions& options,
                                     TokenCache& cache) const;
    [[nodiscard]] Truth evaluate_node(const StoredRow& row, const QueryNode& node, const SearchOptions& options,
                                      TokenCache& cache) const;
    [[nodiscard]] Truth evaluate_text(const StoredRow& row, const Predicate& predicate, TokenCache& cache) const;
    [[nodiscard]] std::size_t measure(const StoredRow& row) const;
    [[nodiscard]] std::uint32_t intern(const std::string& token);

    std::size_t budget_bytes_;
    std::size_t memory_bytes_ = 0;
    std::unordered_map<std::int64_t, StoredRow> rows_;
    std::unordered_map<std::string, std::uint32_t> dictionary_;
    std::size_t dictionary_bytes_ = 0;
};

/// Evaluates the sequence-level part of a predicate against one sequence (shared helper, also
/// used by current-document search).
[[nodiscard]] bool sequence_matches(const SequenceFacts& sequence, const Predicate& predicate);

} // namespace mwb::search
