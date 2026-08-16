#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include "key_event_uapi.h"

static void print_ev(const struct key_event *ev)
{
    printf("t=%llu key=%u pressed=%u\n",
           (unsigned long long)ev->timestamp_ns, ev->key, ev->pressed);
}

int main(int argc, char *argv[])
{
    int fd;
    int mode = 0;   /* 0=阻塞 1=nonblock 2=poll */

    /* 用法：key_test [--nonblock|--poll|--clear] */
    if (argc > 1) {
        if (strcmp(argv[1], "--nonblock") == 0)
            mode = 1;
        else if (strcmp(argv[1], "--poll") == 0)
            mode = 2;
        else if (strcmp(argv[1], "--clear") == 0) {
            fd = open("/dev/key-event", O_RDWR);
            if (fd < 0) { perror("open"); return 1; }
            ioctl(fd, KEY_EVENT_CLEAR);
            close(fd);
            printf("event cleared\n");
            return 0;
        }
    }

    fd = open("/dev/key-event", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    if (mode == 1) {
        /* 非阻塞模式：循环读，-EAGAIN 时打印计数 */
        int flags = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        printf("nonblock mode: press KEY0 (Ctrl-C to exit)\n");
        for (;;) {
            struct key_event ev;
            ssize_t n = read(fd, &ev, sizeof(ev));
            if (n == (ssize_t)sizeof(ev))
                print_ev(&ev);
            else if (n < 0 && errno == EAGAIN)
                printf("no event (EAGAIN)\n");
            usleep(100 * 1000);   /* 100ms 轮询 */
        }
    } else if (mode == 2) {
        /* poll 模式：select/poll 等待可读 */
        printf("poll mode: press KEY0 (Ctrl-C to exit)\n");
        for (;;) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            int r = poll(&pfd, 1, 3000);
            if (r > 0 && (pfd.revents & POLLIN)) {
                struct key_event ev;
                if (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
                    print_ev(&ev);
            } else if (r == 0) {
                printf("poll timeout 3s, no event\n");
            } else {
                perror("poll");
                break;
            }
        }
    } else {
        /* 阻塞模式：阻塞等待，每次打印一次事件 */
        printf("block mode: press KEY0 (Ctrl-C to exit)\n");
        for (;;) {
            struct key_event ev;
            if (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
                print_ev(&ev);
        }
    }

    close(fd);
    return 0;
}