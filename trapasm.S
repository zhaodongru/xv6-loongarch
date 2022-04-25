#include "param.h"
#include "traps.h"
#include "regs.h"
#include "mmu.h"


.globl tlbrefill
tlbrefill:
  ori $r12, $r12, 0x1f

.globl tlbrefill_end
tlbrefill_end:


.globl gentraps
gentraps:
  ori $r12, $r12, 0x1f

.globl gentraps_end
gentraps_end:

.globl go_trap
go_trap:
  ori $r12, $r12, 0x1f

.globl trapret
trapret:
  ori $r12, $r12, 0x1f
