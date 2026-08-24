#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

typedef enum { ZONE_ONLINE, ZONE_ARMED, ZONE_ALARM, ZONE_OFFLINE } zone_state_t;
void sm_init(void);
void arm_all(void);
void disarm_all(void);
void ack_alarm(int id);

#endif
