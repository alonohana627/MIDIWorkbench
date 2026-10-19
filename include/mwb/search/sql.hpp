#pragma once

#include "mwb/core/error.hpp"
#include "mwb/search/query.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/// Compilation of the typed query AST into parameterized SQL over the catalog schema of
/// specification section 23.2 (section 25.3). User text only ever travels as bound parameters;
/// full-text terms pass through the FTS query builder before binding.
namespace mwb::search {

/// A bound SQL parameter value.
using SqlValue = std::variant<std::int64_t, double, std::string>;

/// SQL text plus its parameters; placeholders are numbered ?1..?N in parameter order.
struct SqlFragment {
    std::string sql;
    std::vector<SqlValue> parameters;
};

/// Sort keys available for keyset pagination (section 19.4). Relevance needs a text term.
enum class SortField : std::uint8_t { title, modified, created, relevance };

/// Requested order.
struct SortOrder {
    SortField field = SortField::title;
    bool descending = false;
};

/// Keyset cursor: the last row's sort value plus its asset ID (never an offset).
struct PageCursor {
    SqlValue sort_value;
    std::int64_t id = 0;
    friend bool operator==(const PageCursor&, const PageCursor&) = default;
};

/// Page size limits (section 25.4).
inline constexpr std::size_t default_page_size = 100;
inline constexpr std::size_t maximum_page_size = 500;
/// Bound of a materialized relevance result set (paged in memory, never via OFFSET).
inline constexpr std::size_t relevance_materialize_limit = 5000;

/// Folded collection names resolved to manual-collection IDs by the caller before compilation,
/// so names are compared with the application's Unicode folding rather than SQL collation.
using CollectionNames = std::map<std::string, std::vector<std::int64_t>, std::less<>>;

/// Options shared by SQL compilation and in-memory evaluation.
struct SearchOptions {
    /// Section 25.2: musical predicates in one AND group must hold in the same sequence. False is
    /// the explicit "across any sequences" mode.
    bool same_sequence = true;
    /// Restrict to the members of one manual collection.
    std::optional<std::int64_t> collection_scope;
    /// Resolved names for `collection:` predicates.
    CollectionNames collections;
};

/// A complete page query. Result columns: id, sort_title, modified_ms, created_ms, rank.
struct CompiledSearch {
    SqlFragment statement;
    bool relevance = false;       ///< Ordered by FTS rank; the caller pages the bounded result.
    std::size_t row_limit = 0;    ///< LIMIT bound in the statement.
};

/// Quotes text as a single FTS5 string literal: everything inside is literal, so FTS operators
/// and column syntax in user text have no effect. Embedded double quotes are doubled.
[[nodiscard]] std::string fts_string_literal(std::string_view folded_text);

/// FTS5 expression of one full-text predicate ("title : \"...\"" for column filters).
[[nodiscard]] std::string fts_expression(const Predicate& predicate);

/// Boolean SQL expression over `asset AS a` for the query (no deleted filter, no ordering).
[[nodiscard]] Result<SqlFragment> compile_condition(const Query& query, const SearchOptions& options);

/// Full keyset page statement: active assets matching the query in the requested order after
/// `after`, at most `limit` rows (clamped to maximum_page_size; relevance pages read up to
/// relevance_materialize_limit rows and are paged by the caller).
[[nodiscard]] Result<CompiledSearch> compile_page_query(const Query& query, const SearchOptions& options,
                                                        const SortOrder& order, const std::optional<PageCursor>& after,
                                                        std::size_t limit);

/// True when the query has a positive full-text term usable as the relevance driver.
[[nodiscard]] bool supports_relevance(const Query& query);

/// Fixed rank function with the bm25 column weights of section 25.4.
inline constexpr std::string_view relevance_rank_function = "bm25(5.0, 1.0, 3.0, 1.0, 0.5)";

} // namespace mwb::search
