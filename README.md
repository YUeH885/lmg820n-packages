# LM-G820N 专用 Debian 软件包

本项目维护 LG G8 ThinQ (LM-G820N, Qualcomm SM8150) Linux 系统的专有软件包与驱动补丁，提供自动化构建流水线与 APT 软件源发布。

## 软件包清单

| 软件包 | 源码基底 | 说明 |
| :--- | :--- | :--- |
| `geoclue-2.0` | Debian sid `geoclue-2.0` | 原生应用桌面授权补丁与定位服务代理 |
| `libspa-0.2-bluetooth-lmg820n` | Debian sid `pipewire` | 蓝牙 SCO 语音传输状态通知插件 |
| `hexagonrpc` | Debian sid `hexagonrpc` | Hexagon DSP RPC 守护进程与运行库 |
| `libssc` | Debian sid `libssc` | 传感器子系统 (SSC) 通信库 |
| `iio-sensor-proxy` | Debian sid `iio-sensor-proxy` | 传感器热插拔与发现补丁 |
| `ffmpeg` / `libavcodec` | Debian sid `ffmpeg` | V4L2 状态机补丁与 DRM-PRIME 零拷贝支持 |
| `libcamera` | Debian sid `libcamera` | S5K3M5 属性、sensor helper、原生 Qcom Pipeline Handler、IFE 3A 统计、运行时 EEPROM 校准的 LG SFAF camera ID 3 模型、低照度搜索步长、粗扫停止与分段运动调度、16×16 测光与 12 节点曝光分配、AWB 与 CCM 色温插值、手动镜头位置与 VFE 多尺寸 NV12 输出 |
| `lmg820n-gnssd` | 本地源码 | SM8150 GNSS 硬件定位守护进程与授时服务 |
| `fcitx5-osk` | 上游源码 | Wayland 移动触摸虚拟键盘与快捷键服务 |

## 软件源使用配置

在 LM-G820N 设备（Debian sid）上执行以下命令配置软件源：

```bash
echo "deb [trusted=yes] https://lmg820n-packages.hwrng.dev sid main" | sudo tee /etc/apt/sources.list.d/lmg820n.list
sudo apt-get update
sudo apt-get install lmg820n-gnssd geoclue-2.0 hexagonrpcd libssc2 iio-sensor-proxy
```

## 本地构建

```bash
# 构建所有软件包
python3 tools/build-packages.py --all

# 构建指定软件包
python3 tools/build-packages.py geoclue

# 验证构建产物
python3 tools/build-packages.py --verify

# 生成 APT 软件源索引
python3 tools/make_repo.py --input build/dist --output repo
```

Debian 衍生包的本地修订号由补丁目录同级的 `revision` 文件声明，默认值为 `1`；发布配方更新时递增该值。

## 持续集成配置

GitHub Actions 工作流（`.github/workflows/build-and-publish.yml`）在原生 ARM64（`ubuntu-24.04-arm`）环境中的 `debian:sid` 容器执行构建，并在构建完成后同步至对象存储。

分发同步需在 GitHub 仓库设置中配置以下变量与密钥：

| 名称 | 类型 | 说明 |
| :--- | :--- | :--- |
| `R2_ACCESS_KEY_ID` | Secret | 对象存储访问密钥 ID |
| `R2_SECRET_ACCESS_KEY` | Secret | 对象存储访问私钥 |
| `R2_ENDPOINT_URL` | Variable / Secret | S3 兼容 API 端点 URL |
| `R2_BUCKET_NAME` | Variable / Secret | 存储桶名称 |
