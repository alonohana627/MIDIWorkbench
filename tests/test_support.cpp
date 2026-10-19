// Shared doctest runner for every mwb test executable.
//
// `--mwb-require-tests` makes a run that executed no test case fail, so a CTest entry named after
// a suite that no longer exists cannot pass silently (specification section 32.2).

#define DOCTEST_CONFIG_IMPLEMENT
#include "test_support.hpp"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace {

std::atomic<int> executed_test_cases{0};

struct CountingListener : doctest::IReporter {
    explicit CountingListener(const doctest::ContextOptions&) {}
    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_run_end(const doctest::TestRunStats&) override {}
    void test_case_start(const doctest::TestCaseData&) override { ++executed_test_cases; }
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats&) override {}
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}
};

REGISTER_LISTENER("mwb_counting", 1, CountingListener);

} // namespace

namespace mwb::test {

TemporaryDirectory::TemporaryDirectory() {
    std::random_device device;
    const auto base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto candidate = base / ("mwb-test-" + std::to_string(device()) + std::to_string(device()));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
            path_ = candidate;
            return;
        }
    }
    throw std::runtime_error("cannot create a temporary test directory");
}

TemporaryDirectory::~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
}

} // namespace mwb::test

int main(int argc, char** argv) {
    bool require_tests = false;
    std::vector<char*> arguments;
    for (int index = 0; index < argc; ++index) {
        if (std::strcmp(argv[index], "--mwb-require-tests") == 0)
            require_tests = true;
        else
            arguments.push_back(argv[index]);
    }
    doctest::Context context;
    context.applyCommandLine(static_cast<int>(arguments.size()), arguments.data());
    const int result = context.run();
    if (context.shouldExit())
        return result;
    if (require_tests && executed_test_cases.load() == 0) {
        std::fprintf(stderr, "error: the selected test suite matched no test cases\n");
        return 1;
    }
    return result;
}
