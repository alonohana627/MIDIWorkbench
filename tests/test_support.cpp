// Shared doctest runner for every mwb test executable.
//
// `--mwb-require-tests` makes a run that executed no test case fail, so a CTest entry named after
// a suite that no longer exists cannot pass silently (specification section 32.2).

#define DOCTEST_CONFIG_IMPLEMENT
#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <atomic>
#include <cmath>
#include <numbers>
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

// --- SoundFont fixture ------------------------------------------------------------------------

namespace {

/// Appends little-endian values to a byte vector (SF2 fixture builder).
struct ByteWriter {
    std::vector<std::uint8_t> bytes;
    void u8(std::uint8_t value) { bytes.push_back(value); }
    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value & 0xFF));
        u8(static_cast<std::uint8_t>(value >> 8));
    }
    void u32(std::uint32_t value) {
        u16(static_cast<std::uint16_t>(value & 0xFFFF));
        u16(static_cast<std::uint16_t>(value >> 16));
    }
    void tag(const char* text) { bytes.insert(bytes.end(), text, text + 4); }
    void name(const char* text, std::size_t width) {
        const std::size_t length = std::min(std::strlen(text), width);
        bytes.insert(bytes.end(), text, text + length);
        bytes.insert(bytes.end(), width - length, 0);
    }
    void chunk(const char* id, const std::vector<std::uint8_t>& payload) {
        tag(id);
        u32(static_cast<std::uint32_t>(payload.size()));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        if (payload.size() % 2 != 0)
            u8(0);
    }
    void list(const char* type, const std::vector<std::uint8_t>& payload) {
        tag("LIST");
        u32(static_cast<std::uint32_t>(payload.size() + 4));
        tag(type);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }
};

/// Builds a minimal valid SoundFont 2 bank in memory: one looped sine sample, one instrument and
/// one preset (bank 0, program 0) named "Test Sine". Generated at test time; no bank is shipped.
std::vector<std::uint8_t> make_test_soundfont_bytes() {
    constexpr std::uint32_t sample_count = 2000;
    ByteWriter samples;
    for (std::uint32_t index = 0; index < sample_count + 46; ++index) {
        const double value = index < sample_count ? std::sin(2.0 * std::numbers::pi * index / 100.0) * 12000.0 : 0.0;
        samples.u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(value))));
    }
    ByteWriter info;
    {
        ByteWriter version;
        version.u16(2);
        version.u16(1);
        info.chunk("ifil", version.bytes);
        ByteWriter engine;
        engine.name("EMU8000", 8);
        info.chunk("isng", engine.bytes);
        ByteWriter title;
        title.name("MWB test", 10);
        info.chunk("INAM", title.bytes);
    }
    ByteWriter sdta;
    sdta.chunk("smpl", samples.bytes);

    ByteWriter pdta;
    ByteWriter phdr;
    phdr.name("Test Sine", 20);
    phdr.u16(0); // preset
    phdr.u16(0); // bank
    phdr.u16(0); // bag index
    phdr.u32(0);
    phdr.u32(0);
    phdr.u32(0);
    phdr.name("EOP", 20);
    phdr.u16(0);
    phdr.u16(0);
    phdr.u16(1);
    phdr.u32(0);
    phdr.u32(0);
    phdr.u32(0);
    pdta.chunk("phdr", phdr.bytes);
    ByteWriter pbag;
    pbag.u16(0);
    pbag.u16(0);
    pbag.u16(1);
    pbag.u16(0);
    pdta.chunk("pbag", pbag.bytes);
    ByteWriter pmod;
    for (int index = 0; index < 5; ++index)
        pmod.u16(0);
    pdta.chunk("pmod", pmod.bytes);
    ByteWriter pgen;
    pgen.u16(41); // instrument
    pgen.u16(0);
    pgen.u16(0);
    pgen.u16(0);
    pdta.chunk("pgen", pgen.bytes);
    ByteWriter inst;
    inst.name("Sine", 20);
    inst.u16(0);
    inst.name("EOI", 20);
    inst.u16(1);
    pdta.chunk("inst", inst.bytes);
    ByteWriter ibag;
    ibag.u16(0);
    ibag.u16(0);
    ibag.u16(2);
    ibag.u16(0);
    pdta.chunk("ibag", ibag.bytes);
    ByteWriter imod;
    for (int index = 0; index < 5; ++index)
        imod.u16(0);
    pdta.chunk("imod", imod.bytes);
    ByteWriter igen;
    igen.u16(54); // sampleModes: continuous loop
    igen.u16(1);
    igen.u16(53); // sampleID (last generator of the zone)
    igen.u16(0);
    igen.u16(0);
    igen.u16(0);
    pdta.chunk("igen", igen.bytes);
    ByteWriter shdr;
    shdr.name("Sine", 20);
    shdr.u32(0);
    shdr.u32(sample_count);
    shdr.u32(100);
    shdr.u32(1100);
    shdr.u32(44100);
    shdr.u8(69);
    shdr.u8(0);
    shdr.u16(0);
    shdr.u16(1);
    shdr.name("EOS", 20);
    for (int index = 0; index < 5; ++index)
        shdr.u32(0);
    shdr.u8(0);
    shdr.u8(0);
    shdr.u16(0);
    shdr.u16(0);
    pdta.chunk("shdr", shdr.bytes);

    ByteWriter body;
    body.list("INFO", info.bytes);
    body.list("sdta", sdta.bytes);
    body.list("pdta", pdta.bytes);
    ByteWriter file;
    file.tag("RIFF");
    file.u32(static_cast<std::uint32_t>(body.bytes.size() + 4));
    file.tag("sfbk");
    file.bytes.insert(file.bytes.end(), body.bytes.begin(), body.bytes.end());
    return file.bytes;
}

} // namespace

namespace mwb::test {

std::vector<std::uint8_t> make_test_soundfont() { return make_test_soundfont_bytes(); }

} // namespace mwb::test
