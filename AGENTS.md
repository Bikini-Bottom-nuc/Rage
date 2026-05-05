# Project Notes for Future Agents

本文件记录在 `D:\1.1.1.1.1` 项目中已经确认过的事实、约束和常见坑。后续回答本项目问题或改代码时，先读本文件，再检查实际文件状态。

## 基本工作规则

- 回答本地项目问题时，先检查实际文件结构、入口文件、构建脚本和当前文件内容，再给运行命令。
- 数据集问题要先统计文件数量、标注格式和标签类别，再判断能不能直接训练。
- 默认使用 PowerShell 命令，除非用户明确在 Linux/SDK 容器中操作。
- 不要修改原始数据集文件。所有转换、清洗、训练产物都放到 `field_nav_workspace` 或新建目录。
- 尽量不改原 SDK 源码。田间导航项目代码应优先放在 `smartsens_sdk\field_nav_external`。
- 用户强调过：不要修改现有头文件，尤其不要改 SDK 原有 `.h/.hpp`。如果必须改头文件，先说明原因并征求确认。
- 手动编辑文件使用 `apply_patch`，不要用 shell 重定向或脚本直接覆盖源码。
- 可能存在脏工作区和 Buildroot 生成文件，不要还原用户或构建系统已有改动。

## 关键路径

- 工作区根目录：`D:\1.1.1.1.1`
- SDK 根目录：`D:\1.1.1.1.1\data\A1_SDK_SC132GS\smartsens_sdk`
- 田间导航训练工作区：`D:\1.1.1.1.1\field_nav_workspace`
- 原始 LabelMe 数据集：`D:\1.1.1.1.1\智慧农业田垄采摘机器人道路识别农作物过道区域识别分割数据集labelme格式211张2类别`
- 数据集实际文件夹：`...\labelme_data`
- Buildroot external：`D:\1.1.1.1.1\data\A1_SDK_SC132GS\smartsens_sdk\field_nav_external`
- 板端 demo 源码：`field_nav_external\src\field_nav_demo`
- 板端模型位置：`field_nav_external\src\field_nav_demo\app_assets\models\navroad_640x480.m1model`
- 最终烧录产物：`D:\1.1.1.1.1\data\A1_SDK_SC132GS\smartsens_sdk\output\images\zImage.smartsens-m1-evb`
- Buildroot 旧缓存目录：`smartsens_sdk\output\build\field_nav_demo`

## A1 开发板参数

- 芯片：Flyingchip A1，面向端侧视觉处理。
- CPU：单核 ARM Cortex-A7，最高约 1.2GHz。
- NPU：0.8TOPS@INT8。
- 内存：DDR3L 16bit 1Gb stacked。
- 存储：256Mb NOR Flash。
- 外设：SPI、I2C、UART、GPIO 等。
- 视频接口：2 x 4-lane MIPI CSI RX，1 x 4-lane MIPI CSI TX。
- ISP：支持双路 3MP 30fps HDR、单路 3MP 60fps HDR、单路 5MP 60fps RGB-IR、单路 8MP 30fps HDR。
- 电源：5V DC。
- 工作温度：0~65C。
- 程序运行方式：编译生成 `zImage.smartsens-m1-evb`，烧录到板子运行。

## 数据集事实

- 数据集是 LabelMe polygon 分割数据。
- 已确认 `labelme_data` 下有 `211` 张 `.jpg` 和 `211` 个 `.json`，配对完整。
- 标签类别是 `sand_road` 和 `grassy_road`。
- 两个类别在当前导航任务中都表示可通行区域，训练时通常合并为一个前景类 `road` / `road_area`。
- 数据集没有现成 mask，需要先把 LabelMe JSON polygon 转成二值 mask。
- LabelMe JSON 是 LabelMe 标注工具保存的标注文件，包含图片名、尺寸、标签名、polygon 点坐标等。
- 不要把 polygon 面标注直接当成导航线。正确流程是：图像 -> 分割模型 -> road mask -> 后处理提取中心线。
- 数据集版权声明含个人使用限制，涉及公开发布或商业用途时要先检查授权。

## 训练工作区

- `field_nav_workspace` 只读原始数据集，写派生数据、清洗副本、模型、报告。
- v1 主要脚本：
  - `tools\prepare_labelme_dataset.py`
  - `tools\train_navroad.py`
  - `tools\evaluate_navroad.py`
- v2 质量提升脚本：
  - `tools\audit_labelme_dataset_v2.py`
  - `tools\prepare_labelme_dataset_v2.py`
  - `tools\train_navroad_v2.py`
  - `tools\evaluate_navroad_v2.py`
  - `tools\compare_onnx_navroad_v2.py`
  - `tools\prove_navroad_host.py`
- v2 流程会生成 `audit_v2`、`labelme_curated_v2`、`processed_v2_640x480`、`runs\navroad_v2` 等派生文件。
- 只允许人工修改 `field_nav_workspace\data\labelme_curated_v2` 里的副本，不要改原始 LabelMe 数据。
- 模型输入约定：灰度 `1x480x640`。
- 推荐输出：低分辨率 road 概率图，常见为 `1x120x160`。
- `120x160` 概率图不是原图，而是模型输出。每个点表示对应区域属于可通行过道的概率，再用阈值转为二值 mask。

## 模型和转换

- 当前板端模型文件名约定为 `navroad_640x480.m1model`，但名字本身不是编译硬要求；真正要求是运行脚本和打包路径一致。
- 当前板端默认模型路径：`/field_nav/app_assets/models/navroad_640x480.m1model`。
- 用户板上确认过模型大小约 `616750` 字节。
- 训练导出 ONNX 后，需要使用 A1 工具链转换为 `.m1model`。
- A1 AI Tool 支持的 ONNX 算子有限。模型应避免把复杂后处理放进 ONNX，后处理放 CPU 更稳。
- 推荐网络结构要保持简单：Conv、Pool、BatchNorm、Add、Mul、Concat、Relu、LeakyRelu、nearest resize/upsample 等。
- 避免在 ONNX 中依赖 Softmax、Sub、Div、NMS、复杂 Transpose 等不确定支持的后处理。

## 板端应用 field_nav_demo

- 不覆盖原有人脸 demo，新增/维护 `field_nav_demo`。
- Buildroot external package 位于 `field_nav_external\package\field_nav_demo`。
- `CMakeLists.txt` 复用 face_detection demo 的 `osd-device.cpp`，OSD 应借鉴人脸 demo 的 `OsdDevice` 链路。
- `field_nav.hpp` 是当前板端 demo 头文件，用户要求不要随意修改。
- `navline_detector.cpp` 负责模型加载、推理输出解析和导航线后处理。
- `image_processor.cpp` 负责摄像头图像链路。
- `osd_overlay.cpp` 负责 OSD 显示。
- `main.cpp` 负责参数、主循环、OSD、UART 输出等。
- `scripts\run.sh` 传入默认模型、LUT、UART 参数。

## 图像和坐标约定

- 传感器/原始尺寸常见为 `720x1280`。
- 当前板端处理思路：`720x1280 -> crop 720x540 offset_y=370 -> resize 640x480`。
- 模型输入是 `640x480` 灰度图。
- 模型输出通常是 `120x160` 概率图，但代码应读取 runtime tensor 宽高，不要硬编码输出尺寸。
- 导航点最终要映射回原画面坐标，用于 OSD 和 UART 输出。

## 原始导航线算法

旧版 `navline_detector.cpp` 已存在，不是后来新建的。旧算法流程：

- 解析模型输出为 `0~1` 概率值。
- 固定阈值 `0.45`。
- 从画面底部向上扫描到高度 `35%`。
- 每隔 `height / 30` 扫描一行。
- 每行找概率最高、宽度足够的连续前景段。
- 对该段做概率加权中心点。
- 至少需要 6 个点。
- 用最小二乘法拟合直线。
- 输出 `bottom_x`、`deviation_px`、`angle_deg`、`confidence`、`valid`。

旧算法没有：形态学修补、连通域过滤、贯通域匹配、行带中心点、历史帧兜底。

## 当前论文升级后的导航线算法

已按论文 TDM-LS 思路升级 `navline_detector.cpp`，只改 `.cpp`，不改头文件：

- 模型输出概率图后，使用阈值 `0.45` 得到二值 mask。
- 在输出 mask 下方约 `65%` 区域做处理，减少远处噪声影响。
- 使用纵向小核形态学修补断裂区域。
- 做连通域标记，过滤小区域。
- 选择主贯通域：优先触底、纵向跨度大、面积大、靠近上一帧或图像中心。
- 改为水平行带中心点提取，行带高度默认 `4` 个 mask 像素。
- 对中心点做最小二乘直线拟合。
- 成功时日志 reason 为 `ok_tdm_ls`。
- 短暂失败时最多 2 帧使用上一帧导航线低置信度兜底。
- 连续失败后输出 `valid=0`。
- 新日志字段包括 `components`、`main_area`、`band_points`、`fallback`、`failure`。

## OSD 经验

- OSD 应复用人脸 demo 已验证的 `OsdDevice` 链路，不要手写未验证的 OSD 初始化。
- `--lut` 是 OSD 颜色查找表路径。
- 默认优先使用 `/field_nav/app_assets/shared_colorLUT.sscl`。
- 备用 LUT：`/field_nav/app_assets/colorLUT.sscl`。
- 用户板上确认过 `shared_colorLUT.sscl` 约 98 字节，`colorLUT.sscl` 约 71 字节。
- 程序启动后应先画 3 秒固定测试框。固定框在 Aurora 中间摄像头画面上显示，不在左侧串口文本区。
- 串口日志在 Aurora 左侧 UART 接收数据窗口显示。
- 如果固定框可见但导航线不可见，优先看 `valid`、`components`、`points`、`reason` 日志。

## 模型加载坑

- `ssne_loadmodel()` 返回的 `model_id` 可能是 `0`，不能把 `model_id_ == 0` 当成失败。
- 正确做法是加载后调用 `ssne_get_model_input_num(model_id_)` 等 API 查询，返回值异常才算失败。
- 这点已参考人脸 demo 方式修正过。

## 模型释放

- 项目没有单独的 `ssne_unloadmodel(model_id)` 调用。`field_nav_demo` 在退出或初始化失败时调用 `ssne_release()` 释放 SSNE 运行时资源。
- `NavLineDetector::Release()` 只释放导航模型相关的输入/输出 tensor 和 AI 预处理管线：`release_tensor(output_)`、`release_tensor(input_)`、`ReleaseAIPreprocessPipe(preprocess_)`。
- 正常退出顺序是先 `overlay.Release()`、`nav_uart.Release()`、`detector.Release()`、`processor.Release()`，最后调用 `ssne_release()`。
- release 的作用是把 NPU/SSNE 运行时、AI buffer、tensor 内存和预处理管线句柄还给系统，避免程序退出或初始化失败时残留资源、内存泄漏、下次启动占用失败。
- 在板端长期运行或反复重启 demo 时，释放顺序很重要：先释放业务层对象持有的 tensor/pipe/设备句柄，再释放底层 `ssne_release()`。

## Buildroot 和构建

- 正式构建在 Linux SDK 容器中执行，不要指望 Windows PowerShell 直接完整编译 Buildroot。
- 构建命令：

```bash
cd /home/smartsens_flying_chip_a1_sdk/A1_SDK_SC132GS/smartsens_sdk
bash ./field_nav_external/scripts/build_field_nav.sh
```

- 构建脚本会执行：
  - `build_dl.sh`
  - 检查/解压 toolchain
  - 检查/解压 package
  - 检查/解压 kernel src
  - `make BR2_EXTERNAL=./smart_software:./field_nav_external field_nav_m1pro_defconfig`
  - `make ... field_nav_demo-dirclean`
  - `make -j$(nproc)`
- `field_nav_demo-dirclean` 很重要，用于避免 Buildroot 使用 `output\build\field_nav_demo` 旧缓存。
- Windows 当前环境可能出现 WSL `E_ACCESSDENIED` 或 `/bin/bash` 不存在，Docker daemon 也可能未运行。遇到这种情况不要判断源码编译失败，要回到 Linux/SDK 容器构建。
- 最终烧录文件是 `output\images\zImage.smartsens-m1-evb`。

## Buildroot 旧缓存含义

- `output\build\field_nav_demo` 是 Buildroot 把 package 解包/复制后编译的缓存目录。
- 修改源目录 `field_nav_external\src\field_nav_demo` 后，如果不 dirclean，Buildroot 可能继续编译旧缓存。
- 判断源码以 `field_nav_external` 为准，缓存目录只能作为旧版本参考。

## 启动参数

- `run.sh` 是板端程序默认启动参数来源。
- `--model`：模型路径，默认 `/field_nav/app_assets/models/navroad_640x480.m1model`。
- `--lut`：OSD 颜色 LUT 文件路径。
- `--nav-uart`：A1 侧导航 UART 输出开关/编号。
- `--nav-baud`：导航串口波特率，默认 `115200`。
- `--nav-rate`：导航帧发送频率，默认 `10Hz`。
- `--sensor-fps`：记录比赛验证时的目标传感器帧率，只作为日志目标值，不单独重配传感器。
- `--osd-rate`：OSD 绘制频率，默认 `15Hz`。设置为 `0` 可关闭运行中 OSD 绘制，用于排查 OSD 是否拖慢 `FPS_app`。
- `--test-seconds`：板端固定时长运行测试，`>0` 时达到指定秒数后打印 final metrics 并退出。
- 参赛证据采集命令：

```sh
FIELD_NAV_UART=1 FIELD_NAV_BAUD=115200 FIELD_NAV_RATE=90 FIELD_NAV_SENSOR_FPS=90 FIELD_NAV_OSD_RATE=15 FIELD_NAV_TEST_SECONDS=60 /field_nav/scripts/run.sh
```

- `FIELD_NAV_SENSOR_FPS=90` 只是把目标帧率写进日志，不能单独证明传感器已配置到 90fps。
- `FIELD_NAV_OSD_RATE=15` 只限制 OSD 刷新频率，不影响 NPU 每帧推理和 UART 90Hz 导航输出。若 `osd_ms` 高，可用 `FIELD_NAV_OSD_RATE=0` 做无 OSD 性能复测。

## UART / GPIO / RDK X5 导航链路

- UART 是通用异步串口，用于低速稳定传输导航数据。
- A1 GPIO 可用情况要按赛题区分。赛题 1/2 可用 GPIO：0、2、8、9、10。赛题 3 可用 GPIO：0、2、8、10。
- GPIO0 默认可复用为 UART TX0，GPIO1 默认可复用为 UART RX0，但 GPIO1 已占用，不能随意改。
- A1 侧推荐只用 UART TX 输出导航结果，不直接控制车轮。
- A1 UART 电平是 1.8V，RDK X5 40Pin UART 是 3.3V，A1 -> RDK 必须加 1.8V 到 3.3V 电平转换。
- 推荐硬件链路：
  - A1 P4-15 / A1_D0_UART0TX -> 电平转换 -> RDK X5 40Pin Pin10 / UART_RXD
  - RDK X5 40Pin Pin8 / UART_TXD -> 下位机 UART_RX
  - A1、RDK X5、下位机必须共地
- 不要用 RDK X5 Micro-USB 调试串口做导航数据通道，它主要用于系统登录和调试。
- RDK X5 侧脚本：`field_nav_external\scripts\rdk_x5_nav_bridge.py`。
- RDK X5 侧先确认串口设备：

```bash
ls -l /dev/ttyS* /dev/ttyUSB* /dev/ttyACM*
dmesg | grep -i tty
```

- RDK 运行示例：

```bash
python3 rdk_x5_nav_bridge.py --port /dev/ttyS1 --baud 115200
```

## 导航帧和控制思路

- A1 每约 100ms 发送一帧导航数据。
- A1 导航数据包括：`valid`、`deviation_px`、`angle_deg`、`confidence`、`bottom_x` 等。
- RDK X5 接收后计算线速度和角速度，再发给下位机。
- 下位机只执行 RDK 的控制指令，不直接解析原始图像或模型输出。
- 如果下位机超过 500ms 未收到有效控制帧，应停车。
- 如果以后需要下位机回传编码器、电池、电机状态，升级为双串口或 USB 转串口链路。

## 串口和 Aurora 软件

- Aurora 左侧 UART 窗口显示 Linux 启动日志和程序 printf 日志。
- 中间 camera/device 画面显示摄像头图像和 OSD 叠加框/线。
- 启动日志如果只看到 kernel 和模块日志，没有 field_nav 日志，要检查程序是否自启动、run.sh 是否存在、模型是否打包。
- 如果日志出现 `ssne_loadmodel failed`，先检查模型路径、文件大小、权限，再检查是否误判 `model_id == 0`。

## 常见诊断命令

板端检查模型和 LUT：

```sh
ls -l /field_nav/app_assets/models/
wc -c /field_nav/app_assets/models/navroad_640x480.m1model
ls -l /field_nav/app_assets/shared_colorLUT.sscl
ls -l /field_nav/app_assets/colorLUT.sscl
```

查看是否打包了导航资源：

```sh
ls -R /field_nav
cat /field_nav/scripts/run.sh
```

RDK X5 串口检查：

```bash
ls -l /dev/ttyS* /dev/ttyUSB* /dev/ttyACM*
dmesg | grep -i tty
```

## 常见日志解释

- `output tensor width=... height=... dtype=...`：模型输出尺寸和类型，正常时会打印一次。
- `prob=[min,max,mean]`：模型输出概率范围。
- `metrics tag=heartbeat window=60s ...`：板端 60 秒滚动统计，每秒打印一次。
- `FPS_app`：应用主循环实测帧率，判断是否接近 90fps 要看这个值和实际传感器模式。
- `fps_ratio`：`FPS_app / target_sensor_fps`，若目标是 90fps 且该值明显低于 1，不能声称满足 90fps 性能项。
- `image_ms=[avg,p95,max]`：取图/ISP pipeline 阶段耗时。如果 P95 接近 33ms，说明实际传感器/ISP 链路仍接近 30fps，单靠应用代码无法达到 90fps。
- `predict_ms=[avg,p95,max]`：AI 预处理、NPU 推理、取输出和后处理整体耗时。若明显超过 8ms，应继续看 `preprocess_ms`、`inference_ms`、`getoutput_ms`、`postprocess_ms` 分段日志。
- `uart_ms=[avg,p95,max]`、`osd_ms=[avg,p95,max]`：输出侧耗时。OSD 高时先降 `FIELD_NAV_OSD_RATE` 或设为 `0` 复测；UART 高时可评估 `921600` 波特率，但 RDK 端必须同步。
- `P95_frame_ms`：当前窗口帧处理耗时 P95。
- `max_frame_ms`：当前窗口最慢帧耗时。
- `valid_nav`、`no_line`、`predict_fail`、`image_fail`：有效导航、无有效导航线、AI/预处理失败、摄像头取帧失败计数。
- `uart_sent`、`uart_fail`：导航 UART 成功发送和失败计数。
- `max_invalid_ms`：当前窗口最长连续无效导航时间；鲁棒性测试中超过 `1000ms` 要如实记录并扣分/优化。
- `components=0`：后处理没有找到有效连通域，可能是模型没有分割出道路或阈值太高。
- `main_area` 很小：主连通域面积不足，可能是画面不对、模型输出弱、阈值过高。
- `band_points < 6`：行带中心点不足，直线拟合不会生效。
- `reason=ok_tdm_ls`：当前 TDM-LS 后处理成功。
- `reason=fallback_last_valid`：当前帧失败，短时间沿用上一帧导航线。
- `valid=0`：当前没有可靠导航线，下游应停车或保持安全状态。

## 论文算法筛选结论

- 适合 A1 的论文算法：形态学修补、小连通域过滤、行带中心点提取、贯通域匹配 TDM、最小二乘 LS、历史帧兜底。
- 不建议直接采用：YCrCb + 2Cg-Cr-Cb + Otsu 颜色分割作为主流程、完整纵向搜索填充模型原样照搬、DBSCAN/Hough 作为主算法、在 640x480/720x540 上做重型轮廓处理。
- 正确方向：保留 NPU 分割模型，把论文的 TDM-LS 思路放在 CPU 后处理上，并尽量在 `120x160` 输出 mask 上完成。

## 代码风格和实现偏好

- 多借鉴原有人脸 demo，尤其是 OSD、模型加载、库链接、run.sh 风格。
- 尽量把新逻辑放在 `.cpp` 内部匿名命名空间，减少头文件和接口变化。
- 板端 CPU 是 Cortex-A7，后处理要轻量，优先处理低分辨率 mask。
- 不要引入 OpenCV 等重依赖到板端 demo，除非 SDK 已经明确支持并且用户同意。
- C++ 目标是 C++11，避免使用 C++17 特性。
- 日志要能直接在串口定位问题，但不要每帧打印大量内容。保持间隔诊断。

## 验证要求

- 完成代码改动后，至少检查：
  - 是否改了 `.h/.hpp`
  - `field_nav_external` 源文件是否是实际修改目标
  - Buildroot 是否执行了 `field_nav_demo-dirclean`
  - 串口是否打印模型路径、LUT 路径、output tensor、导航后处理统计
  - OSD 固定测试框是否先出现
  - `valid=1` 时导航线是否显示
  - UART/RDK 是否收到导航帧
- 如果本机无法运行 Buildroot，不要声称编译通过。明确说明需要在 Linux SDK 容器中验证。
- 参赛合规判断必须基于板端 60 秒日志和现场画面：
  - 普通光照、强光/开窗、暗光/关灯至少各跑 60 秒。
  - 保存 Aurora 串口日志中的 `metrics`、`output tensor`、`valid`、`nav UART frame sent`。
  - 没有 60 秒日志时，只能说“项目具备主链路”，不能说“完全满足要求”。
  - 如果 `FPS_app / 90` 不接近 `1.0`，要诚实说明不满足接近 90fps 的高分性能项。

## 本次确认：地瓜派 RDK X5 联动代码文件

- 当前项目与地瓜派联动不是网络、ROS 或文件共享链路，而是 UART 串口链路。
- A1 端由 `field_nav_demo` 在板端运行，读取摄像头、执行模型推理、提取 `NavLine`，再从 `GPIO_PIN_0` 复用的 `UART_TX0` 发出 16 字节导航帧。
- 地瓜派 RDK X5 端运行 `field_nav_external\scripts\rdk_x5_nav_bridge.py`，从 40Pin UART 接收 A1 导航帧，按偏移和角度计算线速度/角速度，再发 16 字节控制帧给下位机。
- 硬件链路约定：A1 P4-15 / A1_D0_UART0TX -> 1.8V 到 3.3V 电平转换 -> RDK X5 40Pin Pin10 / UART_RXD；RDK X5 40Pin Pin8 / UART_TXD -> 下位机 UART_RX；A1、RDK X5、下位机必须共地。
- A1 导航帧协议：16 字节，帧头 `A5 5A`，版本字节 `0x01`，valid 标志，seq，`deviation_px * 10`，`angle_deg * 100`，confidence 百分比，导航点数量，bottom_x，status，前 15 字节累加校验。
- RDK 控制帧协议：16 字节，帧头 `B5 5B`，版本字节 `0x01`，enable/valid 标志，seq，线速度 mm/s，角速度 mrad/s，`deviation_px * 10`，mode，前 15 字节累加校验。
- 直接相关文件清单：
  - `field_nav_external\src\field_nav_demo\src\main.cpp`：C++ 源码。定义 `NavUartPublisher`，初始化 GPIO/UART，配置 `GPIO_PIN_0=UART_TX0`，按 `--nav-baud` 和 `--nav-rate` 发送导航帧；主循环中每帧根据摄像头、推理和导航线状态设置 `status`，调用 `nav_uart.Publish(status, line)`。
  - `field_nav_external\scripts\rdk_x5_nav_bridge.py`：Python 脚本。地瓜派端桥接程序；用 Linux `termios` 打开 `/dev/ttyS*` 等串口，无第三方依赖；解析 A1 导航帧，校验帧头和 checksum，计算控制量，并向同一 UART 写出下位机控制帧。
  - `field_nav_external\src\field_nav_demo\include\field_nav.hpp`：C++ 头文件。定义 `NavLine`、`NavPoint`、坐标和裁剪常量；`NavLine` 中的 `valid`、`bottom_x`、`deviation_px`、`angle_deg`、`confidence`、`points` 是 UART 导航帧的核心数据来源。不要随意修改此头文件。
  - `field_nav_external\src\field_nav_demo\src\navline_detector.cpp`：C++ 源码。模型推理后处理，生成 `NavLine`；通过最小二乘拟合得到 `bottom_x`、`deviation_px`、`angle_deg`、`confidence` 和 `valid`，这些结果被 `main.cpp` 打包发送给 RDK X5。
  - `field_nav_external\src\field_nav_demo\scripts\run.sh`：Shell 启动脚本。读取 `FIELD_NAV_UART`、`FIELD_NAV_BAUD`、`FIELD_NAV_RATE`、`FIELD_NAV_SENSOR_FPS`、`FIELD_NAV_OSD_RATE`、`FIELD_NAV_TEST_SECONDS`，并传给 `field_nav_demo` 的 `--nav-uart`、`--nav-baud`、`--nav-rate` 等参数。
  - `field_nav_external\board\m1pro\rootfs_overlay\usr\smartsoc\smartsoc_start.sh`：板端自启动脚本。加载 `gpio_kmod.ko`、`uart_kmod.ko` 等内核模块，然后执行 `/field_nav/scripts/run.sh`，保证 UART 联动能力随系统启动。
  - `field_nav_external\src\field_nav_demo\CMakeLists.txt`：CMake 构建文件。把 `main.cpp`、`navline_detector.cpp` 等编译成 `field_nav_demo`，并链接 GPIO/UART/SSNE/OSD 等 M1 SDK 库。
  - `field_nav_external\src\field_nav_demo\cmake_config\Paths.cmake`：CMake 路径配置。声明 `libgpio.so` 和 `libuart.so` 路径，供 `CMakeLists.txt` 链接 A1 端 UART 发送能力。
  - `field_nav_external\package\field_nav_demo\field_nav_demo.mk`：Buildroot package 文件。把 `field_nav_demo`、`scripts\run.sh`、模型和 LUT 安装到目标根文件系统 `/field_nav`，使板端启动后能运行并输出 UART 导航帧。
  - `field_nav_external\configs\field_nav_m1pro_defconfig`：Buildroot defconfig。启用 `BR2_PACKAGE_FIELD_NAV_DEMO=y`，叠加 `field_nav_external` rootfs overlay，并设置默认模型路径。
  - `field_nav_external\package\field_nav_demo\Config.in`：Buildroot 菜单配置。定义 `field_nav_demo` 包和 `/field_nav` 内模型相对路径选项。
  - `field_nav_external\scripts\build_field_nav.sh`：Linux SDK 容器构建脚本。生成包含 `field_nav_demo`、自启动脚本、UART 模块加载和导航资源的 `zImage.smartsens-m1-evb`。
  - `field_nav_external\README.md`：项目说明文档。记录 A1 到 RDK X5 的 UART 接线、板端运行环境变量、RDK X5 串口查找命令和桥接脚本运行示例。

## 本次确认：6 个只读子代理重新扫描项目

- 本次扫描按 6 个只读范围执行：项目结构/文档、数据集、训练和模型、板端 demo 源码、Buildroot/打包、UART/RDK/上板验证。
- 本次没有修改原始数据集，没有修改 `.h/.hpp`，没有还原已有改动。
- 详细中文报告已生成到 `field_nav_workspace\reports\project_function_advantages_summary_2026-05-05.md`。
- 根目录实际包含 A1 SDK、`field_nav_workspace`、Aurora 工具、原始 LabelMe 数据集、`a1-sdk-builder-latest.tar`、`docker_create_sdk_builder.bat` 和本文件。
- 根目录本身不是 git 仓库；在 `D:\1.1.1.1.1` 执行 `git status --short` 返回 `fatal: not a git repository`。
- `field_nav_external` 递归约 19 个文件，结构集中在 `board`、`configs`、`package`、`scripts`、`src`。
- `field_nav_workspace` 递归约 1855 个文件，包含训练脚本、派生数据、训练 runs、报告和主机端证明图。
- Aurora 工具目录存在，约 89 个 `.log`；本次未找到匹配 `field_nav`、`metrics tag=`、`nav UART`、`FPS_app`、`uart_sent` 的板端实跑日志。
- 原始数据集 `labelme_data` 实测为 211 张 `.jpg` 和 211 个 `.json`，同名配对 211 对，缺失 0。
- 原始数据集 LabelMe JSON 版本可见为 `5.5.0`，全部 shape 类型为 `polygon`，`imagePath` 均存在，`imageData` 为空。
- 原始数据集标签统计：`sand_road` 836 个 polygon，出现在 186 个 JSON；`grassy_road` 197 个 polygon，出现在 52 个 JSON；同时含两类的 JSON 为 27 个。
- 数据集功能判断仍是：可作为语义分割源数据，但不能直接训练；需要先把 LabelMe polygon 转成 mask。导航线必须从 road mask 后处理提取，不能把 polygon 直接当导航线。
- `processed_v2_640x480` 已有 211 个 images、211 个 masks、211 个 previews，split 为 `train=147`、`val=32`、`test=32`。
- `processed_v2_640x480\class_map.json` 确认为 `background=0`、`road=1`，v2 训练把 `sand_road` 和 `grassy_road` 合并为单前景 road。
- `runs\navroad_v2` 已有 `best.pt`、`last.pt`、`navroad_640x480.onnx`、`history.json`、`summary.json`、`host_proof`。
- `runs\navroad_v2\summary.json` 关键指标：`best_epoch=64`，`best_val.iou≈0.4438`，`mean_center_error_px≈54.11`，`mean_bottom_error_px≈59.96`。
- `host_proof\proof_metrics.json` 关键指标：test 样本 32，`mean_iou≈0.4992`，`valid_navline_samples=32`，`mean_line_error_px_original720≈86.92`，`mean_crop_bottom_error_px_original720≈100.03`。
- `data\audit_v2\audit_summary.json` 显示 211 个样本中 170 个 suspicious，主要原因包括 `low_resolution=89`、`fragmented_mask=94`、`many_vertices=46`。
- 当前模型质量可运行、可证明主机端链路，但还不能视为最终高质量模型；后续应优先清理可疑样本、增强数据、复查 crop 策略。
- 板端 demo 源码文件统计：4 个 `.cpp`、1 个 `.hpp`、1 个 `run.sh`、1 个 `CMakeLists.txt`、1 个 `Paths.cmake`、1 个 `.m1model`。
- 板端 `.m1model` 路径为 `field_nav_external\src\field_nav_demo\app_assets\models\navroad_640x480.m1model`，大小 616750 字节。
- `main.cpp` 已实现 `NavUartPublisher`，配置 `GPIO_PIN_0=UART_TX0`，发送 16 字节 `A5 5A` 导航帧。
- `navline_detector.cpp` 已实现动态读取 output tensor 尺寸、阈值 `0.45`、低分辨率 mask 后处理、连通域、行带中心点、最小二乘拟合、`ok_tdm_ls` 和短时 fallback。
- `CMakeLists.txt` 依赖 `FIELD_NAV_FACE_DEMO_ROOT` 下的人脸 demo `osd-device.cpp`；该依赖缺失会导致 CMake 失败。
- `field_nav_demo\app_assets` 源目录中只看到模型；最终 LUT 依赖 Buildroot package 从人脸 demo assets 复制。
- `field_nav_demo.mk` 会安装 `/field_nav/field_nav_demo`、`/field_nav/scripts/run.sh`、`/field_nav/app_assets`，并复制 `shared_colorLUT.sscl` 和 `colorLUT.sscl`。
- `output\target\field_nav` 已实际包含 `field_nav_demo` 58768 字节、`navroad_640x480.m1model` 616750 字节、`run.sh` 1345 字节、`shared_colorLUT.sscl` 98 字节、`colorLUT.sscl` 71 字节。
- `output\images` 已有 `rootfs.cpio`、`rootfs.cpio.gz`、`zImage.smartsens-m1-evb`；其中 `zImage.smartsens-m1-evb` 大小 5920184 字节，时间为 2026-05-05 03:15:02。
- 本次没有重新运行 Linux SDK 容器构建，不能声称本轮编译通过；只能说已有 target rootfs 和烧录镜像产物存在。
- A1/RDK 代码链路判断为主链路已具备：A1 摄像头/推理/导航线结果 -> UART 16 字节导航帧 -> RDK X5 桥接 -> 下位机 16 字节控制帧。
- `rdk_x5_nav_bridge.py` 使用 Linux `termios`，无第三方依赖，解析 `NAV_HEADER=b"\xA5\x5A"`，输出 `CMD_HEADER=b"\xB5\x5B"`。
- 当前仍缺少板端 Aurora 60 秒 `metrics`、`uart_sent`、`nav UART frame sent` 等实跑证据，不能证明 UART 实际发出、RDK 实际收到、下位机实际收控，也不能证明 90fps 达标。
- `FIELD_NAV_SENSOR_FPS=90` 仍只代表日志目标值，不代表传感器实际配置到 90fps；必须依据板端 `FPS_app`、`fps_ratio` 和 `image_ms` 判断。

## 本次确认：GitHub 上传前检查

- 根目录 `D:\1.1.1.1.1` 当前仍不是 Git 仓库，`git rev-parse --is-inside-work-tree` 返回 `fatal: not a git repository`。
- 根目录当前没有 `.gitignore`。
- 本机 Git 可用，版本为 `git version 2.54.0.windows.1`。
- 本机 Git LFS 可用，版本为 `git-lfs/3.7.1`。
- 本机全局 Git 用户信息已配置：`user.name=ccl`，`user.email=1565331896@qq.com`。
- 本机未安装 GitHub CLI，`gh --version` 返回命令不存在。
- 当前项目递归约 `231836` 个文件，总体积约 `8.199 GB`。
- 顶层目录体积实测：`Aurora-2.0.0-ciciec.14` 约 `4.043 GB`，`data` 约 `3.64 GB`，`field_nav_workspace` 约 `0.216 GB`，原始 LabelMe 数据集目录约 `0.145 GB`。
- 当前至少有 2 个超过 GitHub 普通 Git 单对象硬限制的文件：
  - `a1-sdk-builder-latest.tar`，约 `158.8 MB`。
  - `data\A1_SDK_SC132GS\smartsens_sdk\cache\linux-5.15.24.tar.gz`，约 `186.08 MB`。
- 当前至少有 4 个超过 `50 MB` 的文件，普通 GitHub push 会触发大文件警告或限制；超过 `100 MB` 的普通 Git 对象会被 GitHub 拒绝。
- 如果用户确实要把完整项目上传到 GitHub，应优先使用 Git LFS 追踪大二进制、压缩包、构建产物和日志文件；否则应先建立 `.gitignore`，排除 Aurora 程序、SDK cache、Buildroot output、日志和其他可再生成文件，只推源码、训练脚本、说明文档和必要小模型。
- 由于远程仓库 URL 和大文件策略尚未确认，不能直接执行最终 `git push`。

## 本次执行：干净源码上传到 GitHub

- 用户指定 GitHub 远程仓库：`https://github.com/Bikini-Bottom-nuc/Rage.git`。
- 用户选择上传策略：干净源码上传。
- `data\A1_SDK_SC132GS` 本身已经是一个嵌套 Git 仓库，远程为 `https://git.smartsenstech.ai/Smartsens/A1_SDK_SC132GS.git`。
- 嵌套 SDK 仓库当前只有一个提交可见：`9e64a9c 更新libssne为最新版本`。
- 嵌套 SDK 仓库中 `smartsens_sdk\field_nav_external` 是未跟踪目录。
- 根目录 `.gitignore` 按干净上传策略创建：排除 Aurora 程序、压缩包、原始数据集、`field_nav_workspace\data`、`field_nav_workspace\runs`、SDK 主体、SDK cache/dl/output、Python cache、日志、训练 checkpoint 和 ONNX。
- 根目录 `.gitignore` 明确保留 `data\A1_SDK_SC132GS\smartsens_sdk\field_nav_external`，让项目自研 Buildroot external、板端 demo、小模型、RDK X5 桥接脚本和说明文档进入 Git。
- 根目录 `.gitattributes` 已创建，用于保持 shell/Python/C++/CMake/Markdown/Buildroot 配置文件为 LF，`.bat` 为 CRLF，图片和模型为 binary。
- 由于 `data\A1_SDK_SC132GS` 是嵌套 Git 仓库，根目录提交时不能直接 `git add .`；本次对 `field_nav_external` 使用 `git hash-object` + `git update-index --cacheinfo` 按普通文件写入根仓库索引，没有移动或删除嵌套 `.git`。
- 远程仓库 `origin/main` 原本已有 `README.md` 和 `AGENTS.md`；本次已用 `--allow-unrelated-histories` 合并远程历史，保留远程 `README.md`，`AGENTS.md` 冲突以本地追加后的项目记录为准。
- 本次 `git push -u origin main` 卡在 GitHub HTTPS 认证：GitHub 返回 `401` 后调用 Windows `git credential-manager get`，禁用 credential helper 后确认错误为 `fatal: unable to get password from user`。后续需要用户在本机完成 GitHub 凭据授权后再推送。
- 用户完成 GitHub 认证后，远程 `origin/main` 已成功更新到本地提交；已通过 `git ls-remote --heads origin main`、`git fetch origin main`、`git rev-parse HEAD` 和 `git rev-parse origin/main` 核对本地/远程提交一致。

## 本次确认：Aurora 导出 dump 图像功能可用于 field_nav_demo 调试

- Aurora 的 A1 图像工具可以用于本项目导出板端保存的 tensor 图像，但当前 `field_nav_demo` 源码还没有接入自动保存 dump 图像逻辑。
- SDK 人脸 demo 中已经有参考实现：`smart_software\src\app_demo\face_detection\ssne_ai_demo\src\scrfd_gray.cpp` 会在推理后保存最后一帧 `ssne_tensor_t`，退出时调用 `save_tensor(...)` 写出调试图像。
- `save_tensor(ssne_tensor_t tensor, const char* filepath)` 在 SDK 头文件 `output\opt\m1_sdk\usr\include\smartsoc\ssne_api.h` 中声明，会保存 tensor 及其元数据；Aurora 可以基于这些元数据读取宽高和格式。
- 为了配合 Aurora 自动导出，调试文件名建议使用 `.bin` 后缀，例如 `field_nav_dbg_crop.bin`、`field_nav_dbg_model_input.bin`；截图里的说明强调 `.bin` 后缀更适合 Aurora 双击/自动导出流程。
- 本项目最有价值的 dump 点：
  - `ImageProcessor::GetImage()` 得到的 `image`：当前在线 pipeline 输出，约为 crop 后的 `720x540` 灰度图，可检查摄像头画面和 crop 是否正确。
  - `NavLineDetector::Predict()` 中 `RunAiPreprocessPipe(...)` 后的 `input_`：模型实际输入，约为 `640x480` 灰度图，可检查 resize 后喂给 NPU 的图像。
  - `output_` 是模型输出概率图，通常约 `120x160`，可作为原始 tensor 调试，但不一定适合直接当普通图片看；更适合结合日志或另写主机脚本解析概率。
- 建议只在调试开关下保存 dump，不要每帧保存；A1 存储和串口导出都慢，连续保存会影响 `FPS_app` 和板端实时性。
- 接入前的 `field_nav_demo` 只处理 `SIGINT/SIGTERM` 和 `FIELD_NAV_TEST_SECONDS`，没有像截图 demo 那样监听串口输入 `q` 退出；如果不重新构建包含本次改动的新镜像，板端旧程序仍然不会响应 `q`。
- 接入后仍需在 Linux SDK 容器中重新执行 `field_nav_external\scripts\build_field_nav.sh`，确保 `field_nav_demo-dirclean` 生效并重新生成烧录镜像；Windows PowerShell 不能证明 Buildroot 编译通过。
- 接入前的 `main.cpp` 没有 `getchar`、`fgets`、`std::cin`、`select` 或 `poll` 等标准输入监听逻辑；在 Aurora 串口窗口输入 `q` 回车不会因此退出，也不会触发 dump 保存。
- 旧程序可用的退出/停止方式是 `SIGINT/SIGTERM`、进程被系统停止，或通过 `FIELD_NAV_TEST_SECONDS` 传入固定运行秒数自动退出。支持 `q` 回车后仍需重新构建烧录镜像才会在板端生效。
- 已按“不改头文件”的约束实现 `q/Q` 回车退出：`main.cpp` 使用 `isatty(STDIN_FILENO)` 判断是否有交互 stdin，使用 `select()` 零超时轮询，再用 `read()` 读取输入字符，收到 `q` 或 `Q` 后优雅退出。
- 收到 `q/Q` 时，当前实现只保存本帧 `ImageProcessor::GetImage()` 得到的 crop tensor，不保存 `NavLineDetector::input_` 或 `output_`，因为这两个 tensor 是私有成员，保存它们需要改 `field_nav.hpp` 增加 debug 接口。
- 退出 dump 默认保存到板端 `/field_nav/field_nav_dbg_crop.bin`，调用的是 SDK 的 `save_tensor(image, "/field_nav/field_nav_dbg_crop.bin")`，保存后串口日志会打印路径、width、height、dtype 和返回码。
- 如果程序由自启动脚本后台启动且 stdin 不是 TTY，启动日志会打印 `stdin is not interactive`，这种情况下 Aurora 串口输入 `q` 不会作用到该进程；需要前台运行 `/field_nav/scripts/run.sh` 才能使用 `q` 回车退出。

## 本次执行：核心逻辑中文注释补充

- 本次只补充中文注释和函数说明，没有修改算法逻辑、运行参数、协议字段、模型文件或数据集文件。
- 本次没有修改 `.h/.hpp`，尤其没有修改 `field_nav_external\src\field_nav_demo\include\field_nav.hpp`。
- 当前根目录 `D:\1.1.1.1.1` 已经是 Git 仓库；本轮检查 `git status --short` 时，进入任务前 `AGENTS.md` 已存在未提交修改。
- 板端注释范围覆盖 `image_processor.cpp`、`main.cpp`、`navline_detector.cpp`、`osd_overlay.cpp`，重点说明摄像头裁剪、NPU 推理、TDM-LS 后处理、OSD、UART 16 字节导航帧和 60 秒 metrics。
- RDK/构建注释范围覆盖 `rdk_x5_nav_bridge.py`、`run.sh`、`build_field_nav.sh`、`smartsoc_start.sh`、`field_nav_demo.mk`、`CMakeLists.txt`，重点说明串口帧解析、停车兜底、Buildroot dirclean 和 LUT/模型打包路径。
- 训练工具注释范围覆盖 `field_nav_workspace\tools` 下的数据准备、审计、训练、评估、ONNX 对比和 host proof 脚本。
- 注释原则：函数级说明输入输出和使用注意，关键变量就地解释含义，关键算法循环前说明实现原理；避免把注释写进头文件或改动可执行逻辑。

## 本次确认：项目鲁棒性体现方式

- 本项目的鲁棒性不能只用“能跑起来”体现，应同时从算法容错、运行稳定、通信安全和现场证据四个层面证明。
- 算法层面重点看 `navline_detector.cpp`：模型输出后先按阈值 `0.45` 得到 road mask，再做纵向形态学修补、连通域过滤、主贯通域选择、行带中心点提取和最小二乘拟合；成功日志为 `reason=ok_tdm_ls`。
- 短时遮挡或模型输出断裂时，`navline_detector.cpp` 最多 2 帧使用上一帧有效导航线低置信度兜底，日志为 `reason=fallback_last_valid`；连续失败后应输出 `valid=0`，不能长期假装有效。
- 后处理诊断字段 `components`、`main_area`、`band_points`、`points`、`fallback`、`valid`、`failure` 是判断鲁棒性的直接依据。强鲁棒性表现为这些字段在光照变化、道路纹理变化、短时遮挡下仍能稳定回到 `ok_tdm_ls`，且不会频繁长时间 `valid=0`。
- 运行层面重点看 `main.cpp` 的 60 秒 metrics：`valid_nav`、`no_line`、`predict_fail`、`image_fail`、`uart_sent`、`uart_fail`、`max_invalid_ms`、`P95_frame_ms`、`max_frame_ms`、`image_ms`、`predict_ms`、`uart_ms`、`osd_ms`。
- `max_invalid_ms` 是鲁棒性核心扣分指标。普通光照、强光/开窗、暗光/关灯至少各跑 60 秒，若连续无效导航超过 `1000ms`，要如实记录并继续优化模型、阈值、crop 或后处理。
- 性能鲁棒性要看 `FPS_app`、`fps_ratio` 和各阶段 P95/max 耗时。`FIELD_NAV_SENSOR_FPS=90` 只代表日志目标值，不能代替真实 90fps 证明；如果 `image_ms` P95 接近 33ms，说明摄像头/ISP 链路仍接近 30fps。
- OSD 可能影响实时性，鲁棒性验证应分别跑 `FIELD_NAV_OSD_RATE=15` 的正常显示测试和 `FIELD_NAV_OSD_RATE=0` 的无 OSD 性能复测。
- 通信层面重点看 A1 `NavUartPublisher` 和 RDK `rdk_x5_nav_bridge.py`：A1 发送 `A5 5A` 16 字节导航帧，RDK 校验后输出 `B5 5B` 控制帧；`uart_sent` 应持续增长，`uart_fail` 应接近 0。
- 安全兜底也属于鲁棒性：RDK 端要求 A1 导航帧 `valid`、`status==0`、置信度达到阈值且未超时；无有效导航、低置信度或超时时输出停车/禁用控制帧。
- 当前主机端证据只能说明链路可运行：`host_proof` test 样本 32 个，`valid_navline_samples=32`，但 `mean_iou` 约 `0.499`、线误差仍偏大，不能把它等同于现场鲁棒性通过。
- 当前项目仍缺少 Aurora 板端 60 秒实跑日志，不能证明 UART 实际发出、RDK 实际收到、下位机实际受控，也不能证明 90fps 达标。
- 建议最终鲁棒性材料包括：三种光照各 60 秒 Aurora 串口日志、摄像头画面/OSD 录像、RDK 端接收日志、下位机停车超时验证、`FIELD_NAV_OSD_RATE=0` 性能复测日志、典型失败帧的 dump 图像或截图。

## 本次确认：后处理提取中心线、偏移和角度算法

- 本次回答已重新检查 `field_nav_external\src\field_nav_demo\src\navline_detector.cpp`，后处理实际入口仍是 `NavLineDetector::DecodeOutputToLine(...)`。
- 后处理不是直接从 LabelMe polygon 或模型输出中取“导航线”，而是先把模型输出概率图转成 road mask，再从 road mask 中提取中心线。
- 概率归一化：`FLOAT32` 输出按概率或 logit 处理，logit 走 `sigmoid`；`INT8` 按 `[-128,127]` 平移到 `[0,255]` 后归一化；`UINT8` 按 `[0,255]` 归一化。
- 阈值分割：使用固定阈值 `kMaskThreshold=0.45`，把 road 概率图二值化为前景 mask。
- ROI 限制：只处理模型输出图下方约 `65%` 区域，也就是从 `height * 0.35` 开始向下处理，减少远处小目标和噪声对近端控制的影响。
- 纵向形态学闭运算：先纵向膨胀再纵向腐蚀，半径 `kMorphVerticalRadius=3`、横向半径 `0`，用于修补田垄道路 mask 的短断裂，同时避免横向错误粘连相邻垄沟。
- 原始前景保留：闭运算后会把原始 mask 中的前景强制保留，避免细道路区域在腐蚀阶段被抹掉。
- 连通域标记：使用 `4` 邻域 DFS 对二值 mask 做连通域标记。
- 小连通域过滤：面积小于 `kMinComponentArea=30` 的连通域视为噪声丢弃。
- 主道路区域选择：对候选连通域打分，偏好触底、纵向跨度大、面积大、贯通 ROI 或接近 ROI 顶部、接近上一帧预测位置或图像中心的区域。
- 历史线预测辅助：如果已有上一帧有效导航线，会用上一帧 `slope/intercept` 预测当前 mask 行的期望 x 位置；没有历史线时使用图像中心作为参考。
- 行带中心点提取：从近端向远端按 `kBandHeight=4` 个 mask 像素为一组扫描水平行带，每个行带在主连通域内计算一个概率加权中心点。
- 坐标映射：行带中心点先在低分辨率输出 mask 坐标中计算，再映射回原始 `720x1280` 坐标系，供 OSD 和 UART 统一使用。
- 最小二乘直线拟合：对提取到的中心点拟合 `x = slope * y + intercept`，最少需要 `kMinValidPoints=6` 个点，点数不足或几何退化时判失败。
- 偏移计算：拟合成功后，先计算原始图像底部交点 `bottom_x = slope * (kOriginalHeight - 1) + intercept`，再计算 `deviation_px = bottom_x - (kOriginalWidth / 2.0f)`。
- 角度计算：使用 `angle_deg = atan(slope) * 180 / pi`，即根据拟合直线斜率换算航向角。
- 置信度计算：中心线整体 `confidence` 使用各行带中心点置信度的平均值。
- 有效性判断：拟合成功后设置 `valid=true`，成功日志 reason 为 `ok_tdm_ls`。
- 失败兜底：如果当前帧后处理失败且最近有效线仍在允许窗口内，最多 `2` 帧沿用上一帧导航线，并把线和点的置信度减半，日志 reason 为 `fallback_last_valid`；超过窗口后输出无效线。
- 当前实现未采用 DBSCAN、Hough、骨架化、完整 Otsu/YCrCb 颜色分割或 OpenCV 重型轮廓算法作为主流程。
