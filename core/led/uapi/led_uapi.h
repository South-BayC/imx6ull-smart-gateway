#ifndef _UAPI_LED_H
#define _UAPI_LED_H

#include <linux/ioctl.h>

#define LED_IOC_MAGIC 'L'
#define LED_IOC_ON    _IO(LED_IOC_MAGIC, 0x01)
#define LED_IOC_OFF   _IO(LED_IOC_MAGIC, 0x02)
#define LED_IOC_GET   _IOR(LED_IOC_MAGIC, 0x03, int)

#endif /* _UAPI_LED_H */
