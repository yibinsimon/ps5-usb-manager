# 图标资产

`usbmanage`（PS5 USB管理器）的应用图标，主文件为 `icon-512.png`。

## 文件与用途

| 文件 | 尺寸 | 用途 | 落点 |
|---|---|---|---|
| `icon-512.png` | 512×512 | **主文件**。归档用，不直接参与构建 | 归档 |
| `icon-192.png` | 192×192 | 大尺寸档 | 归档 |
| `icon-144.png` | 144×144 | 大尺寸档 | 归档 |
| `icon-96.png` | 96×96 | **PS5 payload 内置页面图标**（favicon + 页头 logo） | `device/ps5-usbmanage/web/icon.png` |
| `icon-96.png` | 96×96 | **PC 控制台页面图标**（favicon + 页头 logo） | `web/icon.png` |
| `icon-72.png` | 72×72 | 小尺寸档 | 归档 |
| `icon-48.png` | 48×48 | 小尺寸档 | 归档 |

> 同一个 96×96 文件同时供两个前端使用：它是 32–40 px 显示尺寸的 2–3 倍图，
> 足够覆盖高 DPI，体积只有 12.6 KB。

## 图形规格（实测值，改图时照此对齐）

- 格式 PNG，8 bit/通道，**RGBA**（带透明通道），非交错。
- **圆角外面的四角是完全透明**（`alpha=0`），圆角方块是图形本体——
  所以放到深色页面上不会出现白角，前端**不需要**再额外加 `border-radius`。
- 图形自带约 **4% 的透明外边距**：512 图上不透明包围盒是 `x[23..488] y[20..492]`；
  96 图上留白 3–4 px。这是 iOS/squircle 风格的标准留白，不要裁掉。
- 圆角半径约为图形边长的 **20%**（512 图上约 94 px）。

各尺寸均由主文件按 1:1 缩放，非重新绘制。

## 主文件指纹

```
sha256  d7365ba043069e6f065f0dac0b243475fcda66561016011e55a2f15b181eba45  icon-512.png
```

## 怎么用

```bash
# PS5 payload：图标会被编进 elf（build.sh 里做，见下）
cp assets/icon/icon-96.png device/ps5-usbmanage/web/icon.png
cd device/ps5-usbmanage && bash build.sh

# PC 控制台：server.py 直接从 web/ 目录读文件，不需要重新构建
cp assets/icon/icon-96.png web/icon.png
```

## 换图标时的注意事项

1. **改图就改主文件**，再从主文件重新生成各尺寸，不要单独改某一档。
2. **96×96 那份是两处共用的**（PS5 页面、PC 控制台），换图后要一起更新。
3. PS5 的 `build.sh` 会把 `web/icon.png` 用 `xxd` 转成 C 数组编进 elf，
   并断言长度与文件一致——**换图后必须重跑 `bash build.sh`**，否则 elf 里还是旧图标。
