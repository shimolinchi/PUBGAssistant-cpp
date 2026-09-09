"""Audit per-source localization on every analysis frame; optionally export CSV.

Reads existing NPZ truth. Never replaces it with the 100 ms viewer timeline.
"""
import argparse
from collections import Counter
import csv
import json
from pathlib import Path

import numpy as np

from generate_mixed_dataset import save_json, sha_file


SECTORS = ['正前', '右前', '右侧', '右后', '正后', '左后', '左侧', '左前']
CLASSES = ['脚步', '车辆', '枪声', '运输飞机']
HEADER = ['帧索引', '声源ID', '类别', '分析窗右端_秒', '标签时刻_秒',
          'x_合成单位', 'y_合成单位', '方位角_度', '方位扇区',
          '距离_合成单位', '距离档', '当前帧发声', '相对速度_合成单位每秒', '地面材质',
          '声源归属', '方位有效', '自身动作速度_合成单位每秒']


def audit_and_export(dataset, output=None, scene_ids=None):
    dataset = Path(dataset).resolve()
    rows = [json.loads(line) for line in (dataset / 'manifest.jsonl').read_text(encoding='utf-8').splitlines()]
    if scene_ids:
        rows = [row for row in rows if row['id'] in set(scene_ids)]
        if {r['id'] for r in rows} != set(scene_ids):
            raise ValueError('Unknown scene ID')
    if output is not None:
        output = Path(output).resolve()
        output.mkdir(parents=True, exist_ok=True)
    totals, exports = Counter(), []
    for row in rows:
        label = dataset / row['label_file']
        if sha_file(label) != row['label_sha256']:
            raise ValueError('Changed source labels: ' + row['id'])
        with np.load(label, allow_pickle=False) as archive:
            keys = ['frame_right_edge_seconds', 'track_xy', 'track_azimuth_degrees',
                    'track_distance_units', 'track_range_index', 'track_activity',
                    'track_class_index', 'track_speed']
            z = {key: archive[key] for key in keys}
            for key in ['track_source_role', 'track_direction_valid', 'track_locomotion_speed']:
                if key in archive:
                    z[key] = archive[key]
        # Float32 can round 359.999999 degrees to 360; both mean the same
        # direction. Canonicalize the exported angle without modifying old NPZs.
        totals['angles_canonicalized_360_to_0'] += int(np.count_nonzero(z['track_azimuth_degrees'] == 360))
        z['track_azimuth_degrees'] %= 360
        ends = z['frame_right_edge_seconds']
        frames, actors = len(ends), len(row['tracks'])
        assert frames == 2997 and z['track_xy'].shape == (actors, frames, 2)
        assert np.allclose(np.diff(ends), .01, rtol=0, atol=1e-12)
        assert np.array_equal(ends, (1024 + np.arange(frames) * 320) / 32000)
        for key in ['track_azimuth_degrees', 'track_distance_units', 'track_range_index', 'track_activity', 'track_speed']:
            assert z[key].shape == (actors, frames) and np.isfinite(z[key]).all()
        for i, meta in enumerate(row['tracks']):
            cls = int(z['track_class_index'][i])
            own = meta.get('source_role') == 'self'
            assert cls == meta['class_index']
            xy = z['track_xy'][i].astype(np.float64)
            measured_distance = np.linalg.norm(xy, axis=1)
            measured_azimuth = np.mod(np.degrees(np.arctan2(xy[:, 0], xy[:, 1])), 360)
            angular_error = (z['track_azimuth_degrees'][i] - measured_azimuth + 180) % 360 - 180
            assert np.max(np.abs(angular_error)) < .001
            np.testing.assert_allclose(z['track_distance_units'][i], measured_distance, atol=1e-4, rtol=1e-6)
            if own:
                assert np.all(z['track_distance_units'][i] == 0)
                assert not z['track_direction_valid'][i].any()
                assert z['track_source_role'][i] == 1
                totals['self_track_frames_with_direction_masked'] += frames
            else:
                assert np.all(z['track_distance_units'][i] > 0)
            assert np.all((z['track_azimuth_degrees'][i] >= 0) & (z['track_azimuth_degrees'][i] < 360))
            assert np.isin(z['track_range_index'][i], [-1] if own else [0, 1, 2]).all()
            active = z['track_activity'][i] != 0
            totals[CLASSES[cls] + '_track_frames'] += frames
            totals['inactive_track_frames_with_position'] += int(np.count_nonzero(~active))
            if cls in [0, 1] and not own:
                chord = xy[-1] - xy[0]
                length = np.linalg.norm(chord)
                deviation = 0 if length < 1e-8 else float(np.max(np.abs((xy[:, 0] - xy[0, 0]) * chord[1] - (xy[:, 1] - xy[0, 1]) * chord[0])) / length)
                totals[CLASSES[cls] + '_tracks'] += 1
                totals[CLASSES[cls] + '_curved_tracks_deviation_over_0_25_units'] += deviation > .25
        totals['scenes'] += 1
        totals['analysis_frames'] += frames
        if output is not None:
            target = output / row['split'] / (row['id'] + '.csv')
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open('w', encoding='utf-8-sig', newline='') as f:
                writer = csv.writer(f)
                writer.writerow(HEADER)
                # Time-major order, one row per actor even during its silence.
                for frame in range(frames):
                    for i, meta in enumerate(row['tracks']):
                        az = float(z['track_azimuth_degrees'][i, frame])
                        own = meta.get('source_role') == 'self'
                        writer.writerow([frame, f'{row["id"]}:track{i+1}', CLASSES[meta['class_index']],
                            f'{ends[frame]:.6f}', f'{ends[frame]-.016-64/44100:.9f}',
                            *[f'{v:.6f}' for v in z['track_xy'][i, frame]], '不适用' if own else f'{az:.6f}',
                            '不适用' if own else SECTORS[int(np.floor((az + 22.5) / 45)) % 8],
                            f'{z["track_distance_units"][i, frame]:.6f}', '自身' if own else ['近', '中', '远'][z['track_range_index'][i, frame]],
                            int(z['track_activity'][i, frame]), f'{z["track_speed"][i, frame]:.6f}', meta.get('surface', ''),
                            '自身' if own else '外部', int(not own),
                            f'{z.get("track_locomotion_speed", z["track_speed"])[i, frame]:.6f}' if own else ''])
            exports.append(dict(scene_id=row['id'], file=str(target), data_rows=frames * actors, sha256=sha_file(target)))
        if len(rows) > 100 and totals['scenes'] % 500 == 0:
            print('FRAME_AUDIT', totals['scenes'], '/', len(rows), flush=True)
    report = dict(status='passed', dataset=str(dataset), annotation_unit='one_audio_analysis_frame_per_source',
        hop_seconds=.01, window_seconds=.032, frames_per_30_second_scene=2997,
        position_reference='window_centre_minus_64_over_44100_second_common_filter_delay',
        distance_units='synthetic_scene_units_not_calibrated_game_metres',
        azimuth_convention='clockwise_from_front_0_right_90_back_180_left_270',
        all_sources_have_finite_positions_on_every_frame=True,
        external_sources_have_distance_and_azimuth_on_every_frame=True,
        self_direction_is_invalid_not_front=True,
        source_activity_separate_from_position=True, counts=dict(totals), csv_exports=exports)
    save_json((output or dataset) / 'frame_annotation_audit.json', report)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dataset', type=Path, required=True)
    parser.add_argument('--output', type=Path, help='Omit to audit existing labels without CSV expansion')
    parser.add_argument('--scene-id', action='append')
    args = parser.parse_args()
    result = audit_and_export(args.dataset, args.output, args.scene_id)
    print(json.dumps({k: v for k, v in result.items() if k != 'csv_exports'}, ensure_ascii=False, indent=2))
