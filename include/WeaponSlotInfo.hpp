#pragma once

#include <string>

namespace pubg {

// 装备栏中一个武器槽位的识别结果。
// name/scope/grip/muzzle/stock 与 EquipmentDetector 返回的字段保持一致。
struct WeaponSlotInfo {
    std::string name;
    double name_score = 0.0;
    std::string scope;
    double scope_score = 0.0;
    std::string grip;
    double grip_score = 0.0;
    std::string muzzle;
    double muzzle_score = 0.0;
    std::string stock;
    double stock_score = 0.0;
};

} // namespace pubg
