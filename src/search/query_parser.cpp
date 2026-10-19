// Query language of specification section 25.2: PEGTL grammar -> parse tree -> typed AST.

#include "mwb/search/query.hpp"

#include "mwb/core/pitch.hpp"
#include "mwb/core/text.hpp"
#include "mwb/core/utility.hpp"

#include <functional>
#include <tao/pegtl.hpp>
#include <tao/pegtl/contrib/parse_tree.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <memory>
#include <utility>

namespace mwb::search {

namespace grammar {

namespace peg = tao::pegtl;

/// Whitespace between terms.
struct whitespace_char : peg::one<' ', '\t', '\r', '\n'> {};
struct optional_space : peg::star<whitespace_char> {};
/// Characters that end a bare word or keyword.
struct delimiter : peg::sor<whitespace_char, peg::one<'(', ')'>, peg::eof> {};
struct keyword_and : peg::seq<peg::istring<'A', 'N', 'D'>, peg::at<delimiter>> {};
struct keyword_or : peg::seq<peg::istring<'O', 'R'>, peg::at<delimiter>> {};
struct keyword_not : peg::seq<peg::istring<'N', 'O', 'T'>, peg::at<delimiter>> {};
struct keyword : peg::sor<keyword_and, keyword_or, keyword_not> {};

/// Quoted strings escape only backslash and quote.
struct escape_sequence : peg::seq<peg::one<'\\'>, peg::one<'"', '\\'>> {};
struct quoted_character : peg::sor<escape_sequence, peg::not_one<'"', '\\'>> {};
struct closing_quote : peg::one<'"'> {};
struct quoted : peg::if_must<peg::one<'"'>, peg::star<quoted_character>, closing_quote> {};

struct range_separator : peg::two<'.'> {};
struct scalar_character : peg::not_one<' ', '\t', '\r', '\n', '(', ')', '"'> {};
struct bare_scalar : peg::plus<peg::not_at<range_separator>, scalar_character> {};
struct scalar : peg::sor<quoted, bare_scalar> {};
struct comparator : peg::sor<peg::string<'>', '='>, peg::string<'<', '='>, peg::one<'>', '<', '='>> {};
struct required_scalar : peg::must<scalar> {};
struct comparison_value : peg::seq<comparator, required_scalar> {};
struct range_tail : peg::if_must<range_separator, scalar> {};
struct plain_value : peg::seq<scalar, peg::opt<range_tail>> {};
struct value : peg::sor<comparison_value, plain_value> {};
struct required_value : peg::must<value> {};

struct field_name : peg::seq<peg::alpha, peg::star<peg::sor<peg::alnum, peg::one<'.', '_'>>>> {};
struct field_term : peg::seq<field_name, peg::one<':'>, required_value> {};
struct bare_word : peg::seq<peg::not_at<keyword>, peg::plus<scalar_character>> {};
struct term : peg::sor<field_term, quoted, bare_word> {};

struct or_expression;
struct closing_parenthesis : peg::one<')'> {};
struct required_inner : peg::must<or_expression> {};
struct parenthesized : peg::if_must<peg::one<'('>, optional_space, required_inner, optional_space, closing_parenthesis> {};
struct operand : peg::sor<parenthesized, term> {};
struct required_operand : peg::must<operand> {};
struct not_expression : peg::if_must<keyword_not, optional_space, required_operand> {};
struct unary : peg::sor<not_expression, operand> {};
struct required_unary : peg::must<unary> {};
struct explicit_and : peg::if_must<peg::seq<optional_space, keyword_and>, optional_space, required_unary> {};
struct implicit_and : peg::seq<optional_space, unary> {};
struct and_expression : peg::seq<unary, peg::star<peg::sor<explicit_and, implicit_and>>> {};
struct required_and : peg::must<and_expression> {};
struct or_tail : peg::if_must<peg::seq<optional_space, keyword_or>, optional_space, required_and> {};
struct or_expression : peg::seq<and_expression, peg::star<or_tail>> {};
struct end_of_query : peg::eof {};
struct query : peg::seq<optional_space, peg::opt<or_expression>, optional_space, peg::must<end_of_query>> {};

/// Parse-tree nodes kept for AST conversion.
template <class Rule>
using selector = peg::parse_tree::selector<
    Rule, peg::parse_tree::store_content::on<or_expression, and_expression, not_expression, field_term,
                                             field_name, comparator, quoted, bare_scalar, bare_word>>;

/// Human message for a failed `must<>` rule.
template <class Rule>
constexpr const char* error_message() {
    if constexpr (std::is_same_v<Rule, closing_quote>)
        return "unterminated quoted string or invalid escape (only \\\\ and \\\" are allowed)";
    else if constexpr (std::is_same_v<Rule, required_scalar> || std::is_same_v<Rule, scalar>)
        return "expected a value";
    else if constexpr (std::is_same_v<Rule, required_value>)
        return "expected a value after ':'";
    else if constexpr (std::is_same_v<Rule, required_inner>)
        return "expected an expression inside parentheses";
    else if constexpr (std::is_same_v<Rule, closing_parenthesis>)
        return "expected ')'";
    else if constexpr (std::is_same_v<Rule, required_operand> || std::is_same_v<Rule, required_unary>)
        return "expected a term after the operator";
    else if constexpr (std::is_same_v<Rule, required_and>)
        return "expected a term after OR";
    else if constexpr (std::is_same_v<Rule, end_of_query>)
        return "unexpected text";
    else
        return "syntax error";
}

/// Control class that turns failed `must<>` rules into readable parse errors.
template <class Rule>
struct control : peg::normal<Rule> {
    template <class ParseInput, class... States>
    [[noreturn]] static void raise(const ParseInput& input, States&&... /*states*/) {
        throw peg::parse_error(error_message<Rule>(), input);
    }
};

} // namespace grammar

namespace {

using ParseNode = tao::pegtl::parse_tree::node;

std::unexpected<QueryError> query_error(std::string code, std::string message, SourceSpan span) {
    return std::unexpected<QueryError>(QueryError{std::move(code), std::move(message), span});
}

/// End of the whitespace-delimited token starting at `begin` (at least one byte when possible).
std::size_t token_end(std::string_view text, std::size_t begin) {
    std::size_t end = begin;
    while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\r' && text[end] != '\n')
        ++end;
    if (end == begin && begin < text.size())
        ++end;
    return end;
}

SourceSpan span_of(const ParseNode& node) {
    return SourceSpan{node.begin().byte, node.end().byte};
}

/// Removes the surrounding quotes and resolves \\ and \" escapes.
std::string unquote(std::string_view quoted_text) {
    std::string result;
    if (quoted_text.size() < 2)
        return result;
    const auto inner = quoted_text.substr(1, quoted_text.size() - 2);
    result.reserve(inner.size());
    for (std::size_t index = 0; index < inner.size(); ++index) {
        if (inner[index] == '\\' && index + 1 < inner.size()) {
            result.push_back(inner[index + 1]);
            ++index;
        } else {
            result.push_back(inner[index]);
        }
    }
    return result;
}

struct FieldEntry {
    std::string_view name;
    Field field;
};

constexpr FieldEntry field_table[] = {
    {"text", Field::text},          {"tag", Field::tag},
    {"title", Field::title},        {"bpm", Field::bpm},
    {"tempo.any", Field::tempo_any}, {"tempo.origin", Field::tempo_origin},
    {"key", Field::key},            {"key.origin", Field::key_origin},
    {"confidence", Field::confidence}, {"length.qn", Field::length_qn},
    {"duration", Field::duration},  {"meter", Field::meter},
    {"tracks", Field::tracks},      {"notes", Field::notes},
    {"pitch.min", Field::pitch_min}, {"pitch.max", Field::pitch_max},
    {"channel", Field::channel},    {"program", Field::program},
    {"role", Field::role},          {"drums", Field::drums},
    {"favorite", Field::favorite},  {"rating", Field::rating},
    {"has", Field::has},            {"source", Field::source},
    {"collection", Field::collection}, {"status", Field::status},
};

std::optional<Field> find_field(std::string_view name) {
    const auto lowered = ascii_lower(name);
    for (const auto& entry : field_table)
        if (entry.name == lowered)
            return entry.field;
    return std::nullopt;
}

bool is_numeric_field(Field field) {
    switch (field) {
    case Field::bpm:
    case Field::tempo_any:
    case Field::confidence:
    case Field::length_qn:
    case Field::duration:
    case Field::tracks:
    case Field::notes:
    case Field::pitch_min:
    case Field::pitch_max:
    case Field::channel:
    case Field::program:
    case Field::rating:
        return true;
    case Field::any_text:
    case Field::tag:
    case Field::title:
    case Field::text:
    case Field::tempo_origin:
    case Field::key:
    case Field::key_origin:
    case Field::meter:
    case Field::role:
    case Field::drums:
    case Field::favorite:
    case Field::has:
    case Field::source:
    case Field::collection:
    case Field::status:
        return false;
    }
    return false;
}

std::optional<double> parse_number(std::string_view text) {
    if (text.empty())
        return std::nullopt;
    double value = 0.0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    if (*begin == '+')
        ++begin;
    const auto [pointer, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || pointer != end || !std::isfinite(value))
        return std::nullopt;
    return value;
}

using ScalarResult = std::expected<double, std::string>;

/// Converts one scalar of a numeric field into its canonical unit.
ScalarResult numeric_scalar(Field field, std::string_view text) {
    if (field == Field::duration) {
        double scale = 1.0;
        std::string_view number = text;
        const auto lowered = ascii_lower(text);
        if (lowered.ends_with("ms")) {
            scale = 0.001;
            number = text.substr(0, text.size() - 2);
        } else if (lowered.ends_with("min")) {
            scale = 60.0;
            number = text.substr(0, text.size() - 3);
        } else if (lowered.ends_with("s")) {
            number = text.substr(0, text.size() - 1);
        }
        const auto value = parse_number(number);
        if (!value || *value < 0)
            return std::unexpected("duration must be a non-negative number with an optional ms, s or min suffix");
        return *value * scale;
    }
    if (field == Field::pitch_min || field == Field::pitch_max) {
        if (const auto value = parse_number(text)) {
            if (*value < 0 || *value > 127 || std::floor(*value) != *value)
                return std::unexpected("pitch must be a whole number 0-127 or a note name such as C4");
            return *value;
        }
        if (const auto pitch = parse_pitch(text))
            return static_cast<double>(*pitch);
        return std::unexpected("pitch must be a whole number 0-127 or a note name such as C4");
    }
    const auto value = parse_number(text);
    if (!value)
        return std::unexpected(std::format("{} expects a number", field_name(field)));
    const auto require_integer = [&](double low, double high, const char* description) -> ScalarResult {
        if (std::floor(*value) != *value || *value < low || *value > high)
            return std::unexpected(std::format("{} expects {}", field_name(field), description));
        return *value;
    };
    switch (field) {
    case Field::bpm:
    case Field::tempo_any:
        if (*value <= 0 || *value > 2000)
            return std::unexpected("tempo must be between 0 and 2000 BPM");
        return *value;
    case Field::confidence:
        if (*value < 0 || *value > 1)
            return std::unexpected("confidence must be between 0 and 1");
        return *value;
    case Field::length_qn:
        if (*value < 0)
            return std::unexpected("length.qn must be non-negative");
        return *value;
    case Field::tracks:
    case Field::notes:
        return require_integer(0, 1e12, "a non-negative whole number");
    case Field::channel:
        return require_integer(1, 16, "a channel number 1-16");
    case Field::program:
        return require_integer(1, 128, "a program number 1-128");
    case Field::rating:
        return require_integer(0, 5, "a rating 0-5");
    case Field::any_text:
    case Field::tag:
    case Field::title:
    case Field::text:
    case Field::tempo_origin:
    case Field::key:
    case Field::key_origin:
    case Field::duration:
    case Field::meter:
    case Field::pitch_min:
    case Field::pitch_max:
    case Field::role:
    case Field::drums:
    case Field::favorite:
    case Field::has:
    case Field::source:
    case Field::collection:
    case Field::status:
        break;
    }
    return *value;
}

std::optional<KeyValue> parse_key(std::string_view raw) {
    const auto text = trim(raw);
    if (text.empty())
        return std::nullopt;
    const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    if (letter < 'A' || letter > 'G')
        return std::nullopt;
    std::size_t position = 1;
    std::string root_text(1, letter);
    while (position < text.size() && (text[position] == '#' || text[position] == 'b')) {
        // "Bb" is B flat, but a lone "b" right before a mode word like "bm" cannot occur after a
        // root letter other than as an accidental, so accidentals are consumed greedily.
        root_text.push_back(text[position]);
        ++position;
    }
    const auto root = parse_pitch_class(root_text);
    if (!root)
        return std::nullopt;
    const auto mode_text = trim(text.substr(position));
    KeyValue key;
    key.root = *root;
    if (mode_text.empty())
        return key;
    auto mode = normalize_key_mode(mode_text);
    if (!mode)
        return std::nullopt;
    key.mode = std::move(*mode);
    return key;
}

std::optional<MeterValue> parse_meter(std::string_view text) {
    const auto slash = text.find('/');
    if (slash == std::string_view::npos)
        return std::nullopt;
    int numerator = 0;
    int denominator = 0;
    const auto first = text.substr(0, slash);
    const auto second = text.substr(slash + 1);
    if (std::from_chars(first.data(), first.data() + first.size(), numerator).ptr != first.data() + first.size() ||
        std::from_chars(second.data(), second.data() + second.size(), denominator).ptr !=
            second.data() + second.size())
        return std::nullopt;
    if (first.empty() || second.empty() || numerator < 1 || numerator > 255 || denominator < 1 ||
        denominator > 256 || (denominator & (denominator - 1)) != 0)
        return std::nullopt;
    return MeterValue{numerator, denominator};
}

std::optional<bool> parse_boolean(std::string_view text) {
    const auto lowered = ascii_lower(text);
    if (lowered == "true" || lowered == "yes" || lowered == "1")
        return true;
    if (lowered == "false" || lowered == "no" || lowered == "0")
        return false;
    return std::nullopt;
}

bool one_of(std::string_view value, std::initializer_list<std::string_view> allowed) {
    return std::ranges::find(allowed, value) != allowed.end();
}

/// Raw value of a field term before validation.
struct RawValue {
    std::optional<Comparator> comparator;
    std::string first;
    std::optional<std::string> second;
    bool quoted = false;
    SourceSpan span;
};

using PredicateResult = std::expected<Predicate, QueryError>;

/// Validates a text value for a full-text field and folds it.
PredicateResult text_predicate(Field field, const std::string& value, bool quoted, SourceSpan span) {
    auto folded = fold_for_search(value);
    if (const auto tokens = ascii_fts_tokens(folded); tokens && tokens->empty())
        return query_error("query.value", "the text contains no searchable letters or digits", span);
    return Predicate{field, PredicateValue{std::move(folded)}, quoted};
}

/// Builds the typed predicate for `field:value` and validates the value.
PredicateResult build_predicate(Field field, const RawValue& raw) {
    const auto span = raw.span;
    if (is_numeric_field(field)) {
        NumericCondition condition;
        const auto first = numeric_scalar(field, raw.first);
        if (!first)
            return query_error("query.value", first.error(), span);
        condition.value = *first;
        if (raw.second) {
            const auto second = numeric_scalar(field, *raw.second);
            if (!second)
                return query_error("query.value", second.error(), span);
            if (*second < *first)
                return query_error("query.value", "the range's lower bound exceeds its upper bound", span);
            condition.comparator = Comparator::range;
            condition.high = *second;
        } else {
            condition.comparator = raw.comparator.value_or(Comparator::equal);
            condition.high = condition.value;
        }
        return Predicate{field, PredicateValue{condition}, raw.quoted};
    }
    if (raw.comparator && *raw.comparator != Comparator::equal)
        return query_error("query.value", std::format("{} does not support comparisons", field_name(field)), span);
    if (raw.second)
        return query_error("query.value", std::format("{} does not support ranges", field_name(field)), span);
    const auto& text = raw.first;
    switch (field) {
    case Field::any_text:
    case Field::title:
    case Field::text:
    case Field::source:
        return text_predicate(field, text, raw.quoted, span);
    case Field::tag:
    case Field::role:
    case Field::collection: {
        auto folded = fold_for_search(trim(text));
        if (folded.empty())
            return query_error("query.value", std::format("{} needs a name", field_name(field)), span);
        return Predicate{field, PredicateValue{std::move(folded)}, raw.quoted};
    }
    case Field::tempo_origin: {
        const auto lowered = ascii_lower(text);
        if (!one_of(lowered, {"explicit", "default", "reference", "unknown"}))
            return query_error("query.value", "tempo.origin is explicit, default, reference or unknown", span);
        return Predicate{field, PredicateValue{lowered}, raw.quoted};
    }
    case Field::key_origin: {
        const auto lowered = ascii_lower(text);
        if (!one_of(lowered, {"manual", "signature", "estimated", "unknown"}))
            return query_error("query.value", "key.origin is manual, signature, estimated or unknown", span);
        return Predicate{field, PredicateValue{lowered}, raw.quoted};
    }
    case Field::status: {
        const auto lowered = ascii_lower(text);
        if (!one_of(lowered, {"ready", "indexing", "warning", "error", "source_missing"}))
            return query_error("query.value", "status is ready, indexing, warning, error or source_missing", span);
        return Predicate{field, PredicateValue{lowered}, raw.quoted};
    }
    case Field::has: {
        const auto lowered = ascii_lower(text);
        if (!one_of(lowered, {"lyrics", "sysex", "bend", "tempo_changes", "meter_changes", "markers", "text",
                              "aftertouch", "program_changes", "controllers", "key_signatures"}))
            return query_error("query.value",
                               "has: expects lyrics, sysex, bend, tempo_changes, meter_changes, markers, text, "
                               "aftertouch, program_changes, controllers or key_signatures",
                               span);
        return Predicate{field, PredicateValue{lowered}, raw.quoted};
    }
    case Field::key: {
        const auto key = parse_key(text);
        if (!key)
            return query_error("query.value", "key expects a root and optional mode, such as \"F# minor\"", span);
        return Predicate{field, PredicateValue{*key}, raw.quoted};
    }
    case Field::meter: {
        const auto meter = parse_meter(text);
        if (!meter)
            return query_error("query.value", "meter expects numerator/denominator such as 6/8", span);
        return Predicate{field, PredicateValue{*meter}, raw.quoted};
    }
    case Field::drums:
    case Field::favorite: {
        const auto value = parse_boolean(text);
        if (!value)
            return query_error("query.value", std::format("{} expects true or false", field_name(field)), span);
        return Predicate{field, PredicateValue{*value}, raw.quoted};
    }
    case Field::bpm:
    case Field::tempo_any:
    case Field::confidence:
    case Field::length_qn:
    case Field::duration:
    case Field::tracks:
    case Field::notes:
    case Field::pitch_min:
    case Field::pitch_max:
    case Field::channel:
    case Field::program:
    case Field::rating:
        break;
    }
    return query_error("query.value", "unsupported field", span);
}

Comparator comparator_from_text(std::string_view text) {
    if (text == ">=")
        return Comparator::greater_equal;
    if (text == "<=")
        return Comparator::less_equal;
    if (text == ">")
        return Comparator::greater;
    if (text == "<")
        return Comparator::less;
    return Comparator::equal;
}

// Local propagation helper for the converter's expected<void, QueryError> checks.
#define MWB_TRY_QUERY(expression)                                                                  \
    do {                                                                                           \
        if (auto mwb_query_check_ = (expression); !mwb_query_check_)                               \
            return std::unexpected(std::move(mwb_query_check_).error());                           \
    } while (false)

/// Converts the PEGTL parse tree into the typed AST, enforcing node and depth limits.
class TreeConverter {
public:
    explicit TreeConverter(const QueryLimits& limits) : limits_(limits) {}

    std::expected<QueryNode, QueryError> convert(const ParseNode& node, std::size_t depth) {
        if (depth > limits_.max_depth)
            return query_error("query.limit", std::format("nesting deeper than {} levels", limits_.max_depth),
                               span_of(node));
        if (node.is_type<grammar::or_expression>() || node.is_type<grammar::and_expression>()) {
            const bool is_or = node.is_type<grammar::or_expression>();
            if (node.children.size() == 1)
                return convert(*node.children.front(), depth);
            QueryNode result;
            result.kind = is_or ? NodeKind::or_node : NodeKind::and_node;
            result.span = span_of(node);
            MWB_TRY_QUERY(count(result.span));
            for (const auto& child : node.children) {
                auto converted = convert(*child, depth + 1);
                if (!converted)
                    return converted;
                result.children.push_back(std::move(*converted));
            }
            return result;
        }
        if (node.is_type<grammar::not_expression>()) {
            QueryNode result;
            result.kind = NodeKind::not_node;
            result.span = span_of(node);
            MWB_TRY_QUERY(count(result.span));
            auto converted = convert(*node.children.front(), depth + 1);
            if (!converted)
                return converted;
            result.children.push_back(std::move(*converted));
            return result;
        }
        QueryNode result;
        result.kind = NodeKind::predicate;
        result.span = span_of(node);
        MWB_TRY_QUERY(count(result.span));
        if (node.is_type<grammar::quoted>() || node.is_type<grammar::bare_word>()) {
            const bool quoted = node.is_type<grammar::quoted>();
            const auto text = quoted ? unquote(node.string_view()) : std::string(node.string_view());
            auto predicate = text_predicate(Field::any_text, text, quoted, result.span);
            if (!predicate)
                return std::unexpected(predicate.error());
            result.predicate = std::move(*predicate);
            return result;
        }
        if (!node.is_type<grammar::field_term>() || node.children.empty())
            return query_error("query.syntax", "unexpected term", result.span);
        const auto& name_node = *node.children.front();
        const auto field = find_field(name_node.string_view());
        if (!field)
            return query_error("query.field", std::format("unknown field '{}'", name_node.string_view()),
                               span_of(name_node));
        RawValue raw;
        raw.span = SourceSpan{node.children.size() > 1 ? node.children[1]->begin().byte : result.span.end,
                              result.span.end};
        bool have_first = false;
        for (std::size_t index = 1; index < node.children.size(); ++index) {
            const auto& child = *node.children[index];
            if (child.is_type<grammar::comparator>()) {
                raw.comparator = comparator_from_text(child.string_view());
                continue;
            }
            const bool quoted = child.is_type<grammar::quoted>();
            auto text = quoted ? unquote(child.string_view()) : std::string(child.string_view());
            raw.quoted = raw.quoted || quoted;
            if (!have_first) {
                raw.first = std::move(text);
                have_first = true;
            } else {
                raw.second = std::move(text);
            }
        }
        auto predicate = build_predicate(*field, raw);
        if (!predicate)
            return std::unexpected(predicate.error());
        result.predicate = std::move(*predicate);
        return result;
    }

    [[nodiscard]] std::size_t node_count() const { return nodes_; }

private:
    std::expected<void, QueryError> count(SourceSpan span) {
        if (++nodes_ > limits_.max_nodes)
            return query_error("query.limit", std::format("more than {} query terms and operators", limits_.max_nodes),
                               span);
        return {};
    }

    const QueryLimits& limits_;
    std::size_t nodes_ = 0;
};

#undef MWB_TRY_QUERY

/// Rejects parenthesis nesting beyond the limit before recursive parsing starts.
std::optional<QueryError> check_nesting(std::string_view text, std::size_t max_depth) {
    std::size_t depth = 0;
    bool in_quote = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (in_quote) {
            if (character == '\\')
                ++index;
            else if (character == '"')
                in_quote = false;
            continue;
        }
        if (character == '"') {
            in_quote = true;
        } else if (character == '(') {
            if (++depth > max_depth)
                return QueryError{"query.limit", std::format("nesting deeper than {} levels", max_depth),
                                  SourceSpan{index, index + 1}};
        } else if (character == ')' && depth > 0) {
            --depth;
        }
    }
    return std::nullopt;
}

} // namespace

std::string_view field_name(Field field) noexcept {
    if (field == Field::any_text)
        return "text";
    for (const auto& entry : field_table)
        if (entry.field == field)
            return entry.name;
    return "unknown";
}

bool is_sequence_field(Field field) noexcept {
    switch (field) {
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
        return true;
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
        return false;
    }
    return false;
}

bool is_text_field(Field field) noexcept {
    return field == Field::any_text || field == Field::title || field == Field::text || field == Field::source;
}

Error QueryError::to_error() const {
    return Error{ErrorCode::invalid_argument, message, std::format("{} at bytes {}..{}", code, span.begin, span.end),
                 nullptr};
}

bool mark_prefix_term(Query& query, std::string_view text) {
    if (text.empty() || text.back() == ' ' || text.back() == '\t' || text.back() == '"' || text.back() == ')')
        return false; // a finished word, a phrase or a group: matched exactly
    QueryNode* last = nullptr;
    const std::function<void(QueryNode&)> visit = [&](QueryNode& node) {
        if (node.kind == NodeKind::predicate && (!last || node.span.end > last->span.end))
            last = &node;
        for (auto& child : node.children)
            visit(child);
    };
    visit(query.root);
    if (last == nullptr || last->span.end != text.size() || last->predicate.quoted ||
        !std::holds_alternative<std::string>(last->predicate.value))
        return false;
    const auto field = last->predicate.field;
    if (field != Field::any_text && field != Field::text && field != Field::title && field != Field::source)
        return false; // tags, numbers and names of things are matched exactly
    last->predicate.prefix = true;
    return true;
}

std::optional<std::vector<std::string>> ascii_fts_tokens(std::string_view folded_text) {
    std::vector<std::string> tokens;
    std::string current;
    for (const char character : folded_text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= 0x80)
            return std::nullopt;
        if (std::isalnum(byte) != 0) {
            current.push_back(static_cast<char>(std::tolower(byte)));
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty())
        tokens.push_back(std::move(current));
    return tokens;
}

IntegerInterval integer_interval(const NumericCondition& condition, std::int64_t minimum, std::int64_t maximum) {
    // Integer fields only carry whole values (validated by the parser), so the conversion is exact.
    const auto value = static_cast<std::int64_t>(condition.value);
    IntegerInterval interval{minimum, maximum};
    switch (condition.comparator) {
    case Comparator::equal:
        interval = {value, value};
        break;
    case Comparator::greater:
        interval.low = value + 1;
        break;
    case Comparator::greater_equal:
        interval.low = value;
        break;
    case Comparator::less:
        interval.high = value - 1;
        break;
    case Comparator::less_equal:
        interval.high = value;
        break;
    case Comparator::range:
        interval = {value, static_cast<std::int64_t>(condition.high)};
        break;
    }
    interval.low = std::max(interval.low, minimum);
    interval.high = std::min(interval.high, maximum);
    return interval;
}

RealInterval real_interval(const NumericCondition& condition) {
    RealInterval interval;
    switch (condition.comparator) {
    case Comparator::equal:
        interval.low = condition.value - real_epsilon;
        interval.high = condition.value + real_epsilon;
        break;
    case Comparator::greater:
        interval.low = condition.value;
        interval.low_open = true;
        interval.has_high = false;
        break;
    case Comparator::greater_equal:
        interval.low = condition.value;
        interval.has_high = false;
        break;
    case Comparator::less:
        interval.high = condition.value;
        interval.high_open = true;
        interval.has_low = false;
        break;
    case Comparator::less_equal:
        interval.high = condition.value;
        interval.has_low = false;
        break;
    case Comparator::range:
        interval.low = condition.value;
        interval.high = condition.high;
        break;
    }
    return interval;
}

bool RealInterval::contains(double value) const {
    if (has_low && (low_open ? !(value > low) : !(value >= low)))
        return false;
    if (has_high && (high_open ? !(value < high) : !(value <= high)))
        return false;
    return true;
}

QueryResult parse_query(std::string_view text, const QueryLimits& limits) {
    if (text.size() > limits.max_bytes)
        return query_error("query.limit", std::format("the query is longer than {} bytes", limits.max_bytes),
                           SourceSpan{limits.max_bytes, text.size()});
    if (!is_valid_utf8(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())))
        return query_error("query.encoding", "the query is not valid UTF-8", SourceSpan{0, text.size()});
    if (auto nesting = check_nesting(text, limits.max_depth))
        return std::unexpected(std::move(*nesting));

    Query query;
    query.text = std::string(text);
    std::unique_ptr<ParseNode> tree;
    // The input outlives the tree: parse-tree nodes refer to its source name when they compute
    // positions (found by the query_parser fuzz target as a stack-use-after-scope).
    tao::pegtl::memory_input<> input(query.text.data(), query.text.size(), "query");
    try {
        tree = tao::pegtl::parse_tree::parse<grammar::query, grammar::selector, tao::pegtl::nothing, grammar::control>(
            input);
    } catch (const tao::pegtl::parse_error& error) {
        const auto position = error.positions().empty() ? std::size_t{0} : error.positions().front().byte;
        std::size_t begin = std::min(position, text.size());
        std::size_t end = token_end(text, begin);
        std::string message(error.message());
        if (message.starts_with("unterminated quoted")) {
            // Span from the opening quote of the failing string to the error position's token.
            const auto opening = text.rfind('"', begin == 0 ? 0 : begin - 1);
            if (opening != std::string_view::npos && opening < begin)
                begin = opening;
            end = std::max(end, std::min(text.size(), position + 1));
        } else if (message == "unexpected text" && begin < text.size()) {
            message = std::format("unexpected '{}'", text.substr(begin, end - begin));
        }
        return query_error("query.syntax", std::move(message), SourceSpan{begin, end});
    }
    if (!tree)
        return query_error("query.syntax", "syntax error", SourceSpan{0, text.size()});
    if (tree->children.empty()) {
        query.root = QueryNode{};
        query.root.span = SourceSpan{0, text.size()};
        query.node_count = 0;
        return query;
    }
    TreeConverter converter(limits);
    auto root = converter.convert(*tree->children.front(), 1);
    if (!root)
        return std::unexpected(std::move(root).error());
    query.root = std::move(*root);
    query.node_count = converter.node_count();
    return query;
}

// --- JSON form (smart collections) ---------------------------------------------------------------

namespace {

using Json = nlohmann::json;

std::string_view comparator_name(Comparator comparator) {
    switch (comparator) {
    case Comparator::equal:
        return "=";
    case Comparator::greater:
        return ">";
    case Comparator::greater_equal:
        return ">=";
    case Comparator::less:
        return "<";
    case Comparator::less_equal:
        return "<=";
    case Comparator::range:
        return "..";
    }
    return "=";
}

std::string_view kind_name(NodeKind kind) {
    switch (kind) {
    case NodeKind::and_node:
        return "and";
    case NodeKind::or_node:
        return "or";
    case NodeKind::not_node:
        return "not";
    case NodeKind::predicate:
        return "predicate";
    }
    return "and";
}

Json node_to_json(const QueryNode& node) {
    Json object;
    object["kind"] = kind_name(node.kind);
    object["span"] = Json::array({node.span.begin, node.span.end});
    if (node.kind == NodeKind::predicate) {
        const auto& predicate = node.predicate;
        object["field"] = predicate.field == Field::any_text ? std::string_view("*") : field_name(predicate.field);
        object["quoted"] = predicate.quoted;
        std::visit(Overloaded{
                       [&](const std::string& text) { object["text"] = text; },
                       [&](const NumericCondition& condition) {
                           object["comparator"] = comparator_name(condition.comparator);
                           object["value"] = condition.value;
                           object["high"] = condition.high;
                       },
                       [&](bool value) { object["boolean"] = value; },
                       [&](const KeyValue& key) {
                           object["key_root"] = key.root;
                           object["key_mode"] = key.mode;
                       },
                       [&](const MeterValue& meter) {
                           object["meter"] = Json::array({meter.numerator, meter.denominator});
                       },
                   },
                   predicate.value);
        return object;
    }
    Json children = Json::array();
    for (const auto& child : node.children)
        children.push_back(node_to_json(child));
    object["children"] = std::move(children);
    return object;
}

Result<QueryNode> node_from_json(const Json& object, std::size_t depth, std::size_t& nodes, const QueryLimits& limits) {
    if (!object.is_object() || !object.contains("kind") || !object["kind"].is_string())
        return make_error(ErrorCode::invalid_data, "query JSON node lacks a kind");
    if (depth > limits.max_depth)
        return make_error(ErrorCode::limit_exceeded, "query JSON nesting exceeds the limit");
    const auto kind = object["kind"].get<std::string>();
    QueryNode node;
    if (object.contains("span") && object["span"].is_array() && object["span"].size() == 2 &&
        object["span"][0].is_number_unsigned() && object["span"][1].is_number_unsigned())
        node.span = SourceSpan{object["span"][0].get<std::size_t>(), object["span"][1].get<std::size_t>()};
    if (kind == "and" || kind == "or" || kind == "not") {
        node.kind = kind == "and" ? NodeKind::and_node : kind == "or" ? NodeKind::or_node : NodeKind::not_node;
        if (!object.contains("children") || !object["children"].is_array())
            return make_error(ErrorCode::invalid_data, "query JSON operator lacks children");
        if (node.kind == NodeKind::not_node && object["children"].size() != 1)
            return make_error(ErrorCode::invalid_data, "query JSON NOT must have one child");
        if ((depth > 1 || !object["children"].empty()) && ++nodes > limits.max_nodes)
                return make_error(ErrorCode::limit_exceeded, "query JSON has too many nodes");
        for (const auto& child : object["children"]) {
            auto converted = node_from_json(child, depth + 1, nodes, limits);
            if (!converted)
                return converted;
            node.children.push_back(std::move(*converted));
        }
        return node;
    }
    if (kind != "predicate")
        return make_error(ErrorCode::invalid_data, "query JSON node has an unknown kind");
    if (++nodes > limits.max_nodes)
        return make_error(ErrorCode::limit_exceeded, "query JSON has too many nodes");
    node.kind = NodeKind::predicate;
    if (!object.contains("field") || !object["field"].is_string())
        return make_error(ErrorCode::invalid_data, "query JSON predicate lacks a field");
    const auto name = object["field"].get<std::string>();
    Field field = Field::any_text;
    if (name != "*") {
        const auto found = find_field(name);
        if (!found)
            return make_error(ErrorCode::invalid_data, "query JSON predicate has an unknown field", name);
        field = *found;
    }
    // Rebuild the predicate through the same validation as parsing by reconstructing raw values.
    RawValue raw;
    raw.quoted = object.contains("quoted") && object["quoted"].is_boolean() && object["quoted"].get<bool>();
    if (is_numeric_field(field)) {
        if (!object.contains("comparator") || !object["comparator"].is_string() || !object.contains("value") ||
            !object["value"].is_number() || !object.contains("high") || !object["high"].is_number())
            return make_error(ErrorCode::invalid_data, "query JSON numeric predicate is incomplete");
        NumericCondition condition;
        const auto comparator = object["comparator"].get<std::string>();
        condition.comparator = comparator == ".." ? Comparator::range : comparator_from_text(comparator);
        if (comparator != ".." && comparator != "=" && comparator_from_text(comparator) == Comparator::equal)
            return make_error(ErrorCode::invalid_data, "query JSON comparator is unknown");
        condition.value = object["value"].get<double>();
        condition.high = object["high"].get<double>();
        // Validate the canonical values with the parser's scalar rules (canonical units round-trip
        // except duration, which is stored in seconds and therefore validated as seconds).
        const auto check = [&](double value) -> Status {
            auto text = std::format("{}", value);
            if (field == Field::duration)
                text += "s";
            const auto converted = numeric_scalar(field, text);
            if (!converted)
                return make_error(ErrorCode::invalid_data, converted.error());
            return ok();
        };
        MWB_TRY(check(condition.value));
        MWB_TRY(check(condition.high));
        if (condition.comparator == Comparator::range && condition.high < condition.value)
            return make_error(ErrorCode::invalid_data, "query JSON range is inverted");
        node.predicate = Predicate{field, PredicateValue{condition}, raw.quoted};
        return node;
    }
    if (field == Field::key) {
        if (!object.contains("key_root") || !object["key_root"].is_number_integer() || !object.contains("key_mode") ||
            !object["key_mode"].is_string())
            return make_error(ErrorCode::invalid_data, "query JSON key predicate is incomplete");
        KeyValue key{object["key_root"].get<int>(), object["key_mode"].get<std::string>()};
        if (key.root < 0 || key.root > 11 ||
            !(key.mode.empty() ||
              one_of(key.mode, {"major", "minor", "dorian", "phrygian", "lydian", "mixolydian", "locrian"})))
            return make_error(ErrorCode::invalid_data, "query JSON key value is invalid");
        node.predicate = Predicate{field, PredicateValue{key}, raw.quoted};
        return node;
    }
    if (field == Field::meter) {
        if (!object.contains("meter") || !object["meter"].is_array() || object["meter"].size() != 2 ||
            !object["meter"][0].is_number_integer() || !object["meter"][1].is_number_integer())
            return make_error(ErrorCode::invalid_data, "query JSON meter predicate is incomplete");
        const auto meter = parse_meter(std::format("{}/{}", object["meter"][0].get<int>(), object["meter"][1].get<int>()));
        if (!meter)
            return make_error(ErrorCode::invalid_data, "query JSON meter value is invalid");
        node.predicate = Predicate{field, PredicateValue{*meter}, raw.quoted};
        return node;
    }
    if (field == Field::drums || field == Field::favorite) {
        if (!object.contains("boolean") || !object["boolean"].is_boolean())
            return make_error(ErrorCode::invalid_data, "query JSON boolean predicate is incomplete");
        node.predicate = Predicate{field, PredicateValue{object["boolean"].get<bool>()}, raw.quoted};
        return node;
    }
    if (!object.contains("text") || !object["text"].is_string())
        return make_error(ErrorCode::invalid_data, "query JSON text predicate is incomplete");
    raw.first = object["text"].get<std::string>();
    auto predicate = build_predicate(field, raw);
    if (!predicate)
        return make_error(ErrorCode::invalid_data, predicate.error().message);
    node.predicate = std::move(*predicate);
    return node;
}

} // namespace

std::string query_to_json(const Query& query) {
    Json object;
    object["version"] = query_json_version;
    object["text"] = query.text;
    object["root"] = node_to_json(query.root);
    return object.dump();
}

Result<Query> query_from_json(std::string_view json, const QueryLimits& limits) {
    const auto parsed = Json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
        return make_error(ErrorCode::invalid_data, "query JSON is malformed");
    if (!parsed.contains("version") || !parsed["version"].is_number_integer() ||
        parsed["version"].get<int>() != query_json_version)
        return make_error(ErrorCode::unsupported, "query JSON has an unsupported version");
    if (!parsed.contains("text") || !parsed["text"].is_string() || !parsed.contains("root"))
        return make_error(ErrorCode::invalid_data, "query JSON lacks text or root");
    Query query;
    query.text = parsed["text"].get<std::string>();
    if (query.text.size() > limits.max_bytes)
        return make_error(ErrorCode::limit_exceeded, "query JSON text exceeds the limit");
    std::size_t nodes = 0;
    auto root = node_from_json(parsed["root"], 1, nodes, limits);
    if (!root)
        return std::unexpected(std::move(root).error());
    query.root = std::move(*root);
    query.node_count = nodes;
    return query;
}

std::optional<std::string> normalize_key_mode(std::string_view text) {
    const auto mode = ascii_lower(trim(text));
    if (mode == "m" || mode == "min" || mode == "minor" || mode == "aeolian")
        return std::string("minor");
    if (mode == "maj" || mode == "major" || mode == "ionian")
        return std::string("major");
    if (mode == "dorian" || mode == "phrygian" || mode == "lydian" || mode == "mixolydian" || mode == "locrian")
        return mode;
    return std::nullopt;
}

} // namespace mwb::search
