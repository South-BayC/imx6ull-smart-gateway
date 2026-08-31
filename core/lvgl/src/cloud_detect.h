/**
 * cloud_detect.h — 云端精判客户端（识别模式=云端精判 时使用）
 *
 * 协议: POST http://<server>/detect?w=&h=   body = RGB565 原始帧（w*h*2 字节，行连续）
 *       响应 JSON: {"person":bool, "known":bool, "names":[...], "type":"person",
 *                   "count":N, "conf":F}
 *       （云端两级判定：YOLOv8 人员/类型检测 + face_recognition 白名单比对，
 *        服务实现见 cloud/server.py，运行于 VM/主机；白名单=cloud/whitelist/<姓名>.jpg）
 *
 * 行为: 阻塞式（连接 2.5s + 收发 2s 超时）；由 detector 工作线程调用
 *       （低优先级，不阻塞显示）。失败返回 -1，调用方按 fail-safe 处理
 *       （有运动未确认 → 人员入侵告警，不漏报）。
 *
 * 服务器地址: 编译期 #define（对齐 mqtt_hub 风格）；运行时可经
 *             cloud_detect_set_server 修改（后续接设置页）。
 */
#ifndef CLOUD_DETECT_H
#define CLOUD_DETECT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 设置云端服务地址
 * @param ipport "IP:端口"（如 "192.168.3.26:8000"）
 */
void cloud_detect_set_server(const char *ipport);

/**
 * 云端复核服务可达性探测（开机就绪上报用）
 * 向云端服务发起一次轻量 TCP 连接（非阻塞 connect + select 超时），
 * 确认服务是否在线；不传帧、不改任何状态。
 * @return 0=服务可达；非0=不可达（断网/服务未启动，云端复核走"本地兜底"）
 * @note 与 cloud_detect_query 共用连接探测，避免启动时阻塞主线程过久
 */
int cloud_detect_ready(void);

/**
 * 上传一帧并获取云端结论（人员检测+类型归一+白名单比对；阻塞，约 0.1~2.5s）
 * @param rgb565 帧数据（RGB565，w*h*2 字节，行连续）
 * @param type   出参：云端入侵类型键 "person"/"animal"/"object"
 *               （有人时有效；无人/不可达为空串；可 NULL）
 * @return 2=有人员且命中白名单（已授权）；1=有人员未命中白名单（陌生人）；
 *         0=无人员；-1=服务不可达/异常
 */
int cloud_detect_query(const uint16_t *rgb565, int w, int h,
                       char *type, int type_n);

#ifdef __cplusplus
}
#endif

#endif /* CLOUD_DETECT_H */
