"""Prepare named local-client sources for the mixed-audio dataset. No downloads.

Requires the previously verified read-only PAK audit and local vgmstream decoder.
Source WAV files remain separate from all derived renders.
"""
import argparse
import concurrent.futures
import ctypes
import hashlib
import importlib.util
import json
import mmap
from pathlib import Path
import re
import struct
import subprocess
import wave

import numpy as np


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def chunks(data):
    pos, result = 0, {}
    while pos + 8 <= len(data):
        name = data[pos:pos + 4]
        size = struct.unpack_from('<I', data, pos + 4)[0]
        if pos + 8 + size > len(data):
            raise ValueError('Invalid bank chunk bounds')
        result[name] = (pos + 8, size)
        pos += 8 + size
    if pos != len(data):
        raise ValueError('Incomplete bank')
    return result


def choose_sources(banks):
    chosen = []
    excluded_vehicle = re.compile(r'(Motorglider|Aircraft|Plane|Heli|Airboat)', re.I)
    for b in sorted(banks, key=lambda x: x['ShortName']):
        bank = b['ShortName']
        candidates = []
        for media in sorted(b.get('IncludedMemoryFiles', []), key=lambda x: x['ShortName']):
            name = media['ShortName']
            if not name.lower().endswith('.wav'):
                continue
            foot = re.search(r'Run_([^\\]+)_Close_\d+\.wav$', name)
            if bank == 'FootStepsBank' and foot:
                cls, family, mode = 0, 'footsteps:' + foot[1], 'steps'
            elif (bank.startswith('Weapons_') and bank not in ['Weapons_Training', 'Weapons_Mortar']
                  and re.search(r'_Shot_Remote_Near_\d+\.wav$', name)):
                cls, family, mode = 2, 'gunfire:' + bank, 'shots'
            elif (bank.startswith('Vehicle') and not excluded_vehicle.search(bank + name)
                  and re.search(r'Idle', name, re.I)
                  and not re.search(r'(FPP|Startup|Shutdown|Water|Tire|Horn)', name, re.I)):
                cls, family, mode = 1, 'vehicle:' + bank, 'engine'
            else:
                continue
            candidates.append(dict(id=f"src_{b['Id']}_{media['Id']}", class_index=cls,
                                   source_group_id=family, mode=mode, bank=bank,
                                   bank_id=int(b['Id']), media_id=int(media['Id']),
                                   source_name=name))
        # Preserve all available footstep variations. Limit large gun/skin banks.
        if bank != 'FootStepsBank':
            candidates = candidates[:6 if bank.startswith('Weapons_') else 2]
        chosen.extend(candidates)
    return chosen


def decode_one(args, item):
    wem = args.output / 'wem' / (item['id'] + '.wem')
    wav = args.output / 'wav' / (item['id'] + '.wav')
    if not wav.exists():
        result = subprocess.run([str(args.node), str(args.decoder_wrapper), str(args.decoder),
                                 str(wem), str(wav)], capture_output=True, text=True,
                                encoding='utf-8', errors='replace', timeout=120)
        if result.returncode:
            raise RuntimeError(item['id'] + ': ' + result.stderr[-600:])
    with wave.open(str(wav), 'rb') as w:
        rate, ch, width, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
        pcm = w.readframes(n)
    if width != 2 or ch not in (1, 2) or rate != 44100:
        return dict(item, excluded_reason=f'unsupported_format_{rate}_{ch}_{width}')
    x = np.frombuffer(pcm, dtype='<i2').reshape(-1, ch).astype(np.float32) / 32768.0
    mono = x.mean(axis=1)
    if n == 0 or not np.isfinite(x).all() or np.max(np.abs(mono)) < 1e-5:
        return dict(item, excluded_reason='empty_or_silent')
    if item['mode'] == 'engine' and n / rate < 0.3:
        return dict(item, excluded_reason='engine_too_short')
    return dict(item, wav_file='wav/' + wav.name, wem_file='wem/' + wem.name,
                wav_sha256=digest(wav), pcm_sha256=hashlib.sha256(pcm).hexdigest(),
                sample_rate_hz=rate, channels=ch, duration_seconds=n / rate,
                source_pcm_rail_samples=int(np.count_nonzero(np.abs(x) >= 32767 / 32768)),
                source_peak=float(np.max(np.abs(x))))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--audit-root', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--node', required=True, type=Path)
    p.add_argument('--decoder-wrapper', required=True, type=Path)
    p.add_argument('--decoder', required=True, type=Path)
    p.add_argument('--workers', type=int, default=4)
    args = p.parse_args()
    args.output = args.output.resolve()
    for sub in ['wem', 'wav']:
        (args.output / sub).mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('verified_pak_audit', args.audit_root / 'catalog_pak.py')
    audit = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(audit)
    metadata_path = args.audit_root / 'metadata/2616915968.txt'
    metadata = json.loads(metadata_path.read_text(encoding='utf-8').rstrip('\0'))
    sources = choose_sources(metadata['SoundBanksInfo']['SoundBanks'])
    wanted = {s['bank_id'] for s in sources}
    catalog = json.loads((args.audit_root / 'physical_catalog.json').read_text(encoding='utf-8'))
    bank_entries = {}
    with audit.PAK.open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        for e in catalog['entries']:
            if e['encrypted'] or e['raw'] < 16:
                continue
            if e.get('kind') == 'bank':
                data = (args.audit_root / 'banks' / f"bank_{e['offset']}.bnk").read_bytes()[:36]
            elif e['raw'] > 8 * 1024 * 1024:
                if not e['method']:
                    data = m[e['payload_offset']:e['payload_offset'] + 36]
                else:
                    start, end = e['blocks'][0]
                    size = min(e['block_size'], e['raw'])
                    src = ctypes.create_string_buffer(m[start:end] + bytes(64))
                    dst = ctypes.create_string_buffer(size + 64)
                    if audit.dll.decode_block(src, end - start, dst, size) != size:
                        raise ValueError('Cannot identify source bank')
                    data = dst.raw[:36]
            else:
                continue
            if data[:4] == b'BKHD':
                bank_id = struct.unpack_from('<I', data, 12)[0]
                if bank_id in wanted:
                    bank_entries[bank_id] = e
        if wanted != set(bank_entries):
            raise ValueError('Missing source banks: ' + str(wanted - set(bank_entries)))
        for bank_id, e in bank_entries.items():
            data = audit.unpack(m, e)
            parts = chunks(data)
            idx, size = parts[b'DIDX']
            start, media_size = parts[b'DATA']
            index = {mid: (off, n) for mid, off, n in struct.iter_unpack('<III', data[idx:idx + size])}
            for s in sources:
                if s['bank_id'] != bank_id:
                    continue
                off, n = index[s['media_id']]
                if off + n > media_size:
                    raise ValueError('Media outside DATA chunk')
                wem = data[start + off:start + off + n]
                if wem[:4] != b'RIFF' or wem[8:12] != b'WAVE' or struct.unpack_from('<I', wem, 4)[0] + 8 != len(wem):
                    raise ValueError('Invalid media RIFF')
                sha = hashlib.sha256(wem).hexdigest()
                target = args.output / 'wem' / (s['id'] + '.wem')
                if target.exists():
                    if digest(target) != sha:
                        raise ValueError('Refuse changed original source ' + s['id'])
                else:
                    target.write_bytes(wem)
                s['provenance'] = dict(pak_file=audit.PAK.name, bank_entry_offset=e['offset'],
                                       bank_payload_sha1=e['sha1'], media_offset_in_bank=start + off,
                                       wem_sha256=sha, metadata_entry_offset=2616915968)
            print('EXTRACTED', bank_id, flush=True)
    decoded = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        for item in pool.map(lambda s: decode_one(args, s), sources):
            decoded.append(item)
            if len(decoded) % 30 == 0:
                print('DECODED', len(decoded), '/', len(sources), flush=True)
    accepted, excluded, duplicates = [], [], []
    seen = {}
    for s in decoded:
        if 'excluded_reason' in s:
            excluded.append(s)
        elif s['pcm_sha256'] in seen:
            duplicates.append(dict(id=s['id'], duplicate_of=seen[s['pcm_sha256']]))
        else:
            seen[s['pcm_sha256']] = s['id']
            accepted.append(s)
    manifest = dict(schema_version=1, source_policy='named_local_client_media_read_only',
                    metadata_sha256=digest(metadata_path), source_count=len(accepted),
                    sources=accepted, excluded=excluded, duplicates=duplicates,
                    limitation='Only 44.1 kHz PCM16 decoded sources; original Near/Close/Idle layers are not dry acoustical measurements.')
    (args.output / 'sources.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(accepted=len(accepted), excluded=len(excluded), duplicates=len(duplicates),
                          per_class={c: sum(s['class_index'] == c for s in accepted) for c in range(3)})), flush=True)


if __name__ == '__main__':
    main()
