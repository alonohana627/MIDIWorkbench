// Query AST -> parameterized SQL over the catalog schema (specification sections 23.2, 25.3).

#include "mwb/search/sql.hpp"

#include "mwb/core/utility.hpp"

#include <algorithm>
#include <format>
#include <limits>

namespace mwb::search {

namespace {

/// Accumulates SQL text and parameters; every parameter gets an explicit ?N placeholder.
class SqlBuilder {
public:
    void append(std::string_view text) { sql_ += text; }

    /// Appends a placeholder bound to `value`.
    void parameter(SqlValue value) {
        parameters_.push_back(std::move(value));
        sql_ += std::format("?{}", parameters_.size());
    }

    [[nodiscard]] SqlFragment finish() && { return SqlFragment{std::move(sql_), std::move(parameters_)}; }

private:
    std::string sql_;
    std::vector<SqlValue> parameters_;
};

/// True when every leaf of the subtree is a per-sequence predicate.
bool is_pure_sequence(const QueryNode& node) {
    if (node.kind == NodeKind::predicate)
        return is_sequence_field(node.predicate.field);
    if (node.children.empty())
        return false;
    return std::ranges::all_of(node.children, [](const QueryNode& child) { return is_pure_sequence(child); });
}

void real_condition(SqlBuilder& builder, std::string_view expression, const NumericCondition& condition) {
    const auto interval = real_interval(condition);
    builder.append("(");
    builder.append(expression);
    builder.append(" IS NOT NULL");
    if (interval.has_low) {
        builder.append(std::format(" AND {} {} ", expression, interval.low_open ? ">" : ">="));
        builder.parameter(interval.low);
    }
    if (interval.has_high) {
        builder.append(std::format(" AND {} {} ", expression, interval.high_open ? "<" : "<="));
        builder.parameter(interval.high);
    }
    builder.append(")");
}

void integer_condition(SqlBuilder& builder, std::string_view expression, const IntegerInterval& interval) {
    if (interval.empty()) {
        builder.append("0");
        return;
    }
    builder.append(std::format("({} IS NOT NULL AND {} BETWEEN ", expression, expression));
    builder.parameter(interval.low);
    builder.append(" AND ");
    builder.parameter(interval.high);
    builder.append(")");
}

constexpr std::int64_t count_maximum = std::numeric_limits<std::int64_t>::max();

const NumericCondition& numeric(const Predicate& predicate) { return std::get<NumericCondition>(predicate.value); }
const std::string& text_value(const Predicate& predicate) { return std::get<std::string>(predicate.value); }

/// Channel mask (bit c-1 for display channel c) selected by the condition.
std::int64_t channel_mask(const NumericCondition& condition) {
    const auto interval = integer_interval(condition, 1, 16);
    std::int64_t mask = 0;
    for (auto channel = interval.low; channel <= interval.high; ++channel)
        mask |= std::int64_t{1} << (channel - 1);
    return mask;
}

void json_member_equals(SqlBuilder& builder, std::string_view path, SqlValue value) {
    builder.append(std::format("EXISTS (SELECT 1 FROM json_each(s.diagnostics_json, '{}') AS j WHERE j.value = ", path));
    builder.parameter(std::move(value));
    builder.append(")");
}

/// Condition on `sequence_summary AS s` for one sequence-level predicate.
void sequence_predicate(SqlBuilder& builder, const Predicate& predicate) {
    switch (predicate.field) {
    case Field::bpm:
        real_condition(builder, "s.tempo_representative", numeric(predicate));
        return;
    case Field::tempo_any: {
        const auto interval = real_interval(numeric(predicate));
        builder.append("(s.tempo_min IS NOT NULL AND EXISTS (SELECT 1 FROM json_each(s.diagnostics_json, '$.tempos') "
                       "AS j WHERE 1");
        if (interval.has_low) {
            builder.append(interval.low_open ? " AND j.value > " : " AND j.value >= ");
            builder.parameter(interval.low);
        }
        if (interval.has_high) {
            builder.append(interval.high_open ? " AND j.value < " : " AND j.value <= ");
            builder.parameter(interval.high);
        }
        builder.append("))");
        return;
    }
    case Field::tempo_origin:
        builder.append("s.tempo_origin = ");
        builder.parameter(text_value(predicate));
        return;
    case Field::key: {
        const auto& key = std::get<KeyValue>(predicate.value);
        builder.append("(s.key_root = ");
        builder.parameter(std::int64_t{key.root});
        if (!key.mode.empty()) {
            builder.append(" AND s.key_mode = ");
            builder.parameter(key.mode);
        }
        builder.append(" AND s.key_origin <> 'unknown' AND (s.key_origin <> 'estimated' OR (s.key_confidence IS NOT "
                       "NULL AND s.key_confidence >= ");
        builder.parameter(estimated_key_min_confidence);
        builder.append(")))");
        return;
    }
    case Field::key_origin:
        builder.append("s.key_origin = ");
        builder.parameter(text_value(predicate));
        return;
    case Field::confidence:
        real_condition(builder, "s.key_confidence", numeric(predicate));
        return;
    case Field::length_qn:
        real_condition(builder, "s.quarter_length", numeric(predicate));
        return;
    case Field::duration:
        real_condition(builder, "s.duration_seconds", numeric(predicate));
        return;
    case Field::meter: {
        const auto& meter = std::get<MeterValue>(predicate.value);
        json_member_equals(builder, "$.meter_names", std::format("{}/{}", meter.numerator, meter.denominator));
        return;
    }
    case Field::tracks:
        integer_condition(builder, "s.track_count", integer_interval(numeric(predicate), 0, count_maximum));
        return;
    case Field::notes:
        integer_condition(builder, "s.note_count", integer_interval(numeric(predicate), 0, count_maximum));
        return;
    case Field::pitch_min:
        integer_condition(builder, "s.min_pitch", integer_interval(numeric(predicate), 0, 127));
        return;
    case Field::pitch_max:
        integer_condition(builder, "s.max_pitch", integer_interval(numeric(predicate), 0, 127));
        return;
    case Field::channel: {
        const auto mask = channel_mask(numeric(predicate));
        if (mask == 0) {
            builder.append("0");
            return;
        }
        builder.append("EXISTS (SELECT 1 FROM track_summary AS t WHERE t.revision_id = s.revision_id AND "
                       "t.sequence_no = s.sequence_no AND (t.channel_mask & ");
        builder.parameter(mask);
        builder.append(") <> 0)");
        return;
    }
    case Field::program: {
        const auto interval = integer_interval(numeric(predicate), 1, 128);
        if (interval.empty()) {
            builder.append("0");
            return;
        }
        builder.append("EXISTS (SELECT 1 FROM track_summary AS t, json_each(t.program_json) AS j WHERE "
                       "t.revision_id = s.revision_id AND t.sequence_no = s.sequence_no AND json_extract(j.value, '$.program') "
                       "BETWEEN ");
        builder.parameter(interval.low - 1);
        builder.append(" AND ");
        builder.parameter(interval.high - 1);
        builder.append(")");
        return;
    }
    case Field::drums:
        builder.append("s.has_drums = ");
        builder.parameter(std::int64_t{std::get<bool>(predicate.value) ? 1 : 0});
        return;
    case Field::has:
        json_member_equals(builder, "$.event_classes", text_value(predicate));
        return;
    case Field::any_text:
    case Field::tag:
    case Field::title:
    case Field::text:
    case Field::role:
    case Field::favorite:
    case Field::rating:
    case Field::source:
    case Field::collection:
    case Field::status:
        break;
    }
    builder.append("0");
}

void sequence_expression(SqlBuilder& builder, const QueryNode& node) {
    switch (node.kind) {
    case NodeKind::predicate:
        sequence_predicate(builder, node.predicate);
        return;
    case NodeKind::not_node:
        builder.append("NOT (");
        sequence_expression(builder, node.children.front());
        builder.append(")");
        return;
    case NodeKind::and_node:
    case NodeKind::or_node: {
        const auto* joiner = node.kind == NodeKind::and_node ? " AND " : " OR ";
        builder.append("(");
        for (std::size_t index = 0; index < node.children.size(); ++index) {
            if (index > 0)
                builder.append(joiner);
            sequence_expression(builder, node.children[index]);
        }
        builder.append(")");
        return;
    }
    }
}

/// EXISTS over the current head's sequences satisfying all `nodes` in the same sequence.
void sequence_exists(SqlBuilder& builder, const std::vector<const QueryNode*>& nodes) {
    builder.append("EXISTS (SELECT 1 FROM sequence_summary AS s WHERE s.revision_id = a.current_revision_id");
    for (const auto* node : nodes) {
        builder.append(" AND ");
        sequence_expression(builder, *node);
    }
    builder.append(")");
}

void status_condition(SqlBuilder& builder, const std::string& status) {
    if (status == "source_missing") {
        builder.append("EXISTS (SELECT 1 FROM source_location AS sl WHERE sl.asset_id = a.id AND sl.state IN "
                       "('missing', 'offline'))");
        return;
    }
    if (status == "indexing") {
        builder.append("NOT EXISTS (SELECT 1 FROM analysis_run AS r WHERE r.revision_id = a.current_revision_id AND "
                       "r.analyzer = 'basic' AND r.status IN ('complete', 'failed'))");
        return;
    }
    if (status == "error") {
        builder.append("EXISTS (SELECT 1 FROM analysis_run AS r WHERE r.revision_id = a.current_revision_id AND "
                       "r.analyzer = 'basic' AND r.status = 'failed')");
        return;
    }
    builder.append("EXISTS (SELECT 1 FROM analysis_run AS r WHERE r.revision_id = a.current_revision_id AND "
                   "r.analyzer = 'basic' AND r.status = 'complete' AND json_extract(r.result_json, '$.state') = ");
    builder.parameter(status);
    builder.append(")");
}

/// Condition on `asset AS a` for one asset-level predicate.
void asset_predicate(SqlBuilder& builder, const Predicate& predicate, const SearchOptions& options) {
    switch (predicate.field) {
    case Field::any_text:
    case Field::text:
    case Field::title:
    case Field::source:
        builder.append("a.id IN (SELECT rowid FROM asset_fts WHERE asset_fts MATCH ");
        builder.parameter(fts_expression(predicate));
        builder.append(")");
        return;
    case Field::tag:
        builder.append("EXISTS (SELECT 1 FROM asset_tag AS at JOIN tag AS tg ON tg.id = at.tag_id WHERE at.asset_id = "
                       "a.id AND tg.normalized_name = ");
        builder.parameter(text_value(predicate));
        builder.append(")");
        return;
    case Field::role:
        builder.append("a.role = ");
        builder.parameter(text_value(predicate));
        return;
    case Field::favorite:
        builder.append("a.favorite = ");
        builder.parameter(std::int64_t{std::get<bool>(predicate.value) ? 1 : 0});
        return;
    case Field::rating:
        integer_condition(builder, "a.rating", integer_interval(numeric(predicate), 0, 5));
        return;
    case Field::collection: {
        const auto found = options.collections.find(text_value(predicate));
        if (found == options.collections.end() || found->second.empty()) {
            builder.append("0");
            return;
        }
        builder.append("EXISTS (SELECT 1 FROM collection_asset AS ca WHERE ca.asset_id = a.id AND ca.collection_id IN (");
        for (std::size_t index = 0; index < found->second.size(); ++index) {
            if (index > 0)
                builder.append(", ");
            builder.parameter(found->second[index]);
        }
        builder.append("))");
        return;
    }
    case Field::status:
        status_condition(builder, text_value(predicate));
        return;
    case Field::bpm:
    case Field::tempo_any:
    case Field::tempo_origin:
    case Field::key:
    case Field::key_origin:
    case Field::confidence:
    case Field::length_qn:
    case Field::duration:
    case Field::meter:
    case Field::tracks:
    case Field::notes:
    case Field::pitch_min:
    case Field::pitch_max:
    case Field::channel:
    case Field::program:
    case Field::drums:
    case Field::has:
        sequence_exists(builder, {});
        break;
    }
}

void asset_expression(SqlBuilder& builder, const QueryNode& node, const SearchOptions& options) {
    if (node.kind == NodeKind::and_node && node.children.empty()) {
        builder.append("1");
        return;
    }
    // Same-sequence mode evaluates a whole musical subtree inside one sequence; the explicit
    // "across any sequences" mode tests each musical predicate against any sequence on its own.
    if (is_pure_sequence(node) && (options.same_sequence || node.kind == NodeKind::predicate)) {
        sequence_exists(builder, {&node});
        return;
    }
    switch (node.kind) {
    case NodeKind::predicate:
        asset_predicate(builder, node.predicate, options);
        return;
    case NodeKind::not_node:
        builder.append("NOT (");
        asset_expression(builder, node.children.front(), options);
        builder.append(")");
        return;
    case NodeKind::or_node:
        builder.append("(");
        for (std::size_t index = 0; index < node.children.size(); ++index) {
            if (index > 0)
                builder.append(" OR ");
            asset_expression(builder, node.children[index], options);
        }
        builder.append(")");
        return;
    case NodeKind::and_node: {
        std::vector<const QueryNode*> grouped;
        bool first = true;
        builder.append("(");
        for (const auto& child : node.children) {
            if (options.same_sequence && is_pure_sequence(child)) {
                grouped.push_back(&child);
                continue;
            }
            if (!first)
                builder.append(" AND ");
            first = false;
            asset_expression(builder, child, options);
        }
        if (!grouped.empty()) {
            if (!first)
                builder.append(" AND ");
            sequence_exists(builder, grouped);
        }
        builder.append(")");
        return;
    }
    }
}

/// Positive full-text predicates directly under the root AND (relevance drivers).
std::vector<const Predicate*> relevance_terms(const Query& query) {
    std::vector<const Predicate*> terms;
    const auto& root = query.root;
    if (root.kind == NodeKind::predicate && is_text_field(root.predicate.field)) {
        terms.push_back(&root.predicate);
    } else if (root.kind == NodeKind::and_node) {
        for (const auto& child : root.children)
            if (child.kind == NodeKind::predicate && is_text_field(child.predicate.field))
                terms.push_back(&child.predicate);
    }
    return terms;
}

struct SortColumns {
    std::string_view column;
    std::string_view index;
};

SortColumns sort_columns(SortField field) {
    switch (field) {
    case SortField::modified:
        return {"a.modified_ms", "asset_modified"};
    case SortField::created:
        return {"a.created_ms", "asset_created"};
    case SortField::title:
    case SortField::relevance:
        break;
    }
    return {"a.sort_title", "asset_browse"};
}

} // namespace

std::string fts_string_literal(std::string_view folded_text) {
    std::string literal = "\"";
    for (const char character : folded_text) {
        if (character == '"')
            literal += "\"\"";
        else
            literal.push_back(character);
    }
    literal += "\"";
    return literal;
}

std::string fts_expression(const Predicate& predicate) {
    const auto& text = std::get<std::string>(predicate.value);
    switch (predicate.field) {
    case Field::title:
        return "title : " + fts_string_literal(text);
    case Field::source:
        return "source_names : " + fts_string_literal(text);
    case Field::any_text:
    case Field::text:
    case Field::tag:
    case Field::bpm:
    case Field::tempo_any:
    case Field::tempo_origin:
    case Field::key:
    case Field::key_origin:
    case Field::confidence:
    case Field::length_qn:
    case Field::duration:
    case Field::meter:
    case Field::tracks:
    case Field::notes:
    case Field::pitch_min:
    case Field::pitch_max:
    case Field::channel:
    case Field::program:
    case Field::role:
    case Field::drums:
    case Field::favorite:
    case Field::rating:
    case Field::has:
    case Field::collection:
    case Field::status:
        break;
    }
    return fts_string_literal(text);
}

bool supports_relevance(const Query& query) { return !relevance_terms(query).empty(); }

Result<SqlFragment> compile_condition(const Query& query, const SearchOptions& options) {
    SqlBuilder builder;
    asset_expression(builder, query.root, options);
    return std::move(builder).finish();
}

Result<CompiledSearch> compile_page_query(const Query& query, const SearchOptions& options, const SortOrder& order,
                                          const std::optional<PageCursor>& after, std::size_t limit) {
    if (limit == 0)
        return make_error(ErrorCode::invalid_argument, "page size must be at least 1");
    SqlBuilder builder;
    CompiledSearch compiled;
    if (order.field == SortField::relevance) {
        const auto terms = relevance_terms(query);
        if (terms.empty())
            return make_error(ErrorCode::invalid_argument, "relevance order needs a positive text term");
        std::string driver;
        for (const auto* term : terms) {
            if (!driver.empty())
                driver += " AND ";
            driver += "(" + fts_expression(*term) + ")";
        }
        builder.append("SELECT a.id, a.sort_title, a.modified_ms, a.created_ms, asset_fts.rank FROM asset_fts JOIN "
                       "asset AS a ON a.id = asset_fts.rowid WHERE asset_fts MATCH ");
        builder.parameter(driver);
        builder.append(std::format(" AND asset_fts.rank MATCH '{}' AND a.deleted_ms IS NULL", relevance_rank_function));
        if (options.collection_scope) {
            builder.append(" AND a.id IN (SELECT asset_id FROM collection_asset WHERE collection_id = ");
            builder.parameter(*options.collection_scope);
            builder.append(")");
        }
        builder.append(" AND ");
        asset_expression(builder, query.root, options);
        compiled.row_limit = relevance_materialize_limit;
        builder.append(" ORDER BY asset_fts.rank LIMIT ");
        builder.parameter(static_cast<std::int64_t>(compiled.row_limit));
        compiled.relevance = true;
        compiled.statement = std::move(builder).finish();
        return compiled;
    }
    const auto columns = sort_columns(order.field);
    builder.append(std::format("SELECT a.id, a.sort_title, a.modified_ms, a.created_ms, NULL FROM asset AS a INDEXED BY "
                               "{} WHERE a.deleted_ms IS NULL",
                               columns.index));
    if (after) {
        const bool wants_text = order.field == SortField::title;
        if (wants_text != std::holds_alternative<std::string>(after->sort_value) ||
            (!wants_text && !std::holds_alternative<std::int64_t>(after->sort_value)))
            return make_error(ErrorCode::invalid_argument, "the page cursor does not match the sort order");
        builder.append(std::format(" AND ({}, a.id) {} (", columns.column, order.descending ? "<" : ">"));
        builder.parameter(after->sort_value);
        builder.append(", ");
        builder.parameter(after->id);
        builder.append(")");
    }
    if (options.collection_scope) {
        builder.append(" AND a.id IN (SELECT asset_id FROM collection_asset WHERE collection_id = ");
        builder.parameter(*options.collection_scope);
        builder.append(")");
    }
    builder.append(" AND ");
    asset_expression(builder, query.root, options);
    const auto* direction = order.descending ? " DESC" : "";
    builder.append(std::format(" ORDER BY {}{}, a.id{} LIMIT ", columns.column, direction, direction));
    compiled.row_limit = std::min(limit, maximum_page_size);
    builder.parameter(static_cast<std::int64_t>(compiled.row_limit));
    compiled.statement = std::move(builder).finish();
    return compiled;
}

} // namespace mwb::search
