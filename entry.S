#include <asm/asm.h>
#include <asm/regdef.h>

.globl kernel_entry
kernel_entry:
// SYM_CODE_START(kernel_entry)            # kernel entry point
    /* Config direct window and set PG */
    // li.d        t0, 0x80000011   # UC, PLV0, 0x8000 xxxx xxxx xxxx
    li.d t0 , 0x9000000000000011
    csrwr       t0, 0x180     //#LOONGARCH_CSR_DMWIN0
    // li.d        t0, CSR_DMW1_INIT   # CA, PLV0, 0x9000 xxxx xxxx xxxx
    // csrwr       t0, LOONGARCH_CSR_DMWIN1

    /* We might not get launched at the address the kernel is linked to,
     * so we jump there. We must setup direct access window before this.
     */
//     la.abs      t0, 0f  //把标号0的地方的代码地址装入寄存器t0
//     jirl        zero, t0, 0 //跳转到t0，此时t0是链接是用的高位没有抹去0的虚拟地址，而前面设置的窗口保证了它能访问到bios将其装入的那段物理内存
// 0:
//     la      t0, __bss_start     # clear .bss
//     st.d        zero, t0, 0
//     la      t1, __bss_stop - 

//     la.abs       t0, 0f
//  jirl        zero, t0, 0
// 0:
//  la      t0, __bss_start     # clear .bss
//  st.d        zero, t0, 0
//  la      t1, __bss_stop - 4//LONGSIZE
// 1:
//  addi.d      t0, t0, 4//LONGSIZE
//  st.d        zero, t0, 0
//  bne     t0, t1, 1b

    /* Enable PG */
    li.w t0, 0xb0       # PLV=0, IE=0, PG=1
    csrwr t0, 0x0  # LOONGARCH_CSR_CRMD
    li.w t0, 0x04       # PLV=0, PIE=1, PWE=0
    csrwr t0, 0x1  # LOONGARCH_CSR_PRMD
    li.w t0, 0x00       # FPE=0, SXE=0, ASXE=0, BTE=0
    csrwr t0, 0x2  # LOONGARCH_CSR_EUEN

    la t0,0f
    jirl zero,t0,0
0:
    bl main
    

// SYM_CODE_END(main)