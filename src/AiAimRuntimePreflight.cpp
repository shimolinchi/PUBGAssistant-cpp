#include "AiAimRuntimePreflight.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace pubg {
namespace {

struct RuntimeAssetContract final {
    std::string_view filename;
    std::uintmax_t size;
    std::string_view sha256;
};

// These values are the host's versioned runtime/model identity. The adjacent
// manifest is checked for packaging consistency, but is never the trust root.
constexpr RuntimeAssetContract kModelContract{
    "PUBGV8.onnx",
    44'594'265,
    "E795F0AC23E9ACF7023CAD1149F59AF7DA35FE1C84023CFA3128E4A9BC179E0A",
};
constexpr RuntimeAssetContract kOnnxRuntimeContract{
    "onnxruntime.dll",
    12'632'456,
    "5421F8789D3D5945AE7083247AF73152F76AC2F4CC64863029B699634BF512C4",
};
constexpr RuntimeAssetContract kDirectMlContract{
    "DirectML.dll",
    11'681'696,
    "950BF49B9AA6F2D9230643D3831ED8CAA72BC1BC08FA8163FEA9B636BDD21A1C",
};
constexpr std::array<RuntimeAssetContract, 3> kRuntimeContracts{
    kModelContract,
    kOnnxRuntimeContract,
    kDirectMlContract,
};

constexpr std::uintmax_t kMaximumManifestBytes = 16U * 1024U;
constexpr std::size_t kMaximumManifestLines = 64;
constexpr std::size_t kMaximumManifestLineBytes = 256;

AiAimRuntimePreflightResult failure(std::string error) {
    AiAimRuntimePreflightResult result;
    result.error = std::move(error);
    return result;
}

AiAimRuntimePreflightResult cancellation() {
    AiAimRuntimePreflightResult result;
    result.cancelled = true;
    result.error = "AI runtime validation was cancelled";
    return result;
}

AiAimRuntimePreflightResult success(
    const bool directml_available,
    std::string warning = {}) {
    AiAimRuntimePreflightResult result;
    result.ready = true;
    result.directml_available = directml_available;
    result.warning = std::move(warning);
    return result;
}

bool isRegularNonEmptyFile(const std::filesystem::path& path, std::string& error) {
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(path, filesystem_error)) {
        error = "AI runtime file is missing or not regular: " + path.string();
        return false;
    }
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || size == 0) {
        error = "AI runtime file is empty or unreadable: " + path.string();
        return false;
    }
    return true;
}

std::uint32_t readLittleEndian32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint16_t readLittleEndian16(const unsigned char* bytes) {
    return static_cast<std::uint16_t>(bytes[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

bool hasExpectedX64DllPeHeaders(const std::filesystem::path& path, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Cannot read AI runtime DLL: " + path.string();
        return false;
    }

    std::array<unsigned char, 64> dos_header{};
    input.read(
        reinterpret_cast<char*>(dos_header.data()),
        static_cast<std::streamsize>(dos_header.size()));
    if (input.gcount() != static_cast<std::streamsize>(dos_header.size()) ||
        dos_header[0] != 'M' || dos_header[1] != 'Z') {
        error = "AI runtime DLL is not a valid PE file: " + path.string();
        return false;
    }

    const std::uint32_t pe_offset = readLittleEndian32(dos_header.data() + 0x3c);
    std::error_code filesystem_error;
    const auto file_size = std::filesystem::file_size(path, filesystem_error);
    constexpr std::size_t pe_header_bytes = 26;
    if (filesystem_error || pe_offset > file_size ||
        file_size - pe_offset < pe_header_bytes) {
        error = "AI runtime DLL has an invalid PE header offset: " + path.string();
        return false;
    }

    input.seekg(static_cast<std::streamoff>(pe_offset), std::ios::beg);
    std::array<unsigned char, pe_header_bytes> pe_header{};
    input.read(
        reinterpret_cast<char*>(pe_header.data()),
        static_cast<std::streamsize>(pe_header.size()));
    if (input.gcount() != static_cast<std::streamsize>(pe_header.size()) ||
        pe_header[0] != 'P' || pe_header[1] != 'E' ||
        pe_header[2] != 0 || pe_header[3] != 0) {
        error = "AI runtime DLL has an invalid PE signature: " + path.string();
        return false;
    }

    constexpr std::uint16_t image_file_machine_amd64 = 0x8664;
    const std::uint16_t machine = readLittleEndian16(pe_header.data() + 4);
    if (machine != image_file_machine_amd64) {
        error = "AI runtime DLL is not x64: " + path.string();
        return false;
    }

    const std::uint16_t section_count = readLittleEndian16(pe_header.data() + 6);
    if (section_count == 0 || section_count > 96) {
        error = "AI runtime DLL has an invalid PE section count: " + path.string();
        return false;
    }

    constexpr std::uint16_t minimum_pe32_plus_optional_header_size = 0x00f0;
    const std::uint16_t optional_header_size = readLittleEndian16(pe_header.data() + 20);
    if (optional_header_size < minimum_pe32_plus_optional_header_size) {
        error = "AI runtime DLL has a truncated PE32+ optional header: " + path.string();
        return false;
    }

    constexpr std::uintmax_t coff_header_bytes = 24;
    constexpr std::uintmax_t section_header_bytes = 40;
    const std::uintmax_t required_header_bytes = coff_header_bytes +
        optional_header_size + static_cast<std::uintmax_t>(section_count) * section_header_bytes;
    if (file_size - pe_offset < required_header_bytes) {
        error = "AI runtime DLL has a truncated PE section table: " + path.string();
        return false;
    }

    constexpr std::uint16_t image_file_dll = 0x2000;
    const std::uint16_t characteristics = readLittleEndian16(pe_header.data() + 22);
    if ((characteristics & image_file_dll) == 0) {
        error = "AI runtime PE file does not have the DLL characteristic: " + path.string();
        return false;
    }

    constexpr std::uint16_t pe32_plus_magic = 0x020b;
    if (readLittleEndian16(pe_header.data() + 24) != pe32_plus_magic) {
        error = "AI runtime DLL is not PE32+: " + path.string();
        return false;
    }
    return true;
}

bool isHexDigest(const std::string& value) {
    return value.size() == 64 && std::all_of(
        value.begin(), value.end(),
        [](const unsigned char character) { return std::isxdigit(character) != 0; });
}

std::string uppercase(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](const unsigned char character) { return static_cast<char>(std::toupper(character)); });
    return value;
}

bool readChecksumManifest(
    const std::filesystem::path& path,
    const std::stop_token stop,
    std::unordered_map<std::string, std::string>& checksums,
    std::string& error) {
    std::error_code filesystem_error;
    const auto manifest_size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        error = "Cannot read AI runtime checksum manifest size: " + path.string();
        return false;
    }
    if (manifest_size > kMaximumManifestBytes) {
        error = "AI runtime checksum manifest exceeds the size limit: " + path.string();
        return false;
    }

    std::ifstream input(path);
    if (!input) {
        error = "Cannot read AI runtime checksum manifest: " + path.string();
        return false;
    }

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        if (stop.stop_requested()) {
            error = "AI runtime validation was cancelled";
            return false;
        }
        ++line_number;
        if (line_number > kMaximumManifestLines) {
            error = "AI runtime checksum manifest has too many lines";
            return false;
        }
        if (line.size() > kMaximumManifestLineBytes) {
            error = "AI runtime checksum manifest line is too long: " +
                std::to_string(line_number);
            return false;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.size() < 66) {
            error = "Malformed AI runtime checksum manifest line " +
                std::to_string(line_number);
            return false;
        }

        std::string digest = line.substr(0, 64);
        if (!isHexDigest(digest)) {
            error = "Invalid SHA-256 digest on AI runtime manifest line " +
                std::to_string(line_number);
            return false;
        }

        std::size_t filename_start = 64;
        while (filename_start < line.size() && line[filename_start] == ' ') {
            ++filename_start;
        }
        if (filename_start < line.size() && line[filename_start] == '*') {
            ++filename_start;
        }
        if (filename_start >= line.size()) {
            error = "Missing filename on AI runtime manifest line " +
                std::to_string(line_number);
            return false;
        }

        const std::string filename = line.substr(filename_start);
        if (filename.find('/') != std::string::npos ||
            filename.find('\\') != std::string::npos ||
            filename == "." || filename == "..") {
            error = "AI runtime manifest filename must not contain a path: " + filename;
            return false;
        }
        const bool known_filename = std::any_of(
            kRuntimeContracts.begin(), kRuntimeContracts.end(),
            [&filename](const RuntimeAssetContract& contract) {
                return contract.filename == filename;
            });
        if (!known_filename) {
            error = "Unknown AI runtime checksum entry: " + filename;
            return false;
        }
        if (!checksums.emplace(filename, uppercase(std::move(digest))).second) {
            error = "Duplicate AI runtime checksum entry: " + filename;
            return false;
        }
        if (checksums.size() > kRuntimeContracts.size()) {
            error = "AI runtime checksum manifest has too many entries";
            return false;
        }
    }
    if (!input.eof()) {
        error = "Failed while reading AI runtime checksum manifest: " + path.string();
        return false;
    }
    return true;
}

bool manifestMatchesHostContract(
    const std::unordered_map<std::string, std::string>& checksums,
    std::string& error) {
    if (checksums.size() != kRuntimeContracts.size()) {
        error = "AI runtime checksum manifest does not contain the three required assets";
        return false;
    }
    for (const auto& contract : kRuntimeContracts) {
        const auto entry = checksums.find(std::string(contract.filename));
        if (entry == checksums.end()) {
            error = "AI runtime checksum is missing for: " + std::string(contract.filename);
            return false;
        }
        if (entry->second != contract.sha256) {
            error = "AI runtime manifest does not match the host asset contract: " +
                std::string(contract.filename);
            return false;
        }
    }
    return true;
}

#ifdef _WIN32

class BCryptAlgorithm final {
public:
    BCryptAlgorithm() = default;
    BCryptAlgorithm(const BCryptAlgorithm&) = delete;
    BCryptAlgorithm& operator=(const BCryptAlgorithm&) = delete;
    BCryptAlgorithm(BCryptAlgorithm&&) = delete;
    BCryptAlgorithm& operator=(BCryptAlgorithm&&) = delete;

    ~BCryptAlgorithm() {
        if (handle_) {
            BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }

    BCRYPT_ALG_HANDLE* output() noexcept { return &handle_; }
    BCRYPT_ALG_HANDLE get() const noexcept { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class BCryptHash final {
public:
    BCryptHash() = default;
    BCryptHash(const BCryptHash&) = delete;
    BCryptHash& operator=(const BCryptHash&) = delete;
    BCryptHash(BCryptHash&&) = delete;
    BCryptHash& operator=(BCryptHash&&) = delete;

    ~BCryptHash() {
        if (handle_) {
            BCryptDestroyHash(handle_);
        }
    }

    BCRYPT_HASH_HANDLE* output() noexcept { return &handle_; }
    BCRYPT_HASH_HANDLE get() const noexcept { return handle_; }

private:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};

bool bcryptSucceeded(const NTSTATUS status) noexcept {
    return status >= 0;
}

bool sha256File(
    const std::filesystem::path& path,
    const std::stop_token stop,
    std::string& digest,
    std::string& error) {
    BCryptAlgorithm algorithm;
    if (!bcryptSucceeded(BCryptOpenAlgorithmProvider(
            algorithm.output(), BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        error = "Cannot initialize Windows SHA-256 provider";
        return false;
    }

    DWORD object_length = 0;
    DWORD hash_length = 0;
    DWORD bytes_written = 0;
    if (!bcryptSucceeded(BCryptGetProperty(
            algorithm.get(), BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length),
            &bytes_written, 0)) || object_length == 0) {
        error = "Cannot query Windows SHA-256 object size";
        return false;
    }
    if (!bcryptSucceeded(BCryptGetProperty(
            algorithm.get(), BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hash_length), sizeof(hash_length),
            &bytes_written, 0)) || hash_length != 32) {
        error = "Windows SHA-256 provider returned an unexpected digest size";
        return false;
    }

    std::vector<unsigned char> hash_object(object_length);
    std::vector<unsigned char> hash_bytes(hash_length);
    BCryptHash hash;
    if (!bcryptSucceeded(BCryptCreateHash(
            algorithm.get(), hash.output(), hash_object.data(), object_length,
            nullptr, 0, 0))) {
        error = "Cannot create Windows SHA-256 hash";
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Cannot read AI runtime file for SHA-256: " + path.string();
        return false;
    }
    std::vector<unsigned char> buffer(1024U * 1024U);
    while (input) {
        if (stop.stop_requested()) {
            error = "AI runtime validation was cancelled";
            return false;
        }
        input.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0 && !bcryptSucceeded(BCryptHashData(
                hash.get(), buffer.data(), static_cast<ULONG>(count), 0))) {
            error = "Windows SHA-256 failed while reading: " + path.string();
            return false;
        }
    }
    if (!input.eof()) {
        error = "Failed while reading AI runtime file: " + path.string();
        return false;
    }
    if (!bcryptSucceeded(BCryptFinishHash(
            hash.get(), hash_bytes.data(), hash_length, 0))) {
        error = "Windows SHA-256 failed to finish: " + path.string();
        return false;
    }

    std::ostringstream formatted;
    formatted << std::uppercase << std::hex << std::setfill('0');
    for (const unsigned char value : hash_bytes) {
        formatted << std::setw(2) << static_cast<unsigned>(value);
    }
    digest = formatted.str();
    return true;
}

#else

bool sha256File(
    const std::filesystem::path&,
    std::stop_token,
    std::string&,
    std::string& error) {
    error = "AI runtime SHA-256 validation is only available on Windows";
    return false;
}

#endif

bool validateAssetIdentity(
    const std::filesystem::path& path,
    const RuntimeAssetContract& contract,
    const std::stop_token stop,
    std::string& error) {
    std::error_code filesystem_error;
    const auto actual_size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || actual_size != contract.size) {
        error = "AI runtime asset size does not match the host contract: " + path.string();
        return false;
    }

    std::string actual_digest;
    if (!sha256File(path, stop, actual_digest, error)) {
        return false;
    }
    if (actual_digest != contract.sha256) {
        error = "AI runtime SHA-256 does not match the host contract: " + path.string();
        return false;
    }
    return true;
}

} // namespace

AiAimRuntimeFiles locateAiAimRuntimeFiles(std::filesystem::path runtime_directory) {
    runtime_directory = std::filesystem::absolute(std::move(runtime_directory));
    return AiAimRuntimeFiles{
        runtime_directory,
        runtime_directory / "onnxruntime.dll",
        runtime_directory / "DirectML.dll",
        runtime_directory / "PUBGV8.onnx",
        runtime_directory / "SHA256SUMS.txt",
    };
}

AiAimRuntimePreflightResult validateAiAimRuntime(
    const AiAimRuntimeFiles& files,
    const AiAimDirectMlPolicy directml_policy,
    const std::stop_token stop) {
    if (stop.stop_requested()) {
        return cancellation();
    }

    std::string error;
    const std::array<const std::filesystem::path*, 3> always_required{
        &files.model,
        &files.onnx_runtime,
        &files.checksums,
    };
    for (const auto* path : always_required) {
        if (!isRegularNonEmptyFile(*path, error)) {
            return failure(std::move(error));
        }
    }
    if (!hasExpectedX64DllPeHeaders(files.onnx_runtime, error)) {
        return failure(std::move(error));
    }

    bool directml_headers_valid = false;
    std::string directml_error;
    if (directml_policy != AiAimDirectMlPolicy::Disabled) {
        directml_headers_valid =
            isRegularNonEmptyFile(files.directml_runtime, directml_error) &&
            hasExpectedX64DllPeHeaders(files.directml_runtime, directml_error);
        if (!directml_headers_valid &&
            directml_policy == AiAimDirectMlPolicy::Required) {
            return failure(std::move(directml_error));
        }
    }

    std::unordered_map<std::string, std::string> expected_checksums;
    if (!readChecksumManifest(files.checksums, stop, expected_checksums, error)) {
        if (stop.stop_requested()) {
            return cancellation();
        }
        return failure(std::move(error));
    }
    if (!manifestMatchesHostContract(expected_checksums, error)) {
        return failure(std::move(error));
    }

    const std::array<std::pair<const std::filesystem::path*, const RuntimeAssetContract*>, 2>
        core_assets{{
            {&files.model, &kModelContract},
            {&files.onnx_runtime, &kOnnxRuntimeContract},
        }};
    for (const auto& [path, contract] : core_assets) {
        if (!validateAssetIdentity(*path, *contract, stop, error)) {
            if (stop.stop_requested()) {
                return cancellation();
            }
            return failure(std::move(error));
        }
    }

    if (directml_policy == AiAimDirectMlPolicy::Disabled) {
        return success(false);
    }

    if (!directml_headers_valid ||
        !validateAssetIdentity(
            files.directml_runtime, kDirectMlContract, stop, directml_error)) {
        if (stop.stop_requested()) {
            return cancellation();
        }
        if (directml_policy == AiAimDirectMlPolicy::Required) {
            return failure(std::move(directml_error));
        }
        return success(false, "DirectML unavailable; using CPU fallback: " + directml_error);
    }

    return success(true);
}

} // namespace pubg
