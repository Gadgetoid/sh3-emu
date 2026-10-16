/* Provide unavailable-operation stubs when the GDB server is not supported. */
#include "core/gdb.h"

#include <stddef.h>

gdb_t *gdb_create(machine_t *machine, int port, bool network, gdb_log_fn log) {
    (void)machine; (void)port; (void)network; (void)log; return NULL;
}
void   gdb_destroy(gdb_t *gdb) {
    (void)gdb;
}
bool   gdb_wait_for_client(gdb_t *gdb) {
    (void)gdb; return false;
}
bool   gdb_connected(const gdb_t *gdb) {
    (void)gdb; return false;
}
bool   gdb_set_process(gdb_t *gdb, const char *name) {
    (void)gdb; (void)name; return false;
}
void   gdb_debug_line(gdb_t *gdb, const char *line) {
    (void)gdb; (void)line;
}
bool   gdb_run(gdb_t *gdb, uint64_t cycles) {
    (void)gdb; (void)cycles; return false;
}
void   gdb_service(gdb_t *gdb) {
    (void)gdb;
}
bool   gdb_halted(const gdb_t *gdb) {
    (void)gdb; return false;
}
void   gdb_after_run(gdb_t *gdb) {
    (void)gdb;
}
void   gdb_set_machine(gdb_t *gdb, machine_t *machine) {
    (void)gdb; (void)machine;
}
