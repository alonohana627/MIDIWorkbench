// Exact (section 26.4) and approximate (section 26.5) motif matching over a MotifIndexReader.

#include "mwb/search/motif.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numeric>
#include <set>
#include <unordered_map>

namespace mwb::search {

namespace {

TokenFamily family_for(MatchMode mode) {
    switch (mode) {
    case MatchMode::absolute:
        return TokenFamily::pitch;
    case MatchMode::transposed:
        return TokenFamily::interval;
    case MatchMode::contour:
        return TokenFamily::contour;
    case MatchMode::rhythm:
        return TokenFamily::rhythm;
    }
    return TokenFamily::interval;
}

int sign(int value) { return value > 0 ? 1 : value < 0 ? -1 : 0; }

/// Exact proportional-rhythm check: candidate IOIs equal query IOIs times one constant scale,
/// within `tolerance` candidate ticks, compared with exact 128-bit integer arithmetic.
bool rhythm_matches(std::span<const QueryNote> query, std::span<const MotifNote> candidate, Tick tolerance,
                    std::int64_t& scale_numerator, std::int64_t& scale_denominator) {
    scale_numerator = 1;
    scale_denominator = 1;
    if (query.size() < 2)
        return true;
    const auto query_first = query[1].onset - query[0].onset;
    const auto candidate_first = candidate[1].onset - candidate[0].onset;
    if (query_first <= 0 || candidate_first <= 0)
        return false;
    for (std::size_t index = 1; index + 1 < query.size(); ++index) {
        const auto query_ioi = query[index + 1].onset - query[index].onset;
        const auto candidate_ioi = candidate[index + 1].onset - candidate[index].onset;
        if (query_ioi <= 0 || candidate_ioi <= 0)
            return false;
        const __int128 left = static_cast<__int128>(candidate_ioi) * query_first;
        const __int128 right = static_cast<__int128>(query_ioi) * candidate_first;
        const __int128 difference = left > right ? left - right : right - left;
        if (difference > static_cast<__int128>(tolerance) * query_first)
            return false;
    }
    const auto divisor = std::gcd(candidate_first, query_first);
    scale_numerator = candidate_first / divisor;
    scale_denominator = query_first / divisor;
    return true;
}

/// Bounded cache of decoded streams for one search.
class StreamCache {
public:
    explicit StreamCache(MotifIndexReader& index) : index_(index) {}

    Result<const StoredMotifStream*> get(std::int64_t id) {
        if (const auto found = streams_.find(id); found != streams_.end())
            return found->second ? &*found->second : nullptr;
        if (streams_.size() > 4096)
            streams_.clear();
        auto loaded = index_.load_stream(id);
        if (!loaded)
            return std::unexpected(std::move(loaded).error());
        auto [iterator, inserted] = streams_.emplace(id, std::move(*loaded));
        std::ignore = inserted; // A fresh key always inserts after the find above.
        return iterator->second ? &*iterator->second : nullptr;
    }

private:
    MotifIndexReader& index_;
    std::unordered_map<std::int64_t, std::optional<StoredMotifStream>> streams_;
};

bool occurrence_after(const MotifOccurrence& occurrence, const std::optional<ExactContinuation>& after) {
    if (!after)
        return true;
    if (occurrence.stream.stream_id != after->stream_id)
        return occurrence.stream.stream_id > after->stream_id;
    return occurrence.first_note > after->first_note;
}

} // namespace

std::optional<MotifOccurrence> verify_exact(const ExactQuery& query, const StoredMotifStream& stored,
                                            std::uint32_t first_note) {
    const auto& notes = stored.stream.notes;
    const auto count = query.notes.size();
    if (count == 0 || first_note > notes.size() || notes.size() - first_note < count)
        return std::nullopt;
    const std::span<const MotifNote> window(notes.data() + first_note, count);
    int transpose = 0;
    switch (query.mode) {
    case MatchMode::absolute:
        for (std::size_t index = 0; index < count; ++index)
            if (window[index].pitch != query.notes[index].pitch)
                return std::nullopt;
        break;
    case MatchMode::transposed:
        // One constant transposition across the whole match (section 26.4 step 4).
        transpose = static_cast<int>(window[0].pitch) - query.notes[0].pitch;
        for (std::size_t index = 1; index < count; ++index)
            if (static_cast<int>(window[index].pitch) - query.notes[index].pitch != transpose)
                return std::nullopt;
        break;
    case MatchMode::contour:
        for (std::size_t index = 0; index + 1 < count; ++index)
            if (sign(static_cast<int>(window[index + 1].pitch) - window[index].pitch) !=
                sign(static_cast<int>(query.notes[index + 1].pitch) - query.notes[index].pitch))
                return std::nullopt;
        break;
    case MatchMode::rhythm:
        break;
    }
    MotifOccurrence occurrence;
    if (query.mode == MatchMode::rhythm || query.require_rhythm) {
        if (!rhythm_matches(query.notes, window, query.rhythm_tolerance, occurrence.tempo_scale_numerator,
                            occurrence.tempo_scale_denominator))
            return std::nullopt;
    }
    occurrence.stream = stored.info;
    occurrence.first_note = first_note;
    occurrence.end_note = first_note + static_cast<std::uint32_t>(count);
    occurrence.start_tick = window.front().onset;
    occurrence.end_tick = 0;
    for (const auto& note : window)
        occurrence.end_tick = std::max(occurrence.end_tick, note.end);
    occurrence.transpose = query.mode == MatchMode::transposed ? transpose : 0;
    for (const auto& note : window)
        occurrence.ordinals.push_back(note.ordinal);
    return occurrence;
}

Result<ExactSearchResult> exact_search(MotifIndexReader& index, const ExactQuery& query, std::stop_token stop) {
    if (query.notes.empty() || query.notes.size() > 256)
        return make_error(ErrorCode::limit_exceeded, "exact motif queries need 1 to 256 notes");
    if (query.result_limit == 0)
        return make_error(ErrorCode::invalid_argument, "the result limit must be at least 1");
    ExactSearchResult result;
    StreamCache cache(index);
    std::vector<MotifOccurrence> found;
    const auto family = family_for(query.mode);
    const auto tokens = family_tokens(query.notes, family);
    const std::size_t gram_length = tokens.size() >= 5 ? 5 : tokens.size() >= 3 ? 3 : 0;
    const auto mark_partial = [&](std::string reason) {
        result.partial = true;
        if (result.partial_reason.empty())
            result.partial_reason = std::move(reason);
    };

    if (gram_length == 0) {
        // Short queries: constrained scan over streams, bounded and labeled when cut short.
        result.indexed = false;
        std::int64_t after_id = query.after ? query.after->stream_id - 1 : 0;
        std::size_t scanned = 0;
        std::optional<std::int64_t> last_scanned;
        bool scan_limited = false;
        while (true) {
            if (stop.stop_requested()) {
                mark_partial("cancelled");
                break;
            }
            auto ids = index.stream_ids(after_id, 256);
            if (!ids)
                return std::unexpected(std::move(ids).error());
            if (ids->empty())
                break;
            for (const auto id : *ids) {
                if (scanned++ >= query.scan_stream_limit) {
                    mark_partial(std::format("scan limited to {} streams", query.scan_stream_limit));
                    scan_limited = true;
                    break;
                }
                last_scanned = id;
                auto stream = cache.get(id);
                if (!stream)
                    return std::unexpected(std::move(stream).error());
                if (*stream == nullptr)
                    continue;
                const auto note_count = (*stream)->stream.notes.size();
                for (std::uint32_t start = 0; start + query.notes.size() <= note_count; ++start) {
                    ++result.candidates_verified;
                    if (auto occurrence = verify_exact(query, **stream, start); occurrence &&
                                                                                 occurrence_after(*occurrence, query.after))
                        found.push_back(std::move(*occurrence));
                }
            }
            if (result.partial || found.size() > query.result_limit)
                break;
            after_id = ids->back();
        }
        // A scan cut short continues after everything of the last scanned stream; the result-limit
        // truncation below replaces this with an earlier position when it applies.
        if (scan_limited && last_scanned)
            result.next = ExactContinuation{*last_scanned, std::numeric_limits<std::uint32_t>::max()};
    } else {
        // Rarest grams first; candidates are (stream, implied start) pairs present in every
        // selected gram's postings. Hash matches are verified against the original values.
        struct Selected {
            GramKey key;
            std::size_t position;
            std::uint64_t count;
        };
        // Counts only rank the grams, so each is capped: selective queries no longer pay for
        // counting the postings of their common grams.
        const auto ranking_cap = std::min<std::uint64_t>(std::uint64_t{query.posting_limit} + 1, 65'536);
        std::vector<Selected> grams;
        for (std::size_t position = 0; position + gram_length <= tokens.size(); ++position) {
            if (stop.stop_requested()) {
                mark_partial("cancelled");
                break;
            }
            const GramKey key{family, static_cast<std::uint8_t>(gram_length),
                              gram_hash(family, std::span(tokens).subspan(position, gram_length))};
            auto count = index.count_postings(key, ranking_cap);
            if (!count)
                return std::unexpected(std::move(count).error());
            grams.push_back(Selected{key, position, *count});
        }
        std::ranges::stable_sort(grams, {}, &Selected::count);
        const std::size_t selected_count = result.partial ? 0 : std::min<std::size_t>(3, grams.size());
        std::vector<std::set<std::pair<std::int64_t, std::int64_t>>> filters;
        std::vector<std::pair<std::int64_t, std::int64_t>> candidates;
        for (std::size_t index_in_selection = 0; index_in_selection < selected_count; ++index_in_selection) {
            const auto& gram = grams[index_in_selection];
            // One posting past the limit tells whether the list is complete.
            auto postings = index.postings(gram.key, query.posting_limit + 1);
            if (!postings)
                return std::unexpected(std::move(postings).error());
            if (postings->size() > query.posting_limit) {
                // A truncated filter gram would reject true matches beyond the limit: filter only
                // with complete posting lists (verify_exact checks every candidate anyway).
                if (index_in_selection > 0)
                    continue;
                mark_partial(std::format("a query gram has more than {} postings", query.posting_limit));
                postings->resize(query.posting_limit);
            }
            std::set<std::pair<std::int64_t, std::int64_t>> starts;
            for (const auto& posting : *postings) {
                const auto start = static_cast<std::int64_t>(posting.start_ordinal) - static_cast<std::int64_t>(gram.position);
                if (start >= 0)
                    starts.emplace(posting.stream_id, start);
            }
            if (index_in_selection == 0)
                candidates.assign(starts.begin(), starts.end());
            else
                filters.push_back(std::move(starts));
            if (stop.stop_requested()) {
                mark_partial("cancelled");
                break;
            }
        }
        for (const auto& [stream_id, start] : candidates) {
            if (stop.stop_requested()) {
                mark_partial("cancelled");
                break;
            }
            if (!std::ranges::all_of(filters, [&](const auto& filter) { return filter.contains({stream_id, start}); }))
                continue;
            if (query.after && (stream_id < query.after->stream_id ||
                                (stream_id == query.after->stream_id && start <= query.after->first_note)))
                continue;
            auto stream = cache.get(stream_id);
            if (!stream)
                return std::unexpected(std::move(stream).error());
            if (*stream == nullptr)
                continue;
            ++result.candidates_verified;
            if (auto occurrence = verify_exact(query, **stream, static_cast<std::uint32_t>(start)))
                found.push_back(std::move(*occurrence));
        }
    }
    std::ranges::sort(found, [](const MotifOccurrence& left, const MotifOccurrence& right) {
        return std::tie(left.stream.stream_id, left.first_note) < std::tie(right.stream.stream_id, right.first_note);
    });
    if (found.size() > query.result_limit) {
        found.resize(query.result_limit);
        mark_partial(std::format("showing the first {} occurrences", query.result_limit));
        result.next = ExactContinuation{found.back().stream.stream_id, found.back().first_note};
    }
    result.occurrences = std::move(found);
    return result;
}

// --- Approximate matching -------------------------------------------------------------------------

namespace {

ApproximateWeights effective_weights(const ApproximateQuery& query) {
    auto weights = query.weights;
    if (query.rhythm_only) {
        weights.pitch = 0.0;
        weights.velocity = 0.0;
    }
    const auto total = weights.pitch + weights.rhythm + weights.duration + weights.velocity + weights.edit;
    if (total > 0) {
        weights.pitch /= total;
        weights.rhythm /= total;
        weights.duration /= total;
        weights.velocity /= total;
        weights.edit /= total;
    }
    return weights;
}

double log_ratio_term(double query_value, double candidate_value, double scale) {
    if (query_value <= 0 || candidate_value <= 0 || scale <= 0)
        return 0.0;
    // scaled candidate value = candidate / scale.
    return std::min(std::abs(std::log2(query_value * scale / candidate_value)) / 2.0, 1.0);
}

double median(std::vector<double> values) {
    if (values.empty())
        return 1.0;
    std::ranges::sort(values);
    const auto middle = values.size() / 2;
    return values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
}

/// One DP cell: cost, then gaps, then candidate start for deterministic tie breaking.
struct Cell {
    double cost = std::numeric_limits<double>::infinity();
    std::uint32_t gaps = 0;
    std::int64_t start = 0;
    std::uint8_t move = 0; ///< 0 start, 1 match, 2 deletion (query note skipped), 3 insertion.

    [[nodiscard]] bool better_than(const Cell& other) const {
        if (cost != other.cost)
            return cost < other.cost;
        if (gaps != other.gaps)
            return gaps < other.gaps;
        return start < other.start;
    }
};

struct Alignment {
    std::vector<AlignedPair> pairs;
    std::size_t deletions = 0;
    std::size_t insertions = 0;
    std::int64_t first = 0;
    std::int64_t last = 0;
    bool valid = false;
};

/// Banded monotone subsequence alignment (profile 1 constants).
Alignment align(std::span<const QueryNote> query, std::span<const MotifNote> candidate, std::int64_t implied_start,
                double scale, const ApproximateWeights& weights) {
    const auto query_count = static_cast<std::int64_t>(query.size());
    const auto band = std::max<std::int64_t>(static_cast<std::int64_t>(alignment_min_band),
                                             static_cast<std::int64_t>(std::ceil(alignment_band_fraction *
                                                                                 static_cast<double>(query_count))));
    const auto low = std::max<std::int64_t>(0, implied_start - band);
    const auto high = std::min<std::int64_t>(static_cast<std::int64_t>(candidate.size()), implied_start + query_count + band);
    Alignment alignment;
    if (high <= low)
        return alignment;
    const auto width = high - low;
    const double local_total = weights.pitch + weights.rhythm + weights.duration + weights.velocity;
    const auto local_cost = [&](std::int64_t query_index, std::int64_t candidate_index) {
        if (local_total <= 0)
            return 0.0;
        const auto& query_note = query[static_cast<std::size_t>(query_index)];
        const auto& candidate_note = candidate[static_cast<std::size_t>(candidate_index)];
        double pitch_term = 0.0;
        double rhythm_term = 0.0;
        if (query_index > 0 && candidate_index > 0) {
            const auto& query_previous = query[static_cast<std::size_t>(query_index - 1)];
            const auto& candidate_previous = candidate[static_cast<std::size_t>(candidate_index - 1)];
            const auto query_interval = static_cast<int>(query_note.pitch) - query_previous.pitch;
            const auto candidate_interval = static_cast<int>(candidate_note.pitch) - candidate_previous.pitch;
            pitch_term = std::min(std::abs(query_interval - candidate_interval) / 12.0, 1.0);
            rhythm_term = log_ratio_term(static_cast<double>(query_note.onset - query_previous.onset),
                                         static_cast<double>(candidate_note.onset - candidate_previous.onset), scale);
        }
        const double duration_term = log_ratio_term(static_cast<double>(query_note.duration),
                                                    static_cast<double>(candidate_note.end - candidate_note.onset), scale);
        const double velocity_term = std::abs(static_cast<int>(query_note.velocity) - candidate_note.velocity) / 127.0;
        return (weights.pitch * pitch_term + weights.rhythm * rhythm_term + weights.duration * duration_term +
                weights.velocity * velocity_term) /
               local_total;
    };
    // cells[i][j]: query notes [0,i) aligned, candidate consumed up to local column j.
    const auto columns = static_cast<std::size_t>(width + 1);
    std::vector<Cell> cells(static_cast<std::size_t>(query_count + 1) * columns);
    const auto at = [&](std::int64_t row, std::int64_t column) -> Cell& {
        return cells[static_cast<std::size_t>(row) * columns + static_cast<std::size_t>(column)];
    };
    const auto in_band = [&](std::int64_t row, std::int64_t column) {
        return std::abs((low + column) - (implied_start + row)) <= band;
    };
    // Only the band's columns of each row are visited (about 2 * band + 1 instead of the full
    // width), which is where every reachable cell lies.
    const auto band_first = [&](std::int64_t row) { return std::max<std::int64_t>(0, implied_start + row - band - low); };
    const auto band_last = [&](std::int64_t row) { return std::min<std::int64_t>(width, implied_start + row + band - low); };
    for (std::int64_t column = band_first(0); column <= band_last(0); ++column)
        at(0, column) = Cell{0.0, 0, low + column, 0};
    for (std::int64_t row = 0; row <= query_count; ++row) {
        for (std::int64_t column = band_first(row); column <= band_last(row); ++column) {
            const auto& cell = at(row, column);
            if (!std::isfinite(cell.cost))
                continue;
            const auto relax = [&](std::int64_t next_row, std::int64_t next_column, double added, std::uint32_t gap,
                                   std::uint8_t move) {
                if (next_row > query_count || next_column > width || !in_band(next_row, next_column))
                    return;
                Cell candidate_cell{cell.cost + added, cell.gaps + gap, cell.start, move};
                if (candidate_cell.better_than(at(next_row, next_column)))
                    at(next_row, next_column) = candidate_cell;
            };
            if (row < query_count && column < width)
                relax(row + 1, column + 1, local_cost(row, low + column), 0, 1);
            if (row < query_count)
                relax(row + 1, column, alignment_gap_cost, 1, 2);
            if (row > 0 && row < query_count && column < width)
                relax(row, column + 1, alignment_gap_cost, 1, 3);
        }
    }
    std::int64_t best_column = -1;
    for (std::int64_t column = band_first(query_count); column <= band_last(query_count); ++column) {
        const auto& cell = at(query_count, column);
        if (std::isfinite(cell.cost) && (best_column < 0 || cell.better_than(at(query_count, best_column))))
            best_column = column;
    }
    if (best_column < 0)
        return alignment;
    // Trace back.
    std::int64_t row = query_count;
    std::int64_t column = best_column;
    while (row > 0) {
        const auto move = at(row, column).move;
        if (move == 1) {
            alignment.pairs.push_back(AlignedPair{static_cast<std::uint32_t>(row - 1),
                                                  static_cast<std::uint32_t>(low + column - 1)});
            --row;
            --column;
        } else if (move == 2) {
            ++alignment.deletions;
            --row;
        } else if (move == 3) {
            ++alignment.insertions;
            --column;
        } else {
            break;
        }
    }
    std::ranges::reverse(alignment.pairs);
    alignment.valid = !alignment.pairs.empty();
    if (alignment.valid) {
        alignment.first = alignment.pairs.front().candidate_index;
        alignment.last = alignment.pairs.back().candidate_index;
    }
    return alignment;
}

double matched_scale(std::span<const QueryNote> query, std::span<const MotifNote> candidate,
                     const std::vector<AlignedPair>& pairs) {
    std::vector<double> ratios;
    for (std::size_t index = 1; index < pairs.size(); ++index) {
        const auto query_ioi = query[pairs[index].query_index].onset - query[pairs[index - 1].query_index].onset;
        const auto candidate_ioi =
            candidate[pairs[index].candidate_index].onset - candidate[pairs[index - 1].candidate_index].onset;
        if (query_ioi > 0 && candidate_ioi > 0)
            ratios.push_back(static_cast<double>(candidate_ioi) / static_cast<double>(query_ioi));
    }
    return median(std::move(ratios));
}

} // namespace

std::optional<ApproximateMatch> align_window(const ApproximateQuery& query, const StoredMotifStream& candidate,
                                             std::int64_t implied_start) {
    const auto& notes = candidate.stream.notes;
    const auto& query_notes = query.notes;
    if (query_notes.size() < 2 || notes.empty())
        return std::nullopt;
    const auto weights = effective_weights(query);
    // Initial global scale from spans, then refined from median matched IOI ratios.
    const auto query_span = query_notes.back().onset - query_notes.front().onset;
    const auto clamp_index = [&](std::int64_t index) {
        return static_cast<std::size_t>(std::clamp<std::int64_t>(index, 0, static_cast<std::int64_t>(notes.size()) - 1));
    };
    const auto candidate_span =
        notes[clamp_index(implied_start + static_cast<std::int64_t>(query_notes.size()) - 1)].onset -
        notes[clamp_index(implied_start)].onset;
    double scale = (query_span > 0 && candidate_span > 0)
                       ? static_cast<double>(candidate_span) / static_cast<double>(query_span)
                       : 1.0;
    auto alignment = align(query_notes, notes, implied_start, scale, weights);
    if (!alignment.valid)
        return std::nullopt;
    scale = matched_scale(query_notes, notes, alignment.pairs);
    alignment = align(query_notes, notes, implied_start, scale, weights);
    if (!alignment.valid)
        return std::nullopt;
    scale = matched_scale(query_notes, notes, alignment.pairs);
    const auto required = static_cast<std::size_t>(
        std::ceil(alignment_min_aligned_fraction * static_cast<double>(query_notes.size())));
    if (alignment.pairs.size() < required)
        return std::nullopt;

    ScoreComponents components;
    components.weights = weights;
    components.time_scale = scale;
    components.matched = alignment.pairs.size();
    components.deletions = alignment.deletions;
    components.insertions = alignment.insertions;
    components.window_notes = static_cast<std::size_t>(alignment.last - alignment.first + 1);
    std::vector<double> pitch_terms;
    std::vector<double> rhythm_terms;
    std::vector<double> duration_terms;
    std::vector<double> velocity_terms;
    std::vector<int> transpositions;
    for (std::size_t index = 0; index < alignment.pairs.size(); ++index) {
        const auto& pair = alignment.pairs[index];
        const auto& query_note = query_notes[pair.query_index];
        const auto& candidate_note = notes[pair.candidate_index];
        transpositions.push_back(static_cast<int>(candidate_note.pitch) - query_note.pitch);
        duration_terms.push_back(log_ratio_term(static_cast<double>(query_note.duration),
                                                static_cast<double>(candidate_note.end - candidate_note.onset), scale));
        velocity_terms.push_back(std::abs(static_cast<int>(query_note.velocity) - candidate_note.velocity) / 127.0);
        if (index == 0)
            continue;
        const auto& previous = alignment.pairs[index - 1];
        const auto query_interval = static_cast<int>(query_note.pitch) - query_notes[previous.query_index].pitch;
        const auto candidate_interval = static_cast<int>(candidate_note.pitch) - notes[previous.candidate_index].pitch;
        pitch_terms.push_back(std::min(std::abs(query_interval - candidate_interval) / 12.0, 1.0));
        rhythm_terms.push_back(log_ratio_term(
            static_cast<double>(query_note.onset - query_notes[previous.query_index].onset),
            static_cast<double>(candidate_note.onset - notes[previous.candidate_index].onset), scale));
    }
    const auto mean = [](const std::vector<double>& values) {
        return values.empty() ? 0.0 : std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    };
    components.pitch = mean(pitch_terms);
    components.rhythm = mean(rhythm_terms);
    components.duration = mean(duration_terms);
    components.velocity = mean(velocity_terms);
    components.edit = std::min(static_cast<double>(alignment.insertions + alignment.deletions) /
                                   static_cast<double>(std::max(query_notes.size(), components.window_notes)),
                               1.0);
    components.distance = weights.pitch * components.pitch + weights.rhythm * components.rhythm +
                          weights.duration * components.duration + weights.velocity * components.velocity +
                          weights.edit * components.edit;
    components.score = 100.0 * (1.0 - std::clamp(components.distance, 0.0, 1.0));
    std::ranges::sort(transpositions);
    components.transpose = transpositions[transpositions.size() / 2];

    ApproximateMatch match;
    match.stream = candidate.info;
    match.first_note = static_cast<std::uint32_t>(alignment.first);
    match.end_note = static_cast<std::uint32_t>(alignment.last + 1);
    match.start_tick = notes[static_cast<std::size_t>(alignment.first)].onset;
    match.end_tick = 0;
    for (auto index = alignment.first; index <= alignment.last; ++index)
        match.end_tick = std::max(match.end_tick, notes[static_cast<std::size_t>(index)].end);
    match.pairs = std::move(alignment.pairs);
    for (const auto& pair : match.pairs)
        match.ordinals.push_back(notes[pair.candidate_index].ordinal);
    match.components = components;
    match.explanation = std::format(
        "score {:.1f}: pitch {:.3f} x {:.2f}, rhythm {:.3f} x {:.2f}, duration {:.3f} x {:.2f}, velocity {:.3f} x "
        "{:.2f}, edit {:.3f} x {:.2f}; {} of {} query notes aligned, {} inserted, {} deleted; time scale {:.3f}, "
        "transpose {:+d}",
        components.score, components.pitch, weights.pitch, components.rhythm, weights.rhythm, components.duration,
        weights.duration, components.velocity, weights.velocity, components.edit, weights.edit, components.matched,
        query_notes.size(), components.insertions, components.deletions, components.time_scale, components.transpose);
    return match;
}

Result<ApproximateSearchResult> approximate_search(MotifIndexReader& index, const ApproximateQuery& query,
                                                   std::stop_token stop) {
    if (query.notes.size() < 4 || query.notes.size() > 256)
        return make_error(ErrorCode::limit_exceeded,
                          "approximate queries need 4 to 256 notes; segment longer queries explicitly");
    ApproximateSearchResult result;
    result.label = std::format("approximate search (budget: {} streams, {} windows per stream)",
                               query.candidate_streams, query.windows_per_stream);
    // Gram-overlap votes as a flat (stream, implied start) list, counted after one sort: no node
    // allocation per posting.
    std::vector<std::pair<std::int64_t, std::int64_t>> raw_votes;
    std::vector<TokenFamily> families{TokenFamily::rhythm};
    if (!query.rhythm_only)
        families.insert(families.begin(), TokenFamily::interval);
    // Each distinct gram is fetched once and votes for every query position where it occurs.
    // Grams more common than the posting limit carry little information and are skipped (unless
    // nothing else is left); the total number of votes is capped.
    struct QueryGram {
        GramKey key;
        std::vector<std::int64_t> positions;
        std::uint64_t count = 0;
    };
    std::vector<QueryGram> query_grams;
    for (const auto family : families) {
        const auto tokens = family_tokens(query.notes, family);
        for (std::size_t position = 0; position + 3 <= tokens.size(); ++position) {
            const GramKey key{family, 3, gram_hash(family, std::span(tokens).subspan(position, 3))};
            auto existing = std::ranges::find_if(query_grams, [&](const QueryGram& gram) {
                return gram.key.family == key.family && gram.key.length == key.length && gram.key.hash == key.hash;
            });
            if (existing == query_grams.end()) {
                query_grams.push_back(QueryGram{key, {}, 0});
                existing = std::prev(query_grams.end());
            }
            existing->positions.push_back(static_cast<std::int64_t>(position));
        }
    }
    // Capped one past the posting limit: enough to tell ubiquitous grams (skipped below) apart.
    for (auto& gram : query_grams) {
        if (stop.stop_requested()) {
            result.partial = true;
            result.partial_reason = "cancelled";
            return result;
        }
        auto count = index.count_postings(gram.key, std::uint64_t{query.posting_limit} + 1);
        if (!count)
            return std::unexpected(std::move(count).error());
        gram.count = *count;
    }
    std::ranges::stable_sort(query_grams, {}, &QueryGram::count);
    constexpr std::size_t vote_budget = 4'000'000;
    for (std::size_t gram_index = 0; gram_index < query_grams.size(); ++gram_index) {
        const auto& gram = query_grams[gram_index];
        if (stop.stop_requested()) {
            result.partial = true;
            result.partial_reason = "cancelled";
            break;
        }
        if (gram.count > query.posting_limit && gram_index > 0) {
            result.candidate_budget_reached = true; // ubiquitous gram skipped
            continue;
        }
        auto postings = index.postings(gram.key, query.posting_limit);
        if (!postings)
            return std::unexpected(std::move(postings).error());
        if (postings->size() >= query.posting_limit)
            result.candidate_budget_reached = true;
        bool budget_hit = false;
        for (std::size_t item = 0; item < postings->size() && !budget_hit; ++item) {
            if ((item & 0xFFFF) == 0 && stop.stop_requested()) {
                result.partial = true;
                break;
            }
            const auto& posting = (*postings)[item];
            for (const auto position : gram.positions) {
                if (raw_votes.size() >= vote_budget) {
                    budget_hit = true;
                    result.candidate_budget_reached = true;
                    break;
                }
                raw_votes.emplace_back(posting.stream_id, static_cast<std::int64_t>(posting.start_ordinal) - position);
            }
        }
        if (budget_hit)
            break;
    }
    std::ranges::sort(raw_votes);
    // votes[stream] = (implied start, count) pairs in start order.
    std::map<std::int64_t, std::vector<std::pair<std::int64_t, std::size_t>>> votes;
    for (std::size_t item = 0; item < raw_votes.size();) {
        std::size_t end = item + 1;
        while (end < raw_votes.size() && raw_votes[end] == raw_votes[item])
            ++end;
        votes[raw_votes[item].first].emplace_back(raw_votes[item].second, end - item);
        item = end;
    }
    raw_votes = {};
    struct StreamVotes {
        std::int64_t id;
        std::size_t total;
    };
    std::vector<StreamVotes> streams;
    for (const auto& [id, starts] : votes) {
        std::size_t total = 0;
        for (const auto& [start, count] : starts)
            total += count;
        streams.push_back(StreamVotes{id, total});
    }
    std::ranges::stable_sort(streams, [](const StreamVotes& left, const StreamVotes& right) { return left.total > right.total; });
    if (streams.size() > query.candidate_streams) {
        streams.resize(query.candidate_streams);
        result.candidate_budget_reached = true;
    }
    StreamCache cache(index);
    std::map<std::pair<std::int64_t, std::uint32_t>, ApproximateMatch> best;
    for (const auto& stream_votes : streams) {
        if (stop.stop_requested()) {
            result.partial = true;
            break;
        }
        auto stored = cache.get(stream_votes.id);
        if (!stored)
            return std::unexpected(std::move(stored).error());
        if (*stored == nullptr)
            continue;
        ++result.streams_considered;
        auto windows = std::move(votes[stream_votes.id]);
        std::ranges::stable_sort(windows, [](const auto& left, const auto& right) { return left.second > right.second; });
        if (windows.size() > query.windows_per_stream)
            windows.resize(query.windows_per_stream);
        for (const auto& [implied_start, count] : windows) {
            ++result.windows_aligned;
            auto match = align_window(query, **stored, implied_start);
            if (!match)
                continue;
            const auto key = std::pair{match->stream.stream_id, match->first_note};
            const auto existing = best.find(key);
            if (existing == best.end() || match->components.score > existing->second.components.score)
                best.insert_or_assign(key, std::move(*match));
        }
    }
    for (auto& [key, match] : best)
        result.matches.push_back(std::move(match));
    std::ranges::stable_sort(result.matches, [](const ApproximateMatch& left, const ApproximateMatch& right) {
        if (left.components.score != right.components.score)
            return left.components.score > right.components.score;
        return std::tie(left.stream.stream_id, left.first_note) < std::tie(right.stream.stream_id, right.first_note);
    });
    if (result.matches.size() > query.result_limit)
        result.matches.resize(query.result_limit);
    if (result.partial && result.partial_reason.empty())
        result.partial_reason = stop.stop_requested() ? "cancelled" : "search budget reached";
    return result;
}

} // namespace mwb::search
