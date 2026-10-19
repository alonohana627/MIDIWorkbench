// In-memory browse index (specification section 19.4) evaluating the query AST with exactly the
// semantics of the SQL compiler in sql_compiler.cpp.

#include "mwb/search/browse_index.hpp"

#include <algorithm>
#include <limits>

namespace mwb::search {

namespace {

constexpr std::int64_t count_maximum = std::numeric_limits<std::int64_t>::max();

Truth truth(bool value) { return value ? Truth::yes : Truth::no; }

Truth negate(Truth value) {
    switch (value) {
    case Truth::yes:
        return Truth::no;
    case Truth::no:
        return Truth::yes;
    case Truth::unknown:
        break;
    }
    return Truth::unknown;
}

bool is_pure_sequence(const QueryNode& node) {
    if (node.kind == NodeKind::predicate)
        return is_sequence_field(node.predicate.field);
    if (node.children.empty())
        return false;
    return std::ranges::all_of(node.children, [](const QueryNode& child) { return is_pure_sequence(child); });
}

bool contains_integer(const IntegerInterval& interval, std::int64_t value) {
    return !interval.empty() && value >= interval.low && value <= interval.high;
}

bool optional_real(const std::optional<double>& value, const NumericCondition& condition) {
    return value.has_value() && real_interval(condition).contains(*value);
}

template <class Container, class Value>
bool contains(const Container& container, const Value& value) {
    return std::ranges::find(container, value) != container.end();
}

/// Evaluates a pure-sequence subtree on one sequence (two-valued, like the SQL).
bool sequence_node(const SequenceFacts& sequence, const QueryNode& node) {
    switch (node.kind) {
    case NodeKind::predicate:
        return sequence_matches(sequence, node.predicate);
    case NodeKind::not_node:
        return !sequence_node(sequence, node.children.front());
    case NodeKind::and_node:
        return std::ranges::all_of(node.children, [&](const QueryNode& child) { return sequence_node(sequence, child); });
    case NodeKind::or_node:
        return std::ranges::any_of(node.children, [&](const QueryNode& child) { return sequence_node(sequence, child); });
    }
    return false;
}

bool any_sequence(const BrowseRow& row, const std::vector<const QueryNode*>& nodes) {
    return std::ranges::any_of(row.sequences, [&](const SequenceFacts& sequence) {
        return std::ranges::all_of(nodes, [&](const QueryNode* node) { return sequence_node(sequence, *node); });
    });
}

const std::string& text_value(const Predicate& predicate) { return std::get<std::string>(predicate.value); }
const NumericCondition& numeric(const Predicate& predicate) { return std::get<NumericCondition>(predicate.value); }

bool asset_predicate(const BrowseRow& row, const Predicate& predicate, const SearchOptions& options) {
    switch (predicate.field) {
    case Field::tag:
        return contains(row.tags, text_value(predicate));
    case Field::role:
        return row.role == text_value(predicate);
    case Field::favorite:
        return row.favorite == std::get<bool>(predicate.value);
    case Field::rating:
        return contains_integer(integer_interval(numeric(predicate), 0, 5), row.rating);
    case Field::collection: {
        const auto found = options.collections.find(text_value(predicate));
        if (found == options.collections.end())
            return false;
        return std::ranges::any_of(found->second, [&](std::int64_t id) { return contains(row.collections, id); });
    }
    case Field::status: {
        const auto& status = text_value(predicate);
        if (status == "source_missing")
            return row.source_missing;
        if (status == "indexing")
            return row.analysis == AnalysisState::indexing;
        if (status == "error")
            return row.analysis == AnalysisState::error;
        if (status == "ready")
            return row.analysis == AnalysisState::ready;
        return row.analysis == AnalysisState::warning;
    }
    case Field::any_text:
    case Field::title:
    case Field::text:
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
    case Field::source:
        break;
    }
    return false;
}

/// Heap bytes of a string: none while it fits the inline (small-string) buffer, which sizeof of
/// the owning struct already counts.
std::size_t string_bytes(const std::string& text) {
    static const std::size_t inline_capacity = std::string().capacity();
    return text.capacity() > inline_capacity ? text.capacity() + 1 : 0;
}

} // namespace

bool sequence_matches(const SequenceFacts& sequence, const Predicate& predicate) {
    switch (predicate.field) {
    case Field::bpm:
        return optional_real(sequence.tempo_representative, numeric(predicate));
    case Field::tempo_any: {
        if (!sequence.tempo_min)
            return false;
        const auto interval = real_interval(numeric(predicate));
        return std::ranges::any_of(sequence.tempos, [&](double tempo) { return interval.contains(tempo); });
    }
    case Field::tempo_origin:
        return sequence.tempo_origin == text_value(predicate);
    case Field::key: {
        const auto& key = std::get<KeyValue>(predicate.value);
        if (!sequence.key_root || *sequence.key_root != key.root)
            return false;
        if (!key.mode.empty() && (!sequence.key_mode || *sequence.key_mode != key.mode))
            return false;
        if (sequence.key_origin == "unknown")
            return false;
        return sequence.key_origin != "estimated" ||
               (sequence.key_confidence && *sequence.key_confidence >= estimated_key_min_confidence);
    }
    case Field::key_origin:
        return sequence.key_origin == text_value(predicate);
    case Field::confidence:
        return optional_real(sequence.key_confidence, numeric(predicate));
    case Field::length_qn:
        return optional_real(sequence.quarter_length, numeric(predicate));
    case Field::duration:
        return real_interval(numeric(predicate)).contains(sequence.duration_seconds);
    case Field::meter: {
        const auto& meter = std::get<MeterValue>(predicate.value);
        return contains(sequence.meters, std::to_string(meter.numerator) + "/" + std::to_string(meter.denominator));
    }
    case Field::tracks:
        return contains_integer(integer_interval(numeric(predicate), 0, count_maximum), sequence.track_count);
    case Field::notes:
        return contains_integer(integer_interval(numeric(predicate), 0, count_maximum), sequence.note_count);
    case Field::pitch_min:
        return sequence.min_pitch && contains_integer(integer_interval(numeric(predicate), 0, 127), *sequence.min_pitch);
    case Field::pitch_max:
        return sequence.max_pitch && contains_integer(integer_interval(numeric(predicate), 0, 127), *sequence.max_pitch);
    case Field::channel: {
        const auto interval = integer_interval(numeric(predicate), 1, 16);
        std::uint32_t mask = 0;
        for (auto channel = interval.low; channel <= interval.high; ++channel)
            mask |= 1u << (channel - 1);
        return std::ranges::any_of(sequence.tracks, [&](const TrackFacts& track) {
            return (static_cast<std::uint32_t>(track.channel_mask) & mask) != 0;
        });
    }
    case Field::program: {
        const auto interval = integer_interval(numeric(predicate), 1, 128);
        if (interval.empty())
            return false;
        return std::ranges::any_of(sequence.tracks, [&](const TrackFacts& track) {
            return std::ranges::any_of(track.programs, [&](std::uint8_t program) {
                return program >= interval.low - 1 && program <= interval.high - 1;
            });
        });
    }
    case Field::drums:
        return sequence.has_drums == std::get<bool>(predicate.value);
    case Field::has:
        return contains(sequence.event_classes, text_value(predicate));
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
    return false;
}

std::uint32_t BrowseIndex::intern(const std::string& token) {
    const auto found = dictionary_.find(token);
    if (found != dictionary_.end())
        return found->second;
    const auto id = static_cast<std::uint32_t>(dictionary_.size());
    dictionary_.emplace(token, id);
    dictionary_bytes_ += string_bytes(token) + sizeof(std::uint32_t) + 2 * sizeof(void*);
    return id;
}

std::size_t BrowseIndex::measure(const StoredRow& stored) const {
    const auto& row = stored.row;
    std::size_t bytes = sizeof(StoredRow) + 2 * sizeof(void*) + string_bytes(row.sort_title) + string_bytes(row.role);
    for (const auto& tag : row.tags)
        bytes += sizeof(std::string) + string_bytes(tag);
    bytes += row.collections.capacity() * sizeof(std::int64_t);
    for (const auto& tokens : stored.tokens)
        bytes += tokens.capacity() * sizeof(std::uint32_t);
    for (const auto& text : row.text)
        bytes += string_bytes(text);
    for (const auto& sequence : row.sequences) {
        bytes += sizeof(SequenceFacts) + sequence.tempos.capacity() * sizeof(double) +
                 string_bytes(sequence.tempo_origin) + string_bytes(sequence.key_origin);
        for (const auto& meter : sequence.meters)
            bytes += sizeof(std::string) + string_bytes(meter);
        for (const auto& event_class : sequence.event_classes)
            bytes += sizeof(std::string) + string_bytes(event_class);
        for (const auto& track : sequence.tracks)
            bytes += sizeof(TrackFacts) + track.programs.capacity();
    }
    return bytes;
}

void BrowseIndex::upsert(const BrowseRow& row) {
    remove(row.id);
    StoredRow stored;
    stored.row = row;
    for (std::size_t column = 0; column < fts_column_count; ++column) {
        const auto tokens = ascii_fts_tokens(row.text[column]);
        if (!tokens) {
            stored.text_exact = false;
            continue;
        }
        stored.tokens[column].reserve(tokens->size());
        for (const auto& token : *tokens)
            stored.tokens[column].push_back(intern(token));
    }
    // Column text is only needed for inexact (non-ASCII) columns; exact columns live as tokens.
    for (std::size_t column = 0; column < fts_column_count; ++column)
        if (ascii_fts_tokens(stored.row.text[column]))
            stored.row.text[column].clear();
    stored.bytes = measure(stored);
    memory_bytes_ += stored.bytes;
    rows_.insert_or_assign(row.id, std::move(stored));
}

void BrowseIndex::remove(std::int64_t id) {
    const auto found = rows_.find(id);
    if (found == rows_.end())
        return;
    memory_bytes_ -= found->second.bytes;
    rows_.erase(found);
}

void BrowseIndex::clear() {
    rows_.clear();
    dictionary_.clear();
    dictionary_bytes_ = 0;
    memory_bytes_ = 0;
}

Truth BrowseIndex::evaluate_text(const StoredRow& stored, const Predicate& predicate, TokenCache& cache) const {
    auto cached = cache.find(&predicate);
    if (cached == cache.end()) {
        std::optional<ResolvedText> resolved;
        if (const auto query_tokens = ascii_fts_tokens(text_value(predicate))) {
            resolved.emplace();
            for (const auto& token : *query_tokens) {
                const auto found = dictionary_.find(token);
                resolved->ids.push_back(found == dictionary_.end() ? std::numeric_limits<std::uint32_t>::max()
                                                                   : found->second);
            }
            if (predicate.prefix && !query_tokens->empty()) {
                // Every known token starting with the typed one, like FTS5's "lea" *.
                const auto& stem = query_tokens->back();
                resolved->last_prefix.assign(dictionary_.size(), false);
                for (const auto& [token, id] : dictionary_)
                    if (token.starts_with(stem) && id < resolved->last_prefix.size())
                        resolved->last_prefix[id] = true;
            }
        }
        cached = cache.emplace(&predicate, std::move(resolved)).first;
    }
    if (!cached->second)
        return Truth::unknown;
    const auto& ids = cached->second->ids;
    const auto& last_prefix = cached->second->last_prefix;
    const bool prefix = predicate.prefix && !ids.empty();
    const auto matches_at = [&](const std::vector<std::uint32_t>& tokens, std::size_t start) {
        const std::size_t exact = prefix ? ids.size() - 1 : ids.size();
        if (!std::equal(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(exact),
                        tokens.begin() + static_cast<std::ptrdiff_t>(start)))
            return false;
        if (!prefix)
            return true;
        const auto last = tokens[start + exact];
        return last < last_prefix.size() && last_prefix[last];
    };
    std::vector<std::size_t> columns;
    switch (predicate.field) {
    case Field::title:
        columns = {0};
        break;
    case Field::source:
        columns = {1};
        break;
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
        columns = {0, 1, 2, 3, 4};
        break;
    }
    bool undecided = false;
    for (const auto column : columns) {
        if (!stored.row.text[column].empty()) {
            undecided = true; // Non-ASCII column: only SQLite's tokenizer decides.
            continue;
        }
        const auto& tokens = stored.tokens[column];
        if (ids.empty() || tokens.size() < ids.size())
            continue;
        for (std::size_t start = 0; start + ids.size() <= tokens.size(); ++start)
            if (matches_at(tokens, start))
                return Truth::yes;
    }
    return undecided ? Truth::unknown : Truth::no;
}

Truth BrowseIndex::evaluate_node(const StoredRow& stored, const QueryNode& node, const SearchOptions& options,
                                 TokenCache& cache) const {
    const auto& row = stored.row;
    if (node.kind == NodeKind::and_node && node.children.empty())
        return Truth::yes;
    if (is_pure_sequence(node) && (options.same_sequence || node.kind == NodeKind::predicate))
        return truth(any_sequence(row, {&node}));
    switch (node.kind) {
    case NodeKind::predicate:
        if (is_text_field(node.predicate.field))
            return evaluate_text(stored, node.predicate, cache);
        return truth(asset_predicate(row, node.predicate, options));
    case NodeKind::not_node:
        return negate(evaluate_node(stored, node.children.front(), options, cache));
    case NodeKind::or_node: {
        Truth result = Truth::no;
        for (const auto& child : node.children) {
            const auto value = evaluate_node(stored, child, options, cache);
            if (value == Truth::yes)
                return Truth::yes;
            if (value == Truth::unknown)
                result = Truth::unknown;
        }
        return result;
    }
    case NodeKind::and_node: {
        Truth result = Truth::yes;
        std::vector<const QueryNode*> grouped;
        for (const auto& child : node.children) {
            if (options.same_sequence && is_pure_sequence(child)) {
                grouped.push_back(&child);
                continue;
            }
            const auto value = evaluate_node(stored, child, options, cache);
            if (value == Truth::no)
                return Truth::no;
            if (value == Truth::unknown)
                result = Truth::unknown;
        }
        if (!grouped.empty() && !any_sequence(row, grouped))
            return Truth::no;
        return result;
    }
    }
    return Truth::no;
}

Truth BrowseIndex::evaluate_row(const StoredRow& stored, const Query& query, const SearchOptions& options,
                                TokenCache& cache) const {
    if (stored.row.deleted)
        return Truth::no;
    if (options.collection_scope && !contains(stored.row.collections, *options.collection_scope))
        return Truth::no;
    return evaluate_node(stored, query.root, options, cache);
}

Truth BrowseIndex::evaluate(std::int64_t id, const Query& query, const SearchOptions& options) const {
    const auto found = rows_.find(id);
    if (found == rows_.end())
        return Truth::no;
    TokenCache cache;
    return evaluate_row(found->second, query, options, cache);
}

Result<std::vector<std::int64_t>> BrowseIndex::matching_ids(const Query& query, const SearchOptions& options) const {
    std::vector<std::int64_t> ids;
    TokenCache cache;
    for (const auto& [id, stored] : rows_) {
        const auto value = evaluate_row(stored, query, options, cache);
        if (value == Truth::unknown)
            return make_error(ErrorCode::unsupported, "the browse index cannot evaluate this text query exactly");
        if (value == Truth::yes)
            ids.push_back(id);
    }
    std::ranges::sort(ids);
    return ids;
}

Result<BrowsePage> BrowseIndex::page(const Query& query, const SearchOptions& options, const SortOrder& order,
                                     const std::optional<PageCursor>& after, std::size_t limit) const {
    if (order.field == SortField::relevance)
        return make_error(ErrorCode::unsupported, "relevance order is served by SQL");
    if (limit == 0)
        return make_error(ErrorCode::invalid_argument, "page size must be at least 1");
    limit = std::min(limit, maximum_page_size);
    // Keys are compared in place (no per-row string copies) with the same order as the SQL page
    // query: (key, id) ascending or both descending; SqlValue's variant order covers a cursor of
    // another type exactly as before.
    const bool numeric = order.field == SortField::modified || order.field == SortField::created;
    const auto number_of = [&](const StoredRow& stored) {
        return order.field == SortField::modified ? stored.row.modified_ms : stored.row.created_ms;
    };
    const auto key_of = [&](const StoredRow& stored) -> SqlValue {
        if (numeric)
            return number_of(stored);
        return stored.row.sort_title;
    };
    const auto ascending = [&](const StoredRow& left, std::int64_t left_id, const StoredRow& right, std::int64_t right_id) {
        if (numeric) {
            if (number_of(left) != number_of(right))
                return number_of(left) < number_of(right);
        } else if (const int compared = left.row.sort_title.compare(right.row.sort_title); compared != 0) {
            return compared < 0;
        }
        return left_id < right_id;
    };
    // Position relative to the cursor: true when the row comes after it in page order.
    const auto past_cursor = [&](const StoredRow& stored, std::int64_t id) {
        const auto& cursor = after->sort_value;
        bool key_less = false;    // row key < cursor key
        bool key_greater = false; // row key > cursor key
        if (numeric && std::holds_alternative<std::int64_t>(cursor)) {
            key_less = number_of(stored) < std::get<std::int64_t>(cursor);
            key_greater = number_of(stored) > std::get<std::int64_t>(cursor);
        } else if (!numeric && std::holds_alternative<std::string>(cursor)) {
            const int compared = std::string_view(stored.row.sort_title).compare(std::get<std::string>(cursor));
            key_less = compared < 0;
            key_greater = compared > 0;
        } else {
            const auto key = key_of(stored);
            key_less = key < cursor;
            key_greater = cursor < key;
        }
        if (order.descending)
            return key_less || (!key_greater && id < after->id);
        return key_greater || (!key_less && id > after->id);
    };
    struct Candidate {
        const StoredRow* row;
        std::int64_t id;
    };
    std::vector<Candidate> candidates;
    TokenCache cache;
    for (const auto& [id, stored] : rows_) {
        const auto value = evaluate_row(stored, query, options, cache);
        if (value == Truth::unknown)
            return make_error(ErrorCode::unsupported, "the browse index cannot evaluate this text query exactly");
        if (value != Truth::yes || (after && !past_cursor(stored, id)))
            continue;
        candidates.push_back(Candidate{&stored, id});
    }
    const auto before = [&](const Candidate& left, const Candidate& right) {
        return order.descending ? ascending(*right.row, right.id, *left.row, left.id)
                                : ascending(*left.row, left.id, *right.row, right.id);
    };
    // Only the page is ordered: O(N log limit) instead of sorting every match for every page.
    const auto taken = std::min(limit, candidates.size());
    std::ranges::partial_sort(candidates, candidates.begin() + static_cast<std::ptrdiff_t>(taken), before);
    BrowsePage page;
    for (std::size_t index = 0; index < taken; ++index)
        page.ids.push_back(candidates[index].id);
    if (taken == limit && taken > 0)
        page.next = PageCursor{key_of(*candidates[taken - 1].row), candidates[taken - 1].id};
    return page;
}

} // namespace mwb::search
