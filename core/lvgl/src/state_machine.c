#include "state_machine.h"
#include <string.h>
static zone_state_t g_states[4] = {ZONE_ONLINE, ZONE_ONLINE, ZONE_ONLINE, ZONE_ONLINE};
void sm_init(void) {}
void arm_all(void) { for(int i=0;i<4;i++) g_states[i]=ZONE_ARMED; }
void disarm_all(void) { for(int i=0;i<4;i++) g_states[i]=ZONE_ONLINE; }
void ack_alarm(int id) { if(id>=0&&id<4) g_states[id]=ZONE_ARMED; }
