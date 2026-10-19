#pragma once

#include <string_view>
#include <vector>

namespace mwb::app {

/// One vendored dependency with its license text, generated from deps/manifest.json.
struct ThirdPartyNotice {
    std::string_view name;
    std::string_view version;
    std::string_view license;     ///< SPDX expression.
    std::string_view homepage;
    std::string_view purpose;
    std::string_view license_text;
};

/// Every vendored library in manifest order (About box, `mwb-cli capabilities`).
[[nodiscard]] const std::vector<ThirdPartyNotice>& third_party_notices();

} // namespace mwb::app
