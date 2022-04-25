#include "param.h"
#include "regs.h"

# By convention, the _start symbol specifies the ELF entry point.
.globl _start
_start:


.comm stack, KSTACKSIZE
