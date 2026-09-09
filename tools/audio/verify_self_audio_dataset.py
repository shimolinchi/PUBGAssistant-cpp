"""Read every v3 WAV/label; verify provenance, gait, movement and reproduction."""
import argparse
from collections import Counter, defaultdict
import concurrent.futures
import hashlib
import io
import json
from pathlib import Path
import wave

import numpy as np

import generate_self_audio_dataset as gen
import generate_mixed_dataset as common
import self_audio_synthesis as motion


def inspect(job):
    root, row = job
    raw = (root / row['audio_file']).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == row['audio_sha256']
    with wave.open(io.BytesIO(raw), 'rb') as w:
        assert (w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()) == (44100, 2, 2, 30 * 44100)
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').astype(np.int32)
    assert 0 < np.max(np.abs(pcm)) <= round(.7 * 32767) + 1
    raw_labels = (root / row['label_file']).read_bytes()
    assert hashlib.sha256(raw_labels).hexdigest() == row['label_sha256']
    with np.load(io.BytesIO(raw_labels), allow_pickle=False) as z:
        assert z['activity'].shape == (2997, 4, 8)
        assert np.array_equal(z['class_supervision_mask'], [1, 1, 1, 0])
        assert not z['activity'][:, 3].any() and not z['activity_loss_mask'][:, 3].any()
        assert np.array_equal(z['activity'], z['active_track_count'] > 0)
        expected_t = (1024 + np.arange(2997) * 320) / 32000
        assert np.array_equal(z['frame_right_edge_seconds'], expected_t)
        centre = expected_t - .016 - 64 / 44100
        expected_count = np.zeros((2997, 4, 8), np.uint8)
        candidates = np.zeros((2997, 4, 8, 3), np.uint8)
        expected_self = np.zeros((2997, 3), np.uint8)
        for i, meta in enumerate(row['tracks']):
            path = motion.trajectory(meta['trajectory'], 30)
            own = meta['source_role'] == 'self'
            assert np.all(path['distance'] == 0) if own else path['distance'].min() > 0
            assert z['track_source_role'][i] == int(own)
            assert np.all(z['track_direction_valid'][i] == int(not own))
            xy = np.column_stack([np.interp(centre, path['time'], path['xy'][:, k]) for k in [0, 1]])
            np.testing.assert_allclose(z['track_xy'][i], xy, atol=3e-5, rtol=1e-6)
            az = np.mod(np.degrees(np.arctan2(xy[:, 0], xy[:, 1])), 360)
            sector = np.floor((az + 22.5) / 45).astype(int) % 8
            distance = np.linalg.norm(xy, axis=1)
            ranges = np.digitize(distance, [np.sqrt(8 * 30), np.sqrt(30 * 90)])
            if own:
                ranges[:] = -1
            np.testing.assert_array_equal(z['track_range_index'][i], ranges)
            np.testing.assert_allclose(z['track_speed'][i], 0 if own else np.interp(centre, path['time'], path['speed']), atol=1e-5)
            np.testing.assert_allclose(z['track_locomotion_speed'][i], np.interp(centre, path['time'], path['speed']), atol=1e-5)
            np.testing.assert_allclose(z['track_distance_units'][i], distance, atol=1e-4)
            assert np.all(np.abs((z['track_azimuth_degrees'][i] - az + 180) % 360 - 180) < .001)
            np.testing.assert_array_equal(z['track_sector_index'][i], -1 if own else sector)
            indices = np.flatnonzero(z['track_activity'][i])
            cls = meta['class_index']
            if own:
                expected_self[indices, cls] += 1
            else:
                expected_count[indices, cls, sector[indices]] += 1
                candidates[indices, cls, sector[indices], ranges[indices]] = 1
            if cls in [0, 1]:
                # Stationary segments have exactly constant position; every other
                # increment equals integrated velocity, without trajectory jumps.
                step = np.linalg.norm(np.diff(path['xy'], axis=0), axis=1)
                ceiling = np.maximum(path['speed'][:-1], path['speed'][1:]) * .01
                assert np.all(step <= ceiling + 1e-9)
                stopped = (path['speed'][:-1] == 0) & (path['speed'][1:] == 0)
                assert np.all(step[stopped] == 0)
            if cls == 0:
                previous = None
                for event in meta['events']:
                    t = event['onset_sample'] / 44100
                    v = np.interp(t, path['time'], path['speed'])
                    assert v > .15
                    assert event['simulated_foot'] != previous
                    previous = event['simulated_foot']
            if cls == 1:
                assert path['rpm'].max() > .2 and path['brake'].max() > .25
                assert meta['lifecycle']['rev_model_rendered'] is False
        assert np.array_equal(expected_count, z['active_track_count'])
        assert np.array_equal(expected_self > 0, z['self_activity'])
        assert np.array_equal(z['any_class_activity'][:, :3], (expected_count > 0).max(axis=2)[:, :3] | (expected_self > 0))
        assert np.array_equal(z['self_activity'].sum(axis=0), row['self_active_class_frame_counts'])
        conflict = candidates.sum(axis=-1) != 1
        assert np.all(z['range_index'][conflict] == -1)
        valid = z['range_loss_mask'] != 0
        assert not np.any(valid & conflict)
        assert not np.any(valid & (z['activity_loss_mask'] == 0))
        assert not np.any(valid & (z['activity'] == 0))
        assert np.all(z['range_index'][valid] == candidates.argmax(axis=-1)[valid])
        counts = z['track_activity'].sum(axis=0)
        assert row['silent_target_frames'] == int(np.count_nonzero(counts == 0))
        assert row['overlapping_frames'] == int(np.count_nonzero(counts >= 2))
        external_active = (z['track_activity'] != 0) & (z['track_source_role'][:, None] == 0)
        range_counts = np.bincount(z['track_range_index'][external_active], minlength=3)
        assert np.array_equal(range_counts, row['active_track_range_frame_counts'])
    return dict(bytes=len(raw) + len(raw_labels), frames=2997,
        active=2997 - row['silent_target_frames'], overlap=row['overlapping_frames'],
        split=row['split'], density=row['density'])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dataset', type=Path, required=True)
    p.add_argument('--workers', type=int, default=4)
    args = p.parse_args()
    root = args.dataset.resolve()
    identity = json.loads((root / 'generation_identity.json').read_text(encoding='utf-8'))
    assert identity['core_sha256'] == common.sha_file(Path(motion.__file__))
    assert identity['generator_sha256'] == common.sha_file(Path(gen.__file__))
    assert identity['shared_v2_core_sha256'] == common.sha_file(Path(motion.base.__file__))
    cfg = identity['config']
    rows = [json.loads(line) for line in (root / 'manifest.jsonl').read_text(encoding='utf-8').splitlines()]
    assert len({r['id'] for r in rows}) == len(rows)
    assert dict(Counter(r['split'] for r in rows)) == cfg['split_clip_counts']
    manifest = json.loads((root / 'source_manifest.json').read_text(encoding='utf-8'))
    assert identity['source_manifest_sha256'] == common.sha_file(root / 'source_manifest.json')
    sources = {s['id']: s for s in manifest['sources']}
    splitting = json.loads((root / 'source_split.json').read_text(encoding='utf-8'))
    pcm_by_split = defaultdict(set)
    footsteps, vehicles, contacts, moving = 0, 0, 0, 0
    for row in rows:
        for t in row['tracks']:
            cls = t['class_index']
            if cls == 0:
                footsteps += 1; contacts += len(t['events'])
                assert t['surface_policy'] == 'one_material_for_entire_track_and_both_simulated_feet'
            if cls == 1:
                vehicles += 1
                assert {'startup', 'shutdown'} <= {e['role'] for e in t['events']}
                assert t['lifecycle']['engine_backend'] == cfg['engine_backend']
            moving += cls in [0, 1]
            ids = set(t['layers'].values()) | {e['source_id'] for e in t['events']}
            for sid in ids:
                source = sources[sid]
                assert source['class_index'] == cls
                if cls == 0 or not source['source_group_id'].startswith('tyres:'):
                    assert source['source_group_id'] == t['source_group_id']
                if row['split'] != 'preview':
                    assert splitting['source_to_split'][sid] == row['split']
                pcm_by_split[row['split']].add(source['pcm_sha256'])
            for event in t['events']:
                native_role = sources[event['source_id']]['role']
                expected_role = ('local_shot' if cls == 2 else event['role'] + '_fpp' if cls == 1 else event['role']) if t['source_role'] == 'self' else event['role']
                assert native_role == expected_role, (native_role, expected_role)
    splits = list(pcm_by_split)
    for i, a in enumerate(splits):
        for b in splits[i + 1:]:
            assert not (pcm_by_split[a] & pcm_by_split[b])
    totals = defaultdict(Counter)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        for i, result in enumerate(pool.map(inspect, [(root, r) for r in rows]), 1):
            totals[(result['split'], result['density'])].update({k: result[k] for k in ['bytes', 'frames', 'active', 'overlap']})
            if i % 100 == 0 or i == len(rows):
                print('VERIFIED', i, '/', len(rows), flush=True)
    density = {}
    for split in splits:
        values = []
        for d in ['low', 'medium', 'high']:
            total = totals[(split, d)]
            if total['frames']:
                v = dict(active_fraction=total['active'] / total['frames'], overlap_fraction=total['overlap'] / total['frames'])
                density[split + '/' + d] = v; values.append(v)
        if len(values) == 3:
            assert values[0]['active_fraction'] < values[1]['active_fraction'] < values[2]['active_fraction']
            assert values[0]['overlap_fraction'] < values[1]['overlap_fraction'] < values[2]['overlap_fraction']
    gain = json.loads((root / 'master_gain.json').read_text(encoding='utf-8'))['gain']
    gen.init_worker(identity['source_root'], manifest, identity['hrir_file'], str(root), cfg, gain)
    selected = rows if splits == ['preview'] else [next(r for r in rows if r['split'] == split and r['density'] == d) for split in splits for d in ['low', 'medium', 'high']]
    if splits != ['preview']:
        for split in splits:
            for cls in range(3):
                match = next((r for r in rows if r['split'] == split and any(t['source_role'] == 'self' and t['class_index'] == cls for t in r['tracks'])), None)
                if match and match not in selected:
                    selected.append(match)
    reproduced = []
    for row in selected:
        signals, paths = gen.render(row)
        mixed = np.sum(signals, axis=0, dtype=np.float32) * gain
        expected_pcm = np.rint(mixed * 32767).astype('<i2').tobytes()
        with wave.open(str(root / row['audio_file']), 'rb') as w:
            assert w.readframes(w.getnframes()) == expected_pcm
        powers = motion.frame_powers(signals, 30) * gain ** 2
        expected = motion.frame_labels(None, row['tracks'], paths, 30, cfg, powers=powers)
        with np.load(root / row['label_file'], allow_pickle=False) as actual:
            assert set(actual.files) == set(expected)
            for key in expected:
                assert np.array_equal(actual[key], expected[key]), (row['id'], key)
        reproduced.append(row['id'])
    assert not list((root / '_render_stage').glob('*.npy'))
    report = dict(status='passed', wav_files=len(rows), label_files=len(rows), total_hours=len(rows) * 30 / 3600,
        audio_and_label_bytes=sum(t['bytes'] for t in totals.values()),
        checked_footstep_tracks=footsteps, checked_foot_contacts=contacts, checked_vehicle_tracks=vehicles,
        checked_moving_tracks=moving, same_surface_for_both_feet=True, preserved_phase_across_pauses=True,
        trajectories_and_frame_labels_match=True, native_vehicle_actions_verified=True,
        proprietary_rev_engine_rendered=False, engine_render_is_documented_approximation=True,
        source_pcm_shared_between_splits=0, no_output_clipping=True,
        self_labels_separate_from_external=True, self_direction_invalid=True,
        self_identity_never_inferred_from_distance=True,
        aircraft_supervision_disabled=True, reproduced_pcm_and_labels_identical=reproduced,
        measured_density=density, limitation='Engineering verification, not a human listening acceptance or a model evaluation.')
    common.save_json(root / 'verification.json', report)
    summary = json.loads((root / 'summary.json').read_text(encoding='utf-8'))
    summary['status'] = 'rendered_and_verified'
    common.save_json(root / 'summary.json', summary)
    print(json.dumps({k: v for k, v in report.items() if k != 'measured_density'}, ensure_ascii=False, indent=2), flush=True)


if __name__ == '__main__':
    main()
