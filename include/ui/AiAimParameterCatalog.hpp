#pragma once

#include <span>

namespace pubg::ui {

enum class AiAimParameterPage { Target, Control, Firing, Prediction };
enum class AiAimParameterKind { Number, Check };

// One UI definition supplies controls, bounds and detailed help. Defaults still
// come from AiAimSettings, and JSON mapping remains owned by the host parser.
struct AiAimParameterSpec {
    AiAimParameterPage page;
    const char* path;
    const char* title;
    AiAimParameterKind kind;
    double minimum;
    double maximum;
    double step;
    int decimals;
    const char* suffix;
    const char* purpose;
    const char* adjustment;
    const char* caution;
};

[[nodiscard]] std::span<const AiAimParameterSpec> aiAimParameterCatalog() noexcept;

} // namespace pubg::ui
