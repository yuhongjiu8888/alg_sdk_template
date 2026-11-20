/**
 * @file kdrv_type.h
 * @brief type definition of KDRV API.
 * @date in the year 2018
 * Copyright Novatek Microelectronics Corp. 2018.  All rights reserved.
 */

#ifndef _KDRV_TYPE_H_
#define _KDRV_TYPE_H_

#include <kwrap/nvt_type.h>

#ifndef TRUE
#define TRUE    1
#endif

#ifndef FALSE
#define FALSE   0
#endif

#ifndef NULL
#define NULL    ((void *)0)
#endif


#define	ENUM_DUMMY4WORD(name)	E_##name	=	0x10000000

/*
 * Alignment utilities
 */
#define ALIGN128_DOWN(x)    (((x) >> 7) << 7)
#define ALIGN64_DOWN(x)     (((x) >> 6) << 6)
#define ALIGN32_DOWN(x)     (((x) >> 5) << 5)
#define ALIGN16_DOWN(x)     (((x) >> 4) << 4)
#define ALIGN8_DOWN(x)      (((x) >> 3) << 3)
#define ALIGN4_DOWN(x)      (((x) >> 2) << 2)
#define ALIGN2_DOWN(x)      (((x) >> 1) << 1)

#define ALIGN4K_UP(x)       ((((x) + 4095) >> 12) << 12)
#define ALIGN128_UP(x)      ((((x) + 127) >> 7) << 7)
#define ALIGN64_UP(x)       ((((x) + 63) >> 6) << 6)
#define ALIGN32_UP(x)       ((((x) + 31) >> 5) << 5)
#define ALIGN16_UP(x)       ((((x) + 15) >> 4) << 4)
#define ALIGN8_UP(x)        ((((x) + 7) >> 3) << 3)
#define ALIGN4_UP(x)        ((((x) + 3) >> 2) << 2)
#define ALIGN2_UP(x)        ((((x) + 1) >> 1) << 1)

#define IS_ALIGN128(x)      ((x) == (((x) >> 7) << 7))
#define IS_ALIGN64(x)       ((x) == (((x) >> 6) << 6))
#define IS_ALIGN32(x)       ((x) == (((x) >> 5) << 5))
#define IS_ALIGN16(x)       ((x) == (((x) >> 4) << 4))
#define IS_ALIGN8(x)        ((x) == (((x) >> 3) << 3))
#define IS_ALIGN4(x)        ((x) == (((x) >> 2) << 2))
#define IS_ALIGN2(x)        ((x) == (((x) >> 1) << 1))

#define ALIGNX_DOWN(level, x) (              \
		(level == 2) ? ALIGN2_DOWN(x) : (    \
		(level == 4) ? ALIGN4_DOWN(x) : (    \
		(level == 8) ? ALIGN8_DOWN(x) : (    \
		(level == 16) ? ALIGN16_DOWN(x) : (  \
		(level == 32) ? ALIGN32_DOWN(x) : (  \
		(level == 64) ? ALIGN64_DOWN(x) : (x) ))))))

#define ALIGNX_UP(level, x) (                \
		(level == 2) ? ALIGN2_UP(x) : (      \
		(level == 4) ? ALIGN4_UP(x) : (      \
		(level == 8) ? ALIGN8_UP(x) : (      \
		(level == 16) ? ALIGN16_UP(x) : (    \
		(level == 32) ? ALIGN32_UP(x) : (    \
		(level == 64) ? ALIGN64_UP(x) : (x)))))))

#define IS_ALIGNX(level, x) (              \
		(level == 2) ? IS_ALIGN2(x) : (    \
		(level == 4) ? IS_ALIGN4(x) : (    \
		(level == 8) ? IS_ALIGN8(x) : (    \
		(level == 16) ? IS_ALIGN16(x) : (  \
		(level == 32) ? IS_ALIGN32(x) : (  \
		(level == 64) ? IS_ALIGN64(x) : 0 ))))))

/*
 *  make high/low 16bits to 32bits
 */
#define MAKE_UINT16_UINT16(h,l) 	(((((UINT32)(h)) & 0x0000ffff)<<16) | (((UINT32)(l)) & 0x0000ffff))

/*
 *  get high/low 32bits from 64bits
 */
#define GET_HI_UINT32(v)			((UINT32)((v) >> 32))
#define GET_LO_UINT32(v)			((UINT32)(v))

/*
 *  get high/low 16bits from 32bits
 */
#define GET_HI_UINT16(v)			((((UINT32)(v)) >> 16) & 0x0000ffff)
#define GET_LO_UINT16(v)			(((UINT32)(v)) & 0x0000ffff)

#define KDRV_CHIP_OFFSET      24
#define KDRV_CHIP_BITMAP      0xFF000000
#define KDRV_ENGINE_OFFSET    8
#define KDRV_ENGINE_BITMAP    0x00FFFF00
#define KDRV_CHANNEL_OFFSET   0
#define KDRV_CHANNEL_BITMAP   0x000000FF

#define KDRV_DEV_ID_CHIP(id)      ((id & KDRV_CHIP_BITMAP) >> KDRV_CHIP_OFFSET)
#define KDRV_DEV_ID_ENGINE(id)    ((id & KDRV_ENGINE_BITMAP) >> KDRV_ENGINE_OFFSET)
#define KDRV_DEV_ID_CHANNEL(id)   ((id & KDRV_CHANNEL_BITMAP) >> KDRV_CHANNEL_OFFSET)
#define KDRV_DEV_ID(chip, engine, channel) \
	(((chip) << KDRV_CHIP_OFFSET) | ((engine) << KDRV_ENGINE_OFFSET) | ((channel) & KDRV_CHANNEL_BITMAP))

typedef enum {
	KDRV_CHIP0 = 0x00,
	KDRV_CHIP1 = 0x01,
	KDRV_CHIP2 = 0x02,
	KDRV_CHIP3 = 0x03,
	KDRV_CHIP4 = 0x04,
} KDRV_DEV_CHIP;

typedef enum {
	KDRV_VDOCAP_ENGINE0  = 0x1000,
	KDRV_VDOCAP_ENGINE1  = 0x1001,
	KDRV_VDOCAP_ENGINE2  = 0x1002,
	KDRV_VDOCAP_ENGINE3  = 0x1003,
	KDRV_VDOCAP_ENGINE4  = 0x1004,
	KDRV_VDOCAP_ENGINE5  = 0x1005,
	KDRV_VDOCAP_ENGINE6  = 0x1006,
	KDRV_VDOCAP_ENGINE7  = 0x1007,
	KDRV_VDOCAP_ENGINE8  = 0x1008,
	KDRV_VDOCAP_ENGINE9  = 0x1009,
	KDRV_VDOCAP_ENGINE10 = 0x100A,
	KDRV_VDOCAP_ENGINE11 = 0x100B,
	KDRV_VDOCAP_ENGINE12 = 0x100C,
	KDRV_VDOCAP_ENGINE13 = 0x100D,
	KDRV_VDOCAP_ENGINE14 = 0x100E,
	KDRV_VDOCAP_ENGINE15 = 0x100F,
	KDRV_VDOCAP_ENGINE16 = 0x1010,
	KDRV_VDOCAP_ENGINE17 = 0x1011,
	KDRV_VDOCAP_ENGINE18 = 0x1012,
	KDRV_VDOCAP_ENGINE19 = 0x1013,
	KDRV_VDOCAP_ENGINE20 = 0x1014,
	KDRV_VDOCAP_ENGINE21 = 0x1015,
	KDRV_VDOCAP_ENGINE22 = 0x1016,
	KDRV_VDOCAP_ENGINE23 = 0x1017,
	KDRV_VDOCAP_ENGINE24 = 0x1018,
	KDRV_VDOCAP_ENGINE25 = 0x1019,
	KDRV_VDOCAP_ENGINE26 = 0x101A,
	KDRV_VDOCAP_ENGINE27 = 0x101B,
	KDRV_VDOCAP_ENGINE28 = 0x101C,
	KDRV_VDOCAP_ENGINE29 = 0x101D,
	KDRV_VDOCAP_ENGINE30 = 0x101E,
	KDRV_VDOCAP_ENGINE31 = 0x101F,

	KDRV_VDOOUT_ENGINE0 = 0x2000,
	KDRV_VDOOUT_ENGINE1 = 0x2001,
	KDRV_VDOOUT_ENGINE2 = 0x2002,
	KDRV_VDOOUT_ENGINE3 = 0x2003,
	KDRV_VDOOUT_ENGINE4 = 0x2004,

	KDRV_VIDEODEC_ENGINE0 = 0x3000,
	KDRV_VIDEODEC_ENGINE_H264 = 0x3100,
	KDRV_VIDEODEC_ENGINE_H265 = 0x3200,
	KDRV_VIDEODEC_ENGINE_JPEG = 0x3300,

	KDRV_VIDEOENC_ENGINE_H264 = 0x4000,
	KDRV_VIDEOENC_ENGINE_H265 = 0x4100,
	KDRV_VIDEOENC_ENGINE_JPEG = 0x4200,

	KDRV_VIDEOPROCS_ENGINE0   = 0x5000,

	KDRV_OSG_ENGINE0    = 0x5100,

	KDRV_AUDCAP_ENGINE0 = 0x6000,
	KDRV_AUDCAP_ENGINE0_LOOPBACK = 0x6010,
	KDRV_AUDCAP_ENGINE1 = 0x6001,
	KDRV_AUDCAP_ENGINE1_LOOPBACK = 0x6011,
	KDRV_AUDCAP_ENGINE2 = 0x6002,
	KDRV_AUDCAP_ENGINE2_LOOPBACK = 0x6012,
	KDRV_AUDCAP_ENGINE3 = 0x6003,
	KDRV_AUDCAP_ENGINE3_LOOPBACK = 0x6013, 
	KDRV_AUDCAP_ENGINEHDMI = 0x6004,

	KDRV_AUDOUT_ENGINE0 = 0x7000,
	KDRV_AUDOUT_ENGINE1 = 0x7001,
	KDRV_AUDOUT_ENGINE2 = 0x7002,
	KDRV_AUDOUT_ENGINE3 = 0x7003,
	KDRV_AUDOUT_ENGINEHDMI = 0x7004,

	
	KDRV_SSENIF_ENGINE_CSI0  = 0x9000,
	KDRV_SSENIF_ENGINE_CSI1  = 0x9001,
	KDRV_SSENIF_ENGINE_CSI2  = 0x9002,
	KDRV_SSENIF_ENGINE_CSI3  = 0x9003,
	KDRV_SSENIF_ENGINE_CSI4  = 0x9004,
	KDRV_SSENIF_ENGINE_CSI5  = 0x9005,
	KDRV_SSENIF_ENGINE_CSI6  = 0x9006,
	KDRV_SSENIF_ENGINE_CSI7  = 0x9007,
	KDRV_RPC_ENGINE0         = 0xA000,

    KDRV_AI_ENGINE_AI    = 0xB000,
    KDRV_AI_ENGINE_CNN   = 0xB001,
    KDRV_AI_ENGINE_NUE   = 0xB002,
    KDRV_AI_ENGINE_NUE2  = 0xB003,
    KDRV_AI_ENGINE_CNN2  = 0xB004,
    KDRV_AI_ENGINE_CNN3  = 0xB005,
} KDRV_DEV_ENGINE;

typedef enum {
	KDRV_VDODEC_TYPE_JPEG = 0,
	KDRV_VDODEC_TYPE_H264,
	KDRV_VDODEC_TYPE_H265,
	ENUM_DUMMY4WORD(KDRV_VDODEC_TYPE)
} KDRV_VDODEC_TYPE;


typedef struct {
	UINT32  x;      ///< X coordinate of the point
	UINT32  y;      ///< Y coordinate of the point
} KDRV_UPOINT;

typedef struct {
	UINT32  w;
	UINT32  h;
} KDRV_USIZE, KDRV_DIM, KDRV_DIM_ALIGN_LEVEL;

typedef struct {
	UINT32  x;
	UINT32  y;
	UINT32  w;
	UINT32  h;
} KDRV_URECT, KDRV_URECT_ALIGN_LEVEL;

typedef struct {
	INT32  x;       ///< X coordinate of the point
	INT32  y;       ///< Y coordinate of the point
} KDRV_IPOINT;

typedef struct {
	INT32  w;       ///< Rectangle width
	INT32  h;       ///< Rectangle height
} KDRV_ISIZE;

typedef struct {
	INT32  x;       ///< X coordinate of the top-left point of the rectangle
	INT32  y;       ///< Y coordinate of the top-left point of the rectangle
	INT32  w;       ///< Rectangle width
	INT32  h;       ///< Rectangle height
} KDRV_IRECT;

typedef enum
{
    KDRV_OK                       = 0,
    KDRV_ERR_NG                   = -1,
    KDRV_NOT_SUPPORTED            = -2,
} KDRV_RESULT; ///< KDRV error code

typedef struct {
	INT32 ddr_id;
	uintptr_t addr_pa; //++ TODO 636
	uintptr_t addr_va; //++ TODO 636
	INT32 size;
} KDRV_BUFFER_INFO;


typedef enum {
	KDRV_BUFTYPE_UNKNOWN,
	KDRV_BUFTYPE_YUV422,       //UYVY
	KDRV_BUFTYPE_YUV422_SCE,   //UYVY+SCE
	KDRV_BUFTYPE_YUV422_MB,    //MB422
	KDRV_BUFTYPE_YUV420_SP,    //420SP, SM420
	KDRV_BUFTYPE_YUV420_SCE,   //420SP+SCE
	KDRV_BUFTYPE_YUV420_MB,    //MB420
	KDRV_BUFTYPE_YUV420_16x2,
	KDRV_BUFTYPE_YUV420_8x4,
	KDRV_BUFTYPE_YUV420_LLC_8x4,
	KDRV_BUFTYPE_YUV420_SP8,
	KDRV_BUFTYPE_ARGB1555,
	KDRV_BUFTYPE_ARGB8888,
	KDRV_BUFTYPE_RGB888,
	KDRV_BUFTYPE_RGB565,
	KDRV_BUFTYPE_ARGB1555_RLE,
	KDRV_BUFTYPE_ARGB8888_RLE,
	KDRV_BUFTYPE_RGB888_RLE,
	KDRV_BUFTYPE_RGB565_RLE,
	KDRV_BUFTYPE_VDDO_RGB2BPP,
	KDRV_BUFTYPE_VDDO_RGB4BPP,
	KDRV_BUFTYPE_VDDO_RGB8BPP,
	KDRV_BUFTYPE_RGB_CV,
} KDRV_PIX_FMT;

typedef struct {
	KDRV_PIX_FMT pix_fmt;
	KDRV_USIZE dim;
} KDRV_VIDEO_FRAME_INFO;


typedef struct {
	INT32 (*callback)(VOID *callback_info, VOID *user_data);
	INT32 (*reserve_buf)(uintptr_t phy_addr, int ddr_id, unsigned int job_id);
	INT32 (*free_buf)(uintptr_t phy_addr, int ddr_id, unsigned int job_id);
} KDRV_CALLBACK_FUNC;

/*
 *  Format alignment value
 */
#define	FORMAT_ALIGN_VAL_YUV422_W         8
#define	FORMAT_ALIGN_VAL_YUV422_H         2
#define	FORMAT_ALIGN_VAL_YUV422_MB_W      16
#define	FORMAT_ALIGN_VAL_YUV422_MB_H      16
#define	FORMAT_ALIGN_VAL_YUV422_SCE_W     32
#define	FORMAT_ALIGN_VAL_YUV422_SCE_H     2

#define	FORMAT_ALIGN_VAL_YUV420_W         16
#define	FORMAT_ALIGN_VAL_YUV420_H         2
#define	FORMAT_ALIGN_VAL_YUV420_MB_W      16
#define	FORMAT_ALIGN_VAL_YUV420_MB_H      16
#define	FORMAT_ALIGN_VAL_YUV420_SCE_W     32
#define	FORMAT_ALIGN_VAL_YUV420_SCE_H     2
#define	FORMAT_ALIGN_VAL_YUV420_16X2_W    64
#define	FORMAT_ALIGN_VAL_YUV420_16X2_H    64
#define	FORMAT_ALIGN_VAL_YUV420_SP8_W     16
#define	FORMAT_ALIGN_VAL_YUV420_SP8_H     16

#define	FORMAT_ALIGN_VAL_ARGB1555_W       1
#define	FORMAT_ALIGN_VAL_ARGB1555_H       1
#define	FORMAT_ALIGN_VAL_ARGB8888_W       1
#define	FORMAT_ALIGN_VAL_ARGB8888_H       1
#define	FORMAT_ALIGN_VAL_RGB565_W         1
#define	FORMAT_ALIGN_VAL_RGB565_H         1
#define	FORMAT_ALIGN_VAL_RGB_CV_W         1
#define	FORMAT_ALIGN_VAL_RGB_CV_H         1

/*
 *  Format size calculation
 */
#if 0  //please use new macro definition in [hdal/drivers/k_driver/include/comm/calc_pool_size.h]
#define FORMAT_SIZE_YUV422(w, h)          ((ALIGN8_UP(w)) * (ALIGN2_UP(h)) * 2)
#define FORMAT_SIZE_YUV422_MB(w, h)       ((ALIGN16_UP(w)) * (ALIGN16_UP(h)) * 2)
#define FORMAT_SIZE_YUV422_SCE(w, h)      (((ALIGN32_UP(w) / 32) * 3 * 16) * ALIGN2_UP(h))

#define FORMAT_SIZE_YUV420_SP(w, h)       (ALIGN16_UP(w) * ALIGN2_UP(h) * 3 / 2)
#define FORMAT_SIZE_YUV420_MB(w, h)       (ALIGN16_UP(w) * ALIGN16_UP(h) * 3 / 2)
#define FORMAT_SIZE_YUV420_SCE(w, h)      (((((ALIGN32_UP(w) / 32) * 3 + 1) / 2) * 16) * ALIGN2_UP(h) * 3 / 2)
#define FORMAT_SIZE_YUV420_16X2(w, h)     (ALIGN64_UP(w) * ALIGN64_UP(h) * 3 / 2)
#define FORMAT_SIZE_YUV420_SP8(w, h)      (ALIGN64_UP(w) * ALIGN64_UP(h) * 3 / 2)

#define FORMAT_SIZE_ARGB1555(w, h)        ((w) * (h) * 2)
#define FORMAT_SIZE_ARGB8888(w, h)        ((w) * (h) * 4)
#define FORMAT_SIZE_RGB565(w, h)          ((w) * (h) * 2)
#define FORMAT_SIZE_RGB_CV(w, h)          ((w) * (h) * 2)
#endif

/*
 *  Size calculation for specific data format
 */
#define GET_DATA_SIZE_CODEC_MB_INFO(w, h, p_size)                       \
	do {                                                                \
		*(p_size) = (ALIGN64_UP(w) * ALIGN64_UP(h) / 16) + 4;           \
	} while(0)

#define	GET_DATA_SIZE_MD_EVENT_DATA(w, h, col_num, p_size)              \
	do {                                                                \
		int mb_size, mb_w_cnt, mb_h_cnt;                                \
		mb_size = ((w) <= 1920 && (h) <= 1080) ? 16 : 32;               \
		if ((mb_w_cnt = (w) / mb_size) < 256)                           \
			mb_w_cnt = 256;                                             \
		mb_h_cnt = (h) / mb_size;                                       \
		*(p_size) = ALIGN8_UP(mb_w_cnt * mb_h_cnt * (col_num + 1)) / 8; \
	} while(0)

#define	GET_DATA_SIZE_CAP_MD_EVENT_DATA(w, h, p_size)           \
	GET_DATA_SIZE_MD_EVENT_DATA(w, h, 1, p_size)

#define	GET_DATA_SIZE_VPE_MD_EVENT_DATA(w, h, p_size)           \
	GET_DATA_SIZE_MD_EVENT_DATA(w, h, 1, p_size)

#endif



