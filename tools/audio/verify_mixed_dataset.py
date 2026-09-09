"""Read back every rendered WAV and label file, audit splits and reproduction."""
import argparse
from collections import Counter, defaultdict
import concurrent.futures
import hashlib
import io
import json
from pathlib import Path
import wave

import numpy as np

import generate_mixed_dataset as gen


def label_property_checks(cfg):
    n = round(cfg['sample_rate_hz'] * cfg['clip_seconds'])
    first = np.zeros((n, 2), np.float32)
    first[44100:88200] = 0.2
    second = first.copy()
    meta = [dict(class_index=0, sector_index=2, range_index=0),
            dict(class_index=0, sector_index=2, range_index=2)]
    lab, _ = gen.frame_labels([first, second], meta, cfg)
    collision = lab['active_track_count'][:, 0, 2] == 2
    assert collision.any() and np.all(lab['range_index'][collision, 0, 2] == -1)
    assert not lab['range_loss_mask'][collision, 0, 2].any()
    assert not lab['activity'][lab['frame_right_edge_seconds'] < 1].any()
    assert not lab['activity'][lab['frame_right_edge_seconds'] > 2.04].any()
    assert not lab['activity_loss_mask'][:, 3, :].any()
    quiet = first * 0.0005
    meta[1]['sector_index'] = 6
    lab, _ = gen.frame_labels([first, quiet], meta, cfg)
    weak = lab['activity'][:, 0, 6] != 0
    assert weak.any() and not lab['activity_loss_mask'][weak, 0, 6].any()
    rng = np.random.default_rng(123)
    x, h = rng.normal(size=511).astype(np.float32), rng.normal(size=129).astype(np.float32)
    np.testing.assert_allclose(gen.fft_convolve(x, h), np.convolve(x, h), atol=2e-5, rtol=2e-5)


def inspect_one(job):
    output, row, cfg = job
    audio_bytes = (output / row['audio_file']).read_bytes()
    assert hashlib.sha256(audio_bytes).hexdigest() == row['audio_sha256'], row['id']
    with wave.open(io.BytesIO(audio_bytes), 'rb') as w:
        assert (w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()) == (
            2, 2, cfg['sample_rate_hz'], round(cfg['sample_rate_hz'] * cfg['clip_seconds']))
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').reshape(-1, 2)
    peak = int(np.max(np.abs(pcm.astype(np.int32))))
    assert 0 < peak <= round(cfg['output_peak_cap'] * 32767) + 1
    label_bytes = (output / row['label_file']).read_bytes()
    assert hashlib.sha256(label_bytes).hexdigest() == row['label_sha256']
    with np.load(io.BytesIO(label_bytes), allow_pickle=False) as data:
        activity = data['activity']
        count = (round(cfg['clip_seconds'] * cfg['analysis_label_sample_rate_hz']) - cfg['analysis_label_window_samples']) // cfg['analysis_label_hop_samples'] + 1
        assert activity.shape == (count, 4, 8)
        assert set(np.unique(activity)).issubset({0, 1})
        assert not activity[:, 3, :].any()
        assert not data['activity_loss_mask'][:, 3, :].any()
        assert np.array_equal(data['class_supervision_mask'], [1, 1, 1, 0])
        assert np.array_equal(activity, data['active_track_count'] > 0)
        valid = data['range_loss_mask'] != 0
        assert not np.any(valid & (activity == 0))
        assert np.all((data['range_index'][valid] >= 0) & (data['range_index'][valid] < 3))
        assert not np.any(valid & (data['activity_loss_mask'] == 0))
        on = min(e['onset_sample'] / cfg['sample_rate_hz'] for t in row['tracks'] for e in t['events'])
        assert not activity[data['frame_right_edge_seconds'] < on - 1 / cfg['sample_rate_hz']].any()
        expected = (cfg['analysis_label_window_samples'] + np.arange(count) * cfg['analysis_label_hop_samples']) / cfg['analysis_label_sample_rate_hz']
        assert np.array_equal(data['frame_right_edge_seconds'], expected)
        active = data['active_track_count'].sum(axis=(1, 2))
        measured = dict(frames=count, active_frames=int(np.count_nonzero(active)),
                        overlap_frames=int(np.count_nonzero(active >= 2)))
        assert row['silent_target_frames'] == count - measured['active_frames']
        assert row['overlapping_frames'] == measured['overlap_frames']
    return dict(id=row['id'], split=row['split'], density=row['density'], peak_pcm=peak,
                bytes=len(audio_bytes) + len(label_bytes), **measured)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dataset', required=True, type=Path)
    p.add_argument('--cache', required=True, type=Path)
    p.add_argument('--workers', type=int, default=4)
    args = p.parse_args()
    output = args.dataset.resolve()
    cfg = json.loads((output / 'config.json').read_text(encoding='utf-8'))
    label_property_checks(cfg)
    rows = [json.loads(line) for line in (output / 'manifest.jsonl').read_text(encoding='utf-8').splitlines()]
    splitting = json.loads((output / 'source_split.json').read_text(encoding='utf-8'))
    source_manifest = json.loads((output / 'source_manifest.json').read_text(encoding='utf-8'))
    sources = {s['id']: s for s in source_manifest['sources']}
    used_hashes = defaultdict(set)
    seen = set()
    distance_counts = Counter()
    for row in rows:
        assert row['id'] not in seen
        seen.add(row['id'])
        for t in row['tracks']:
            assert splitting['group_to_split'][t['source_group_id']] == row['split']
            assert cfg['azimuth_degrees'][t['sector_index']] == t['azimuth_degrees']
            distance_counts[t['range_index']] += 1
            for e in t['events']:
                s = sources[e['source_id']]
                assert s['source_group_id'] == t['source_group_id']
                assert s['class_index'] == t['class_index']
                assert splitting['source_to_split'][s['id']] == row['split']
                assert 0 <= e['onset_sample'] < cfg['clip_seconds'] * cfg['sample_rate_hz']
                used_hashes[row['split']].add(s['pcm_sha256'])
    names = list(cfg['split_clip_counts'])
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            assert not used_hashes[a].intersection(used_hashes[b])
    totals = defaultdict(lambda: Counter())
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        for i, result in enumerate(pool.map(inspect_one, ((output, row, cfg) for row in rows)), 1):
            totals[(result['split'], result['density'])].update({k: result[k] for k in ['frames', 'active_frames', 'overlap_frames', 'bytes']})
            if i % 1000 == 0:
                print('VERIFIED', i, '/', len(rows), flush=True)
    density = {}
    for split in names:
        values = []
        for d in ['low', 'medium', 'high']:
            x = totals[(split, d)]
            v = dict(active_fraction=x['active_frames'] / x['frames'], overlap_fraction=x['overlap_frames'] / x['frames'])
            density[split + '/' + d] = v
            values.append(v)
        assert values[0]['active_fraction'] < values[1]['active_fraction'] < values[2]['active_fraction']
        assert values[0]['overlap_fraction'] < values[1]['overlap_fraction'] < values[2]['overlap_fraction']
    gain = json.loads((output / 'master_gain.json').read_text(encoding='utf-8'))['gain']
    gen.init_worker(str(output), str(args.cache.resolve()), cfg, gain)
    reproduced = []
    for split in names:
        for d in ['low', 'medium', 'high']:
            row = next(r for r in rows if r['split'] == split and r['density'] == d)
            tracks = [x * gain for x in gen.render_tracks(row)]
            pcm = np.rint(np.sum(tracks, axis=0, dtype=np.float32) * 32767).astype('<i2')
            with wave.open(str(output / row['audio_file']), 'rb') as w:
                assert w.readframes(w.getnframes()) == pcm.tobytes()
            reproduced.append(row['id'])
    files_count = {s: sum(r['split'] == s for r in rows) for s in names}
    assert files_count == cfg['split_clip_counts']
    far_fraction = distance_counts[2] / sum(distance_counts.values())
    if len(rows) > 1000:
        assert abs(far_fraction - cfg['distance_probabilities'][2]) < 0.002
    report = dict(status='passed', checked_wav_files=len(rows), checked_label_files=len(rows),
        hours=len(rows) * cfg['clip_seconds'] / 3600, split_counts=files_count,
        source_pcm_hash_overlap_between_splits=0, source_groups_disjoint=True,
        wav_and_label_hashes_verified=True, no_output_clipping=True,
        aircraft_supervision_disabled=True, label_property_checks_passed=True,
        reproduced_pcm_identical=reproduced, far_source_track_fraction=far_fraction,
        distance_track_counts={cfg['distance_names'][d]: distance_counts[d] for d in range(3)},
        measured_density=density, audio_and_label_bytes=sum(v['bytes'] for v in totals.values()),
        limitation='These checks verify synthesis, labels and split integrity; not real-game recognition accuracy.')
    gen.save_json(output / 'verification.json', report)
    summary = json.loads((output / 'summary.json').read_text(encoding='utf-8'))
    summary['status'] = 'rendered_and_verified'
    gen.save_json(output / 'summary.json', summary)
    print(json.dumps({k: v for k, v in report.items() if k != 'measured_density'}, ensure_ascii=False, indent=2), flush=True)


if __name__ == '__main__':
    main()
