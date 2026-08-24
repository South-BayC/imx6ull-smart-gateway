/**
 * storage_mgr.c — 抓拍持久化存储管理实现（TF 卡 BMP）
 *
 * 文件名协议: snap_z<zone>_<cmp>_<HHMMSS>.bmp
 *   zone: 0-3（分区索引）；cmp: hi/md/lo/none
 *   例: snap_z0_hi_143205.bmp = 前门 · 高级别 · 14:32:05
 *
 * BMP 格式: 24bit 底行起（标准 BMP 行序自底向上，行尾 4 字节对齐）
 * 满卡策略: 剩余 < STORAGE_KEEP_MB 时按 mtime 删最旧 snap_*.bmp 后重试一次
 */
#include "storage_mgr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static const char *tf_mnts[] = {
    "/mnt/mmc", "/media/mmcblk1p1", "/media/mmcblk0p1",
    "/mnt/sd", "/media/sdcard", "/mnt/tf", NULL,
};

const char *storage_tf_mount(void)
{
    for (int i = 0; tf_mnts[i]; i++)
        if (access(tf_mnts[i], W_OK) == 0)
            return tf_mnts[i];
    return NULL;
}

void storage_tf_capacity(int *total_mb, int *avail_mb)
{
    const char *mnt = storage_tf_mount();
    if (!mnt) {
        if (total_mb) *total_mb = -1;
        if (avail_mb) *avail_mb = -1;
        return;
    }

    char path[128];
    snprintf(path, sizeof(path), "%s/", mnt);
    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0) {
        if (total_mb) *total_mb = -1;
        if (avail_mb) *avail_mb = -1;
        return;
    }
    if (total_mb)
        *total_mb = (int)(vfs.f_blocks * (vfs.f_frsize >> 20));
    if (avail_mb)
        *avail_mb = (int)(vfs.f_bavail * (vfs.f_frsize >> 20));
}

/* 满卡清理：按 mtime 删除最旧 snap_*.bmp，直到剩余 ≥ KEEP_MB 或删完 */
static void tf_cleanup_old(void)
{
    const char *mnt = storage_tf_mount();
    if (!mnt) return;

    int total_mb = 0, avail_mb = 0;
    storage_tf_capacity(&total_mb, &avail_mb);
    if (avail_mb < 0 || avail_mb >= STORAGE_KEEP_MB) return;

    /* 收集 snap_*.bmp 路径与 mtime */
    char paths[64][160];
    time_t mtimes[64];
    int n = 0;
    DIR *dir = opendir(mnt);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < 64) {
        if (strncmp(ent->d_name, "snap_", 5) != 0) continue;
        char *dot = strstr(ent->d_name, ".bmp");
        if (!dot || strcmp(dot, ".bmp") != 0) continue;
        snprintf(paths[n], sizeof(paths[n]), "%s/%s", mnt, ent->d_name);
        struct stat st;
        if (stat(paths[n], &st) == 0) mtimes[n] = st.st_mtime;
        else mtimes[n] = 0;
        n++;
    }
    closedir(dir);

    /* 选择排序逐个删最旧，直到空间达标 */
    while (avail_mb < STORAGE_KEEP_MB) {
        int oldest = -1;
        time_t oldest_t = (time_t)-1;
        for (int i = 0; i < n; i++) {
            if (paths[i][0] && (oldest < 0 || mtimes[i] < oldest_t)) {
                oldest = i;
                oldest_t = mtimes[i];
            }
        }
        if (oldest < 0) break;   /* 删完 */
        remove(paths[oldest]);
        paths[oldest][0] = 0;    /* 标记已删 */
        storage_tf_capacity(&total_mb, &avail_mb);
        if (avail_mb < 0) break;
    }
}

int storage_snap_save(const uint16_t *pix, int w, int h,
                      int zone_idx, const char *cmp, const char *time_str)
{
    const char *mnt = storage_tf_mount();
    if (!mnt) return -1;

    /* 满卡预清理 */
    tf_cleanup_old();

    int row_bytes = w * 3;
    int pad = (4 - (row_bytes % 4)) % 4;
    int stride = row_bytes + pad;
    unsigned int data_size = (unsigned int)(stride * h);
    unsigned int file_size = 54 + data_size;

    char fname[64];
    snprintf(fname, sizeof(fname), "snap_z%d_%s_%s.bmp",
             zone_idx, cmp ? cmp : "none", time_str);
    /* 时间冒号 → '-'（FAT 非法字符） */
    for (char *p = fname; *p; p++)
        if (*p == ':') *p = '-';

    char path[224];
    snprintf(path, sizeof(path), "%s/%s", mnt, fname);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        perror("[STORAGE] bmp open");
        return -1;
    }

    unsigned char hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &file_size, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    memcpy(hdr + 34, &data_size, 4);
    fwrite(hdr, 1, 54, fp);

    unsigned char zero[4] = {0, 0, 0, 0};
    for (int y = h - 1; y >= 0; y--) {
        const uint16_t *line = pix + y * w;
        for (int x = 0; x < w; x++) {
            unsigned char bgr[3];
            uint16_t px = line[x];
            bgr[0] = (unsigned char)((px & 0x1F) << 3);
            bgr[1] = (unsigned char)(((px >> 5) & 0x3F) << 2);
            bgr[2] = (unsigned char)((px >> 11) << 3);
            fwrite(bgr, 1, 3, fp);
        }
        if (pad) fwrite(zero, 1, pad, fp);
    }

    fclose(fp);
    printf("[STORAGE] saved %s (%uKB)\n", path, file_size / 1024);
    return 0;
}

int storage_snap_list(char out_paths[][160], int max)
{
    const char *mnt = storage_tf_mount();
    if (!mnt) return 0;

    char paths[64][160];
    time_t mtimes[64];
    int n = 0;

    DIR *dir = opendir(mnt);
    if (!dir) return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < 64) {
        if (strncmp(ent->d_name, "snap_", 5) != 0) continue;
        char *dot = strstr(ent->d_name, ".bmp");
        if (!dot || strcmp(dot, ".bmp") != 0) continue;
        snprintf(paths[n], sizeof(paths[n]), "%s/%s", mnt, ent->d_name);
        struct stat st;
        mtimes[n] = stat(paths[n], &st) == 0 ? st.st_mtime : 0;
        n++;
    }
    closedir(dir);

    /* 按 mtime 降序排序（最新在前） */
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (mtimes[j] > mtimes[i]) {
                time_t tmpt = mtimes[i]; mtimes[i] = mtimes[j]; mtimes[j] = tmpt;
                char tmp[160];
                memcpy(tmp, paths[i], 160); memcpy(paths[i], paths[j], 160); memcpy(paths[j], tmp, 160);
            }

    int cnt = (n < max) ? n : max;
    for (int i = 0; i < cnt; i++)
        memcpy(out_paths[i], paths[i], 160);
    return cnt;
}

int storage_bmp_read(const char *path, uint16_t *pix, int w, int h)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    unsigned char hdr[54];
    if (fread(hdr, 1, 54, fp) != 54) { fclose(fp); return -1; }
    if (hdr[0] != 'B' || hdr[1] != 'M') { fclose(fp); return -1; }

    int bw = 0, bh = 0;
    unsigned short bpp = 0;
    memcpy(&bw, hdr + 18, 4);
    memcpy(&bh, hdr + 22, 4);
    memcpy(&bpp, hdr + 28, 2);
    if (bw != w || bh != h || bpp != 24) { fclose(fp); return -2; }

    int row_bytes = w * 3;
    int pad = (4 - (row_bytes % 4)) % 4;
    int stride = row_bytes + pad;

    /* BMP 底行起 → 逐行读并转 RGB565（存顶行起） */
    unsigned char *rowbuf = (unsigned char *)malloc(stride);
    if (!rowbuf) { fclose(fp); return -1; }

    for (int y = h - 1; y >= 0; y--) {
        if (fread(rowbuf, 1, stride, fp) != (size_t)stride) break;
        uint16_t *dline = pix + y * w;
        for (int x = 0; x < w; x++) {
            unsigned char b = rowbuf[x * 3];
            unsigned char g = rowbuf[x * 3 + 1];
            unsigned char r = rowbuf[x * 3 + 2];
            dline[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
    free(rowbuf);
    fclose(fp);
    return 0;
}
