"""Select native gait, vehicle lifecycle and tyre layers from the local client.

Reuses the verified v1 read-only extractor and decoder. REV .model assets are
catalogued, not mislabelled as decodable recordings of acceleration.
"""
import re

import prepare_synthetic_sources as base


VEHICLES = {
    'Vehicle_DaciaBank', 'Vehicle_BuggyBank', 'Vehicle_UAZBank',
    'Vehicle_PickupTruck', 'Vehicle_Rony', 'Vehicle_Sedan',
    'Vehicle_CoupeRB', 'Vehicle_Mirado', 'Vehicle_Minibus',
}
SURFACES = {'Concrete', 'Dirt', 'Fabric', 'Rock', 'Snow'}


def choose_sources(banks):
    selected = []
    for bank in sorted(banks, key=lambda b: b['ShortName']):
        name = bank['ShortName']
        counts = {}
        for media in sorted(bank.get('IncludedMemoryFiles', []), key=lambda m: m['ShortName']):
            path = media['ShortName']
            leaf = path.rsplit('\\', 1)[-1]
            if not path.lower().endswith('.wav'):
                continue
            role = None
            gait = re.search(r'(Walk|Run|Sprint)_([A-Za-z]+)_(Close|Normal)_\d+\.wav$', path)
            if name == 'FootStepsBank' and 'FootSteps_NEW\\Stand_' in path and gait and gait[2] in SURFACES:
                cls, group, mode, role = 0, 'footsteps:' + gait[2], 'steps', gait[1].lower()
            elif name in VEHICLES and not re.search(r'(FPP|Horn)', path, re.I):
                if re.search(r'Idle', leaf, re.I):
                    role = 'idle'
                elif re.search(r'Startup.*TPP', leaf, re.I):
                    role = 'startup'
                elif re.search(r'Shutdown.*TPP', leaf, re.I):
                    role = 'shutdown'
                if role:
                    if counts.get(role, 0) >= (1 if role == 'idle' else 2):
                        continue
                    counts[role] = counts.get(role, 0) + 1
                    cls, group, mode = 1, 'vehicle:' + name, 'engine' if role == 'idle' else 'vehicle_event'
            elif name == 'Vehicle_Surface':
                match = re.search(r'Vehicle_Surface_(Roll|Brake)_(Dirt|Concrete)_01\.wav$', path)
                if match:
                    cls, group, mode, role = 1, 'tyres:' + match[2], 'tyres', match[1].lower()
            elif name == 'Vehicle_Common' and re.search(r'Vehivel_Buggy_Skid_Dirt_\d+\.wav$', path):
                cls, group, mode, role = 1, 'tyres:BuggySkidDirt', 'tyres', 'skid'
            elif (name.startswith('Weapons_') and name not in ['Weapons_Training', 'Weapons_Mortar']
                  and re.search(r'_Shot_Remote_Near_\d+\.wav$', path)):
                if counts.get('shot', 0) >= 5:
                    continue
                counts['shot'] = counts.get('shot', 0) + 1
                cls, group, mode, role = 2, 'gunfire:' + name, 'shots', 'shot'
            if role is None:
                continue
            selected.append(dict(id=f"src_{bank['Id']}_{media['Id']}", class_index=cls,
                source_group_id=group, mode=mode, role=role, bank=name,
                bank_id=int(bank['Id']), media_id=int(media['Id']), source_name=path,
                source_semantics='native_named_media; foot_side_not_encoded_in_metadata'))
    return selected


if __name__ == '__main__':
    base.choose_sources = choose_sources
    base.main()
