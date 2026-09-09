"""Generate reproducible stereo mixtures plus frame labels from local sources.

Phases are plan -> fixed spatial source cache -> global peak scan -> WAV/labels.
Only a single dataset gain is applied, never a clip- or ear-specific limiter.
Requires NumPy; all other imports are standard library. See the dataset README.
"""
import argparse
from collections import Counter, defaultdict
import concurrent.futures
from functools import lru_cache
import hashlib
import io
import itertools
import json
import math
import os
from pathlib import Path
import shutil
import sys
import time
import wave
import zipfile

import numpy as np

STATE = {}
AUTO_BANKS = set('AK47 HK416 M762 Groza SCAR_L Steyr_AUG_A3 MicroUzi Vector UMP MP5K Bizon Thompson M249 FamasG2 QBZ G36C Ace32 K2 JS9 Scorpion Glock18C M16A4 Mk47 O12 VSS'.split())
BOLT_BANKS = set('AWM Kar98k M24 Winchester1894 Berreta686 SawedOffShotgun NagantM1895 Rhino'.split())


def canonical_hash(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True).encode('utf-8')).hexdigest()


def sha_file(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def save_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def save_jsonl(path, values):
    with Path(path).open('w', encoding='utf-8', newline='\n') as f:
        for value in values:
            f.write(json.dumps(value, ensure_ascii=False, separators=(',', ':')) + '\n')


def read_wave(path):
    with wave.open(str(path), 'rb') as w:
        if w.getsampwidth() != 2 or w.getframerate() != 44100:
            raise ValueError('Unexpected source format')
        return np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').reshape(-1, w.getnchannels()).astype(np.float32) / 32768


def edge_fade(x, count):
    count = min(count, len(x) // 2)
    if count:
        env = np.linspace(0, 1, count, dtype=np.float32)
        x[:count] *= env[:, None] if x.ndim == 2 else env
        x[-count:] *= env[::-1, None] if x.ndim == 2 else env[::-1]
    return x


def fft_convolve(x, h):
    length = len(x) + len(h) - 1
    n = 1 << (length - 1).bit_length()
    return np.fft.irfft(np.fft.rfft(x, n) * np.fft.rfft(h, n), n)[:length].astype(np.float32)


def exact_assignments(n, values, probabilities, rng):
    raw = np.asarray(probabilities) * n
    counts = np.floor(raw).astype(int)
    order = np.argsort(-(raw - counts), kind='stable')
    for i in order[:n - counts.sum()]:
        counts[i] += 1
    out = [v for v, count in zip(values, counts) for _ in range(int(count))]
    rng.shuffle(out)
    return out


def split_sources(sources, cfg):
    rng = np.random.default_rng(cfg['seed'])
    split_names = list(cfg['split_clip_counts'])
    groups = {s['source_group_id']: s['class_index'] for s in sources}
    mapping = {}
    for cls in range(3):
        names = sorted(g for g, c in groups.items() if c == cls)
        if len(names) < 3:
            raise ValueError('Need at least three independent source groups per class')
        rng.shuffle(names)
        ratios = cfg['source_group_split_ratios']
        validation_count = max(1, round(len(names) * ratios[1]))
        test_count = max(1, round(len(names) * ratios[2]))
        if validation_count + test_count >= len(names):
            raise ValueError('Source split leaves no training groups')
        for i, name in enumerate(names):
            split = split_names[0] if i < len(names) - validation_count - test_count else split_names[1] if i < len(names) - test_count else split_names[2]
            mapping[name] = split
    return dict(group_to_split=mapping, source_to_split={s['id']: mapping[s['source_group_id']] for s in sources},
                method='whole_surface_for_footsteps; whole_bank_family_for_vehicles_and_weapons; PCM duplicates removed before splitting')


def make_track(cls, groups, sources_by_id, cfg, rng, density):
    group = str(rng.choice(sorted(groups)))
    ids = groups[group]
    rate = cfg['sample_rate_hz']
    track = dict(class_index=cls, source_group_id=group,
                 source_gain_jitter_db=float(rng.uniform(*cfg['source_gain_jitter_db'])), events=[])
    if cls == 1:
        sid = str(rng.choice(ids))
        available = round(sources_by_id[sid]['duration_seconds'] * rate)
        seconds = {'low': (0.65, 1.4), 'medium': (2.0, 3.8), 'high': (5.0, 6.0)}[density]
        duration = min(int(rng.uniform(*seconds) * rate), available - round(0.05 * rate))
        onset = int(rng.uniform(0, cfg['clip_seconds'] * rate - duration))
        crop = int(rng.integers(0, available - duration + 1))
        track['events'] = [dict(source_id=sid, onset_sample=onset, crop_start_sample=crop,
                                crop_length_samples=duration, gain=1.0)]
    elif cls == 0:
        if density == 'low':
            start = float(rng.uniform(0.2, 4.8))
            intervals = [(start, min(5.7, start + rng.uniform(0.35, 0.9)))]
            cadence = float(rng.uniform(0.4, 0.65))
        elif density == 'medium':
            intervals = [(float(rng.uniform(0.3, 0.8)), float(rng.uniform(1.6, 2.1))),
                         (float(rng.uniform(3.2, 3.7)), float(rng.uniform(4.7, 5.3)))]
            cadence = float(rng.uniform(0.36, 0.6))
        else:
            intervals = [(float(rng.uniform(0.05, 0.35)), float(rng.uniform(5.4, 5.85)))]
            cadence = float(rng.uniform(0.28, 0.45))
        for t, stop in intervals:
            while t < stop:
                track['events'].append(dict(source_id=str(rng.choice(ids)), onset_sample=round(t * rate),
                                            gain=float(rng.uniform(0.85, 1.05))))
                t += cadence * float(rng.uniform(0.90, 1.10))
    else:
        weapon = sources_by_id[ids[0]]['bank'].removeprefix('Weapons_')
        if density == 'low':
            start = float(rng.uniform(0.3, 5.0))
            for i in range(1 if rng.random() < 0.8 else 2):
                track['events'].append(dict(source_id=str(rng.choice(ids)), onset_sample=round((start + 0.3 * i) * rate),
                                            gain=float(rng.uniform(0.9, 1.03))))
        elif weapon in AUTO_BANKS:
            starts = [rng.uniform(0.7, 1.5), rng.uniform(3.1, 4.2)] if density == 'medium' else [0.2, 1.6, 3.0, 4.4]
            for start in starts:
                spacing = float(rng.uniform(0.075, 0.14))
                for i in range(int(rng.integers(2, 5) if density == 'medium' else rng.integers(3, 8))):
                    track['events'].append(dict(source_id=str(rng.choice(ids)), onset_sample=round((start + i * spacing) * rate),
                                                gain=float(rng.uniform(0.9, 1.03))))
        else:
            t = float(rng.uniform(0.5, 1.5))
            while t < (4.5 if density == 'medium' else 5.7):
                track['events'].append(dict(source_id=str(rng.choice(ids)), onset_sample=round(t * rate),
                                            gain=float(rng.uniform(0.9, 1.03))))
                t += float(rng.uniform(1.1, 1.9) if weapon in BOLT_BANKS else rng.uniform(0.6, 1.1) if density == 'medium' else rng.uniform(0.35, 0.65))
    return track


def plan(sources, splitting, cfg):
    by_id = {s['id']: s for s in sources}
    scenes = []
    for split_i, (split, count) in enumerate(cfg['split_clip_counts'].items()):
        rng = np.random.default_rng(cfg['seed'] + 1000 + split_i)
        groups = [defaultdict(list) for _ in range(3)]
        for s in sources:
            if splitting['source_to_split'][s['id']] == split:
                groups[s['class_index']][s['source_group_id']].append(s['id'])
        densities = exact_assignments(count, list(cfg['density_probabilities']), list(cfg['density_probabilities'].values()), rng)
        layout_by_density = {}
        for density in cfg['density_probabilities']:
            probabilities = cfg['density_layout_probabilities'][density]
            layout_by_density[density] = iter(exact_assignments(densities.count(density), list(probabilities), list(probabilities.values()), rng))
        current = []
        for i, density in enumerate(densities):
            layout = next(layout_by_density[density])
            if layout == 'single_source':
                classes = [int(rng.integers(0, 3))]
            elif layout == 'all_three_classes':
                classes = [0, 1, 2]
            elif layout == 'two_distinct_classes':
                classes = sorted(int(c) for c in rng.choice(3, 2, replace=False))
            else:
                a, b = rng.choice(3, 2, replace=False)
                classes = [int(a), int(a), int(b)]
            tracks = [make_track(cls, groups[cls], by_id, cfg, rng, density) for cls in classes]
            current.append(dict(id=f'{split}_{i + 1:06d}', split=split, layout=layout, density=density,
                                sample_rate_hz=cfg['sample_rate_hz'], duration_seconds=cfg['clip_seconds'],
                                tracks=tracks))
        # Exact class-stratified near/mid/far counts, and balanced directions
        # within each distance, avoid an accidental class/direction correlation.
        for cls in range(3):
            for density in cfg['density_probabilities']:
                tracks = [t for scene in current if scene['density'] == density for t in scene['tracks'] if t['class_index'] == cls]
                distances = exact_assignments(len(tracks), list(range(3)), cfg['distance_probabilities'], rng)
                for t, distance in zip(tracks, distances):
                    t['range_index'] = distance
                for distance in range(3):
                    bucket = [t for t in tracks if t['range_index'] == distance]
                    angles = exact_assignments(len(bucket), cfg['azimuth_degrees'], [1 / 8] * 8, rng)
                    for t, angle in zip(bucket, angles):
                        t['azimuth_degrees'] = angle
                        t['sector_index'] = cfg['azimuth_degrees'].index(angle)
        scenes.extend(current)
    return scenes


def prepare_cache(sources, source_root, cache_root, hrir_path, cfg):
    cache_root.mkdir(parents=True, exist_ok=True)
    if sha_file(hrir_path) != cfg['hrir_archive_sha256']:
        raise ValueError('HRIR archive changed')
    hrir = {}
    with zipfile.ZipFile(hrir_path) as z:
        for angle in cfg['azimuth_degrees']:
            a = 360 - angle if angle > 180 else angle
            with wave.open(io.BytesIO(z.read(f'elev0/H0e{a:03d}a.wav')), 'rb') as w:
                h = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').reshape(-1, 2).astype(np.float32) / 32768
            if len(h) != cfg['hrir_length_samples']:
                raise ValueError('Unexpected HRIR length')
            hrir[angle] = h[:, ::-1].copy() if angle > 180 else h
    gain = 1 / math.sqrt(float(np.mean(np.sum(hrir[0].astype(np.float64) ** 2, axis=0))))
    for angle in hrir:
        hrir[angle] *= gain
    for a in [45, 90, 135]:
        if not np.array_equal(hrir[a][:, ::-1], hrir[360 - a]):
            raise ValueError('HRIR mirror convention invalid')
    q = np.arange(cfg['lowpass_taps']) - (cfg['lowpass_taps'] - 1) / 2
    filters = []
    for cutoff in cfg['distance_lowpass_hz']:
        h = 2 * cutoff / cfg['sample_rate_hz'] * np.sinc(2 * cutoff / cfg['sample_rate_hz'] * q) * np.kaiser(cfg['lowpass_taps'], cfg['lowpass_kaiser_beta'])
        filters.append((h / h.sum()).astype(np.float32))
    index = []
    for i, s in enumerate(sources):
        if sha_file(source_root / s['wav_file']) != s['wav_sha256']:
            raise ValueError('Original source changed ' + s['id'])
        x = read_wave(source_root / s['wav_file']).mean(axis=1)
        x -= x.mean(dtype=np.float64)
        edge_fade(x, round(cfg['source_fade_ms'][s['mode']] / 1000 * cfg['sample_rate_hz']))
        cls = s['class_index']
        rms = float(np.sqrt(np.mean(x.astype(np.float64) ** 2)))
        peak = float(np.max(np.abs(x)))
        reference_gain = min(cfg['source_reference_rms'][cls] / max(rms, 1e-12), cfg['source_reference_peak_caps'][cls] / max(peak, 1e-12))
        x *= reference_gain
        for distance in range(3):
            filtered = fft_convolve(x, filters[distance])[64:64 + len(x)] * cfg['distance_gain'][distance]
            for angle in cfg['azimuth_degrees']:
                target = cache_root / f"{s['id']}_r{distance}_a{angle}.npy"
                if not target.exists():
                    stereo = np.column_stack([fft_convolve(filtered, hrir[angle][:, ear]) for ear in range(2)])
                    temporary = target.with_suffix('.tmp.npy')
                    np.save(temporary, stereo.astype(np.float32), allow_pickle=False)
                    os.replace(temporary, target)
        index.append(dict(source_id=s['id'], source_reference_gain=reference_gain, normalized_samples=len(x)))
        if (i + 1) % 30 == 0:
            print('CACHE', i + 1, '/', len(sources), flush=True)
    save_json(cache_root / 'index.json', dict(filter_gain=gain, source_parameters=index,
                                             convention='pilot_distance_lowpass_same_then_full_HRIR; no_individual_normalization'))


def init_worker(output, cache_root, cfg, master_gain):
    global STATE
    STATE = dict(output=Path(output), cache_root=Path(cache_root), cfg=cfg, master_gain=master_gain)
    variant.cache_clear()


@lru_cache(maxsize=96)
def variant(sid, distance, angle):
    return np.load(STATE['cache_root'] / f'{sid}_r{distance}_a{angle}.npy', mmap_mode='r', allow_pickle=False)


def render_tracks(scene):
    cfg = STATE['cfg']
    n = round(cfg['sample_rate_hz'] * cfg['clip_seconds'])
    tracks = []
    for track in scene['tracks']:
        signal = np.zeros((n, 2), dtype=np.float32)
        gain = 10 ** (track['source_gain_jitter_db'] / 20)
        for event in track['events']:
            src = variant(event['source_id'], track['range_index'], track['azimuth_degrees'])
            if 'crop_start_sample' in event:
                start = event['crop_start_sample']
                src = src[start:start + event['crop_length_samples']].copy()
                edge_fade(src, round(0.03 * cfg['sample_rate_hz']))
            onset = event['onset_sample']
            length = min(len(src), n - onset)
            if length > 0:
                signal[onset:onset + length] += src[:length] * (event['gain'] * gain)
        edge_fade(signal, round(0.004 * cfg['sample_rate_hz']))
        tracks.append(signal)
    return tracks


def peak_one(scene):
    tracks = render_tracks(scene)
    mixed = np.sum(tracks, axis=0, dtype=np.float32)
    return float(np.max(np.abs(mixed)))


def frame_labels(tracks, track_metadata, cfg):
    analysis_rate = cfg['analysis_label_sample_rate_hz']
    win = cfg['analysis_label_window_samples']
    hop = cfg['analysis_label_hop_samples']
    count = (round(cfg['clip_seconds'] * analysis_rate) - win) // hop + 1
    ends = (win + np.arange(count) * hop) / analysis_rate
    starts = ends - win / analysis_rate
    left = np.rint(starts * cfg['sample_rate_hz']).astype(int)
    right = np.rint(ends * cfg['sample_rate_hz']).astype(int)
    powers = []
    for signal in tracks:
        energy = np.mean(signal.astype(np.float64) ** 2, axis=1)
        integral = np.concatenate(([0.0], np.cumsum(energy)))
        powers.append((integral[right] - integral[left]) / (right - left))
    powers = np.maximum(np.asarray(powers), 0)
    thresholds = np.maximum(powers.max(axis=1) * 10 ** (cfg['active_threshold_relative_db'] / 10),
                            10 ** (cfg['active_threshold_absolute_dbfs'] / 10))
    active = powers >= thresholds[:, None]
    shape = (count, 4, 8)
    activity = np.zeros(shape, dtype=np.uint8)
    track_count = np.zeros(shape, dtype=np.uint8)
    ranges = np.full(shape, -1, dtype=np.int8)
    range_candidates = np.zeros(shape + (3,), dtype=np.uint8)
    cell_power = np.zeros(shape, dtype=np.float64)
    for i, meta in enumerate(track_metadata):
        cls, sector, distance = meta['class_index'], meta['sector_index'], meta['range_index']
        track_count[:, cls, sector] += active[i]
        range_candidates[:, cls, sector, distance] |= active[i]
        cell_power[:, cls, sector] += powers[i] * active[i]
    activity[:] = track_count > 0
    total_power = powers.sum(axis=0)[:, None, None]
    # Sum of isolated track powers is an observability proxy, not a physical
    # measurement of masked audibility or the exact power of coherent mixtures.
    ratio = cell_power / np.maximum(total_power - cell_power, 1e-15)
    observable = ratio >= 10 ** (cfg['observable_min_sir_db'] / 10)
    activity_mask = np.ones(shape, dtype=np.uint8)
    activity_mask[:, 3, :] = 0  # Aircraft is untrained, not a negative class.
    activity_mask[(activity != 0) & ~observable] = 0
    unique = range_candidates.sum(axis=-1) == 1
    ranges[unique] = range_candidates.argmax(axis=-1)[unique]
    range_mask = (unique & (activity != 0) & observable).astype(np.uint8)
    return dict(activity=activity, activity_loss_mask=activity_mask, range_index=ranges,
                range_loss_mask=range_mask, active_track_count=track_count,
                frame_right_edge_seconds=ends.astype(np.float64),
                class_supervision_mask=np.asarray(cfg['class_supervision_mask'], dtype=np.uint8)), powers


def write_one(scene):
    cfg = STATE['cfg']
    output = STATE['output']
    gain = STATE['master_gain']
    tracks = [x * gain for x in render_tracks(scene)]
    mixed = np.sum(tracks, axis=0, dtype=np.float32)
    peak = float(np.max(np.abs(mixed)))
    if not np.isfinite(mixed).all() or peak > cfg['output_peak_cap'] + 2e-6 or peak < 1e-7:
        raise ValueError(f"Invalid mix peak {scene['id']}: {peak}")
    pcm = np.rint(mixed * 32767).astype('<i2')
    audio_rel = f"audio/{scene['split']}/{scene['id']}.wav"
    label_rel = f"labels/{scene['split']}/{scene['id']}.npz"
    audio = output / audio_rel
    with wave.open(str(audio), 'wb') as w:
        w.setparams((2, 2, cfg['sample_rate_hz'], 0, 'NONE', 'not compressed'))
        w.writeframes(pcm.tobytes())
    labels, powers = frame_labels(tracks, scene['tracks'], cfg)
    if not all(labels['activity'][:, cls, :].any() for cls in {t['class_index'] for t in scene['tracks']}):
        raise ValueError('Missing intended activity ' + scene['id'])
    np.savez_compressed(output / label_rel, **labels)
    active_frames = labels['active_track_count'].sum(axis=(1, 2))
    result = dict(scene, audio_file=audio_rel, label_file=label_rel,
                  master_gain=gain, audio_sha256=sha_file(audio), label_sha256=sha_file(output / label_rel),
                  peak_dbfs=float(20 * np.log10(max(peak, 1e-12))),
                  rms_dbfs=float(20 * np.log10(max(float(np.sqrt(np.mean(mixed.astype(np.float64) ** 2))), 1e-12))),
                  overlapping_frames=int(np.count_nonzero(active_frames >= 2)),
                  total_label_frames=len(active_frames),
                  masked_positive_cells=int(np.count_nonzero((labels['activity'] != 0) & (labels['activity_loss_mask'] == 0))),
                  positive_cells=int(labels['activity'].sum()),
                  silent_target_frames=int(np.count_nonzero(active_frames == 0)))
    return result


def parallel_map(function, items, args, cfg, gain):
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers, initializer=init_worker,
            initargs=(str(args.output), str(args.cache), cfg, gain)) as pool:
        yield from pool.map(function, items, chunksize=20)


def summarize(records, cfg, master_gain):
    stats = {}
    for split, count in cfg['split_clip_counts'].items():
        rows = [r for r in records if r['split'] == split]
        tracks = [t for r in rows for t in r['tracks']]
        distances = Counter(t['range_index'] for t in tracks)
        per_class = {}
        for c in range(3):
            subset = [t for t in tracks if t['class_index'] == c]
            per_class[cfg['class_names'][c]] = dict(track_count=len(subset),
                distance_counts={cfg['distance_names'][d]: sum(t['range_index'] == d for t in subset) for d in range(3)},
                direction_counts={str(a): sum(t['azimuth_degrees'] == a for t in subset) for a in cfg['azimuth_degrees']})
        density_stats = {}
        for density in cfg['density_probabilities']:
            selected = [r for r in rows if r['density'] == density]
            frames = sum(r['total_label_frames'] for r in selected)
            density_stats[density] = dict(clip_count=len(selected),
                active_frame_fraction=1 - sum(r['silent_target_frames'] for r in selected) / max(frames, 1),
                overlap_frame_fraction=sum(r['overlapping_frames'] for r in selected) / max(frames, 1),
                mean_events_per_clip=sum(len(t['events']) for r in selected for t in r['tracks']) / max(len(selected), 1))
        stats[split] = dict(clip_count=len(rows), hours=len(rows) * cfg['clip_seconds'] / 3600,
            track_count=len(tracks), distance_counts={cfg['distance_names'][d]: distances[d] for d in range(3)},
            far_track_fraction=distances[2] / len(tracks), layout_counts=dict(Counter(r['layout'] for r in rows)),
            max_peak_dbfs=max(r['peak_dbfs'] for r in rows),
            minimum_overlap_frames=min(r['overlapping_frames'] for r in rows),
            overlap_frame_fraction=sum(r['overlapping_frames'] for r in rows) / sum(r['total_label_frames'] for r in rows),
            positive_cells=sum(r['positive_cells'] for r in rows), masked_positive_cells=sum(r['masked_positive_cells'] for r in rows),
            per_class=per_class, density=density_stats)
        if len(rows) != count:
            raise ValueError('Incomplete split')
    return dict(dataset_id=cfg['dataset_id'], total_clips=len(records),
                total_hours=len(records) * cfg['clip_seconds'] / 3600,
                common_master_gain=master_gain, splits=stats,
                status='rendered_pending_independent_verification',
                distance_fraction_unit='source_tracks; not_number_of_shots_or_independent_original_recordings')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--config', type=Path, required=True)
    p.add_argument('--sources', type=Path, required=True, help='Directory containing sources.json and wav/')
    p.add_argument('--hrir', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--workers', type=int, default=4)
    p.add_argument('--limit-per-split', type=int)
    p.add_argument('--plan-only', action='store_true')
    args = p.parse_args()
    args.output = args.output.resolve()
    args.sources = args.sources.resolve()
    args.cache = args.sources.parent / 'render_cache_v1'
    cfg = json.loads(args.config.read_text(encoding='utf-8'))
    if args.limit_per_split:
        cfg['split_clip_counts'] = {k: args.limit_per_split for k in cfg['split_clip_counts']}
    if args.workers < 1 or args.workers > 16:
        raise ValueError('workers must be 1..16')
    source_manifest = json.loads((args.sources / 'sources.json').read_text(encoding='utf-8'))
    sources = source_manifest['sources']
    args.output.mkdir(parents=True, exist_ok=True)
    identity = dict(config=cfg, sources_manifest_sha256=sha_file(args.sources / 'sources.json'),
                    generator_sha256=sha_file(Path(__file__)), hrir_sha256=sha_file(args.hrir),
                    python_version=sys.version.split()[0], numpy_version=np.__version__)
    identity_path = args.output / 'generation_identity.json'
    if identity_path.exists() and json.loads(identity_path.read_text(encoding='utf-8')) != identity:
        raise ValueError('Output contains another generation identity; choose a new output directory')
    save_json(identity_path, identity)
    save_json(args.output / 'config.json', cfg)
    splitting = split_sources(sources, cfg)
    save_json(args.output / 'source_split.json', splitting)
    save_json(args.output / 'source_manifest.json', source_manifest)
    scenes = plan(sources, splitting, cfg)
    save_jsonl(args.output / 'recipes.jsonl', scenes)
    for split in cfg['split_clip_counts']:
        for sub in ['audio', 'labels']:
            (args.output / sub / split).mkdir(parents=True, exist_ok=True)
    print('PLANNED', len(scenes), 'clips', flush=True)
    if args.plan_only:
        return
    required = len(scenes) * round(cfg['sample_rate_hz'] * cfg['clip_seconds']) * 4 + 8 * 1024 ** 3
    if shutil.disk_usage(args.output).free < required:
        raise ValueError('Insufficient disk headroom for WAV files and derived cache')
    # Cache has its own identity independent of sample count and scheduling.
    cache_identity = dict(sources=identity['sources_manifest_sha256'], hrir=identity['hrir_sha256'],
        rendering={k: cfg[k] for k in ['sample_rate_hz', 'distance_gain', 'distance_lowpass_hz',
          'azimuth_degrees', 'source_reference_rms', 'source_reference_peak_caps', 'source_fade_ms',
          'lowpass_taps', 'lowpass_kaiser_beta', 'hrir_length_samples']},
        algorithm='fft_lowpass_same_64_then_full_hrir_v1')
    args.cache.mkdir(parents=True, exist_ok=True)
    cache_identity_path = args.cache / 'identity.json'
    if cache_identity_path.exists() and json.loads(cache_identity_path.read_text(encoding='utf-8')) != cache_identity:
        raise ValueError('Render cache identity mismatch')
    save_json(cache_identity_path, cache_identity)
    started = time.monotonic()
    prepare_cache(sources, args.sources, args.cache, args.hrir, cfg)
    print('PEAK_SCAN_START', flush=True)
    largest = 0.0
    for i, peak in enumerate(parallel_map(peak_one, scenes, args, cfg, 1.0), 1):
        largest = max(largest, peak)
        if i % 1000 == 0:
            print('PEAK_SCAN', i, '/', len(scenes), 'max', round(largest, 5), flush=True)
    master_gain = min(cfg['maximum_common_master_gain'], cfg['output_peak_cap'] / largest)
    save_json(args.output / 'master_gain.json', dict(gain=master_gain, unscaled_max_peak=largest,
        purpose='one_common_clipping_safety_gain_for_all_splits; no_per_clip_loudness_normalization'))
    print('RENDER_START master_gain', master_gain, flush=True)
    records = []
    with (args.output / 'manifest.jsonl').open('w', encoding='utf-8', newline='\n') as manifest:
        for i, result in enumerate(parallel_map(write_one, scenes, args, cfg, master_gain), 1):
            records.append(result)
            manifest.write(json.dumps(result, ensure_ascii=False, separators=(',', ':')) + '\n')
            if i % 500 == 0:
                manifest.flush()
                print('RENDERED', i, '/', len(scenes), 'elapsed_s', round(time.monotonic() - started), flush=True)
    for split in cfg['split_clip_counts']:
        save_jsonl(args.output / f'manifest_{split}.jsonl', (r for r in records if r['split'] == split))
    summary = summarize(records, cfg, master_gain)
    summary['render_elapsed_seconds'] = round(time.monotonic() - started, 1)
    save_json(args.output / 'summary.json', summary)
    print('COMPLETE', len(records), 'clips;', summary['total_hours'], 'hours', flush=True)


if __name__ == '__main__':
    main()
