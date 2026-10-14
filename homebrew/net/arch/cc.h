/* arch/cc.h: compiler/platform glue for lwIP on the NW-A829 (glibc 2.3.3, ARM EABI, little endian). */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define LWIP_TIMEVAL_PRIVATE 0
#include <sys/time.h>
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif
#define LWIP_NO_INTTYPES_H 1
#define U16_F "hu"
#define S16_F "hd"
#define X16_F "hx"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "u"
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x
#define LWIP_PROVIDE_ERRNO 0
#endif
