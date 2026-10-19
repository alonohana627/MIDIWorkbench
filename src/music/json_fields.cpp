#include "mwb/music/json_fields.hpp"

namespace mwb::music {

FieldReader::FieldReader(const nlohmann::json& object, std::string context)
    : object_(object), context_(std::move(context)) {
    if (!object_.is_object())
        status_ = make_error(ErrorCode::invalid_data, "expected a JSON object", context_);
}

const nlohmann::json* FieldReader::find(std::string_view key) const {
    if (!object_.is_object())
        return nullptr;
    const auto found = object_.find(std::string(key));
    return found == object_.end() ? nullptr : &*found;
}

bool FieldReader::present(std::string_view key) const { return find(key) != nullptr; }

bool FieldReader::is_null(std::string_view key) const {
    const auto* field = find(key);
    return field != nullptr && field->is_null();
}

void FieldReader::fail(std::string_view key, std::string message) {
    if (!status_)
        return;
    std::string where = context_.empty() ? std::string(key) : context_ + "." + std::string(key);
    status_ = make_error(ErrorCode::invalid_data, "invalid field: " + message, where);
}

bool FieldReader::read_int64(std::string_view key, std::int64_t& value) {
    const auto* field = find(key);
    if (!field || !status_)
        return false;
    if (field->is_number_integer()) {
        if (field->is_number_unsigned()) {
            const auto unsigned_value = field->get<std::uint64_t>();
            if (!std::in_range<std::int64_t>(unsigned_value)) {
                fail(key, "integer out of range");
                return false;
            }
            value = static_cast<std::int64_t>(unsigned_value);
            return true;
        }
        value = field->get<std::int64_t>();
        return true;
    }
    fail(key, "expected an integer");
    return false;
}

void FieldReader::optional_integer(std::string_view key, std::optional<int>& out) {
    const auto* field = find(key);
    if (!field || !status_)
        return;
    if (field->is_null()) {
        out.reset();
        return;
    }
    int value = 0;
    integer(key, value);
    if (status_)
        out = value;
}

void FieldReader::boolean(std::string_view key, bool& out) {
    const auto* field = find(key);
    if (!field || !status_)
        return;
    if (!field->is_boolean()) {
        fail(key, "expected a boolean");
        return;
    }
    out = field->get<bool>();
}

void FieldReader::text(std::string_view key, std::string& out) {
    const auto* field = find(key);
    if (!field || !status_)
        return;
    if (!field->is_string()) {
        fail(key, "expected a string");
        return;
    }
    out = field->get<std::string>();
}

void FieldReader::rational(std::string_view key, Rational& out) {
    const auto* field = find(key);
    if (!field || !status_)
        return;
    // Both parts stay within the recipe magnitude bound, so grid and strength products of a
    // fraction and a tick count can never overflow (code-core-9).
    const auto bounded = [](std::int64_t value) {
        return value <= max_parameter_magnitude && value >= -max_parameter_magnitude;
    };
    if (field->is_number_integer()) {
        std::int64_t value = 0;
        if (!read_int64(key, value))
            return;
        if (!bounded(value)) {
            fail(key, "fraction out of range");
            return;
        }
        out = Rational{value, 1};
        return;
    }
    if (!field->is_string()) {
        fail(key, "expected a fraction string such as \"1/16\"");
        return;
    }
    auto parsed = Rational::parse(field->get<std::string>());
    if (!parsed) {
        fail(key, parsed.error().message);
        return;
    }
    if (!bounded(parsed->numerator) || !bounded(parsed->denominator)) {
        fail(key, "fraction out of range");
        return;
    }
    out = *parsed;
}

void FieldReader::integer_list(std::string_view key, std::vector<std::int64_t>& out) {
    const auto* field = find(key);
    if (!field || !status_)
        return;
    if (!field->is_array()) {
        fail(key, "expected an array of integers");
        return;
    }
    std::vector<std::int64_t> values;
    for (const auto& item : *field) {
        if (!item.is_number_integer() || (item.is_number_unsigned() && !std::in_range<std::int64_t>(item.get<std::uint64_t>()))) {
            fail(key, "expected an array of integers");
            return;
        }
        const auto value = item.get<std::int64_t>();
        if (value > max_parameter_magnitude || value < -max_parameter_magnitude) {
            fail(key, "integer out of range"); // same bound as single integers (code-core-9)
            return;
        }
        values.push_back(value);
    }
    out = std::move(values);
}

void FieldReader::integer_list(std::string_view key, std::vector<int>& out) {
    std::vector<std::int64_t> wide;
    if (!present(key))
        return;
    integer_list(key, wide);
    if (!status_)
        return;
    std::vector<int> values;
    for (const auto value : wide) {
        if (!std::in_range<int>(value)) {
            fail(key, "integer out of range");
            return;
        }
        values.push_back(static_cast<int>(value));
    }
    out = std::move(values);
}

const nlohmann::json* FieldReader::object(std::string_view key) {
    const auto* field = find(key);
    if (!field || !status_)
        return nullptr;
    if (!field->is_object()) {
        fail(key, "expected an object");
        return nullptr;
    }
    return field;
}

const nlohmann::json* FieldReader::array(std::string_view key) {
    const auto* field = find(key);
    if (!field || !status_)
        return nullptr;
    if (!field->is_array()) {
        fail(key, "expected an array");
        return nullptr;
    }
    return field;
}

Result<nlohmann::json> parse_json_object(std::string_view text) {
    nlohmann::json value = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (value.is_discarded())
        return make_error(ErrorCode::invalid_data, "malformed JSON");
    if (!value.is_object())
        return make_error(ErrorCode::invalid_data, "expected a JSON object");
    return value;
}

} // namespace mwb::music
