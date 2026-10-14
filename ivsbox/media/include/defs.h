#ifndef __DEFS_H__
#define __DEFS_H__

/**
 * @file defs.h
 * @brief 全局配置参数与业务常量定义
 * @note 所有的路径、阈值、API接口均在此统一维护
 */

/* -------------------------------------------------------------------------- */
/* 路径与存储配置                                                             */
/* -------------------------------------------------------------------------- */

/** @brief 系统日志保存路径 */
#define PATH_LOG_FILE          "/opt/log/media.log"

/** @brief 录制视频存储根目录 (建议挂载到 SD 卡或大容量分区) */
#define PATH_RECORD_ROOT       "/mnt/sdcard/video"

/** @brief V4L2 摄像头设备节点 */
#define PATH_VIDEO_DEVICE      "/dev/video0"


/* -------------------------------------------------------------------------- */
/* 媒体业务参数                                                               */
/* -------------------------------------------------------------------------- */
/** @brief 单个录制任务支持的最大流数量 (通常为 1路视频 + N路音频) */
#define RECORD_MAX_STREAMS     8

/** @brief 最大并发处理通道数 (任务槽位数量) */
#define MEDIA_MAX_CHANNELS     8

/** @brief 录制线程栈大小 (512KB)，确保 FFmpeg 运行有足够的局部变量空间 */
#define MEDIA_STACK_SIZE       (512 * 1024)

// --- 录制相关 ---

/** @brief 视频自动分片时长 (单位: 秒)，建议 15 分钟 */
#define RECORD_SEGMENT_SEC     (15 * 60) 

/** @brief 业务心跳保活超时时间 (单位: 秒)，超过此时间未查询状态则自动停录 */
#define RECORD_TIMEOUT_SEC     (15 * 60) 

/** @brief 强制分片容忍度 (单位: 秒)。若时间到但无关键帧，超出此时间则强行切片 */
#define RECORD_FORCE_SEG_LIMIT 30        

// --- 抓图相关 ---

/** @brief 抓图分辨率 - 宽度 */
#define CAPTURE_WIDTH          1280

/** @brief 抓图分辨率 - 高度 */
#define CAPTURE_HEIGHT         720

/** @brief 抓图丢帧数。开启摄像头后丢弃前 N 帧，等待 ISP 自动曝光和白平衡稳定 */
#define CAPTURE_SKIP_FRAMES    5         


/* -------------------------------------------------------------------------- */
/* HTTP API 接口定义                                                          */
/* -------------------------------------------------------------------------- */

/** * @brief 接口：同步抓图
 * @example /capture?path=/data/snapshot/xxx.jpg 
 */
#define API_URI_CAPTURE        "/capture"

/** * @brief 接口：开启录制
 * @example /record/start?id=0&url=rtsp://xxx 
 */
#define API_URI_RECORD_START   "/record/start"

/** * @brief 接口：停止录制
 * @example /record/stop?id=0 
 */
#define API_URI_RECORD_STOP    "/record/stop"

/** * @brief 接口：查询状态 (兼具心跳续命功能)
 * @example /record/status?id=0 
 */
#define API_URI_RECORD_STATUS  "/record/status"

// --- URL 查询参数名称 ---

/** @brief 参数名：保存路径 (用于抓图接口) */
#define API_QUERY_PATH         "path"

/** @brief 参数名：通道索引 ID (用于录制相关接口) */
#define API_QUERY_ID           "id"

/** @brief 参数名：RTSP 源地址 (用于开始录制接口) */
#define API_QUERY_URL          "url"


/* -------------------------------------------------------------------------- */
/* 系统限制与协议配置                                                         */
/* -------------------------------------------------------------------------- */

/** @brief 单个日志文件最大字节数，超过后自动进行滚动备份 (5MB) */
#define LOG_MAX_FILE_SIZE      (5 * 1024 * 1024) 

/** @brief RTSP 连接与读取超时 (单位: 微秒)，FFmpeg 内部使用 */
#define RTSP_CONN_TIMEOUT_MS   5000000           

/** @brief RTSP 网络接收缓冲区大小 (2MB)，防止高码率时丢包 */
#define RTSP_BUFFER_SIZE       (2 * 1024 * 1024)

#endif // __DEFS_H__
