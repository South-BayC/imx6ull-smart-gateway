/**
 * cloud_detect.c — 云端精判客户端实现
 *
 * 传输: 原始 TCP + HTTP/1.1 POST（无第三方依赖，对齐 mqtt_hub 风格：
 *       编译期 #define 服务地址 + I/O 超时 + 静默失败，调用方 fail-safe）。
 * 超时: 非阻塞 connect + select（2s，死服务快速失败不挂工作线程）；
 *       收发 SO_SNDTIMEO/SO_RCVTIMEO 2s 兜底。
 * 协议: POST /detect?w=&h=  body=RGB565 原始帧（428KB，百兆网 ~50ms）
 *       响应 200: {"person":bool,"known":bool,...}（云端两级判定：YOLOv8 人员
 *       检测 + 白名单比对，server.py 固定紧凑格式，strstr 判定足够，无需 JSON 库）。
 */
#include "cloud_detect.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>

#define CLOUD_DETECT_SERVER          "192.168.3.26:8000"
#define CLOUD_DETECT_HTTP_TIMEOUT_MS 2500
#define CLOUD_DETECT_RESP_MAX        4096

static char s_server[64] = CLOUD_DETECT_SERVER;

void cloud_detect_set_server(const char *ipport)
{
    if (!ipport || !ipport[0]) return;
    strncpy(s_server, ipport, sizeof(s_server) - 1);
    s_server[sizeof(s_server) - 1] = 0;
}

/* 非阻塞 connect + select 超时（死服务 ~2s 快速失败，不挂工作线程） */
static int cloud_tcp_connect(const char *ip, int port, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = inet_addr(ip);
    if (a.sin_addr.s_addr == INADDR_NONE) { close(fd); return -1; }

    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int r = connect(fd, (struct sockaddr *)&a, sizeof(a));
    if (r < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (r < 0) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        if (select(fd + 1, NULL, &wset, NULL, &tv) <= 0) { close(fd); return -1; }
        int err = 0;
        socklen_t el = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err != 0) {
            close(fd);
            return -1;
        }
    }
    /* 恢复阻塞 + 收发超时兜底 */
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int cloud_detect_query(const uint16_t *rgb565, int w, int h,
                       char *type, int type_n)
{
    if (type && type_n > 0) type[0] = 0;
    /* 拆 "IP:端口" */
    char ip[64];
    int port = 8000;
    strncpy(ip, s_server, sizeof(ip) - 1);
    ip[sizeof(ip) - 1] = 0;
    char *colon = strchr(ip, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }

    int fd = cloud_tcp_connect(ip, port, CLOUD_DETECT_HTTP_TIMEOUT_MS);
    if (fd < 0) return -1;

    /* HTTP 头 + RGB565 帧（428KB，百兆网 ~50ms） */
    size_t body_len = (size_t)w * h * 2;
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
                      "POST /detect?w=%d&h=%d HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "Content-Type: application/octet-stream\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n\r\n",
                      w, h, s_server, body_len);

    int fail = 0;
    if (send_all(fd, hdr, (size_t)hn) < 0 ||
        send_all(fd, rgb565, body_len) < 0)
        fail = 1;

    /* 收响应（Connection: close，读至对端关闭；2s 收超时兜底） */
    char resp[CLOUD_DETECT_RESP_MAX];
    size_t got = 0;
    while (!fail && got < sizeof(resp) - 1) {
        ssize_t n = recv(fd, resp + got, sizeof(resp) - 1 - got, 0);
        if (n <= 0) break;
        got += (size_t)n;
    }
    resp[got] = 0;
    close(fd);
    if (fail) return -1;

    /* 结论判定（服务端固定格式，strstr 足够）：
     * 200 且 "person":true → 有人；再查 "known":true → 白名单命中（已授权）
     * "person":false → 无人；其他 → 异常。有人时顺带取入侵类型键 */
    if (strstr(resp, " 200 ") == NULL) return -1;
    if (strstr(resp, "\"person\":true")) {
        if (type && type_n > 0) {
            if (strstr(resp, "\"type\":\"person\""))      strncpy(type, "person", type_n - 1);
            else if (strstr(resp, "\"type\":\"animal\"")) strncpy(type, "animal", type_n - 1);
            else if (strstr(resp, "\"type\":\"object\"")) strncpy(type, "object", type_n - 1);
            type[type_n - 1] = 0;
        }
        if (strstr(resp, "\"known\":true"))
            return 2;                     /* 白名单命中（已授权） */
        return 1;                         /* 陌生人 */
    }
    if (strstr(resp, "\"person\":false")) return 0;
    return -1;
}

int cloud_detect_ready(void)
{
    /* 拆分 "IP:端口"（与 query 一致） */
    char ip[64];
    int port = 8000;
    strncpy(ip, s_server, sizeof(ip) - 1);
    ip[sizeof(ip) - 1] = 0;
    char *colon = strchr(ip, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }

    /* 轻量 TCP 探测：仅确认服务可达，不收发帧。
     * 非阻塞 connect + select 超时（死服务 ~2s 快速失败，不挂主线程）。 */
    int fd = cloud_tcp_connect(ip, port, CLOUD_DETECT_HTTP_TIMEOUT_MS);
    if (fd < 0) return -1;
    close(fd);
    return 0;
}
