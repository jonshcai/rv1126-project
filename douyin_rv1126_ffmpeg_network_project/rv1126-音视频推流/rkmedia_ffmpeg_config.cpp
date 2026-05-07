#include "rkmedia_ffmpeg_config.h"  // 包含FFmpeg配置相关的头文件

// 全局FFmpeg配置数组，存储多个推流配置（高码流和低码流），NETWORK_NUM通常为2
RKMEDIA_FFMPEG_CONFIG rkmedia_ffmpeg_configs[NETWORK_NUM];
pthread_mutex_t rkmedia_ffmpeg_config_mutex;  // 互斥锁，保护全局配置数组的线程安全

// 初始化FFmpeg配置管理功能
int init_rkmedia_ffmpeg_function()
{
    // 初始化互斥锁，NULL表示使用默认属性
    pthread_mutex_init(&rkmedia_ffmpeg_config_mutex, NULL);
    // 将全局配置数组全部清零，sizeof获取数组总字节数
    memset(rkmedia_ffmpeg_configs, 0, sizeof(rkmedia_ffmpeg_configs));
    return 0;
}
// 设置指定ID的FFmpeg配置
int set_rkmedia_ffmpeg_config(unsigned int config_id, RKMEDIA_FFMPEG_CONFIG *ffmpeg_config)
{
    // 加锁，防止多线程同时修改配置
    pthread_mutex_lock(&rkmedia_ffmpeg_config_mutex);
    // 将传入的配置结构体拷贝到全局数组的指定位置
    rkmedia_ffmpeg_configs[config_id] = *ffmpeg_config;
    // 解锁
    pthread_mutex_unlock(&rkmedia_ffmpeg_config_mutex);
    return 0;
}

// 获取指定ID的FFmpeg配置
unsigned int get_rkmedia_ffmpeg_config(unsigned int config_id, RKMEDIA_FFMPEG_CONFIG *ffmpeg_config)
{
    // 加锁，防止读取过程中配置被修改
    pthread_mutex_lock(&rkmedia_ffmpeg_config_mutex);
    // 从全局数组拷贝配置到输出参数
    *ffmpeg_config = rkmedia_ffmpeg_configs[config_id];
    // 解锁
    pthread_mutex_unlock(&rkmedia_ffmpeg_config_mutex);
    return 0;
}

// 添加流（视频或音频）到输出上下文
// ost: 输出流结构体，存储流相关信息
// oc: AVFormatContext，封装格式上下文
// codec: 编码器指针的指针，会被赋值为找到的编码器
// codec_id: 编码器ID（如AV_CODEC_ID_H264）
// width, height: 视频宽度和高度（音频流不需要）
int add_stream(OutputStream *ost, AVFormatContext *oc, AVCodec **codec, enum AVCodecID codec_id, int width, int height)
{
    AVCodecContext *c = NULL;  // 编码器上下文指针，初始化为空
    // 创建输出码流的AVStream，AVStream是存储每一个视频/音频流信息的结构体
    // 参数oc: 输出上下文，NULL: 不使用已有编码器，返回值赋给ost->stream
    ost->stream = avformat_new_stream(oc, NULL);
    if (!ost->stream)  // 检查创建是否成功
    {
        printf("Can't not avformat_new_stream\n");  // 打印错误信息
        return 0;  // 返回0表示失败
    }
    else
    {
        printf("Success avformat_new_stream\n");  // 打印成功信息
    }

    // 通过codec_id找到对应的编码器（如H264编码器）
    *codec = avcodec_find_encoder(codec_id);
    if (!(*codec))  // 检查是否找到编码器
    {
        printf("Can't not find any encoder");  // 打印错误信息
        return 0;
    }
    else
    {
        printf("Success find encoder");  // 打印成功信息
    }

    // nb_streams是输入视频的AVStream个数，即当前有几种Stream（视频流、音频流、字幕流）
    // oc->nb_streams - 1 对应的是AVStream中的索引index
    ost->stream->id = oc->nb_streams - 1;  // 设置流ID为当前流数量-1
    // 通过编码器分配编码器上下文
    c = avcodec_alloc_context3(*codec);
    if (!c)  // 检查分配是否成功
    {
        printf("Can't not allocate context3\n");
        return 0;
    }
    else
    {
        printf("Success allocate context3");
    }

    ost->enc = c;  // 将编码器上下文保存到输出流结构体中

    // 根据编码器类型（音频或视频）分别配置参数
    switch ((*codec)->type)  // 获取编码器类型
    {
    case AVMEDIA_TYPE_AUDIO:  // 音频流配置
        // 采样格式：优先使用编码器支持的第一个采样格式，否则使用AV_SAMPLE_FMT_FLTP（平面浮点）
        c->sample_fmt = (*codec)->sample_fmts ? (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
        c->bit_rate = 153600;   // 音频码率153.6kbps
        c->sample_rate = 48000; // 采样率48kHz（CD音质）
        c->channel_layout = AV_CH_LAYOUT_STEREO;  // 声道布局：立体声（双声道）
        c->channels = av_get_channel_layout_nb_channels(c->channel_layout); // 获取声道数：2
        ost->stream->time_base = (AVRational){1, c->sample_rate};  // 音频时间基 = 1/48000秒
        break;

    case AVMEDIA_TYPE_VIDEO:  // 视频流配置
        c->bit_rate = width * height * 3;  // 视频码率 = 宽×高×3（经验值，约6.2Mbps@1080P）
        c->width = width;   // 视频宽度
        c->height = height; // 视频高度

        ost->stream->r_frame_rate.den = 1;  // 帧率分母设为1
        ost->stream->r_frame_rate.num = 25; // 帧率分子设为25，即帧率25fps
        ost->stream->time_base = (AVRational){1, 25};  // 流时间基 = 1/25秒，与帧率匹配

        c->time_base = ost->stream->time_base;  // 编码器时间基与流时间基保持一致
        c->gop_size = GOPSIZE;  // GOP大小（关键帧间隔），GOPSIZE通常为25或30
        c->pix_fmt = AV_PIX_FMT_NV12;  // 像素格式：NV12（YUV420半平面格式）
        break;

    default:  // 其他类型（字幕等），不做处理
        break;
    }

    // 如果输出格式需要全局头（如FLV格式需要在头部包含SPS/PPS）
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
    {
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;  // 设置编码器全局头标志
    }

    return 0;  // 返回0表示成功
}

// 打开视频编码器
// oc: 输出上下文，codec: 视频编码器，ost: 输出流，opt_arg: 可选参数字典（未使用）
int open_video(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg)
{
    AVCodecContext *c = ost->enc;  // 获取编码器上下文

    // 打开编码器，NULL表示不使用额外选项
    avcodec_open2(c, codec, NULL);

    // 分配视频AVPacket包，用于存储编码后的数据
    ost->packet = av_packet_alloc();

    // 将AVCodecContext中的编码参数复制到AVCodecParameters（复用器需要的参数格式）
    avcodec_parameters_from_context(ost->stream->codecpar, c);
    return 0;
}

// 打开音频编码器
int open_audio(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg)
{
    AVCodecContext *c = ost->enc;  // 获取编码器上下文

    // 打开编码器
    avcodec_open2(c, codec, NULL);

    // 分配音频AVPacket包
    ost->packet = av_packet_alloc();

    // 将编码器参数复制到流参数
    avcodec_parameters_from_context(ost->stream->codecpar, c);
    return 0;
}

// 释放流资源
void free_stream(AVFormatContext *oc, OutputStream *ost)
{
    avcodec_close(ost->enc);  // 关闭编码器
    avcodec_free_context(&ost->enc);  // 释放编码器上下文内存，并将指针置空

    av_buffer_unref(&(ost->packet->buf));  // 减少缓冲区引用计数，如果为0则释放

    av_packet_unref(ost->packet);  // 解引用AVPacket，清理缓冲区
    av_packet_free(&ost->packet);  // 释放AVPacket结构体内存
}

// 初始化RKMedia的FFmpeg推流上下文
int init_rkmedia_ffmpeg_context(RKMEDIA_FFMPEG_CONFIG *ffmpeg_config)
{
    AVOutputFormat *fmt = NULL;  // 输出格式指针
    AVCodec *audio_codec = NULL;  // 音频编码器指针
    AVCodec *video_codec = NULL;  // 视频编码器指针
    int ret = 0;  // 返回值，0表示成功

    // 根据协议类型选择封装格式
    if (ffmpeg_config->protocol_type == FLV_PROTOCOL)  // FLV协议（用于RTMP推流）
    {
        // 初始化一个FLV格式的AVFormatContext
        // 参数: 输出上下文指针, 输出格式(NULL自动检测), 格式名"flv", 输出URL
        ret = avformat_alloc_output_context2(&ffmpeg_config->oc, NULL, "flv", ffmpeg_config->network_addr);
        if (ret < 0)  // 返回值小于0表示失败
        {
            return -1;
        }
    }
    else if (ffmpeg_config->protocol_type == TS_PROTOCOL)  // TS协议（用于SRT/UDP/RTSP推流）
    {
        // 初始化一个MPEGTS格式的AVFormatContext
        ret = avformat_alloc_output_context2(&ffmpeg_config->oc, NULL, "mpegts", ffmpeg_config->network_addr);
        if (ret < 0)
        {
            return -1;
        }
    }
  
    fmt = ffmpeg_config->oc->oformat;  // 获取输出格式（FLV或MPEGTS）
    // 指定输出格式使用的编码器类型
    fmt->video_codec = ffmpeg_config->video_codec;  // 设置视频编码器（如H264）
    fmt->audio_codec = ffmpeg_config->audio_codec;  // 设置音频编码器（如AAC）

    // 如果视频编码器不是AV_CODEC_ID_NONE（表示需要视频流）
    if (fmt->video_codec != AV_CODEC_ID_NONE)
    {
        // 添加视频流到输出上下文
        ret = add_stream(&ffmpeg_config->video_stream, ffmpeg_config->oc, &video_codec, 
                         fmt->video_codec, ffmpeg_config->width, ffmpeg_config->height);
        if (ret < 0)  // 添加失败
        {
            avcodec_free_context(&ffmpeg_config->video_stream.enc);  // 释放编码器上下文
            free_stream(ffmpeg_config->oc, &ffmpeg_config->video_stream);  // 释放流资源
            avformat_free_context(ffmpeg_config->oc);  // 释放输出上下文
            return -1;
        }

        // 打开视频编码器
        ret = open_video(ffmpeg_config->oc, video_codec, &ffmpeg_config->video_stream, NULL);
        if (ret < 0)  // 打开失败
        {
            avformat_free_context(ffmpeg_config->oc);  // 释放输出上下文
        }
    }

#if 0  // 音频流代码被注释，当前版本未使用音频
    if (fmt->audio_codec != AV_CODEC_ID_NONE)
    {
        ret = (&ffmpeg_config->audio_stream, ffmpeg_config->oc, &audio_codec, fmt->audio_codec);
        if (ret < 0)
        {
            avcodec_free_context(&ffmpeg_config->audio_stream.enc);
            free_stream(ffmpeg_config->oc, &ffmpeg_config->audio_stream);
            avformat_free_context(ffmpeg_config->oc);
            return -1;
        }

        ret = open_audio(ffmpeg_config->oc, audio_codec, &ffmpeg_config->audio_stream, NULL);
        if (ret < 0)
        {
            avformat_free_context(ffmpeg_config->oc);
        }
    }
#endif

    // 打印输出格式信息到控制台，用于调试
    // 参数: 输出上下文, 索引0, 输出URL, 1表示输出到标准输出
    av_dump_format(ffmpeg_config->oc, 0, ffmpeg_config->network_addr, 1);

    // 如果输出格式需要打开文件（网络流也需要）
    if (!(fmt->flags & AVFMT_NOFILE))
    {
        // 打开输出文件/网络流
        // 参数: AVIOContext指针, URL, 打开模式（AVIO_FLAG_WRITE表示写）
        ret = avio_open(&ffmpeg_config->oc->pb, ffmpeg_config->network_addr, AVIO_FLAG_WRITE);
        if (ret < 0)  // 打开失败
        {
            free_stream(ffmpeg_config->oc, &ffmpeg_config->video_stream);  // 释放视频流
            free_stream(ffmpeg_config->oc, &ffmpeg_config->audio_stream);  // 释放音频流
            avformat_free_context(ffmpeg_config->oc);  // 释放输出上下文
            return -1;
        }
    }

    // 写入文件头（FLV格式会写入FLV Header，TS格式会写入PAT/PMT）
    // NULL表示不使用额外选项
    avformat_write_header(ffmpeg_config->oc, NULL);
    return 0;  // 返回0表示成功
}



