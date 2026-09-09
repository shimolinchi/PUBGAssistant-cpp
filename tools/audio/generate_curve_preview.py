"""Demonstrate explicit S-curves while preserving the frozen v2 renderer/data."""
import argparse
import copy
import json
from pathlib import Path
import sys

import numpy as np

import generate_mixed_dataset as common
import generate_motion_dataset as gen
import motion_synthesis as motion


FOOT_HEADING = [[0, 70], [7, 125], [11, 125], [18, 40], [22, 40], [30, 115]]
CAR_HEADING = [[0, 85], [9, 120], [14, 120], [20, 40], [25, 55], [30, 110]]


def set_path(track, xy, heading):
    track['trajectory'].update(initial_xy=xy, heading_knots_unwrapped_degrees=heading,
                               path_shape='explicit_s_curve_with_smooth_heading')
    path = motion.trajectory(track['trajectory'], 30)
    chord = path['xy'][-1] - path['xy'][0]
    offset = path['xy'] - path['xy'][0]
    deviation = np.max(np.abs(offset[:, 0] * chord[1] - offset[:, 1] * chord[0])) / np.linalg.norm(chord)
    if path['distance'].min() < 4 or deviation < 5:
        raise ValueError('Curve must stay away from the listener and visibly bend')
    return float(deviation)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', required=True, type=Path)
    args = p.parse_args()
    workspace = Path(__file__).resolve().parents[2]
    sources_root = workspace / 'datasets/audio-synthetic-20260906-v2/sources'
    hrir = workspace / 'datasets/audio-synthetic-20260906-v1/sources/hrir/kemar-diffuse.zip'
    output = args.output.resolve()
    source_manifest = json.loads((sources_root / 'sources.json').read_text(encoding='utf-8'))
    cfg = gen.configuration()
    splitting, groups = gen.source_pools(source_manifest['sources'], cfg)
    base = gen.preview_plan(source_manifest['sources'], cfg, groups)
    rows = [copy.deepcopy(base[i]) for i in [0, 2, 4]]
    titles = ['水泥地脚步：S形曲线、走停与跑步', 'Dacia：S形曲线、加减速与刹停', '混合：弯曲脚步、弯曲车辆与固定枪手']
    curves = []
    for index, row in enumerate(rows):
        row.update(id=f'C{index+1:02d}', title=titles[index])
        for track in row['tracks']:
            if track['class_index'] == 0:
                deviation = set_path(track, [-34., 10.], FOOT_HEADING)
            elif track['class_index'] == 1:
                deviation = set_path(track, [-100., 35.], CAR_HEADING)
            else:
                continue
            curves.append(dict(scene=row['id'], class_index=track['class_index'], max_chord_deviation_units=deviation))
    cfg.update(dataset_id='pubg_explicit_curves_preview_20260906', split_clip_counts={'preview': len(rows)})
    identity = dict(config=cfg, source_manifest_sha256=common.sha_file(sources_root / 'sources.json'),
        source_root=str(sources_root), hrir_file=str(hrir), hrir_sha256=common.sha_file(hrir),
        generator_sha256=common.sha_file(Path(gen.__file__)), core_sha256=common.sha_file(Path(motion.__file__)),
        shared_v1_sha256=common.sha_file(Path(common.__file__)),
        curve_planner_sha256=common.sha_file(Path(__file__)), python=sys.version.split()[0], numpy=np.__version__)
    output.mkdir(parents=True, exist_ok=True)
    target = output / 'generation_identity.json'
    if target.exists() and json.loads(target.read_text(encoding='utf-8')) != identity:
        raise ValueError('Changed generation identity; use a new preview directory')
    common.save_json(target, identity)
    common.save_json(output / 'config.json', cfg)
    common.save_json(output / 'source_manifest.json', source_manifest)
    common.save_json(output / 'source_split.json', splitting)
    common.save_jsonl(output / 'recipes.jsonl', rows)
    for rel in ['audio/preview', 'labels/preview', '_render_stage']:
        (output / rel).mkdir(parents=True, exist_ok=True)
    for source in source_manifest['sources']:
        if common.sha_file(sources_root / source['wav_file']) != source['wav_sha256']:
            raise ValueError('Changed native source')
    gen.init_worker(str(sources_root), source_manifest, str(hrir), str(output), cfg, 1.)
    peak = max(gen.peak_one(row) for row in rows)
    gain = min(.6, .7 / peak)
    common.save_json(output / 'master_gain.json', dict(gain=gain, unscaled_max_peak=peak,
        purpose='one_common_gain_for_all_curve_previews'))
    gen.STATE['gain'] = gain
    records = [gen.write_one(row) for row in rows]
    common.save_jsonl(output / 'manifest.jsonl', records)
    common.save_jsonl(output / 'manifest_preview.jsonl', records)
    common.save_json(output / 'summary.json', dict(status='rendered_pending_independent_verification',
        total_clips=len(records), total_hours=len(records) / 120, master_gain=gain,
        curves=curves, native_rev_engine_rendered=False))
    gen.export_timelines(records, output)
    print(json.dumps(dict(output=str(output), samples=len(rows), curves=curves), ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
