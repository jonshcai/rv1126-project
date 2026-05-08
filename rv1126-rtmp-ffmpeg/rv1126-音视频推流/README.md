# RV1126 双路视频 RTMP 推流系统

基于 Rockchip RV1126 SoC 的双路视频采集、编码与 RTMP 网络推流系统。通过硬件加速实现从摄像头采集到网络直播的完整链路。

## 项目简介

本项目利用 RV1126 芯片的硬件编解码能力，实现单摄像头双路码流（1080P + 720P）的实时 RTMP 推流。整个处理管线完全基于硬件加速（VI 采集、RGA 缩放、VENC H.264 编码），通过 FFmpeg 将编码后的视频帧发送到 RTMP 服务器。

适用于安防监控、视频会议、直播推流等需要同时输出高低两种分辨率的场景。

## 系统架构

```
Camera Sensor (rkispp_scale0)
        |
        v
    VI Channel 0 (1920x1080, NV12)
        |
        +----------------------------+
        |                            |
        v                            v
  VENC Channel 0              RGA Channel 0
  (H.264 1080P 编码)          (1080P → 720P 缩放)
        |                            |
        v                            v
  high_video_queue            RK_MPI_SYS_SendMediaBuffer
        |                            |
        |                            v
        |                     VENC Channel 1
        |                     (H.264 720P 编码)
        |                            |
        |                            v
        |                     low_video_queue
        |                            |
        v                            v
  high_video_push_thread     low_video_push_thread
        |                            |
        v                            v
  FFmpeg AVPacket             FFmpeg AVPacket
        |                            |
        v                            v
  RTMP Server (1080P)         RTMP Server (720P)
```

## 逻辑分析

### 1. 数据流管线 (Pipeline)

程序的核心是一条硬件加速的数据管线，分为两条并行的处理路径：

- **高码流路径 (1080P)**: VI → VENC Channel 0 → 队列 → FFmpeg 推流
- **低码流路径 (720P)**: VI → RGA (缩放) → VENC Channel 1 → 队列 → FFmpeg 推流

VI 模块从摄像头采集 1920x1080 的 NV12 格式图像后，数据被同时送往两个方向：直接进入高分辨率编码器，以及经过 RGA 缩放到 1280x720 后进入低分辨率编码器。

### 2. 多线程模型

系统采用 **5 个独立工作线程**，基于生产者-消费者模型协作：

| 线程 | 角色 | 说明 |
|------|------|------|
| `camera_venc_thread` | 生产者 | 从 VENC Channel 0 获取 1080P H.264 码流，写入 high_video_queue |
| `get_rga_thread` | 中继 | 从 RGA 获取缩放后的 720P 帧，送入 VENC Channel 1 |
| `low_camera_venc_thread` | 生产者 | 从 VENC Channel 1 获取 720P H.264 码流，写入 low_video_queue |
| `high_video_push_thread` | 消费者 | 从 high_video_queue 取帧，通过 FFmpeg 推送 1080P RTMP 流 |
| `low_video_push_thread` | 消费者 | 从 low_video_queue 取帧，通过 FFmpeg 推送 720P RTMP 流 |

### 3. 线程安全队列

`VIDEO_QUEUE` 类封装了基于 `pthread_mutex` + `pthread_cond` 的阻塞队列：

- **put**: 加锁 → 入队 → 广播条件变量 → 解锁
- **get**: 加锁 → 空则 `cond_wait` 阻塞 → 出队 → 解锁

编码线程作为生产者不断将编码数据放入队列，推流线程作为消费者阻塞等待数据到来。当队列为空时消费者自动挂起，避免 CPU 空转。

### 4. FFmpeg 推流流程

每路推流的 FFmpeg 配置流程如下：

1. `avformat_alloc_output_context2` — 根据协议类型创建 FLV 或 TS 封装上下文
2. `add_stream` — 创建 AVStream，配置 H.264 编码参数（码率、帧率、GOP）
3. `open_video` — 打开编码器，复制参数到流
4. `avio_open` — 建立网络连接
5. `avformat_write_header` — 写入文件头（FLV Header）
6. 循环: 从队列取帧 → `av_buffer_realloc` 拷贝数据 → 设置 PTS → `av_interleaved_write_frame` 发送
7. 退出时: `av_write_trailer` → 释放资源

### 5. 关键设计决策

- **H.264 Baseline Profile (66)**: 选择最低编码等级以降低延迟、减小带宽消耗，适合网络传输
- **CBR 码率控制**: 码率 = 宽 × 高 × 3，保证稳定的网络带宽占用
- **NV12 像素格式**: RV1126 硬件原生支持的 YUV420 半平面格式，避免额外的格式转换
- **分离线程架构**: 编码和推流分离，推流网络延迟不会阻塞编码管线

## 模块说明

| 源文件 | 职责 |
|--------|------|
| `rv1126_ffmpeg_main.cpp` | 程序入口，解析命令行参数，初始化队列和各模块 |
| `rkmedia_module.cpp/h` | RKMedia API 封装层（VI、AI、VENC、AENC 初始化） |
| `rkmedia_module_function.cpp/h` | 模块初始化编排：配置 VI（1080P）、双路 VENC、RGA 缩放 |
| `rkmedia_assignment_manage.cpp/h` | 任务调度：绑定管线节点（VI→VENC, VI→RGA），创建全部工作线程 |
| `rkmedia_data_process.cpp/h` | 数据处理线程实现：编码采集、RGA 中继、FFmpeg 推流 |
| `rkmedia_ffmpeg_config.cpp/h` | FFmpeg 输出配置：创建 AVFormatContext、添加流、连接 RTMP |
| `rkmedia_container.cpp/h` | 线程安全的通道 ID 注册表（VI/VENC ID 存取） |
| `ffmpeg_video_queue.cpp/h` | 线程安全视频帧阻塞队列（mutex + condvar） |
| `ffmpeg_audio_queue.cpp/h` | 线程安全音频帧队列（预留，当前未启用） |
| `rkmedia_config_public.h` | 公共配置结构体定义（VI/VENC 参数结构体） |
| `rv1126_isp_function.cpp/h` | ISP 图像信号处理初始化 |
| `sample_common_isp.c` | Rockchip ISP 辅助函数 |

## 功能特性

- **双路码流**: 单摄像头同时输出 1080P（高清）和 720P（标清）两路 H.264 视频流
- **全硬件加速**: 采集、缩放、编码全部由 RV1126 硬件完成，不占用 CPU 资源
- **线程安全队列**: 基于 pthread mutex + condition variable 的生产者-消费者模型
- **多线程架构**: 5 个独立工作线程分别处理编码、缩放和推流任务
- **RTMP 推流**: 基于 FFmpeg 的 `av_interleaved_write_frame` 实现网络推流

## 编译环境

- **交叉编译器**: `arm-linux-gnueabihf-g++`（RV1126 SDK v1.8.0 工具链）
- **依赖库**:
  - Rockchip: `easymedia`, `rockchip_mpp`, `rkaiq`, `rga`
  - FFmpeg: `avformat`, `avcodec`, `avutil`, `swresample`
  - 编码: `x264`
  - 网络: `srt`, `ssl`, `crypto`
  - 系统: `pthread`, `asound`, `v4l2`, `drm`

## 编译

```bash
make
```

## 使用方法

```bash
./rv1126_ffmpeg_main <高清流类型> <高清推流地址> <低清流类型> <低清推流地址>
```

**参数说明**:
- 流类型: `0` = FLV (RTMP)，`1` = TS (SRT，开发中)
- 推流地址: RTMP 服务器地址

**示例**:

```bash
./rv1126_ffmpeg_main 0 rtmp://server/live/stream_high 0 rtmp://server/live/stream_low
```

## 已知限制

- 目前仅支持 RTMP 协议推流，SRT 协议仍在开发中
- 音频采集与推流已预留接口（AUDIO_QUEUE、AAC 编码配置），但当前未启用
- OSD（屏幕显示/水印）功能有代码框架但未启用
- `rkmedia_assignment_manage.cpp` 中存在两处空指针判断错误（检查了错误的变量名）

## License

MIT
