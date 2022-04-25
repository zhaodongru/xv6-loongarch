# Initial process execs /init.

#include "syscall.h"
#include "traps.h"

.globl start
start:
  ori $r12, $r12, 0x1f

