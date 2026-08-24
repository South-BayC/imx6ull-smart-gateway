/**
 * mqtt_hub.h — 云端事件上报（轻量 MQTT 3.1.1 QoS0 客户端）
 *
 * 设计:
 *   - 无外部依赖：TCP socket 手工构造 MQTT CONNECT/PUBLISH/DISCONNECT 报文
 *   - fire-and-forget：独立线程短连接发送（connect→publish→disconnect），
 *     UI 线程不阻塞；发送失败静默（断网容错，符合设计"断网本地存图，联网补传"）
 *   - 未配置 broker（MQTT_HUB_BROKER 为空）时整个模块禁用
 *
 * 配置（编译期）:
 *   MQTT_HUB_BROKER   broker IP/域名（空字符串 = 禁用）
 *   MQTT_HUB_PORT     端口（默认 1883）
 *   MQTT_HUB_CLIENT   client_id
 */
#ifndef MQTT_HUB_H
#define MQTT_HUB_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 发布事件（异步：内部起线程发送，立即返回）
 * @param type   触发类型（"SENSOR"/"CAM"）
 * @param zone   区域名（"前门" 等，UTF-8）
 * @param level  级别（"high"/"medium"/"low"）
 * @param time   时间 "HH:MM"
 */
void mqtt_hub_publish_event(const char *type, const char *zone,
                            const char *level, const char *time);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_HUB_H */
