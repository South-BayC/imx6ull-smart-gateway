/**
 * mqtt_hub.c — 轻量 MQTT 3.1.1 QoS0 发布客户端（零依赖）
 *
 * 报文构造（MQTT 3.1.1 spec）:
 *   CONNECT:  0x10 | 剩余长度 | 0x00 0x04 'M' 'Q' 'T' 'T' | 0x04 | flags=0x02
 *             | keepalive(2B) | client_id
 *   PUBLISH:  0x30 | 剩余长度 | topic(2B len + data) | payload
 *   DISCONNECT: 0xE0 0x00
 *
 * 发送策略: 每次发布起独立线程 → socket(500ms 超时) → CONNECT → PUBLISH → DISCONNECT
 *           失败静默（设计 7：网络断 → 本地存图，联网补传）
 */
#include "mqtt_hub.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ---- 编译期配置（板端环境定好后改这里） ---- */
#define MQTT_HUB_ENABLED 1                 /* 置 0 禁用整个模块 */
#define MQTT_HUB_BROKER  "192.168.3.100"   /* broker 地址 */
#define MQTT_HUB_PORT    1883
#define MQTT_HUB_CLIENT  "imx6ull-gateway"
#define MQTT_IO_TIMEOUT_MS 800

/* ---- 剩余长度变长编码（MQTT 规范） ---- */
static int mqtt_encode_len(unsigned char *buf, int len)
{
    int n = 0;
    do {
        unsigned char d = len % 128;
        len /= 128;
        if (len > 0) d |= 0x80;
        buf[n++] = d;
    } while (len > 0);
    return n;
}

/* UTF-8 字符串写入（2B 大端长度 + 数据），返回写入总字节数 */
static int mqtt_write_str(unsigned char *buf, const char *s)
{
    int len = strlen(s);
    buf[0] = (unsigned char)(len >> 8);
    buf[1] = (unsigned char)(len & 0xFF);
    memcpy(buf + 2, s, len);
    return len + 2;
}

/* 带超时的 TCP 连接 */
static int mqtt_tcp_connect(const char *broker, int port)
{
    char ip[64];
    strncpy(ip, broker, sizeof(ip) - 1);
    ip[sizeof(ip) - 1] = 0;

    struct in_addr addr;
    if (inet_aton(ip, &addr) == 0) {
        /* 域名解析 */
        struct hostent *he = gethostbyname(ip);
        if (!he) return -1;
        addr.s_addr = *(unsigned long *)he->h_addr_list[0];
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr = addr;

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }

    /* 收发超时 */
    struct timeval tv = { MQTT_IO_TIMEOUT_MS / 1000, (MQTT_IO_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

/* 一次性发布（短连接） */
static void mqtt_do_publish(const char *topic, const char *payload)
{
    unsigned char pkt[512];
    int n = 0;

    /* --- CONNECT --- */
    {
        unsigned char vb[128];
        int vn = 0;
        vn += mqtt_write_str(vb + vn, "MQTT");
        vb[vn++] = 0x04;                       /* protocol level 4 (3.1.1) */
        vb[vn++] = 0x02;                       /* clean session */
        vb[vn++] = 0; vb[vn++] = 60;           /* keepalive 60s */
        vn += mqtt_write_str(vb + vn, MQTT_HUB_CLIENT);

        unsigned char rl[4];
        int rln = mqtt_encode_len(rl, vn);

        pkt[n++] = 0x10;                       /* CONNECT */
        memcpy(pkt + n, rl, rln); n += rln;
        memcpy(pkt + n, vb, vn); n += vn;
    }

    int fd = mqtt_tcp_connect(MQTT_HUB_BROKER, MQTT_HUB_PORT);
    if (fd < 0) return;                        /* 静默（断网容错） */
    if (send(fd, pkt, n, 0) != n) { close(fd); return; }

    /* CONNACK（2 字节）——读取并简单校验 */
    unsigned char connack[4];
    if (recv(fd, connack, 4, 0) < 4) { close(fd); return; }
    if (connack[0] != 0x20 || connack[3] != 0x00) { close(fd); return; }  /* 拒绝 */

    /* --- PUBLISH（QoS0）--- */
    {
        unsigned char vb[400];
        int vn = mqtt_write_str(vb, topic);
        memcpy(vb + vn, payload, strlen(payload));
        vn += strlen(payload);

        unsigned char rl[4];
        int rln = mqtt_encode_len(rl, vn);

        n = 0;
        pkt[n++] = 0x30;                       /* PUBLISH QoS0 */
        memcpy(pkt + n, rl, rln); n += rln;
        memcpy(pkt + n, vb, vn); n += vn;
        if (send(fd, pkt, n, 0) != n) { close(fd); return; }
    }

    /* --- DISCONNECT --- */
    {
        unsigned char disc[2] = { 0xE0, 0x00 };
        send(fd, disc, 2, 0);
    }

    close(fd);
}

/* ---- 异步发送线程 ---- */
struct mqtt_msg {
    char topic[64];
    char payload[320];
};

static void *mqtt_send_thread(void *arg)
{
    struct mqtt_msg *m = (struct mqtt_msg *)arg;
    mqtt_do_publish(m->topic, m->payload);
    free(m);
    return NULL;
}

void mqtt_hub_publish_event(const char *type, const char *zone,
                            const char *level, const char *time)
{
#if MQTT_HUB_ENABLED
    struct mqtt_msg *m = (struct mqtt_msg *)malloc(sizeof(*m));
    if (!m) return;

    snprintf(m->topic, sizeof(m->topic), "gateway/event");
    snprintf(m->payload, sizeof(m->payload),
             "{\"type\":\"%s\",\"zone\":\"%s\",\"level\":\"%s\",\"time\":\"%s\"}",
             type ? type : "SENSOR", zone ? zone : "", level ? level : "",
             time ? time : "");

    pthread_t tid;
    if (pthread_create(&tid, NULL, mqtt_send_thread, m) == 0)
        pthread_detach(tid);
    else
        free(m);
#else
    (void)type; (void)zone; (void)level; (void)time;
#endif
}
