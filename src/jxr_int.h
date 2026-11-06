/* jxr_int.h - the JPEG XR decoder from jxrlib (Microsoft, BSD licence, see
 * LICENSE-JXRLIB.txt), merged into a few C89 files for FLASHNT.
 * Only the decoding side is kept. */
#ifndef FLASHNT_JXR_INT_H
#define FLASHNT_JXR_INT_H
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef __ANSI__
#define __ANSI__
#endif
#ifndef DISABLE_PERF_MEASUREMENT
#define DISABLE_PERF_MEASUREMENT
#endif
#include "flashnt.h"
/* the decoder's own checks and reports are compiled out */
#ifndef NDEBUG
#define NDEBUG
#endif
#include <assert.h>
#include <stdarg.h>
int jxr_noprint(const char *f, ...);
/* ---- windowsmediaphoto.h ---- */
/**@@@+++@@@@****************************************************************** */
/* */
/* Copyright ??? Microsoft Corp. */
/* All rights reserved. */
/* */
/* Redistribution and use in source and binary forms, with or without */
/* modification, are permitted provided that the following conditions are met: */
/* */
/* ??? Redistributions of source code must retain the above copyright notice, */
/*   this list of conditions and the following disclaimer. */
/* ??? Redistributions in binary form must reproduce the above copyright notice, */
/*   this list of conditions and the following disclaimer in the documentation */
/*   and/or other materials provided with the distribution. */
/* */
/* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" */
/* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE */
/* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE */
/* ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE */
/* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR */
/* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF */
/* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS */
/* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN */
/* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) */
/* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE */
/* POSSIBILITY OF SUCH DAMAGE. */
/* */
/**@@@---@@@@****************************************************************** */

#ifndef WMI_WINDOWSMEDIAPHOTO_H
#define WMI_WINDOWSMEDIAPHOTO_H

/*================================================================ */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__cplusplus) && !defined(EXTERN_C)
#define EXTERN_C extern "C"
#elif !defined(EXTERN_C)/* __cplusplus */
#define EXTERN_C extern
#endif /* __cplusplus */

/********************************************************************************
  Type definitions
********************************************************************************/
typedef int Bool;
typedef char Char;
typedef double Double;
typedef int Int;
typedef signed char I8;
typedef short I16; /* 16 bit int */
typedef int I32;
typedef long Long;
typedef unsigned char  PixelC;
typedef int PixelI;
typedef unsigned int UInt;
typedef unsigned long ULong;
typedef unsigned char U8; /* 8 bit uint */
typedef unsigned short U16;
typedef unsigned int U32; /* 32 bit uint */
typedef void Void;

typedef void* CTXSTRCODEC;


#define REENTRANT_MODE 1
/*
    DESCRIPTION OF COMPILER FLAG REENTRANT_MODE:

    //#define REENTRANT_MODE 1

    This compiler flag is related to the capability of banded decode 
    (decoding only one MB row of the source JPEG XR image at a time). 
	
    With REENTRANT_MODE defined, the decoder decodes one MB row on each call to 
    ImageStrDecDecode().
    
    The decoder acts as if it can only write to the single MBRow whose pointer was passed to it. 
    This acts as a proof of concept that the API would work if you passed it a small buffer 
    on each call to ImageStrDecDecode(). 
	
    The REENTRANT_MODE flag only works when the output image is in Orientations 0, 1 
    (vertically	flipped) or 2 (horizontally flipped).

    With REENTRANT_MODE defined, the function PKImageDecode_Copy_WMP()
    decodes only as far as the pRect parameter indicates. The width of the rectangle must be the width
    of the image, but on each call, this function will decode the image up to the end of the MB Row 
    which contains the i-th pixel row, where i = pRect->Y. 

    A target use of this version would be to have PKImageDecode_Copy_WMP() called in a loop, once for
    each MB row. On each call, pRect would specify a 1-MB-Row-tall rectangle that is the width of the 
    image. The decoder state is preserved until the Decoder finishes decoding the image.

    If, at a certain point, a request is made for a rectangle _above_ the last row decoded, then the
    decoder instance is terminated and re-initiated, and decoding re-starts, going from the beginning 
    of the image to the end of the current rectangle.

    ***

    We've chosen to uncomment-out this definition in this header file.  An alternate method would be
    to allow the user to define this in the PREPROCESSOR DEFINITIONS section of the properties page
    for each of the following projects: CommonLib, DecodeLib, JXRDecApp and JXRGlueLib.

*/
/*************************************************************************
    enums
*************************************************************************/
typedef enum {
    ICERR_OK = 0, ICERR_ERROR = -1
} ERR_CODE;

typedef enum BITDEPTH {
    BD_SHORT, BD_LONG,

    /* add new BITDEPTH here */ BD_MAX
} BITDEPTH;

typedef enum BITDEPTH_BITS {
    /* regular ones */
    BD_1, /*White is foreground */
    BD_8, BD_16, BD_16S, BD_16F, BD_32, BD_32S, BD_32F,

    /* irregular ones */
    BD_5, BD_10, BD_565,

    /* add new BITDEPTH_BITS here */ BDB_MAX,

    BD_1alt = 0xf, /*Black is foreground */
} BITDEPTH_BITS;

typedef enum OVERLAP {
    OL_NONE = 0, OL_ONE, OL_TWO,

    /* add new OVERLAP here */ OL_MAX
} OVERLAP;

typedef enum BITSTREAMFORMAT {
    SPATIAL = 0,     /* spatial order */
    FREQUENCY,       /* frequency order */
} BITSTREAMFORMAT;

typedef enum COLORFORMAT {
    Y_ONLY  = 0, 
	YUV_420 = 1,
	YUV_422 = 2,
	YUV_444 = 3,
	CMYK    = 4, 
	/*CMYKDIRECT = 5, */
	NCOMPONENT = 6,

    /* these are external-only */
    CF_RGB  = 7,
	CF_RGBE = 8,

    /* add new COLORFORMAT here */ CFT_MAX
} COLORFORMAT;

/* rotation and flip */
typedef enum ORIENTATION {
    /* CRW: Clock Wise 90% Rotation; FlipH: Flip Horizontally;  FlipV: Flip Vertically */
    /* Peform rotation FIRST! */
    /*                CRW FlipH FlipV */
    O_NONE = 0,    /* 0    0     0 */
    O_FLIPV,       /* 0    0     1 */
    O_FLIPH,       /* 0    1     0 */
    O_FLIPVH,      /* 0    1     1 */
    O_RCW,         /* 1    0     0 */
    O_RCW_FLIPV,   /* 1    0     1 */
    O_RCW_FLIPH,   /* 1    1     0 */
    O_RCW_FLIPVH,  /* 1    1     1 */
    /* add new ORIENTATION here */ O_MAX
} ORIENTATION;

typedef enum SUBBAND {
    SB_ALL = 0,             /* keep all subbands */
    SB_NO_FLEXBITS,     /* skip flex bits */
    SB_NO_HIGHPASS,     /* skip highpass */
    SB_DC_ONLY,         /* skip lowpass and highpass, DC only */
    SB_ISOLATED,        /* not decodable */
    /* add new SUBBAND here */ SB_MAX
} SUBBAND;

enum { RAW = 0, BMP = 1, PPM = 2, TIF = 3, HDR = 4, IYUV = 5, YUV422 = 6, YUV444 = 7};

typedef enum {ERROR_FAIL = -1, SUCCESS_DONE, PRE_READ_HDR, PRE_SETUP, PRE_DECODE, POST_READ_HDR } WMIDecoderStatus;

#ifndef FALSE
#define FALSE 0
#endif /* FALSE */

#ifndef TRUE
#define TRUE 1
#endif /* TRUE */

#define MAX_CHANNELS 16
#define LOG_MAX_TILES 12
#define MAX_TILES (1 << LOG_MAX_TILES)


/*================================================================ */
/* Codec-specific constants */
#define MB_WIDTH_PIXEL 16
#define MB_HEIGHT_PIXEL 16

#define BLK_WIDTH_PIXEL 4
#define BLK_HEIGHT_PIXEL 4

#define MB_WIDTH_BLK 4
#define MB_HEIGHT_BLK 4

/* The codec operates most efficiently when the framebuffers for encoder input */
/* and decoder output are: 1) aligned on a particular boundary, and 2) the stride */
/* is also aligned to this boundary (so that each scanline is also aligned). */
/* This boundary is defined below. */
#define FRAMEBUFFER_ALIGNMENT 128


/*================================================================ */
#define WMP_errSuccess 0

#define WMP_errFail -1
#define WMP_errNotYetImplemented -2
#define WMP_errAbstractMethod -3

#define WMP_errOutOfMemory -101
#define WMP_errFileIO -102
#define WMP_errBufferOverflow -103
#define WMP_errInvalidParameter -104
#define WMP_errInvalidArgument -105
#define WMP_errUnsupportedFormat -106
#define WMP_errIncorrectCodecVersion -107
#define WMP_errIndexNotFound -108
#define WMP_errOutOfSequence -109
#define WMP_errNotInitialized -110
#define WMP_errMustBeMultipleOf16LinesUntilLastCall -111
#define WMP_errPlanarAlphaBandedEncRequiresTempFile -112
#define WMP_errAlphaModeCannotBeTranscoded -113
#define WMP_errIncorrectCodecSubVersion -114


/*================================================================ */
typedef int ERR;

#define Failed(err) ((err)<0)

#define CRLF "\r\n"

#define CT_ASSERT(exp, uniq) typedef char __CT_ASSERT__##uniq[(exp) ? 1 : -1] /* Caller must provide a unique tag, or this fails to compile under GCC */

#if defined(_DEBUG) || defined(DBG)
#define Report(err, szExp, szFile, nLine) \
    fprintf(stderr, "FAILED: %ld=%s" CRLF, (err), (szExp)); \
    fprintf(stderr, "        %s:%ld" CRLF, (szFile), (nLine));  \

#else
#define Report(err, szExp, szFile, lLine) err = err
#endif

#define Call(exp) \
    if (Failed(err = (exp))) \
    { \
        Report(err, #exp, __FILE__, (long)__LINE__); \
        goto Cleanup; \
    } \
    else err = err

#define CallIgnoreError(errTmp, exp) \
    if (Failed(errTmp = (exp))) \
    { \
        Report(errTmp, #exp, __FILE__, (long)__LINE__); \
    } \
    else errTmp = errTmp


#define Test(exp, err) Call((exp) ? WMP_errSuccess : (err))
#define FailIf(exp, err) Call((exp) ? (err) : WMP_errSuccess)

/*================================================================ */
/* WMPStream interface */
/*================================================================ */
struct WMPStream
{
    union
    {
        struct tagFile
        {
            FILE* pFile;
        } file;

        struct tagBuf
        {
            U8* pbBuf;
            size_t cbBuf;
            size_t cbCur;
            size_t cbBufCount;
        } buf;

        void* pvObj;
    } state;

    Bool fMem;

    ERR (*Close)(struct WMPStream** pme);

    Bool (*EOS)(struct WMPStream* me);
    
    ERR (*Read)(struct WMPStream* me, void* pv, size_t cb);
    ERR (*Write)(struct WMPStream* me, const void* pv, size_t cb);
    /*ERR (*GetLine)(struct WMPStream* me, void* pv, size_t cb); */

    ERR (*SetPos)(struct WMPStream* me, size_t offPos);
    ERR (*GetPos)(struct WMPStream* me, size_t* poffPos);
};

EXTERN_C ERR CreateWS_File(struct WMPStream** ppWS, const char* szFilename, const char* szMode);
EXTERN_C ERR CloseWS_File(struct WMPStream** ppWS);

EXTERN_C ERR CreateWS_Memory(struct WMPStream** ppWS, void* pv, size_t cb);
EXTERN_C ERR CloseWS_Memory(struct WMPStream** ppWS);


/*================================================================ */
/* Enc/Dec data structure */
/*================================================================ */
typedef struct tagCWMImageInfo {
    size_t cWidth;
    size_t cHeight;
    COLORFORMAT cfColorFormat;
    BITDEPTH_BITS bdBitDepth;
    size_t cBitsPerUnit;
    size_t cLeadingPadding; /* number of leading padding */
    Bool bRGB; /* true: RGB;  false: BGR */
    U8 cChromaCenteringX; /* Relative location of Chroma w.r.t Luma */
    U8 cChromaCenteringY; /* Relative location of Chroma w.r.t Luma */

    /* Region of interest decoding */
    size_t cROILeftX;
    size_t cROIWidth;
    size_t cROITopY;
    size_t cROIHeight;

    /* thumbnail decode */
    Bool   bSkipFlexbits;
    size_t cThumbnailWidth;
    size_t cThumbnailHeight;

    /* image orientation */
    ORIENTATION oOrientation;

    /* post processing */
    U8 cPostProcStrength; /* 0(none) 1(light) 2(medium) 3(strong) 4(very strong) */

    /* user buffer is always padded to whole MB */
    Bool fPaddedUserBuffer;
} CWMImageInfo;

typedef struct tagCWMIStrCodecParam {
    Bool bVerbose;

    /* for macroblock quantization (DQUANT) */
    U8 uiDefaultQPIndex;
    U8 uiDefaultQPIndexYLP;
    U8 uiDefaultQPIndexYHP;
    U8 uiDefaultQPIndexU;
    U8 uiDefaultQPIndexULP;
    U8 uiDefaultQPIndexUHP;
    U8 uiDefaultQPIndexV;
    U8 uiDefaultQPIndexVLP;
    U8 uiDefaultQPIndexVHP;
    U8 uiDefaultQPIndexAlpha;

    COLORFORMAT cfColorFormat; 
    BITDEPTH bdBitDepth;
    OVERLAP olOverlap;
    BITSTREAMFORMAT bfBitstreamFormat;
    size_t cChannel; /* number of color channels including alpha */
    U8 uAlphaMode; /* 0:no alpha 1: alpha only else: something + alpha */
    SUBBAND sbSubband;  /* which subbands to keep */
    U8  uiTrimFlexBits;

    struct WMPStream* pWStream;
    size_t cbStream;

    /* tiling info */
    U32  cNumOfSliceMinus1V;     /* # of vertical slices */
    U32 uiTileX[MAX_TILES]; /* width in MB of each veritical slice */
    U32  cNumOfSliceMinus1H;     /* # of horizontal slices */
    U32 uiTileY[MAX_TILES]; /* height in MB of each horizontal slice */

    /*32f and 32s conversion parameters */
    U8 nLenMantissaOrShift;
    I8 nExpBias;

    Bool bBlackWhite;

    Bool bUseHardTileBoundaries; /*default is soft tile boundaries */
 
    Bool bProgressiveMode; /*default is sequential mode */

    Bool bYUVData; /*default is cfColorFormat data */

    Bool bUnscaledArith; /*force unscaled arithmetic */
   
    /* Perf measurement */
    Bool fMeasurePerf;
} CWMIStrCodecParam;

typedef struct tagCWMImageBufferInfo {
    void* pv;           /* pointer to scanline buffer */
    size_t cLine;       /* count of scanlines */
    size_t cbStride;    /* count of BYTE for stride */
#ifdef REENTRANT_MODE
    unsigned int uiFirstMBRow;     /* Current First MB Row being decoded */
    unsigned int uiLastMBRow;     /* Current Last MB Row being decoded */
    size_t cLinesDecoded;         /* Number of lines decoded and returned in low-mem mode */
#endif /* REENTRANT_MODE */
} CWMImageBufferInfo;




/****************************************************************/
/* Encode API                                                   */
/****************************************************************/
EXTERN_C Int ImageStrEncInit(
    CWMImageInfo* pII,
    CWMIStrCodecParam *pSCP,
    CTXSTRCODEC* pctxSC);

EXTERN_C Int ImageStrEncEncode(
    CTXSTRCODEC ctxSC,
    const CWMImageBufferInfo* pBI);

EXTERN_C Int ImageStrEncTerm(
    CTXSTRCODEC ctxSC);


/****************************************************************/
/* Decode API                                                   */
/****************************************************************/
struct CWMImageStrCodec;

EXTERN_C Int ImageStrDecGetInfo(
    CWMImageInfo* pII,
    CWMIStrCodecParam *pSCP);

EXTERN_C Int ImageStrDecInit(
    CWMImageInfo* pII,
    CWMIStrCodecParam *pSCP,
    CTXSTRCODEC* pctxSC);

EXTERN_C Int ImageStrDecDecode(
    CTXSTRCODEC ctxSC,
    const CWMImageBufferInfo* pBI
#ifdef REENTRANT_MODE
    , size_t *pcDecodedLines
#endif    
    );

EXTERN_C Int ImageStrDecTerm(
    CTXSTRCODEC ctxSC);

EXTERN_C Int WMPhotoValidate(
    CWMImageInfo * pII,
    CWMIStrCodecParam * pSCP);


/****************************************************************/
/* Transcoding API                                              */
/****************************************************************/
typedef struct tagCWMTranscodingParam {
    size_t cLeftX;
    size_t cWidth;
    size_t cTopY;
    size_t cHeight;  /* interested region */

    BITSTREAMFORMAT bfBitstreamFormat; /* desired bitstream format */
/*    COLORFORMAT cfColorFormat; // desired color format */
    U8 uAlphaMode; /* 0:no alpha 1: alpha only else: something + alpha */
    SUBBAND sbSubband;  /* which subbands to keep */
    ORIENTATION oOrientation; /* flip / right angle rotation */
    Bool bIgnoreOverlap;
} CWMTranscodingParam;

EXTERN_C Int WMPhotoTranscode(
    struct WMPStream* pStreamDec,   /* input bitstrean */
    struct WMPStream* pStreamEnc,   /* output bitstream */
    CWMTranscodingParam* pParam     /* transcoding parameters */
);

typedef struct tagCWMDetilingParam {
    size_t cWidth;
    size_t cHeight;  /* image size */
    size_t cChannel; /* # of channels */
    OVERLAP olOverlap; /* overlap */
    BITDEPTH_BITS bdBitdepth; /* bit depth */

    /* tiling info */
    U32  cNumOfSliceMinus1V;     /* # of vertical slices */
    U32 uiTileX[MAX_TILES]; /* position in MB of each veritical slice */
    U32  cNumOfSliceMinus1H;     /* # of horizontal slices */
    U32 uiTileY[MAX_TILES]; /* position in MB of each horizontal slice */

    /* image info */
    void * pImage;
    size_t cbStride;
} CWMDetilingParam;

EXTERN_C Int WMPhotoDetile(
    CWMDetilingParam * pParam   /* detiling parameters */
);

#if _MSC_VER
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

#endif /* WMI_WINDOWSMEDIAPHOTO_H */


/* ---- common.h ---- */
/**@@@+++@@@@****************************************************************** */
/* */
/* Copyright (c) Microsoft Corp. */
/* All rights reserved. */
/* */
/* Redistribution and use in source and binary forms, with or without */
/* modification, are permitted provided that the following conditions are met: */
/* */
/* * Redistributions of source code must retain the above copyright notice, */
/*   this list of conditions and the following disclaimer. */
/* * Redistributions in binary form must reproduce the above copyright notice, */
/*   this list of conditions and the following disclaimer in the documentation */
/*   and/or other materials provided with the distribution. */
/* */
/* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" */
/* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE */
/* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE */
/* ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE */
/* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR */
/* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF */
/* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS */
/* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN */
/* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) */
/* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE */
/* POSSIBILITY OF SUCH DAMAGE. */
/* */
/**@@@---@@@@****************************************************************** */

#ifndef WMI_COMMON_H
#define WMI_COMMON_H

/*************************************************************************
// Common typedef's
*************************************************************************/
typedef enum  { ENCODER = 0, DECODER = 1 } CODINGMODE;

typedef enum tagBand
{
    BAND_HEADER = 0,
    BAND_DC = 1,
    BAND_LP = 2,
    BAND_AC = 3,
    BAND_FL = 4
} BAND;

/*************************************************************************
    struct / class definitions
*************************************************************************/
/*#define SIGNATURE_BYTES 8   // Bytes for GDI+ signature */
#define CODEC_VERSION 1
#define CODEC_SUBVERSION 0
#define CODEC_SUBVERSION_NEWSCALING_SOFT_TILES 1
#define CODEC_SUBVERSION_NEWSCALING_HARD_TILES 9

#define CONTEXTX 8
#define CTDC 5
#define NUMVLCTABLES 21 /* CONTEXTX * 2 + CTDC */
#define AVG_NDIFF 3

#define MAXTOTAL 32767 /* 511 should be enough */

/** Quantization related defines **/
#define SHIFTZERO 1 /* >= 0 */
#define QPFRACBITS 2  /* or 0 only supported */

/** adaptive huffman encoding / decoding struct **/
typedef struct CAdaptiveHuffman
{
    Int     m_iNSymbols;
    const Int *m_pTable;
    const Int *m_pDelta, *m_pDelta1;
    Int     m_iTableIndex;
    const short *m_hufDecTable;
    Bool    m_bInitialize;
    /*Char    m_pLabel[8]; // for debugging - label attached to constructor */

    Int     m_iDiscriminant, m_iDiscriminant1;
    Int     m_iUpperBound;
    Int     m_iLowerBound;
} CAdaptiveHuffman;


/************************************************************************************
  Context structures
************************************************************************************/
typedef struct CAdaptiveModel {
    Int     m_iFlcState[2];
    Int     m_iFlcBits[2];
    BAND    m_band;
} CAdaptiveModel;

typedef struct CCBPModel {
    Int     m_iCount0[2];
    Int     m_iCount1[2];
    Int     m_iState[2];
} CCBPModel;

/*************************************************************************
    globals
*************************************************************************/
extern Int grgiZigzagInv4x4_lowpass[];
extern Int grgiZigzagInv4x4H[];
extern Int grgiZigzagInv4x4V[];
extern const Int gSignificantRunBin[];
extern const Int gSignificantRunFixedLength[];
static const Int cblkChromas[] = {0,4,8,16, 16,16,16, 0,0};
/*************************************************************************
    function declarations
*************************************************************************/
/* common utilities */
Void Clean (CAdaptiveHuffman *pAdHuff);
CAdaptiveHuffman *Allocate (Int iNSymbols, CODINGMODE cm);

/* Timing functions */
void    reset_timing(double *time);
void    report_timing(const char *s, double time);
/* static double   timeperclock; */

/** adaptive model functions **/
Void UpdateModelMB (COLORFORMAT cf, Int iChannels, Int iLaplacianMean[], CAdaptiveModel *m_pModel);

/** adaptive huffman encoder / decoder functions **/
Void Adapt (CAdaptiveHuffman *pAdHuff, Bool bFixedTables);
Void AdaptFixed (CAdaptiveHuffman *pAdHuff);
Void AdaptDiscriminant (CAdaptiveHuffman *pAdHuff);

#ifndef _PREFAST_
#endif

#endif  /* WMI_COMMON_H */

/* ---- ansi.h ---- */
/**@@@+++@@@@****************************************************************** */
/* */
/* Copyright (c) Microsoft Corp. */
/* All rights reserved. */
/* */
/* Redistribution and use in source and binary forms, with or without */
/* modification, are permitted provided that the following conditions are met: */
/* */
/* * Redistributions of source code must retain the above copyright notice, */
/*   this list of conditions and the following disclaimer. */
/* * Redistributions in binary form must reproduce the above copyright notice, */
/*   this list of conditions and the following disclaimer in the documentation */
/*   and/or other materials provided with the distribution. */
/* */
/* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" */
/* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE */
/* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE */
/* ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE */
/* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR */
/* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF */
/* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS */
/* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN */
/* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) */
/* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE */
/* POSSIBILITY OF SUCH DAMAGE. */
/* */
/**@@@---@@@@****************************************************************** */

/*================================ */
/* bitio functions */
/*================================ */
#define PACKETLENGTH (1U<<12)   /* 4kB */

#define readIS_L1(pSC, pIO) readIS(pSC, pIO)
#define readIS_L2(pSC, pIO) ((void)(pSC), (void)(pIO))

#define writeIS_L1(pSC, pIO) writeIS(pSC, pIO)
#define writeIS_L2(pSC, pIO) ((void)(pSC), (void)(pIO))


/*================================ */
/* common defines */
/*================================ */
#define FORCE_INLINE
#define CDECL

/* Note these are modified from the original library, */
/* which made its own defs that were wrong on Linux. */
/* removed: #include <stdint.h> */
#define UINTPTR_T size_t
#define INTPTR_T ptrdiff_t


/*================================ */
/* quantization optimization */
/*================================ */
/*#define RECIP_QUANT_OPT */


/*================================ */
/* For big-endian Linux; not tested */
/*================================ */
/* little-endian targets only (the endian.h test is gone) */

/* ---- strcodec.h ---- */
/**@@@+++@@@@****************************************************************** */
/* */
/* Copyright (c) Microsoft Corp. */
/* All rights reserved. */
/* */
/* Redistribution and use in source and binary forms, with or without */
/* modification, are permitted provided that the following conditions are met: */
/* */
/* * Redistributions of source code must retain the above copyright notice, */
/*   this list of conditions and the following disclaimer. */
/* * Redistributions in binary form must reproduce the above copyright notice, */
/*   this list of conditions and the following disclaimer in the documentation */
/*   and/or other materials provided with the distribution. */
/* */
/* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" */
/* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE */
/* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE */
/* ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE */
/* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR */
/* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF */
/* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS */
/* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN */
/* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) */
/* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE */
/* POSSIBILITY OF SUCH DAMAGE. */
/* */
/**@@@---@@@@****************************************************************** */

#include <stddef.h>

/* (merged) #include "windowsmediaphoto.h" */
/* (merged) #include "common.h" */
/* #include "xplatform_image.h" */

/* added for Xcode PK universal binary */
#ifdef __ppc__
#define _BIG__ENDIAN_
#endif

/*================================================================ */
#ifdef ENABLE_OPTIMIZATIONS
#if defined(JXR_WIN32_INTRINSICS) && !defined(_WIN64)
#define WMP_OPT_SSE2

#define WMP_OPT_CC_ENC
/*#define WMP_OPT_TRFM_ENC */
/*#define WMP_OPT_QT */

#define WMP_OPT_CC_DEC
#define WMP_OPT_TRFM_DEC

#define X86OPT_INLINE

#endif
#endif /* ENABLE_OPTIMIZATIONS */

/*================================================================ */
/*#ifdef JXR_WIN32_INTRINSICS */
#if defined(JXR_WIN32_INTRINSICS) && !defined(UNDER_CE)   /* _WIN32 seems to be defined always in VS2005 for ARM platform */
#define PLATFORM_X86
/* (merged) #include "..\x86\x86.h" */
#endif

#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(P) { (P) = (P); }
#endif /* UNREFERENCED_PARAMETER */

#ifdef UNDER_CE
#define PLATFORM_WCE
/* (merged) #include "arm.h" */
#endif

#ifdef __ANSI__
#define PLATFORM_ANSI
/* (merged) #include "ansi.h" */
#endif

/*================================================================ */

#ifdef PLATFORM_ANSI
typedef u64 U64;
#else /* PLATFORM_ANSI */
typedef unsigned __int64 U64;
#endif /* PLATFORM_ANSI */

/*================================================================ */
#define MARKERCOUNT (PACKETLENGTH * 2)

/* The following macros depend on UINTPTR_T and INTPTR_T being properly defined */
/* so that they are equal to pointer width. Confirm and fail if our assumptions are wrong. */
CT_ASSERT(sizeof(UINTPTR_T) == sizeof(void*), strcodec1);
CT_ASSERT(sizeof(INTPTR_T) == sizeof(void*), strcodec2);

/* wrap around pointer, s=pow(2,n), p wraps aligned to s */
#define WRAPPTR(p, s) ((void*)((UINTPTR_T)(p) & ~(UINTPTR_T)(s)))

/* mask certain bit inside a pointer, simulate wrap around */
#define MASKPTR(p, m) ((void*)((UINTPTR_T)(p) & (INTPTR_T)(m)))

/* test for more than 1 packet data */
#define PACKET1(ps, pc, s) (((INTPTR_T)(ps) ^ (INTPTR_T)(pc)) & ((UINTPTR_T)(s)))

/* alternate pointer p between 2 values aligned to s, s=pow(2,n) */
/*#define ALTPTR(p, s) ((void*)((uintptr_t)(p) ^ (s))) */

/* align point, s=pow(2,n), p aligns to s */
#define ALIGNUP(p, s) ((void*)(((UINTPTR_T)(p) + ((UINTPTR_T)(s) - 1)) & ~((UINTPTR_T)(s) - 1)))
#define ALIGNDOWN(p, s) ((void*)((UINTPTR_T)(p) & ~((UINTPTR_T)(s) - 1)))

/*================================================================ */
/* timer support */
/*================================================================ */

#define TraceResult(a)

/*================================================================ */
typedef enum tagPacketType
{
    PK_NULL = 0,
    PK_DC = 1, PK_AD, PK_AC, PK_CP,
    PK_MAX,
} PACKETTYPE;

typedef struct tagIOContext
{
    U8 P0[PACKETLENGTH];        /* packet circular buffer 0 */
    U8 P1[PACKETLENGTH];        /* packet circular buffer 1 */

    union
    {
        U8 P2[PACKETLENGTH];
        struct
        {
            U32 uiShadow;   /* shadow of P0[0]-P0[3] */

            U32 uiAccumulator;  /* 32bit acc as bit field cache */
            U32 cBitsUsed;  /* # of bits used of acc, [0,16) */

            U8* pbPacket;   /* packet pointer */
            U8* pbCurrent;  /* current pointer */

            struct WMPStream* pWS;    /* pointer to WMPStream */
            long offPacket; /* byte offset into stream */

            /*ULARGE_INTEGER u64Acc; */

    /*======================================== */
    /* index packet, used for packet retrieval */
    /*======================================== */
            U32 cIndex; /* current index for index packet */
            long offIndex;  /* byte offset into stream for index packet */
        }State;
    }P2Info;
    U8 P3[PACKETLENGTH];    /* index packet buffer */
} IOContext;

typedef struct tagMemReadState
{    
    U8* pbBuf;
    size_t cbBuf;
    size_t cbCur;
} MemReadState;

typedef struct tagBitIOInfo
{
    U32 uiShadow;   /* shadow of first 4B of circular buffer */

    U32 uiAccumulator;  /* 32bit acc as bit field cache */
    U32 cBitsUsed;  /* # of bits used of acc, [0,16) */
#ifdef ARMOPT_BITIO
    U32 cBitsUnused;  /* # of bits remain unused in acc, [0,32] */
#endif

    I32 iMask;  /* mask used simulate pointer wrap around */

    U8* pbStart;    /* start pointer */
#ifndef ARMOPT_BITIO
    U8* pbCurrent;  /* current pointer */
#else
    U32* pbCurrent;  /* current pointer */
#endif

    struct WMPStream* pWS;  /* pointer to WMPStream */
    size_t offRef;  /* reference offset on IStream, */
                            /* for read, it moves along the stream */
                            /* for write, it stays at the attach point */
} BitIOInfo;

/*================================================================ */
typedef struct tagCWMIQuantizer {
    U8 iIndex;
    I32 iQP;
    I32 iOffset;
    I32 iMan;
    I32 iExp;
#if defined(WMP_OPT_QT)
    float f1_QP;
    double d1_QP;
#endif    
} CWMIQuantizer;

/* temporary bridge between old APIs and streaming APIs */
typedef struct tagCWMIMBInfo {
    I32 iBlockDC[MAX_CHANNELS][16];
    I32 iOrientation;
    Int iCBP[MAX_CHANNELS];
    Int iDiffCBP[MAX_CHANNELS];
    U8 iQIndexLP; /* 0 - 15 */
    U8 iQIndexHP; /* 0 - 15 */
} CWMIMBInfo;

struct CWMImageStrCodec;

typedef Int (*ImageDataProc)(struct CWMImageStrCodec*);

/** scan model **/
typedef struct CAdaptiveScan {
    U32		uTotal;
    U32		uScan;
} CAdaptiveScan;

/** Adaptive context model **/
typedef struct CCodingContext {
    BitIOInfo * m_pIODC;
    BitIOInfo * m_pIOLP;
    BitIOInfo * m_pIOAC;
    BitIOInfo * m_pIOFL;

    /** adaptive huffman structs **/
    CAdaptiveHuffman *m_pAdaptHuffCBPCY;
    CAdaptiveHuffman *m_pAdaptHuffCBPCY1;
    CAdaptiveHuffman *m_pAHexpt[NUMVLCTABLES];

    /** 4x4 zigzag patterns */
    CAdaptiveScan m_aScanLowpass[16];
    CAdaptiveScan m_aScanHoriz[16];
    CAdaptiveScan m_aScanVert[16];

    /** Adaptive bit reduction model **/
    CAdaptiveModel m_aModelAC;
    CAdaptiveModel m_aModelLP;
    CAdaptiveModel m_aModelDC;

    /** Adaptive lowpass CBP model **/
    Int     m_iCBPCountZero;
    Int     m_iCBPCountMax;

    /** Adaptive AC CBP model **/
    CCBPModel m_aCBPModel;

    /** Trim flex bits - externally set **/
    Int     m_iTrimFlexBits;

    Bool m_bInROI;  /* inside ROI (for region decode and compressed domain cropping)? */
} CCodingContext;

/* Following stuff used to be in strPredQuant.h */
/* circulant buffer for 2 MB rows: current row and previous row */
typedef struct tagCWMIPredInfo {
    Int iQPIndex;     /* QP Index */
    Int iCBP;      /* coded block pattern */
    PixelI iDC;    /* DC of MB */
    PixelI iAD[6];
    PixelI * piAD; /* AC of DC block: [2] 420UV [4] 422UV [6] elsewhere */
}CWMIPredInfo;

/* the following is used on decode side while reading image info */
typedef struct CWMImageStrCodecParameters {
    size_t cVersion;
    size_t cSubVersion;
    COLORFORMAT cfColorFormat; /* color format */
    Bool bRBSwapped; /* blue and red shall be swapped in BGR555,565,101010 */
    Bool bAlphaChannel; /* alpha channel present */
    Bool bScaledArith; /* lossless mode */
    Bool bIndexTable; /* index table present */
    Bool bTrimFlexbitsFlag; /* trimmed flexbits indicated in packet header */
    Bool bUseHardTileBoundaries; /*default is soft tile boundaries */
    size_t cNumChannels;
    size_t cExtraPixelsTop;
    size_t cExtraPixelsLeft;
    size_t cExtraPixelsBottom;
    size_t cExtraPixelsRight;
    Bool bTranscode;  /* transcoding flag */
    U32 uQPMode;       /* 0/1: no dquant/with dquant, first bit for DC, second bit for LP, third bit for HP */
    U8 uiQPIndexDC[MAX_CHANNELS];
    U8 uiQPIndexLP[MAX_CHANNELS];
    U8 uiQPIndexHP[MAX_CHANNELS];
}CCoreParameters;

typedef struct CWMITile
{
    CWMIQuantizer * pQuantizerDC[MAX_CHANNELS];
    CWMIQuantizer * pQuantizerLP[MAX_CHANNELS];
    CWMIQuantizer * pQuantizerHP[MAX_CHANNELS];
    U8 cNumQPLP;
    U8 cNumQPHP;
    U8 cBitsLP;
    U8 cBitsHP;

    Bool bUseDC;
    Bool bUseLP;
    U8 cChModeDC;
    U8 cChModeLP[16];
    U8 cChModeHP[16];
} CWMITile;

#ifdef ARMOPT_COLORCONVERSION_C
/* (merged) #include "ARM_InvColorConversion.h" */
#endif

struct tagPostProcInfo{
    Int iMBDC;                  /* DC of MB */
    U8 ucMBTexture;             /* MB texture   : 0(flat) 1(horizontal) 2(vertical) 3(bumpy) */
    Int iBlockDC[4][4];         /* DC of block */
    U8 ucBlockTexture[4][4];    /* block texture: 0(flat) 1(horizontal) 2(vertical) 3(bumpy) */
};

typedef struct CWMImageStrCodec {
#ifdef ARMOPT_COLORCONVERSION_C
    CWMImageStrInvCCParam InvCCParam;
#endif

    size_t cbStruct;

    CWMImageInfo WMII;
    CWMIStrCodecParam WMISCP;
    CWMImageBufferInfo WMIBI;
    CWMIMBInfo MBInfo;

    /** core parameters **/
    CCoreParameters m_param;

    struct CWMDecoderParameters *m_Dparam;  /* this is specified thru pointer because the same set of parameters may be used by multiple image planes */

    U8 cSB;

    Bool m_bUVResolutionChange;

    Bool bTileExtraction;

    BitIOInfo * pIOHeader;

    Bool bUseHardTileBoundaries; /*default is soft tile boundaries */

    PixelI * pInterU;
    PixelI * pInterV;

    /*============== tile related info begins here =========== */
    /* index table */
    size_t *pIndexTable;
  
    /* current tile position */
    size_t cTileRow;
    size_t cTileColumn;

    /* tile boundary */
    Bool m_bCtxLeft;
    Bool m_bCtxTop;

    Bool m_bResetRGITotals;
    Bool m_bResetContext;

    CWMITile * pTile;
    
    /* BitIOs */
    BitIOInfo ** m_ppBitIO;
    size_t cNumBitIO;
    size_t cHeaderSize;

    /* coding contexts */
    struct CCodingContext *m_pCodingContext;
    size_t cNumCodingContext;

    /*============== tile related info ends here  =========== */

    size_t cNumOfQPIndex;        /* number of QP indexes */
    U8 cBitsDQUANT;              /* number of bits to encode DQUANT */

    size_t cRow;        /* row for current macro block */
    size_t cColumn;     /* column for current macro block */

    size_t cmbWidth;    /* macro block/image width */
    size_t cmbHeight;   /* macro block/image height */

    size_t cbChannel;   /* byte/channel */

    size_t mbX, mbY;
    size_t tileX, tileY;
    Bool bVertTileBoundary, bHoriTileBoundary;
    Bool bOneMBLeftVertTB, bOneMBRightVertTB; /*Macroblock to the left and to the right of tile boundaries */

    PixelI iPredBefore[2][2];
    PixelI iPredAfter[2][2];

    /*================================ */
    /* input data into */
    /* macro block 3 of 2x2 working widow */
    /*================================ */
    ImageDataProc Load;
    /*ImageDataProc Load2; */
    ImageDataProc Transform;
    ImageDataProc TransformCenter;

    /*================================ */
    ImageDataProc Quantize;
    /*ImageDataProc QuantizeLuma; */
    /*ImageDataProc QuantizeChroma; */

    /*================================ */
    /* process and store data from */
    /* macro block 0 of 2x2 working window */
    /*================================ */
    ImageDataProc ProcessTopLeft;
    ImageDataProc ProcessTop;
    ImageDataProc ProcessTopRight;
    ImageDataProc ProcessLeft;
    ImageDataProc ProcessCenter;
    ImageDataProc ProcessRight;
    ImageDataProc ProcessBottomLeft;
    ImageDataProc ProcessBottom;
    ImageDataProc ProcessBottomRight;
    

    /*================================ */
    /* 2 MB working window for encoder */
    /*================================ */
    PixelI *pPlane[MAX_CHANNELS];

    /*================================ */
    /* 2 rows of MB buffer */
    /*================================ */
    PixelI *a0MBbuffer[MAX_CHANNELS];  /* pointer to start of previous MB row */
    PixelI *a1MBbuffer[MAX_CHANNELS];  /* pointer to start of current MB row */
    PixelI *p0MBbuffer[MAX_CHANNELS];  /* working pointer to start of previous row MB */
    PixelI *p1MBbuffer[MAX_CHANNELS];  /* working pointer to start of current row MB */

    /*================================ */
    /* downsampling buffer for UV */
    /*================================ */
    PixelI * pResU;
    PixelI * pResV;

    /*================================ */
    /* circular buffer for 2 MB rows: current row and previous row */
    /*================================ */
    CWMIPredInfo *PredInfo[MAX_CHANNELS];
    CWMIPredInfo *PredInfoPrevRow[MAX_CHANNELS];
    CWMIPredInfo *pPredInfoMemory;

    struct WMPStream ** ppWStream;

    char **ppTempFile;

    /* interleaved alpha support - linked structure for Alpha channel */
    struct CWMImageStrCodec *m_pNextSC;
    Bool   m_bSecondary;

    /*================================ */
    /* Perf Timers */
    /*================================ */
#ifndef DISABLE_PERF_MEASUREMENT
    Bool            m_fMeasurePerf;
    struct PERFTIMERSTATE *m_ptEndToEndPerf;   /* Measures from Init to Term, including I/O */
    struct PERFTIMERSTATE *m_ptEncDecPerf;     /* Measures time spent in ImageStrEncEncode/ImageStrDecDecode, excluding I/O */
#endif /* DISABLE_PERF_MEASUREMENT */

    /* postproc information for 2 MB rows: 0(previous row) 1(current row) */
    struct tagPostProcInfo * pPostProcInfo[MAX_CHANNELS][2];
} CWMImageStrCodec;


/*================================================================ */
ERR WMPAlloc(void** ppv, size_t cb);
ERR WMPFree(void** ppv);

/*================================================================ */
Void initMRPtr(CWMImageStrCodec*);
Void advanceMRPtr(CWMImageStrCodec*);
Void swapMRPtr(CWMImageStrCodec*);

Int IDPEmpty(CWMImageStrCodec*);

/*================================================================ */
extern const int dctIndex[3][16];
extern const int blkOffset[16];
extern const int blkOffsetUV[4];
extern const int blkOffsetUV_422[8];

extern const U8 idxCC[16][16];
extern const U8 idxCC_420[8][8];

extern const Char gGDISignature[];

/*================================================================ */
Int allocatePredInfo(CWMImageStrCodec*);
Void freePredInfo(CWMImageStrCodec*);
Void advanceOneMBRow(CWMImageStrCodec*);

/*================================================================ */
/* bit I/O */
/*================================================================ */
Int allocateBitIOInfo(CWMImageStrCodec*);
Int setBitIOPointers(CWMImageStrCodec* pSC);

#ifndef ARMOPT_BITIO
U32 peekBit16(BitIOInfo* pIO, U32 cBits);
U32 flushBit16(BitIOInfo* pIO, U32 cBits);
U32 getBit16(BitIOInfo* pIO, U32 cBits);
U32 getBool16(BitIOInfo* pIO);
I32 getBit16s(BitIOInfo* pIO, U32 cBits);
U32 getBit32(BitIOInfo* pIO, U32 cBits);
U32 flushToByte(BitIOInfo* pIO);
#endif  /* ARMOPT_BITIO */

Void putBit16z(BitIOInfo* pIO, U32 uiBits, U32 cBits);
Void putBit16(BitIOInfo* pIO, U32 uiBits, U32 cBits);
Void putBit32(BitIOInfo* pIO, U32 uiBits, U32 cBits);
Void fillToByte(BitIOInfo* pIO);

U32 getSizeRead(BitIOInfo* pIO);
U32 getSizeWrite(BitIOInfo* pIO);

U32 getPosRead(BitIOInfo* pIO);

/* safe function, solely for the convenience of test code */
#ifndef ARMOPT_BITIO
U32 getBit16_S(CWMImageStrCodec* pSC, BitIOInfo* pIO, U32 cBits);
#endif  /* ARMOPT_BITIO */

/*================================================================ */
/* packet I/O */
/*================================================================ */
ERR attachISRead(BitIOInfo* pIO, struct WMPStream* pWS, CWMImageStrCodec* pSC);
ERR readIS(CWMImageStrCodec* pSC, BitIOInfo* pIO);
ERR detachISRead(CWMImageStrCodec* pSC, BitIOInfo* pIO);

ERR attachISWrite(BitIOInfo* pIO, struct WMPStream* pWS);
ERR writeIS(CWMImageStrCodec* pSC, BitIOInfo* pIO);
ERR detachISWrite(CWMImageStrCodec* pSC, BitIOInfo* pIO);


/*================================================================ */
/* post processing for decoder */
/*================================================================ */
Int initPostProc(struct tagPostProcInfo * strPostProcInfo[MAX_CHANNELS][2], size_t mbWidth, size_t iNumChannels);
Void termPostProc(struct tagPostProcInfo * strPostProcInfo[MAX_CHANNELS][2], size_t iNumChannels);
Void slideOneMBRow(struct tagPostProcInfo * strPostProcInfo[MAX_CHANNELS][2], size_t iNumChannels, size_t mbWidth, Bool top, Bool bottom);
Void updatePostProcInfo(struct tagPostProcInfo * strPostProcInfo[MAX_CHANNELS][2], PixelI * p, size_t mbX, size_t cc);
Void postProcMB(struct tagPostProcInfo * strPostProcInfo[MAX_CHANNELS][2], PixelI * p0, PixelI * p1, size_t mbX, size_t cc, Int threshold);
Void postProcBlock(struct tagPostProcInfo * strPostProcInfo[MAX_CHANNELS][2], PixelI * p0, PixelI * p1, size_t mbX, size_t cc, Int threshold);

/*================================================================ */
/* Simple BitIO access functions */
/*================================================================ */
typedef struct tagSimpleBitIO
{
    struct WMPStream* pWS;
    U32 cbRead;
    U8 bAccumulator;
    U32 cBitLeft;
} SimpleBitIO;

ERR attach_SB(SimpleBitIO* pSB, struct WMPStream* pWS);
U32 getBit32_SB(SimpleBitIO* pSB, U32 cBits);
Void flushToByte_SB(SimpleBitIO* pSB);
U32 getByteRead_SB(SimpleBitIO* pSB);
ERR detach_SB(SimpleBitIO* pSB);

/*---------------------------------------------------------------- */
EXTERN_C Bool EOSWS_File(struct WMPStream* pWS);

EXTERN_C ERR ReadWS_File(struct WMPStream* pWS, void* pv, size_t cb);
EXTERN_C ERR WriteWS_File(struct WMPStream* pWS, const void* pv, size_t cb);
/*EXTERN_C ERR GetLineWS_File(struct WMPStream* pWS, void* pv, size_t cb); */

EXTERN_C ERR SetPosWS_File(struct WMPStream* pWS, size_t offPos);
EXTERN_C ERR GetPosWS_File(struct WMPStream* pWS, size_t* poffPos);

/*---------------------------------------------------------------- */
EXTERN_C Bool EOSWS_Memory(struct WMPStream* pWS);

EXTERN_C ERR ReadWS_Memory(struct WMPStream* pWS, void* pv, size_t cb);
EXTERN_C ERR WriteWS_Memory(struct WMPStream* pWS, const void* pv, size_t cb);
/*EXTERN_C ERR GetLineWS_Memory(struct WMPStream* pWS, void* pv, size_t cb); */

EXTERN_C ERR SetPosWS_Memory(struct WMPStream* pWS, size_t offPos);
EXTERN_C ERR GetPosWS_Memory(struct WMPStream* pWS, size_t* poffPos);

/*EXTERN_C ERR GetPtrWS_Memory(struct WMPStream* pWS, size_t align, U8** ppb); */
/*---------------------------------------------------------------- */
EXTERN_C Bool EOSWS_List(struct WMPStream* pWS);

EXTERN_C ERR ReadWS_List(struct WMPStream* pWS, void* pv, size_t cb);
EXTERN_C ERR WriteWS_List(struct WMPStream* pWS, const void* pv, size_t cb);

EXTERN_C ERR SetPosWS_List(struct WMPStream* pWS, size_t offPos);
EXTERN_C ERR GetPosWS_List(struct WMPStream* pWS, size_t* poffPos);

EXTERN_C ERR CreateWS_List(struct WMPStream** ppWS);
EXTERN_C ERR CloseWS_List(struct WMPStream** ppWS);

/********************************************************************/
/* Stuff related to scale/spatial ordering */
typedef struct PacketInfo
{
    BAND    m_iBand;
    size_t  m_iSize;
    size_t  m_iOffset;
    struct PacketInfo *m_pNext;
} PacketInfo;
/********************************************************************/

/********************************************************************/
const static Int blkIdxByRow[4][4] = {{0, 1, 4, 5}, {2, 3, 6, 7}, {8, 9, 12, 13}, {10, 11, 14, 15}};
const static Int blkIdxByColumn[4][4] = {{0, 2, 8, 10}, {1, 3, 9, 11},{4, 6, 12, 14},{5, 7, 13, 15}};

Int getACPredMode(CWMIMBInfo *, COLORFORMAT);
Int getDCACPredMode(CWMImageStrCodec *, size_t);
Void updatePredInfo(CWMImageStrCodec* pSC, CWMIMBInfo *, size_t, COLORFORMAT);

Int AllocateCodingContextDec(struct CWMImageStrCodec *pSC, Int iNumContexts);
Void ResetCodingContext(CCodingContext *pContext);
Void getTilePos(CWMImageStrCodec* pSC, size_t mbX, size_t mbY);
Void InitZigzagScan(CCodingContext * pSC);
Int checkImageBuffer(CWMImageStrCodec *, size_t, size_t);

/*U32 log2(U32); */

/*DQUANT stuff */
EXTERN_C Void remapQP(CWMIQuantizer *, I32, Bool);
Int allocateTileInfo(CWMImageStrCodec *);
Void freeTileInfo(CWMImageStrCodec *);
Int allocateQuantizer(CWMIQuantizer * pQuantizer[MAX_CHANNELS], size_t, size_t);
Void freeQuantizer(CWMIQuantizer * pQuantizer[MAX_CHANNELS]);
Void setUniformQuantizer(CWMImageStrCodec *, size_t);
Void useDCQuantizer(CWMImageStrCodec *, size_t);
Void useLPQuantizer(CWMImageStrCodec *, size_t, size_t);
Void formatQuantizer(CWMIQuantizer * pQuantizer[MAX_CHANNELS], U8, size_t, size_t, Bool, Bool);
U8 dquantBits(U8);

#ifdef ARMOPT_BITIO
#define peekBit16   peekBits
#define flushBit16  flushBits
#define getBit16    getBits
#define getBit32    getBits
#define getBit16s   getBitsS
#define getBool16(pIO) getBits(pIO, 1)   

U32 peekBits(BitIOInfo* pIO, U32 cBits);
void flushBits(BitIOInfo* pIO, U32 cBits);
U32 getBits(BitIOInfo* pIO, U32 cBits);
U32 getBitsS(BitIOInfo* pIO, U32 cBits);
void flushToByte(BitIOInfo* pIO);
#endif /* ARMOPT_BITIO */

/*************************************************************************
    Bitio defines
*************************************************************************/
#define PEEKBIT16(pIO, cBits) \
    assert(0 <= (I32)cBits && cBits <= 16);\
    return (pIO->uiAccumulator >> (32 - cBits/* - pIO->cBitsUsed*/));

#define FLUSHBIT16(pIO, cBits) \
    assert(0 <= (I32)cBits && cBits <= 16);\
    assert((pIO->iMask & 1) == 0);\
    pIO->cBitsUsed += cBits;\
    pIO->pbCurrent = MASKPTR(pIO->pbCurrent + ((pIO->cBitsUsed >> 3)/* & 2*/), pIO->iMask);\
    pIO->cBitsUsed &= 16 - 1;\
    pIO->uiAccumulator = LOAD16(pIO->pbCurrent) << pIO->cBitsUsed;\
    return 0;
/*    pIO->uiAccumulator = LOAD16(pIO->pbCurrent) & ((U32)(-1) >> pIO->cBitsUsed); */

void OutputPerfTimerReport(CWMImageStrCodec *pState);

#if (defined(JXR_WIN32_INTRINSICS) && !defined(UNDER_CE)) || (defined(UNDER_CE) && defined(_ARM_))
/* WinCE ARM and Desktop x86 */
#else
/* other platform */
#ifdef _BIG__ENDIAN_
#define jxr_bswap32(x)  (x)
#else /* _BIG__ENDIAN_ */
U32 jxr_bswap32(U32 bits);
#endif /* _BIG__ENDIAN_ */
#endif

/* ---- perfTimer.h ---- */
/**@@@+++@@@@****************************************************************** */
/* */
/* Copyright (c) Microsoft Corp. */
/* All rights reserved. */
/* */
/* Redistribution and use in source and binary forms, with or without */
/* modification, are permitted provided that the following conditions are met: */
/* */
/* * Redistributions of source code must retain the above copyright notice, */
/*   this list of conditions and the following disclaimer. */
/* * Redistributions in binary form must reproduce the above copyright notice, */
/*   this list of conditions and the following disclaimer in the documentation */
/*   and/or other materials provided with the distribution. */
/* */
/* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" */
/* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE */
/* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE */
/* ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE */
/* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR */
/* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF */
/* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS */
/* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN */
/* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) */
/* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE */
/* POSSIBILITY OF SUCH DAMAGE. */
/* */
/**@@@---@@@@****************************************************************** */

#ifndef __PERFTIMER_H_
#define __PERFTIMER_H_

/**************************************************************************** */
/* Description */
/* */
/*   Performance timer API used to measure codec performance. The underlying */
/* implementation of this API may vary - from ANSI-C implementation via clock, */
/* Win32 implementation via QueryPerformanceCounter or GetProcessTimes. At */
/* present we only support one implementation of this PerfTimer "object". */
/* You choose the implementation by choosing which one of the many files */
/* to compile and link with your application. */
/**************************************************************************** */

#ifdef DISABLE_PERF_MEASUREMENT

#define PERFTIMER_ONLY(code)
#define PERFTIMER_NEW(fPerf, ppPerfTimer)
#define PERFTIMER_DELETE(fPerf, ppPerfTimer)
#define PERFTIMER_START(fPerf, pPerfTimer)
#define PERFTIMER_STOP(fPerf, pPerfTimer)
#define PERFTIMER_GETRESULTS(fPerf, pPerfTimer, pResults)
#define PERFTIMER_COPYSTARTTIME(fPerf, pDst, pSrc)
#define PERFTIMER_REPORT(fPerf, pCodec)

#else /* DISABLE_PERF_MEASUREMENT */

#define PERFTIMER_ONLY(code) code
#define PERFTIMER_NEW(fPerf, ppPerfTimer)    if (fPerf) {Bool b = b = PerfTimerNew(ppPerfTimer); assert(b);};
#define PERFTIMER_DELETE(fPerf, pPerfTimer)  if (fPerf) {PerfTimerDelete(pPerfTimer);};
#define PERFTIMER_START(fPerf, pPerfTimer)   if (fPerf) {Bool b = b = PerfTimerStart(pPerfTimer); assert(b);};
#define PERFTIMER_STOP(fPerf, pPerfTimer)    if (fPerf) {Bool b = b = PerfTimerStop(pPerfTimer); assert(b);};
#define PERFTIMER_GETRESULTS(fPerf, pPerfTimer, pResults) \
    if (fPerf) {Bool b = b = PerfTimerGetResults((pPerfTimer), (pResults)); assert(b);};
#define PERFTIMER_COPYSTARTTIME(fPerf, pDst, pSrc) \
    if (fPerf) {Bool b = b = PerfTimerCopyStartTime((pDst), (pSrc)); assert(b);};
#define PERFTIMER_REPORT(fPerf, pCodec) \
    if (fPerf) {OutputPerfTimerReport(pCodec);};
#endif /* DISABLE_PERF_MEASUREMENT */

/**************************************************************************** */
/* Data Types */
/**************************************************************************** */
typedef U64 PERFTIMERTIME;
typedef struct PERFTIMERRESULTS
{
    PERFTIMERTIME   iElapsedTime;       /* In nanoseconds or CPU cycles */
    PERFTIMERTIME   iTicksPerSecond;    /* Number of ticks per second (clock frequency) */
    PERFTIMERTIME   iZeroTimeIntervals; /* Number of zero-time intervals. */
        /* Presence of zero-time intervals may indicate insufficient clock precision */
} PERFTIMERRESULTS;

#define NANOSECONDS_PER_SECOND  1000000000


/**************************************************************************** */
/* Data Declarations */
/**************************************************************************** */
typedef enum
{
    CS_UNINIT,
    CS_RUNNING,
    CS_STOPPED,
} CLOCKSTATE;

typedef struct PERFTIMERSTATE
{
    CLOCKSTATE          eState;
    PERFTIMERTIME       iElapsedTime;
    PERFTIMERTIME       iPrevStartTime;
    PERFTIMERTIME       iZeroTimeIntervals;
} PERFTIMERSTATE;


/**************************************************************************** */
/* Functions and Macros */
/**************************************************************************** */
Bool PerfTimerNew(PERFTIMERSTATE **ppNewPerfTimer);
void PerfTimerDelete(PERFTIMERSTATE *pThisPerfTimer);
Bool PerfTimerStart(PERFTIMERSTATE *pThisPerfTimer);
Bool PerfTimerStop(PERFTIMERSTATE *pThisPerfTimer);
Bool PerfTimerGetResults(PERFTIMERSTATE *pThisPerfTimer,
                         PERFTIMERRESULTS *pPerfTimerResults);
Bool PerfTimerCopyStartTime(PERFTIMERSTATE *pDestPerfTimer,
                            PERFTIMERSTATE *pSrcPerfTimer);

#endif /* __PERFTIMER_H_ */

/* ---- decode.h ---- */
/**@@@+++@@@@****************************************************************** */
/* */
/* Copyright (c) Microsoft Corp. */
/* All rights reserved. */
/* */
/* Redistribution and use in source and binary forms, with or without */
/* modification, are permitted provided that the following conditions are met: */
/* */
/* * Redistributions of source code must retain the above copyright notice, */
/*   this list of conditions and the following disclaimer. */
/* * Redistributions in binary form must reproduce the above copyright notice, */
/*   this list of conditions and the following disclaimer in the documentation */
/*   and/or other materials provided with the distribution. */
/* */
/* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" */
/* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE */
/* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE */
/* ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE */
/* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR */
/* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF */
/* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS */
/* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN */
/* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) */
/* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE */
/* POSSIBILITY OF SUCH DAMAGE. */
/* */
/**@@@---@@@@****************************************************************** */

#ifndef WMI_DECODE_H
#define WMI_DECODE_H

typedef struct CWMDecoderParameters {
    /** ROI decode **/
    Bool bDecodeFullFrame;
    Bool bDecodeFullWidth;

    /** thumbnail decode **/
    Bool bSkipFlexbits;
    size_t cThumbnailScale;    /* 1: cThumbnailScale thumbnail, only supports cThumbnailScale = 2^m for now */
    Bool bDecodeHP;
    Bool bDecodeLP;

    /* Region of interest decoding */
    size_t cROILeftX;
    size_t cROIRightX;
    size_t cROITopY;
    size_t cROIBottomY;

    /* table lookups for rotation and flip */
    size_t * pOffsetX;
    size_t * pOffsetY;
} CWMDecoderParameters;

Void predCBPDec(CWMImageStrCodec *, CCodingContext *);
Void predDCACDec(CWMImageStrCodec *);
Void predACDec(CWMImageStrCodec *);

Int dequantizeMacroblock(CWMImageStrCodec *);
Int invTransformMacroblock(CWMImageStrCodec * pSC);
Int invTransformMacroblock_alteredOperators_hard(CWMImageStrCodec * pSC);

Int DecodeMacroblockDC(CWMImageStrCodec * pSC, CCodingContext *pContext, Int iMBX, Int iMBY);
Int DecodeMacroblockLowpass(CWMImageStrCodec * pSC, CCodingContext *pContext, Int iMBX, Int iMBY);
Int DecodeMacroblockHighpass(CWMImageStrCodec * pSC, CCodingContext *pContext, Int iMBX, Int iMBY);

Int AdaptLowpassDec(struct CCodingContext *);
Int AdaptHighpassDec(struct CCodingContext *);

Void ResetCodingContextDec(CCodingContext *pContext);
Void FreeCodingContextDec(struct CWMImageStrCodec *pSC);

/*************************************************************************/
/* Inverse transform functions */
/* 2-point post filter for boundaries (only used in 420 UV DC subband) */
Void strPost2(PixelI *, PixelI *);

/* 2x2 post filter (only used in 420 UV DC subband) */
Void strPost2x2(PixelI *, PixelI *, PixelI *, PixelI *);

/** 4-point post filter for boundaries **/
Void strPost4(PixelI *, PixelI *, PixelI *, PixelI *);

/** data allocation in working buffer (first stage) **/

/** Y, 444 U and V **/
/**  0  1  2  3 **/
/** 32 33 34 35 **/
/** 64 65 66 67 **/
/** 96 97 98 99 **/

/** 420 U and V **/
/**   0   2   4   6 **/
/**  64  66  68  70 **/
/** 128 130 132 134 **/
/** 192 194 196 198 **/

/** 4x4 inverse DCT for first stage **/
Void strIDCT4x4FirstStage(PixelI *);
Void strIDCT4x4Stage1(PixelI*);
Void strIDCT4x4FirstStage420UV(PixelI *);

/** 4x4 post filter for first stage **/
Void strPost4x4FirstStage(PixelI *);
Void strPost4x4Stage1Split(PixelI*, PixelI*, Int, Int, Bool);
Void strPost4x4Stage1(PixelI*, Int, Int, Bool);
Void strPost4x4Stage1Split_alternate(PixelI*, PixelI*, Int);
Void strPost4x4Stage1_alternate(PixelI*, Int);
/*Void strPost4x4Stage1Split_420(PixelI*, PixelI*); */
/*Void strPost4x4Stage1_420(PixelI*); */

Void strPost4x4FirstStage420UV(PixelI *);

/** data allocation in working buffer (second stage)**/

/** Y, 444 U and V **/
/**   0   4   8  12 **/
/** 128 132 136 140 **/
/** 256 260 264 268 **/
/** 384 388 392 396 **/

/** 420 U and V **/
/**   0   8 **/
/** 256 264 **/

/** 4x4 invesr DCT for second stage **/
/*Void strIDCT4x4SecondStage(PixelI *); */
Void strIDCT4x4Stage2(PixelI*);
Void strNormalizeDec(PixelI*, Bool);
Void strDCT2x2dnDec(PixelI *, PixelI *, PixelI *, PixelI *);

/** 4x4 post filter for second stage **/
Void strPost4x4SecondStage(PixelI *);
Void strPost4x4Stage2Split(PixelI*, PixelI*);
Void strPost4x4Stage2Split_alternate(PixelI*, PixelI*);

/** Huffman decode related defines **/
#define HUFFMAN_DECODE_ROOT_BITS_LOG    3
#define HUFFMAN_DECODE_ROOT_BITS    (5)   

Int getHuff(const short *pDecodeTable, BitIOInfo* pIO);

#endif /* WMI_DECODE_H */


/* ---- strTransform.h ---- */
/**@@@+++@@@@****************************************************************** */
/* */
/* Copyright (c) Microsoft Corp. */
/* All rights reserved. */
/* */
/* Redistribution and use in source and binary forms, with or without */
/* modification, are permitted provided that the following conditions are met: */
/* */
/* * Redistributions of source code must retain the above copyright notice, */
/*   this list of conditions and the following disclaimer. */
/* * Redistributions in binary form must reproduce the above copyright notice, */
/*   this list of conditions and the following disclaimer in the documentation */
/*   and/or other materials provided with the distribution. */
/* */
/* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" */
/* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE */
/* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE */
/* ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE */
/* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR */
/* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF */
/* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS */
/* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN */
/* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) */
/* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE */
/* POSSIBILITY OF SUCH DAMAGE. */
/* */
/**@@@---@@@@****************************************************************** */

#ifndef WMI_STRTRANSFORM_H
#define WMI_STRTRANSFORM_H

/* (merged) #include "windowsmediaphoto.h" */

#define COMPUTE_CORNER_PRED_DIFF(a, b) (*(a) -= (b))
#define COMPUTE_CORNER_PRED_ADD(a, b) (*(a) += (b))

/** 2x2 foward DCT == 2x2 inverse DCT **/
Void strDCT2x2dn(PixelI *, PixelI *, PixelI *, PixelI *);
Void strDCT2x2up(PixelI *, PixelI *, PixelI *, PixelI *);
Void FOURBUTTERFLY_HARDCODED1(PixelI *p);

/** 2x2 dct of a group of 4**/
#define FOURBUTTERFLY(p, i00, i01, i02, i03, i10, i11, i12, i13,\
    i20, i21, i22, i23, i30, i31, i32, i33)		                \
    strDCT2x2dn(&p[i00], &p[i01], &p[i02], &p[i03]);			\
    strDCT2x2dn(&p[i10], &p[i11], &p[i12], &p[i13]);			\
    strDCT2x2dn(&p[i20], &p[i21], &p[i22], &p[i23]);			\
    strDCT2x2dn(&p[i30], &p[i31], &p[i32], &p[i33])

#endif /* WMI_STRTRANSFORM_H */

/* no console output (and no stdio in the NT 3.1 build) */
int jxr_nofprint(void *fp, const char *f, ...);
#undef printf
#undef fprintf
#define printf jxr_noprint
#define fprintf jxr_nofprint
#endif
