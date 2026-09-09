"""Trajectory-first gait/vehicle synthesis and continuous binaural rendering.

NumPy only. Coordinates are synthetic scene units, not calibrated game metres.
Engine RPM rendering is explicitly an approximation from native idle recordings;
it is NOT a decoder or reproduction of the client's proprietary REV .model.
"""
from functools import lru_cache
import io
import math
from pathlib import Path
import wave
import zipfile

import numpy as np

from generate_mixed_dataset import edge_fade, fft_convolve, read_wave, sha_file


RATE = 44100
DT = .01


def smoothstep(x):
    x = np.clip(x, 0, 1)
    return x * x * (3 - 2 * x)


def curve(knots, times):
    """C1 interpolation, also valid for already-unwrapped heading angles."""
    k = np.asarray(knots, dtype=np.float64)
    i = np.clip(np.searchsorted(k[:, 0], times, side='right') - 1, 0, len(k) - 2)
    u = smoothstep((times - k[i, 0]) / (k[i + 1, 0] - k[i, 0]))
    return k[i, 1] + (k[i + 1, 1] - k[i, 1]) * u


def trajectory(spec, seconds):
    t = np.arange(round(seconds / DT) + 1, dtype=np.float64) * DT
    v = curve(spec['speed_knots'], t)
    heading = np.radians(curve(spec['heading_knots_unwrapped_degrees'], t))
    velocity = v[:, None] * np.column_stack((np.sin(heading), np.cos(heading)))
    xy = np.empty_like(velocity)
    xy[0] = spec['initial_xy']
    xy[1:] = xy[0] + np.cumsum((velocity[:-1] + velocity[1:]) * (.5 * DT), axis=0)
    acceleration = np.gradient(v, DT)
    distance = np.sqrt(np.sum(xy * xy, axis=1))
    azimuth = np.mod(np.degrees(np.arctan2(xy[:, 0], xy[:, 1])), 360)
    # Smoothing gear-related RPM changes is for audio only, never for position.
    gear = np.clip(1 + np.floor(v / 6), 1, 5).astype(np.int8)
    rpm_target = np.where(v < .15, 0, np.clip((v / gear) / 7.5, 0, 1))
    throttle = smoothstep((acceleration + .2) / 1.8) * smoothstep(v / .7)
    rpm_target = np.clip(rpm_target + .12 * throttle, 0, 1)
    rpm = np.zeros_like(v)
    rpm[0] = rpm_target[0]
    alpha = 1 - math.exp(-DT / .18)
    for j in range(1, len(v)):
        rpm[j] = rpm[j - 1] + alpha * (rpm_target[j] - rpm[j - 1])
    brake = smoothstep((-acceleration - .45) / 2.5) * smoothstep(v / .7)
    state = np.zeros(len(t), np.uint8)
    state[v > .15] = 2  # cruise
    state[(v > .15) & (acceleration > .25)] = 1
    state[(v > .15) & (acceleration < -.25)] = 3
    state[brake > .3] = 4
    return dict(time=t, xy=xy, speed=v, acceleration=acceleration, azimuth=azimuth,
                distance=distance, rpm=rpm, throttle=throttle, brake=brake, gear=gear,
                motion_state=state)


def path_spec(cls, density, seconds, rng, initial_range, azimuth, demo=False):
    """Finish an actor's movement plan before selecting/scheduling its audio."""
    radius = [9, 32, 105][initial_range] * float(rng.uniform(.88, 1.15))
    a = math.radians(azimuth)
    initial = [radius * math.sin(a), radius * math.cos(a)]
    heading = azimuth + float(rng.choice([-1, 1])) * rng.uniform(95, 145)
    headings = [[0., heading], [seconds * .5, heading + rng.uniform(-35, 35)],
                [seconds, heading + rng.uniform(-60, 60)]]
    if cls == 2:
        speeds = [[0., 0.], [seconds, 0.]]
        life = None
    elif cls == 0:
        speeds = [[0., 0.]]
        if density == 'low':
            begin = rng.uniform(1, seconds - 7)
            intervals = [(begin, begin + rng.uniform(2.0, 4.0), rng.uniform(1.3, 3.2))]
        elif density == 'medium':
            intervals = [(rng.uniform(.5, 2), seconds * .34, rng.uniform(1.4, 3.3)),
                         (seconds * .57, seconds * .83, rng.uniform(2.5, 5.5))]
        else:
            intervals = [(.35, seconds * .30, rng.uniform(1.6, 3.2)),
                         (seconds * .36, seconds * .63, rng.uniform(3.2, 5.8)),
                         (seconds * .68, seconds - .55, rng.uniform(2.0, 5.0))]
        if demo:
            intervals = [(1., 8., 1.7), (10., 18., 3.5), (20., seconds - 1., 5.4)]
        for begin, end, speed in intervals:
            ramp = min(.7, (end - begin) / 4)
            speeds.extend([[float(begin), 0.], [float(begin + ramp), float(speed)],
                           [float(end - ramp), float(speed)], [float(end), 0.]])
        speeds.append([seconds, 0.])
        life = None
    else:
        begin = float(rng.uniform(1, seconds - 14)) if density == 'low' else .3
        length = float(rng.uniform(10, 12)) if density == 'low' else seconds * .73 if density == 'medium' else seconds - .8
        end = min(seconds - .3, begin + length)
        # Startup must finish before motion. Shutdown follows the final stop.
        move = begin + 3.5
        stop = end - 2.6
        moving_seconds = stop - move
        peak_speed = float(rng.uniform(6, 13) if density == 'low' else rng.uniform(11, 21))
        times = np.asarray([0, .22, .40, .55, .64, .70, .83, 1.]) * moving_seconds + move
        values = [0, peak_speed, peak_speed, peak_speed * .48, 0, 0, peak_speed * .62, 0]
        speeds = [[0., 0.], [begin, 0.]] + [[float(t), float(v)] for t, v in zip(times, values)]
        speeds.extend([[end, 0.], [seconds, 0.]])
        life = dict(startup_seconds=begin, move_seconds=move, final_stop_seconds=stop,
                    shutdown_seconds=stop + .6, engine_end_seconds=end,
                    engine_backend='native_idle_variable_rate_and_load_filter_approximation',
                    rev_model_rendered=False)
    return dict(initial_xy=initial, speed_knots=speeds,
        heading_knots_unwrapped_degrees=headings, integration_step_seconds=DT,
        coordinate_units='synthetic_scene_units_not_calibrated_game_metres',
        initial_range_index=initial_range, initial_azimuth_degrees=azimuth), life


def schedule_footsteps(motion, by_role, rng):
    """Preserve gait phase and alternate simulated sides even across pauses."""
    speed = motion['speed']
    cadence = np.minimum(3.65, 1.55 * np.power(speed, .47))
    cadence[speed < .15] = 0
    phase = float(rng.uniform(.1, .6)) + np.cumsum(cadence) * DT
    indices = np.flatnonzero(np.diff(np.floor(phase), prepend=np.floor(phase[0])) > 0)
    pools = {}
    for role in ['walk', 'run', 'sprint']:
        ids = sorted(by_role[role])
        rng.shuffle(ids)
        if len(ids) < 2:
            raise ValueError('Each gait needs at least two native variations')
        pools[role] = [ids[::2], ids[1::2]]
    counters = {(role, side): 0 for role in pools for side in [0, 1]}
    first_side = int(rng.integers(0, 2))
    events = []
    for j, i in enumerate(indices):
        role = 'walk' if speed[i] < 2.4 else 'run' if speed[i] < 4.8 else 'sprint'
        side = (first_side + j) % 2
        ids = pools[role][side]
        k = counters[(role, side)]
        counters[(role, side)] += 1
        events.append(dict(source_id=ids[k % len(ids)], onset_sample=round(motion['time'][i] * RATE),
            gain=float(rng.uniform(.96, 1.04)), role=role, simulated_foot='left' if side == 0 else 'right',
            foot_side_provenance='alternating_variation_pools_not_native_left_right_labels'))
    return events


def lowpass(cutoff, taps=129):
    q = np.arange(taps) - (taps - 1) / 2
    h = 2 * cutoff / RATE * np.sinc(2 * cutoff / RATE * q) * np.kaiser(taps, 7.)
    return (h / h.sum()).astype(np.float32)


def seamless_cycle(x, crossfade_samples):
    """Overlap original tail/head without resetting phase at movement nodes."""
    fade = min(crossfade_samples, len(x) // 4)
    if fade < 2:
        raise ValueError('Loop too short')
    period = len(x) - fade
    y = x[:period].copy()
    w = smoothstep(np.arange(fade, dtype=np.float64) / fade)
    y[:fade] = x[period:] * (1 - w) + x[:fade] * w
    return y


def read_cycle(cycle, positions):
    p = np.mod(positions, len(cycle))
    left = np.floor(p).astype(np.int64)
    frac = p - left
    return (cycle[left] * (1 - frac) + cycle[(left + 1) % len(cycle)] * frac).astype(np.float32)


class SourceLibrary:
    def __init__(self, root, sources):
        self.root = Path(root)
        self.sources = {s['id']: s for s in sources}

    @lru_cache(maxsize=128)
    def mono(self, sid):
        s = self.sources[sid]
        x = read_wave(self.root / s['wav_file']).mean(axis=1)
        x -= x.mean(dtype=np.float64)
        cls = s['class_index']
        target = [.065, .085, .09][cls]
        cap = [.4, .42, .55][cls]
        gain = min(target / max(float(np.sqrt(np.mean(x.astype(np.float64) ** 2))), 1e-12),
                   cap / max(float(np.max(np.abs(x))), 1e-12))
        x *= gain
        if s['role'] not in ['idle', 'roll', 'brake', 'skid']:
            edge_fade(x, round(.001 * RATE))
        return x

    @lru_cache(maxsize=48)
    def cycle(self, sid, bandlimit=False):
        x = self.mono(sid)
        if bandlimit:
            # Playback rate stays below 2.1, so reject aliases before resampling.
            x = fft_convolve(x, lowpass(9500))[64:64 + len(x)]
        return seamless_cycle(x, round(.12 * RATE))


def mono_track(meta, motion, library, seconds):
    n = round(seconds * RATE)
    x = np.zeros(n, dtype=np.float32)
    for e in meta['events']:
        src = library.mono(e['source_id'])
        onset = e['onset_sample']
        count = min(len(src), n - onset)
        if count > 0:
            x[onset:onset + count] += src[:count] * e.get('gain', 1.)
    if meta['class_index'] == 1:
        times = np.arange(n, dtype=np.float64) / RATE
        interp = lambda name: np.interp(times, motion['time'], motion[name])
        rpm, throttle, speed, brake = (interp(k) for k in ['rpm', 'throttle', 'speed', 'brake'])
        life = meta['lifecycle']
        layers = meta['layers']
        # A single integrated source cursor persists through idle and all gear states.
        playback_rate = .96 + .83 * rpm + .20 * throttle
        position = meta['engine_phase_samples'] + np.cumsum(playback_rate)
        engine = read_cycle(library.cycle(layers['idle'], True), position)
        softer = fft_convolve(engine, lowpass(2000))[64:64 + n]
        engine = softer * (1 - throttle) + engine * throttle
        start = life['startup_seconds'] + .9
        envelope = smoothstep((times - start) / 1.2) * (1 - smoothstep((times - life['shutdown_seconds']) / .65))
        x += engine * ((.65 + .24 * rpm + .26 * throttle) * envelope).astype(np.float32)
        for role, sid in layers.items():
            if role == 'idle':
                continue
            if role == 'roll':
                env = .22 * smoothstep(speed / 8) * envelope
            else:
                # Coasting is mostly engine unload; strong friction accompanies braking.
                env = .8 * brake * envelope
            cycle = library.cycle(sid)
            layer = read_cycle(cycle, np.arange(n, dtype=np.float64) + meta['engine_phase_samples'] % len(cycle))
            x += layer * env.astype(np.float32)
    edge_fade(x, round(.004 * RATE))
    return x * np.float32(10 ** (meta['source_gain_jitter_db'] / 20))


class SpatialRenderer:
    """Time-varying FIR, shared input history, crossfaded at sample resolution.

    Interpolate 5-degree horizontal HRIRs, distance gain and lowpass continuously.
    Crossfade filter outputs over 20 ms; never reset history at a path node.
    The causal lowpass has a documented common 64-sample group delay.
    """
    def __init__(self, hrir, expected_hash=None):
        if expected_hash and sha_file(hrir) != expected_hash:
            raise ValueError('HRIR archive changed')
        self.block = 882
        self.history = 255
        self.nfft = 2048
        responses = []
        with zipfile.ZipFile(hrir) as z:
            for angle in range(0, 360, 5):
                a = min(angle, 360 - angle)
                with wave.open(io.BytesIO(z.read(f'elev0/H0e{a:03d}a.wav')), 'rb') as w:
                    if w.getframerate() != RATE or w.getnchannels() != 2 or w.getnframes() != 128:
                        raise ValueError('Unexpected HRIR')
                    h = np.frombuffer(w.readframes(128), dtype='<i2').reshape(-1, 2).astype(np.float64) / 32768
                responses.append(h[:, ::-1] if angle > 180 else h)
        responses = np.asarray(responses)
        self.reference_gain = 1 / np.sqrt(np.mean(np.sum(responses[0] ** 2, axis=0)))
        self.hrir_time = responses * self.reference_gain
        self.hrir_fft = np.fft.rfft(self.hrir_time, self.nfft, axis=1)
        self.cutoff_grid = np.unique(np.r_[np.geomspace(1200, 16000, 33), 3500., 7500., 16000.])
        self.lp_fft = np.fft.rfft(np.asarray([lowpass(fc) for fc in self.cutoff_grid]), self.nfft, axis=1)

    def filters(self, xy):
        az = np.mod(np.degrees(np.arctan2(xy[:, 0], xy[:, 1])), 360) / 5
        left = np.floor(az).astype(int) % 72
        f = (az - np.floor(az))[:, None, None]
        hrir = self.hrir_fft[left] * (1 - f) + self.hrir_fft[(left + 1) % 72] * f
        distance = np.maximum(np.sqrt(np.sum(xy * xy, axis=1)), 1.)
        # Preserve the accepted three anchor responses, interpolate between them.
        logd = np.log(distance)
        gain = np.exp(np.interp(logd, np.log([1, 8, 30, 90, 400]), np.log([1.15, 1, .5, .22, .045])))
        cutoff = np.exp(np.interp(logd, np.log([1, 8, 30, 90, 400]), np.log([16000, 16000, 7500, 3500, 1200])))
        l = np.clip(np.searchsorted(self.cutoff_grid, cutoff, side='right') - 1, 0, len(self.cutoff_grid) - 2)
        u = ((np.log(cutoff) - np.log(self.cutoff_grid[l])) /
             (np.log(self.cutoff_grid[l + 1]) - np.log(self.cutoff_grid[l])))[:, None]
        lp = self.lp_fft[l] * (1 - u) + self.lp_fft[l + 1] * u
        return hrir * lp[:, :, None] * gain[:, None, None]

    def render(self, mono, motion):
        n = len(mono)
        blocks = (n + self.block - 1) // self.block
        padded = np.pad(mono, (self.history, blocks * self.block - n))
        views = np.lib.stride_tricks.sliding_window_view(padded, self.history + self.block)[::self.block]
        boundaries = np.arange(blocks + 1) * self.block / RATE
        xy = np.column_stack([np.interp(boundaries, motion['time'], motion['xy'][:, k]) for k in range(2)])
        blend = (np.arange(self.block) / self.block)[None, :, None]
        out = np.empty((blocks * self.block, 2), np.float32)
        for start in range(0, blocks, 24):
            stop = min(start + 24, blocks)
            response = self.filters(xy[start:stop + 1])
            spectrum = np.fft.rfft(views[start:stop], self.nfft, axis=1)
            first = np.fft.irfft(spectrum[:, :, None] * response[:-1], self.nfft, axis=1)[:, self.history:self.history + self.block]
            last = np.fft.irfft(spectrum[:, :, None] * response[1:], self.nfft, axis=1)[:, self.history:self.history + self.block]
            out[start * self.block:stop * self.block] = (first * (1 - blend) + last * blend).reshape(-1, 2)
        return out[:n]


def frame_powers(signals, seconds):
    count = (round(seconds * 32000) - 1024) // 320 + 1
    ends = (1024 + np.arange(count) * 320) / 32000
    left = np.rint((ends - 1024 / 32000) * RATE).astype(int)
    right = np.rint(ends * RATE).astype(int)
    powers = []
    for x in signals:
        integrated = np.r_[0., np.cumsum(np.mean(x.astype(np.float64) ** 2, axis=1))]
        powers.append(np.maximum(0, (integrated[right] - integrated[left]) / (right - left)))
    return np.asarray(powers)


def frame_labels(signals, metadata, motions, seconds, cfg, powers=None):
    count = (round(seconds * 32000) - 1024) // 320 + 1
    ends = (1024 + np.arange(count) * 320) / 32000
    if powers is None:
        powers = frame_powers(signals, seconds)
    if powers.shape != (len(metadata), count) or not np.isfinite(powers).all() or np.any(powers < 0):
        raise ValueError('Invalid isolated-track frame powers')
    active = powers >= np.maximum(powers.max(axis=1)[:, None] * 10 ** (-35 / 10), 10 ** (-90 / 10))
    shape = (count, 4, 8)
    tracks = np.zeros(shape, np.uint8)
    candidates = np.zeros(shape + (3,), np.uint8)
    cell_power = np.zeros(shape, np.float64)
    centres = ends - .016 - 64 / RATE
    positions, speeds, azimuths, distances, ranges_all, states, rpms, brakes, engine_states, gait_states = [], [], [], [], [], [], [], [], [], []
    for i, (meta, m) in enumerate(zip(metadata, motions)):
        xy = np.column_stack([np.interp(centres, m['time'], m['xy'][:, k]) for k in range(2)])
        distance = np.sqrt(np.sum(xy ** 2, axis=1))
        azimuth = np.mod(np.degrees(np.arctan2(xy[:, 0], xy[:, 1])), 360)
        sector = (np.floor((azimuth + 22.5) / 45).astype(int)) % 8
        ranges = np.digitize(distance, [math.sqrt(8 * 30), math.sqrt(30 * 90)])
        cls = meta['class_index']
        indices = np.flatnonzero(active[i])
        tracks[indices, cls, sector[indices]] += 1
        candidates[indices, cls, sector[indices], ranges[indices]] = 1
        cell_power[indices, cls, sector[indices]] += powers[i, indices]
        positions.append(xy); azimuths.append(azimuth); distances.append(distance); ranges_all.append(ranges)
        speeds.append(np.interp(centres, m['time'], m['speed']))
        nearest = np.clip(np.rint(centres / DT).astype(int), 0, len(m['time']) - 1)
        states.append(m['motion_state'][nearest])
        rpms.append(m['rpm'][nearest] if cls == 1 else np.zeros(count))
        brakes.append(m['brake'][nearest] if cls == 1 else np.zeros(count))
        gait = np.zeros(count, np.uint8)
        if cls == 0:
            gait = np.digitize(speeds[-1], [.15, 2.4, 4.8]).astype(np.uint8)
        gait_states.append(gait)
        engine = np.zeros(count, np.uint8)
        if cls == 1:
            life = meta['lifecycle']
            active_engine = (centres >= life['startup_seconds']) & (centres < life['shutdown_seconds'])
            # 0 off, 1 starting, 2 idle, 3 accelerating, 4 cruising,
            # 5 coasting/decelerating, 6 braking, 7 shutting down.
            engine[active_engine] = np.asarray([2, 3, 4, 5, 6], np.uint8)[m['motion_state'][nearest][active_engine]]
            engine[active_engine & (centres < life['move_seconds'])] = 1
            engine[(centres >= life['shutdown_seconds']) & (centres < life['engine_end_seconds'])] = 7
        engine_states.append(engine)
    activity = (tracks > 0).astype(np.uint8)
    total = powers.sum(axis=0)[:, None, None]
    observable = cell_power / np.maximum(total - cell_power, 1e-15) >= 10 ** (-30 / 10)
    activity_mask = np.ones(shape, np.uint8)
    activity_mask[:, 3, :] = 0
    activity_mask[(activity != 0) & ~observable] = 0
    unique = candidates.sum(axis=-1) == 1
    ranges = np.full(shape, -1, np.int8)
    ranges[unique] = candidates.argmax(axis=-1)[unique]
    return dict(activity=activity, activity_loss_mask=activity_mask,
        range_index=ranges, range_loss_mask=(unique & (activity != 0) & observable).astype(np.uint8),
        active_track_count=tracks, frame_right_edge_seconds=ends,
        class_supervision_mask=np.asarray([1, 1, 1, 0], np.uint8),
        track_xy=np.asarray(positions, np.float32), track_speed=np.asarray(speeds, np.float32),
        track_azimuth_degrees=np.asarray(azimuths, np.float32),
        track_distance_units=np.asarray(distances, np.float32),
        track_range_index=np.asarray(ranges_all, np.int8), track_activity=active.astype(np.uint8),
        track_class_index=np.asarray([m['class_index'] for m in metadata], np.uint8),
        track_motion_state=np.asarray(states, np.uint8),
        track_engine_state=np.asarray(engine_states, np.uint8),
        track_gait_state=np.asarray(gait_states, np.uint8),
        track_engine_rpm_normalized=np.asarray(rpms, np.float32),
        track_brake_control=np.asarray(brakes, np.float32))
