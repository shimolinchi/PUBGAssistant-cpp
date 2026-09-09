"""Own identity, close external counterexamples, and listener-bound locomotion."""
import json
from pathlib import Path
import unittest

import numpy as np

import generate_self_audio_dataset as gen
import self_audio_synthesis as m

ROOT = Path(__file__).resolve().parents[2]


class SelfAudioProperties(unittest.TestCase):
    def labels(self, roles, distances, powers):
        seconds = 1
        paths = [m.trajectory(dict(initial_xy=[0, d], speed_knots=[[0, 0], [seconds, 0]],
            heading_knots_unwrapped_degrees=[[0, 0], [seconds, 0]], listener_bound=r == 'self'), seconds)
            for r, d in zip(roles, distances)]
        count = (32000 - 1024) // 320 + 1
        return m.frame_labels(None, [dict(class_index=0, source_role=r) for r in roles], paths, seconds,
            gen.configuration(), powers=np.broadcast_to(np.asarray(powers)[:, None], (len(roles), count)))

    def test_self_only_has_no_external_direction_target(self):
        z = self.labels(['self'], [0], [.01])
        self.assertTrue(z['self_activity'][:, 0].all())
        self.assertFalse(z['activity'].any())
        self.assertFalse(z['track_direction_valid'].any())
        self.assertTrue((z['track_sector_index'] == -1).all())
        self.assertTrue((z['track_range_index'] == -1).all())

    def test_point_blank_external_is_never_self(self):
        z = self.labels(['external'], [.5], [.01])
        self.assertFalse(z['self_activity'].any())
        self.assertTrue(z['activity'][:, 0, 0].all())
        self.assertTrue(z['track_direction_valid'].all())

    def test_self_and_external_same_class_coexist(self):
        z = self.labels(['self', 'external'], [0, 150], [.01, .001])
        self.assertTrue(z['self_activity'][:, 0].all())
        self.assertTrue(z['activity'][:, 0, 0].all())
        self.assertTrue((z['range_index'][:, 0, 0] == 2).all())
        self.assertTrue(z['range_loss_mask'][:, 0, 0].all())

    def test_own_masking_cannot_create_false_external_negatives(self):
        z = self.labels(['self', 'external'], [0, 150], [.1, .00001])
        self.assertTrue(z['activity'][:, 0, 0].all())
        self.assertFalse(z['activity_loss_mask'][:, 0, 0].any())
        self.assertFalse(z['range_loss_mask'][:, 0, 0].any())
        self.assertTrue(z['self_activity_loss_mask'][:, 0].all())

    def test_silent_self_does_not_suppress_close_external(self):
        z = self.labels(['self', 'external'], [0, 2], [0, .001])
        self.assertFalse(z['self_activity'].any())
        self.assertTrue(z['activity_loss_mask'][:, 0, 0].all())
        self.assertTrue(z['any_class_activity'][:, 0].all())

    def test_plans_preserve_surface_native_identity_and_own_motion(self):
        sources = json.loads((ROOT / 'datasets/audio-synthetic-20260906-v3/sources/sources.json').read_text(encoding='utf-8'))['sources']
        ids = {s['id']: s for s in sources}
        cfg = gen.configuration()
        split, groups = gen.source_pools(sources, cfg)
        cfg['split_clip_counts'] = dict(train=20, validation=20, test=20)
        rows = gen.plan(sources, cfg, split, groups)
        seen = set()
        for row in rows:
            for track in row['tracks']:
                own = track['source_role'] == 'self'
                path = m.trajectory(track['trajectory'], 30)
                if own:
                    seen.add(track['class_index'])
                    self.assertFalse(path['xy'].any())
                    if track['class_index'] in [0, 1]:
                        self.assertGreater(path['speed'].max(), 1)
                for e in track['events']:
                    s = ids[e['source_id']]
                    self.assertEqual(split['source_to_split'][s['id']], row['split'])
                    self.assertEqual(s['source_group_id'], track['source_group_id'])
                    if track['class_index'] == 2:
                        self.assertEqual(s['role'], 'local_shot' if own else 'shot')
                    if track['class_index'] == 1:
                        self.assertEqual(s['role'], e['role'] + ('_fpp' if own else ''))
        self.assertEqual(seen, {0, 1, 2})


if __name__ == '__main__':
    unittest.main(verbosity=2)
