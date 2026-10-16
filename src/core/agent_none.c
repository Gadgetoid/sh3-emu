/* Provide unavailable-operation stubs when the debugger agent socket is not supported. */
#include "core/agent.h"

#include <stddef.h>

agent_t *agent_create(const char *socket_path, agent_log_fn log) {
    (void)socket_path; (void)log; return NULL;
}
void     agent_destroy(agent_t *agent) {
    (void)agent;
}
void     agent_poll(agent_t *agent, mailbox_t *mailbox) {
    (void)agent; (void)mailbox;
}
