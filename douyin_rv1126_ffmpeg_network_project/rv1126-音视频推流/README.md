# RV1126 双路视频 RTMP 推流系统

基于 Rockchip RV1126 SoC 的双路视频采集、编码与 RTMP 网络推流系统。通过硬件加速实现从摄像头采集到网络直播的完整链路。

## 项目简介

本项目利用 RV1126 芯片的硬件编解码能力，实现单摄像头双路码流（1080P + 720P）的实时 RTMP 推流。整个处理管线完全基于硬件加速（VI 采集、RGA 缩放、VENC H.264 编码），通过 FFmpeg 将编码后的视频帧发送到 RTMP 服务器。

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

## 功能特性

- **双路码流**: 单摄像头同时输出 1080P（高清）和 720P（标清）两路 H.264 视频流
- **全硬件加速**: 采集、缩放、编码全部由 RV1126 硬件完成，不占用 CPU 资源
- **线程安全队列**: 基于 pthread mutex + condition variable 的生产者-消费者模型
- **多线程架构**: 5 个独立工作线程分别处理编码、缩放和推流任务
- **RTMP 推流**: 基于 FFmpeg 的 `av_interleaved_write_frame` 实现网络推流

## 模块说明

| 源文件 | 职责 |
|--------|------|
| `rv1126_ffmpeg_main.cpp` | 程序入口，解析命令行参数，初始化各模块 |
| `rkmedia_module.cpp/h` | RKMedia API 封装层（VI、AI、VENC、AENC 初始化） |
| `rkmedia_module_function.cpp/h` | 模块初始化编排，配置 VI（1080P）、VENC（双路）、RGA（缩放） |
| `rkmedia_assignment_manage.cpp/h` | 任务调度管理，绑定管线节点，创建所有工作线程 |
| `rkmedia_data_process.cpp/h` | 数据处理线程：采集、缩放、编码、FFmpeg 推流 |
| `rkmedia_ffmpeg_config.cpp/h` | FFmpeg 输出配置：创建 AVFormatContext、添加流、连接网络 |
| `rkmedia_container.cpp/h` | 线程安全的通道 ID 注册表 |
| `ffmpeg_video_queue.cpp/h` | 线程安全视频帧队列（生产者-消费者模型） |
| `ffmpeg_audio_queue.cpp/h` | 线程安全音频帧队列（预留） |
| `rkmedia_config_public.h` | 公共配置结构体定义 |
| `rv1126_isp_function.cpp/h` | ISP 图像信号处理初始化 |
| `sample_common_isp.c` | Rockchip ISP 辅助函数 |

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

## 工作线程

| 线程 | 功能 |
|------|------|
| `camera_venc_thread` | 从 VENC 通道 0 获取 1080P 编码数据，写入 high_video_queue |
| `get_rga_thread` | 从 RGA 获取缩放后的 720P 帧，送入 VENC 通道 1 编码 |
| `low_camera_venc_thread` | 从 VENC 通道 1 获取 720P 编码数据，写入 low_video_queue |
| `high_video_push_thread` | 从 high_video_queue 取帧，通过 FFmpeg 推送 1080P 流 |
| `low_video_push_thread` | 从 low_video_queue 取帧，通过 FFmpeg 推送 720P 流 |

## 已知限制

- 目前仅支持 RTMP 协议推流，SRT 协议仍在开发中
- 音频采集与推流已预留接口（AUDIO_QUEUE、AAC 编码配置），但当前未启用
- OSD（屏幕显示/水印）功能有代码框架但未启用

## License

MIT
