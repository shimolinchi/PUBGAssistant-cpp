"""Build a local audio player with synchronized trajectory and action timelines."""
import argparse
import json
from pathlib import Path

import numpy as np

import self_audio_synthesis as motion


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--preview', type=Path, required=True)
    args = p.parse_args()
    root = args.preview.resolve()
    rows = [json.loads(line) for line in (root / 'manifest.jsonl').read_text(encoding='utf-8').splitlines()]
    sources = {s['id']: s for s in json.loads((root / 'source_manifest.json').read_text(encoding='utf-8'))['sources']}
    bundle = []
    for row in rows:
        tracks, timeline = [], []
        with np.load(root / row['label_file'], allow_pickle=False) as z:
            frame_activity = z['track_activity'].tolist()
        for i, track in enumerate(row['tracks']):
            cls = track['class_index']
            path = motion.trajectory(track['trajectory'], 30)
            points = np.column_stack([path['xy'], path['azimuth'], path['distance'], path['speed'], path['motion_state']])[::5]
            surface_names = {'Concrete': '水泥地', 'Dirt': '泥地', 'Fabric': '织物地面', 'Rock': '岩石地面', 'Snow': '雪地'}
            name = (surface_names.get(track.get('surface'), track.get('surface', '')) + '脚步') if cls == 0 else track['source_group_id'].split(':')[1].replace('Vehicle_', '').replace('Bank', '') if cls == 1 else '远处枪声'
            own = track.get('source_role') == 'self'
            if own:
                name = '自身' + (name if cls != 2 else '枪声')
            actor = dict(cls=cls, name=name, own=own, points=np.round(points, 4).tolist(), activity=frame_activity[i])
            if cls == 0:
                timeline.extend([[name, interval, action] for interval, action in [
                    ('0–1 秒', '静止'), ('1–8 秒', '行走：左右交替'), ('8–10 秒', '停步：不产生新脚步'),
                    ('10–18 秒', '跑步：继续交替'), ('18–20 秒', '停步'), ('20–29 秒', '冲刺：步频随速度提高'), ('29–30 秒', '停止，新脚步结束，保留自然尾音')]])
            elif cls == 1:
                actor['lifecycle'] = track['lifecycle']
                start_event = next(e for e in track['events'] if e['role'] == 'startup')
                actor['startupEnd'] = start_event['onset_sample'] / 44100 + sources[start_event['source_id']]['duration_seconds']
                for e in track['events']:
                    onset = e['onset_sample'] / 44100
                    end = min(30, onset + sources[e['source_id']]['duration_seconds'])
                    timeline.append([name, f'{onset:.1f}–{end:.1f} 秒', '发动机启动（原声）' if e['role'] == 'startup' else '发动机熄火（原声）'])
                segments = track['trajectory']['speed_knots']
                for (begin, v0), (end, v1) in zip(segments, segments[1:]):
                    if v0 == v1 == 0:
                        if begin > track['lifecycle']['move_seconds'] and end < track['lifecycle']['shutdown_seconds']:
                            timeline.append([name, f'{begin:.1f}–{end:.1f} 秒', '刹停后怠速；位置保持不变'])
                        continue
                    action = '起步加速' if v0 == 0 else '加速' if v1 > v0 else '巡航' if v1 == v0 else '减速并刹停' if v1 == 0 else '减速'
                    timeline.append([name, f'{begin:.1f}–{end:.1f} 秒', action])
            else:
                timeline.append([name, '0–30 秒', '自身 Local 枪声；听者相对位置固定' if own else '间歇枪声；枪手位置固定'])
            tracks.append(actor)
        order = {track['name']: i for i, track in enumerate(tracks)}
        timeline.sort(key=lambda entry: (order[entry[0]], float(entry[1].split('–')[0])))
        csv_file = f'逐帧标注/{row["split"]}/{row["id"]}.csv'
        bundle.append(dict(id=row['id'], title=row['title'], audio=row['audio_file'], tracks=tracks, timeline=timeline,
            frameCsv=csv_file if (root / csv_file).exists() else None))
    template = Path(__file__).with_name('motion_preview.html').read_text(encoding='utf-8')
    data = json.dumps(bundle, ensure_ascii=False, separators=(',', ':')).replace('<', '\\u003c')
    html = template.replace('/*__SCENE_DATA__*/', 'const sceneData = ' + data + ';')
    (root / '试听与轨迹.html').write_text(html, encoding='utf-8')
    lines = ['# 轨迹试听', '', '每段 30 秒。每条脚步轨迹固定一种地面；左右脚仅为交替节奏标记，原始素材没有左右脚标签。',
        '', '车辆启动、熄火来自客户端原声。发动机转速随车速/负载变化，但该部分为合成近似，未还原 REV `.model`。', '',
        f'[打开同步轨迹试听页](<{(root / "试听与轨迹.html").as_posix()}>)', '',
        '各段保持相同播放音量比较。自身位置为听者相对原点，方位不适用；近处外部声音仍标为外部。', '',
        '| 样本 | 内容 | 生成的方位与距离 | 方位/远近是否正确 | 动作/自身听感是否自然 | 备注 |', '|---|---|---|---|---|---|']
    for row in rows:
        descriptions = []
        for i, track in enumerate(row['tracks']):
            path = motion.trajectory(track['trajectory'], 30)
            if track.get('source_role') == 'self':
                descriptions.append(f'声源{i+1}：自身，方位不适用，位置0')
            else:
                descriptions.append(f'声源{i+1}：{round(path["azimuth"][0]) % 360}°→{round(path["azimuth"][-1]) % 360}°，距离{path["distance"].min():.1f}–{path["distance"].max():.1f}')
        lines.append(f'| [{row["id"]}]({(root / row["audio_file"]).as_posix()}) | {row["title"]} | {"；".join(descriptions)} |  |  |  |')
    lines.extend(['', '轨迹文件中的方位角：正前 0°、右侧 90°、正后 180°、左侧 270°。距离为合成单位，只用于粗分远近。', '',
        '音频、标签和轨迹已通过程序检查；表中听感项留空，未代填人工验收。'])
    if not (root / '试听索引.md').exists():
        (root / '试听索引.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    (root / '试听列表.m3u8').write_text('#EXTM3U\n' + '\n'.join(str(root / r['audio_file']) for r in rows) + '\n', encoding='utf-8')
    print(json.dumps(dict(player=str(root / '试听与轨迹.html'), samples=len(rows), bytes=len(html.encode('utf-8'))), ensure_ascii=False))


if __name__ == '__main__':
    main()
