"""Focused DSP, gait and moving-label properties; no subjective listening claims."""
import json
import math
from pathlib import Path
import unittest

import numpy as np

import generate_motion_dataset as gen
import motion_synthesis as m


ROOT = Path(__file__).resolve().parents[2]
SOURCES = ROOT / 'datasets/audio-synthetic-20260906-v2/sources'
HRIR = ROOT / 'datasets/audio-synthetic-20260906-v1/sources/hrir/kemar-diffuse.zip'


class MotionProperties(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sources = json.loads((SOURCES / 'sources.json').read_text(encoding='utf-8'))['sources']
        cls.by_id = {s['id']: s for s in cls.sources}
        cls.cfg = gen.configuration()
        cls.renderer = m.SpatialRenderer(HRIR)
        cls.library = m.SourceLibrary(SOURCES, cls.sources)

    def test_constant_velocity_integrates_in_cartesian_coordinates(self):
        spec = dict(initial_xy=[-10., 20.], speed_knots=[[0, 2.], [3, 2.]], heading_knots_unwrapped_degrees=[[0, 90], [3, 90]])
        path = m.trajectory(spec, 3)
        np.testing.assert_allclose(path['xy'][:, 0], -10 + 2 * path['time'], atol=1e-10)
        np.testing.assert_allclose(path['xy'][:, 1], 20, atol=1e-10)

    def test_walk_stop_resume_does_not_teleport_or_reset_feet(self):
        spec, _ = m.path_spec(0, 'high', 30, np.random.default_rng(5), 1, 0, demo=True)
        path = m.trajectory(spec, 30)
        for start, end in [(8.01, 9.99), (18.01, 19.99)]:
            region = (path['time'] >= start) & (path['time'] <= end)
            self.assertTrue(np.all(path['speed'][region] == 0))
            self.assertLess(float(np.ptp(path['xy'][region], axis=0).max()), 1e-9)
        roles = {r: [s['id'] for s in self.sources if s['source_group_id'] == 'footsteps:Concrete' and s['role'] == r] for r in ['walk', 'run', 'sprint']}
        events = m.schedule_footsteps(path, roles, np.random.default_rng(4))
        self.assertEqual({e['role'] for e in events}, {'walk', 'run', 'sprint'})
        self.assertEqual({self.by_id[e['source_id']]['source_group_id'] for e in events}, {'footsteps:Concrete'})
        for a, b in zip(events, events[1:]):
            self.assertNotEqual(a['simulated_foot'], b['simulated_foot'])
            self.assertGreaterEqual((b['onset_sample'] - a['onset_sample']) / m.RATE, .26)
        for e in events:
            self.assertGreater(path['speed'][round(e['onset_sample'] / m.RATE / m.DT)], .15)

    def test_seamless_loop_retains_phase_across_arbitrary_chunking(self):
        x = np.random.default_rng(0).normal(size=15000).astype(np.float32)
        cycle = m.seamless_cycle(x, 1000)
        p = np.cumsum(np.linspace(.96, 1.95, 48000))
        whole = m.read_cycle(cycle, p)
        pieces = np.r_[m.read_cycle(cycle, p[:17777]), m.read_cycle(cycle, p[17777:])]
        np.testing.assert_array_equal(whole, pieces)
        # The splice's first differences are bounded relative to the source.
        self.assertLess(np.max(np.abs(np.diff(np.r_[cycle, cycle[0]]))), 1.01 * np.max(np.abs(np.diff(x))))

    def test_static_dynamic_renderer_matches_direct_causal_convolution(self):
        rng = np.random.default_rng(77)
        x = rng.normal(0, .1, 13337).astype(np.float32)
        distance, azimuth = 30., 90.
        xy = [distance, 0.]
        path = dict(time=np.array([0., 1.]), xy=np.array([xy, xy]))
        actual = self.renderer.render(x, path)
        hrir = self.renderer.hrir_time[18]
        expected = np.column_stack([np.convolve(x, np.convolve(m.lowpass(7500), hrir[:, ear]))[:len(x)] for ear in range(2)]) * .5
        np.testing.assert_allclose(actual, expected, atol=4e-8, rtol=4e-6)
        # Includes block seams and a final partial block.
        self.assertTrue(np.isfinite(actual).all())

    def test_hrir_wrap_has_no_filter_step(self):
        angles = np.radians([359.9999, .0001])
        xy = 30 * np.column_stack([np.sin(angles), np.cos(angles)])
        filters = self.renderer.filters(xy)
        self.assertLess(np.max(np.abs(filters[0] - filters[1])), 1e-4)
        angles = np.radians([89.9999, 90.0001])
        filters = self.renderer.filters(30 * np.column_stack([np.sin(angles), np.cos(angles)]))
        self.assertLess(np.max(np.abs(filters[0] - filters[1])), 1e-4)

    def test_interpolated_left_right_hrirs_remain_mirrors(self):
        a = math.radians(43.7)
        filters = self.renderer.filters(np.array([[30 * math.sin(a), 30 * math.cos(a)], [-30 * math.sin(a), 30 * math.cos(a)]]))
        np.testing.assert_allclose(filters[0, :, ::-1], filters[1], atol=1e-12)

    def test_moving_labels_follow_instantaneous_position_and_mask_conflicts(self):
        seconds = 3
        cfg = dict(self.cfg, clip_seconds=seconds)
        x = np.full((seconds * m.RATE, 2), .1, np.float32)
        spec = dict(initial_xy=[-6., 6.], speed_knots=[[0, 4.], [seconds, 4.]], heading_knots_unwrapped_degrees=[[0, 90], [seconds, 90]])
        path = m.trajectory(spec, seconds)
        label = m.frame_labels([x], [dict(class_index=0)], [path], seconds, cfg)
        self.assertTrue(label['activity'][:30, 0, 7].all())
        self.assertTrue(label['activity'][-30:, 0, 1].all())
        self.assertTrue(label['activity'][140:160, 0, 0].all())
        self.assertFalse(label['track_engine_rpm_normalized'].any())
        self.assertFalse(label['track_brake_control'].any())
        self.assertTrue(np.all(label['track_gait_state'] == 2))
        np.testing.assert_allclose(label['track_xy'][0, :, 0], -6 + 4 * (label['frame_right_edge_seconds'] - .016 - 64 / m.RATE), atol=5e-7)
        static = dict(initial_xy=[0., 90.], speed_knots=[[0, 0], [seconds, 0]], heading_knots_unwrapped_degrees=[[0, 0], [seconds, 0]])
        far = m.trajectory(static, seconds)
        static['initial_xy'] = [0., 8.]
        near = m.trajectory(static, seconds)
        label = m.frame_labels([x, x], [dict(class_index=0), dict(class_index=0)], [far, near], seconds, cfg)
        self.assertTrue(np.all(label['range_index'][:, 0, 0] == -1))
        self.assertFalse(label['range_loss_mask'][:, 0, 0].any())
        self.assertFalse(label['activity_loss_mask'][:, 3].any())
        label = m.frame_labels([x, x * .0005], [dict(class_index=0), dict(class_index=0)], [far, path], seconds, cfg)
        self.assertFalse(label['activity_loss_mask'][:30, 0, 7].any())

    def test_native_vehicle_actions_match_leaf_names_not_directories(self):
        for s in self.sources:
            leaf = s['source_name'].rsplit('\\', 1)[-1].lower()
            if s['role'] in ['startup', 'shutdown']:
                self.assertIn(s['role'], leaf)
                self.assertIn('tpp', leaf)
            self.assertFalse(any(word in s['bank'].lower() for word in ['plane', 'aircraft', 'helicopter', 'glider']))

    def test_vehicle_controls_and_audio_survive_a_stop(self):
        split, groups = gen.source_pools(self.sources, self.cfg)
        rows = gen.preview_plan(self.sources, self.cfg, groups)
        vehicle = rows[2]['tracks'][0]
        path = m.trajectory(vehicle['trajectory'], 30)
        self.assertGreater(path['rpm'].max() - path['rpm'].min(), .6)
        self.assertGreater(path['brake'].max(), .9)
        dry = m.mono_track(vehicle, path, self.library, 30)
        stopped = (path['speed'] == 0) & (path['time'] > 10) & (path['time'] < 24)
        self.assertTrue(stopped.any())
        t = float(path['time'][stopped].mean())
        clip = dry[round((t - .15) * m.RATE):round((t + .15) * m.RATE)]
        self.assertGreater(float(np.sqrt(np.mean(clip ** 2))), .001)


if __name__ == '__main__':
    unittest.main(verbosity=2)
