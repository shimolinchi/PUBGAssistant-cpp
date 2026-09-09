"""Analytical architecture audit, not model execution or a hardware benchmark.

Run with Python 3: python docs/audio/audit_model_spec.py
"""
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = json.loads((ROOT / "docs/audio/model_v1.json").read_text(encoding="utf-8"))
rows = []
cache_elements = 0
branch_history = []


def record(name, shape, params, macs):
    rows.append(dict(name=name, output_shape=shape,
                     trainable_parameters=params, macs_per_feature_frame=macs))


for branch in spec["branches"]:
    c, f = branch["input_channels"], branch["input_frequency_bins"]
    history = 0
    for layer in branch["layers"]:
        kt, kf = layer["kernel"]
        st, sf = layer["stride"]
        assert st == 1 and kf % 2 == 1
        next_f = (f + 2 * (kf // 2) - kf) // sf + 1
        out_c = layer["out_channels"]
        cache_elements += (kt - 1) * c * f
        history += kt - 1
        if layer["type"] == "conv2d":
            weights = c * out_c * kt * kf
            normalization = 2 * out_c
        else:
            assert layer["type"] == "depthwise_pointwise"
            weights = c * kt * kf + c * out_c
            normalization = 2 * c + 2 * out_c
        record(branch["name"] + "." + layer["name"],
               ["B", out_c, "T", next_f], weights + normalization, weights * next_f)
        c, f = out_c, next_f
    out = branch["projection"]["out_features"]
    weights = c * f * out
    record(branch["name"] + ".projection", ["B", "T", out], weights + out, weights)
    branch_history.append(history)

levels = spec["frontend"]["level_features"]["channels"]
fusion_in = sum(b["projection"]["out_features"] for b in spec["branches"]) + levels
c = spec["fusion"]["out_features"]
record("fusion", ["B", "T", c], fusion_in * c + c, fusion_in * c)
temporal = spec["temporal"]
assert c == temporal["channels"]
h, k = temporal["expansion_channels"], temporal["kernel_size"]
for d in temporal["dilations"]:
    weights = c * k + c * h + h * c
    normalization = 2 * c + 2 * h + 2 * c
    cache_elements += (k - 1) * d * c
    record("tcn.dilation_" + str(d), ["B", "T", c], weights + normalization, weights)

class_count = len(spec["labels"]["classes"])
sector_count = len(spec["labels"]["sector_names_zh"])
assert class_count == 4 and sector_count == 8
for head in spec["heads"]:
    expected_classes = 3 if head['name'] == 'self_activity' else class_count
    assert head["output_dimensions_per_frame"][0] == expected_classes
    ci = c + (levels if head["append_level_features"] else 0)
    co = math.prod(head["output_dimensions_per_frame"])
    hidden = head["hidden_features"]
    weights = ci * hidden + hidden * co if hidden else ci * co
    bias = hidden + co if hidden else co
    record("head." + head["name"], ["B", "T"] + head["output_dimensions_per_frame"],
           weights + bias, weights)

past_frames = max(branch_history) + sum((k - 1) * d for d in temporal["dilations"])
stft = spec["frontend"]["stft"]
sr = spec["frontend"]["sample_rate_hz"]
hop = stft["hop_samples"]
params = sum(r["trainable_parameters"] for r in rows)
macs = sum(r["macs_per_feature_frame"] for r in rows)
assert sr / hop == 100
report = {
    "model_id": spec["model_id"],
    "audit_kind": "analytical_from_spec_not_framework_execution",
    "trainable_parameters": params,
    "fp32_trainable_weight_bytes": params * 4,
    "macs_per_feature_frame": macs,
    "macs_per_audio_second_with_state_cache": macs * sr // hop,
    "macs_per_four_new_frame_call": macs * 4,
    "past_feature_frames": past_frames,
    "receptive_field_feature_frames": past_frames + 1,
    "audio_receptive_span_ms": (stft["window_samples"] + past_frames * hop) / sr * 1000,
    "cnn_tcn_state_cache_float32_elements": cache_elements,
    "cnn_tcn_state_cache_bytes": cache_elements * 4,
    "excluded_from_macs": ["FFT and resampler", "mel projection", "normalization and activation", "memory movement", "postprocessing"],
    "excluded_from_weight_bytes": ["batch_norm_running_statistics", "serialization_metadata", "activations", "runtime_and_device_buffers"],
    "excluded_from_state_cache": ["frontend_resampler_and_STFT_state", "working_activations", "runtime_buffers"],
    "latency_and_accuracy": "not_measured; no_model_implementation_or_weights_yet",
    "layers": rows
}
(ROOT / "docs/audio/model_v1_audit.json").write_text(
    json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(json.dumps({k: v for k, v in report.items() if k != "layers"}, ensure_ascii=False, indent=2))
