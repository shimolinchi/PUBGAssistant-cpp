"""Generate trajectory-driven v3 external and listener-bound audio, with an inspectable plan before rendering.

Default scale: 2400/300/300 thirty-second scenes, 20/2.5/2.5 hours.
Use --preview-only for ten thirty-second far/self trajectories and state timelines.
"""
import argparse
from collections import Counter, defaultdict
import concurrent.futures
import csv
import json
from pathlib import Path
import shutil
import sys
import time
import wave

import numpy as np

import generate_mixed_dataset as old
import self_audio_synthesis as motion


STATE = {}


def configuration():
    cfg = json.loads(Path(__file__).with_name('mix_dataset_v1.json').read_text(encoding='utf-8'))
    cfg.update(dataset_id='pubg_trajectory_self_v3_20260906', clip_seconds=30,
        split_clip_counts={'train': 2400, 'validation': 300, 'test': 300},
        trajectory_step_seconds=.01, spatial_filter_transition_seconds=.02,
        spatial_hrir_grid_degrees=5, lowpass_common_delay_samples=64,
        position_label_reference='analysis_window_centre_minus_common_lowpass_delay',
        engine_backend='native_idle_variable_rate_and_load_filter_approximation',
        rev_model_rendered=False, fixed_surface_per_footstep_track=True,
        native_foot_side_labels_available=False,
        external_initial_range_probabilities=[.10, .10, .80],
        far_initial_radius_multiplier=1.6,
        self_identity_rule='explicit_source_provenance_never_distance_threshold',
        source_role_names=['external', 'self'],
        self_direction='invalid_mask_0_sector_minus1_azimuth0_placeholder',
        self_distance='listener_relative_origin_0_not_a_measured_acoustic_distance',
        self_foot_backend='same_surface_native_variations_listener_centred_approximation',
        self_vehicle_backend='native_FPP_actions_shared_idle_RPM_approximation',
        self_gun_backend='native_Local_recordings_stereo_preserved')
    return cfg


def source_pools(sources, cfg):
    by_group = defaultdict(list)
    for s in sources:
        by_group[s['source_group_id']].append(s)
    eligible = []
    for group, members in by_group.items():
        roles = {s['role'] for s in members}
        if group.startswith('tyres:'):
            continue
        if group.startswith('gunfire:') and 'shot' not in roles:
            continue
        if group.startswith('vehicle:') and not {'idle', 'startup', 'shutdown'} <= roles:
            continue
        if group.startswith('footsteps:') and not {'walk', 'run', 'sprint'} <= roles:
            continue
        eligible.extend(members)
    splitting = old.split_sources(eligible, cfg)
    # Entire tyre recording families remain isolated. The test skid recordings
    # are native Buggy dirt variants; no common PCM is reused across splits.
    aux = {'tyres:Dirt': 'train', 'tyres:Concrete': 'validation', 'tyres:BuggySkidDirt': 'test'}
    for s in sources:
        if s['source_group_id'] in aux:
            splitting['group_to_split'][s['source_group_id']] = aux[s['source_group_id']]
            splitting['source_to_split'][s['id']] = aux[s['source_group_id']]
    splitting['method'] += '; auxiliary tyre families assigned whole to distinct splits'
    splitting['excluded_incomplete_groups'] = sorted(set(by_group) - set(splitting['group_to_split']))
    return splitting, by_group


def build_track(cls, group, members, aux, cfg, rng, density, distance, azimuth, demo=False):
    seconds = cfg['clip_seconds']
    spec, lifecycle = motion.path_spec(cls, density, seconds, rng, distance, azimuth, demo)
    path = motion.trajectory(spec, seconds)
    # Keep paths outside the listener's head. Rotate the movement, not the audio.
    if path['distance'].min() < 4:
        for knot in spec['heading_knots_unwrapped_degrees']:
            knot[1] += 100
        path = motion.trajectory(spec, seconds)
        if path['distance'].min() < 4:
            for knot in spec['heading_knots_unwrapped_degrees']:
                knot[1] = azimuth
            path = motion.trajectory(spec, seconds)
    roles = defaultdict(list)
    by_id = {s['id']: s for s in members}
    for s in members:
        roles[s['role']].append(s['id'])
    meta = dict(class_index=cls, source_group_id=group, trajectory=spec,
        initial_range_index=distance, initial_azimuth_degrees=azimuth,
        source_gain_jitter_db=float(rng.uniform(-1.5, 1.5)), events=[], layers={})
    if cls == 0:
        meta['events'] = motion.schedule_footsteps(path, roles, rng)
        meta['surface'] = group.split(':', 1)[1]
        meta['surface_policy'] = 'one_material_for_entire_track_and_both_simulated_feet'
    elif cls == 1:
        meta['lifecycle'] = lifecycle
        meta['engine_phase_samples'] = int(rng.integers(0, 400000))
        for role in ['startup', 'shutdown']:
            # Prefer the shortest complete native action, never stretch/reverse it.
            sid = min(roles[role], key=lambda sid: by_id[sid]['duration_seconds'])
            if role == 'startup' and by_id[sid]['duration_seconds'] > 3.5:
                raise ValueError('Motion starts before this native startup completes')
            meta['events'].append(dict(source_id=sid, onset_sample=round(lifecycle[role + '_seconds'] * motion.RATE), role=role, gain=.8))
            if role == 'shutdown':
                lifecycle['engine_end_seconds'] = min(seconds, lifecycle['shutdown_seconds'] + by_id[sid]['duration_seconds'])
        meta['layers']['idle'] = roles['idle'][0]
        for role in ['roll', 'brake', 'skid']:
            candidates = [s for s in aux if s['role'] == role]
            if candidates:
                meta['layers'][role] = str(rng.choice([s['id'] for s in candidates]))
        meta['tyre_layer_note'] = 'native surface brake/roll where available; held-out test uses independent Buggy dirt skid recordings'
    else:
        # Extend the established burst scheduler using independent six-second bouts.
        offsets = [float(rng.uniform(0, 24))] if density == 'low' else [0., 11., 22.] if density == 'medium' else [0., 6., 12., 18., 24.]
        for offset in offsets:
            events = old.make_track(2, {group: sorted(by_id)}, by_id, cfg, rng, density)['events']
            for e in events:
                e['onset_sample'] += round(offset * motion.RATE)
                e['role'] = 'shot'
                if e['onset_sample'] < seconds * motion.RATE:
                    meta['events'].append(e)
    return meta


def plan(sources, cfg, splitting, by_group):
    scenes = []
    for si, (split, count) in enumerate(cfg['split_clip_counts'].items()):
        rng = np.random.default_rng(cfg['seed'] + 24000 + si)
        pools = {c: sorted(g for g, members in by_group.items() if not g.startswith('tyres:') and
                    members[0]['class_index'] == c and splitting['group_to_split'].get(g) == split) for c in range(3)}
        aux = [s for s in sources if s['source_group_id'].startswith('tyres:') and splitting['source_to_split'][s['id']] == split]
        densities = old.exact_assignments(count, ['low', 'medium', 'high'], [.4, .35, .25], rng)
        current = []
        for i, density in enumerate(densities):
            probs = cfg['density_layout_probabilities'][density]
            layout = str(rng.choice(list(probs), p=list(probs.values())))
            if layout == 'single_source':
                classes = [int(rng.integers(0, 3))]
            elif layout == 'all_three_classes':
                classes = [0, 1, 2]
            elif layout == 'two_distinct_classes':
                classes = sorted(map(int, rng.choice(3, 2, replace=False)))
            else:
                a, b = map(int, rng.choice(3, 2, replace=False)); classes = [a, a, b]
            current.append(dict(id=f'{split}_{i + 1:06d}', split=split, density=density, layout=layout,
                sample_rate_hz=motion.RATE, duration_seconds=cfg['clip_seconds'],
                tracks=[dict(class_index=c) for c in classes]))
        for cls in range(3):
            for density in ['low', 'medium', 'high']:
                targets = [t for s in current if s['density'] == density for t in s['tracks'] if t['class_index'] == cls]
                distances = old.exact_assignments(len(targets), [0, 1, 2], cfg['external_initial_range_probabilities'], rng)
                azimuths = old.exact_assignments(len(targets), list(range(0, 360, 45)), [1/8] * 8, rng)
                for t, d, a in zip(targets, distances, azimuths):
                    group = str(rng.choice(pools[cls]))
                    t.update(build_track(cls, group, external_members(by_group[group]), aux, cfg, rng, density, d, a))
                    t['source_role'] = 'external'
        policies = old.exact_assignments(count,
            ['external_only', 'self_only_foot', 'self_only_vehicle', 'self_only_gun',
             'mixed_self_foot', 'mixed_self_vehicle', 'mixed_self_gun'],
            [.25, .05, .05, .05, .20, .20, .20], rng)
        for scene, policy in zip(current, policies):
            scene['self_policy'] = policy
            if policy != 'external_only':
                cls = {'foot': 0, 'vehicle': 1, 'gun': 2}[policy.split('_')[-1]]
                eligible = [g for g in pools[cls] if own_members(by_group[g], cls)]
                group = str(rng.choice(eligible))
                own = build_self_track(cls, group, by_group[group], cfg, rng, scene['density'])
                if policy.startswith('self_only'):
                    scene['tracks'] = []
                scene['tracks'].append(own)
        scenes.extend(current)
    return scenes


def external_members(members):
    return [s for s in members if s['role'] not in ['local_shot', 'startup_fpp', 'shutdown_fpp']]


def own_members(members, cls):
    roles = {s['role'] for s in members}
    if cls == 0:
        return members if {'walk', 'run', 'sprint'} <= roles else []
    if cls == 2:
        return [s for s in members if s['role'] == 'local_shot']
    if not {'idle', 'startup_fpp', 'shutdown_fpp'} <= roles:
        return []
    return [dict(s, role=s['role'].replace('_fpp', '')) for s in members
            if s['role'] in ['idle', 'startup_fpp', 'shutdown_fpp']]


def build_self_track(cls, group, members, cfg, rng, density, demo=False):
    selected = own_members(members, cls)
    if not selected:
        raise ValueError('No own-source assets for ' + group)
    track = build_track(cls, group, selected, [], cfg, rng, density, 0, 0, demo)
    track.update(source_role='self', initial_range_index=-1,
        source_gain_jitter_db=float(rng.uniform(-6, 2)),
        self_audio_provenance=['listener_centred_same_surface_approximation',
            'native_FPP_actions_idle_RPM_approximation', 'native_Local_gunshots'][cls])
    track['trajectory'].update(listener_bound=True, initial_xy=[0., 0.],
        initial_range_index=-1, position_meaning='listener_relative_not_world_position')
    return track


def preview_plan(sources, cfg, by_group):
    rng = np.random.default_rng(2026090603)
    foot, vehicle, gun = 'footsteps:Concrete', 'vehicle:Vehicle_DaciaBank', 'gunfire:Weapons_AK47'
    def outside(cls, group, far=True):
        t = build_track(cls, group, external_members(by_group[group]), [], cfg, rng, 'high', 2 if far else 0, 0, True)
        t['source_role'] = 'external'
        if far:
            t['trajectory']['initial_xy'] = [[-70., 140.], [-170., 155.], [140., -170.]][cls]
            t['trajectory']['heading_knots_unwrapped_degrees'] = [[0., 70.], [7., 125.], [11., 125.], [18., 40.], [22., 40.], [30., 115.]]
            t['trajectory']['path_shape'] = 'explicit_s_curve'
        else:
            t['trajectory']['initial_xy'] = [2.5, 2.5]
            if cls == 0:
                # Circle close to the listener, with pauses and unchanged surface.
                t['trajectory']['heading_knots_unwrapped_degrees'] = [[0., 135.], [10., 495.], [20., 855.], [30., 1215.]]
        return t
    def own(cls, group):
        return build_self_track(cls, group, by_group[group], cfg, rng, 'high', True)
    presets = [
        ('F01', '远处脚步：S 形曲线、行走与停步', [outside(0, foot)]),
        ('F02', '远处车辆：S 形曲线、起步加速与刹停', [outside(1, vehicle)]),
        ('F03', '远处枪声：右后方固定枪手', [outside(2, gun)]),
        ('F04', '远处混合：脚步、车辆、枪声', [outside(0, foot), outside(1, vehicle), outside(2, gun)]),
        ('P01', '自身脚步：听者位置固定、同地面交替与停步', [own(0, foot)]),
        ('P02', '自身枪声：客户端 Local 原声', [own(2, gun)]),
        ('P03', '自身车辆：第一人称启动、行驶、刹停与熄火', [own(1, vehicle)]),
        ('M01', '自身脚步＋远处脚步与枪声', [own(0, foot), outside(0, foot), outside(2, gun)]),
        ('M02', '自身车辆＋远处脚步、车辆和枪声', [own(1, vehicle), outside(0, foot), outside(1, vehicle), outside(2, gun)]),
        ('N01', '近处外部脚步对照：靠近也不标成自身', [outside(0, foot, False)]),
    ]
    return [dict(id=sid, title=title, split='preview', density='high', layout='preview',
        sample_rate_hz=motion.RATE, duration_seconds=30, tracks=tracks) for sid, title, tracks in presets]


def init_worker(source_root, source_manifest, hrir, output, cfg, gain):
    STATE.clear()
    STATE.update(library=motion.SourceLibrary(source_root, source_manifest['sources']),
        renderer=motion.SpatialRenderer(hrir, cfg['hrir_archive_sha256']),
        output=Path(output), cfg=cfg, gain=gain)


def render(scene):
    signals, paths = [], []
    for meta in scene['tracks']:
        path = motion.trajectory(meta['trajectory'], scene['duration_seconds'])
        if meta.get('source_role') == 'self':
            wet = motion.render_self(meta, path, STATE['library'], scene['duration_seconds'])
        else:
            dry = motion.mono_track(meta, path, STATE['library'], scene['duration_seconds'])
            wet = STATE['renderer'].render(dry, path)
        signals.append(wet)
        paths.append(path)
    return signals, paths


def peak_one(scene):
    signals, _ = render(scene)
    mixed = np.sum(signals, axis=0, dtype=np.float32)
    temporary = STATE['output'] / '_render_stage' / scene['id']
    np.save(temporary.with_suffix('.npy'), mixed, allow_pickle=False)
    np.save(temporary.with_suffix('.powers.npy'), motion.frame_powers(signals, scene['duration_seconds']), allow_pickle=False)
    return float(np.max(np.abs(mixed)))


def write_one(scene):
    stage = STATE['output'] / '_render_stage'
    temporary = stage / scene['id']
    mixed = np.load(temporary.with_suffix('.npy'), allow_pickle=False) * STATE['gain']
    powers = np.load(temporary.with_suffix('.powers.npy'), allow_pickle=False) * STATE['gain'] ** 2
    if mixed.shape != (round(scene['duration_seconds'] * motion.RATE), 2):
        raise ValueError('Invalid staged mixture shape')
    paths = [motion.trajectory(meta['trajectory'], scene['duration_seconds']) for meta in scene['tracks']]
    peak = float(np.max(np.abs(mixed)))
    if not np.isfinite(mixed).all() or not 0 < peak <= .700002:
        raise ValueError('Invalid output peak ' + scene['id'] + ': ' + str(peak))
    labels = motion.frame_labels(None, scene['tracks'], paths, scene['duration_seconds'], STATE['cfg'], powers=powers)
    pcm = np.rint(mixed * 32767).astype('<i2')
    audio = f"audio/{scene['split']}/{scene['id']}.wav"
    label = f"labels/{scene['split']}/{scene['id']}.npz"
    root = STATE['output']
    with wave.open(str(root / audio), 'wb') as w:
        w.setparams((2, 2, motion.RATE, 0, 'NONE', 'not compressed')); w.writeframes(pcm.tobytes())
    np.savez_compressed(root / label, **labels)
    counts = labels['track_activity'].sum(axis=0)
    external_active = (labels['track_activity'] != 0) & (labels['track_source_role'][:, None] == 0)
    range_counts = np.bincount(labels['track_range_index'][external_active], minlength=3)
    active = labels['track_activity'] != 0
    result = dict(scene, audio_file=audio, label_file=label,
        audio_sha256=old.sha_file(root / audio), label_sha256=old.sha_file(root / label),
        master_gain=STATE['gain'], peak_dbfs=float(20 * np.log10(peak)),
        total_label_frames=len(counts), silent_target_frames=int(np.count_nonzero(counts == 0)),
        overlapping_frames=int(np.count_nonzero(counts >= 2)), active_track_range_frame_counts=range_counts.tolist(),
        intended_tracks_with_activity=int(np.count_nonzero(active.any(axis=1))),
        self_active_class_frame_counts=labels['self_activity'].sum(axis=0).tolist())
    if result['intended_tracks_with_activity'] != len(scene['tracks']):
        raise ValueError('Missing intended source activity ' + scene['id'])
    # Delete only these two self-created, completed intermediate files.
    for path in [temporary.with_suffix('.npy'), temporary.with_suffix('.powers.npy')]:
        if path.resolve().parent != stage.resolve() or not stage.resolve().is_relative_to(root.resolve()):
            raise ValueError('Refuse cleanup outside this generation directory')
        path.unlink()
    return result


def parallel(function, scenes, args, manifest, cfg, gain):
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers, initializer=init_worker,
            initargs=(str(args.sources), manifest, str(args.hrir), str(args.output), cfg, gain)) as pool:
        yield from pool.map(function, scenes, chunksize=2)


def export_timelines(rows, output):
    for row in rows:
        with (output / (row['id'] + '_轨迹.csv')).open('w', encoding='utf-8-sig', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(['秒', '轨迹', '类别', '地面', '右向坐标', '前向坐标', '方位角_正前0顺时针', '粗距离', '速度_合成单位每秒', '运动状态', '归一化转速_近似', '刹车强度'])
            for j, track in enumerate(row['tracks']):
                m = motion.trajectory(track['trajectory'], row['duration_seconds'])
                for k in range(0, len(m['time']), 10):
                    speed = m['speed'][k]
                    state = ['停住', '加速', '巡航', '减速', '刹车'][m['motion_state'][k]]
                    if track['class_index'] == 0:
                        state = '停步' if speed < .15 else '行走' if speed < 2.4 else '跑步' if speed < 4.8 else '冲刺'
                    distance = int(np.digitize(m['distance'][k], [np.sqrt(8 * 30), np.sqrt(30 * 90)]))
                    writer.writerow([round(m['time'][k], 2), j + 1, ['脚步', '车辆', '枪声'][track['class_index']], track.get('surface', ''),
                        *np.round(m['xy'][k], 3), round(m['azimuth'][k], 2), ['近', '中', '远'][distance], round(speed, 3), state,
                        round(m['rpm'][k], 4) if track['class_index'] == 1 else '', round(m['brake'][k], 4) if track['class_index'] == 1 else ''])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sources', type=Path, required=True)
    p.add_argument('--hrir', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--workers', type=int, default=4)
    p.add_argument('--limit-per-split', type=int)
    p.add_argument('--preview-only', action='store_true')
    p.add_argument('--plan-only', action='store_true')
    args = p.parse_args()
    args.sources, args.hrir, args.output = (x.resolve() for x in [args.sources, args.hrir, args.output])
    cfg = configuration()
    if args.limit_per_split:
        cfg['split_clip_counts'] = {s: args.limit_per_split for s in cfg['split_clip_counts']}
    manifest = json.loads((args.sources / 'sources.json').read_text(encoding='utf-8'))
    splitting, groups = source_pools(manifest['sources'], cfg)
    if args.preview_only:
        cfg['split_clip_counts'] = {'preview': 10}
        cfg['dataset_id'] = 'pubg_far_self_preview_v3_20260906'
        scenes = preview_plan(manifest['sources'], cfg, groups)
    else:
        scenes = plan(manifest['sources'], cfg, splitting, groups)
    args.output.mkdir(parents=True, exist_ok=True)
    identity = dict(config=cfg, source_manifest_sha256=old.sha_file(args.sources / 'sources.json'),
        source_root=str(args.sources), hrir_file=str(args.hrir), hrir_sha256=old.sha_file(args.hrir),
        generator_sha256=old.sha_file(Path(__file__)), core_sha256=old.sha_file(Path(motion.__file__)),
        shared_v2_core_sha256=old.sha_file(Path(motion.base.__file__)),
        shared_v1_sha256=old.sha_file(Path(old.__file__)), python=sys.version.split()[0], numpy=np.__version__)
    target = args.output / 'generation_identity.json'
    if target.exists() and json.loads(target.read_text(encoding='utf-8')) != identity:
        raise ValueError('Changed generation identity; use a new output directory')
    old.save_json(target, identity)
    old.save_json(args.output / 'config.json', cfg)
    old.save_json(args.output / 'source_manifest.json', manifest)
    old.save_json(args.output / 'source_split.json', splitting)
    old.save_jsonl(args.output / 'recipes.jsonl', scenes)
    if args.plan_only:
        print('PLANNED', len(scenes), flush=True); return
    for s in manifest['sources']:
        if old.sha_file(args.sources / s['wav_file']) != s['wav_sha256']:
            raise ValueError('Source changed ' + s['id'])
    if shutil.disk_usage(args.output).free < len(scenes) * 30 * motion.RATE * 8 + 3 * 1024 ** 3:
        raise ValueError('Insufficient output disk space')
    (args.output / '_render_stage').mkdir(exist_ok=True)
    for split in cfg['split_clip_counts']:
        for kind in ['audio', 'labels']:
            (args.output / kind / split).mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    largest = 0.
    print('PLANNED', len(scenes), 'trajectory scenes; PEAK_SCAN_START', flush=True)
    for i, peak in enumerate(parallel(peak_one, scenes, args, manifest, cfg, 1.), 1):
        largest = max(largest, peak)
        if i % 25 == 0 or i == len(scenes):
            print('PEAK_SCAN', i, '/', len(scenes), 'max', round(largest, 5), 'elapsed_s', round(time.monotonic() - started), flush=True)
    gain = min(.6, .7 / largest)
    old.save_json(args.output / 'master_gain.json', dict(gain=gain, unscaled_max_peak=largest,
        purpose='single_gain_for_whole_run_no_clip_or_ear_normalization'))
    rows = []
    with (args.output / 'manifest.jsonl').open('w', encoding='utf-8', newline='\n') as f:
        for i, row in enumerate(parallel(write_one, scenes, args, manifest, cfg, gain), 1):
            rows.append(row)
            f.write(json.dumps(row, ensure_ascii=False, separators=(',', ':')) + '\n')
            if i % 25 == 0 or i == len(scenes):
                f.flush(); print('RENDERED', i, '/', len(scenes), 'elapsed_s', round(time.monotonic() - started), flush=True)
    stats = {}
    for split in cfg['split_clip_counts']:
        selected = [r for r in rows if r['split'] == split]
        old.save_jsonl(args.output / ('manifest_' + split + '.jsonl'), selected)
        by_density = {}
        for density in ['low', 'medium', 'high']:
            subset = [r for r in selected if r['density'] == density]
            frames = sum(r['total_label_frames'] for r in subset)
            if frames:
                by_density[density] = dict(clips=len(subset), active_frame_fraction=1 - sum(r['silent_target_frames'] for r in subset) / frames,
                    overlapping_frame_fraction=sum(r['overlapping_frames'] for r in subset) / frames)
        stats[split] = dict(clips=len(selected), hours=len(selected) * 30 / 3600, density=by_density)
    range_counts = np.sum([r['active_track_range_frame_counts'] for r in rows], axis=0)
    old.save_json(args.output / 'summary.json', dict(status='rendered_pending_independent_verification',
        total_clips=len(rows), total_hours=len(rows) * 30 / 3600, splits=stats, master_gain=gain,
        active_track_range_frame_counts=range_counts.tolist(), far_active_track_frame_fraction=float(range_counts[2] / range_counts.sum()),
        elapsed_seconds=round(time.monotonic() - started, 2), native_rev_engine_rendered=False,
        self_active_class_frame_counts=np.sum([r['self_active_class_frame_counts'] for r in rows], axis=0).tolist(),
        self_policy_scene_counts=dict(Counter(r.get('self_policy', 'preview') for r in rows)),
        source_identity_inferred_from_distance=False))
    print('COMPLETE', len(rows), 'scenes', flush=True)


if __name__ == '__main__':
    main()
