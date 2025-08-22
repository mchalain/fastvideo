#ifndef __FORMATS_H__
#define __FORMATS_H__

#ifndef FOURCC
#define FOURCC(a,b,c,d)	((a << 0) | (b << 8) | (c << 16) | (d << 24))
#endif

#define FOURCC_AB24  FOURCC('A','B','2','4')
#define FOURCC_XB24  FOURCC('X','B','2','4')
#define FOURCC_AR24  FOURCC('A','R','2','4')
#define FOURCC_XR24  FOURCC('X','R','2','4')
#define FOURCC_BGR4  FOURCC('B','G','R','4')
#define FOURCC_BG24  FOURCC('B','G','2','4')
#define FOURCC_RG24  FOURCC('R','G','2','4')
#define FOURCC_RGBA  FOURCC('R','G','B','A')
#define FOURCC_RGBP  FOURCC('R','G','B','P')
#define FOURCC_RG16  FOURCC('R','G','1','6')
#define FOURCC_R8    FOURCC('R','8',' ',' ')

#define FOURCC_GREY  FOURCC('G','R','E','Y')
#define FOURCC_YUYV  FOURCC('Y','U','Y','V')
#define FOURCC_YUY2  FOURCC('Y','U','Y','2')
#define FOURCC_NV12  FOURCC('N','V','1','2')
#define FOURCC_U008  FOURCC('U','0','0','8')

#define FOURCC_BA81  FOURCC('B','A','8','1')
#define FOURCC_RGGB  FOURCC('R','G','G','B')
#define FOURCC_GRBG  FOURCC('G','R','B','G')
#define FOURCC_GBRG  FOURCC('G','B','R','G')
#define FOURCC_BG10  FOURCC('B','G','1','0')
#define FOURCC_GB10  FOURCC('G','B','1','0')
#define FOURCC_BA10  FOURCC('B','A','1','0')
#define FOURCC_RG10  FOURCC('R','G','1','0')
#define FOURCC_BG12  FOURCC('B','G','1','2')
#define FOURCC_GB12  FOURCC('G','B','1','2')
#define FOURCC_BA12  FOURCC('B','A','1','2')
#define FOURCC_RG12  FOURCC('R','G','1','2')

#define FOURCC_JPEG  FOURCC('J','P','E','G')
#define FOURCC_MJPG  FOURCC('M','J','P','G')
#define FOURCC_H264  FOURCC('H','2','6','4')

#define BPP_TO_BYTE(_bpp)	(((_bpp) + 7) / 8)

#endif
