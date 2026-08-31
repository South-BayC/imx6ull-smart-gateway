/**
 * detector.h — 入侵精判框架（统一两级管线：本地快响应 + 云端复核 + 离线兜底）
 *
 * 架构（08-30 定案，用户四决策；多线程模型）:
 *   cam_feed 帧差粗判命中（间隔/差值可设）→ dev_bridge 冷却节流
 *     → detector_submit(zone)：抓最近一帧缓存为精判数据图 + 信号量唤醒工作线程
 *     → 线程 4（低优先级 nice+10）串行两段（同一帧）：
 *         段1 本地 SCRFD 人脸检测 → 发初步结论(stage=INITIAL)
 *             检出人脸 → FACE_UNKNOWN / 未检出 → NO_FACE
 *         等初步结论被 dev_bridge 消费（防覆盖竞态）
 *         段2 云端复核（设置→云端复核 开关开时）→ cloud_detect_query 帧上传
 *             （YOLOv8 人员/类型 + 白名单比对）→ 发最终结论(stage=FINAL)
 *             2→FACE_KNOWN / 1→FACE_UNKNOWN / 0→OTHER / -1→NONE(不可达)
 *         开关关闭：只跑段1，初步结论即最终结论
 *   dev_bridge 200ms 周期 detector_poll_result() 消费结论并分发告警/时间轴：
 *     开关关（纯本地）: FACE_UNKNOWN→STRANGER 告警 / NO_FACE→INTRUDER 告警
 *     开关开（两级）  : INITIAL: FACE_UNKNOWN→立即 STRANGER 告警；
 *                       NO_FACE→轻提醒（画面变动，等云端定论）
 *                       FINAL: FACE_KNOWN→白名单命中，在告警则自动消警；
 *                       FACE_UNKNOWN→不在告警则升级 STRANGER；
 *                       OTHER(无人)→在告警则自动消警（本地误报）；
 *                       NONE(不可达)→初步=NO_FACE 则升级 INTRUDER（不漏报），
 *                       已告警则维持本地结论
 *
 * 线程模型: submit 由 dev_bridge 周期线程调用；worker 为独立低优先级线程
 *           （显示/采集优先抢 CPU，推理用碎片算力）；忙时丢帧（冷却节流兜底）；
 *           poll 由 dev_bridge 周期线程调用——显示线程从不等待推理。
 *
 * 帧约定: RGB565 630×340（cam_feed_copy_frame 原样拷贝最近一帧）
 */
#ifndef DETECTOR_H
#define DETECTOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 精判结论（告警语义 08-29/08-30 用户定案） */
enum {
    DET_RESULT_NONE = 0,      /* 无待消费结论；FINAL+NONE=云端不可达（无云端结论） */
    DET_RESULT_NO_FACE,       /* 有运动但未检出人脸 */
    DET_RESULT_FACE_UNKNOWN,  /* 检出人脸且非白名单/云端判定陌生人 */
    DET_RESULT_FACE_KNOWN,    /* 云端白名单命中（已授权）/ 本地比对预留 */
    DET_RESULT_OTHER,         /* 云端判定无人 */
};

/* 结论阶段（两级管线：段1 初步 / 段2 最终；开关关时仅有 INITIAL 一条） */
enum {
    DET_STAGE_INITIAL = 0,    /* 初步结论（本地 SCRFD） */
    DET_STAGE_FINAL = 1,      /* 最终结论（云端复核后；不可达也发 FINAL+NONE 定案） */
};

/* 人形/人脸推理引擎接口（P2 NCNN 接入点；v1 桩不注册即走内部桩）
 * @param rgb565 精判数据图（630×340 RGB565）
 * @param name   命中的人脸名字输出（FACE_KNOWN 时有效，可为 NULL）
 * @return DET_RESULT_*（NO_FACE/FACE_UNKNOWN/FACE_KNOWN） */
typedef int (*detector_verifier_fn)(const uint8_t *rgb565, int w, int h,
                                    char *name, int name_n);

/* 检出的人脸框（画布 630×340 坐标系，随帧显示） */
typedef struct {
    float x, y, w, h;   /* 框（画布坐标） */
    float prob;         /* 置信度 0~1 */
} detector_box_t;

/**
 * 取最近一次精判检出的人脸框（读后不清，3 秒保持窗后自动失效）
 * @param out 输出数组（调用方提供）
 * @param max 数组容量（建议 ≥8）
 * @return 框数量（0=无/已过期）
 */
int detector_get_boxes(detector_box_t *out, int max);

void detector_init(void);
void detector_set_verifier(detector_verifier_fn fn);

/**
 * 粗判命中提交（统一两级管线唯一入口；dev_bridge 冷却节流后调用）
 * @param zone 命中归属分区（0-3）
 * @return 1=已入队精判；0=忙丢弃/无帧（冷却节流兜底，不影响功能）
 * 行为: 抓取最近一帧缓存为精判数据图，信号量唤醒工作线程后立即返回
 */
int detector_submit(int zone);

/**
 * dev_bridge 周期轮询：取一条待消费结论（读后清空）
 * @param zone  结论归属分区
 * @param result DET_RESULT_*
 * @param stage 结论阶段（DET_STAGE_INITIAL/FINAL，可 NULL）
 * @param name  附注：FINAL=云端入侵类型词（"人员/动物/物体"，无人/不可达为空）；
 *              FACE_KNOWN 预留人名；可为 NULL
 * @return 1=有结论；0=无
 */
int detector_poll_result(int *zone, int *result, int *stage, char *name, int name_n);

/* 检测面板信息："[精判:桩 命中:n]" */
int detector_get_info(char *buf, int n);

#ifdef __cplusplus
}
#endif
#endif /* DETECTOR_H */
