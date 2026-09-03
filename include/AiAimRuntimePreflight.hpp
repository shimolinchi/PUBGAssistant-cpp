#pragma once

#include <filesystem>
#include <stop_token>
#include <string>

namespace pubg {

struct AiAimRuntimeFiles {
    std::filesystem::path directory;
    std::filesystem::path onnx_runtime;
    std::filesystem::path directml_runtime;
    std::filesystem::path model;
    std::filesystem::path checksums;
};

enum class AiAimDirectMlPolicy {
    Disabled,
    Optional,
    Required,
};

struct AiAimRuntimePreflightResult {
    bool ready = false;
    bool cancelled = false;
    bool directml_available = false;
    std::string warning;
    std::string error;
};

[[nodiscard]] AiAimRuntimeFiles locateAiAimRuntimeFiles(
    std::filesystem::path runtime_directory);

// Performs bounded checks against the host's compiled asset contract. It does not
// load a DLL, create a graphics device, or initialize an ONNX Runtime session.
// This is a startup compatibility/integrity check, not a security boundary against
// an attacker replacing files concurrently after validation.
[[nodiscard]] AiAimRuntimePreflightResult validateAiAimRuntime(
    const AiAimRuntimeFiles& files,
    AiAimDirectMlPolicy directml_policy,
    std::stop_token stop = {});

} // namespace pubg
