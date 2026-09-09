"""Add native Local gunshots and FPP vehicle lifecycle sounds for self labels."""
import re

import prepare_synthetic_sources as base
import prepare_motion_sources as moving


def choose_sources(banks):
    selected = moving.choose_sources(banks)
    for bank in sorted(banks, key=lambda b: b['ShortName']):
        name = bank['ShortName']
        counts = {}
        for media in sorted(bank.get('IncludedMemoryFiles', []), key=lambda m: m['ShortName']):
            path = media['ShortName']
            leaf = path.rsplit('\\', 1)[-1]
            role = None
            if (name.startswith('Weapons_') and name not in ['Weapons_Training', 'Weapons_Mortar']
                    and not re.search(r'(\\Old\\|Reflection|Indoor)', path, re.I)
                    and re.search(r'_Shot_Local_(?:(?:New|Remastered)_)?\d+\.wav$', leaf)):
                cls, mode, group, role = 2, 'shots', 'gunfire:' + name, 'local_shot'
            elif name in moving.VEHICLES:
                if re.search(r'_Startup_FPP_\d+\.wav$', leaf, re.I):
                    role = 'startup_fpp'
                elif re.search(r'_Shutdown_FPP_\d+\.wav$', leaf, re.I):
                    role = 'shutdown_fpp'
                cls, mode, group = 1, 'vehicle_event', 'vehicle:' + name
            if role is None or counts.get(role, 0) >= (4 if role == 'local_shot' else 2):
                continue
            counts[role] = counts.get(role, 0) + 1
            selected.append(dict(id=f"src_{bank['Id']}_{media['Id']}", class_index=cls,
                source_group_id=group, mode=mode, role=role, bank=name,
                bank_id=int(bank['Id']), media_id=int(media['Id']), source_name=path,
                source_semantics='native_Local_gunshot_or_FPP_vehicle_media'))
    return selected


if __name__ == '__main__':
    base.choose_sources = choose_sources
    base.main()
