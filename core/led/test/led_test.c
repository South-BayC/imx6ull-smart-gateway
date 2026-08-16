#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>
#include "led_uapi.h"

int main(int argc, char *argv[])
{
    int fd = open("/dev/led", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    if (argc > 1 && strcmp(argv[1], "on") == 0)
        ioctl(fd, LED_IOC_ON);
    else if (argc > 1 && strcmp(argv[1], "off") == 0)
        ioctl(fd, LED_IOC_OFF);
    int st; ioctl(fd, LED_IOC_GET, &st);
    printf("led state: %d\n", st);
    close(fd);
    return 0;
}
