"""V3 listener-bound own sounds, with independent external/self supervision.

Self identity is simulator provenance, never inferred from distance. External
rendering reuses the frozen v2 engine. Own footsteps and continuous engine are
approximations; native Local gunshots and FPP vehicle actions retain stereo.
"""
from functools import lru_cache

import numpy as np

import motion_synthesis as base
from motion_synthesis import RATE, DT, SourceLibrary as BaseLibrary
from motion_synthesis import SpatialRenderer, frame_powers, schedule_footsteps, mono_track


def path_spec(*args, **kwargs):
    spec, life = base.path_spec(*args, **kwargs)
    if spec['initial_range_index'] == 2:
        spec['initial_xy'] = (np.asarray(spec['initial_xy']) * 1.6).tolist()
    return spec, life


def trajectory(spec, seconds):
    m = base.trajectory(spec, seconds)
    if spec.get('listener_bound', False):
        m['xy'][:] = 0
        m['distance'][:] = 0
        m['azimuth'][:] = 0  # Invalid placeholder; direction validity is explicit.
        # speed drives gait/RPM. Relative-position speed is separately labelled 0.
    return m


class SourceLibrary(BaseLibrary):
    @lru_cache(maxsize=96)
    def stereo(self, sid):
        s = self.sources[sid]
        x = base.read_wave(self.root / s['wav_file'])
        if x.shape[1] == 1:
            x = np.repeat(x, 2, axis=1)
        x -= x.mean(axis=0, dtype=np.float64)
        cls = s['class_index']
        gain = min([.065, .085, .09][cls] / max(float(np.sqrt(np.mean(x.astype(np.float64)**2))), 1e-12),
                   [.4, .42, .55][cls] / max(float(np.max(np.abs(x))), 1e-12))
        x *= gain  # One shared gain preserves original inter-ear differences.
        for ear in range(2):
            base.edge_fade(x[:, ear], round(.001 * RATE))
        return x


def render_self(meta, path, library, seconds):
    n = round(seconds * RATE)
    x = np.zeros((n, 2), np.float32)
    if meta['class_index'] == 1:
        engine_meta = dict(meta, events=[], source_gain_jitter_db=0.)
        engine = base.mono_track(engine_meta, path, library, seconds)
        x += engine[:, None]
    for e in meta['events']:
        src = library.stereo(e['source_id'])
        onset = e['onset_sample']
        count = min(len(src), n - onset)
        if count > 0:
            x[onset:onset + count] += src[:count] * e.get('gain', 1.)
    x *= np.float32(10 ** (meta['source_gain_jitter_db'] / 20))
    for ear in range(2):
        base.edge_fade(x[:, ear], round(.004 * RATE))
    # Match the documented common delay without imposing an external direction.
    return np.pad(x, ((64, 0), (0, 0)))[:n]


def frame_labels(signals, metadata, motions, seconds, cfg, powers=None):
    if powers is None:
        powers = frame_powers(signals, seconds)
    z = base.frame_labels(None, metadata, motions, seconds, cfg, powers=powers)
    own = np.asarray([m.get('source_role') == 'self' for m in metadata], bool)
    count = len(z['frame_right_edge_seconds'])
    shape = (count, 4, 8)
    counts = np.zeros(shape, np.uint8)
    candidates = np.zeros(shape + (3,), np.uint8)
    cell_power = np.zeros(shape)
    own_counts = np.zeros((count, 3), np.uint8)
    own_power = np.zeros((count, 3))
    z['track_azimuth_degrees'] %= 360
    sectors = ((np.floor((z['track_azimuth_degrees'] + 22.5) / 45)).astype(np.int16) % 8).astype(np.int8)
    z['track_locomotion_speed'] = z['track_speed'].copy()
    for i, meta in enumerate(metadata):
        cls = meta['class_index']
        idx = np.flatnonzero(z['track_activity'][i])
        if own[i]:
            own_counts[idx, cls] += 1
            own_power[idx, cls] += powers[i, idx]
            z['track_xy'][i] = 0
            z['track_speed'][i] = 0
            z['track_distance_units'][i] = 0
            z['track_azimuth_degrees'][i] = 0
            z['track_range_index'][i] = -1
            sectors[i] = -1
        else:
            sector = sectors[i, idx]
            ranges = z['track_range_index'][i, idx]
            counts[idx, cls, sector] += 1
            candidates[idx, cls, sector, ranges] = 1
            cell_power[idx, cls, sector] += powers[i, idx]
    total = powers.sum(axis=0)
    observable = cell_power / np.maximum(total[:, None, None] - cell_power, 1e-15) >= 10 ** (-30 / 10)
    activity = (counts > 0).astype(np.uint8)
    mask = np.ones(shape, np.uint8)
    mask[:, 3] = 0
    mask[(activity != 0) & ~observable] = 0
    unique = candidates.sum(axis=-1) == 1
    ranges = np.full(shape, -1, np.int8)
    ranges[unique] = candidates.argmax(axis=-1)[unique]
    self_activity = (own_counts > 0).astype(np.uint8)
    self_observable = own_power / np.maximum(total[:, None] - own_power, 1e-15) >= 10 ** (-30 / 10)
    self_mask = np.ones((count, 3), np.uint8)
    self_mask[(self_activity != 0) & ~self_observable] = 0
    any_class = activity.max(axis=2)
    any_class[:, :3] |= self_activity
    # An auxiliary positive is trainable only if some corresponding source is observable.
    any_mask = mask.max(axis=2)
    any_mask[:, :3] = np.where(any_class[:, :3] != 0,
        ((activity * mask).max(axis=2)[:, :3] | (self_activity * self_mask)), 1)
    z.update(activity=activity, activity_loss_mask=mask, active_track_count=counts,
        range_index=ranges, range_loss_mask=(unique & (activity != 0) & observable).astype(np.uint8),
        self_activity=self_activity, self_activity_loss_mask=self_mask,
        any_class_activity=any_class, any_class_activity_loss_mask=any_mask,
        track_source_role=own.astype(np.uint8),  # 0 external, 1 listener/self
        track_direction_valid=np.broadcast_to((~own)[:, None], (len(own), count)).astype(np.uint8).copy(),
        track_sector_index=sectors)
    return z
