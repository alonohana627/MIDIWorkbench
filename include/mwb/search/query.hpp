#pragma once

#include "mwb/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/// Structured library query language of specification section 25.2: a PEGTL grammar parsed into
/// a typed, validated AST that both the SQL compiler and the in-memory browse index evaluate.
namespace mwb::search {

/// Byte range [begin, end) inside the original query text.
struct SourceSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
    friend bool operator==(const SourceSpan&, const SourceSpan&) = default;
};

/// Query limits (section 28.3: 4 KiB / depth 16 / 128 nodes).
struct QueryLimits {
    std::size_t max_bytes = 4096;
    std::size_t max_depth = 16;
    std::size_t max_nodes = 128;
};

/// Searchable fields of the section 25.2 table. `any_text` is a bare or quoted term.
enum class Field : std::uint8_t {
    any_text,
    tag,
    title,
    text,
    bpm,
    tempo_any,
    tempo_origin,
    key,
    key_origin,
    confidence,
    length_qn,
    duration,
    meter,
    tracks,
    notes,
    pitch_min,
    pitch_max,
    channel,
    program,
    role,
    drums,
    favorite,
    rating,
    has,
    source,
    collection,
    status,
};

/// Field name as written in queries ("tempo.any"); "text" for any_text.
[[nodiscard]] std::string_view field_name(Field field) noexcept;

/// True for predicates evaluated per sequence of the current head (same-sequence rule, 25.2).
[[nodiscard]] bool is_sequence_field(Field field) noexcept;

/// True for predicates answered by the full-text index (title, text, source, bare terms).
[[nodiscard]] bool is_text_field(Field field) noexcept;

/// Comparison operator of a numeric value; `range` is inclusive [low, high].
enum class Comparator : std::uint8_t { equal, greater, greater_equal, less, less_equal, range };

/// Numeric condition after unit conversion (duration in seconds, program/channel in the 1-based
/// user convention).
struct NumericCondition {
    Comparator comparator = Comparator::equal;
    double value = 0.0;
    double high = 0.0; ///< Upper bound for Comparator::range.
    friend bool operator==(const NumericCondition&, const NumericCondition&) = default;
};

/// A key such as "F# minor": root pitch class and optional folded mode name (empty = any mode).
struct KeyValue {
    int root = 0;
    std::string mode;
    friend bool operator==(const KeyValue&, const KeyValue&) = default;
};

/// A meter such as "6/8".
struct MeterValue {
    int numerator = 4;
    int denominator = 4;
    friend bool operator==(const MeterValue&, const MeterValue&) = default;
};

/// Value carried by a predicate. Text values are already normalized (fold_for_search).
using PredicateValue = std::variant<std::string, NumericCondition, bool, KeyValue, MeterValue>;

/// One validated term.
struct Predicate {
    Field field = Field::any_text;
    PredicateValue value;
    bool quoted = false; ///< Value was a quoted string (phrase semantics for text fields).
    friend bool operator==(const Predicate&, const Predicate&) = default;
};

/// AST node kinds. An `and_node` without children matches everything (empty query).
enum class NodeKind : std::uint8_t { and_node, or_node, not_node, predicate };

/// Typed AST node with the source span it was parsed from.
struct QueryNode {
    NodeKind kind = NodeKind::and_node;
    std::vector<QueryNode> children;
    Predicate predicate; ///< Valid when kind == predicate.
    SourceSpan span;
    friend bool operator==(const QueryNode&, const QueryNode&) = default;
};

/// A parsed query: the original text, its AST and the AST node count.
struct Query {
    std::string text;
    QueryNode root;
    std::size_t node_count = 0;
};

/// A rejected query with a stable code, readable message and the exact offending span.
struct QueryError {
    std::string code;    ///< For example "query.syntax", "query.value", "query.limit".
    std::string message;
    SourceSpan span;

    /// Converts into the module-independent error type (span in the context).
    [[nodiscard]] Error to_error() const;
};

/// Result of parsing.
using QueryResult = std::expected<Query, QueryError>;

/// Parses and validates a query. Whitespace-only text yields the match-all query.
[[nodiscard]] QueryResult parse_query(std::string_view text, const QueryLimits& limits = {});

/// Current version of the AST JSON envelope stored in smart collections (section 24.1).
inline constexpr int query_json_version = 1;

/// Versioned JSON form of the AST (never SQL) for smart collections.
[[nodiscard]] std::string query_to_json(const Query& query);

/// Rebuilds and revalidates an AST from its JSON form (limits are enforced again).
[[nodiscard]] Result<Query> query_from_json(std::string_view json, const QueryLimits& limits = {});

/// Splits folded text into the ASCII tokens produced by FTS5 unicode61 for ASCII input:
/// maximal runs of [a-z0-9]. Returns nullopt when the text contains non-ASCII bytes, whose
/// tokenization only SQLite can decide.
[[nodiscard]] std::optional<std::vector<std::string>> ascii_fts_tokens(std::string_view folded_text);

/// Inclusive integer interval a numeric condition selects over [minimum, maximum]
/// (empty when low > high). Used identically by SQL compilation and in-memory evaluation.
struct IntegerInterval {
    std::int64_t low = 0;
    std::int64_t high = -1;
    [[nodiscard]] bool empty() const { return low > high; }
};
[[nodiscard]] IntegerInterval integer_interval(const NumericCondition& condition, std::int64_t minimum,
                                               std::int64_t maximum);

/// Real-valued interval with open/closed ends. Equality on real fields uses +/- real_epsilon.
struct RealInterval {
    double low = 0.0;
    double high = 0.0;
    bool low_open = false;
    bool high_open = false;
    bool has_low = true;
    bool has_high = true;
    [[nodiscard]] bool contains(double value) const;
};
inline constexpr double real_epsilon = 0.005;
[[nodiscard]] RealInterval real_interval(const NumericCondition& condition);

/// Stored mode name ("minor", "major" or a church mode) of user text such as "m", "Minor" or
/// "aeolian"; nullopt when the text names no supported mode. Shared by `key:` and key overrides.
[[nodiscard]] std::optional<std::string> normalize_key_mode(std::string_view text);

/// Default minimum confidence for estimated keys to satisfy `key:` (section 25.2).
inline constexpr double estimated_key_min_confidence = 0.60;

} // namespace mwb::search
