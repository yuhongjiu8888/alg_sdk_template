#ifndef _VOS_USER_PLATFORM_H_
#define _VOS_USER_PLATFORM_H_

#define _ALIGNED(x) __attribute__((aligned(x)))
#define _PACKED_BEGIN
#define _PACKED_END __attribute__ ((packed))
#define _INLINE static inline
#define _ASM_NOP __asm__("nop");
#define _SECTION(sec)

//BSP identifier
//Note:
//1. bsp_def.h is generated automatically by vos (user-space) Makefile.
//2. if bsp_def.h not found, please make vos first.
//3. Makefile keyword: BSP_DEF_NAME or BSP_DEF_PATH
#include <kwrap/bsp_def.h>

//todo identifier
#define _TODO           0

#endif //_VOS_USER_PLATFORM_H_
