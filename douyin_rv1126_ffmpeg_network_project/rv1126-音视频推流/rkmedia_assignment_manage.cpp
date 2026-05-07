#include "rkmedia_assignment_manage.h"
#include "rkmedia_data_process.h"
#include "rkmedia_ffmpeg_config.h"
#include "rkmedia_module.h"

#include "rkmedia_ffmpeg_config.h"
#include "rkmedia_container.h"

// 初始化RV1126的第一次任务分配（配置推流任务）
// protocol_type: 协议类型（0=FLV/RTMP, 1=TS/SRT）
// network_address: 高分辨率推流地址（如 rtmp://xxx/live/stream1）
// low_url_type: 低分辨率协议类型
// low_url_address: 低分辨率推流地址（如 rtmp://xxx/live/stream2）
int init_rv1126_first_assignment(int protocol_type, char * network_address, int low_url_type, char *low_url_address)
{
    int ret;  // 用于存储函数返回值，0表示成功

    // ========== 1. 配置高分辨率推流（1080P） ==========
    // 为高分辨率推流配置分配内存
    RKMEDIA_FFMPEG_CONFIG *ffmpeg_config = (RKMEDIA_FFMPEG_CONFIG *)malloc(sizeof(RKMEDIA_FFMPEG_CONFIG));
    if (ffmpeg_config == NULL)  // 检查内存分配是否成功
    {
        printf("malloc ffmpeg_config failed\n");  // 打印错误信息
    }
    ffmpeg_config->width = 1920;              // 设置视频宽度为1920像素（1080P）
    ffmpeg_config->height = 1080;             // 设置视频高度为1080像素（1080P）
    ffmpeg_config->config_id = 0;             // 设置配置ID为0（高码流标识）
    ffmpeg_config->protocol_type = protocol_type;  // 设置协议类型（FLV或TS）
    ffmpeg_config->video_codec = AV_CODEC_ID_H264; // 设置视频编码为H264
    ffmpeg_config->audio_codec = AV_CODEC_ID_AAC;  // 设置音频编码为AAC
    // 将网络地址字符串拷贝到配置结构体中
    memcpy(ffmpeg_config->network_addr, network_address, strlen(network_address));
    // 初始化FFmpeg输出模块（创建输出上下文、打开编码器、连接网络）
    init_rkmedia_ffmpeg_context(ffmpeg_config);

    // ========== 2. 配置低分辨率推流（720P） ==========
    // 为低分辨率推流配置分配内存
    RKMEDIA_FFMPEG_CONFIG *low_ffmpeg_config = (RKMEDIA_FFMPEG_CONFIG *)malloc(sizeof(RKMEDIA_FFMPEG_CONFIG));
    if (ffmpeg_config == NULL)  // 注意：这里判断条件写错了，应该是判断low_ffmpeg_config
    {
        printf("malloc ffmpeg_config failed\n");
    }

    low_ffmpeg_config->width = 1280;           // 设置视频宽度为1280像素（720P）
    low_ffmpeg_config->height = 720;           // 设置视频高度为720像素（720P）
    low_ffmpeg_config->config_id = 1;          // 设置配置ID为1（低码流标识）
    low_ffmpeg_config->protocol_type = protocol_type;  // 设置协议类型
    low_ffmpeg_config->video_codec = AV_CODEC_ID_H264; // 视频编码H264
    low_ffmpeg_config->audio_codec = AV_CODEC_ID_AAC;  // 音频编码AAC
    // 将低分辨率推流地址拷贝到配置结构体
    memcpy(low_ffmpeg_config->network_addr, low_url_address, strlen(low_url_address));
    // 初始化低分辨率FFmpeg输出模块
    init_rkmedia_ffmpeg_context(low_ffmpeg_config);

    printf("Bind Before...\n");  // 打印绑定前的提示信息

    // ========== 3. 定义MPP通道结构体 ==========
    MPP_CHN_S vi_channel;      // VI（视频输入）通道结构体
    MPP_CHN_S venc_channel;    // VENC（视频编码器）通道结构体（高分辨率）
    MPP_CHN_S rga_channel;     // RGA（图像缩放/旋转）通道结构体
    MPP_CHN_S low_venc_channel; // 低分辨率VENC通道结构体
    
    // ========== 4. 从容器获取VI模块ID ==========
    RV1126_VI_CONTAINTER vi_container;  // 定义VI容器
    get_vi_container(0, &vi_container); // 获取索引0的VI容器（存储了VI通道ID）

    // ========== 5. 从容器获取高分辨率VENC模块ID ==========
    RV1126_VENC_CONTAINER venc_container;  // 定义VENC容器
    get_venc_container(0, &venc_container); // 获取索引0的VENC容器（高分辨率编码器ID）

    // ========== 6. 配置VI通道 ==========
    vi_channel.enModId = RK_ID_VI;              // 设置模块ID为VI（视频输入）
    vi_channel.s32ChnId = vi_container.vi_id;   // 设置VI通道ID（从容器中获取）
    
    // ========== 7. 配置高分辨率VENC通道 ==========
    venc_channel.enModId = RK_ID_VENC;          // 设置模块ID为VENC（视频编码器）
    venc_channel.s32ChnId = venc_container.venc_id; // 设置VENC通道ID

    // ========== 8. 绑定VI和VENC节点（摄像头 → 编码器） ==========
    ret = RK_MPI_SYS_Bind(&vi_channel, &venc_channel);  // 执行绑定操作
    if (ret != 0)  // 绑定失败
    {
        printf("bind venc error\n");  // 打印错误信息
        return -1;  // 返回-1表示失败
    }
    else  // 绑定成功
    {
        printf("bind venc success\n");  // 打印成功信息
    }

    // ========== 9. 获取低分辨率VENC容器 ==========
    RV1126_VENC_CONTAINER low_venc_container;  // 定义低分辨率VENC容器
    get_venc_container(1, &low_venc_container); // 获取索引1的VENC容器（低分辨率编码器ID）
    
    // ========== 10. 配置RGA通道（用于图像缩放） ==========
    rga_channel.enModId = RK_ID_RGA;     // 设置模块ID为RGA（图像处理加速器）
    rga_channel.s32ChnId = 0;            // RGA通道0（用于1080P→720P缩放）
    
    // ========== 11. 绑定VI和RGA节点（摄像头 → 图像缩放） ==========
    ret = RK_MPI_SYS_Bind(&vi_channel, &rga_channel);  // 执行绑定
    if (ret != 0)  // 绑定失败
    {
        printf("vi bind rga error\n");
        return -1;
    }
    else  // 绑定成功
    {
        printf("vi bind rga success\n");
    }

    // ========== 12. 创建所有工作线程 ==========
    pthread_t pid;  // 线程ID变量（会被多次覆盖，实际只能记录最后一个线程ID）

    // 12.1 创建高分辨率编码线程
    VENC_PROC_PARAM *venc_arg_params = (VENC_PROC_PARAM *)malloc(sizeof(VENC_PROC_PARAM));  // 分配参数结构体内存
    if (venc_arg_params == NULL)  // 检查内存分配
    {
        printf("malloc venc arg error\n");
        free(venc_arg_params);  // 释放内存（注意：NULL时free是安全的）
    }
    venc_arg_params->vencId = venc_channel.s32ChnId;  // 设置编码器ID为高分辨率VENC通道ID
    // 创建线程，线程函数为camera_venc_thread，传入参数venc_arg_params
    ret = pthread_create(&pid, NULL, camera_venc_thread, (void *)venc_arg_params);
    if (ret != 0)  // 线程创建失败
    {
        printf("create camera_venc_thread failed\n");
    }

    // 12.2 创建RGA处理线程（将缩放后的图像发送到低分辨率编码器）
    ret = pthread_create(&pid, NULL, get_rga_thread, NULL);  // 创建线程，无参数
    if(ret != 0)  // 线程创建失败
    {
        printf("create get_rga_thread failed\n");
    }

    // 12.3 创建低分辨率编码线程
    VENC_PROC_PARAM *low_venc_arg_params = (VENC_PROC_PARAM *)malloc(sizeof(VENC_PROC_PARAM));  // 分配参数内存
    if (venc_arg_params == NULL)  // 注意：这里判断条件错误，应该是判断low_venc_arg_params
    {
        printf("malloc venc arg error\n");
        free(venc_arg_params);  // 错误：应该free low_venc_arg_params
    }
    low_venc_arg_params->vencId = low_venc_channel.s32ChnId;  // 设置编码器ID为低分辨率VENC通道ID
    // 创建低分辨率编码线程
    ret = pthread_create(&pid, NULL, low_camera_venc_thread, (void *)low_venc_arg_params);
    if (ret != 0)
    {
        printf("create camera_venc_thread failed\n");
    }

    // 12.4 创建高分辨率推流线程
    // 线程函数为high_video_push_thread，传入高分辨率FFmpeg配置
    ret = pthread_create(&pid, NULL, high_video_push_thread, (void *)ffmpeg_config);
    if (ret != 0)
    {
        printf("push_server_thread error\n");
    }

    // 12.5 创建低分辨率推流线程
    // 线程函数为low_video_push_thread，传入低分辨率FFmpeg配置
    ret = pthread_create(&pid, NULL, low_video_push_thread, (void *)low_ffmpeg_config);
    if (ret != 0)
    {
        printf("push_server_thread error\n");
    }

    return 0;  // 返回0表示所有初始化成功
}