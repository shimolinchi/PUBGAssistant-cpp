#include "AiAimRuntimePreflight.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr const char* kCanonicalManifest =
    "950BF49B9AA6F2D9230643D3831ED8CAA72BC1BC08FA8163FEA9B636BDD21A1C  DirectML.dll\n"
    "E795F0AC23E9ACF7023CAD1149F59AF7DA35FE1C84023CFA3128E4A9BC179E0A  PUBGV8.onnx\n"
    "5421F8789D3D5945AE7083247AF73152F76AC2F4CC64863029B699634BF512C4  onnxruntime.dll\n";

void require(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TemporaryRuntimeRoot {
public:
    TemporaryRuntimeRoot() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
            ("pubg_ai_runtime_test_" + std::to_string(nonce));
        std::filesystem::create_directories(root_);
    }

    ~TemporaryRuntimeRoot() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return root_; }

private:
    std::filesystem::path root_;
};

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.good(), "failed to create temporary runtime file");
    output << text;
    require(output.good(), "failed to write temporary runtime file");
}

pubg::AiAimRuntimeFiles checkedInRuntimeFiles() {
    return pubg::locateAiAimRuntimeFiles(
        std::filesystem::path(PUBG_TEST_SOURCE_DIR) /
        "modules" / "pubg_ai_aim" / "runtime");
}

void copyAndModifyLastByte(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    std::filesystem::copy_file(
        source, destination, std::filesystem::copy_options::overwrite_existing);
    std::fstream file(destination, std::ios::binary | std::ios::in | std::ios::out);
    require(file.good(), "failed to open copied runtime asset");
    file.seekg(-1, std::ios::end);
    char value = 0;
    file.read(&value, 1);
    require(file.good(), "failed to read copied runtime asset");
    value = static_cast<char>(static_cast<unsigned char>(value) ^ 0x5aU);
    file.seekp(-1, std::ios::end);
    file.write(&value, 1);
    require(file.good(), "failed to modify copied runtime asset");
}

void writeMinimalPe(
    const std::filesystem::path& path,
    const std::uint16_t machine = 0x8664,
    const std::uint16_t optional_magic = 0x020b,
    const bool is_dll = true) {
    std::vector<unsigned char> bytes(512, 0);
    bytes[0] = 'M';
    bytes[1] = 'Z';
    bytes[0x3c] = 0x40;
    bytes[0x40] = 'P';
    bytes[0x41] = 'E';
    bytes[0x44] = static_cast<unsigned char>(machine & 0xffU);
    bytes[0x45] = static_cast<unsigned char>((machine >> 8U) & 0xffU);
    bytes[0x46] = 1;
    bytes[0x54] = 0xf0;
    if (is_dll) {
        bytes[0x57] = 0x20;
    }
    bytes[0x58] = static_cast<unsigned char>(optional_magic & 0xffU);
    bytes[0x59] = static_cast<unsigned char>((optional_magic >> 8U) & 0xffU);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.good(), "failed to create temporary PE file");
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "failed to write temporary PE file");
}

void requireRuntimeBundlePasses(const std::filesystem::path& runtime_directory) {
    const auto files = pubg::locateAiAimRuntimeFiles(runtime_directory);
    const auto result = pubg::validateAiAimRuntime(
        files, pubg::AiAimDirectMlPolicy::Required);
    require(result.ready, result.error.c_str());
    require(!result.cancelled, "valid runtime validation was marked cancelled");
    require(result.directml_available, "valid DirectML runtime was not reported available");
    require(result.warning.empty(), "valid runtime validation produced a warning");
}

void testCheckedInRuntimeBundlePasses() {
    requireRuntimeBundlePasses(checkedInRuntimeFiles().directory);
}

void testMissingFilesFailBeforeRuntimeLoading() {
    TemporaryRuntimeRoot root;
    const auto files = pubg::locateAiAimRuntimeFiles(root.path());
    const auto result = pubg::validateAiAimRuntime(
        files, pubg::AiAimDirectMlPolicy::Required);
    require(!result.ready, "missing runtime bundle was accepted");
    require(result.error.find("PUBGV8.onnx") != std::string::npos,
            "missing model did not produce a specific error");
}

void testNonPeRuntimeIsRejected() {
    TemporaryRuntimeRoot root;
    const auto files = pubg::locateAiAimRuntimeFiles(root.path());
    writeText(files.model, "abc");
    writeText(files.onnx_runtime, "abc");
    writeText(files.checksums,
              "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD  PUBGV8.onnx\n"
              "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD  onnxruntime.dll\n");

    const auto result = pubg::validateAiAimRuntime(
        files, pubg::AiAimDirectMlPolicy::Disabled);
    require(!result.ready, "non-PE ONNX Runtime DLL was accepted");
    require(result.error.find("PE") != std::string::npos,
            "non-PE DLL did not produce an architecture error");
}

void testChangedOnnxRuntimeIsRejectedByCompiledContract() {
    TemporaryRuntimeRoot root;
    auto files = checkedInRuntimeFiles();
    files.onnx_runtime = root.path() / "onnxruntime.dll";
    copyAndModifyLastByte(
        checkedInRuntimeFiles().onnx_runtime, files.onnx_runtime);

    const auto result = pubg::validateAiAimRuntime(
        files, pubg::AiAimDirectMlPolicy::Disabled);
    require(!result.ready, "modified ONNX Runtime DLL was accepted");
    require(result.error.find("SHA-256 does not match") != std::string::npos,
            "modified ONNX Runtime DLL did not reach the compiled hash check");
}

void testCancellationIsReported() {
    std::stop_source stop;
    stop.request_stop();
    const auto files = pubg::locateAiAimRuntimeFiles("unused");
    const auto result = pubg::validateAiAimRuntime(
        files, pubg::AiAimDirectMlPolicy::Required, stop.get_token());
    require(!result.ready && result.cancelled,
            "pre-cancelled runtime validation did not stop");
}

void testPeArchitectureAndDllContractAreRejectedEarly() {
    TemporaryRuntimeRoot root;
    const auto files = pubg::locateAiAimRuntimeFiles(root.path());
    writeText(files.model, "abc");
    writeText(files.checksums, "manifest");

    writeMinimalPe(files.onnx_runtime, 0x014c);
    auto result = pubg::validateAiAimRuntime(
        files, pubg::AiAimDirectMlPolicy::Disabled);
    require(!result.ready && result.error.find("not x64") != std::string::npos,
            "x86 ONNX Runtime DLL was accepted");

    writeMinimalPe(files.onnx_runtime, 0x8664, 0x010b);
    result = pubg::validateAiAimRuntime(
        files, pubg::AiAimDirectMlPolicy::Disabled);
    require(!result.ready && result.error.find("PE32+") != std::string::npos,
            "PE32 ONNX Runtime DLL was accepted");

    writeMinimalPe(files.onnx_runtime, 0x8664, 0x020b, false);
    result = pubg::validateAiAimRuntime(
        files, pubg::AiAimDirectMlPolicy::Disabled);
    require(!result.ready && result.error.find("DLL characteristic") != std::string::npos,
            "non-DLL PE image was accepted as ONNX Runtime");

    writeMinimalPe(files.onnx_runtime);
    std::filesystem::resize_file(files.onnx_runtime, 300);
    result = pubg::validateAiAimRuntime(
        files, pubg::AiAimDirectMlPolicy::Disabled);
    require(!result.ready && result.error.find("section table") != std::string::npos,
            "truncated PE section table was accepted");
}

void testOptionalDirectMlFallsBackButRequiredDirectMlFails() {
    auto files = checkedInRuntimeFiles();
    TemporaryRuntimeRoot root;
    files.directml_runtime = root.path() / "missing-DirectML.dll";

    const auto optional = pubg::validateAiAimRuntime(
        files, pubg::AiAimDirectMlPolicy::Optional);
    require(optional.ready, optional.error.c_str());
    require(!optional.directml_available,
            "missing optional DirectML runtime was reported available");
    require(optional.warning.find("CPU fallback") != std::string::npos,
            "optional DirectML failure did not report CPU fallback");

    const auto required = pubg::validateAiAimRuntime(
        files, pubg::AiAimDirectMlPolicy::Required);
    require(!required.ready, "missing required DirectML runtime was accepted");
    require(required.warning.empty(), "required DirectML failure was downgraded to warning");
}

void testChangedDirectMlFallsBackOnlyWhenPolicyAllowsIt() {
    TemporaryRuntimeRoot root;
    auto files = checkedInRuntimeFiles();
    files.directml_runtime = root.path() / "DirectML.dll";
    copyAndModifyLastByte(
        checkedInRuntimeFiles().directml_runtime, files.directml_runtime);

    const auto optional = pubg::validateAiAimRuntime(
        files, pubg::AiAimDirectMlPolicy::Optional);
    require(optional.ready && !optional.directml_available,
            "modified optional DirectML runtime did not fall back to CPU");
    require(optional.warning.find("SHA-256 does not match") != std::string::npos,
            "modified DirectML runtime did not reach its compiled hash check");
}

void testAdjacentManifestCannotRedefineHostContract() {
    TemporaryRuntimeRoot root;
    auto files = checkedInRuntimeFiles();
    files.checksums = root.path() / "SHA256SUMS.txt";
    writeText(
        files.checksums,
        "950BF49B9AA6F2D9230643D3831ED8CAA72BC1BC08FA8163FEA9B636BDD21A1C  DirectML.dll\n"
        "E795F0AC23E9ACF7023CAD1149F59AF7DA35FE1C84023CFA3128E4A9BC179E0A  PUBGV8.onnx\n"
        "0000000000000000000000000000000000000000000000000000000000000000  onnxruntime.dll\n");

    const auto result = pubg::validateAiAimRuntime(
        files, pubg::AiAimDirectMlPolicy::Disabled);
    require(!result.ready, "modified adjacent manifest redefined the host contract");
    require(result.error.find("host asset contract") != std::string::npos,
            "modified manifest did not report a host contract mismatch");
}

void testManifestParserIsStrictAndBounded() {
    struct ManifestCase final {
        std::string contents;
        const char* expected_error;
    };
    const std::vector<ManifestCase> cases{
        {
            "Z501F8789D3D5945AE7083247AF73152F76AC2F4CC64863029B699634BF512C4  onnxruntime.dll\n",
            "Invalid SHA-256",
        },
        {
            std::string(kCanonicalManifest) +
                "5421F8789D3D5945AE7083247AF73152F76AC2F4CC64863029B699634BF512C4  onnxruntime.dll\n",
            "Duplicate",
        },
        {
            "950BF49B9AA6F2D9230643D3831ED8CAA72BC1BC08FA8163FEA9B636BDD21A1C  ../DirectML.dll\n",
            "must not contain a path",
        },
        {
            "E795F0AC23E9ACF7023CAD1149F59AF7DA35FE1C84023CFA3128E4A9BC179E0A  PUBGV8.onnx\n"
            "5421F8789D3D5945AE7083247AF73152F76AC2F4CC64863029B699634BF512C4  onnxruntime.dll\n",
            "three required assets",
        },
        {
            "950BF49B9AA6F2D9230643D3831ED8CAA72BC1BC08FA8163FEA9B636BDD21A1C  unexpected.dll\n",
            "Unknown",
        },
        {
            std::string(16U * 1024U + 1U, 'x'),
            "size limit",
        },
    };

    for (const auto& test_case : cases) {
        TemporaryRuntimeRoot root;
        auto files = checkedInRuntimeFiles();
        files.checksums = root.path() / "SHA256SUMS.txt";
        writeText(files.checksums, test_case.contents);
        const auto result = pubg::validateAiAimRuntime(
            files, pubg::AiAimDirectMlPolicy::Disabled);
        require(!result.ready, "malformed AI runtime manifest was accepted");
        require(result.error.find(test_case.expected_error) != std::string::npos,
                "malformed manifest produced the wrong validation error");
    }
}

void testCancellationInterruptsStreamingHash() {
    std::stop_source stop;
    std::jthread canceller([&stop] {
        std::this_thread::sleep_for(5ms);
        stop.request_stop();
    });
    const auto result = pubg::validateAiAimRuntime(
        checkedInRuntimeFiles(),
        pubg::AiAimDirectMlPolicy::Required,
        stop.get_token());
    require(!result.ready && result.cancelled,
            "streaming runtime hash did not observe cancellation");
}

} // namespace

int main(const int argc, char** argv) {
    try {
        testCheckedInRuntimeBundlePasses();
        testMissingFilesFailBeforeRuntimeLoading();
        testNonPeRuntimeIsRejected();
        testChangedOnnxRuntimeIsRejectedByCompiledContract();
        testCancellationIsReported();
        testPeArchitectureAndDllContractAreRejectedEarly();
        testOptionalDirectMlFallsBackButRequiredDirectMlFails();
        testChangedDirectMlFallsBackOnlyWhenPolicyAllowsIt();
        testAdjacentManifestCannotRedefineHostContract();
        testManifestParserIsStrictAndBounded();
        testCancellationInterruptsStreamingHash();
        if (argc == 2) {
            requireRuntimeBundlePasses(argv[1]);
        } else if (argc > 2) {
            throw std::runtime_error("expected at most one runtime directory argument");
        }
        std::cout << "AiAim runtime preflight tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AiAim runtime preflight tests failed: " << error.what() << '\n';
        return 1;
    }
}
