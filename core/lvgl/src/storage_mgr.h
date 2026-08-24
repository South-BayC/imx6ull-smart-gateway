/**
 * storage_mgr.h — 抓拍持久化存储管理（TF 卡 BMP）
 *
 * 职责（设计文档 4 组件 storage-mgr）:
 *   - TF 卡挂载点探测与容量查询
 *   - 抓拍 BMP 写入（含满卡自动清理最旧文件）
 *   - 启动时扫描 TF 恢复抓拍列表（重启后相册回填）
 *
 * 文件名协议: snap_z<zone>_<cmp>_<HHMMSS>.bmp
 *   zone: 0=前门 1=后门 2=仓库 3=窗户（手动抓拍=当前通道 idx）
 *   cmp:  hi / md / lo / none（预警级别；手动为 none）
 *
 * 无 LVGL 依赖（纯 POSIX 文件 IO），可独立测试。
 */
#ifndef STORAGE_MGR_H
#define STORAGE_MGR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_SNAP_MAX   6      /* 相册容量（与 UI 一致） */
#define STORAGE_KEEP_MB    50     /* TF 剩余低于此值时清理最旧抓拍 */

/**
 * 探测 TF 卡挂载点（第一个可写挂载点）
 * @return 挂载点路径；无 TF 返回 NULL
 */
const char *storage_tf_mount(void);

/**
 * TF 容量
 * @param total_mb 输出总容量 MB（可 NULL）；无 TF 输出 -1
 * @param avail_mb 输出剩余 MB（可 NULL）
 */
void storage_tf_capacity(int *total_mb, int *avail_mb);

/**
 * 保存抓拍 BMP（含满卡自动清理最旧文件后重试一次）
 * @param pix      RGB565 像素（底行起由本函数处理 BMP 行序）
 * @param w,h      尺寸
 * @param zone_idx 分区索引 0-3（文件名编码）
 * @param level    级别串 "hi"/"md"/"lo"/"none"
 * @param time_str "HH:MM"（文件名用，冒号自动转 '-'）
 * @return 0 成功；-1 失败（无 TF/写错误/卡满）
 */
int storage_snap_save(const uint16_t *pix, int w, int h,
                      int zone_idx, const char *level, const char *time_str);

/**
 * 扫描 TF 上的抓拍文件（按修改时间降序 = 最新在前）
 * @param out_paths 输出路径数组
 * @param max       数组容量
 * @return 实际条数（0=无 TF 或无文件）
 */
int storage_snap_list(char out_paths[][160], int max);

/**
 * 读取抓拍 BMP → RGB565
 * @param path BMP 路径
 * @param pix  输出像素（须 ≥ w*h×2）
 * @param w,h  期望尺寸（不匹配报错）
 * @return 0 成功
 */
int storage_bmp_read(const char *path, uint16_t *pix, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_MGR_H */
