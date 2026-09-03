# Runtime files

```text
onnxruntime.dll  ONNX Runtime 1.15 API 15
DirectML.dll     DirectML 1.10.1
PUBGV8.onnx      YOLOv8 head/body model, input 1x3x320x320
```

`DirectML.dll` 与 `onnxruntime.dll` 应位于同一目录。库通过 `LoadLibraryExW` 动态加载 ORT，
不需要 `onnxruntime.lib`；模型和 DLL 的位置由 `OrtDmlDetectorConfig` 明确传入。

DirectML 需要兼容的 D3D12 GPU/驱动；两套 DLL 均为 x64，并依赖相应的 VC++ runtime。
调用方应传绝对路径。模块的 `cmake --install` 规则不会自动安装这些 runtime 文件。

完整散列见同目录 `SHA256SUMS.txt`。
