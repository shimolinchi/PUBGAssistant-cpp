#include "AiAimHostContext.hpp"

namespace pubg {
namespace {

float scopeZoomForAi(const std::string& scope) noexcept {
    if (scope == "2") return 2.0F;
    if (scope == "3") return 3.0F;
    if (scope == "4") return 4.0F;
    if (scope == "6") return 6.0F;
    if (scope == "8") return 8.0F;
    return 1.0F;
}

} // namespace

pubg::ai_aim::CombatContext buildAiAimCombatContext(
    const AiAimHostCombatSnapshot& host) {
    pubg::ai_aim::CombatContext context;
    context.weapon = host.weapon_supported ? host.weapon : std::string{};
    context.stance = host.stance;
    context.crouched = host.stance == "squat";
    context.ads = host.ads;
    context.firing = host.firing;
    context.lean_left = host.lean_left;
    context.lean_right = host.lean_right;

    if (const auto matched = recognizedEquipmentForWeapon(
            host.equipment, host.weapon)) {
        context.scope = matched->scope;
        context.grip = matched->grip;
        context.muzzle = matched->muzzle;
        context.stock = matched->stock;
        context.scope_zoom = scopeZoomForAi(context.scope);
    }

    if (host.disable_variable_high_zoom &&
        (context.scope == "6" || context.scope == "8" ||
         context.scope == "multiple")) {
        context.weapon.clear();
    }
    return context;
}

} // namespace pubg
