#include "rkmedia_data_process.h"
#include "ffmpeg_video_queue.h"
#include "ffmpeg_audio_queue.h"
#include "rkmedia_module.h"
#include "rkmedia_ffmpeg_config.h"
#include "rkmedia_data_process.h"
#include "SDL.h"
#include "SDL_ttf.h"

extern VIDEO_QUEUE *high_video_queue;
extern VIDEO_QUEUE *low_video_queue;

// 将输入值对齐到指定倍数的辅助函数
// input_value: 需要对齐的输入值
// align: 对齐倍数（如16）
static int get_align16_value(int input_value, int align)
{
    int handle_value = 0;  // 存储对齐后的值，初始为0
    
    // 如果对齐倍数非0，且输入值不是对齐倍数的整数倍
    if (align && (input_value % align))
        // 向上取整到align的倍数
        // 例如：input_value=100, align=16, 则 (100/16+1)*16 = (6+1)*16 = 112
        handle_value = (input_value / align + 1) * align;
    
    return handle_value;  // 返回对齐后的值（如果已对齐则返回原值）
}

// 从RV1126视频编码数据转换到FFMPEG的Video AVPacket（高分辨率）
AVPacket *get_high_ffmpeg_video_avpacket(AVPacket *pkt)
{
    // 从高分辨率视频队列获取编码后的视频数据包（阻塞等待）
    video_data_packet_t *video_data_packet = high_video_queue->getVideoPacketQueue();

    if (video_data_packet != NULL)  // 成功获取到视频数据
    {
        /*
        重新分配给定的缓冲区
        1. 如果入参的 AVBufferRef 为空，直接调用 av_realloc 分配一个新的缓存区
        2. 如果入参的缓存区长度和入参 size 相等，直接返回 0
        3. 如果对应的 AVBuffer 设置了 BUFFER_FLAG_REALLOCATABLE 标志，或者不可写，
           再或者 AVBufferRef data 字段指向的数据地址和 AVBuffer 的 data 地址不同，
           递归调用 av_buffer_realloc 分配一个新的 buffer，并将 data 拷贝过去
        4. 不满足上面的条件，直接调用 av_realloc 重新分配缓存区
        */
        // 重新分配AVPacket缓冲区，大小为视频帧大小+70字节（额外安全空间）
        int ret = av_buffer_realloc(&pkt->buf, video_data_packet->video_frame_size + 70);
        if (ret < 0)  // 内存分配失败
        {
            return NULL;  // 返回空指针
        }
        
        // 设置AVPacket的数据大小为视频帧大小
        pkt->size = video_data_packet->video_frame_size;
        
        // 将RV1126编码的视频数据拷贝到AVPacket缓冲区
        memcpy(pkt->buf->data, video_data_packet->buffer, video_data_packet->video_frame_size);
        
        // 设置pkt->data指针指向缓冲区数据
        pkt->data = pkt->buf->data;
        
        // 标记为关键帧（I帧），FLV/RTMP推流时需要
        pkt->flags |= AV_PKT_FLAG_KEY;
        
        // 释放video_data_packet结构体内存
        if (video_data_packet != NULL)
        {
            free(video_data_packet);  // 释放内存
            video_data_packet = NULL;  // 指针置空，防止野指针
        }

        return pkt;  // 返回填充好的AVPacket
    }
    else  // 队列为空，没有数据
    {
        return NULL;  // 返回空指针
    }
}

// 从RV1126视频编码数据转换到FFMPEG的Video AVPacket（低分辨率）
AVPacket *get_low_ffmpeg_video_avpacket(AVPacket *pkt)
{
    // 从低分辨率视频队列获取编码后的视频数据包
    video_data_packet_t *video_data_packet = low_video_queue->getVideoPacketQueue();

    if (video_data_packet != NULL)  // 成功获取到视频数据
    {
        // 重新分配AVPacket缓冲区
        int ret = av_buffer_realloc(&pkt->buf, video_data_packet->video_frame_size + 70);
        if (ret < 0)  // 内存分配失败
        {
            return NULL;
        }
        
        // 设置AVPacket大小
        pkt->size = video_data_packet->video_frame_size;
        
        // 拷贝视频数据
        memcpy(pkt->buf->data, video_data_packet->buffer, video_data_packet->video_frame_size);
        
        // 设置数据指针
        pkt->data = pkt->buf->data;
        
        // 标记为关键帧
        pkt->flags |= AV_PKT_FLAG_KEY;
        
        // 释放原始数据包
        if (video_data_packet != NULL)
        {
            free(video_data_packet);
            video_data_packet = NULL;
        }

        return pkt;
    }
    else
    {
        return NULL;
    }
}

// 将AVPacket写入FFmpeg输出上下文（推流）
// fmt_ctx: FFmpeg格式上下文
// time_base: 编码器时间基
// st: 视频/音频流
// pkt: 要写入的AVPacket
int write_ffmpeg_avpacket(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt)
{
    // 将输出数据包的时间戳从编码器时基转换为流时基
    // 例如：编码器时间基是1/25秒，流时间基可能是1/1000秒
    av_packet_rescale_ts(pkt, *time_base, st->time_base);
    
    // 设置pkt所属的流索引（告诉FFmpeg这个包属于哪个流）
    pkt->stream_index = st->index;

    // 将数据包交错写入输出文件/网络流（自动处理音视频交错）
    return av_interleaved_write_frame(fmt_ctx, pkt);
}

// 处理高分辨率视频AVPacket（推流主逻辑）
// oc: FFmpeg格式上下文
// ost: 输出流（包含编码器上下文、AVPacket等）
int deal_high_video_avpacket(AVFormatContext *oc, OutputStream *ost)
{
    int ret;  // 返回值
    AVCodecContext *c = ost->enc;  // 获取编码器上下文
    
    // 从队列获取视频数据并转换为AVPacket
    AVPacket *video_packet = get_high_ffmpeg_video_avpacket(ost->packet);
    
    if (video_packet != NULL)  // 成功获取到数据
    {
        // 设置PTS（显示时间戳），按帧率累加
        // 例如：25fps时，PTS依次为0,1,2,3...
        video_packet->pts = ost->next_timestamp++;
    }

    // 将AVPacket写入输出流进行推流
    ret = write_ffmpeg_avpacket(oc, &c->time_base, ost->stream, video_packet);
    if (ret != 0)  // 写入失败
    {
        printf("write video avpacket error");
        return -1;  // 返回-1表示失败
    }

    return 0;  // 成功
}

// 处理低分辨率视频AVPacket
int deal_low_video_avpacket(AVFormatContext *oc, OutputStream *ost)
{
    int ret;
    AVCodecContext *c = ost->enc;
    
    // 从低分辨率队列获取数据
    AVPacket *video_packet = get_low_ffmpeg_video_avpacket(ost->packet);
    
    if (video_packet != NULL)
    {
        video_packet->pts = ost->next_timestamp++;  // 设置PTS
    }

    // 写入推流
    ret = write_ffmpeg_avpacket(oc, &c->time_base, ost->stream, video_packet);
    if (ret != 0)
    {
        printf("write video avpacket error");
        return -1;
    }

    return 0;
}

#if 0  // 以下代码被注释，未使用（OSD文字叠加功能）
// 高分辨率OSD（屏幕字符叠加）线程（已废弃）
void *osd_venc_thread(void *args)
{
    pthread_detach(pthread_self());  // 分离线程，自动回收资源
    int ret;
    TTF_Font *ttf_font;  // TTF字体
    char *pstr = "2019-11-21 15:40:29";  // 要显示的字符串
    SDL_Surface *text_surface;  // SDL表面（文本渲染后的图像）
    SDL_Surface *convert_text_surface;  // 转换后的SDL表面
    SDL_PixelFormat *pixel_format;  // 像素格式

    // TTF模块的初始化（TrueType字体）
    ret = TTF_Init();
    if (ret < 0)
    {
        printf("TTF_Init Failed...\n");
    }

    // 打开TTF字库文件
    ttf_font = TTF_OpenFont("./fzlth.ttf", 48);  // 字体文件路径，字号48
    if (ttf_font == NULL)
    {
        printf("TTF_OpenFont Failed...\n");
    }

    // SDL_COLOR黑色(RGB:0,0,0)
    SDL_Color sdl_color;
    sdl_color.r = 0;  // 红色分量
    sdl_color.g = 0;  // 绿色分量
    sdl_color.b = 0;  // 蓝色分量
    // 渲染文字为SDL表面（实心文字）
    text_surface = TTF_RenderText_Solid(ttf_font, pstr, sdl_color);

    // 配置ARGB_8888像素格式
    pixel_format = (SDL_PixelFormat *)malloc(sizeof(SDL_PixelFormat));
    pixel_format->BitsPerPixel = 32;   // 每个像素32位
    pixel_format->BytesPerPixel = 4;   // 每个像素4字节
    pixel_format->Amask = 0XFF000000;  // Alpha掩码
    pixel_format->Rmask = 0X00FF0000;  // 红色掩码
    pixel_format->Gmask = 0X0000FF00;  // 绿色掩码
    pixel_format->Bmask = 0X000000FF;  // 蓝色掩码
    
    // 转换SDL表面到指定像素格式
    convert_text_surface = SDL_ConvertSurface(text_surface, pixel_format, 0);
    if (convert_text_surface == NULL)
    {
        printf("convert_text_surface failed...\n");
    }

    BITMAP_S bitmap;  // 位图结构体
    // 宽度对齐到16的倍数（硬件要求）
    bitmap.u32Width = get_align16_value(convert_text_surface->w, 16);
    // 高度对齐到16的倍数
    bitmap.u32Height = get_align16_value(convert_text_surface->h, 16);
    bitmap.enPixelFormat = PIXEL_FORMAT_ARGB_8888;  // 像素格式
    // 分配位图数据内存
    bitmap.pData = malloc((bitmap.u32Width) * (bitmap.u32Height) * pixel_format->BytesPerPixel);
    // 拷贝文字图像数据到位图
    memcpy(bitmap.pData, convert_text_surface->pixels, 
           (convert_text_surface->w) * (convert_text_surface->h) * pixel_format->BytesPerPixel);

    OSD_REGION_INFO_S rgn_info;  // OSD区域信息
    rgn_info.enRegionId = REGION_ID_0;  // 区域ID为0
    rgn_info.u32Width = bitmap.u32Width;  // OSD宽度
    rgn_info.u32Height = bitmap.u32Height;  // OSD高度
    rgn_info.u32PosX = 128;  // X坐标（左上角）
    rgn_info.u32PosY = 128;  // Y坐标（左上角）
    rgn_info.u8Enable = 1;   // 使能OSD
    rgn_info.u8Inverse = 0;  // 禁止翻转

    // 设置VENC的OSD位图
    ret = RK_MPI_VENC_RGN_SetBitMap(0, &rgn_info, &bitmap);
    if (ret)
    {
        printf("HIGI_RK_MPI_VENC_RGN_SetBitMap failed...\n");
    }
    else
    {
        printf("HIGI_RK_MPI_VENC_RGN_SetBitMap Success...\n");
    }

    return NULL;
}

// 低分辨率OSD线程（已废弃）
void *low_osd_venc_thread(void *args)
{
    // 类似上面的实现，位置不同（X=256, Y=256）
    // ... 省略注释 ...
}
#endif

// 高分辨率摄像头编码线程
// 功能：从VENC（视频编码器）获取编码后的H264数据，放入高分辨率队列
void *camera_venc_thread(void *args)
{
    pthread_detach(pthread_self());  // 分离线程，结束时自动回收资源
    MEDIA_BUFFER mb = NULL;  // 媒体缓冲区指针

    // 获取线程参数（VENC通道ID）
    VENC_PROC_PARAM venc_arg = *(VENC_PROC_PARAM *)args;
    free(args);  // 释放参数内存（不再需要）

    printf("video_venc_thread...\n");  // 打印线程启动信息

    while (1)  // 无限循环，持续获取编码数据
    {
        // 从指定VENC通道获取编码后的媒体缓冲区
        // 参数：模块ID=VENC，通道ID=venc_arg.vencId，超时=-1（无限等待）
        mb = RK_MPI_SYS_GetMediaBuffer(RK_ID_VENC, venc_arg.vencId, -1);
        if (!mb)  // 获取失败
        {
            printf("high_get venc media buffer error\n");
            break;  // 退出循环
        }

        // 分配video_data_packet_t结构体（自定义的视频数据包）
        video_data_packet_t *video_data_packet = (video_data_packet_t *)malloc(sizeof(video_data_packet_t));
        
        // 将VENC视频缓冲区数据拷贝到video_data_packet的buffer中
        memcpy(video_data_packet->buffer, RK_MPI_MB_GetPtr(mb), RK_MPI_MB_GetSize(mb));
        
        // 记录视频帧大小
        video_data_packet->video_frame_size = RK_MPI_MB_GetSize(mb);
        
        // 将视频数据包放入高分辨率队列（等待推流线程取走）
        high_video_queue->putVideoPacketQueue(video_data_packet);
        
        // 释放VENC媒体缓冲区（引用计数减1）
        RK_MPI_MB_ReleaseBuffer(mb);
    }

    // ========== 线程退出时的清理工作 ==========
    MPP_CHN_S vi_channel;   // VI通道结构体
    MPP_CHN_S venc_channel; // VENC通道结构体

    vi_channel.enModId = RK_ID_VI;   // VI模块ID
    vi_channel.s32ChnId = 0;          // VI通道0
    
    venc_channel.enModId = RK_ID_VENC;  // VENC模块ID
    venc_channel.s32ChnId = venc_arg.vencId;  // VENC通道ID

    int ret;
    // 解绑VI和VENC
    ret = RK_MPI_SYS_UnBind(&vi_channel, &venc_channel);
    if (ret != 0)
    {
        printf("VI UnBind failed \n");
    }
    else
    {
        printf("Vi UnBind success\n");
    }

    // 销毁VENC通道
    ret = RK_MPI_VENC_DestroyChn(0);
    if (ret)
    {
        printf("Destroy Venc error! ret=%d\n", ret);
        return 0;
    }
    
    // 禁用VI通道
    ret = RK_MPI_VI_DisableChn(0, 0);
    if (ret)
    {
        printf("Disable Chn Venc error! ret=%d\n", ret);
        return 0;
    }

    return NULL;
}

// RGA处理线程
// 功能：从RGA获取缩放后的图像数据，发送到低分辨率编码器
void * get_rga_thread(void * args)
{
    MEDIA_BUFFER mb = NULL;  // 媒体缓冲区

    while (1)  // 无限循环
    {
        // 从RGA通道0获取处理后的媒体缓冲区（缩放后的720P图像）
        mb = RK_MPI_SYS_GetMediaBuffer(RK_ID_RGA, 0, -1);
        if(!mb)  // 获取失败
        {
            break;  // 退出循环
        }

        // 将RGA处理后的数据发送到VENC通道1（低分辨率编码器）
        RK_MPI_SYS_SendMediaBuffer(RK_ID_VENC, 1, mb);
        
        // 释放媒体缓冲区
        RK_MPI_MB_ReleaseBuffer(mb);
    }

    return NULL;
}

// 低分辨率摄像头编码线程
// 功能：从VENC（低分辨率编码器）获取编码后的H264数据，放入低分辨率队列
void *low_camera_venc_thread(void *args)
{
    pthread_detach(pthread_self());  // 分离线程
    MEDIA_BUFFER mb = NULL;  // 媒体缓冲区

    VENC_PROC_PARAM venc_arg = *(VENC_PROC_PARAM *)args;  // 获取参数
    free(args);  // 释放参数内存

    printf("low_video_venc_thread...\n");  // 打印启动信息

    while (1)  // 无限循环
    {
        // 从VENC通道1获取编码后的媒体缓冲区（720P编码数据）
        mb = RK_MPI_SYS_GetMediaBuffer(RK_ID_VENC, 1, -1);
        if (!mb)  // 获取失败
        {
            printf("low_venc break....\n");
            break;
        }

        // 分配视频数据包结构体
        video_data_packet_t *video_data_packet = (video_data_packet_t *)malloc(sizeof(video_data_packet_t));
        
        // 拷贝编码数据
        memcpy(video_data_packet->buffer, RK_MPI_MB_GetPtr(mb), RK_MPI_MB_GetSize(mb));
        
        // 记录帧大小
        video_data_packet->video_frame_size = RK_MPI_MB_GetSize(mb);
        
        // 放入低分辨率队列
        low_video_queue->putVideoPacketQueue(video_data_packet);
        
        // 释放VENC缓冲区
        RK_MPI_MB_ReleaseBuffer(mb);
    }

    return NULL;
}

// 高分辨率音视频合成推流线程
// 功能：从高分辨率队列取数据，转换为AVPacket，推流到网络
void *high_video_push_thread(void *args)
{
    pthread_detach(pthread_self());  // 分离线程
    
    // 获取FFmpeg配置（深拷贝）
    RKMEDIA_FFMPEG_CONFIG ffmpeg_config = *(RKMEDIA_FFMPEG_CONFIG *)args;
    free(args);  // 释放原参数内存
    
    AVOutputFormat *fmt = NULL;  // 输出格式（未使用）
    int ret;  // 返回值

    while (1)  // 无限循环，持续推流
    {
        // 处理高分辨率视频AVPacket（从队列取数据→转换→推流）
        ret = deal_high_video_avpacket(ffmpeg_config.oc, &ffmpeg_config.video_stream);
        if (ret == -1)  // 处理失败（如队列为空且出错）
        {
            printf("deal_video_avpacket error\n");
            break;  // 退出循环
        }
    }

    // ========== 线程退出时的清理 ==========
    av_write_trailer(ffmpeg_config.oc);  // 写入文件尾（FLV/TS格式需要）
    // 释放视频流资源
    free_stream(ffmpeg_config.oc, &ffmpeg_config.video_stream);
    // 释放音频流资源
    free_stream(ffmpeg_config.oc, &ffmpeg_config.audio_stream);
    // 关闭AVIO上下文（网络连接）
    avio_closep(&ffmpeg_config.oc->pb);
    // 释放AVFormatContext
    avformat_free_context(ffmpeg_config.oc);
    return NULL;
}

// 低分辨率推流线程
void *low_video_push_thread(void *args)
{
    pthread_detach(pthread_self());  // 分离线程
    
    // 获取FFmpeg配置
    RKMEDIA_FFMPEG_CONFIG ffmpeg_config = *(RKMEDIA_FFMPEG_CONFIG *)args;
    free(args);
    
    AVOutputFormat *fmt = NULL;
    int ret;

    while (1)  // 无限循环
    {
        // 处理低分辨率视频AVPacket
        ret = deal_low_video_avpacket(ffmpeg_config.oc, &ffmpeg_config.video_stream);
        if (ret == -1)
        {
            printf("deal_video_avpacket error\n");
            break;
        }
    }

    // 清理资源
    av_write_trailer(ffmpeg_config.oc);
    free_stream(ffmpeg_config.oc, &ffmpeg_config.video_stream);
    free_stream(ffmpeg_config.oc, &ffmpeg_config.audio_stream);
    avio_closep(&ffmpeg_config.oc->pb);
    avformat_free_context(ffmpeg_config.oc);
    return NULL;
}