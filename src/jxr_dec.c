/* jxr_dec.c - jxrlib JPEG XR decoder (see jxr_int.h) */
#include "jxr_int.h"
/* ---- decode.c ---- */
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
/******************************************************************************

Module Name:
    decode.c 
    
Abstract:    
    Defines the entry point for the console application.

Author:

Revision History:
*******************************************************************************/
/* (merged) #include "strcodec.h" */
/* (merged) #include "decode.h" */

#ifdef MEM_TRACE
#define TRACE_MALLOC    1
#define TRACE_NEW       0
#define TRACE_HEAP      0
/* (merged) #include "memtrace.h" */
#endif

/******************************************************************
Free Adaptive Huffman Table
******************************************************************/
static Void CleanAH(CAdaptiveHuffman **ppAdHuff)
{
    CAdaptiveHuffman *pAdHuff;
    
    if (NULL != ppAdHuff) {
        pAdHuff = *ppAdHuff;
        if (NULL != pAdHuff) {
            free(pAdHuff);
        }
        *ppAdHuff = NULL;
    }
}

/* CleanAHDec (unused) removed */

/*************************************************************************
    Initialize an adaptive huffman table
*************************************************************************/
static Int InitializeAH(CAdaptiveHuffman **ppAdHuff, Int iSym)
{
    Int iMemStatus = 0;

    CAdaptiveHuffman *pAdHuff = Allocate(iSym, DECODER);
    if (pAdHuff == NULL) {
        iMemStatus = -1;    /* out of memory */
        goto ErrorExit;
    }

    /*Adapt(pAdHuff, bFixedTables); */
    /*InitHuffman(pAdHuff->m_pHuffman); */
    /*if (ICERR_OK != initHuff(pAdHuff->m_pHuffman, 1, pAdHuff->m_pTable, NULL)) { */
    /*    goto ErrorExit; */
    /*} */
    *ppAdHuff = pAdHuff;
    return ICERR_OK;

ErrorExit:
    if (pAdHuff) {
        free(pAdHuff);
    }
    *ppAdHuff = NULL;
    if (-1 == iMemStatus) {
        printf("Insufficient memory to init decoder.\n");
    }
    return ICERR_ERROR;
}


/*************************************************************************
    Context allocation
*************************************************************************/
Int AllocateCodingContextDec(CWMImageStrCodec *pSC, Int iNumContexts)
{
    Int i, iCBPSize, k;
    static const Int aAlphabet[] = {5,4,8,7,7,  12,6,6,12,6,6,7,7,  12,6,6,12,6,6,7,7};

    if (iNumContexts > MAX_TILES || iNumContexts < 1)  /* only between 1 and MAX_TILES allowed */
        return ICERR_ERROR;

    if (pSC == NULL)
        return ICERR_ERROR;

    pSC->m_pCodingContext = malloc (iNumContexts * sizeof (CCodingContext));
    if (pSC->m_pCodingContext == NULL) {
        pSC->cNumCodingContext = 0;
        return ICERR_ERROR;
    }
    memset (pSC->m_pCodingContext, 0, iNumContexts * sizeof (CCodingContext));

    pSC->cNumCodingContext = iNumContexts;
    iCBPSize = (pSC->m_param.cfColorFormat == Y_ONLY || pSC->m_param.cfColorFormat == NCOMPONENT
        || pSC->m_param.cfColorFormat == CMYK) ? 5 : 9;

    /** allocate / initialize members **/
    for (i = 0; i < iNumContexts; i++) {
        CCodingContext *pContext = &(pSC->m_pCodingContext[i]);

        /** allocate adaptive Huffman encoder **/    
        if (InitializeAH(&pContext->m_pAdaptHuffCBPCY, iCBPSize) != ICERR_OK) {
            return ICERR_ERROR;
        }
        if (InitializeAH(&pContext->m_pAdaptHuffCBPCY1, 5) != ICERR_OK) {
            return ICERR_ERROR;
        }

        for(k = 0; k < NUMVLCTABLES; k ++){
            if (InitializeAH(&pContext->m_pAHexpt[k], aAlphabet[k]) != ICERR_OK) {
                return ICERR_ERROR;
            }
        }

        ResetCodingContextDec(pContext);
    }

    return ICERR_OK;
}

/*************************************************************************
    Context reset on encoder
*************************************************************************/
Void ResetCodingContextDec(CCodingContext *pContext)
{
    Int k;
    /** set flags **/
    pContext->m_pAdaptHuffCBPCY->m_bInitialize = FALSE;
    pContext->m_pAdaptHuffCBPCY1->m_bInitialize = FALSE;
    for(k = 0; k < NUMVLCTABLES; k ++)
        pContext->m_pAHexpt[k]->m_bInitialize = FALSE;

    /* reset VLC tables */
    AdaptLowpassDec (pContext);
    AdaptHighpassDec (pContext);

    /* reset zigzag patterns, totals */
    InitZigzagScan(pContext);
    /* reset bit reduction and cbp models */
    ResetCodingContext(pContext);
}

/*************************************************************************
    Context deletion
*************************************************************************/
Void FreeCodingContextDec(CWMImageStrCodec *pSC)
{
    Int iContexts = (Int)(pSC->cNumCodingContext), i, k;

    if (iContexts > 0 && pSC->m_pCodingContext) {

        for (i = 0; i < iContexts; i++) {
            CCodingContext *pContext = &(pSC->m_pCodingContext[i]);
            CleanAH (&pContext->m_pAdaptHuffCBPCY);
            CleanAH (&pContext->m_pAdaptHuffCBPCY1);
            for (k = 0; k < NUMVLCTABLES; k++)
                CleanAH (&pContext->m_pAHexpt[k]);
        }
        free (pSC->m_pCodingContext);
    }
}


#undef TRACE_MALLOC
#undef TRACE_NEW
#undef TRACE_HEAP
/* ---- postprocess.c ---- */
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

/* (merged) #include "windowsmediaphoto.h" */
/* (merged) #include "strcodec.h" */

Void smoothMB(PixelI * p1, PixelI * p0, PixelI * q0, PixelI * q1)
{
    /*  p1 p0 | q0 q1 */
    PixelI delta = ((((*q0 - *p0) << 2) + (*p1 - *q1)) >> 3);
    
    *q0 -= delta;
    *p0 += delta;
}

Void smooth(PixelI * p2, PixelI * p1, PixelI * p0, PixelI * q0, PixelI * q1, PixelI * q2)
{
    /*    p2 p1 p0 | q0 q1 q2 */
    PixelI delta = ((((*q0 - *p0) << 2) + (*p1 - *q1)) >> 3);

    *q0 -= delta;
    *p0 += delta;

    *p1 = (*p1 >> 1) + ((*p0 + *p2) >> 2);
    *q1 = (*q1 >> 1) + ((*q0 + *q2) >> 2);
}

Int initPostProc(struct tagPostProcInfo * strPostProcInfo[MAX_CHANNELS][2], size_t mbWidth, size_t iNumChannels)
{
    size_t i, j, k, l;
    Bool b32bit = sizeof(int) == 4;
    
    for(j = 0; j < iNumChannels; j ++){
        for(i = 0; i < 2; i ++){
            /* 2 more are allocated to avoid boundary check */
            if(b32bit) /* integer overlow/underflow check for 32-bit system */
                if((((mbWidth + 2) >> 16) * sizeof(struct tagPostProcInfo)) & 0xffff0000)
                    return ICERR_ERROR;
            strPostProcInfo[j][i] = (struct tagPostProcInfo *)malloc((mbWidth + 2) * sizeof(struct tagPostProcInfo));
            assert(strPostProcInfo[j][i] != NULL);
            if(strPostProcInfo[j][i] == NULL){
                return ICERR_ERROR;
            }
            strPostProcInfo[j][i] ++;
            
            /* initialize out-of-bound MBs as bumpy (no post at all) to avoid boundary check */
            /* left boundary */
            strPostProcInfo[j][i][-1].ucMBTexture = 3;
            for(l = 0; l < 4; l ++){
                for(k = 0; k < 4; k ++){
                    strPostProcInfo[j][i][-1].ucBlockTexture[l][k] = 3;
                }
            }
            /* right boundary */
            strPostProcInfo[j][i][mbWidth] = strPostProcInfo[j][i][-1];
        }
    }

    return ICERR_OK;
}

Void termPostProc(struct tagPostProcInfo * strPostProcInfo[MAX_CHANNELS][2], size_t iNumChannels)
{
    size_t i, j;

    for(j = 0; j < iNumChannels; j ++){
        for(i = 0; i < 2; i ++){
            if(strPostProcInfo[j][i] != NULL){
                free(strPostProcInfo[j][i] - 1);
            }
        }
    }
}

Void slideOneMBRow(struct tagPostProcInfo * strPostProcInfo[MAX_CHANNELS][2], size_t iNumChannels, size_t mbWidth, Bool top, Bool bottom)
{
    size_t i, j;
    struct tagPostProcInfo * bar;

    for(i = 0; i < iNumChannels; i ++){
        /* swap previous row and current row */
        bar = strPostProcInfo[i][0];
        strPostProcInfo[i][0] = strPostProcInfo[i][1];
        strPostProcInfo[i][1] = bar;

        if(top){ /* if top row, previous row is out of boundary */
            for(j = 0; j < mbWidth; j ++){
                strPostProcInfo[i][0][j] = strPostProcInfo[i][0][-1]; /* set as bumpy */
            }
        }
        
        if(bottom){ /* if bottom bottom row, set current row of MBs (out of boundary) as bumpy */
            for(j = 0; j < mbWidth; j ++){
                strPostProcInfo[i][1][j] = strPostProcInfo[i][1][-1]; /* set as bumpy */
            }
        }
    }
}

/* get DC and texture infomation right before transform */
Void updatePostProcInfo(struct tagPostProcInfo * strPostProcInfo[MAX_CHANNELS][2], PixelI * pMB, size_t mbX, size_t cc)
{
    size_t i, j;
    struct tagPostProcInfo * pMBInfo = strPostProcInfo[cc][1] + mbX;

    /* DC of MB */
    pMBInfo->iMBDC = pMB[0];

    /* texture of MB */
    pMBInfo->ucMBTexture = 0; /* smooth */
    for(i = 16; i < 256; i += 16){
        if(pMB[i] != 0){
            pMBInfo->ucMBTexture = 3; /* bumpy */
            break;
        }
    }

    /* DCs of blocks not available yet, will collect after demacroblocking */

    /* textures of blocks */
    for(j = 0; j < 4; j ++)
        for(i = 0; i < 4; i ++){
            PixelI * p = pMB + i * 64 + j * 16;
            size_t k;

            for(k = 1, pMBInfo->ucBlockTexture[j][i] = 0; k < 16; k ++){
                if(p[k] != 0){
                    pMBInfo->ucBlockTexture[j][i] = 3;
                    break;
                }
            }
        }
}

/* demacroblock critirion: two MBs have same texture other than bumpy and DCs differ less than 1 */
#define DMB(a, b) (a->ucMBTexture + b->ucMBTexture == 0) && (abs(a->iMBDC - b->iMBDC) <= threshold)

/* demacroblock and get DCs of blocks */
Void postProcMB(struct tagPostProcInfo * strPostProcInfo[MAX_CHANNELS][2], PixelI * p0, PixelI * p1, size_t mbX, size_t cc, Int threshold)
{
    /* 4 MBs involved, current MB is d, we have 4 2-pixel boundary segments */
    /*    |     */
    /*  a | b   */
    /* - - + +  */
    /*  c ! d   */
    /*    !     */
    struct tagPostProcInfo * pMBb = strPostProcInfo[cc][0] + mbX, * pMBa = pMBb - 1, * pMBd = strPostProcInfo[cc][1] + mbX, * pMBc = pMBd - 1;

    /* demacroblock segment -- */
   if(DMB(pMBa, pMBc)){
        smoothMB(p0 - 256 + 10 * 16, p0 - 256 + 11 * 16, p1 - 256 +  8 * 16, p1 - 256 +  9 * 16);
        smoothMB(p0 - 256 + 14 * 16, p0 - 256 + 15 * 16, p1 - 256 + 12 * 16, p1 - 256 + 13 * 16);
    }

    /* demacroblock segment ++ */
    if(DMB(pMBb, pMBd)){
        smoothMB(p0 + 2 * 16, p0 + 3 * 16, p1 + 0 * 16, p1 + 1 * 16);
        smoothMB(p0 + 6 * 16, p0 + 7 * 16, p1 + 4 * 16, p1 + 5 * 16);
    }

    /* demacroblock segment | */
    if(DMB(pMBa, pMBb)){
        smoothMB(p0 - 256 + 10 * 16, p0 - 256 + 14 * 16, p0 + 2 * 16, p0 + 6 * 16);
        smoothMB(p0 - 256 + 11 * 16, p0 - 256 + 15 * 16, p0 + 3 * 16, p0 + 7 * 16);
    }

    /* demacroblock segment ! */
    if(DMB(pMBc, pMBd)){
        smoothMB(p1 - 256 + 8 * 16, p1 - 256 + 12 * 16, p1 + 0 * 16, p1 + 4 * 16);
        smoothMB(p1 - 256 + 9 * 16, p1 - 256 + 13 * 16, p1 + 1 * 16, p1 + 5 * 16);
    }

    /* update DCs of blocks */
    /* MB d */
    pMBd->iBlockDC[0][0] = p1[0 * 16];
    pMBd->iBlockDC[0][1] = p1[4 * 16];
    pMBd->iBlockDC[1][0] = p1[1 * 16];
    pMBd->iBlockDC[1][1] = p1[5 * 16];
    
    /* MB b */
    pMBb->iBlockDC[2][0] = p0[2 * 16];
    pMBb->iBlockDC[2][1] = p0[6 * 16];
    pMBb->iBlockDC[3][0] = p0[3 * 16];
    pMBb->iBlockDC[3][1] = p0[7 * 16];

    /* MB c */
    pMBc->iBlockDC[0][2] = p1[ 8 * 16 - 256];
    pMBc->iBlockDC[0][3] = p1[12 * 16 - 256];
    pMBc->iBlockDC[1][2] = p1[ 9 * 16 - 256];
    pMBc->iBlockDC[1][3] = p1[13 * 16 - 256];

    /* MB a */
    pMBa->iBlockDC[2][2] = p0[10 * 16 - 256];
    pMBa->iBlockDC[2][3] = p0[14 * 16 - 256];
    pMBa->iBlockDC[3][2] = p0[11 * 16 - 256];
    pMBa->iBlockDC[3][3] = p0[15 * 16 - 256];
}

/* deblock and destair blocks */
/* 4 MBs involved, need to process 16 blocks of a */
/*    |     */
/*  a | b   */
/* - - - -  */
/*  c | d   */
/*    |     */
Void postProcBlock(struct tagPostProcInfo * strPostProcInfo[MAX_CHANNELS][2], PixelI * p0, PixelI * p1, size_t mbX, size_t cc, Int threshold)
{
    size_t i, j, k;
    Int dc[5][5];
    U8 texture[5][5];
    struct tagPostProcInfo * pMBb = strPostProcInfo[cc][0] + mbX, * pMBa = pMBb - 1, * pMBd = strPostProcInfo[cc][1] + mbX, * pMBc = pMBd - 1;
    PixelI * pc, * pt;

    /* copy DC and Texture info, can be optimized out */
    for(j = 0; j < 4; j ++){
        /* from MB a */
        for(i = 0; i < 4; i ++){
            dc[j][i] = pMBa->iBlockDC[j][i];
            texture[j][i] = pMBa->ucBlockTexture[j][i];
        }
        
        /* 4 blocks from MB c */
        dc[4][j] = pMBc->iBlockDC[0][j];
        texture[4][j] = pMBc->ucBlockTexture[0][j];

        /* 4 blocks from MB b */
        dc[j][4] = pMBb->iBlockDC[j][0];
        texture[j][4] = pMBb->ucBlockTexture[j][0];
    }
    /* 1 block from MB d */
    dc[4][4] = pMBd->iBlockDC[0][0];
    texture[4][4] = pMBd->ucBlockTexture[0][0];

    /* block boundaries */
    /*     | */
    /*     | */
    /*  ---  */

    for(j = 0; j < 4; j ++){
        for(i = 0; i < 4; i ++){
            pc = p0 - 256 + i * 64 + j * 16;

            /* deblock */
            if(texture[j][i] + texture[j + 1][i] < 3 && abs(dc[j][i] - dc[j + 1][i]) <= threshold){
                /* smooth horizontal boundary ---- */
                pt = (j < 3 ? pc + 16 : p1 - 256 + i * 64);
                for(k = 0; k < 4; k ++){
                    smooth(pc + idxCC[1][k], pc + idxCC[2][k], pc + idxCC[3][k], pt + idxCC[0][k], pt + idxCC[1][k], pt + idxCC[2][k]);
                }
            }

            /* two horizontally adjacent blocks have same texture and similiar DCs */
            if(texture[j][i] + texture[j][i + 1] < 3 && abs(dc[j][i] - dc[j][i + 1]) <= threshold){
                /* smooth vertical boundary | */
                pt = pc + 64;
                for(k = 0; k < 4; k ++){
                    smooth(pc + idxCC[k][1], pc + idxCC[k][2], pc + idxCC[k][3], pt + idxCC[k][0], pt + idxCC[k][1], pt + idxCC[k][2]);
                }
            }
        }
    }
}


#undef DMB
/* ---- segdec.c ---- */
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

/* (merged) #include "strcodec.h" */
/* (merged) #include "decode.h" */

#ifdef MEM_TRACE
#define TRACE_MALLOC    1
#define TRACE_NEW       0
#define TRACE_HEAP      0
/* (merged) #include "memtrace.h" */
#endif

extern const int dctIndex[3][16];
extern const int blkOffset[16];
extern const int blkOffsetUV[4];
static Int DecodeSignificantAbsLevel (struct CAdaptiveHuffman *pAHexpt, BitIOInfo* pIO);

/*#undef X86OPT_INLINE */

#ifdef X86OPT_INLINE
#define _FORCEINLINE __forceinline
#else /* X86OPT_INLINE */
#define _FORCEINLINE
#endif /* X86OPT_INLINE */

/*================================================================ */
/* Memory access functions */
/*================================================================ */
static U32 _FORCEINLINE _load4(void* pv)
{
#ifdef _BIG__ENDIAN_
    return (*(U32*)pv);
#else /* _BIG__ENDIAN_ */
#if defined(_M_IA64) || defined(_ARM_)
    U32  v;
    v = ((U16 *) pv)[0];
    v |= ((U32)((U16 *) pv)[1]) << 16;
    return jxr_bswap32(v);
#else /* _M_IA64 */
    return jxr_bswap32(*(U32*)pv);
#endif /* _M_IA64 */
#endif /* _BIG__ENDIAN_ */
}

static _FORCEINLINE U32 _peekBit16(BitIOInfo* pIO, U32 cBits)
{
    PEEKBIT16(pIO, cBits);
    /* masking is not needed here because shift of unsigned int is implemented as a logical shift (SHR)! */
}

#define LOAD16 _load4
static _FORCEINLINE U32 _flushBit16(BitIOInfo* pIO, U32 cBits)
{
    FLUSHBIT16(pIO, cBits);
}

static _FORCEINLINE U32 _getBit16(BitIOInfo* pIO, U32 cBits)
{
    U32 uiRet = _peekBit16(pIO, cBits);
    _flushBit16(pIO, cBits);

    return uiRet;
}

#define SIGN_BIT(TypeOrValue) (((UInt) 1) << (8 * sizeof (TypeOrValue) - 1))
/***********************************************************************************************************
  Huffman decode (input is a fully built Huffman table)
***********************************************************************************************************/
Int getHuff(const short *pDecodeTable, BitIOInfo* pIO)
{
    Int iSymbol, iSymbolHuff;
    iSymbol = pDecodeTable[peekBit16(pIO, HUFFMAN_DECODE_ROOT_BITS)];

    flushBit16(pIO, iSymbol < 0 ? HUFFMAN_DECODE_ROOT_BITS : iSymbol & ((1 << HUFFMAN_DECODE_ROOT_BITS_LOG) - 1));
	iSymbolHuff = iSymbol >> HUFFMAN_DECODE_ROOT_BITS_LOG;

	if (iSymbolHuff < 0) {
		iSymbolHuff = iSymbol;
        while ((iSymbolHuff = pDecodeTable[iSymbolHuff + SIGN_BIT (pDecodeTable[0]) + getBit16(pIO, 1)]) < 0);
	}
    return (iSymbolHuff);
}

#if 1
static _FORCEINLINE U32 _getBool16(BitIOInfo* pIO)
{
    U32 uiRet = pIO->uiAccumulator >> 31;/*_peekBit16(pIO, 1); */
    /*_flushBit16(pIO, 1); */
    pIO->cBitsUsed++;
    if (pIO->cBitsUsed < 16) {
        pIO->uiAccumulator <<= 1;
    }
    else {
        pIO->pbCurrent = MASKPTR(pIO->pbCurrent + ((pIO->cBitsUsed >> 3)/* & 2*/), pIO->iMask);
        pIO->cBitsUsed &= 16 - 1;
        pIO->uiAccumulator = LOAD16(pIO->pbCurrent) << pIO->cBitsUsed;
    }

    return uiRet;
}

static _FORCEINLINE I32 _getSign(BitIOInfo* pIO)
{
    I32 uiRet = (int) pIO->uiAccumulator >> 31;/*_peekBit16(pIO, 1); */
    /*_flushBit16(pIO, 1); */
    pIO->cBitsUsed++;
    if (pIO->cBitsUsed < 16) {
        pIO->uiAccumulator <<= 1;
    }
    else {
        pIO->pbCurrent = MASKPTR(pIO->pbCurrent + ((pIO->cBitsUsed >> 3)/* & 2*/), pIO->iMask);
        pIO->cBitsUsed &= 16 - 1;
        pIO->uiAccumulator = LOAD16(pIO->pbCurrent) << pIO->cBitsUsed;
    }

    return uiRet;
}
#else
#define _getBool16(x)   _getBit16((x),1)
#define _getSign(x)   (-_getBit16((x),1))
#endif

/** this function returns cBits if zero is read, or a signed value if first cBits are not all zero **/
static _FORCEINLINE I32 _getBit16s(BitIOInfo* pIO, U32 cBits)
{
    I32 iRet = (I32)_peekBit16(pIO, cBits + 1);
    iRet = ((iRet >> 1) ^ (-(iRet & 1))) + (iRet & 1);
    _flushBit16(pIO, cBits + (iRet != 0));
    return iRet;
}

/*************************************************************************
    Huffman decoding with short tables
*************************************************************************/
static _FORCEINLINE Int _getHuffShort(const short *pDecodeTable, BitIOInfo* pIO)
{
    Int iSymbol = pDecodeTable[_peekBit16(pIO, HUFFMAN_DECODE_ROOT_BITS)];
    assert(iSymbol >= 0);
    /* for some strange reason, inlining flushBit doesn't work well */
    flushBit16(pIO, iSymbol & ((1 << HUFFMAN_DECODE_ROOT_BITS_LOG) - 1));
    return (iSymbol >> HUFFMAN_DECODE_ROOT_BITS_LOG);
}
/*************************************************************************
    Adapt + Huffman init
*************************************************************************/
static Int AdaptDecFixed (CAdaptiveHuffman *pAH)
{
    AdaptDiscriminant (pAH);
    return ICERR_OK;
}

/*************************************************************************
    DecodeCBP
*************************************************************************/
static Void DecodeCBP(CWMImageStrCodec * pSC, CCodingContext *pContext)
{
    BitIOInfo* pIO = pContext->m_pIOAC;
    const COLORFORMAT cf = pSC->m_param.cfColorFormat;
    const Int iChannel = (cf == NCOMPONENT || cf == CMYK) ? (Int) pSC->m_param.cNumChannels : 1;
    Int iCBPCY, iCBPCU , iCBPCV;
    Int k, iBlock, i;
    Int iNumCBP;
    Bool bIsChroma;
    CAdaptiveHuffman *pAHCBP = pContext->m_pAdaptHuffCBPCY;
    CAdaptiveHuffman *pAHCBP1 = pContext->m_pAdaptHuffCBPCY1;
    CAdaptiveHuffman *pAHex1 = pContext->m_pAHexpt[1];
        
    readIS_L1(pSC, pIO);

    for (i = 0; i < iChannel; i++) {

        iCBPCY = iCBPCU = iCBPCV = 0;
        iNumCBP = _getHuffShort(pAHCBP1->m_hufDecTable, pIO);
        pAHCBP1->m_iDiscriminant += pAHCBP1->m_pDelta[iNumCBP];

        switch (iNumCBP) {
            case 2:
                iNumCBP = _getBit16(pIO, 2);
                if (iNumCBP == 0)
                    iNumCBP = 3;
                else if (iNumCBP == 1)
                    iNumCBP = 5;
                else {
                    static const Int aTab[] = { 6, 9, 10, 12 };
                    iNumCBP = aTab[iNumCBP * 2 + _getBool16 (pIO) - 4];
                }
                break;
            case 1:
                iNumCBP = 1 << _getBit16(pIO, 2);
                break;
            case 3:
                iNumCBP = 0xf ^ (1 << _getBit16(pIO, 2));
                break;
            case 4:
                iNumCBP = 0xf;
        }

        for (iBlock = 0; iBlock < 4; iBlock++) {
            if (iNumCBP & (1 << iBlock)) {
                static const UInt gFLC0[] = { 0,2,1,2,2,0 };
                static const UInt gOff0[] = { 0,4,2,8,12,1 };
                static const UInt gOut0[] = { 0,15,3,12, 1,2,4,8, 5,6,9,10, 7,11,13,14 };
                Int iNumBlockCBP = getHuff(pAHCBP->m_hufDecTable, pIO);
                unsigned int val = (unsigned int) iNumBlockCBP + 1, iCode1;

                pAHCBP->m_iDiscriminant += pAHCBP->m_pDelta[iNumBlockCBP];
                iNumBlockCBP = 0;

                if (val >= 6) { /* chroma present */
                    if (_getBool16 (pIO)) {
                        iNumBlockCBP = 0x10;
                    }
                    else if (_getBool16 (pIO)) {
                        iNumBlockCBP = 0x20;
                    }
                    else {
                        iNumBlockCBP = 0x30;
                    }
                    if (val == 9) {
                        if (_getBool16 (pIO)) {
                            /* do nothing */
                        }
                        else if (_getBool16 (pIO)) {
                            val = 10;
                        }
                        else {
                            val = 11;
                        }
                    }
                    val -= 6;
                }
                iCode1 = gOff0[val];
                if (gFLC0[val]) {
                    iCode1 += _getBit16(pIO, gFLC0[val]);
                }
                iNumBlockCBP += gOut0[iCode1];

                switch (cf) {
                    case YUV_444:
                        iCBPCY |= ((iNumBlockCBP & 0xf) << (iBlock * 4));
                        for (k = 0; k < 2; k++) {
                            bIsChroma = ((iNumBlockCBP>>(k+4)) & 0x01);
                            if (bIsChroma) { /* U is present in block */
                                Int iCode = _getHuffShort(pAHex1->m_hufDecTable, pIO);
                                switch (iCode) { 
                            case 1:
                                iCode = _getBit16(pIO, 2);
                                if (iCode == 0)
                                    iCode = 3;
                                else if (iCode == 1)
                                    iCode = 5;
                                else {
                                    static const Int aTab[] = { 6, 9, 10, 12 };
                                    iCode = aTab[iCode * 2 + _getBool16 (pIO) - 4];
                                }
                                break;
                            case 0:
                                iCode = 1 << _getBit16(pIO, 2);
                                break;
                            case 2:
                                iCode = 0xf ^ (1 << _getBit16(pIO, 2));
                                break;
                            case 3:
                                iCode = 0xf;
                                }
                                if (k == 0)
                                    iCBPCU |= (iCode << (iBlock * 4));
                                else 
                                    iCBPCV |= (iCode << (iBlock * 4));
                            }
                        }
                        break;

                    case YUV_420:
                        iCBPCY |= ((iNumBlockCBP & 0xf) << (iBlock * 4));
                        iCBPCU |= ((iNumBlockCBP >> 4) & 0x1) << (iBlock);
                        iCBPCV |= ((iNumBlockCBP >> 5) & 0x1) << (iBlock);
                        break;

                    case YUV_422:
                        iCBPCY |= ((iNumBlockCBP & 0xf) << (iBlock * 4));
                        for (k = 0; k < 2; k ++) {
                            Int iCode = 5;
                            const Int iShift[4] = {0, 1, 4, 5};
                            if((iNumBlockCBP >> (k + 4)) & 0x01) {
                                if(_getBool16(pIO)) {
                                    iCode = 1;
                                }
                                else if(_getBool16(pIO)){
                                    iCode = 4;
                                }
                                iCode <<= iShift[iBlock];
                                if(k == 0) iCBPCU |= iCode;
                                else iCBPCV |= iCode;
                            }
                        }
                        break;

                    default:
                        iCBPCY |= (iNumBlockCBP << (iBlock * 4));
                }
            }
        }

        pSC->MBInfo.iDiffCBP[i] = iCBPCY;
        if (cf == YUV_420 || cf == YUV_444 || cf == YUV_422) {
            pSC->MBInfo.iDiffCBP[1] = iCBPCU;
            pSC->MBInfo.iDiffCBP[2] = iCBPCV;
        }
    }
}

/*************************************************************************
    Experimental code -- decodeBlock
    SR = <0 1 2> == <last, nonsignificant, significant run>
    alphabet 12:
        pAHexpt[0] == <SR', SL, SR | first symbol>
    alphabet 6:
        pAHexpt[1] == <SR', SL | continuous>
        pAHexpt[2] == <SR', SL | continuous>
    alphabet 4:
        pAHexpt[3] == <SR', SL | 2 free slots> (SR may be last or insignificant only)
    alphabet f(run) (this can be extended to 6 contexts - SL and SR')
        pAHexpt[4] == <run | continuous>
    alphabet f(lev) (this can be extended to 9 contexts)
        pAHexpt[5-6] == <lev | continuous> first symbol
        pAHexpt[7-8] == <lev | continuous> condition on SRn no use
*************************************************************************/

Int _FORCEINLINE DecodeSignificantRun (Int iMaxRun, struct CAdaptiveHuffman *pAHexpt, BitIOInfo* pIO)
{
    Int iIndex;
    static const Int aRemap[] = {1,2,3,5,7,   1,2,3,5,7,   /*1,2,3,4,6,  */1,2,3,4,5 };
    Int iBin = gSignificantRunBin[iMaxRun];
    Int iRun = 0, iFLC = 0;

    if (iMaxRun < 5) {
        if (iMaxRun == 1) {
            return 1;
        }
        else if (_getBool16 (pIO)) {
            return 1;
        }
        else if (iMaxRun == 2 || _getBool16 (pIO)) {
            return 2;
        }
        else if (iMaxRun == 3 || _getBool16 (pIO)) {
            return 3;
        }
        return 4;
    }
    iIndex = _getHuffShort (pAHexpt->m_hufDecTable, pIO);
    iIndex += iBin * 5;
    iRun = aRemap[iIndex];
    iFLC = gSignificantRunFixedLength[iIndex];
    if (iFLC) {
        iRun += _getBit16 (pIO, iFLC);
    }
    return iRun;
}

#ifndef X86OPT_INLINE
static Void DecodeFirstIndex (Int *pIndex, struct CAdaptiveHuffman *pAHexpt, 
                      BitIOInfo* pIO)
#else
static __forceinline Void DecodeFirstIndex (Int *pIndex, struct CAdaptiveHuffman *pAHexpt, 
                      BitIOInfo* pIO)
#endif
{
    Int iIndex;
    iIndex = getHuff (pAHexpt->m_hufDecTable, pIO);
    pAHexpt->m_iDiscriminant += pAHexpt->m_pDelta[iIndex];
    pAHexpt->m_iDiscriminant1 += pAHexpt->m_pDelta1[iIndex];
    *pIndex = iIndex;
}

#ifndef X86OPT_INLINE
static Void DecodeIndex (Int *pIndex, Int iLoc, struct CAdaptiveHuffman *pAHexpt, 
                 BitIOInfo* pIO)
#else
static __forceinline Void DecodeIndex (Int *pIndex, Int iLoc,
                                       struct CAdaptiveHuffman *pAHexpt, BitIOInfo* pIO)
#endif
{
    Int iIndex;
    if (iLoc < 15) {
        iIndex = _getHuffShort (pAHexpt->m_hufDecTable, pIO);
        pAHexpt->m_iDiscriminant += pAHexpt->m_pDelta[iIndex];
        pAHexpt->m_iDiscriminant1 += pAHexpt->m_pDelta1[iIndex];
        *pIndex = iIndex;
    }
    else if (iLoc == 15) {
        if (_getBool16 (pIO) == 0) {
            iIndex = 0;
        }
        else if (_getBool16 (pIO) == 0) {
            iIndex = 2;
        }
        else {
            iIndex = 1 + 2 * _getBool16 (pIO);
        }
        *pIndex = iIndex;
    }
    else { /* if (iLoc == 16): deterministic */
        Int iSL = _getBit16 (pIO, 1/* + 1*/);
        *pIndex = iSL;/* >> 1; */
    }
}

static _FORCEINLINE Int DecodeBlock (Bool bChroma, Int *aLocalCoef, struct CAdaptiveHuffman **pAHexpt,
                        const Int iContextOffset, BitIOInfo* pIO, Int iLocation)
{
    Int iSR, iSRn, iIndex, iNumNonzero = 1, iCont, iSign;
    struct CAdaptiveHuffman **pAH1 = pAHexpt + iContextOffset + bChroma * 3;

    /** first symbol **/
    DecodeFirstIndex (&iIndex, /*&iSign, */pAH1[0], pIO);
    iSR = (iIndex & 1);
    iSRn = iIndex >> 2;

    iCont = iSR & iSRn;
    iSign = _getSign(pIO);

    if (iIndex & 2 /* iSL */) {
        aLocalCoef[1] = (DecodeSignificantAbsLevel (pAHexpt[6 + iContextOffset + iCont], pIO) ^ iSign) - iSign;
    }
    else {
        aLocalCoef[1] = (1 | iSign); /* 0 -> 1; -1 -> -1 */
    }
    aLocalCoef[0] = 0;
    if (iSR == 0) {
        aLocalCoef[0] = DecodeSignificantRun (15 - iLocation, pAHexpt[0], pIO);
    }
    iLocation += aLocalCoef[0] + 1;

    while (iSRn != 0) {
        iSR = iSRn & 1;
        aLocalCoef[iNumNonzero * 2] = 0;
        if (iSR == 0) {
            aLocalCoef[iNumNonzero * 2] = DecodeSignificantRun (15 - iLocation, pAHexpt[0], pIO);
        }
        iLocation += aLocalCoef[iNumNonzero * 2] + 1;
        DecodeIndex (&iIndex, /*&iSign, */iLocation, pAH1[iCont + 1], pIO);
        iSRn = iIndex >> 1;

        assert (iSRn >= 0 && iSRn < 3);
        iCont &= iSRn;  /** huge difference! **/
        iSign = _getSign(pIO);

        if (iIndex & 1 /* iSL */) {
            aLocalCoef[iNumNonzero * 2 + 1] = 
                (DecodeSignificantAbsLevel (pAHexpt[6 + iContextOffset + iCont], pIO) ^ iSign) - iSign;
        }
        else {
            aLocalCoef[iNumNonzero * 2 + 1] = (1 | iSign); /* 0 -> 1; -1 -> -1 (was 1 + (iSign * 2)) */
        }
        iNumNonzero++;
    }
    return iNumNonzero;
}

/*************************************************************************
    DecodeBlockHighpass : 
*************************************************************************/
static _FORCEINLINE Int DecodeBlockHighpass (const Bool bChroma, struct CAdaptiveHuffman **pAHexpt,
                       BitIOInfo* pIO, const Int iQP, Int *pCoef, CAdaptiveScan *pScan)
{
    const Int iContextOffset = CTDC + CONTEXTX;
    UInt  iLoc = 1;
    Int iSR, iSRn, iIndex, iNumNonzero = 1, iCont, iSign, iLevel;
    struct CAdaptiveHuffman **pAH1 = pAHexpt + iContextOffset + bChroma * 3;
    const CAdaptiveScan *pConstScan = (const CAdaptiveScan *) pScan;

    /** first symbol **/
    DecodeFirstIndex (&iIndex, /*&iSign, */pAH1[0], pIO);
    iSR = (iIndex & 1);
    iSRn = iIndex >> 2;

    iCont = iSR & iSRn;
    iSign = _getSign(pIO);

    iLevel = (iQP ^ iSign) - iSign;
    if (iIndex & 2 /* iSL */) {
        iLevel *= DecodeSignificantAbsLevel (pAHexpt[6 + iContextOffset + iCont], pIO);/* ^ iSign) - iSign; */
    }
    /*else { */
    /*    iLevel = (1 | iSign); // 0 -> 1; -1 -> -1 */
    /*} */
    if (iSR == 0) {
       iLoc += DecodeSignificantRun (15 - iLoc, pAHexpt[0], pIO);
    }
    iLoc &= 0xf;
	pCoef[pConstScan[iLoc].uScan] = (PixelI) iLevel;/*(PixelI)(iQP * iLevel); */
    pScan[iLoc].uTotal++;
	if (iLoc && pScan[iLoc].uTotal > pScan[iLoc - 1].uTotal) {
		CAdaptiveScan cTemp = pScan[iLoc];
		pScan[iLoc] = pScan[iLoc - 1];
		pScan[iLoc - 1] = cTemp;
    }
    iLoc = (iLoc + 1) & 0xf;
    /*iLoc++; */

    while (iSRn != 0) {
        iSR = iSRn & 1;
        if (iSR == 0) {
            iLoc += DecodeSignificantRun (15 - iLoc, pAHexpt[0], pIO);
            if (iLoc >= 16)
                return 16;
        }
        DecodeIndex (&iIndex, /*&iSign, */iLoc + 1, pAH1[iCont + 1], pIO);
        iSRn = iIndex >> 1;

        assert (iSRn >= 0 && iSRn < 3);
        iCont &= iSRn;  /** huge difference! **/
        iSign = _getSign(pIO);

        iLevel = (iQP ^ iSign) - iSign;
        if (iIndex & 1 /* iSL */) {
            iLevel *= DecodeSignificantAbsLevel (pAHexpt[6 + iContextOffset + iCont], pIO);/* ^ iSign) - iSign; */
            /*iLevel = (DecodeSignificantAbsLevel (pAHexpt[6 + iContextOffset + iCont], pIO) ^ iSign) - iSign; */
        }
        /*else { */
        /*    iLevel = (1 | iSign); // 0 -> 1; -1 -> -1 (was 1 + (iSign * 2)) */
        /*} */
       
	    pCoef[pConstScan[iLoc].uScan] = (PixelI) iLevel;/*(PixelI)(iQP * iLevel); */
        pScan[iLoc].uTotal++;
	    if (iLoc && pScan[iLoc].uTotal > pScan[iLoc - 1].uTotal) {
		    CAdaptiveScan cTemp = pScan[iLoc];
		    pScan[iLoc] = pScan[iLoc - 1];
		    pScan[iLoc - 1] = cTemp;
        }

        iLoc = (iLoc + 1) & 0xf;
        iNumNonzero++;
    }
    return iNumNonzero;
}

/*************************************************************************
    DecodeBlockAdaptive
*************************************************************************/
static _FORCEINLINE Int DecodeBlockAdaptive (Bool bNoSkip, Bool bChroma, CAdaptiveHuffman **pAdHuff,
                                BitIOInfo *pIO, BitIOInfo *pIOFL,
                                PixelI *pCoeffs, CAdaptiveScan *pScan,
                                const Int iModelBits, const Int iTrim, const Int iQP,
                                const Int *pOrder, const Bool bSkipFlexbits)
{
    /* const Int iLocation = 1; */
    /* const Int iContextOffset = CTDC + CONTEXTX; */
    Int kk, iNumNonzero = 0, iFlex = iModelBits - iTrim;

    if (iFlex < 0 || bSkipFlexbits)
        iFlex = 0;

    if (bNoSkip) {
        const Int iQP1 = (iQP << iModelBits);
        iNumNonzero = DecodeBlockHighpass (bChroma, pAdHuff, pIO, iQP1, pCoeffs, pScan);
    }
    if (iFlex) {
        UInt k;
        if (iQP + iTrim == 1) { /* only iTrim = 0, iQP = 1 is legal */
            assert (iTrim == 0);
            assert (iQP == 1);

            for (k = 1; k < 16; k++) {
                PixelI *pk = pCoeffs + pOrder[k];
                if (*pk < 0) {
                    Int fine = _getBit16(pIOFL, iFlex);
                    *pk -= (PixelI)(fine);
                }
                else if (*pk > 0) {
                    Int fine = _getBit16(pIOFL, iFlex);
                    *pk += (PixelI)(fine);
                }
                else {
                    *pk = (PixelI)(_getBit16s(pIOFL, iFlex));
                }
            }
        }
        else {
            const Int iQP1 = iQP << iTrim;
            for (k = 1; k < 16; k++) {
                kk = pCoeffs[pOrder[k]];
                if (kk < 0) {
                    Int fine = _getBit16(pIOFL, iFlex);
                    pCoeffs[pOrder[k]] -= (PixelI)(iQP1 * fine);
                }
                else if (kk > 0) {
                    Int fine = _getBit16(pIOFL, iFlex);
                    pCoeffs[pOrder[k]] += (PixelI)(iQP1 * fine);
                }
                else {
                    pCoeffs[pOrder[k]] = (PixelI)(iQP1 * _getBit16s(pIOFL, iFlex));
                }
            }
        }
    }

    return iNumNonzero;
}


/*************************************************************************
    GetCoeffs
*************************************************************************/
static _FORCEINLINE Int DecodeCoeffs (CWMImageStrCodec * pSC, CCodingContext *pContext,
                         Int iMBX, Int iMBY,
                         BitIOInfo* pIO, BitIOInfo *pIOFL)
{
    CWMITile * pTile = pSC->pTile + pSC->cTileColumn;
    const COLORFORMAT cf = pSC->m_param.cfColorFormat;
    const Int iChannels = (Int) pSC->m_param.cNumChannels;
    const Int iPlanes = (cf == YUV_420 || cf == YUV_422) ? 1 : iChannels;
    Int  iQP;
    CAdaptiveScan *pScan;
    PixelI  *pCoeffs;
    Int i, iBlock, iSubblock, iNBlocks = 4;
    Int iModelBits = pContext->m_aModelAC.m_iFlcBits[0];
    Int aLaplacianMean[2] = { 0, 0}, *pLM = aLaplacianMean + 0;
    const Int *pOrder = dctIndex[0];
    const Int iOrient = pSC->MBInfo.iOrientation;
    Bool bChroma = FALSE;

    Int iCBPCU = pSC->MBInfo.iCBP[1];
    Int iCBPCV = pSC->MBInfo.iCBP[2];
    Int iCBPCY = pSC->MBInfo.iCBP[0];

    UNREFERENCED_PARAMETER( iMBX );
    UNREFERENCED_PARAMETER( iMBY );

    /** set scan arrays and other MB level constants **/
    if (iOrient == 1) {
        pScan = pContext->m_aScanVert;
    }
    else {
        pScan = pContext->m_aScanHoriz;
    }

    if (cf == YUV_420) {
        iNBlocks = 6;
        iCBPCY += (iCBPCU << 16) + (iCBPCV << 20);
    }
    else if (cf == YUV_422) {
        iNBlocks = 8;
        iCBPCY += (iCBPCU << 16) + (iCBPCV << 24);
    }

    for (i = 0; i < iPlanes; i++) {
        Int iIndex = 0, iNumNonZero;

        if(pSC->WMISCP.sbSubband != SB_NO_FLEXBITS)
            readIS_L1(pSC, pIOFL);

        for (iBlock = 0; iBlock < iNBlocks; iBlock++) {

            readIS_L2(pSC, pIO);
            if (pIO != pIOFL)
                readIS_L2(pSC, pIOFL);

            iQP = (pSC->m_param.bTranscode ? 1 : pTile->pQuantizerHP[iPlanes > 1 ? i : (iBlock > 3 ? (cf == YUV_420 ? iBlock - 3 : iBlock / 2 - 1) : 0)][pSC->MBInfo.iQIndexHP].iQP);

            for (iSubblock = 0; iSubblock < 4; iSubblock++, iIndex++, iCBPCY >>= 1) {
                pCoeffs = pSC->p1MBbuffer[i] + blkOffset[iIndex & 0xf];

                /*if (iBlock < 4) {//(cf == YUV_444) { */
                    /*bBlockNoSkip = ((iTempCBPC & (1 << iIndex1)) != 0); */
                    /*pCoeffs = pSC->p1MBbuffer[iBlock >> 2] + blkOffset[iIndex & 0xf]; */
                /*} */
                /*else { */
                if (iBlock >= 4) {
                    if(cf == YUV_420) {
                        pCoeffs = pSC->p1MBbuffer[iBlock - 3] + blkOffsetUV[iSubblock];
                    }
                    else { /* YUV_422 */
                        pCoeffs = pSC->p1MBbuffer[1 + (1 & (iBlock >> 1))] + ((iBlock & 1) * 32) + blkOffsetUV_422[iSubblock];
                    }
                }

                /** read AC values **/
                assert (pSC->m_Dparam->bSkipFlexbits == 0 || pSC->WMISCP.bfBitstreamFormat == FREQUENCY || pSC->WMISCP.sbSubband == SB_NO_FLEXBITS);
                iNumNonZero = DecodeBlockAdaptive ((iCBPCY & 1), bChroma, pContext->m_pAHexpt,
                    pIO, pIOFL, pCoeffs, pScan, iModelBits, pContext->m_iTrimFlexBits,
                    iQP, pOrder, pSC->m_Dparam->bSkipFlexbits);
                if(iNumNonZero > 16) /* something is wrong! */
                    return ICERR_ERROR;
                /* shouldn't this be > 15? */
                (*pLM) += iNumNonZero;
            }
            if (iBlock == 3) {
                iModelBits = pContext->m_aModelAC.m_iFlcBits[1];
                pLM = aLaplacianMean + 1;
                bChroma = TRUE;
            }
        }

        iCBPCY = pSC->MBInfo.iCBP[(i + 1) & 0xf];
        assert (MAX_CHANNELS == 16);
    }

    /** update model at end of MB **/
    UpdateModelMB (cf, iChannels, aLaplacianMean, &(pContext->m_aModelAC));
    return ICERR_OK;
}

/*************************************************************************
    DecodeSignificantAbsLevel
*************************************************************************/
#ifndef X86OPT_INLINE
static Int DecodeSignificantAbsLevel (struct CAdaptiveHuffman *pAHexpt, BitIOInfo* pIO)
#else
static __forceinline Int DecodeSignificantAbsLevel (struct CAdaptiveHuffman *pAHexpt, BitIOInfo* pIO)
#endif
{
    UInt iIndex;
    Int iFixed, iLevel;
    static const Int aRemap[] = { 2, 3, 4, 6, 10, 14 };
    static const Int aFixedLength[] = { 0, 0, 1, 2, 2, 2 };

    iIndex = (UInt)getHuff (pAHexpt->m_hufDecTable, pIO);
    assert(iIndex <= 6);
    pAHexpt->m_iDiscriminant += pAHexpt->m_pDelta[iIndex];
    if (iIndex < 2) {
        iLevel = iIndex + 2; /* = aRemap[iIndex] */
    }
    else if (iIndex < 6) {
        iFixed = aFixedLength[iIndex];
        iLevel = aRemap[iIndex] + _getBit16 (pIO, iFixed);
    }
    else{
        iFixed = _getBit16 (pIO, 4) + 4;
        if (iFixed == 19) {
            iFixed += _getBit16 (pIO, 2);
            if (iFixed == 22) {
                iFixed += _getBit16 (pIO, 3);
            }
        }
        iLevel = 2 + (1 << iFixed);
        iIndex = getBit32 (pIO, iFixed);
        iLevel += iIndex;
    }
    return iLevel;
}

U8 decodeQPIndex(BitIOInfo* pIO,U8 cBits)
{
    if(_getBit16(pIO, 1) == 0)
        return 0;
    return (U8)(_getBit16(pIO, cBits) + 1);
}

/*************************************************************************
    DecodeSecondStageCoeff
*************************************************************************/
Int DecodeMacroblockLowpass (CWMImageStrCodec * pSC, CCodingContext *pContext,
        Int iMBX, Int iMBYdummy)
{
    const COLORFORMAT cf = pSC->m_param.cfColorFormat;
    const Int iChannels = (Int) pSC->m_param.cNumChannels;
    const Int iFullPlanes = (cf == YUV_420 || cf == YUV_422) ? 2 : iChannels;
    Int k;
	CAdaptiveScan *pScan = pContext->m_aScanLowpass;
    BitIOInfo* pIO = pContext->m_pIOLP;
    Int iModelBits = pContext->m_aModelLP.m_iFlcBits[0];
    Int aRLCoeffs[32], iNumNonzero = 0, iIndex = 0;
    Int aLaplacianMean[2] = { 0, 0}, *pLM = aLaplacianMean;
    Int iChannel, iCBP = 0;
#ifndef ARMOPT_BITIO    /* ARM opt always uses 32-bit version of getBits */
    U32 (*getBits)(BitIOInfo* pIO, U32 cBits) = _getBit16;
#endif
    CWMIMBInfo * pMBInfo = &pSC->MBInfo;
    I32 *aDC[MAX_CHANNELS];

    UNREFERENCED_PARAMETER( iMBX );
    UNREFERENCED_PARAMETER( iMBYdummy );

    readIS_L1(pSC, pIO);
    if((pSC->WMISCP.bfBitstreamFormat != SPATIAL) && (pSC->pTile[pSC->cTileColumn].cBitsLP > 0))  /* MB-based LP QP index */
        pMBInfo->iQIndexLP = decodeQPIndex(pIO, pSC->pTile[pSC->cTileColumn].cBitsLP);
    
    /* set arrays */
    for (k = 0; k < (Int) pSC->m_param.cNumChannels; k++) {
        aDC[k & 15] = pMBInfo->iBlockDC[k];
    }

    /** reset adaptive scan totals **/
    if (pSC->m_bResetRGITotals) {
        int iScale = 2;
        int iWeight = iScale * 16;
		pScan[0].uTotal = MAXTOTAL;
        for (k = 1; k < 16; k++) {
			pScan[k].uTotal = iWeight;
            iWeight -= iScale;
        }
    }

    /** in raw mode, this can take 6% of the bits in the extreme low rate case!!! **/
    if (cf == YUV_420 || cf == YUV_422 || cf == YUV_444) {
        int iCountM = pContext->m_iCBPCountMax, iCountZ = pContext->m_iCBPCountZero;
        int iMax = iFullPlanes * 4 - 5; /* actually (1 << iNChannels) - 1 **/
        if (iCountZ <= 0 || iCountM < 0) {
            iCBP = 0;
            if (_getBool16 (pIO)) {
                iCBP = 1;
                k = _getBit16 (pIO, iFullPlanes - 1);
                if (k) {
                    iCBP = k * 2 + _getBit16(pIO, 1);
                }
            }
            if (iCountM < iCountZ)
                iCBP = iMax - iCBP;
        }
        else {
            iCBP = _getBit16(pIO, iFullPlanes);
        }

        iCountM += 1 - 4 * (iCBP == iMax);/*(b + c - 2*a); */
        iCountZ += 1 - 4 * (iCBP == 0);/*(a + b - 2*c); */
        if (iCountM < -8)
            iCountM = -8;
        else if (iCountM > 7)
            iCountM = 7;
        pContext->m_iCBPCountMax = iCountM;

        if (iCountZ < -8)
            iCountZ = -8;
        else if (iCountZ > 7)
            iCountZ = 7;
        pContext->m_iCBPCountZero = iCountZ;
    }
    else { /** 1 or N channel **/
        for (iChannel = 0; iChannel < iChannels; iChannel++)
            iCBP |= (getBits (pIO, 1) << iChannel);
    }

#ifndef ARMOPT_BITIO    /* ARM opt always uses 32-bit version of getBits */
    if (pContext->m_aModelLP.m_iFlcBits[0] > 14 || pContext->m_aModelLP.m_iFlcBits[1] > 14) {
        getBits = getBit32;
    }
#endif

    for (iChannel = 0; iChannel < iFullPlanes; iChannel++) {
        PixelI *pCoeffs = aDC[iChannel];

        if (iCBP & 1) {
            iNumNonzero = DecodeBlock (iChannel > 0, aRLCoeffs, pContext->m_pAHexpt,
                CTDC, pIO, 1 + 9 * ((cf == YUV_420) && (iChannel == 1))
                + ((cf == YUV_422) && (iChannel == 1)));

            if ((cf == YUV_420 || cf == YUV_422) && iChannel) {
                Int aTemp[16]; /*14 required, 16 for security */
                static const Int aRemap[] = { 4,  1,2,3,  5,6,7 };
                const Int *pRemap = aRemap + (cf == YUV_420);
                const Int iCount = (cf == YUV_420) ? 6 : 14;

                (*pLM) += iNumNonzero;
                iIndex = 0;
                memset (aTemp, 0, sizeof(aTemp));

                for (k = 0; k < iNumNonzero; k++) {
                    iIndex += aRLCoeffs[k * 2];
                    aTemp[iIndex & 0xf] = aRLCoeffs[k * 2 + 1];
                    iIndex++;
                }

                for (k = 0; k < iCount; k++) {
                    aDC[(k & 1) + 1][pRemap[k >> 1]] = aTemp[k];
                }
            }
            else {
                (*pLM) += iNumNonzero;
                iIndex = 1;

                for (k = 0; k < iNumNonzero; k++) {
                    iIndex += aRLCoeffs[k * 2];
					pCoeffs[pScan[iIndex].uScan] = aRLCoeffs[k * 2 + 1];
					pScan[iIndex].uTotal++;
					if (pScan[iIndex].uTotal > pScan[iIndex - 1].uTotal) {
						CAdaptiveScan cTemp = pScan[iIndex];
						pScan[iIndex] = pScan[iIndex - 1];
						pScan[iIndex - 1] = cTemp;
					}
                    iIndex++;
                }
            }
        }

        if (iModelBits) {
            if ((cf == YUV_420 || cf == YUV_422) && iChannel) {
                for (k = 1; k < (cf == YUV_420 ? 4 : 8); k++) {
                    if (aDC[1][k] > 0) {
                        aDC[1][k] <<= iModelBits;
                        aDC[1][k] += getBits (pIO, iModelBits);
                    }
                    else if (aDC[1][k] < 0) {
                        aDC[1][k] <<= iModelBits;
                        aDC[1][k] -= getBits (pIO, iModelBits);
                    }
                    else {
                        aDC[1][k] = getBits (pIO, iModelBits);
                        if (aDC[1][k] && _getBool16 (pIO))
                            aDC[1][k] = -aDC[1][k];
                    }

                    if (aDC[2][k] > 0) {
                        aDC[2][k] <<= iModelBits;
                        aDC[2][k] += getBits (pIO, iModelBits);
                    }
                    else if (aDC[2][k] < 0) {
                        aDC[2][k] <<= iModelBits;
                        aDC[2][k] -= getBits (pIO, iModelBits);
                    }
                    else {
                        aDC[2][k] = getBits (pIO, iModelBits);
                        if (aDC[2][k] && _getBool16 (pIO))
                            aDC[2][k] = -aDC[2][k];
                    }
                }
            }
            else {
#ifdef JXR_WIN32_INTRINSICS   /* never defined: the portable code on every target */
                const Int iMask = (1 << iModelBits) - 1;
#endif /* _WIN32 */
                for (k = 1; k < 16; k++) {
#ifdef JXR_WIN32_INTRINSICS   /* never defined: the portable code on every target */
                    if (pCoeffs[k]) {
                        Int r1 = _rotl(pCoeffs[k], iModelBits);
                        pCoeffs[k] = (r1 ^ getBits(pIO, iModelBits)) - (r1 & iMask);
                    }
#else /* _WIN32 */
                    if (pCoeffs[k] > 0) {
                        pCoeffs[k] <<= iModelBits;
                        pCoeffs[k] += getBits (pIO, iModelBits);
                    }
                    else if (pCoeffs[k] < 0) {
                        pCoeffs[k] <<= iModelBits;
                        pCoeffs[k] -= getBits (pIO, iModelBits);
                    }
#endif /* _WIN32 */
                    else {
                        /*pCoeffs[k] = getBits (pIO, iModelBits); */
                        /*if (pCoeffs[k] && _getBool16 (pIO)) */
                        /*    pCoeffs[k] = -pCoeffs[k]; */
                        Int r1 = _peekBit16 (pIO, iModelBits + 1);
                        pCoeffs[k] = ((r1 >> 1) ^ (-(r1 & 1))) + (r1 & 1);
                        _flushBit16 (pIO, iModelBits + (pCoeffs[k] != 0));
                    }
                }
            }
        }
        pLM = aLaplacianMean + 1;
        iModelBits = pContext->m_aModelLP.m_iFlcBits[1];

        iCBP >>= 1;
    }

    UpdateModelMB (cf, iChannels, aLaplacianMean, &(pContext->m_aModelLP));

    if (pSC->m_bResetContext) {
        AdaptLowpassDec(pContext);
    }

    return ICERR_OK;
}

/*************************************************************************
    8 bit YUV 420 macroblock decode function with 4x4 transform
    Index order is as follows:
    Y:              U:      V:
     0  1  4  5     16 17   20 21
     2  3  6  7     18 19   22 23
     8  9 12 13
    10 11 14 15

    DCAC coefficients stored for 4x4 - offsets (x == no storage)
    Y:
    x x x [0..3]
    x x x [4..7]
    x x x [8..11]
    [16..19] [20..23] [24..27] [28..31,12..15]

    U, V:
    x [0..3]
    [8..11] [4..7,12..15]
*************************************************************************/
Int DecodeMacroblockDC(CWMImageStrCodec * pSC, CCodingContext *pContext, Int iMBX, Int iMBY)
{
    CWMITile * pTile = pSC->pTile + pSC->cTileColumn;
    CWMIMBInfo * pMBInfo = &pSC->MBInfo;
    const COLORFORMAT cf = pSC->m_param.cfColorFormat;
    const Int iChannels = (Int) pSC->m_param.cNumChannels;
    BitIOInfo* pIO = pContext->m_pIODC;
    Int iIndex, i;
    Int aLaplacianMean[2] = { 0, 0}, *pLM = aLaplacianMean;
    Int iModelBits = pContext->m_aModelDC.m_iFlcBits[0];
    struct CAdaptiveHuffman *pAH;
    Int iQDCY, iQDCU, iQDCV;
    /* const Int iChromaElements = (cf == YUV_420) ? 8 * 8 : ((cf == YUV_422) ? 8 * 16 : 16 * 16); */

    UNREFERENCED_PARAMETER( iMBX );
    UNREFERENCED_PARAMETER( iMBY );

    for (i = 0; i < iChannels; i++)
        memset (pMBInfo->iBlockDC[i], 0, 16 * sizeof (I32));

    readIS_L1(pSC, pIO);

    pMBInfo->iQIndexLP = pMBInfo->iQIndexHP = 0;

    if(pSC->WMISCP.bfBitstreamFormat == SPATIAL && pSC->WMISCP.sbSubband != SB_DC_ONLY){
        if(pTile->cBitsLP > 0)  /* MB-based LP QP index */
            pMBInfo->iQIndexLP = decodeQPIndex(pIO, pTile->cBitsLP);
        if( pSC->WMISCP.sbSubband != SB_NO_HIGHPASS && pTile->cBitsHP > 0)  /* MB-based HP QP index */
            pMBInfo->iQIndexHP = decodeQPIndex(pIO, pTile->cBitsHP);
    }
    if(pTile->cBitsHP == 0 && pTile->cNumQPHP > 1) /* use LP QP */
        pMBInfo->iQIndexHP = pMBInfo->iQIndexLP;
    if (pMBInfo->iQIndexLP >= pTile->cNumQPLP || pMBInfo->iQIndexHP >= pTile->cNumQPHP)
        return ICERR_ERROR;

    if(cf == Y_ONLY || cf == CMYK || cf == NCOMPONENT) {
        for (i = 0; i < iChannels; i++) {
            iQDCY = 0;
            /** get luminance DC **/
            if (_getBool16 (pIO)) {
                iQDCY = DecodeSignificantAbsLevel(pContext->m_pAHexpt[3], pIO) - 1;
                *pLM += 1;
            }
            if (iModelBits) {
                iQDCY = (iQDCY << iModelBits) | _getBit16(pIO, iModelBits);
            }
            if (iQDCY && _getBool16 (pIO))
                iQDCY = -iQDCY;
            pMBInfo->iBlockDC[i][0] = iQDCY;

            pLM = aLaplacianMean + 1;
            iModelBits = pContext->m_aModelDC.m_iFlcBits[1];
        }
    }
    else {
        /** find significant level in 3D **/
        pAH = pContext->m_pAHexpt[2];
        iIndex = getHuff (pAH->m_hufDecTable, pIO);
        iQDCY = iIndex >> 2;
        iQDCU = (iIndex >> 1) & 1;
        iQDCV = iIndex & 1;

        /** get luminance DC **/
        if (iQDCY) {
            iQDCY = DecodeSignificantAbsLevel(pContext->m_pAHexpt[3], pIO) - 1;
            *pLM += 1;
        }
        if (iModelBits) {
            iQDCY = (iQDCY << iModelBits) | _getBit16(pIO, iModelBits);
        }
        if (iQDCY && _getBool16 (pIO))
            iQDCY = -iQDCY;
        pMBInfo->iBlockDC[0][0] = iQDCY;

        /** get chrominance DC **/        
        pLM = aLaplacianMean + 1;
        iModelBits = pContext->m_aModelDC.m_iFlcBits[1];

        if (iQDCU) {
            iQDCU = DecodeSignificantAbsLevel(pContext->m_pAHexpt[4], pIO) - 1;
            *pLM += 1;
        }
        if (iModelBits) {
            iQDCU = (iQDCU << iModelBits) | _getBit16(pIO, iModelBits);
        }
        if (iQDCU && _getBool16 (pIO))
            iQDCU = -iQDCU;
        pMBInfo->iBlockDC[1][0] = iQDCU;

        if (iQDCV) {
            iQDCV = DecodeSignificantAbsLevel(pContext->m_pAHexpt[4], pIO) - 1;
            *pLM += 1;
        }
        if (iModelBits) {
            iQDCV = (iQDCV << iModelBits) | _getBit16(pIO, iModelBits);
        }
        if (iQDCV && _getBool16 (pIO))
            iQDCV = -iQDCV;
        pMBInfo->iBlockDC[2][0] = iQDCV;
    }

    UpdateModelMB (cf, iChannels, aLaplacianMean, &(pContext->m_aModelDC));
    
    if(((!(pSC->WMISCP.bfBitstreamFormat != FREQUENCY || pSC->m_Dparam->cThumbnailScale < 16)) || pSC->WMISCP.sbSubband == SB_DC_ONLY) && pSC->m_bResetContext){
        Int kk;
        for (kk = 2; kk < 5; kk++) {
            if (ICERR_OK != AdaptDecFixed (pContext->m_pAHexpt[kk])) {
                return ICERR_ERROR;
            }
        }
    }

    return ICERR_OK;
}

/*************************************************************************
    DecodeMacroblockHighpass
*************************************************************************/
Int DecodeMacroblockHighpass (CWMImageStrCodec *pSC, CCodingContext *pContext, 
                      Int iMBX, Int iMBY)
{   
    /** reset adaptive scan totals **/
    if (pSC->m_bResetRGITotals) {
        int iScale = 2, k;
        int iWeight = iScale * 16;
        pContext->m_aScanHoriz[0].uTotal = pContext->m_aScanVert[0].uTotal = MAXTOTAL;
        for (k = 1; k < 16; k++) {
            pContext->m_aScanHoriz[k].uTotal = pContext->m_aScanVert[k].uTotal = iWeight;
            iWeight -= iScale;
        }
    }
    if((pSC->WMISCP.bfBitstreamFormat != SPATIAL) && (pSC->pTile[pSC->cTileColumn].cBitsHP > 0)) { /* MB-based HP QP index */
        pSC->MBInfo.iQIndexHP = decodeQPIndex(pContext->m_pIOAC, pSC->pTile[pSC->cTileColumn].cBitsHP);
        if (pSC->MBInfo.iQIndexHP >= pSC->pTile[pSC->cTileColumn].cNumQPHP)
            goto ErrorExit;
    }
    else if(pSC->pTile[pSC->cTileColumn].cBitsHP == 0 && pSC->pTile[pSC->cTileColumn].cNumQPHP > 1) /* use LP QP */
        pSC->MBInfo.iQIndexHP = pSC->MBInfo.iQIndexLP;


    DecodeCBP (pSC, pContext);
    predCBPDec(pSC, pContext);

    if (DecodeCoeffs (pSC, pContext, iMBX, iMBY, 
        pContext->m_pIOAC, pContext->m_pIOFL) != ICERR_OK)
        goto ErrorExit;

    if (pSC->m_bResetContext) {
        AdaptHighpassDec(pContext);
    }

    return ICERR_OK;
ErrorExit:
    return ICERR_ERROR;
}

/*************************************************************************
    Adapt
*************************************************************************/
Int AdaptLowpassDec(CCodingContext * pSC)
{
    Int kk;
    for (kk = 0; kk < CONTEXTX + CTDC; kk++) {
        if (ICERR_OK != AdaptDecFixed (pSC->m_pAHexpt[kk])) {
            goto ErrorExit;
        }
    }
    return ICERR_OK;

ErrorExit:
    return ICERR_ERROR;

}

Int AdaptHighpassDec(CCodingContext * pSC)
{
    Int kk;
    if (ICERR_OK != AdaptDecFixed (pSC->m_pAdaptHuffCBPCY)) {
        goto ErrorExit;
    }
    if (ICERR_OK != AdaptDecFixed (pSC->m_pAdaptHuffCBPCY1)) {
        goto ErrorExit;
    }
    for (kk = 0; kk < CONTEXTX; kk++) {
        if (ICERR_OK != AdaptDecFixed (pSC->m_pAHexpt[kk + CONTEXTX + CTDC])) {
            goto ErrorExit;
        }
    }

    return ICERR_OK;
    
ErrorExit:
    return ICERR_ERROR;
}

#undef TRACE_MALLOC
#undef TRACE_NEW
#undef TRACE_HEAP
#undef _FORCEINLINE
#undef LOAD16
#undef SIGN_BIT
#undef _getBool16
#undef _getSign
/* ---- strdec.c ---- */
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
/* (merged) #include "strcodec.h" */
/* (merged) #include "decode.h" */
/* (merged) #include "strTransform.h" */
#include <math.h>
/* (merged) #include "perfTimer.h" */

#ifdef MEM_TRACE
#define TRACE_MALLOC    1
#define TRACE_NEW       0
#define TRACE_HEAP      0
/* (merged) #include "memtrace.h" */
#endif

#ifdef X86OPT_INLINE
#define _FORCEINLINE __forceinline
#else /* X86OPT_INLINE */
#define _FORCEINLINE
#endif /* X86OPT_INLINE */

#if defined(WMP_OPT_SSE2) || defined(WMP_OPT_CC_DEC) || defined(WMP_OPT_TRFM_DEC)
void StrDecOpt(CWMImageStrCodec* pSC);
#endif /* OPT defined */

Int processMacroblockDec(CWMImageStrCodec *);

U8 readQuantizerSB(U8 pQPIndex[MAX_CHANNELS], SimpleBitIO * pIO, size_t cChannel)
{
    U8 cChMode = 0;
    
    if(cChannel >= MAX_CHANNELS)
        return 0;

    if(cChannel > 1)
        cChMode = (U8)getBit32_SB(pIO, 2); /* Channel mode */

    pQPIndex[0] = (U8)getBit32_SB(pIO, 8); /* Y */

    if(cChMode == 1)  /* MIXED */
        pQPIndex[1] = (U8)getBit32_SB(pIO, 8); /* UV */
    else if(cChMode > 0){ /* INDEPENDENT */
        size_t i;

        for(i = 1; i < cChannel; i ++)
            pQPIndex[i] = (U8)getBit32_SB(pIO, 8); /* UV */
    }

    return cChMode;
}

U8 readQuantizer(CWMIQuantizer * pQuantizer[MAX_CHANNELS], BitIOInfo * pIO, size_t cChannel, size_t iPos)
{
    U8 cChMode = 0;

    if(cChannel > 1)
        cChMode = (U8)getBit16(pIO, 2); /* Channel mode */

    pQuantizer[0][iPos].iIndex = (U8)getBit16(pIO, 8); /* Y */

    if(cChMode == 1)  /* MIXED */
        pQuantizer[1][iPos].iIndex = (U8)getBit16(pIO, 8); /* UV */
    else if(cChMode > 0){ /* INDEPENDENT */
        size_t i;

        for(i = 1; i < cChannel; i ++)
            pQuantizer[i][iPos].iIndex = (U8)getBit16(pIO, 8); /* UV */
    }

    return cChMode;
}

/* packet header: 00000000 00000000 00000001 ?????xxx */
/* xxx:           000(spatial) 001(DC) 010(AD) 011(AC) 100(FL) 101-111(reserved) */
/* ?????:         (iTileY * cNumOfSliceV + iTileX) % 32 */
Int readPacketHeader(BitIOInfo * pIO, U8 ptPacketType, U8 pID)
{
    UNREFERENCED_PARAMETER( ptPacketType );
    UNREFERENCED_PARAMETER( pID );
    if(getBit16(pIO, 8) != 0 || getBit16(pIO, 8) != 0 || getBit16(pIO, 8) != 1)
        return ICERR_ERROR;
    getBit16(pIO, 8);
    return ICERR_OK;
}

Int readTileHeaderDC(CWMImageStrCodec * pSC, BitIOInfo * pIO)
{
    if((pSC->m_param.uQPMode & 1) != 0){ /* not DC uniform */
        size_t iTile;
        CWMITile * pTile = pSC->pTile + pSC->cTileColumn;

        if(pSC->cTileRow + pSC->cTileColumn == 0) /* allocate DC QP info */
            for(iTile = 0; iTile <= pSC->WMISCP.cNumOfSliceMinus1V; iTile ++)
                if(allocateQuantizer(pSC->pTile[iTile].pQuantizerDC, pSC->m_param.cNumChannels, 1) != ICERR_OK)
                    return ICERR_ERROR;

        pTile->cChModeDC = readQuantizer(pTile->pQuantizerDC, pIO, pSC->m_param.cNumChannels, 0);
        formatQuantizer(pTile->pQuantizerDC, pTile->cChModeDC, pSC->m_param.cNumChannels, 0, TRUE, pSC->m_param.bScaledArith);
    }

    return ICERR_OK;
}

Int readTileHeaderLP(CWMImageStrCodec * pSC, BitIOInfo * pIO)
{
    if(pSC->WMISCP.sbSubband != SB_DC_ONLY && (pSC->m_param.uQPMode & 2) != 0){ /* not LP uniform */
        CWMITile * pTile = pSC->pTile + pSC->cTileColumn;
        U8 i;

        pTile->bUseDC = (getBit16(pIO, 1) == 1 ? TRUE : FALSE);
        pTile->cBitsLP = 0;
        pTile->cNumQPLP = 1;

        if(pSC->cTileRow > 0)
            freeQuantizer(pTile->pQuantizerLP);
        
        if(pTile->bUseDC == TRUE){
            if(allocateQuantizer(pTile->pQuantizerLP, pSC->m_param.cNumChannels, pTile->cNumQPLP) != ICERR_OK)
                return ICERR_ERROR;
            useDCQuantizer(pSC, pSC->cTileColumn);
        }
        else{
            pTile->cNumQPLP = (U8)getBit16(pIO, 4) + 1;
            pTile->cBitsLP = dquantBits(pTile->cNumQPLP);
            
            if(allocateQuantizer(pTile->pQuantizerLP, pSC->m_param.cNumChannels, pTile->cNumQPLP) != ICERR_OK)
                return ICERR_ERROR;

            for(i = 0; i < pTile->cNumQPLP; i ++){
                pTile->cChModeLP[i] = readQuantizer(pTile->pQuantizerLP, pIO, pSC->m_param.cNumChannels, i);
                formatQuantizer(pTile->pQuantizerLP, pTile->cChModeLP[i], pSC->m_param.cNumChannels, i, TRUE, pSC->m_param.bScaledArith);
            }
        }
    }

    return ICERR_OK;
}

Int readTileHeaderHP(CWMImageStrCodec * pSC, BitIOInfo * pIO)
{
    if(pSC->WMISCP.sbSubband != SB_DC_ONLY && pSC->WMISCP.sbSubband != SB_NO_HIGHPASS && (pSC->m_param.uQPMode & 4) != 0){ /* not HP uniform */
        CWMITile * pTile = pSC->pTile + pSC->cTileColumn;
        U8 i;

        pTile->bUseLP = (getBit16(pIO, 1) == 1 ? TRUE : FALSE);
        pTile->cBitsHP = 0;
        pTile->cNumQPHP = 1;

        if(pSC->cTileRow > 0)
            freeQuantizer(pTile->pQuantizerHP);
        
        if(pTile->bUseLP == TRUE){
            pTile->cNumQPHP = pTile->cNumQPLP;
            if(allocateQuantizer(pTile->pQuantizerHP, pSC->m_param.cNumChannels, pTile->cNumQPHP) != ICERR_OK)
                return ICERR_ERROR;
            useLPQuantizer(pSC, pTile->cNumQPHP, pSC->cTileColumn);
        }
        else{
            pTile->cNumQPHP = (U8)getBit16(pIO, 4) + 1;
            pTile->cBitsHP = dquantBits(pTile->cNumQPHP);

            if(allocateQuantizer(pTile->pQuantizerHP, pSC->m_param.cNumChannels, pTile->cNumQPHP) != ICERR_OK)
                return ICERR_ERROR;

            for(i = 0; i < pTile->cNumQPHP; i ++){
                pTile->cChModeHP[i] = readQuantizer(pTile->pQuantizerHP, pIO, pSC->m_param.cNumChannels, i);
                formatQuantizer(pTile->pQuantizerHP, pTile->cChModeHP[i], pSC->m_param.cNumChannels, i, FALSE, pSC->m_param.bScaledArith);
            }
        }
    }

    return ICERR_OK;
}

Int readPackets(CWMImageStrCodec * pSC)
{
    if(pSC->cColumn == 0 && pSC->cRow == pSC->WMISCP.uiTileY[pSC->cTileRow]){ /* start of a new horizontal slice */
        size_t k;
        
        if (pSC->m_bSecondary) {
             if(pSC->cNumBitIO > 0){
                for(k = 0; k <= pSC->WMISCP.cNumOfSliceMinus1V; k ++){
                    /* reset coding contexts */
                    ResetCodingContextDec(&pSC->m_pCodingContext[k]);
                }
            }
            else{ /* for multiple decoding calls! */
                ResetCodingContextDec(&pSC->m_pCodingContext[0]);
            }
        }
        else {
            /* get sizes of each packet and update index table */
            for(k = 0; k < pSC->cNumBitIO; k ++){
                if(pSC->ppWStream != NULL){ /* new API */
                    unsigned cBands = (pSC->WMISCP.bfBitstreamFormat == SPATIAL ? 1 : pSC->cSB);
                    struct WMPStream ** ppWS = pSC->ppWStream + (pSC->WMISCP.cNumOfSliceMinus1V + 1) * pSC->cTileRow * cBands
                        + k / cBands * cBands + (k % cBands);

                    if(pSC->cTileRow > 0 && pSC->m_ppBitIO[k]->pWS != NULL)     /* attached to the same packet of the tile on top */
                        detachISRead(pSC, pSC->m_ppBitIO[k]);    /* detach it */
                    
                    if(ppWS[0] != NULL)
                        attachISRead(pSC->m_ppBitIO[k], ppWS[0], pSC); /* need to attach it */
                }
                else{
                    if(pSC->cTileRow > 0)
                        detachISRead(pSC, pSC->m_ppBitIO[k]);
                    pSC->WMISCP.pWStream->SetPos(pSC->WMISCP.pWStream, pSC->pIndexTable[pSC->cNumBitIO * pSC->cTileRow + k] + pSC->cHeaderSize);
                    attachISRead(pSC->m_ppBitIO[k], pSC->WMISCP.pWStream, pSC);
                }
            }
            
            if(pSC->cNumBitIO == 0){
                detachISRead(pSC, pSC->pIOHeader);
                if(pSC->ppWStream != NULL){/* new API */
                    attachISRead(pSC->pIOHeader, pSC->ppWStream[0], pSC); /* need to attach it */
                }
                else{
                    pSC->WMISCP.pWStream->SetPos(pSC->WMISCP.pWStream, pSC->cHeaderSize);
                    attachISRead(pSC->pIOHeader, pSC->WMISCP.pWStream, pSC);
                }
            }
            
            for(k = 0; k <= pSC->WMISCP.cNumOfSliceMinus1V; k ++){
                U8 pID = (U8)((pSC->cTileRow * (pSC->WMISCP.cNumOfSliceMinus1V + 1) + k) & 0x1F);
                
                /* read packet header */
                if(pSC->WMISCP.bfBitstreamFormat == SPATIAL){
                    BitIOInfo * pIO = (pSC->cNumBitIO == 0 ? pSC->pIOHeader : pSC->m_ppBitIO[k]);

                    if(pIO->pWS == NULL || readPacketHeader(pIO, 0, pID) != ICERR_OK)
                        return ICERR_ERROR;
                    pSC->m_pCodingContext[k].m_iTrimFlexBits = (pSC->m_param.bTrimFlexbitsFlag) ? getBit16(pIO, 4) : 0;
                }
                else{
                    if(pSC->m_ppBitIO[k * pSC->cSB + 0] == NULL || readPacketHeader(pSC->m_ppBitIO[k * pSC->cSB + 0], 1, pID) != ICERR_OK)
                        return ICERR_ERROR;
                    if(pSC->cSB > 1){
                        if(pSC->m_ppBitIO[k * pSC->cSB + 1] == NULL || readPacketHeader(pSC->m_ppBitIO[k * pSC->cSB + 1], 2, pID) != ICERR_OK)
                            return ICERR_ERROR;
                    }
                    if(pSC->cSB > 2){
                        if(pSC->m_ppBitIO[k * pSC->cSB + 2] == NULL || readPacketHeader(pSC->m_ppBitIO[k * pSC->cSB + 2], 3, pID) != ICERR_OK)
                            return ICERR_ERROR;
/*                        readTileHeaderHP(pSC, pSC->m_ppBitIO[k * pSC->cSB + 2]); */
                    }
                    if(pSC->cSB > 3){
                        if(pSC->m_ppBitIO[k * pSC->cSB + 3] == NULL)
                            return ICERR_ERROR;
                        readPacketHeader(pSC->m_ppBitIO[k * pSC->cSB + 3], 4, pID);  /* bad flexbits packet doesn't generate an error */
                        pSC->m_pCodingContext[k].m_iTrimFlexBits = (pSC->m_param.bTrimFlexbitsFlag) ? getBit16(pSC->m_ppBitIO[k * pSC->cSB + 3], 4) : 0;
                    }
                }

                /* reset coding contexts */
                ResetCodingContextDec(&pSC->m_pCodingContext[k]);
            }
        }
    }
    
    if(pSC->m_bCtxLeft && pSC->m_bCtxTop && pSC->m_bSecondary == FALSE){
        CCodingContext *pContext = &pSC->m_pCodingContext[pSC->cTileColumn];
        
        readTileHeaderDC(pSC, pContext->m_pIODC);
        if(pSC->m_pNextSC != NULL)
            readTileHeaderDC(pSC->m_pNextSC, pContext->m_pIODC);
        if(pSC->cSB > 1){
            readTileHeaderLP(pSC, pContext->m_pIOLP);
            if(pSC->m_pNextSC != NULL)
                readTileHeaderLP(pSC->m_pNextSC, pContext->m_pIOLP);
        }
        if(pSC->cSB > 2){
            readTileHeaderHP(pSC, pContext->m_pIOAC);
            if(pSC->m_pNextSC != NULL)
                readTileHeaderHP(pSC->m_pNextSC, pContext->m_pIOAC);
        }
    }

    return ICERR_OK;
}

/* inverse transform and overlap possible part of a macroblock */
Int processMacroblockDec(CWMImageStrCodec * pSC)
{
    const OVERLAP olOverlap = pSC->WMISCP.olOverlap;
    /* const Bool left = (pSC->cColumn == 0); */
    const Bool /*top = (pSC->cRow == 0),*/ bottom = (pSC->cRow == pSC->cmbHeight);
    const Bool bottomORright = (bottom || pSC->cColumn == pSC->cmbWidth);
    /* const size_t mbWidth = pSC->cmbWidth, mbX = pSC->cColumn; */
    /* Int iQIndex = 0; */
    ERR_CODE result = ICERR_OK;
    size_t j, jend = (pSC->m_pNextSC != NULL);

    for (j = 0; j <= jend; j++) {
        if(!bottomORright){
            CCodingContext *pContext;
            
            getTilePos(pSC, pSC->cColumn, pSC->cRow);
            
            if(jend){
                pSC->m_pNextSC->cTileColumn = pSC->cTileColumn;
                pSC->m_pNextSC->cTileRow = pSC->cTileRow;
            }

            pContext = &pSC->m_pCodingContext[pSC->cTileColumn];
           
            if(readPackets(pSC) != ICERR_OK)
                return ICERR_ERROR;
         
            /* check if we need to do entropy decode */
			if(!pSC->m_Dparam->bDecodeFullFrame){
				if(pSC->cColumn == pSC->WMISCP.uiTileX[pSC->cTileColumn]){ /* switching to a new tile */
					size_t rLeft = pSC->m_Dparam->cROILeftX, rRight = pSC->m_Dparam->cROIRightX;
					size_t rTop = pSC->m_Dparam->cROITopY, rBottom = pSC->m_Dparam->cROIBottomY;
					size_t rExt = (olOverlap == OL_NONE ? 0 : olOverlap == OL_ONE ? 2 : 10);
					size_t tLeft = pSC->cColumn * 16, tTop = pSC->WMISCP.uiTileY[pSC->cTileRow] * 16;
					size_t tRight = (pSC->cTileColumn != pSC->WMISCP.cNumOfSliceMinus1V ? pSC->WMISCP.uiTileX[pSC->cTileColumn + 1] : pSC->cmbWidth) * 16;
					size_t tBottom = (pSC->cTileRow != pSC->WMISCP.cNumOfSliceMinus1H ? pSC->WMISCP.uiTileY[pSC->cTileRow + 1] : pSC->cmbHeight) * 16;

					/* tile overlaps with ROI? */
					pContext->m_bInROI = ((rLeft >= tRight + rExt || rTop >= tBottom + rExt || tLeft > rRight + rExt ||
						tTop > rBottom + rExt || pSC->cRow * 16 > rBottom + rExt) ? FALSE : TRUE);
				}
			}

            if(pSC->m_Dparam->bDecodeFullFrame || pContext->m_bInROI){                
                if ((result = DecodeMacroblockDC(pSC, pContext, (Int)pSC->cColumn, (Int)pSC->cRow)) != ICERR_OK)
                    return result;

                if(pSC->m_Dparam->bDecodeLP){
                    if ((result = DecodeMacroblockLowpass(pSC, pContext, (Int)pSC->cColumn, (Int)pSC->cRow)) != ICERR_OK)
                        return result;
                }

                predDCACDec(pSC);
                                
                dequantizeMacroblock(pSC);

                if(pSC->m_Dparam->bDecodeHP){
                    if ((result = DecodeMacroblockHighpass(pSC, pContext, (Int)pSC->cColumn, (Int)pSC->cRow)) != ICERR_OK)
                        return result;
                    predACDec(pSC);
                }
           
                /* keep necessary info for future prediction */
                updatePredInfo(pSC, &pSC->MBInfo, (Int)pSC->cColumn, pSC->m_param.cfColorFormat);
            }
        }

        if((!pSC->m_Dparam->bDecodeFullFrame) &&
            ((pSC->cColumn * 16 > pSC->m_Dparam->cROIRightX  + 25) || (pSC->cColumn * 16 + 25 < pSC->m_Dparam->cROILeftX)
            || (pSC->cRow * 16 > pSC->m_Dparam->cROIBottomY + 25) || (pSC->cRow * 16 + 25 < pSC->m_Dparam->cROITopY)))
        {
            /* do nothing */
        }
        else {
            pSC->Transform(pSC);
        }

        if (jend) {
            pSC->m_pNextSC->cRow = pSC->cRow;
            pSC->m_pNextSC->cColumn = pSC->cColumn;
            pSC = pSC->m_pNextSC;
        }
    }

    return result;
}

/*================================================================ */
/* Inverse Color Conversion */
/*#define _ICC1(r, g, b) (g^=b^=g^=b, r^=g^=r^=g, b += ((g) >> 1), r += ((g) >> 1), g -= (b+3*r+2) >> 2) */
/*#define _ICC(r, g, b) (g^=b^=g^=b, r^=g^=r^=g, b += ((g) >> 1), r += ((g) >> 1), g -= (b+3*r+2) >> 2) */
/*================================================================ */
/*#define _ICC1(r, g, b) r -= (g >> 1), g += r, r -= ((b + 1) >> 1), b += r */
/*#define _ICC(r, g, b) r -= (g >> 1), g += r, r -= (b >> 1), b += r */

#define _ICC(r, g, b)  (g -= ((r + 0) >> 1), r -= ((b + 1) >> 1) - g, b += r)
#define _ICC_CMYK(c, m, y, k) (k -= ((m + 1) >> 1), m -= (c >> 1) - k, c -= ((y + 1) >> 1) - m, y += c)

#define _CLIP2(l, v, h) ((v) < (l) ? (l) : ((h) < (v) ? (h) : (v)))
#define _CLIP8(v) ((U8)_CLIP2(0, v, 255))
#define _CLIP16(v) ((I16)_CLIP2(-32768, v, 32767))
#define _CLIPU16(v) ((U16)_CLIP2(0, v, 65535))

#define min(a,b) (((a) < (b)) ? (a) : (b))

/*inverseConvert: Inverse conversion from float RGB to RGBE */
static _FORCEINLINE void inverseConvert (PixelI iF, U8 *pRGB, U8 *pE)
{
    if (iF <= 0) {
        *pRGB = *pE = 0;
    }
    else if ((iF >> 7) > 1) {
        /** normal form **/
        *pE = (U8) (iF >> 7); /*+ 1; */
        *pRGB = (iF & 0x7f) | 0x80;
    }
    else {  
        /** denormal form **/
        *pE = 1;
        *pRGB = (U8) iF;
    }
}

#ifdef __ANSI__
#define max(a,b) ((a) > (b) ? (a) : (b))
#endif /* __ANSI__ */

static _FORCEINLINE void inverseConvertRGBE (PixelI iFr, PixelI iFg, PixelI iFb, U8 *pR, U8 *pG, U8 *pB, U8 *pE)
{
    U8 iShift;

    U8 pR_E, pG_E, pB_E;

    inverseConvert (iFr, pR, &pR_E);
    inverseConvert (iFg, pG, &pG_E);
    inverseConvert (iFb, pB, &pB_E);

    *pE = max(max(pR_E, pG_E), pB_E); 

    if(*pE > pR_E){
            iShift = (*pE - pR_E);
            *pR = (U8)((((int)*pR) * 2 + 1) >> (iShift + 1));
    }
    if(*pE > pG_E){
            iShift = (*pE - pG_E);
            *pG = (U8)((((int)*pG) * 2 + 1) >> (iShift + 1));
    }
    if(*pE > pB_E){
            iShift = (*pE - pB_E);
            *pB = (U8)((((int)*pB) * 2 + 1) >> (iShift + 1));
    }
}


/*pixel to float 32! */
static _FORCEINLINE float pixel2float(PixelI _h, const char _c, const unsigned char _lm)
{
    union uif
    {
        I32   i;
        float f;
    } x;

    I32 s, iTempH, m, e, lmshift = (1 << _lm);

    /* assert (_c <= 127); */

    iTempH = (I32) _h ;
    s = (iTempH >> 31);
    iTempH = (iTempH ^ s) - s; /* abs(iTempH) */

    e = (U32) iTempH >> _lm;/* & ((1 << (31 - _lm)) - 1); */
    m = (iTempH & (lmshift - 1)) | lmshift; /* actual mantissa, with normalizer */
    if (e == 0) { /* denormal land */
        m ^= lmshift; /* normalizer removed */
        e = 1; /* actual exponent */
    }

    e += (127 - _c);
    while (m < lmshift && e > 1 && m > 0) { /* denormal originally, see if normal is possible */
        e--;
        m <<= 1;
    }
    if (m < lmshift) /* truly denormal */
        e = 0;
    else
        m ^= lmshift;
    m <<= (23 - _lm);

    x.i = (s & 0x80000000) | (e << 23) | m;

    return x.f;
}

/*convert Half-16 to internal format, only need to handle sign bit */
static _FORCEINLINE U16 backwardHalf (PixelI hHalf)
{
    PixelI s;
    s = hHalf >> 31;
    hHalf = ((hHalf & 0x7fff) ^ s) - s; /* don't worry about overflow */
    return (U16) hHalf;
}


Void interpolateUV(CWMImageStrCodec * pSC)
{
    const COLORFORMAT cfExt = pSC->WMII.cfColorFormat;
    const size_t cWidth = pSC->cmbWidth * 16;
    PixelI * pSrcU = pSC->a0MBbuffer[1], * pSrcV = pSC->a0MBbuffer[2];
    PixelI * pDstU = pSC->pResU, * pDstV = pSC->pResV;
    size_t iRow, iColumn;
    size_t iIdxS = 0, iIdxD = 0;

    if(pSC->m_param.cfColorFormat == YUV_422){  /* 422 => 444, interpolate horizontally */
        for(iRow = 0; iRow < 16; iRow ++){
            for(iColumn = 0; iColumn < cWidth; iColumn += 2){
                iIdxS = ((iColumn >> 4) << 7) + idxCC[iRow][(iColumn >> 1) & 7];
                iIdxD = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];

                /* copy over */
                pDstU[iIdxD] = pSrcU[iIdxS];
                pDstV[iIdxD] = pSrcV[iIdxS];

                if(iColumn > 0){
                    size_t iL = iColumn - 2, iIdxL = ((iL >> 4) << 8) + idxCC[iRow][iL & 15];
                    size_t iC = iColumn - 1, iIdxC = ((iC >> 4) << 8) + idxCC[iRow][iC & 15];
                    
                    /* interpolate */
                    pDstU[iIdxC] = ((pDstU[iIdxL] + pDstU[iIdxD] + 1) >> 1);
                    pDstV[iIdxC] = ((pDstV[iIdxL] + pDstV[iIdxD] + 1) >> 1);
                }
            }

            /*last pixel */
            iIdxS = (((iColumn - 1) >> 4) << 8) + idxCC[iRow][(iColumn - 1) & 15];
            pDstU[iIdxS] = pDstU[iIdxD];
            pDstV[iIdxS] = pDstV[iIdxD];
        }
    }
    else{ /* 420 => 422 or 444, interpolate vertically */
        const size_t cShift = (cfExt == YUV_422 ? 3 : 4);
        
        for(iColumn = 0; iColumn < cWidth; iColumn += 2){
            const size_t cMB = ((iColumn >> 4) << (4 + cShift)), cPix = (iColumn >> (4 - cShift)) & ((1 << cShift) - 1);
            
            for(iRow = 0; iRow < 16; iRow += 2){
                iIdxS = ((iColumn >> 4) << 6) + idxCC_420[iRow >> 1][(iColumn >> 1) & 7];
                iIdxD = cMB + idxCC[iRow][cPix];
                
                /* copy over */
                pDstU[iIdxD] = pSrcU[iIdxS];
                pDstV[iIdxD] = pSrcV[iIdxS];

                if(iRow > 0){
                    size_t iIdxT = cMB + idxCC[iRow - 2][cPix];
                    size_t iIdxC = cMB + idxCC[iRow - 1][cPix];
                    
                    /* interpolate */
                    pDstU[iIdxC] = ((pDstU[iIdxT] + pDstU[iIdxD] + 1) >> 1);
                    pDstV[iIdxC] = ((pDstV[iIdxT] + pDstV[iIdxD] + 1) >> 1);
                }
            }

            /*last row */
            iIdxS = cMB + idxCC[15][cPix];
            if(pSC->cRow == pSC->cmbHeight){ /* image boundary */
                pDstU[iIdxS] = pDstU[iIdxD];
                pDstV[iIdxS] = pDstV[iIdxD];
            }
            else{ /* need next MB row */
                size_t iIdxB = ((iColumn >> 4) << 6) + idxCC_420[0][(iColumn >> 1) & 7];

                pDstU[iIdxS] = ((pSC->a1MBbuffer[1][iIdxB] + pDstU[iIdxD] + 1) >> 1);
                pDstV[iIdxS] = ((pSC->a1MBbuffer[2][iIdxB] + pDstV[iIdxD] + 1) >> 1);
            }
        }

        if(cfExt != YUV_422){ /* 420 => 444, interpolate horizontally */
            for(iRow = 0; iRow < 16; iRow ++){
                for(iColumn = 1; iColumn < cWidth - 2; iColumn += 2){
                    size_t iIdxL = (((iColumn - 1) >> 4) << 8) + idxCC[iRow][(iColumn - 1) & 15];

                    iIdxD = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
                    iIdxS = (((iColumn + 1) >> 4) << 8) + idxCC[iRow][(iColumn + 1) & 15];
                    
                    pDstU[iIdxD] = ((pDstU[iIdxS] + pDstU[iIdxL] + 1) >> 1);
                    pDstV[iIdxD] = ((pDstV[iIdxS] + pDstV[iIdxL] + 1) >> 1);
                }

                /* last pixel */
                iIdxD = (((cWidth - 1) >> 4) << 8) + idxCC[iRow][(cWidth - 1) & 15];
                pDstU[iIdxD] = pDstU[iIdxS];
                pDstV[iIdxD] = pDstV[iIdxS];
            }
        }
    }
}

/* write one MB row of Y_ONLY/CF_ALPHA/YUV_444/N_CHANNEL to output buffer */
Void outputNChannel(CWMImageStrCodec * pSC, size_t iFirstRow, size_t iFirstColumn, size_t cWidth, size_t cHeight, size_t iShift, PixelI iBias)
{
    const CWMImageInfo* pII = &pSC->WMII;
    const size_t cChannel = pII->cfColorFormat == Y_ONLY ? 1 : pSC->WMISCP.cChannel;
    /* const U8 cbChannels[BDB_MAX] = {-1, 1, 2, 2, 2, 4, 4, -1, -1, }; */
    const U8 nLen = pSC->WMISCP.nLenMantissaOrShift;
    const I8 nExpBias = pSC->WMISCP.nExpBias;

    PixelI * pChannel[16];
    size_t iChannel, iRow, iColumn;
    size_t * pOffsetX = pSC->m_Dparam->pOffsetX, * pOffsetY = pSC->m_Dparam->pOffsetY + (pSC->cRow - 1) * 16, iY;

    assert(cChannel <= 16);

    for(iChannel = 0; iChannel < cChannel; iChannel ++)
        pChannel[iChannel & 15] = pSC->a0MBbuffer[iChannel];

    if(pSC->m_bUVResolutionChange)
        pChannel[1] = pSC->pResU, pChannel[2] = pSC->pResV;

    switch(pSC->WMII.bdBitDepth){
        case BD_8:
            for(iRow = iFirstRow; iRow < cHeight; iRow ++){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                    U8 * pDst = (U8 *)pSC->WMIBI.pv + iY + pOffsetX[iColumn];

                    for(iChannel = 0; iChannel < cChannel; iChannel ++){
                        PixelI p = ((pChannel[iChannel & 15][((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] + iBias) >> iShift);
                        
                        pDst[iChannel] = _CLIP8(p);
                    }
                }
            }
            break;

        case BD_16:
            for(iRow = iFirstRow; iRow < cHeight; iRow ++){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                    U16 * pDst = (U16 *)pSC->WMIBI.pv + iY + pOffsetX[iColumn];

                    for(iChannel = 0; iChannel < cChannel; iChannel ++){
                        PixelI p = ((pChannel[iChannel & 15][((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] + iBias) >> iShift);
                        
                        p <<= nLen;
                        pDst[iChannel] = _CLIPU16(p);
                    }
                }
            }
            break;

        case BD_16S:
            for(iRow = iFirstRow; iRow < cHeight; iRow ++){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                    I16 * pDst = (I16 *)pSC->WMIBI.pv + iY + pOffsetX[iColumn];

                    for(iChannel = 0; iChannel < cChannel; iChannel ++){
                        PixelI p = ((pChannel[iChannel & 15][((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] + iBias) >> iShift);

                        p <<= nLen;                        
                        pDst[iChannel] = _CLIP16(p);
                    }
                }
            }
            break;

        case BD_16F:
            for(iRow = iFirstRow; iRow < cHeight; iRow ++){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                    U16 * pDst = (U16 *)pSC->WMIBI.pv + iY + pOffsetX[iColumn];

                    for(iChannel = 0; iChannel < cChannel; iChannel ++){
                        PixelI p = ((pChannel[iChannel & 15][((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf]] + iBias) >> iShift);
                        
                        pDst[iChannel] = backwardHalf(p);
                    }
                }
            }
            break;

        case BD_32:
            for(iRow = iFirstRow; iRow < cHeight; iRow ++){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                    U32 * pDst = (U32 *)pSC->WMIBI.pv + iY + pOffsetX[iColumn];

                    for(iChannel = 0; iChannel < cChannel; iChannel ++){
                        PixelI p = ((pChannel[iChannel & 15][((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf]] + iBias) >> iShift);

                        p <<= nLen;
                        pDst[iChannel] = (U32)(p);
                    }
                }
            }
            break;

        case BD_32S:
            for(iRow = iFirstRow; iRow < cHeight; iRow ++){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                    I32 * pDst = (I32 *)pSC->WMIBI.pv + iY + pOffsetX[iColumn];

                    for(iChannel = 0; iChannel < cChannel; iChannel ++){
                        PixelI p = ((pChannel[iChannel & 15][((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf]] + iBias) >> iShift);

                        p <<= nLen;
                        pDst[iChannel] = (I32)(p);
                    }
                }
            }
            break;

        case BD_32F:
            for(iRow = iFirstRow; iRow < cHeight; iRow ++){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                    float * pDst = (float *)pSC->WMIBI.pv + iY + pOffsetX[iColumn];

                    for(iChannel = 0; iChannel < cChannel; iChannel ++){
                        PixelI p = ((pChannel[iChannel & 15][((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf]] + iBias) >> iShift);                       
                        
                        pDst[iChannel] = pixel2float (p, nExpBias, nLen);
                    }
                }
            }
            break;

        default:
            assert(0);
            break;
    }
}

#ifndef REENTRANT_MODE
static void fixup_Y_ONLY_to_Others(
    const CWMImageStrCodec* pSC,
    const CWMImageBufferInfo* pBI)
{
    const CWMImageInfo* pII = &pSC->WMII;
    const CWMIStrCodecParam* pSCP = &pSC->WMISCP;
    size_t cWidth = 0, cHeight = 0;
    size_t idxY = 0, idxX = 0;

    if (CF_RGB != pII->cfColorFormat || Y_ONLY != pSCP->cfColorFormat)
        return;

    cWidth = 0 != pII->cROIWidth ? pII->cROIWidth : pII->cWidth;
    cHeight = 0 != pII->cROIHeight ? pII->cROIHeight : pII->cHeight;

#define fixup(type, nCh) \
for (idxY = 0; idxY < cHeight; ++idxY) \
{ \
    type * pT = (type *)((U8*)pBI->pv + pBI->cbStride * idxY); \
    for (idxX = 0; idxX < cWidth; ++idxX) \
    { \
        pT[2] = pT[1] = pT[0]; \
        pT += nCh; \
    } \
} \
break

    switch (pII->bdBitDepth)
    {
        case BD_8:
            fixup(U8, (pII->cBitsPerUnit >> 3));
            break;

        case BD_16:
        case BD_16S:
        case BD_16F:
            fixup(U16, (pII->cBitsPerUnit >> 3) / sizeof(U16));
            break;

        case BD_32:
        case BD_32S:
        case BD_32F:
            fixup(U32, (pII->cBitsPerUnit >> 3) / sizeof(float));
            break;

        case BD_5:
        case BD_10:
        case BD_565:
        default:
            break;
    }
}
#endif

/* centralized alpha channel color conversion, small perf penalty */
Int outputMBRowAlpha(CWMImageStrCodec * pSC)
{
    if(pSC->WMII.bdBitDepth == BD_8 && pSC->WMISCP.cfColorFormat == CF_RGB)  /* has been taken care of and optimized out */
        return ICERR_OK;

    if(pSC->m_bSecondary == FALSE && pSC->m_pNextSC != NULL){ /* with alpha channel */
        const BITDEPTH_BITS bd = pSC->WMII.bdBitDepth;
        const PixelI iShift = (pSC->m_param.bScaledArith ? SHIFTZERO + QPFRACBITS : 0);
        const size_t cHeight = min((pSC->m_Dparam->cROIBottomY + 1) - (pSC->cRow - 1) * 16, 16);
        const size_t cWidth = (pSC->m_Dparam->cROIRightX + 1);
        const size_t iFirstRow = ((pSC->cRow - 1) * 16 > pSC->m_Dparam->cROITopY ? 0 : (pSC->m_Dparam->cROITopY & 0xf)), iFirstColumn = pSC->m_Dparam->cROILeftX;
        const size_t iAlphaPos = pSC->WMII.cLeadingPadding + (pSC->WMII.cfColorFormat == CMYK ? 4 : 3);/*only RGB and CMYK may have interleaved alpha */
        const PixelI * pA = pSC->m_pNextSC->a0MBbuffer[0];
        const U8 nLen = pSC->WMISCP.nLenMantissaOrShift;
        const I8 nExpBias = pSC->WMISCP.nExpBias;
        size_t iRow, iColumn;
        size_t * pOffsetX = pSC->m_Dparam->pOffsetX, * pOffsetY = pSC->m_Dparam->pOffsetY + (pSC->cRow - 1) * 16, iY;
        
        if (CF_RGB != pSC->WMII.cfColorFormat && CMYK != pSC->WMII.cfColorFormat)
            return ICERR_ERROR;

        if(bd == BD_8){
            const PixelI iBias = (1 << (iShift + 7)) + (iShift == 0 ? 0 : (1 << (iShift - 1)));
            
            for(iRow = iFirstRow; iRow < cHeight; iRow ++)
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                    PixelI a = ((pA[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] + iBias) >> iShift);
                    ((U8 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY)[iAlphaPos] = _CLIP8(a);
                }
        }
        else if(bd == BD_16){
            const PixelI iBias = (1 << (iShift + 15)) + (iShift == 0 ? 0 : (1 << (iShift - 1)));

            for(iRow = iFirstRow; iRow < cHeight; iRow ++)
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                    PixelI a = (((pA[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] + iBias) >> iShift) << nLen);
                    ((U16 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY)[iAlphaPos] = _CLIPU16(a);
                }
        }
        else if(bd == BD_16S){
            const PixelI iBias = (iShift == 0 ? 0 : (1 << (iShift - 1)));
            
            for(iRow = iFirstRow; iRow < cHeight; iRow ++)
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                    PixelI a = (((pA[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] + iBias) >> iShift) << nLen);
                    ((I16 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY)[iAlphaPos] = _CLIP16(a);
                }
        }
        else if(bd == BD_16F){
            const PixelI iBias = (iShift == 0 ? 0 : (1 << (iShift - 1)));

            for(iRow = iFirstRow; iRow < cHeight; iRow ++)
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                    PixelI a = ((pA[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] + iBias) >> iShift);
                    ((U16 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY)[iAlphaPos] = backwardHalf(a);
                }
        }
        else if(bd == BD_32S){
            const PixelI iBias = (iShift == 0 ? 0 : (1 << (iShift - 1)));
            
            for(iRow = iFirstRow; iRow < cHeight; iRow ++)
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                    PixelI a = (((pA[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] + iBias) >> iShift) << nLen);
                    ((I32 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY)[iAlphaPos] = a;
                }
        }
        else if(bd == BD_32F){
            const PixelI iBias = (iShift == 0 ? 0 : (1 << (iShift - 1)));
            
            for(iRow = iFirstRow; iRow < cHeight; iRow ++)
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                    PixelI a = ((pA[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] + iBias) >> iShift);
                    ((float *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY)[iAlphaPos] = pixel2float (a, nExpBias, nLen);
                }
        }
        else /* not supported */
            return ICERR_ERROR;
    }

    return ICERR_OK;
}

Int outputMBRow(CWMImageStrCodec * pSC)
{
    const COLORFORMAT cfExt = (pSC->m_param.cfColorFormat == Y_ONLY ? Y_ONLY : pSC->WMII.cfColorFormat);
    const BITDEPTH_BITS bd = pSC->WMII.bdBitDepth;
    const PixelI iShift = (pSC->m_param.bScaledArith ? SHIFTZERO + QPFRACBITS : 0);
    const size_t cHeight = min((pSC->m_Dparam->cROIBottomY + 1) - (pSC->cRow - 1) * 16, 16);
    const size_t cWidth = (pSC->m_Dparam->cROIRightX + 1);
    const size_t iFirstRow = ((pSC->cRow - 1) * 16 > pSC->m_Dparam->cROITopY ? 0 : (pSC->m_Dparam->cROITopY & 0xf)), iFirstColumn = pSC->m_Dparam->cROILeftX;
    const PixelI *pY = pSC->a0MBbuffer[0];
    const PixelI *pU = (pSC->m_bUVResolutionChange ? pSC->pResU : pSC->a0MBbuffer[1]);
    const PixelI *pV = (pSC->m_bUVResolutionChange ? pSC->pResV : pSC->a0MBbuffer[2]);
    const PixelI *pA = NULL;
	const size_t iB = (pSC->WMII.bRGB ? 2 : 0);
	const size_t iR = 2 - iB;
    const U8 nLen = pSC->WMISCP.nLenMantissaOrShift;
    const I8 nExpBias = pSC->WMISCP.nExpBias;
    size_t iRow, iColumn, iIdx;
    size_t * pOffsetX = pSC->m_Dparam->pOffsetX, * pOffsetY = pSC->m_Dparam->pOffsetY + (pSC->cRow - 1) * (cfExt == YUV_420 ? 8 : 16), iY;


    if (pSC->m_pNextSC) {
        assert (pSC->m_param.bScaledArith == pSC->m_pNextSC->m_param.bScaledArith);  /* will be relaxed later */
    }

    /* guard output buffer */
    if(checkImageBuffer(pSC, pSC->WMII.oOrientation >= O_RCW ? pSC->WMII.cROIHeight : pSC->WMII.cROIWidth, cHeight - iFirstRow) != ICERR_OK)
        return ICERR_ERROR;

    if(pSC->m_bUVResolutionChange)
        interpolateUV(pSC);

    if(pSC->WMISCP.bYUVData){
        I32 * pDst = (I32 *)pSC->WMIBI.pv + (pSC->cRow - 1) *
            (pSC->m_param.cfColorFormat == YUV_420 ? 8 : 16) * pSC->WMIBI.cbStride / sizeof(I32);

        switch(pSC->m_param.cfColorFormat){
        case Y_ONLY:
        case YUV_444:
        case NCOMPONENT:
            {
                PixelI * pChannel[16];
                size_t iChannel;

                const size_t cChannel = pSC->WMII.cfColorFormat == Y_ONLY ? 1 : pSC->WMISCP.cChannel;
                assert(cChannel <= 16);

                for(iChannel = 0; iChannel < cChannel; iChannel ++)
                    pChannel[iChannel & 15] = pSC->a0MBbuffer[iChannel];

                for(iRow = iFirstRow; iRow < cHeight; iRow ++){
                    I32 * pRow = pDst;
                    for(iColumn = iFirstColumn; iColumn < cWidth; iColumn ++){
                        for(iChannel = 0; iChannel < cChannel; iChannel ++){
                            PixelI p = pChannel[iChannel & 15][((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf]];

                            *pRow++ = p;
                        }
                    }
                    pDst += pSC->WMIBI.cbStride / sizeof(I32);
                }
            }
            break;

        case YUV_422:
            {
				PixelI y0, y1, u, v;
	
				for(iRow = iFirstRow; iRow < cHeight; iRow ++){
                    I32 * pRow = pDst;
					for(iColumn = iFirstColumn; iColumn < cWidth; iColumn += 2){
						iIdx = ((iColumn >> 4) << 7) + idxCC[iRow][(iColumn >> 1) & 7];
						u = pU[iIdx], v = pV[iIdx];
	
						y0 = pY[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]];
						y1 = pY[(((iColumn + 1) >> 4) << 8) + idxCC[iRow][(iColumn + 1) & 15]];
						
						pRow[0] = u, pRow[1] = y0, pRow[2] = v, pRow[3] = y1;
                        pRow += 4;
					}
                    pDst += pSC->WMIBI.cbStride / sizeof(I32);
				}
			}
            break;

        case YUV_420:
			{
				PixelI y0, y1, y2, y3, u, v;
				/* const size_t iS4[8][4] = {{0, 1, 2, 3}, {2, 3, 0, 1}, {1, 0, 3, 2}, {3, 2, 1, 0}, {1, 3, 0, 2}, {3, 1, 2, 0}, {0, 2, 1, 3}, {2, 0, 3, 1}}; */
	
				for(iRow = iFirstRow; iRow < cHeight; iRow += 2){
                    I32 * pRow = pDst;
					for(iColumn = iFirstColumn; iColumn < cWidth; iColumn += 2){
						iIdx = ((iColumn >> 4) << 6) + idxCC_420[iRow >> 1][(iColumn >> 1) & 7];
						u = pU[iIdx], v = pV[iIdx];
	
						y0 = pY[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]];
						y1 = pY[(((iColumn + 1) >> 4) << 8) + idxCC[iRow][(iColumn + 1) & 15]];
						y2 = pY[((iColumn >> 4) << 8) + idxCC[iRow + 1][iColumn & 15]];
						y3 = pY[(((iColumn + 1) >> 4) << 8) + idxCC[iRow + 1][(iColumn + 1) & 15]];
	
						pRow[0] = y0, pRow[1] = y1, pRow[2] = y2, pRow[3] = y3, pRow[4] = u, pRow[5] = v;
                        pRow += 6;
					}
                    pDst += pSC->WMIBI.cbStride / sizeof(I32);
				}
			}
            break;

        default:
            assert(0);
            break;
        }
    }
    else if(bd == BD_8){
        U8 * pDst;
        const PixelI iBias1 = 128 << iShift;
        const PixelI iBias2 = pSC->m_param.bScaledArith ? ((1 << (SHIFTZERO + QPFRACBITS - 1)) - 1) : 0;
        const PixelI iBias = iBias1 + iBias2;

        switch(cfExt){
        case CF_RGB:
        {
            PixelI r, g, b, a;

            if (pSC->m_pNextSC && pSC->WMISCP.uAlphaMode > 0) { /* RGBA */

                pA = pSC->m_pNextSC->a0MBbuffer[0];

                if (pSC->m_param.bScaledArith == FALSE) {
                    for(iRow = iFirstRow; iRow < cHeight; iRow ++)
                        for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                            iIdx = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
                            
                            g = pY[iIdx] + iBias, r = -pU[iIdx], b = pV[iIdx];
                            a = pA[iIdx] + iBias;

                            _ICC(r, g, b);
                            
                            pDst = (U8 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY;
                            if ((g | b | r | a) & ~0xff)
                                pDst[iR] = _CLIP8(r), pDst[1] = _CLIP8(g), pDst[iB] = _CLIP8(b), pDst[3] = _CLIP8(a);
                            else
                                pDst[iR] = (U8)r, pDst[1] = (U8)g, pDst[iB] = (U8)b, pDst[3] = (U8)(a);
                        }
                }
                else{
                    for(iRow = iFirstRow; iRow < cHeight; iRow ++)
                        for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                            iIdx = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
                            
                            g = pY[iIdx] + iBias, r = -pU[iIdx], b = pV[iIdx];
                            a = pA[iIdx] + iBias;

                            _ICC(r, g, b);

                            g >>= iShift, b >>= iShift, r >>= iShift, a >>= iShift;
                            pDst = (U8 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY;
                            if ((g | b | r | a) & ~0xff)
                                pDst[iR] = _CLIP8(r), pDst[1] = _CLIP8(g), pDst[iB] = _CLIP8(b), pDst[3] = _CLIP8(a);
                            else
                                pDst[iR] = (U8)r, pDst[1] = (U8)g, pDst[iB] = (U8)b, pDst[3] = (U8)(a);
                        }
                }
            }
            else {
                if (pSC->m_param.bScaledArith == FALSE) {
                    for(iRow = iFirstRow; iRow < cHeight; iRow ++)
                        for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                            iIdx = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
                            
                            g = pY[iIdx] + iBias, r = -pU[iIdx], b = pV[iIdx];

                            _ICC(r, g, b);
                            
                            pDst = (U8 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY;
                            if ((g | b | r) & ~0xff)
                                pDst[iR] = _CLIP8(r), pDst[1] = _CLIP8(g), pDst[iB] = _CLIP8(b);
                            else
                                pDst[iR] = (U8)r, pDst[1] = (U8)g, pDst[iB] = (U8)b;
                        }
                }
                else{
                    for(iRow = iFirstRow; iRow < cHeight; iRow ++)
                        for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                            iIdx = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
                            
                            g = pY[iIdx] + iBias, r = -pU[iIdx], b = pV[iIdx];

                            _ICC(r, g, b);

                            g >>= iShift, b >>= iShift, r >>= iShift;
                            pDst = (U8 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY;
                            if ((g | b | r) & ~0xff)
                                pDst[iR] = _CLIP8(r), pDst[1] = _CLIP8(g), pDst[iB] = _CLIP8(b);
                            else
                                pDst[iR] = (U8)r, pDst[1] = (U8)g, pDst[iB] = (U8)b;
                        }
                }
            }
            break;
        }

        case Y_ONLY:
        case YUV_444:
        case NCOMPONENT:
            outputNChannel(pSC, iFirstRow, iFirstColumn, cWidth, cHeight, iShift, iBias);
            break;

        case YUV_422:
			{
				PixelI y0, y1, u, v;
				/* const ORIENTATION oO = pSC->WMII.oOrientation; */
				/* const size_t i0 = ((oO > O_FLIPV && oO <= O_RCW_FLIPVH) ? 1 : 0), i1 = 1 - i0; */
	
				for(iRow = iFirstRow; iRow < cHeight; iRow ++){
					for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn += 2){
						iIdx = ((iColumn >> 4) << 7) + idxCC[iRow][(iColumn >> 1) & 7];
						u = ((pU[iIdx] + iBias) >> iShift), v = ((pV[iIdx] + iBias) >> iShift);
	
						y0 = ((pY[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] + iBias) >> iShift);
						y1 = ((pY[(((iColumn + 1) >> 4) << 8) + idxCC[iRow][(iColumn + 1) & 15]] + iBias) >> iShift);
						
						pDst = (U8 *)pSC->WMIBI.pv + pOffsetX[iColumn >> 1] + iY;
						if ((y0 | y1 | u | v) & ~0xff)/*UYVY */
							pDst[0] = _CLIP8(u), pDst[1] = _CLIP8(y0), pDst[2] = _CLIP8(v), pDst[3] = _CLIP8(y1);
						else
							pDst[0] = (U8)u, pDst[1] = (U8)y0, pDst[2] = (U8)v, pDst[3] = (U8)y1;
					}
				}
			}
			break;

        case YUV_420:
			{
				PixelI y0, y1, y2, y3, u, v;
				const size_t iS4[8][4] = {{0, 1, 2, 3}, {2, 3, 0, 1}, {1, 0, 3, 2}, {3, 2, 1, 0}, {1, 3, 0, 2}, {3, 1, 2, 0}, {0, 2, 1, 3}, {2, 0, 3, 1}};
				const ORIENTATION oO = pSC->WMII.oOrientation;
				const size_t i0 = iS4[oO][0], i1 = iS4[oO][1], i2 = iS4[oO][2], i3 = iS4[oO][3];
	
				for(iRow = iFirstRow; iRow < cHeight; iRow += 2){
					for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> 1]; iColumn < cWidth; iColumn += 2){
						iIdx = ((iColumn >> 4) << 6) + idxCC_420[iRow >> 1][(iColumn >> 1) & 7];
						u = ((pU[iIdx] + iBias) >> iShift), v = ((pV[iIdx] + iBias) >> iShift);
	
						y0 = ((pY[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] + iBias) >> iShift);
						y1 = ((pY[(((iColumn + 1) >> 4) << 8) + idxCC[iRow][(iColumn + 1) & 15]] + iBias) >> iShift);
						y2 = ((pY[((iColumn >> 4) << 8) + idxCC[iRow + 1][iColumn & 15]] + iBias) >> iShift);
						y3 = ((pY[(((iColumn + 1) >> 4) << 8) + idxCC[iRow + 1][(iColumn + 1) & 15]] + iBias) >> iShift);
	
						pDst = (U8 *)pSC->WMIBI.pv + pOffsetX[iColumn >> 1] + iY;
						if ((y0 | y1 | y2 | y3 | u | v) & ~0xff)
							pDst[i0] = _CLIP8(y0), pDst[i1] = _CLIP8(y1), pDst[i2] = _CLIP8(y2), pDst[i3] = _CLIP8(y3), pDst[4] = _CLIP8(u), pDst[5] = _CLIP8(v);
						else
							pDst[i0] = (U8)y0, pDst[i1] = (U8)y1, pDst[i2] = (U8)y2, pDst[i3] = (U8)y3, pDst[4] = (U8)u, pDst[5] = (U8)v;
					}
				}
			}
			break;

        case CMYK:
			{
				PixelI c, m, y, k;
				PixelI * pK = pSC->a0MBbuffer[3];
	
				for(iRow = iFirstRow; iRow < cHeight; iRow++){
					for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn++){
						iIdx = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
	
						m = -pY[iIdx] + iBias1, c = pU[iIdx], y = -pV[iIdx], k = pK[iIdx] + iBias2;
						
						_ICC_CMYK(c, m, y, k); /* color conversion */
	
						c >>= iShift, m >>= iShift, y >>= iShift, k >>= iShift;
	
						pDst = (U8 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY;
						if ((c | m | y | k) & ~0xff)
							pDst[0] = _CLIP8(c), pDst[1] = _CLIP8(m), pDst[2] = _CLIP8(y), pDst[3] = _CLIP8(k);
						else
							pDst[0] = (U8)c, pDst[1] = (U8)m, pDst[2] = (U8)y, pDst[3] = (U8)k;
					}
				}
			}
			break;

        case CF_RGBE:
			{
				PixelI r, g, b;
				
				for(iRow = iFirstRow; iRow < cHeight; iRow ++){
					for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
							iIdx = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
							
							g = pY[iIdx] + iBias2, r = -pU[iIdx], b = pV[iIdx];
	
							_ICC(r, g, b);
	
							pDst = (U8 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY;
	
							inverseConvertRGBE (r >> iShift, g >> iShift, b >> iShift, pDst, pDst + 1, pDst + 2, pDst + 3);
						}
				}
			}
			break;

        default:
            assert(0);
            break;
    }
    }
    else if(bd == BD_16){
        const PixelI iBias = (((1 << 15) >> nLen) << iShift) + (iShift == 0 ? 0 : (1 << (iShift - 1)));
        U16 * pDst;

        switch(cfExt){
        case CF_RGB:
        {
            PixelI r, g, b;
			if (pSC->m_param.bScaledArith == FALSE) {
				for(iRow = iFirstRow; iRow < cHeight; iRow ++)
					for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
						iIdx = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
						
						g = pY[iIdx] + iBias, r = -pU[iIdx], b = pV[iIdx];

						_ICC(r, g, b);

						g <<= nLen, b <<= nLen, r <<= nLen;
						
						pDst = (U16 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY;
						
						if ((g | b | r) & ~0xffff)
							pDst[0] = _CLIPU16(r),  pDst[1] = _CLIPU16(g), pDst[2] = _CLIPU16(b);
						else
							pDst[0] = (U16)r, pDst[1] = (U16)g, pDst[2] = (U16)b;
					}
			}
			else{
				for(iRow = iFirstRow; iRow < cHeight; iRow ++)
					for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
						iIdx = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
						
						g = pY[iIdx] + iBias, r = -pU[iIdx], b = pV[iIdx];

						_ICC(r, g, b);

						g = (g >> iShift) << nLen, b = (b >> iShift) << nLen, r = (r >> iShift) << nLen;
						pDst = (U16 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY;
						if ((g | b | r) & ~0xffff)
							pDst[0] = _CLIPU16(r),  pDst[1] = _CLIPU16(g), pDst[2] = _CLIPU16(b);
						else
							pDst[0] = (U16)r, pDst[1] = (U16)g, pDst[2] = (U16)b;
					}
			}
            break;
        }

        case Y_ONLY:
        case YUV_444:
        case NCOMPONENT:
            outputNChannel(pSC, iFirstRow, iFirstColumn, cWidth, cHeight, iShift, iBias);
            break;

        case YUV_422:
			{
				PixelI y0, y1, u, v;
				const ORIENTATION oO = pSC->WMII.oOrientation;
				const size_t i0 = ((oO == O_FLIPH || oO == O_FLIPVH || oO == O_RCW_FLIPV || oO == O_RCW_FLIPVH) ? 1 : 0), i1 = 1 - i0;
	
				for(iRow = iFirstRow; iRow < cHeight; iRow ++){
					for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn += 2){
						iIdx = ((iColumn >> 4) << 7) + idxCC[iRow][(iColumn >> 1) & 7];
						u = ((pU[iIdx] + iBias) >> iShift) << nLen, v = ((pV[iIdx] + iBias) >> iShift) << nLen;
	
						y0 = ((pY[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] + iBias) >> iShift) << nLen;
						y1 = ((pY[(((iColumn + 1) >> 4) << 8) + idxCC[iRow][(iColumn + 1) & 15]] + iBias) >> iShift) << nLen;
						
						pDst = (U16 *)pSC->WMIBI.pv + pOffsetX[iColumn >> 1] + iY;
						if ((y0 | y1 | u | v) & ~0xffff)
							{
								pDst[i0] = _CLIPU16(u);
								pDst[i1] = _CLIPU16(y0); 
								pDst[2] = _CLIPU16(v); 
								pDst[3] = _CLIPU16(y1);
							}
						else
							{
								pDst[i0] = (U16)(u);
								pDst[i1] = (U16)(y0);
								pDst[2] = (U16)(v); 
								pDst[3] = (U16)(y1);
							}
					}
				}
			}
			break;

        case YUV_420:
			{
				PixelI y0, y1, y2, y3, u, v;
				const size_t iS4[8][4] = {{0, 1, 2, 3}, {2, 3, 0, 1}, {1, 0, 3, 2}, {3, 2, 1, 0}, {1, 3, 0, 2}, {3, 1, 2, 0}, {0, 2, 1, 3}, {2, 0, 3, 1}};
				const ORIENTATION oO = pSC->WMII.oOrientation;
				const size_t i0 = iS4[oO][0], i1 = iS4[oO][1], i2 = iS4[oO][2], i3 = iS4[oO][3];
	
				for(iRow = iFirstRow; iRow < cHeight; iRow += 2){
					for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> 1]; iColumn < cWidth; iColumn += 2){
						iIdx = ((iColumn >> 3) << 6) + idxCC[iRow][(iColumn >> 1) & 7];
						u = ((pU[iIdx] + iBias) >> iShift) << nLen, v = ((pV[iIdx] + iBias) >> iShift) << nLen;
	
						y0 = ((pY[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] + iBias) >> iShift) << nLen;
						y1 = ((pY[(((iColumn + 1) >> 4) << 8) + idxCC[iRow][(iColumn + 1) & 15]] + iBias) >> iShift) << nLen;
						y2 = ((pY[((iColumn >> 4) << 8) + idxCC[iRow + 1][iColumn & 15]] + iBias) >> iShift) << nLen;
						y3 = ((pY[(((iColumn + 1) >> 4) << 8) + idxCC[iRow + 1][(iColumn + 1) & 15]] + iBias) >> iShift) << nLen;
	
						pDst = (U16 *)pSC->WMIBI.pv + pOffsetX[iColumn >> 1] + iY;
						if ((y0 | y1 | y2 | y3 | u | v) & ~0xffff)
							{
								pDst[i0] = _CLIPU16(y0); 
								pDst[i1] = _CLIPU16(y1); 
								pDst[i2] = _CLIPU16(y2); 
								pDst[i3] = _CLIPU16(y3); 
								pDst[4] = _CLIPU16(u);
								pDst[5] = _CLIPU16(v);
							}
						else
							{
								pDst[i0] = (U16)(y0);
								pDst[i1] = (U16)(y1);
								pDst[i2] = (U16)(y2);
								pDst[i3] = (U16)(y3);
								pDst[4] = (U16)(u);
								pDst[5] = (U16)(v);
							}
					}
				}
			}
			break;

        case CMYK:
			{
				PixelI c, m, y, k;
				PixelI * pK = pSC->a0MBbuffer[3];
				const PixelI iBias1 = (32768 >> nLen) << iShift;
				const PixelI iBias2 = iBias - iBias1;
	
				for(iRow = iFirstRow; iRow < cHeight; iRow++){
					for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn++){
						iIdx = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
	
						m = -pY[iIdx] + iBias1, c = pU[iIdx], y = -pV[iIdx], k = pK[iIdx] + iBias2;
						
						_ICC_CMYK(c, m, y, k); /* color conversion */
	
						c = (c >> iShift) << nLen, m = (m >> iShift) << nLen, y = (y >> iShift) << nLen, k = (k >> iShift) << nLen;
	
						pDst = (U16 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY;
						if ((c | m | y | k) & ~0xffff)
							pDst[0] = _CLIPU16(c), pDst[1] = _CLIPU16(m), pDst[2] = _CLIPU16(y), pDst[3] = _CLIPU16(k);
						else
							pDst[0] = (U16)(c), pDst[1] = (U16)(m), pDst[2] = (U16)(y), pDst[3] = (U16)(k);
						}
				}
			}
			break; 
        default:
            assert(0);
            break;
    }
    }
    else if(bd == BD_16S){
        const PixelI iBias = pSC->m_param.bScaledArith ? ((1 << (SHIFTZERO + QPFRACBITS - 1)) - 1) : 0;
        I16 * pDst;

        switch(cfExt){
        case CF_RGB:
        {
            PixelI r, g, b;
            
            for(iRow = iFirstRow; iRow < cHeight; iRow ++)
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                    iIdx = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];

                    g = pY[iIdx] + iBias, r = -pU[iIdx], b = pV[iIdx];
                    
                    _ICC(r, g, b);
                    
                    r = (r >> iShift) << nLen, g = (g >> iShift) << nLen, b = (b >> iShift) << nLen;
                    
                    pDst = (I16 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY;
                    pDst[0] = _CLIP16(r), pDst[1] = _CLIP16(g), pDst[2] = _CLIP16(b);
                }
            break;
        }

        case Y_ONLY:
        case YUV_444:
        case NCOMPONENT:
            outputNChannel(pSC, iFirstRow, iFirstColumn, cWidth, cHeight, iShift, iBias);
            break;

        case CMYK:
			{
				PixelI c, m, y, k;
				PixelI * pK = pSC->a0MBbuffer[3];
	
				for(iRow = iFirstRow; iRow < cHeight; iRow++){
					for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn++){
						iIdx = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
	
						m = -pY[iIdx], c = pU[iIdx], y = -pV[iIdx], k = pK[iIdx] + iBias;
						
						_ICC_CMYK(c, m, y, k); /* color conversion */
	
						c = (c >> iShift) << nLen, m = (m >> iShift) << nLen, y = (y >> iShift) << nLen, k = (k >> iShift) << nLen;
	
						pDst = (I16 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY;
						pDst[0] = (I16)(c), pDst[1] = (I16)(m), pDst[2] = (I16)(y), pDst[3] = (I16)(k);
					}
				}
			}
			break;

        default:
            assert(0);
            break;
    }
    }
    else if(bd == BD_16F){
        const PixelI iBias = pSC->m_param.bScaledArith ? ((1 << (SHIFTZERO + QPFRACBITS - 1)) - 1) : 0;
        U16 *pDst;

        switch (cfExt)
        {
        case CF_RGB:
        {
            PixelI r, g, b;
            
            for(iRow = iFirstRow; iRow < cHeight; iRow ++){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                    iIdx = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
                    
                    g = pY[iIdx] + iBias, r = -pU[iIdx], b = pV[iIdx];
                    
                    _ICC(r, g, b);
                                        
                    pDst = (U16 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY;
                    pDst[0] = backwardHalf(r >> iShift);
                    pDst[1] = backwardHalf(g >> iShift);
                    pDst[2] = backwardHalf(b >> iShift);
                }
            }
            break;
        }

        case Y_ONLY:
        case YUV_444:
        case NCOMPONENT:
            outputNChannel(pSC, iFirstRow, iFirstColumn, cWidth, cHeight, iShift, iBias);
            break;

        default:
            assert(0);
            break;
        }
    }
    else if(bd == BD_32){
        const PixelI iBias = (((1 << 31) >> nLen) << iShift) + (iShift == 0 ? 0 : (1 << (iShift - 1)));
        U32 * pDst;

        switch (cfExt)
        {
        case CF_RGB:
			{
				PixelI r, g, b;
				
				for(iRow = iFirstRow; iRow < cHeight; iRow ++){
					for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
						iIdx = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
						
						g = pY[iIdx] + iBias, r = -pU[iIdx], b = pV[iIdx];
	
						_ICC(r, g, b);
						
						pDst = (U32 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY;
						pDst[0] = ((r >> iShift) << nLen);
						pDst[1] = ((g >> iShift) << nLen);
						pDst[2] = ((b >> iShift) << nLen);
					}
				}
			}
			break;

        case Y_ONLY:
        case YUV_444:
        case NCOMPONENT:
        {
            outputNChannel(pSC, iFirstRow, iFirstColumn, cWidth, cHeight, iShift, iBias);
            break;    
        }
        default:
            assert(0);
            break;
        }
    }
    else if(bd == BD_32S){
        const PixelI iBias = pSC->m_param.bScaledArith ? ((1 << (SHIFTZERO + QPFRACBITS - 1)) - 1) : 0;
        int * pDst;

        switch (cfExt)
        {
        case CF_RGB:
        {
            PixelI r, g, b;
            
            for(iRow = iFirstRow; iRow < cHeight; iRow ++){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                    iIdx = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
                    
                    g = pY[iIdx] + iBias, r = -pU[iIdx], b = pV[iIdx];
                    
                    _ICC(r, g, b);
                                        
                    pDst = (int *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY;
                    pDst[0] = ((r >> iShift) << nLen);
                    pDst[1] = ((g >> iShift) << nLen);
                    pDst[2] = ((b >> iShift) << nLen);
                }
            }
            break;
        }

        case Y_ONLY:
        case YUV_444:
        case NCOMPONENT:
            outputNChannel(pSC, iFirstRow, iFirstColumn, cWidth, cHeight, iShift, iBias);
            break;    

		default:
            assert(0);
            break;
        }
    }
    else if(bd == BD_32F){
        const PixelI iBias = pSC->m_param.bScaledArith ? ((1 << (SHIFTZERO + QPFRACBITS - 1)) - 1) : 0;
        float * pDst;

        switch (cfExt)
        {
        case CF_RGB:
        {
            PixelI r, g, b;
            
            for(iRow = iFirstRow; iRow < cHeight; iRow ++){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                    iIdx = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
                    
                    g = pY[iIdx] + iBias, r = -pU[iIdx], b = pV[iIdx];
                    
                    _ICC(r, g, b);
                                        
                    pDst = (float *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY;
                    pDst[0] = pixel2float (r >> iShift, nExpBias, nLen);
                    pDst[1] = pixel2float (g >> iShift, nExpBias, nLen);
                    pDst[2] = pixel2float (b >> iShift, nExpBias, nLen);
                }
            }
            break;
        }
        case Y_ONLY:
        case YUV_444:
        case NCOMPONENT:
            outputNChannel(pSC, iFirstRow, iFirstColumn, cWidth, cHeight, iShift, iBias);
            break;    

        default:
            assert(0);
            break;
        }
    }
    else if(bd == BD_5){
        const PixelI iBias = (16 << iShift) + (pSC->m_param.bScaledArith ? ((1 << (SHIFTZERO + QPFRACBITS - 1)) - 1) : 0);
        PixelI r, g, b;
        U16 * pDst;

        assert(cfExt == CF_RGB);

        for(iRow = iFirstRow; iRow < cHeight; iRow ++)
            for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                iIdx = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
                
                g = pY[iIdx] + iBias, r = -pU[iIdx], b = pV[iIdx];

                _ICC(r, g, b);
                
                g >>= iShift, b >>= iShift, r >>= iShift;
                pDst = (U16 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY;
                if (pSC->m_param.bRBSwapped)
                    pDst[0] = (U16)_CLIP2(0, b, 31) + (((U16)_CLIP2(0, g, 31)) << 5) + (((U16)_CLIP2(0, r, 31)) << 10);
                else
                    pDst[0] = (U16)_CLIP2(0, r, 31) + (((U16)_CLIP2(0, g, 31)) << 5) + (((U16)_CLIP2(0, b, 31)) << 10);
            }
    }
    else if(bd == BD_565){
        const PixelI iBias = (32 << iShift) + (pSC->m_param.bScaledArith ? ((1 << (SHIFTZERO + QPFRACBITS - 1)) - 1) : 0);
        PixelI r, g, b;
        U16 * pDst;

        assert(cfExt == CF_RGB);

        for(iRow = iFirstRow; iRow < cHeight; iRow ++)
            for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                iIdx = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
                
                g = pY[iIdx] + iBias, r = -pU[iIdx], b = pV[iIdx];

                _ICC(r, g, b);
                
                g >>= iShift, b >>= iShift + 1, r >>= iShift + 1;
                pDst = (U16 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY;
                if (pSC->m_param.bRBSwapped)
                    pDst[0] = (U16)_CLIP2(0, b, 31) + (((U16)_CLIP2(0, g, 63)) << 5) + (((U16)_CLIP2(0, r, 31)) << 11);
                else
                    pDst[0] = (U16)_CLIP2(0, r, 31) + (((U16)_CLIP2(0, g, 63)) << 5) + (((U16)_CLIP2(0, b, 31)) << 11);
            }
    }
    else if(bd == BD_10){
        const PixelI iBias = (512 << iShift) + (pSC->m_param.bScaledArith ? ((1 << (SHIFTZERO + QPFRACBITS - 1)) - 1) : 0);
        PixelI r, g, b;
        U32 * pDst;

        assert(cfExt == CF_RGB);

        for(iRow = iFirstRow; iRow < cHeight; iRow ++)
            for(iColumn = iFirstColumn, iY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){
                iIdx = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
                
                g = pY[iIdx] + iBias, r = -pU[iIdx], b = pV[iIdx];

                _ICC(r, g, b);
                
                g >>= iShift, b >>= iShift, r >>= iShift;

                pDst = (U32 *)pSC->WMIBI.pv + pOffsetX[iColumn] + iY;
                if (pSC->m_param.bRBSwapped)
                    pDst[0] = (U32)_CLIP2(0, b, 1023) + 
                        (((U32)_CLIP2(0, g, 1023)) << 10) + 
                        (((U32)_CLIP2(0, r, 1023)) << 20);
                else
                    pDst[0] = (U32)_CLIP2(0, r, 1023) + 
                        (((U32)_CLIP2(0, g, 1023)) << 10) + 
                        (((U32)_CLIP2(0, b, 1023)) << 20);
            }
    }
    else if(bd == BD_1){
        const size_t iPos = pSC->WMII.cLeadingPadding;
        const Int iTh = (iShift > 0) ? (1 << (iShift - 1)) : 1;
        assert(cfExt == Y_ONLY && pSC->m_param.cfColorFormat == Y_ONLY);
        
        if(pSC->WMII.oOrientation < O_RCW)
            for(iRow = iFirstRow; iRow < cHeight; iRow ++) {
                iY = pOffsetY[iRow] + iPos;
                for(iColumn = iFirstColumn; iColumn < cWidth; iColumn ++) {
                    U8 cByte = ((U8 *)pSC->WMIBI.pv + (pOffsetX[iColumn] >> 3) + iY)[0];
                    U8 cShift = (U8)(7 - (pOffsetX[iColumn] & 7));
                    ((U8 *)pSC->WMIBI.pv + (pOffsetX[iColumn] >> 3) + iY)[0] ^= /* exor is used because we can't assume the byte was originally zero */
                    (((pSC->WMISCP.bBlackWhite + (pY[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] >= iTh)
                    + (cByte >> cShift)) & 0x1) << cShift);
                }
            }
        else
            for(iRow = iFirstRow; iRow < cHeight; iRow ++) {
                iY = pOffsetY[iRow] + iPos;
                for(iColumn = iFirstColumn; iColumn < cWidth; iColumn ++) {
                    U8 cByte = ((U8 *)pSC->WMIBI.pv + pOffsetX[iColumn] + (iY >> 3))[0];
                    U8 cShift = (U8)(7 - (iY & 7));  /* should be optimized out */
                    ((U8 *)pSC->WMIBI.pv + pOffsetX[iColumn] + (iY >> 3))[0] ^= /* exor is used because we can't assume the byte was originally zero */
                    (((pSC->WMISCP.bBlackWhite + (pY[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] >= iTh)
                    + (cByte >> cShift)) & 0x1) << cShift);
                }
            }
    }

    if(pSC->WMISCP.uAlphaMode > 0)
        if(outputMBRowAlpha(pSC) != ICERR_OK)
            return ICERR_ERROR;

#ifdef REENTRANT_MODE
    pSC->WMIBI.cLinesDecoded = cHeight - iFirstRow;

    if (CF_RGB == pSC->WMII.cfColorFormat && Y_ONLY == pSC->WMISCP.cfColorFormat)
    {
        const CWMImageInfo* pII = &pSC->WMII;

#define fixupFullSize(type, nCh) \
for(iRow = iFirstRow; iRow < cHeight; iRow ++) {\
    size_t iOffsetY;\
    for(iColumn = iFirstColumn, iOffsetY = pOffsetY[iRow]; iColumn < cWidth; iColumn ++){\
        type *pT = (type*)(U8 *)pSC->WMIBI.pv + iOffsetY + pOffsetX[iColumn];\
        pT[2] = pT[1] = pT[0]; \
        pT += nCh; \
    } \
} \
break

        switch (pII->bdBitDepth)
        {
            case BD_8:
                fixupFullSize(U8, (pII->cBitsPerUnit >> 3));
                break;

            case BD_16:
            case BD_16S:
            case BD_16F:
                fixupFullSize(U16, (pII->cBitsPerUnit >> 3) / sizeof(U16));
                break;

            case BD_32:
            case BD_32S:
            case BD_32F:
                fixupFullSize(U32, (pII->cBitsPerUnit >> 3) / sizeof(float));
                break;

            case BD_5:
            case BD_10:
            case BD_565:
            default:
                break;
        }
    }
#endif

    return ICERR_OK;
}

/* Y_ONLY/CF_ALPHA/YUV_444/N_CHANNEL thumbnail decode */
Void outputNChannelThumbnail(CWMImageStrCodec * pSC, const PixelI cMul, const size_t rShiftY, size_t iFirstRow, size_t iFirstColumn)
{
    const size_t tScale = pSC->m_Dparam->cThumbnailScale;
    const size_t cWidth = (pSC->m_Dparam->cROIRightX + 1);
    const size_t cHeight = min((pSC->m_Dparam->cROIBottomY + 1) - (pSC->cRow - 1) * 16, 16);
    const size_t cChannel = pSC->WMISCP.cChannel;
    const U8 nLen = pSC->WMISCP.nLenMantissaOrShift;
    const I8 nExpBias = pSC->WMISCP.nExpBias;
    size_t nBits = 0;
    PixelI iOffset;
    PixelI * pChannel[16];
    size_t iChannel, iRow, iColumn;
    size_t * pOffsetX = pSC->m_Dparam->pOffsetX, * pOffsetY = pSC->m_Dparam->pOffsetY + (pSC->cRow - 1) * 16 / tScale, iY;

    while((size_t)(1U << nBits) < tScale)
        nBits ++;

    assert(cChannel <= 16);

    for(iChannel = 0; iChannel < cChannel; iChannel ++)
        pChannel[iChannel & 15] = pSC->a0MBbuffer[iChannel];

    if(pSC->m_bUVResolutionChange)
        pChannel[1] = pSC->pResU, pChannel[2] = pSC->pResV;

    switch(pSC->WMII.bdBitDepth){
        case BD_8:
            for(iOffset = (128 << rShiftY) / cMul, iRow = iFirstRow; iRow < cHeight; iRow += tScale){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                    U8 * pDst = (U8 *)pSC->WMIBI.pv + iY + pOffsetX[iColumn >> nBits];

                    for(iChannel = 0; iChannel < cChannel; iChannel ++){
                        PixelI p = ((pChannel[iChannel & 15][((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] + iOffset) * cMul) >> rShiftY;
                        
                        pDst[iChannel] = _CLIP8(p);
                    }
                }
            }
            break;

        case BD_16:
            for(iOffset = (32768 << rShiftY) / cMul, iRow = iFirstRow; iRow < cHeight; iRow += tScale){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                    U16 * pDst = (U16 *)pSC->WMIBI.pv + iY + pOffsetX[iColumn >> nBits];

                    for(iChannel = 0; iChannel < cChannel; iChannel ++){
                        PixelI p = (((pChannel[iChannel & 15][((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] + iOffset) * cMul) >> rShiftY) << nLen;
                        
                        pDst[iChannel] = _CLIPU16(p);
                    }
                }
            }
            break;

        case BD_16S:
            for(iRow = iFirstRow; iRow < cHeight; iRow += tScale){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                    I16 * pDst = (I16 *)pSC->WMIBI.pv + iY + pOffsetX[iColumn >> nBits];

                    for(iChannel = 0; iChannel < cChannel; iChannel ++){
                        PixelI p = ((pChannel[iChannel & 15][((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] * cMul) >> rShiftY) << nLen;
                        
                        pDst[iChannel] = _CLIP16(p);
                    }
                }
            }
            break;

        case BD_16F:
            for(iRow = iFirstRow; iRow < cHeight; iRow += tScale){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                    U16 * pDst = (U16 *)pSC->WMIBI.pv + iY + pOffsetX[iColumn >> nBits];

                    for(iChannel = 0; iChannel < cChannel; iChannel ++){
                        PixelI p = (pChannel[iChannel & 15][((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] * cMul) >> rShiftY;
                        
                        pDst[iChannel] = backwardHalf(p);
                    }
                }
            }
            break;
        case BD_32:
            for(iOffset = (((1 << 31) >> nLen) << rShiftY) / cMul, iRow = iFirstRow; iRow < cHeight; iRow += tScale){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                    U32 * pDst = (U32 *)pSC->WMIBI.pv + iY + pOffsetX[iColumn >> nBits];

                    for(iChannel = 0; iChannel < cChannel; iChannel ++){
                        PixelI p = (((pChannel[iChannel & 15][((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] + iOffset) * cMul) >> rShiftY) << nLen;
                        
                        pDst[iChannel] = (U32)(p);
                    }
                }
            }
            break;
        case BD_32S:
            for(iRow = iFirstRow; iRow < cHeight; iRow += tScale){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                    I32 * pDst = (I32 *)pSC->WMIBI.pv + iY + pOffsetX[iColumn >> nBits];

                    for(iChannel = 0; iChannel < cChannel; iChannel ++){
                        PixelI p = ((pChannel[iChannel & 15][((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] * cMul) >> rShiftY) << nLen;
                        
                        pDst[iChannel] = (I32)(p);
                    }
                }
            }
            break;
        case BD_32F:
            for(iRow = iFirstRow; iRow < cHeight; iRow += tScale){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                    float * pDst = (float *)pSC->WMIBI.pv + iY + pOffsetX[iColumn >> nBits];

                    for(iChannel = 0; iChannel < cChannel; iChannel ++){
                        PixelI p = (pChannel[iChannel & 15][((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] * cMul) >> rShiftY;
                        
                        pDst[iChannel] = pixel2float (p, nExpBias, nLen);
                    }
                }
            }
            break;

        default:
            assert(0);
            break;
    }
}

/* centralized alpha channel thumbnail, small perf penalty */
Int decodeThumbnailAlpha(CWMImageStrCodec * pSC, const size_t nBits, const PixelI cMul, const size_t rShiftY)
{
    if(pSC->m_bSecondary == FALSE && pSC->m_pNextSC != NULL){ /* with alpha channel */
        const size_t tScale = (size_t)(1U << nBits);
        const size_t cHeight = min((pSC->m_Dparam->cROIBottomY + 1) - (pSC->cRow - 1) * 16, 16);
        const size_t cWidth = (pSC->m_Dparam->cROIRightX + 1);
        const size_t iFirstRow = ((((pSC->cRow - 1) * 16 > pSC->m_Dparam->cROITopY ? 0 : (pSC->m_Dparam->cROITopY & 0xf)) + tScale - 1) / tScale * tScale);
        const size_t iFirstColumn = (pSC->m_Dparam->cROILeftX + tScale - 1) / tScale * tScale;
        const size_t iAlphaPos = pSC->WMII.cLeadingPadding + (pSC->WMII.cfColorFormat == CMYK ? 4 : 3);/*only RGB and CMYK may have interleaved alpha */
        const BITDEPTH_BITS bd = pSC->WMII.bdBitDepth;
        const PixelI * pSrc = pSC->m_pNextSC->a0MBbuffer[0];
        const U8 nLen = pSC->m_pNextSC->WMISCP.nLenMantissaOrShift;
        const I8 nExpBias = pSC->m_pNextSC->WMISCP.nExpBias;
        size_t iRow, iColumn;
        size_t * pOffsetX = pSC->m_Dparam->pOffsetX, * pOffsetY = pSC->m_Dparam->pOffsetY + (pSC->cRow - 1) * 16 / tScale, iY;
        
        if (CF_RGB != pSC->WMII.cfColorFormat && CMYK != pSC->WMII.cfColorFormat)
            return ICERR_ERROR;

        if(bd == BD_8){
            const PixelI offset = (128 << rShiftY) / cMul;

            for(iRow = iFirstRow; iRow < cHeight; iRow += tScale)
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                    PixelI a = ((pSrc[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf]] + offset) * cMul) >> rShiftY;
                    
                    ((U8 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY)[iAlphaPos] = _CLIP8(a);
                }
        }
        else if(bd == BD_16){
            const PixelI offset = (32768 << rShiftY) / cMul;

            for(iRow = iFirstRow; iRow < cHeight; iRow += tScale)
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                    PixelI a = (((pSrc[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf]] + offset) * cMul) >> rShiftY) << nLen;

                    ((U16 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY)[iAlphaPos] = _CLIPU16(a);
                }
        }
        else if(bd == BD_16S){
            for(iRow = iFirstRow; iRow < cHeight; iRow += tScale)
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                    PixelI a = ((pSrc[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf]] * cMul) >> rShiftY) << nLen;

                    ((I16 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY)[iAlphaPos] = _CLIP16(a);
                }
        }
        else if(bd == BD_16F){
            for(iRow = iFirstRow; iRow < cHeight; iRow += tScale)
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                    PixelI a = (pSrc[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf]] * cMul) >> rShiftY;

                    ((U16 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY)[iAlphaPos] = backwardHalf(a);
                }
        }
        else if(bd == BD_32S){
            for(iRow = iFirstRow; iRow < cHeight; iRow += tScale)
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                    PixelI a = ((pSrc[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf]] * cMul) >> rShiftY) << nLen;

                    ((I32 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY)[iAlphaPos] = a;
                }
        }
        else if(bd == BD_32F){
            for(iRow = iFirstRow; iRow < cHeight; iRow += tScale)
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                    PixelI a = (pSrc[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf]] * cMul) >> rShiftY;

                    ((float *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY)[iAlphaPos] = pixel2float (a, nExpBias, nLen);
                }
        }
        else /* not supported */
            return ICERR_ERROR;
    }

    return ICERR_OK;
}

Int decodeThumbnail(CWMImageStrCodec * pSC)
{
    const size_t tScale = pSC->m_Dparam->cThumbnailScale;
    const size_t cHeight = min((pSC->m_Dparam->bDecodeFullFrame ? pSC->WMII.cHeight : pSC->m_Dparam->cROIBottomY + 1) - (pSC->cRow - 1) * 16, 16);
    const size_t cWidth = (pSC->m_Dparam->bDecodeFullFrame ? pSC->WMII.cWidth : pSC->m_Dparam->cROIRightX + 1);
    const size_t iFirstRow = ((((pSC->cRow - 1) * 16 > pSC->m_Dparam->cROITopY ? 0 : (pSC->m_Dparam->cROITopY & 0xf)) + tScale - 1) / tScale * tScale);
    const size_t iFirstColumn = (pSC->m_Dparam->cROILeftX + tScale - 1) / tScale * tScale;
    const COLORFORMAT cfInt = pSC->m_param.cfColorFormat;
    const COLORFORMAT cfExt = (pSC->m_param.cfColorFormat == Y_ONLY ? Y_ONLY : pSC->WMII.cfColorFormat);
    const BITDEPTH_BITS bd = pSC->WMII.bdBitDepth;
    const OVERLAP ol = pSC->WMISCP.olOverlap;
	const size_t iB = (pSC->WMII.bRGB ? 2 : 0);
    const size_t iR = 2 - iB;

    const U8 nLen = pSC->WMISCP.nLenMantissaOrShift;
    const I8 nExpBias = pSC->WMISCP.nExpBias;
    PixelI offset;
    size_t iRow, iColumn, iIdx1, iIdx2, iIdx3 = 0, nBits = 0;
    PixelI * pSrcY = pSC->a0MBbuffer[0];
    PixelI * pSrcU = pSC->a0MBbuffer[1], * pSrcV = pSC->a0MBbuffer[2];
    size_t * pOffsetX = pSC->m_Dparam->pOffsetX, * pOffsetY = pSC->m_Dparam->pOffsetY + (pSC->cRow - 1) * 16 / tScale, iY;
    const PixelI cMul = (tScale >= 16 ? (ol == OL_NONE ? 16 : (ol == OL_ONE ? 23 : 34)) : (tScale >= 4 ? (ol == OL_NONE ? 64 : 93) : 258));
    const size_t rShiftY = 8 + (pSC->m_param.bScaledArith ? (SHIFTZERO + QPFRACBITS) : 0);
    const size_t rShiftUV = rShiftY - ((pSC->m_param.bScaledArith && tScale >= 16) ? ((cfInt == YUV_420 || cfInt == YUV_422) ? 2 : 1) : 0);

    while((size_t)(1U << nBits) < tScale)
        nBits ++;

    assert(tScale == (size_t)(1U << nBits));

    /* guard output buffer */
    if(checkImageBuffer(pSC, pSC->WMII.oOrientation < O_RCW ? pSC->WMII.cROIWidth : pSC->WMII.cROIHeight, (cHeight - iFirstRow) / pSC->m_Dparam->cThumbnailScale) != ICERR_OK)
        return ICERR_ERROR;

    if((((pSC->cRow - 1) * 16) % tScale) != 0)
        return ICERR_OK;

    if(pSC->cRow * 16 <= pSC->m_Dparam->cROITopY || pSC->cRow * 16 > pSC->m_Dparam->cROIBottomY + 16)
        return ICERR_OK;
    
    if((cfInt == YUV_422 || cfInt == YUV_420) && cfExt != Y_ONLY){
        PixelI * pDstU = pSC->pResU, * pDstV = pSC->pResV;

        for(iRow = 0; iRow < 16; iRow += tScale){
            for(iColumn = 0; iColumn < cWidth; iColumn += tScale){
                iIdx1 = (cfInt == YUV_422 ? ((iColumn >> 4) << 7) + idxCC[iRow][(iColumn >> 1) & 7] : ((iColumn >> 4) << 6) + idxCC_420[iRow >> 1][(iColumn >> 1) & 7]);
                iIdx2 = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];

                /* copy over */
                pDstU[iIdx2] = pSrcU[iIdx1];
                pDstV[iIdx2] = pSrcV[iIdx1];
            }
        }

        if(tScale == 4){
            if(cfInt == YUV_420){
                for(iColumn = 0; iColumn < cWidth; iColumn += 8){
                    iIdx1 = ((iColumn >> 4) << 8) + idxCC[0][iColumn & 15];
                    iIdx2 = ((iColumn >> 4) << 8) + idxCC[4][iColumn & 15];
                    iIdx3 = ((iColumn >> 4) << 8) + idxCC[8][iColumn & 15];

                    pDstU[iIdx2] = ((pDstU[iIdx1] + pDstU[iIdx3] + 1) >> 1);
                    pDstV[iIdx2] = ((pDstV[iIdx1] + pDstV[iIdx3] + 1) >> 1);

                    iIdx1 = ((iColumn >> 4) << 8) + idxCC[12][iColumn & 15];
                    pDstU[iIdx1] = pDstU[iIdx3];
                    pDstV[iIdx1] = pDstV[iIdx3];
                }
            }

            for(iRow = 0; iRow < 16; iRow += 4){
                for(iColumn = 0; iColumn < cWidth - 8; iColumn += 8){
                    iIdx1 = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15];
                    iIdx2 = ((iColumn >> 4) << 8) + idxCC[iRow][(iColumn + 4) & 15];
                    iIdx3 = ((iColumn >> 4) << 8) + idxCC[iRow][(iColumn + 8) & 15];

                    pDstU[iIdx2] = ((pDstU[iIdx1] + pDstU[iIdx3] + 1) >> 1);
                    pDstV[iIdx2] = ((pDstV[iIdx1] + pDstV[iIdx3] + 1) >> 1);
                }

                iIdx2 = ((iColumn >> 4) << 8) + idxCC[iRow][(iColumn + 4) & 15];
                pDstU[iIdx2] = pDstU[iIdx3];
                pDstV[iIdx2] = pDstV[iIdx3];
            }
        }

        pSrcU = pDstU, pSrcV = pDstV;
    }

    if(bd == BD_8){
        U8 * pDst;

        offset = (128 << rShiftY) / cMul;

        switch(cfExt){
            case CF_RGB:
                for(iRow = iFirstRow; iRow < cHeight; iRow += tScale){
                    for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                        size_t iPos = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf];
                        PixelI g = ((pSrcY[iPos] + offset) * cMul) >> rShiftY, r = -(pSrcU[iPos] * cMul) >> rShiftUV, b = (pSrcV[iPos] * cMul) >> rShiftUV;

                        _ICC(r, g, b);

                        pDst = (U8 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY;
                        pDst[iB] = _CLIP8(b), pDst[1] = _CLIP8(g), pDst[iR] = _CLIP8(r);
                }
            }
            break;

        case Y_ONLY:
        case YUV_444:
        case NCOMPONENT:
            outputNChannelThumbnail(pSC, cMul, rShiftY, iFirstRow, iFirstColumn);
            break;

        case CF_RGBE:
            for(iRow = iFirstRow; iRow < cHeight; iRow += tScale){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                    size_t iPos = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf];
                    PixelI g = ((pSrcY[iPos] * cMul) >> rShiftY), r = - ((pSrcU[iPos] * cMul) >> rShiftUV), b = ((pSrcV[iPos] * cMul) >> rShiftUV);
                    
                    _ICC(r, g, b);
                    
                    pDst = (U8 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY;
                    inverseConvertRGBE (r, g, b, pDst, pDst + 1, pDst + 2, pDst + 3);
                }
            }
            break;

        case CMYK:
        {
            PixelI * pSrcK = pSC->a0MBbuffer[3];
            PixelI iBias1 = (128 << rShiftY) / cMul, iBias2 = (((128 << rShiftUV) / cMul) >> 1);

            for(iRow = iFirstRow; iRow < cHeight; iRow += tScale){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                    size_t iPos = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf];
                    PixelI m = ((-pSrcY[iPos] + iBias1) * cMul) >> rShiftY, c = (pSrcU[iPos] * cMul) >> rShiftUV, y = -(pSrcV[iPos] * cMul) >> rShiftUV, k = ((pSrcK[iPos] + iBias2) * cMul) >> rShiftUV;
                    
                    _ICC_CMYK(c, m, y, k);
                    
                    pDst = (U8 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY;
                    pDst[0] = _CLIP8(c), pDst[1] = _CLIP8(m), pDst[2] = _CLIP8(y), pDst[3] = _CLIP8(k);
                }
            }
            break;
        }
        default:
            assert(0);
            break;
        }
    }
    if(bd == BD_16){
        U16 * pDst;

        offset = (((1 << 15) >> nLen) << rShiftY) / cMul;

        switch(cfExt){
            case CF_RGB:
                for(iRow = iFirstRow; iRow < cHeight; iRow += tScale){
                    for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                        size_t iPos = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf];
                        PixelI g = ((pSrcY[iPos] + offset) * cMul) >> rShiftY, r = -(pSrcU[iPos] * cMul) >> rShiftUV, b = (pSrcV[iPos] * cMul) >> rShiftUV;

                        _ICC(r, g, b);
                                                
                        pDst = (U16 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY;
                        r <<= nLen, g <<= nLen, b <<= nLen;
                        pDst[0] = _CLIPU16(r);
                        pDst[1] = _CLIPU16(g);
                        pDst[2] = _CLIPU16(b);
                }
            }
            break;

        case Y_ONLY:
        case YUV_444:
        case NCOMPONENT:
            outputNChannelThumbnail(pSC, cMul, rShiftY, iFirstRow, iFirstColumn);
            break;

        case CMYK:
        {
            PixelI * pSrcK = pSC->a0MBbuffer[3];
            PixelI iBias1 = (32768 << rShiftY) / cMul, iBias2 = (((32768 << rShiftUV) / cMul) >> 1);

            for(iRow = iFirstRow; iRow < cHeight; iRow += tScale){
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                    size_t iPos = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf];
                    PixelI m = ((-pSrcY[iPos] + iBias1) * cMul) >> rShiftY, c = (pSrcU[iPos] * cMul) >> rShiftUV, y = -(pSrcV[iPos] * cMul) >> rShiftUV, k = ((pSrcK[iPos] + iBias2) * cMul) >> rShiftUV;
                    
                    _ICC_CMYK(c, m, y, k);
                    
                    pDst = (U16 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY;
                    c <<= nLen, m <<= nLen, y <<= nLen, k <<= nLen;
                    pDst[0] = _CLIPU16(c);
                    pDst[1] = _CLIPU16(m);
                    pDst[2] = _CLIPU16(y);
                    pDst[3] = _CLIPU16(k);
                }
            }
            break;
        }
        default:
            assert(0);
            break;
        }
    }
    if(bd == BD_16S){
        I16 * pDst;

        switch(cfExt){
            case CF_RGB:
                for(iRow = iFirstRow; iRow < cHeight; iRow += tScale){
                    for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                        size_t iPos = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf];
                        PixelI g = (pSrcY[iPos] * cMul) >> rShiftY, r = -(pSrcU[iPos] * cMul) >> rShiftUV, b = (pSrcV[iPos] * cMul) >> rShiftUV;

                        _ICC(r, g, b);
                                                
                        pDst = (I16 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY;
                        r <<= nLen, g <<= nLen, b <<= nLen;
                        pDst[0] = _CLIP16(r);
                        pDst[1] = _CLIP16(g);
                        pDst[2] = _CLIP16(b);
                }
            }
            break;

        case Y_ONLY:
        case YUV_444:
        case NCOMPONENT:
            outputNChannelThumbnail(pSC, cMul, rShiftY, iFirstRow, iFirstColumn);
            break;

        case CMYK:
			{
				PixelI * pSrcK = pSC->a0MBbuffer[3];
	
				for(iRow = iFirstRow; iRow < cHeight; iRow += tScale){
					for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
						size_t iPos = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf];
						PixelI m = -(pSrcY[iPos] * cMul) >> rShiftY, c = (pSrcU[iPos] * cMul) >> rShiftUV, y = -(pSrcV[iPos] * cMul) >> rShiftUV, k = (pSrcK[iPos] * cMul) >> rShiftUV;
						
						_ICC_CMYK(c, m, y, k);
	
						pDst = (I16 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY;
						c <<= nLen, m <<= nLen, y <<= nLen, k <<= nLen;
						pDst[0] = _CLIP16(c); 
						pDst[1] = _CLIP16(m);
						pDst[2] = _CLIP16(y);
						pDst[3] = _CLIP16(k);
					}
				}
			}
			break;

        default:
            assert(0);
            break;
        }
    }
    else if(bd == BD_16F){
        U16 * pDst;

        switch(cfExt){
            case CF_RGB:
                for(iRow = iFirstRow; iRow < cHeight; iRow += tScale){
                    for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                        size_t iPos = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf];
                        PixelI g = (pSrcY[iPos] * cMul) >> rShiftY, r = -(pSrcU[iPos] * cMul) >> rShiftUV, b = (pSrcV[iPos] * cMul) >> rShiftUV;

                        _ICC(r, g, b);
                        
                        pDst = (U16 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY;
                        pDst[0] = backwardHalf (r);
                        pDst[1] = backwardHalf (g);
                        pDst[2] = backwardHalf (b);
                    }
                }
                break;

            case Y_ONLY:
            case YUV_444:
            case NCOMPONENT:
                outputNChannelThumbnail(pSC, cMul, rShiftY, iFirstRow, iFirstColumn);
            break;

            default:
                assert(0);
                break;
        }        
    }
    else if(bd == BD_32){
        U32 * pDst;

        offset = (((1 << 31) >> nLen) << rShiftY) / cMul;

        switch(cfExt){
            case CF_RGB:
                for(iRow = iFirstRow; iRow < cHeight; iRow += tScale){
                    for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                        size_t iPos = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf];
                        PixelI g = ((pSrcY[iPos] + offset) * cMul) >> rShiftY, r = -(pSrcU[iPos] * cMul) >> rShiftUV, b = (pSrcV[iPos] * cMul) >> rShiftUV;

                        _ICC(r, g, b);
                        
                        pDst = (U32 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY;

                        pDst[0] = (U32)(r << nLen);
                        pDst[1] = (U32)(g << nLen);
                        pDst[2] = (U32)(b << nLen);
                    }
                }
                break;

            case Y_ONLY:
            case YUV_444:
            case NCOMPONENT:
                outputNChannelThumbnail(pSC, cMul, rShiftY, iFirstRow, iFirstColumn);
            break;

            default:
                assert(0);
                break;
        }    
    }
    else if(bd == BD_32S){
        I32 * pDst;

        switch(cfExt){
            case CF_RGB:
                for(iRow = iFirstRow; iRow < cHeight; iRow += tScale){
                    for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                        size_t iPos = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf];
                        PixelI g = (pSrcY[iPos] * cMul) >> rShiftY, r = -(pSrcU[iPos] * cMul) >> rShiftUV, b = (pSrcV[iPos] * cMul) >> rShiftUV;

                        _ICC(r, g, b);
                        
                        pDst = (I32 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY;
                        pDst[0] = (I32)(r << nLen);
                        pDst[1] = (I32)(g << nLen);
                        pDst[2] = (I32)(b << nLen);
                    }
                }
                break;

            case Y_ONLY:
            case YUV_444:
            case NCOMPONENT:
                outputNChannelThumbnail(pSC, cMul, rShiftY, iFirstRow, iFirstColumn);
            break;

            default:
                assert(0);
                break;
        }        
    }

    else if(bd == BD_32F){
        float * pDst;

        switch(cfExt){
            case CF_RGB:
                for(iRow = iFirstRow; iRow < cHeight; iRow += tScale){
                    for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                        size_t iPos = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf];
                        PixelI g = (pSrcY[iPos] * cMul) >> rShiftY, r = -(pSrcU[iPos] * cMul) >> rShiftUV, b = (pSrcV[iPos] * cMul) >> rShiftUV;

                        _ICC(r, g, b);
                        
                        pDst = (float *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY;
                        pDst[0] = pixel2float (r, nExpBias, nLen);
                        pDst[1] = pixel2float (g, nExpBias, nLen);
                        pDst[2] = pixel2float (b, nExpBias, nLen);
                    }
                }
                break;

            case Y_ONLY:
            case YUV_444:
            case NCOMPONENT:
                outputNChannelThumbnail(pSC, cMul, rShiftY, iFirstRow, iFirstColumn);
                break;

            default:
                assert(0);
                break;
        }        
    }
    else if(bd == BD_1){
        const size_t iPos = pSC->WMII.cLeadingPadding;
        Bool bBW; 
        U8 cByte, cShift;
        assert(cfExt == Y_ONLY && pSC->m_param.cfColorFormat == Y_ONLY);        

        if(pSC->WMII.oOrientation < O_RCW){
            for(iRow = iFirstRow; iRow < cHeight; iRow += tScale)
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits] + iPos; iColumn < cWidth; iColumn += tScale){
                    bBW = (pSC->WMISCP.bBlackWhite ^ (pSrcY[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] > 0));
                    cByte = ((U8 *)pSC->WMIBI.pv + (pOffsetX[iColumn >> nBits] >> 3) + iY)[0];
                    cShift = (U8)(7 - (pOffsetX[iColumn >> nBits] & 7)); 
                    ((U8 *)pSC->WMIBI.pv + (pOffsetX[iColumn >> nBits] >> 3) + iY)[0] ^= ((((bBW + (cByte >> cShift)) & 0x1)) << cShift);
                }
        }
        else{
            for(iRow = iFirstRow; iRow < cHeight; iRow += tScale)
                for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits] + iPos; iColumn < cWidth; iColumn += tScale){
                    bBW = (pSC->WMISCP.bBlackWhite ^ (pSrcY[((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 15]] > 0));
                    cByte = ((U8 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + (iY >> 3))[0];
                    cShift = (U8)(7 - (iY & 7));
                    ((U8 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + (iY >> 3))[0] ^= ((((bBW + (cByte >> cShift)) & 0x1)) << cShift);
                }
        }
    }
    else if(bd == BD_5){
        U16 * pDst;

        offset = (16 << rShiftY) / cMul;
        
        for(iRow = iFirstRow; iRow < cHeight; iRow += tScale){
            for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                size_t iPos = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf];
                PixelI g = ((pSrcY[iPos] + offset) * cMul) >> rShiftY, r = -(pSrcU[iPos] * cMul) >> rShiftUV, b = (pSrcV[iPos] * cMul) >> rShiftUV;

                _ICC(r, g, b);
                pDst = (U16 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY;
                pDst[0] = (U16)_CLIP2(0, r, 31) + (((U16)_CLIP2(0, g, 31)) << 5) + (((U16)_CLIP2(0, b, 31)) << 10);
            }
        }
    }
    else if(bd == BD_565){
        U16 * pDst;

        offset = (32 << rShiftY) / cMul;
        
        for(iRow = iFirstRow; iRow < cHeight; iRow += tScale){
            for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                size_t iPos = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf];
                PixelI g = ((pSrcY[iPos] + offset) * cMul) >> rShiftY, r = -(pSrcU[iPos] * cMul) >> rShiftUV, b = (pSrcV[iPos] * cMul) >> rShiftUV;

                _ICC(r, g, b);
                r /= 2, b /= 2;
                pDst = (U16 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY;
                pDst[0] = (U16)_CLIP2(0, r, 31) + (((U16)_CLIP2(0, g, 63)) << 5) + (((U16)_CLIP2(0, b, 31)) << 11);
            }
        }
    }
    else if(bd == BD_10){
        U32 * pDst;

        offset = (512 << rShiftY) / cMul;
        
        for(iRow = iFirstRow; iRow < cHeight; iRow += tScale){
            for(iColumn = iFirstColumn, iY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){
                size_t iPos = ((iColumn >> 4) << 8) + idxCC[iRow][iColumn & 0xf];
                PixelI g = ((pSrcY[iPos] + offset) * cMul) >> rShiftY, r = -(pSrcU[iPos] * cMul) >> rShiftUV, b = (pSrcV[iPos] * cMul) >> rShiftUV;

                _ICC(r, g, b);
                pDst = (U32 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iY;
                pDst[0] = (U32)_CLIP2(0, r, 1023) + 
                    (((U32)_CLIP2(0, g, 1023)) << 10) + 
                    (((U32)_CLIP2(0, b, 1023)) << 20);
            }
        }
    }

    if(pSC->WMISCP.uAlphaMode > 0)
        if(decodeThumbnailAlpha(pSC, nBits, cMul, rShiftY) != ICERR_OK)
            return ICERR_ERROR;

#ifdef REENTRANT_MODE
    pSC->WMIBI.cLinesDecoded = ( cHeight - iFirstRow + tScale - 1 ) / tScale;
    if (CF_RGB == pSC->WMII.cfColorFormat && Y_ONLY == pSC->WMISCP.cfColorFormat)
    {
        const CWMImageInfo* pII = &pSC->WMII;

#define fixupThumb(type, nCh) \
for(iRow = iFirstRow; iRow < cHeight; iRow += tScale) {\
    size_t iOffsetY;\
    for(iColumn = iFirstColumn, iOffsetY = pOffsetY[iRow >> nBits]; iColumn < cWidth; iColumn += tScale){\
        type *pT = (type*)((U8 *)pSC->WMIBI.pv + pOffsetX[iColumn >> nBits] + iOffsetY);\
        pT[iB] = pT[1] = pT[iR]; \
    } \
} \
break

        switch (pII->bdBitDepth)
        {
            case BD_8:
                fixupThumb(U8, (pII->cBitsPerUnit >> 3));
                break;

            case BD_16:
            case BD_16S:
            case BD_16F:
                fixupThumb(U16, (pII->cBitsPerUnit >> 3) / sizeof(U16));
                break;

            case BD_32:
            case BD_32S:
            case BD_32F:
                fixupThumb(U32, (pII->cBitsPerUnit >> 3) / sizeof(float));
                break;

            case BD_5:
            case BD_10:
            case BD_565:
            default:
                break;
        }
    }
#endif

    return ICERR_OK;
}

/*************************************************************************
    Read variable length byte aligned integer
*************************************************************************/
static size_t GetVLWordEsc(BitIOInfo* pIO, Int *iEscape)
{
    size_t s;

    if (iEscape)
        *iEscape = 0;

    s = getBit32(pIO, 8);
    if (s == 0xfd || s == 0xfe || s == 0xff) {
        if (iEscape)
            *iEscape = (Int) s;
        s = 0;
    }
    else if (s < 0xfb) {
        s = (s << 8) | getBit32(pIO, 8);
    }
    else {
        s -= 0xfb;
        if (s) {
            s = getBit32(pIO, 16) << 16;
            s = (s | getBit32(pIO, 16)) << 16;
            s <<= 16;
        }
        s |= (getBit32(pIO, 16) << 16);
        s |= getBit32(pIO, 16);
    }
    return s;
}

/*================================================================ */
Int readIndexTable(CWMImageStrCodec * pSC)
{
    BitIOInfo* pIO = pSC->pIOHeader;
    readIS_L1(pSC, pIO);

    if(pSC->cNumBitIO > 0){
        size_t *pTable = pSC->pIndexTable;
        U32 iEntry = (U32)pSC->cNumBitIO * (pSC->WMISCP.cNumOfSliceMinus1H + 1), i;

        /* read index table header [0x0001] - 2 bytes */
        if (getBit32(pIO, 16) != 1)
            return ICERR_ERROR;

        /*iBits = getBit16(pIO, 5) + 1; // how many bits per entry */
        for(i = 0; i < iEntry; i ++){
            readIS_L1(pSC, pIO);
            pTable[i] = GetVLWordEsc(pIO, NULL);  /* escape handling is not important since the respective band is not accessed */
        }
    }

    pSC->cHeaderSize = GetVLWordEsc(pIO, NULL);  /* escape handling is not important */
    flushToByte(pIO);    
    
    pSC->cHeaderSize += getPosRead(pSC->pIOHeader); /* get header length */

    return ICERR_OK;
}

Int StrIODecInit(CWMImageStrCodec* pSC)
{
    if(allocateBitIOInfo(pSC) != ICERR_OK){
        return ICERR_ERROR;
    }
    
    attachISRead(pSC->pIOHeader, pSC->WMISCP.pWStream, pSC);

    readIndexTable(pSC);

    if(pSC->WMISCP.bVerbose){
        U32 i, j;

        printf("\n%d horizontal tiles:\n", pSC->WMISCP.cNumOfSliceMinus1H + 1);
        for(i = 0; i <= pSC->WMISCP.cNumOfSliceMinus1H; i ++){
            printf("    offset of tile %d in MBs: %d\n", i, pSC->WMISCP.uiTileY[i]);
        }

        printf("\n%d vertical tiles:\n", pSC->WMISCP.cNumOfSliceMinus1V + 1);
        for(i = 0; i <= pSC->WMISCP.cNumOfSliceMinus1V; i ++){
            printf("    offset of tile %d in MBs: %d\n", i, pSC->WMISCP.uiTileX[i]);
        }

        if(pSC->WMISCP.bfBitstreamFormat == SPATIAL){
            printf("\nSpatial order bitstream\n");
        }
        else{
            printf("\nFrequency order bitstream\n");
        }

        if(!pSC->m_param.bIndexTable){
            printf("\nstreaming mode, no index table.\n");
        }
        else if(pSC->WMISCP.bfBitstreamFormat == SPATIAL){
            for(j = 0; j <= pSC->WMISCP.cNumOfSliceMinus1H; j ++){
                for(i = 0; i <= pSC->WMISCP.cNumOfSliceMinus1V; i ++){
                    size_t * p = &pSC->pIndexTable[j * (pSC->WMISCP.cNumOfSliceMinus1V + 1) + i];
                    if(i + j != pSC->WMISCP.cNumOfSliceMinus1H + pSC->WMISCP.cNumOfSliceMinus1V){
                        printf("bitstream size for tile (%d, %d): %d.\n", j, i, (int) (p[1] - p[0]));
                    }
                    else{
                        printf("bitstream size for tile (%d, %d): unknown.\n", j, i);
                    }
                }
            }
        }
        else{
            for(j = 0; j <= pSC->WMISCP.cNumOfSliceMinus1H; j ++){
                for(i = 0; i <= pSC->WMISCP.cNumOfSliceMinus1V; i ++){
                    size_t * p = &pSC->pIndexTable[(j * (pSC->WMISCP.cNumOfSliceMinus1V + 1) + i) * 4];
                    if(i + j != pSC->WMISCP.cNumOfSliceMinus1H + pSC->WMISCP.cNumOfSliceMinus1V){
                        printf("bitstream size of (DC, LP, AC, FL) for tile (%d, %d): %d %d %d %d.\n", j, i,
                            (int) (p[1] - p[0]), (int) (p[2] - p[1]), (int) (p[3] - p[2]), (int) (p[4] - p[3]));
                    }
                    else{
                        printf("bitstream size of (DC, LP, AC, FL) for tile (%d, %d): %d %d %d unknown.\n", j, i,
                            (int) (p[1] - p[0]), (int) (p[2] - p[1]), (int) (p[3] - p[2]));
                    }
                }
            }
        }
    }

    return 0;
}

Int StrIODecTerm(CWMImageStrCodec* pSC)
{
    detachISRead(pSC, pSC->pIOHeader);

    free(pSC->m_ppBitIO);
    free(pSC->pIndexTable);

    return 0;
}

Int initLookupTables(CWMImageStrCodec* pSC)
{
    static const U8 cbChannels[BDB_MAX] = {1, 1, 2, 2, 2, 4, 4, 4, (U8) -1, (U8) -1, (U8) -1 };

    CWMImageInfo * pII = &pSC->WMII;
    size_t cStrideX, cStrideY;
    size_t w, h, i, iFirst = 0;
    Bool bReverse;

    /* lookup tables for rotation and flipping */
    if(pSC->m_Dparam->cThumbnailScale > 1) /* thumbnail */
        w = pII->cThumbnailWidth, h = pII->cThumbnailHeight;
    else
        w = pII->cWidth, h = pII->cHeight;
    w += (pSC->m_Dparam->cROILeftX + pSC->m_Dparam->cThumbnailScale - 1) / pSC->m_Dparam->cThumbnailScale;
    h += (pSC->m_Dparam->cROITopY + pSC->m_Dparam->cThumbnailScale - 1) / pSC->m_Dparam->cThumbnailScale;

    switch(pII->bdBitDepth){
        case BD_16:
        case BD_16S:
        case BD_5:
        case BD_565:
        case BD_16F:
            cStrideY = pSC->WMIBI.cbStride / 2;
            break;

        case BD_32:
        case BD_32S:
        case BD_32F:
        case BD_10:
            cStrideY = pSC->WMIBI.cbStride / 4;
            break;

        default: /*BD_8, BD_1 */
            cStrideY = pSC->WMIBI.cbStride;
            break;
    }

    switch(pII->cfColorFormat){
        case YUV_420:
            cStrideX = 6;
            w >>= 1, h >>= 1;
            break;

        case YUV_422:
            cStrideX = 4;
            w >>= 1;
            break;

        default:
            cStrideX = (pII->cBitsPerUnit >> 3) / cbChannels[pII->bdBitDepth];
            break;
    }

    if(pII->bdBitDepth == BD_1 || pII->bdBitDepth == BD_5 || pII->bdBitDepth == BD_10 ||  pII->bdBitDepth == BD_565)
        cStrideX = 1;

    if(pII->oOrientation > O_FLIPVH) /* rotated !! */
        i =cStrideX, cStrideX = cStrideY, cStrideY = i;

    pSC->m_Dparam->pOffsetX = (size_t *)malloc(w * sizeof(size_t));
    if(pSC->m_Dparam->pOffsetX == NULL || w * sizeof(size_t) < w)
        return ICERR_ERROR;
    /*
    consider a row in the source image. if it becomes a reversed row in the target, or a reversed (upside-down)column 
    in the target, we have to reverse the offsets. bReverse here tells us when this happened.
    */
    bReverse = (pII->oOrientation == O_FLIPH || pII->oOrientation == O_FLIPVH || 
        pII->oOrientation == O_RCW_FLIPV || pII->oOrientation == O_RCW_FLIPVH);
    if(!pSC->m_Dparam->bDecodeFullFrame) /* take care of region decode here! */
        iFirst = (pSC->m_Dparam->cROILeftX + pSC->m_Dparam->cThumbnailScale - 1) / pSC->m_Dparam->cThumbnailScale;
    for(i = 0; i + iFirst < w; i ++){
        pSC->m_Dparam->pOffsetX[i + iFirst] = pII->cLeadingPadding + (bReverse ? (pSC->m_Dparam->bDecodeFullFrame ? w : 
    (pSC->m_Dparam->cROIRightX - pSC->m_Dparam->cROILeftX + pSC->m_Dparam->cThumbnailScale) / pSC->m_Dparam->cThumbnailScale / ((pII->cfColorFormat == YUV_420 || pII->cfColorFormat == YUV_422) ? 2 : 1)) - 1 - i : i) * cStrideX;
    }

    pSC->m_Dparam->pOffsetY = (size_t *)malloc(h * sizeof(size_t));
    if(pSC->m_Dparam->pOffsetY == NULL || h * sizeof(size_t) < h)
        return ICERR_ERROR;
    /*
    consider a column in the source image. if it becomes an upside-down column in the target, or a reversed row 
    in the target, we have to reverse the offsets. bReverse here tells us when this happened.
    */
    bReverse = (pII->oOrientation == O_FLIPV || pII->oOrientation == O_FLIPVH || 
        pII->oOrientation == O_RCW || pII->oOrientation == O_RCW_FLIPV);
    if(!pSC->m_Dparam->bDecodeFullFrame) /* take care of region decode here! */
        iFirst = (pSC->m_Dparam->cROITopY + pSC->m_Dparam->cThumbnailScale - 1) / pSC->m_Dparam->cThumbnailScale;
    for(i = 0; i + iFirst < h; i ++){
        pSC->m_Dparam->pOffsetY[i + iFirst] = (bReverse ? (pSC->m_Dparam->bDecodeFullFrame ? h : 
    (pSC->m_Dparam->cROIBottomY - pSC->m_Dparam->cROITopY + pSC->m_Dparam->cThumbnailScale) / pSC->m_Dparam->cThumbnailScale / (pII->cfColorFormat == YUV_420 ? 2 : 1)) - 1 - i : i) * cStrideY;
    }

    return ICERR_OK;
}

Void setROI(CWMImageStrCodec* pSC)
{
    CWMImageInfo * pWMII = &pSC->WMII;
    CWMIStrCodecParam * pSCP = &pSC->WMISCP;

    /* inscribed image size */
    pWMII->cWidth -= pSC->m_param.cExtraPixelsLeft + pSC->m_param.cExtraPixelsRight;
    pWMII->cHeight -= pSC->m_param.cExtraPixelsTop + pSC->m_param.cExtraPixelsBottom;

    pSC->m_Dparam->bSkipFlexbits = (pSCP->sbSubband == SB_NO_FLEXBITS);
    pSC->m_Dparam->bDecodeHP = (pSCP->sbSubband == SB_ALL || pSCP->sbSubband == SB_NO_FLEXBITS);
    pSC->m_Dparam->bDecodeLP = (pSCP->sbSubband != SB_DC_ONLY);
    pSC->m_Dparam->cThumbnailScale = 1;
    while(pSC->m_Dparam->cThumbnailScale * pWMII->cThumbnailWidth < pWMII->cWidth)
        pSC->m_Dparam->cThumbnailScale <<= 1;
    if(pSC->WMISCP.bfBitstreamFormat == FREQUENCY){
        if(pSC->m_Dparam->cThumbnailScale >= 4)
            pSC->m_Dparam->bDecodeHP = FALSE;  /* no need to decode HP */
        if(pSC->m_Dparam->cThumbnailScale >= 16)
            pSC->m_Dparam->bDecodeLP = FALSE; /* only need to decode DC */
    }

    /* original image size */
    pWMII->cWidth += pSC->m_param.cExtraPixelsLeft + pSC->m_param.cExtraPixelsRight;
    pWMII->cHeight += pSC->m_param.cExtraPixelsTop + pSC->m_param.cExtraPixelsBottom;

    /** region decode stuff */
    pSC->m_Dparam->cROILeftX = pWMII->cROILeftX * pSC->m_Dparam->cThumbnailScale + pSC->m_param.cExtraPixelsLeft;
    pSC->m_Dparam->cROIRightX = pSC->m_Dparam->cROILeftX + pWMII->cROIWidth * pSC->m_Dparam->cThumbnailScale - 1;
    pSC->m_Dparam->cROITopY = pWMII->cROITopY * pSC->m_Dparam->cThumbnailScale + pSC->m_param.cExtraPixelsTop;
    pSC->m_Dparam->cROIBottomY = pSC->m_Dparam->cROITopY + pWMII->cROIHeight * pSC->m_Dparam->cThumbnailScale - 1;
    if(pSC->m_Dparam->cROIRightX >= pWMII->cWidth)
        pSC->m_Dparam->cROIRightX = pWMII->cWidth - 1;
    if(pSC->m_Dparam->cROIBottomY >= pWMII->cHeight)
        pSC->m_Dparam->cROIBottomY = pWMII->cHeight - 1;

    pSC->m_Dparam->bDecodeFullFrame = (pSC->m_Dparam->cROILeftX + pSC->m_Dparam->cROITopY == 0 &&
        ((pSC->m_Dparam->cROIRightX + 15) / 16 >= (pWMII->cWidth + 14) / 16) && ((pSC->m_Dparam->cROIBottomY + 15) / 16 >= (pWMII->cHeight + 14) / 16));

    pSC->m_Dparam->bDecodeFullWidth = (pSC->m_Dparam->cROILeftX == 0 && ((pSC->m_Dparam->cROIRightX + 15) / 16 >= (pWMII->cWidth + 14) / 16));

    /* inscribed image size */
    pWMII->cWidth -= pSC->m_param.cExtraPixelsLeft + pSC->m_param.cExtraPixelsRight;
    pWMII->cHeight -= pSC->m_param.cExtraPixelsTop + pSC->m_param.cExtraPixelsBottom;

    if(pSC->WMISCP.bfBitstreamFormat == FREQUENCY && pWMII->bSkipFlexbits == TRUE)
        pSC->m_Dparam->bSkipFlexbits = TRUE;

    pSC->cTileColumn = pSC->cTileRow = 0;
}

Int StrDecInit(CWMImageStrCodec* pSC)
{
    /* CWMImageInfo * pWMII = &pSC->WMII; */
    COLORFORMAT cfInt = pSC->m_param.cfColorFormat;
    COLORFORMAT cfExt = pSC->WMII.cfColorFormat;
    size_t i;

    /** color transcoding with resolution change **/
    pSC->m_bUVResolutionChange = ((cfExt != Y_ONLY) && ((cfInt == YUV_420 && cfExt != YUV_420) || 
        (cfInt == YUV_422 && cfExt != YUV_422))) && !pSC->WMISCP.bYUVData;
    if(pSC->m_bUVResolutionChange){
        pSC->pResU = (PixelI *)malloc((cfExt == YUV_422 ? 128 : 256) * pSC->cmbWidth * sizeof(PixelI));
        pSC->pResV = (PixelI *)malloc((cfExt == YUV_422 ? 128 : 256) * pSC->cmbWidth * sizeof(PixelI));
        if(pSC->pResU == NULL || pSC->pResV == NULL || (cfExt == YUV_422 ? 128 : 256) * pSC->cmbWidth * sizeof(PixelI) < pSC->cmbWidth){
            return ICERR_ERROR;
        }
    }

    if(allocatePredInfo(pSC) != ICERR_OK){
        return ICERR_ERROR;
    }

    if(allocateTileInfo(pSC) != ICERR_OK)
        return ICERR_ERROR;

    if((pSC->m_param.uQPMode & 1) == 0){ /* DC frame uniform quantization */
        if(allocateQuantizer(pSC->pTile[0].pQuantizerDC, pSC->m_param.cNumChannels, 1) != ICERR_OK)
            return ICERR_ERROR;
        setUniformQuantizer(pSC, 0);
        for(i = 0; i < pSC->m_param.cNumChannels; i ++)
            pSC->pTile[0].pQuantizerDC[i]->iIndex = pSC->m_param.uiQPIndexDC[i];
        formatQuantizer(pSC->pTile[0].pQuantizerDC, (pSC->m_param.uQPMode >> 3) & 3, pSC->m_param.cNumChannels, 0, TRUE, pSC->m_param.bScaledArith);
    }

    if(pSC->WMISCP.sbSubband != SB_DC_ONLY){
        if((pSC->m_param.uQPMode & 2) == 0){ /* LP frame uniform quantization */
            if(allocateQuantizer(pSC->pTile[0].pQuantizerLP, pSC->m_param.cNumChannels, 1) != ICERR_OK)
                return ICERR_ERROR;
            setUniformQuantizer(pSC, 1);
            if((pSC->m_param.uQPMode & 0x200) == 0) /* use DC quantizer */
                useDCQuantizer(pSC, 0);
            else{
                for(i = 0; i < pSC->m_param.cNumChannels; i ++)
                    pSC->pTile[0].pQuantizerLP[i]->iIndex = pSC->m_param.uiQPIndexLP[i];
                formatQuantizer(pSC->pTile[0].pQuantizerLP, (pSC->m_param.uQPMode >> 5) & 3, pSC->m_param.cNumChannels, 0, TRUE, pSC->m_param.bScaledArith);
            }
        }

        if(pSC->WMISCP.sbSubband != SB_NO_HIGHPASS){
            if((pSC->m_param.uQPMode & 4) == 0){ /* HP frame uniform quantization */
                if(allocateQuantizer(pSC->pTile[0].pQuantizerHP, pSC->m_param.cNumChannels, 1) != ICERR_OK)
                    return ICERR_ERROR;
                setUniformQuantizer(pSC, 2);

                if((pSC->m_param.uQPMode & 0x400) == 0) /* use LP quantizer */
                    useLPQuantizer(pSC, 1, 0);
                else{
                    for(i = 0; i < pSC->m_param.cNumChannels; i ++)
                        pSC->pTile[0].pQuantizerHP[i]->iIndex = pSC->m_param.uiQPIndexHP[i];
                    formatQuantizer(pSC->pTile[0].pQuantizerHP, (pSC->m_param.uQPMode >> 7) & 3, pSC->m_param.cNumChannels, 0, FALSE, pSC->m_param.bScaledArith);
                }
            }
        }
    }

    if(pSC->WMISCP.cNumOfSliceMinus1V >= MAX_TILES || AllocateCodingContextDec(pSC, pSC->WMISCP.cNumOfSliceMinus1V + 1) != ICERR_OK){
        return ICERR_ERROR;
    }

    if (pSC->m_bSecondary) {
        pSC->pIOHeader = pSC->m_pNextSC->pIOHeader;
        pSC->m_ppBitIO = pSC->m_pNextSC->m_ppBitIO;
        pSC->cNumBitIO = pSC->m_pNextSC->cNumBitIO;
        pSC->cSB = pSC->m_pNextSC->cSB;
    }

    setBitIOPointers(pSC);

    return ICERR_OK;
}

Int StrDecTerm(CWMImageStrCodec* pSC)
{
    size_t j, jend = (pSC->m_pNextSC != NULL);

    for (j = 0; j <= jend; j++) {
        if(pSC->m_bUVResolutionChange){        
            if(pSC->pResU != NULL)
                free(pSC->pResU);
            if(pSC->pResV != NULL)
                free(pSC->pResV);
        }

        freePredInfo(pSC);

        freeTileInfo(pSC);

        FreeCodingContextDec(pSC);

        if (j == 0) {
            StrIODecTerm(pSC);

            /* free lookup tables for rotation and flipping */
            if(pSC->m_Dparam->pOffsetX != NULL)
                free(pSC->m_Dparam->pOffsetX);
            if(pSC->m_Dparam->pOffsetY != NULL)
                free(pSC->m_Dparam->pOffsetY);
        }

        pSC = pSC->m_pNextSC;
    }

    return 0;
}

/*************************************************************************
    Read header of image plane
*************************************************************************/
Int ReadImagePlaneHeader(CWMImageInfo* pII, CWMIStrCodecParam *pSCP,
    CCoreParameters *pSC, SimpleBitIO* pSB)
{
    ERR err = WMP_errSuccess;

    pSC->cfColorFormat = getBit32_SB(pSB, 3); /* internal color format */
    FailIf((pSC->cfColorFormat < Y_ONLY || pSC->cfColorFormat > NCOMPONENT), WMP_errUnsupportedFormat);
    pSCP->cfColorFormat = pSC->cfColorFormat;  /* this should be removed later */
    pSC->bScaledArith = getBit32_SB(pSB, 1); /* lossless mode */

    /* subbands */
    pSCP->sbSubband = getBit32_SB(pSB, 4);

/* color parameters */
    switch (pSC->cfColorFormat) {
        case Y_ONLY:
            pSC->cNumChannels = 1;
            break;
        case YUV_420:
            pSC->cNumChannels = 3;
            getBit32_SB(pSB, 1);
            pII->cChromaCenteringX = (U8) getBit32_SB(pSB, 3);
            getBit32_SB(pSB, 1);
            pII->cChromaCenteringY = (U8) getBit32_SB(pSB, 3);
            break; 
        case YUV_422:
            pSC->cNumChannels = 3;
            getBit32_SB(pSB, 1);
            pII->cChromaCenteringX = (U8) getBit32_SB(pSB, 3);
            getBit32_SB(pSB, 4);
            break; 
        case YUV_444:
            pSC->cNumChannels = 3;
            getBit32_SB(pSB, 4);
            getBit32_SB(pSB, 4);
            break;
        case NCOMPONENT:
            pSC->cNumChannels = (Int) getBit32_SB(pSB, 4) + 1;
            getBit32_SB(pSB, 4);
            break;
        case CMYK:
            pSC->cNumChannels = 4;
            break;
        default:
            break;
    }

/* float and 32s additional parameters */
    switch (pII->bdBitDepth) {
        case BD_16:
        case BD_16S:
        case BD_32:
        case BD_32S:
            pSCP->nLenMantissaOrShift = (U8) getBit32_SB(pSB, 8);
            break;
        case BD_32F:
            pSCP->nLenMantissaOrShift = (U8) getBit32_SB(pSB, 8);/*float conversion parameters */
            pSCP->nExpBias = (I8) getBit32_SB(pSB, 8);
            break;
        default:
            break;
    }

        /* quantization */
    pSC->uQPMode = 0;
    if(getBit32_SB(pSB, 1) == 1) /* DC uniform */
        pSC->uQPMode += (readQuantizerSB(pSC->uiQPIndexDC, pSB, pSC->cNumChannels) << 3);
    else
        pSC->uQPMode ++;
    if(pSCP->sbSubband != SB_DC_ONLY){
        if(getBit32_SB(pSB, 1) == 0){ /* don't use DC QP */
            pSC->uQPMode += 0x200;
            if(getBit32_SB(pSB, 1) == 1) /* LP uniform */
                pSC->uQPMode += (readQuantizerSB(pSC->uiQPIndexLP, pSB, pSC->cNumChannels) << 5);
            else
                pSC->uQPMode += 2;
        }
        else
            pSC->uQPMode += ((pSC->uQPMode & 1) << 1) + ((pSC->uQPMode & 0x18) << 2);

        if(pSCP->sbSubband != SB_NO_HIGHPASS){
            if(getBit32_SB(pSB, 1) == 0){ /* don't use LP QP */
                pSC->uQPMode += 0x400;
                if(getBit32_SB(pSB, 1) == 1) /* HP uniform */
                    pSC->uQPMode += (readQuantizerSB(pSC->uiQPIndexHP, pSB, pSC->cNumChannels) << 7);
                else
                    pSC->uQPMode += 4;
            }
            else
                pSC->uQPMode += ((pSC->uQPMode & 2) << 1) + ((pSC->uQPMode & 0x60) << 2);
        }
    }

    if(pSCP->sbSubband == SB_DC_ONLY)
        pSC->uQPMode |= 0x200;
    else if(pSCP->sbSubband == SB_NO_HIGHPASS)
        pSC->uQPMode |= 0x400;
    

    FailIf((pSC->uQPMode & 0x600) == 0, WMP_errInvalidParameter); /* frame level QPs must be specified independently! */

    flushToByte_SB(pSB);  /* remove this later */

Cleanup:
    return WMP_errSuccess == err ? ICERR_OK : ICERR_ERROR;
}

/*************************************************************************
    Read header of image, and header of FIRST PLANE only
*************************************************************************/
Int ReadWMIHeader(
    CWMImageInfo* pII,
    CWMIStrCodecParam *pSCP,
    CCoreParameters *pSC)
{
    U32 i;
    ERR err = WMP_errSuccess;
    Bool bTilingPresent, bInscribed, bTileStretch, bAbbreviatedHeader;
    struct WMPStream* pWS = pSCP->pWStream;

    SimpleBitIO SB = {0};
    SimpleBitIO* pSB = &SB;

    U8 szMS[8] = {0};
    U32 cbStream = 0;

    /* U32 bits = 0; */
    /* Int HEADERSIZE = 0; */

    assert(pSC != NULL);
    /*================================ */
/* 0 */
    /** signature **/
    Call(pWS->Read(pWS, szMS, sizeof(szMS)));
    FailIf(szMS != (U8 *) strstr((char *) szMS, "WMPHOTO"), WMP_errUnsupportedFormat);
    /*================================ */
    Call(attach_SB(pSB, pWS));

/* 8 */
    /** codec version and subversion **/
    i = getBit32_SB(pSB, 4);
    FailIf((i != CODEC_VERSION), WMP_errIncorrectCodecVersion);
    pSC->cVersion = i;
    i = getBit32_SB(pSB, 4); /* subversion */
    FailIf((i != CODEC_SUBVERSION &&
        i != CODEC_SUBVERSION_NEWSCALING_SOFT_TILES && i != CODEC_SUBVERSION_NEWSCALING_HARD_TILES),
        WMP_errIncorrectCodecSubVersion);
    pSC->cSubVersion = i;

    pSC->bUseHardTileBoundaries = FALSE;
    if (pSC->cSubVersion == CODEC_SUBVERSION_NEWSCALING_HARD_TILES) 
        pSC->bUseHardTileBoundaries = TRUE;

    pSCP->bUseHardTileBoundaries = pSC->bUseHardTileBoundaries;

/* 9 primary parameters */
    bTilingPresent = (Bool) getBit32_SB(pSB, 1); /* tiling present */
    pSCP->bfBitstreamFormat = getBit32_SB(pSB, 1); /* bitstream layout */
    pII->oOrientation = (ORIENTATION)getBit32_SB(pSB, 3); /* presentation orientation */
    pSC->bIndexTable = getBit32_SB(pSB, 1);
    i = getBit32_SB(pSB, 2); /* overlap */
    FailIf((i == 3), WMP_errInvalidParameter);
    pSCP->olOverlap = i;

/* 11 some other parameters */
    bAbbreviatedHeader = (Bool) getBit32_SB(pSB, 1); /* short words for size and tiles */
    pSCP->bdBitDepth = (BITDEPTH) getBit32_SB(pSB, 1); /* long word */
pSCP->bdBitDepth = BD_LONG; /* remove when optimization is done */
    bInscribed = (Bool) getBit32_SB(pSB, 1); /* windowing */
    pSC->bTrimFlexbitsFlag = (Bool) getBit32_SB(pSB, 1); /* trim flexbits flag */
    bTileStretch = (Bool) getBit32_SB(pSB, 1); /* tile stretching flag */
    pSC->bRBSwapped = (Bool) getBit32_SB(pSB, 1); /* red-blue swap flag */
    getBit32_SB(pSB, 1);  /* padding / reserved bit */
    pSC->bAlphaChannel = (Bool) getBit32_SB(pSB, 1); /* alpha channel present */

/* 10 - informational */
    pII->cfColorFormat = getBit32_SB(pSB, 4); /* source color format */
    pII->bdBitDepth = getBit32_SB(pSB, 4); /* source bit depth */

    if(BD_1alt == pII->bdBitDepth)
    {
        pII->bdBitDepth = BD_1;
        pSCP->bBlackWhite = 1;
    }

/* 12 - Variable length fields */
/* size */
    pII->cWidth = getBit32_SB(pSB, bAbbreviatedHeader ? 16 : 32) + 1;
    pII->cHeight = getBit32_SB(pSB, bAbbreviatedHeader ? 16 : 32) + 1;
    pSC->cExtraPixelsTop = pSC->cExtraPixelsLeft = pSC->cExtraPixelsBottom = pSC->cExtraPixelsRight = 0;
    if (bInscribed == FALSE && (pII->cWidth & 0xf) != 0)
        pSC->cExtraPixelsRight = 0x10 - (pII->cWidth & 0xF);
    if (bInscribed == FALSE && (pII->cHeight & 0xf) != 0)
        pSC->cExtraPixelsBottom = 0x10 - (pII->cHeight & 0xF);

/* tiling */
    pSCP->cNumOfSliceMinus1V = pSCP->cNumOfSliceMinus1H = 0;
    if (bTilingPresent) {
        pSCP->cNumOfSliceMinus1V = getBit32_SB(pSB, LOG_MAX_TILES); /* # of vertical slices along X axis */
        pSCP->cNumOfSliceMinus1H = getBit32_SB(pSB, LOG_MAX_TILES); /* # of horizontal slices along Y axis */
    }
    FailIf((pSC->bIndexTable == FALSE) && (pSCP->bfBitstreamFormat == FREQUENCY || pSCP->cNumOfSliceMinus1V + pSCP->cNumOfSliceMinus1H > 0),
        WMP_errUnsupportedFormat);

/* tile sizes */
    pSCP->uiTileX[0] = pSCP->uiTileY[0] = 0;
    for(i = 0; i < pSCP->cNumOfSliceMinus1V; i ++){ /* width in MB of vertical slices, not needed for last slice! */
        pSCP->uiTileX[i + 1] = (U32) getBit32_SB(pSB, bAbbreviatedHeader ? 8 : 16) + pSCP->uiTileX[i];
    }
    for(i = 0; i < pSCP->cNumOfSliceMinus1H; i ++){ /* width in MB of vertical slices, not needed for last slice! */
        pSCP->uiTileY[i + 1] = (U32) getBit32_SB(pSB, bAbbreviatedHeader ? 8 : 16) + pSCP->uiTileY[i];
    }
    if (bTileStretch) {  /* no handling of tile stretching enabled as of now */
        for (i = 0; i < (pSCP->cNumOfSliceMinus1V + 1) * (pSCP->cNumOfSliceMinus1H + 1); i++)
            getBit32_SB(pSB, 8);
    }

/* window due to compressed domain processing */
    if (bInscribed) {
        pSC->cExtraPixelsTop = (U8)getBit32_SB(pSB, 6);
        pSC->cExtraPixelsLeft = (U8)getBit32_SB(pSB, 6);
        pSC->cExtraPixelsBottom = (U8)getBit32_SB(pSB, 6);
        pSC->cExtraPixelsRight = (U8)getBit32_SB(pSB, 6);
    }
    
    if(((pII->cWidth + pSC->cExtraPixelsLeft + pSC->cExtraPixelsRight) & 0xf) + ((pII->cHeight + pSC->cExtraPixelsTop + pSC->cExtraPixelsBottom) & 0xf) != 0){
        FailIf((pII->cWidth & 0xf) + (pII->cHeight & 0xf) + pSC->cExtraPixelsLeft + pSC->cExtraPixelsTop != 0, WMP_errInvalidParameter);
        FailIf(pII->cWidth <= pSC->cExtraPixelsRight || pII->cHeight <= pSC->cExtraPixelsBottom, WMP_errInvalidParameter);
        pII->cWidth -= pSC->cExtraPixelsRight, pII->cHeight -= pSC->cExtraPixelsBottom;
    }

    flushToByte_SB(pSB);  /* redundant */

    /* read header of first image plane */
    FailIf(ReadImagePlaneHeader(pII, pSCP, pSC, pSB), WMP_errUnsupportedFormat);

    /* maybe UNALIGNED!!! */

    /*================================ */
    detach_SB(pSB);
    pSCP->cbStream = cbStream - getByteRead_SB(pSB);

    pSCP->uAlphaMode = (pSC->bAlphaChannel ? pSCP->uAlphaMode : 0);
    pSCP->cChannel = pSC->cNumChannels;

    if((pII->bdBitDepth == BD_5 || pII->bdBitDepth == BD_10 || pII->bdBitDepth == BD_565) && 
        (pSCP->cfColorFormat != YUV_444 && pSCP->cfColorFormat != YUV_422 && pSCP->cfColorFormat != YUV_420 && pSCP->cfColorFormat != Y_ONLY))
        return ICERR_ERROR;
    
Cleanup:
    return WMP_errSuccess == err ? ICERR_OK : ICERR_ERROR;
}

/*---------------------------------------------------------------- */
/* streaming api init/decode/term */
EXTERN_C Int ImageStrDecGetInfo(
    CWMImageInfo* pII,
    CWMIStrCodecParam *pSCP)
{
    ERR err = WMP_errSuccess;
    size_t cMarker;
    CCoreParameters aDummy;
    /* mark position of start of data */
    Call(pSCP->pWStream->GetPos(pSCP->pWStream, &cMarker));
    Call(ReadWMIHeader(pII, pSCP, &aDummy));
    /* rewind to start of data */
    Call(pSCP->pWStream->SetPos(pSCP->pWStream, cMarker));
    return ICERR_OK;

Cleanup:
    return ICERR_ERROR;
}

EXTERN_C Int WMPhotoValidate(
    CWMImageInfo * pII,
    CWMIStrCodecParam * pSCP)
{
    CWMImageInfo cII;
    CWMIStrCodecParam cSCP = *pSCP;
    size_t cScale = 1;

    if(ImageStrDecGetInfo(&cII, pSCP) != ICERR_OK)
        return ICERR_ERROR;

    /* copy over un-overwritable ImageInfo parameters */
    pII->bdBitDepth = cII.bdBitDepth;
    pII->cWidth = cII.cWidth;
    pII->cHeight = cII.cHeight;

    if(pII->cWidth == 0 || pII->cHeight == 0)
        return ICERR_ERROR;

    /* copy over overwritable CodecParam parameters */
    pSCP->bVerbose = cSCP.bVerbose;
    pSCP->cbStream = cSCP.cbStream;
    pSCP->pWStream = cSCP.pWStream;
    if(pSCP->uAlphaMode > 1) /* something + alpha */
        pSCP->uAlphaMode = cSCP.uAlphaMode; /* something + alpha to alpha or something transcoding! */

    /* validate color transcoding */
    if(pSCP->cfColorFormat == NCOMPONENT)
        pII->cfColorFormat = NCOMPONENT;
    if(pSCP->cfColorFormat == CMYK && pII->cfColorFormat != Y_ONLY && pII->cfColorFormat != CF_RGB)
        pII->cfColorFormat = CMYK;
    if(pSCP->cfColorFormat == YUV_422 && pII->cfColorFormat == YUV_420)
        pII->cfColorFormat = YUV_422;
    if(pSCP->cfColorFormat == YUV_444 && (pII->cfColorFormat == YUV_422 || pII->cfColorFormat == YUV_420))
        pII->cfColorFormat = YUV_444;
    if(cII.cfColorFormat == CF_RGB && pII->cfColorFormat != Y_ONLY && 
        pII->cfColorFormat != NCOMPONENT)  /* no guarantee that number of channels will be >= 3 */
        pII->cfColorFormat = cII.cfColorFormat;
    if(cII.cfColorFormat == CF_RGBE)
        pII->cfColorFormat = CF_RGBE;

    /* validate thumbnail parameters */
    if(pII->cThumbnailWidth == 0 || pII->cThumbnailWidth > pII->cWidth)
        pII->cThumbnailWidth = pII->cWidth;
    if(pII->cThumbnailHeight == 0 || pII->cThumbnailHeight > pII->cHeight)
        pII->cThumbnailHeight = pII->cHeight;
    if((pII->cWidth + pII->cThumbnailWidth - 1) / pII->cThumbnailWidth != (pII->cHeight + pII->cThumbnailHeight - 1) / pII->cThumbnailHeight) {
        while((pII->cWidth + cScale - 1) / cScale > pII->cThumbnailWidth &&
            (pII->cHeight + cScale - 1) / cScale > pII->cThumbnailHeight && (cScale << 1))
            cScale <<= 1;
    }
    else {
        cScale = (pII->cWidth + pII->cThumbnailWidth - 1) / pII->cThumbnailWidth;    
        if (cScale == 0)
            cScale = 1;
    }
    pII->cThumbnailWidth = (pII->cWidth + cScale - 1) / cScale;
    pII->cThumbnailHeight = (pII->cHeight + cScale - 1) / cScale;

    /* validate region decode parameters */
    if(pII->cROIHeight == 0 || pII->cROIWidth == 0){
        pII->cROILeftX = pII->cROITopY = 0;
        pII->cROIWidth = pII->cThumbnailWidth;
        pII->cROIHeight = pII->cThumbnailHeight;
    }
    if(pII->cROILeftX >= pII->cThumbnailWidth)
        pII->cROILeftX = 0;
    if(pII->cROITopY >= pII->cThumbnailHeight)
        pII->cROITopY = 0;
    if(pII->cROILeftX + pII->cROIWidth > pII->cThumbnailWidth)
        pII->cROIWidth = pII->cThumbnailWidth - pII->cROILeftX;
    if(pII->cROITopY + pII->cROIHeight > pII->cThumbnailHeight)
        pII->cROIHeight = pII->cThumbnailHeight - pII->cROITopY;

    return ICERR_OK;
}

/*************************************************************************
  Initialization of CWMImageStrCodec struct
*************************************************************************/
static Void InitializeStrDec(CWMImageStrCodec *pSC,
  const CCoreParameters *pParams, const CWMImageStrCodec *pSCIn)
{
    /* copy core parameters */
    memcpy (&(pSC->m_param), pParams, sizeof (CCoreParameters));

    pSC->cbStruct = sizeof(*pSC);
    pSC->WMII = pSCIn->WMII;
    pSC->WMISCP = pSCIn->WMISCP;

    pSC->cRow = 0;
    pSC->cColumn = 0;
    
    pSC->cmbWidth = (pSC->WMII.cWidth + 15) / 16;
    pSC->cmbHeight = (pSC->WMII.cHeight + 15) / 16;

    pSC->Load = outputMBRow; /* output decoding result (ICC, etc) */
    pSC->Transform = pParams->cSubVersion == CODEC_SUBVERSION ?
        invTransformMacroblock : invTransformMacroblock_alteredOperators_hard;
    pSC->TransformCenter = pSC->Transform;

    pSC->ProcessTopLeft = processMacroblockDec;
    pSC->ProcessTop = processMacroblockDec;
    pSC->ProcessTopRight = processMacroblockDec;
    pSC->ProcessLeft = processMacroblockDec;
    pSC->ProcessCenter = processMacroblockDec;
    pSC->ProcessRight = processMacroblockDec;
    pSC->ProcessBottomLeft = processMacroblockDec;
    pSC->ProcessBottom = processMacroblockDec;
    pSC->ProcessBottomRight = processMacroblockDec;

    pSC->m_pNextSC = NULL;
    pSC->m_bSecondary = FALSE;
}

/*************************************************************************
  ImageStrDecInit
*************************************************************************/
Int ImageStrDecInit(
    CWMImageInfo* pII,
    CWMIStrCodecParam *pSCP,
    CTXSTRCODEC* pctxSC)
{
    static size_t cbChannels[BD_MAX] = {2, 4};
    ERR err = WMP_errSuccess;

    size_t cbChannel = 0, cblkChroma = 0;
    size_t cbMacBlockStride = 0, cbMacBlockChroma = 0, cMacBlock = 0;

    CWMImageStrCodec SC = {0};
    CWMImageStrCodec *pSC = NULL, *pNextSC = NULL;
    char* pb = NULL;
    size_t cb = 0, i;
    Bool bLossyTranscoding = FALSE;
    Bool bUseHardTileBoundaries = FALSE; /*default is soft tile boundaries */
    Bool bLessThan64Bit = sizeof(void *) < 8;

    *pctxSC = NULL;

    if(WMPhotoValidate(pII, pSCP) != ICERR_OK)
        return ICERR_ERROR;

    if(pSCP->sbSubband == SB_ISOLATED) /* can not do anything with isolated bitstream */
        return ICERR_ERROR;

    /*================================================ */
    SC.WMISCP.pWStream = pSCP->pWStream;
    if (ReadWMIHeader(&SC.WMII, &SC.WMISCP, &SC.m_param) != ICERR_OK) {
        return ICERR_ERROR;
    }

    bUseHardTileBoundaries = SC.WMISCP.bUseHardTileBoundaries;
    if(SC.WMII.cfColorFormat == CMYK && pII->cfColorFormat == CF_RGB)
        bLossyTranscoding = TRUE;
    (void)bLossyTranscoding;
    if(pSCP->cfColorFormat != CMYK && (pII->cfColorFormat == CMYK))
        return ICERR_ERROR;

    /*================================================ */
    SC.WMISCP = *pSCP;
    SC.WMII   = *pII;

    /* original image size */
    SC.WMII.cWidth += SC.m_param.cExtraPixelsLeft + SC.m_param.cExtraPixelsRight;
    SC.WMII.cHeight += SC.m_param.cExtraPixelsTop + SC.m_param.cExtraPixelsBottom;
    pII->cROILeftX += SC.m_param.cExtraPixelsLeft;
    pII->cROITopY += SC.m_param.cExtraPixelsTop;
    
    /*================================================ */
    cbChannel = cbChannels[SC.WMISCP.bdBitDepth];
    cblkChroma = cblkChromas[SC.m_param.cfColorFormat];

    cbMacBlockStride = cbChannel * 16 * 16;
    cbMacBlockChroma = cbChannel * 16 * cblkChroma;
    cMacBlock = (SC.WMII.cWidth + 15) / 16;

    /*================================================ */
    cb = sizeof(*pSC) + (128 - 1) + sizeof(CWMDecoderParameters);
    cb += (PACKETLENGTH * 4 - 1) + (PACKETLENGTH * 2 ) + sizeof(*pSC->pIOHeader);

    i = (cbMacBlockStride + cbMacBlockChroma * (SC.m_param.cNumChannels - 1)) * 2; /* i <= 2^15 */
    if (bLessThan64Bit && ((i * (cMacBlock >> 16)) & 0xffffc000)) {
        /** potential overflow - 32 bit pointers insufficient to address cache **/
        return ICERR_ERROR;
    }
    cb += i * cMacBlock;

    pb = malloc(cb);
    if(pb == NULL)
        return WMP_errOutOfMemory;
    memset(pb, 0, cb);

    /*================================================ */
    pSC = (CWMImageStrCodec*)pb; pb += sizeof(*pSC);
    if(pSC == NULL)
        return ICERR_ERROR;

    /* Set up perf timers */
    PERFTIMER_ONLY(pSC->m_fMeasurePerf = pSCP->fMeasurePerf);
    PERFTIMER_NEW(pSC->m_fMeasurePerf, &pSC->m_ptEndToEndPerf);
    PERFTIMER_NEW(pSC->m_fMeasurePerf, &pSC->m_ptEncDecPerf);
    PERFTIMER_START(pSC->m_fMeasurePerf, pSC->m_ptEndToEndPerf);
    PERFTIMER_START(pSC->m_fMeasurePerf, pSC->m_ptEncDecPerf);
    PERFTIMER_COPYSTARTTIME(pSC->m_fMeasurePerf, pSC->m_ptEncDecPerf, pSC->m_ptEndToEndPerf);

    pSC->m_Dparam = (CWMDecoderParameters*)pb; pb += sizeof(CWMDecoderParameters);
    pSC->cbChannel = cbChannel;
    /*pSC->cNumChannels = SC.WMISCP.cChannel; */
    pSC->bUseHardTileBoundaries = bUseHardTileBoundaries;

    /*================================================ */
    InitializeStrDec(pSC, &SC.m_param, &SC);

    /*================================================ */
    /* 2 Macro Row buffers for each channel */
    pb = ALIGNUP(pb, 128);
    for (i = 0; i < pSC->m_param.cNumChannels; i++) {
        pSC->a0MBbuffer[i] = (PixelI*)pb; pb += cbMacBlockStride * pSC->cmbWidth;
        pSC->a1MBbuffer[i] = (PixelI*)pb; pb += cbMacBlockStride * pSC->cmbWidth;
        cbMacBlockStride = cbMacBlockChroma;
    }

    /*================================================ */
    /* lay 2 aligned IO buffers just below pIO struct */
    pb = (char*)ALIGNUP(pb, PACKETLENGTH * 4) + PACKETLENGTH * 2;
    pSC->pIOHeader = (BitIOInfo*)pb; pb += sizeof(*pSC->pIOHeader);

    /* if interleaved alpha is needed */
    if (pSC->m_param.bAlphaChannel) {
        SimpleBitIO SB = {0};
        cbMacBlockStride = cbChannel * 16 * 16;

        /* 1. allocate new pNextSC info */
        /*================================================ */
        cb = sizeof(*pNextSC) + (128 - 1) + cbMacBlockStride * cMacBlock * 2;
        /* if primary image is safe to allocate, alpha channel is certainly safe */
        pb = malloc(cb);
        if(pb == NULL)
            return WMP_errOutOfMemory;
        memset(pb, 0, cb);
        /*================================================ */
        pNextSC = (CWMImageStrCodec*)pb; pb += sizeof(*pNextSC);

        /* read plane header of second image plane */
        Call(attach_SB(&SB, pSCP->pWStream));
        InitializeStrDec(pNextSC, &SC.m_param, &SC);
        ReadImagePlaneHeader(&pNextSC->WMII, &pNextSC->WMISCP, &pNextSC->m_param, &SB);
        detach_SB(&SB);

        /* 2. initialize pNextSC */
        if(pNextSC == NULL)
            return ICERR_ERROR;
        pNextSC->m_Dparam = pSC->m_Dparam;
        pNextSC->cbChannel = cbChannel;
        /*================================================ */

        /* 3. initialize arrays */
/*        InitializeStrDec(pNextSC, &SC.m_param, &SC); */
        pNextSC->m_param.cfColorFormat = Y_ONLY;
        pNextSC->m_param.cNumChannels = 1;
        pNextSC->m_param.bAlphaChannel = TRUE;
        /*================================================ */

        /* 2 Macro Row buffers for each channel */
        pb = ALIGNUP(pb, 128);
        pNextSC->a0MBbuffer[0] = (PixelI*)pb; pb += cbMacBlockStride * pNextSC->cmbWidth;
        pNextSC->a1MBbuffer[0] = (PixelI*)pb;
        /*================================================ */
        pNextSC->pIOHeader = pSC->pIOHeader;
        /*================================================ */

        /* 4. link pSC->pNextSC = pNextSC */
        pNextSC->m_pNextSC = pSC;
        pNextSC->m_bSecondary = TRUE;

    }
    else
        pSC->WMISCP.uAlphaMode = 0;

    /*================================================ */
    FailIf((StrIODecInit(pSC) != ICERR_OK), WMP_errOutOfMemory);
    FailIf((StrDecInit(pSC) != ICERR_OK), WMP_errOutOfMemory);
    if (pNextSC) {
        /* 5. StrEncInit */
        FailIf((StrDecInit(pNextSC) != ICERR_OK), WMP_errOutOfMemory);
    }

    pSC->m_pNextSC = pNextSC;
    /*================================================ */
    *pII = pSC->WMII;
    *pSCP = pSC->WMISCP;
    *pctxSC = (CTXSTRCODEC)pSC;

    if(pSC->WMII.cPostProcStrength){
        initPostProc(pSC->pPostProcInfo, pSC->cmbWidth, pSC->m_param.cNumChannels);
        if (pSC->m_param.bAlphaChannel) 
            initPostProc(pNextSC->pPostProcInfo, pNextSC->cmbWidth, pNextSC->m_param.cNumChannels);
    }

    PERFTIMER_STOP(pSC->m_fMeasurePerf, pSC->m_ptEncDecPerf);

Cleanup:
    return WMP_errSuccess == err ? ICERR_OK : ICERR_ERROR;
}

Int ImageStrDecDecode(
    CTXSTRCODEC ctxSC,
    const CWMImageBufferInfo* pBI
#ifdef REENTRANT_MODE
    , size_t *pcDecodedLines
#endif
    )
{
    CWMImageStrCodec* pSC = (CWMImageStrCodec*)ctxSC;
    CWMImageStrCodec* pNextSC = pSC->m_pNextSC;
    size_t cMBRow, k;

    ImageDataProc ProcessLeft, ProcessCenter, ProcessRight;
    ImageDataProc Transform = NULL;
    const size_t iChromaElements = (pSC->m_param.cfColorFormat == YUV_420) ? 8 * 8 
        : ((pSC->m_param.cfColorFormat == YUV_422) ? 8 * 16 : 16 * 16);

    if (sizeof(*pSC) != pSC->cbStruct)
    {
        return ICERR_ERROR;
    }

    /*================================ */
    PERFTIMER_START(pSC->m_fMeasurePerf, pSC->m_ptEncDecPerf);

    pSC->WMIBI = *pBI;

#ifdef REENTRANT_MODE
    if (0 == pSC->WMIBI.uiFirstMBRow)
    {
        setROI(pSC);
        if (pNextSC) {
            pNextSC->WMIBI = pSC->WMIBI;
            setROI(pNextSC);
        }
    }
#else
    setROI(pSC);
    if (pNextSC) {
        pNextSC->WMIBI = pSC->WMIBI;
        setROI(pNextSC);
    }
#endif /* REENTRANT_MODE */

/* optimization flags can be defined only after ROI is set! */
#if defined(WMP_OPT_SSE2) || defined(WMP_OPT_CC_DEC) || defined(WMP_OPT_TRFM_DEC)
    StrDecOpt(pSC);
#endif /* OPT defined */



    cMBRow = (pSC->m_Dparam->bDecodeFullFrame ? pSC->cmbHeight : ((pSC->m_Dparam->cROIBottomY + 16) >> 4));

#ifdef REENTRANT_MODE
    if (0 == pSC->WMIBI.uiFirstMBRow)
    {
        if(initLookupTables(pSC) != ICERR_OK)
            return ICERR_ERROR;
        if (pNextSC && initLookupTables(pNextSC) != ICERR_OK)
            return ICERR_ERROR;
    }
#else
    if(initLookupTables(pSC) != ICERR_OK)
        return ICERR_ERROR;
    if (pNextSC && initLookupTables(pNextSC) != ICERR_OK)
        return ICERR_ERROR;
#endif /* REENTRANT_MODE */

#ifndef REENTRANT_MODE
    if(pSC->WMII.bdBitDepth == BD_1){
        size_t i;


        for(i = 0; i < pSC->WMIBI.cLine; i ++)
            memset(pSC->WMIBI.pv, 0, pSC->WMIBI.cbStride);
    }
#endif

    /*================================ */
    /* top row */
#ifdef REENTRANT_MODE
#else
    pSC->cRow = 0;
    ProcessLeft = pSC->ProcessTopLeft;
    ProcessCenter = pSC->ProcessTop;
    ProcessRight = pSC->ProcessTopRight;
    Transform = pSC->m_param.cSubVersion == CODEC_SUBVERSION ?
        invTransformMacroblock : invTransformMacroblock_alteredOperators_hard;
#endif /* REENTRANT_MODE */

#ifdef REENTRANT_MODE
    for (pSC->cRow = pSC->WMIBI.uiFirstMBRow; pSC->cRow <= pSC->WMIBI.uiLastMBRow; pSC->cRow++)
    {
        /* const COLORFORMAT cfExt = (pSC->m_param.cfColorFormat == Y_ONLY ? Y_ONLY : pSC->WMII.cfColorFormat); */

        if (0 == pSC->cRow)
        {
            ProcessLeft = pSC->ProcessTopLeft;
            ProcessCenter = pSC->ProcessTop;
            ProcessRight = pSC->ProcessTopRight;
            Transform = pSC->m_param.cSubVersion == CODEC_SUBVERSION ?
                invTransformMacroblock : invTransformMacroblock_alteredOperators_hard;
        }
        else if (cMBRow == pSC->cRow)
        {
            /*================================ */
            /* bottom row */
            ProcessLeft = pSC->ProcessBottomLeft;
            ProcessCenter = pSC->ProcessBottom;
            ProcessRight = pSC->ProcessBottomRight;
            Transform = pSC->m_param.cSubVersion == CODEC_SUBVERSION ?
                invTransformMacroblock : invTransformMacroblock_alteredOperators_hard;
        }
        else { /* middle rows */
            ProcessLeft = pSC->ProcessLeft;
            ProcessCenter = pSC->ProcessCenter;
            ProcessRight = pSC->ProcessRight;
            Transform = pSC->TransformCenter;
        }
#else
    /*================================ */
    /* central rows */
    for(pSC->cRow = 0; pSC->cRow <= cMBRow; pSC->cRow++)
    {
#endif /* REENTRANT_MODE */
        pSC->cColumn = 0;
        initMRPtr(pSC);
        /** zero out the transform coefficients (pull this out to once per MB row) **/
        memset(pSC->p1MBbuffer[0], 0, sizeof(PixelI) * 16 * 16 * pSC->cmbWidth);
        for (k = 1; k < pSC->m_param.cNumChannels; k++) {
            memset(pSC->p1MBbuffer[k], 0, sizeof(PixelI) * iChromaElements * pSC->cmbWidth);
        }
        if (pSC->m_pNextSC != NULL) {  /* alpha channel */
            memset(pSC->m_pNextSC->p1MBbuffer[0], 0, sizeof(PixelI) * 16 * 16 * pSC->m_pNextSC->cmbWidth);
        }

        if(ProcessLeft(pSC) != ICERR_OK)
            return ICERR_ERROR;
        advanceMRPtr(pSC);

        pSC->Transform = Transform;
        for (pSC->cColumn = 1; pSC->cColumn < pSC->cmbWidth; ++pSC->cColumn)
        {
            if(ProcessCenter(pSC) != ICERR_OK)
                return ICERR_ERROR;
            advanceMRPtr(pSC);
        }
        pSC->Transform = pSC->m_param.cSubVersion == CODEC_SUBVERSION ?
            invTransformMacroblock : invTransformMacroblock_alteredOperators_hard;

        if(ProcessRight(pSC) != ICERR_OK)
            return ICERR_ERROR;

        if (pSC->cRow) {
            if(pSC->m_Dparam->cThumbnailScale < 2 && (pSC->m_Dparam->bDecodeFullFrame || 
                ((pSC->cRow * 16 > pSC->m_Dparam->cROITopY) && (pSC->cRow * 16 <= pSC->m_Dparam->cROIBottomY + 16)))) {
                if( pSC->Load(pSC) != ICERR_OK ) /* bypass CC for thumbnail decode */
            		return ICERR_ERROR;
            }

            if(pSC->m_Dparam->cThumbnailScale >= 2) /* decode thumbnail */
                decodeThumbnail(pSC);
        }

        advanceOneMBRow(pSC);
        swapMRPtr(pSC);
#ifdef REENTRANT_MODE
        *pcDecodedLines = pSC->WMIBI.cLinesDecoded;
#else
        if (pSC->cRow == cMBRow - 1) {
        /*================================ */
        /* bottom row */
            ProcessLeft = pSC->ProcessBottomLeft;
            ProcessCenter = pSC->ProcessBottom;
            ProcessRight = pSC->ProcessBottomRight;
            Transform = pSC->m_param.cSubVersion == CODEC_SUBVERSION ?
                invTransformMacroblock : invTransformMacroblock_alteredOperators_hard;
        }
        else {
            ProcessLeft = pSC->ProcessLeft;
            ProcessCenter = pSC->ProcessCenter;
            ProcessRight = pSC->ProcessRight;
            Transform = pSC->TransformCenter;
        }
#endif /* REENTRANT_MODE */
    }

#ifndef REENTRANT_MODE
    fixup_Y_ONLY_to_Others(pSC, pBI);
#endif /* REENTRANT_MODE */

    PERFTIMER_STOP(pSC->m_fMeasurePerf, pSC->m_ptEncDecPerf);
    return ICERR_OK;
}

Int ImageStrDecTerm(
    CTXSTRCODEC ctxSC)
{
    CWMImageStrCodec* pSC = (CWMImageStrCodec*)ctxSC;
    if (NULL == pSC)
    {
        return ICERR_OK;
    }
    if (sizeof(*pSC) != pSC->cbStruct)
    {
        return ICERR_ERROR;
    }

    PERFTIMER_START(pSC->m_fMeasurePerf, pSC->m_ptEncDecPerf);

    StrDecTerm(pSC);
    PERFTIMER_STOP(pSC->m_fMeasurePerf, pSC->m_ptEncDecPerf);
    PERFTIMER_REPORT(pSC->m_fMeasurePerf, pSC);
    PERFTIMER_DELETE(pSC->m_fMeasurePerf, pSC->m_ptEncDecPerf);
    PERFTIMER_DELETE(pSC->m_fMeasurePerf, pSC->m_ptEndToEndPerf);

    free(pSC);

    return ICERR_OK;
}


#undef TRACE_MALLOC
#undef TRACE_NEW
#undef TRACE_HEAP
#undef _FORCEINLINE
#undef _ICC
#undef _ICC_CMYK
#undef _CLIP2
#undef _CLIP8
#undef _CLIP16
#undef _CLIPU16
#undef min
#undef max
#undef fixup
#undef fixupFullSize
#undef fixupThumb
/* ---- strInvTransform.c ---- */
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

/* (merged) #include "strTransform.h" */
/* (merged) #include "strcodec.h" */
/* (merged) #include "decode.h" */

/** rotation by -pi/8 **/
#define IROTATE1(a, b) (a) -= (((b) + 1) >> 1), (b) += (((a) + 1) >> 1)  /* this works well too */
#define IROTATE2(a, b) (a) -= (((b)*3 + 4) >> 3), (b) += (((a)*3 + 4) >> 3)  /* this works well too */

/** local functions **/
static Void invOddOdd(PixelI *, PixelI *, PixelI *, PixelI *);
static Void invOddOddPost(PixelI *, PixelI *, PixelI *, PixelI *);
static Void invOdd(PixelI *, PixelI *, PixelI *, PixelI *);
static Void strHSTdec(PixelI *, PixelI *, PixelI *, PixelI *);
static Void strHSTdec1(PixelI *, PixelI *);
static Void strHSTdec1_alternate(PixelI *, PixelI *);
static Void strHSTdec1_edge(PixelI *pa, PixelI *pd);

/** IDCT stuff **/
/** reordering should be combined with zigzag scan **/
/** data order before IDCT **/
/** 0  8  4  6 **/
/** 2 10 14 12 **/
/** 1 11 15 13 **/
/** 9  3  7  5 **/
/** data order after IDCT **/
/**  0  1  2  3 **/
/**  4  5  6  7 **/
/**  8  9 10 11 **/
/** 12 13 14 15 **/
Void strIDCT4x4Stage1(PixelI* p)
{
    /** top left corner, butterfly => butterfly **/
    strDCT2x2up(p + 0, p + 1, p + 2, p + 3);

    /** top right corner, -pi/8 rotation => butterfly **/
    invOdd(p + 5, p + 4, p + 7, p + 6);

    /** bottom left corner, butterfly => -pi/8 rotation **/
    invOdd(p + 10, p + 8, p + 11, p + 9);

    /** bottom right corner, -pi/8 rotation => -pi/8 rotation **/
    invOddOdd(p + 15, p + 14, p + 13, p + 12);
    
    /** butterfly **/
    /*FOURBUTTERFLY(p, 0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15); */
    FOURBUTTERFLY_HARDCODED1(p);
}

Void strIDCT4x4Stage2(PixelI* p)
{
    /** bottom left corner, butterfly => -pi/8 rotation **/
    invOdd(p + 32, p + 48, p + 96, p + 112);
    
    /** top right corner, -pi/8 rotation => butterfly **/
    invOdd(p + 128, p + 192, p + 144, p + 208);
    
    /** bottom right corner, -pi/8 rotation => -pi/8 rotation **/
    invOddOdd(p + 160, p + 224, p + 176, p + 240);

    /** top left corner, butterfly => butterfly **/
    strDCT2x2up(p + 0, p + 64, p + 16, p + 80);
    
    /** butterfly **/
    FOURBUTTERFLY(p, 0, 192, 48, 240, 64, 128, 112, 176, 16, 208, 32, 224, 80, 144, 96, 160);
}

Void strNormalizeDec(PixelI* p, Bool bChroma)
{
    int i;
    if (!bChroma) {
        /*for (i = 0; i < 256; i += 16) { */
        /*    p[i] <<= 2; */
        /*} */
    }
    else {
        for (i = 0; i < 256; i += 16) {
            p[i] += p[i];
        }
    }
}

/** 2x2 DCT with post-scaling - for use on decoder side **/
Void strDCT2x2dnDec(PixelI *pa, PixelI *pb, PixelI *pc, PixelI *pd)
{
    PixelI a, b, c, d, C, t;
    a = *pa;
    b = *pb;
    C = *pc;
    d = *pd;
  
    a += d;
    b -= C;
    t = ((a - b) >> 1);
    c = t - d;
    d = t - C;
    a -= d;
    b += c;

    *pa = a * 2;
    *pb = b * 2;
    *pc = c * 2;
    *pd = d * 2;
}


/** post filter stuff **/
/** 2-point post for boundaries **/
Void strPost2(PixelI * a, PixelI * b)
{
    *b += ((*a + 4) >> 3);
    *a += ((*b + 2) >> 2);
    *b += ((*a + 4) >> 3);
}

Void strPost2_alternate(PixelI * pa, PixelI * pb)
{
    PixelI a, b;
    a = *pa;
    b = *pb;

    /** rotate **/
    b += ((a + 2) >> 2);
    a += ((b + 1) >> 1);
    a += (b >> 5);
    a += (b >> 9);
    a += (b >> 13);

    b += ((a + 2) >> 2);

    *pa = a;
    *pb = b;
}

Void strPost2x2(PixelI *pa, PixelI *pb, PixelI *pc, PixelI *pd)
{
    PixelI a, b, c, d;
    a = *pa;
    b = *pb;
    c = *pc;
    d = *pd;

    /** butterflies **/
    a += d;
    b += c;
    d -= (a + 1) >> 1;
    c -= (b + 1) >> 1;

    /** rotate **/
    b += ((a + 2) >> 2);
    a += ((b + 1) >> 1);
    b += ((a + 2) >> 2);

    /** butterflies **/
    d += (a + 1) >> 1;
    c += (b + 1) >> 1;
    a -= d;
    b -= c;

    *pa = a;
    *pb = b;
    *pc = c;
    *pd = d;
}

Void strPost2x2_alternate(PixelI *pa, PixelI *pb, PixelI *pc, PixelI *pd)
{
    PixelI a, b, c, d;
    a = *pa;
    b = *pb;
    c = *pc;
    d = *pd;

    /** butterflies **/
    a += d;
    b += c;
    d -= (a + 1) >> 1;
    c -= (b + 1) >> 1;

    /** rotate **/
    b += ((a + 2) >> 2);
    a += ((b + 1) >> 1);
    a += (b >> 5);
    a += (b >> 9);
    a += (b >> 13);
    b += ((a + 2) >> 2);

    /** butterflies **/
    d += (a + 1) >> 1;
    c += (b + 1) >> 1;
    a -= d;
    b -= c;

    *pa = a;
    *pb = b;
    *pc = c;
    *pd = d;
}

/** 4-point post for boundaries **/
Void strPost4(PixelI *pa, PixelI *pb, PixelI *pc, PixelI *pd)
{
    PixelI a, b, c, d;
    a = *pa;
    b = *pb;
    c = *pc;
    d = *pd;

    a += d, b += c;
    d -= ((a + 1) >> 1), c -= ((b + 1) >> 1);
    
    IROTATE1(c, d);

    d += ((a + 1) >> 1), c += ((b + 1) >> 1);
    a -= d - ((d * 3 + 16) >> 5), b -= c - ((c * 3 + 16) >> 5);
    d += ((a * 3 + 8) >> 4), c += ((b * 3 + 8) >> 4);
    a += ((d * 3 + 16) >> 5), b += ((c * 3 + 16) >> 5);

    *pa = a;
    *pb = b;
    *pc = c;
    *pd = d;
}

Void strPost4_alternate(PixelI *pa, PixelI *pb, PixelI *pc, PixelI *pd)
{
    PixelI a, b, c, d;
    a = *pa;
    b = *pb;
    c = *pc;
    d = *pd;

    a += d, b += c;
    d -= ((a + 1) >> 1), c -= ((b + 1) >> 1);
    
    strHSTdec1_edge(&a, &d); strHSTdec1_edge(&b, &c);
    IROTATE1(c, d);
    d += ((a + 1) >> 1), c += ((b + 1) >> 1);

    a -= d, b -= c;

    *pa = a;
    *pb = b;
    *pc = c;
    *pd = d;
}

/*****************************************************************************************
  Input data offsets:
  (15)(14)|(10+64)(11+64) p0 (15)(14)|(74)(75)
  (13)(12)|( 8+64)( 9+64)    (13)(12)|(72)(73)
  --------+--------------    --------+--------
  ( 5)( 4)|( 0+64) (1+64) p1 ( 5)( 4)|(64)(65)
  ( 7)( 6)|( 2+64) (3+64)    ( 7)( 6)|(66)(67)
*****************************************************************************************/
Void DCCompensate (PixelI *a, PixelI *b, PixelI *c, PixelI *d, int iDC)
{
    iDC = iDC>>1;
    *a -= iDC;
    *d -= iDC;
    *b += iDC;
    *c += iDC;
}

#ifndef max
#define max(a,b)            (((a) > (b)) ? (a) : (b))
#endif

#ifndef min
#define min(a,b)            (((a) < (b)) ? (a) : (b))
#endif

int ClipDCL(int iDCL, int iAltDCL)
{
    int iClipDCL = 0;
    if (iDCL > 0) {
        if (iAltDCL > 0)
            iClipDCL = min(iDCL, iAltDCL);
        else
            iClipDCL = 0;
    }
    else if (iDCL < 0) {
        if (iAltDCL < 0)
            iClipDCL = max(iDCL, iAltDCL);
        else
            iClipDCL = 0;
    }
    return iClipDCL;
}

Void strPost4x4Stage1Split(PixelI *p0, PixelI *p1, Int iOffset, Int iHPQP, Bool bHPAbsent)
{
    int iDCLAlt1, iDCLAlt2, iDCLAlt3, iDCLAlt0;
    int iDCL1, iDCL2, iDCL3, iDCL0;
    int iTmp1, iTmp2, iTmp3, iTmp0;

    PixelI *p2 = p0 + 72 - iOffset;
    PixelI *p3 = p1 + 64 - iOffset;
    p0 += 12;
    p1 += 4;

    /** buttefly **/
    strDCT2x2dn(p0 + 0, p2 + 0, p1 + 0, p3 + 0);
    strDCT2x2dn(p0 + 1, p2 + 1, p1 + 1, p3 + 1);
    strDCT2x2dn(p0 + 2, p2 + 2, p1 + 2, p3 + 2);
    strDCT2x2dn(p0 + 3, p2 + 3, p1 + 3, p3 + 3);

    /** bottom right corner: -pi/8 rotation => -pi/8 rotation **/
    invOddOddPost(p3 + 0, p3 + 1, p3 + 2, p3 + 3);
    
    /** anti diagonal corners: rotation by -pi/8 **/
    IROTATE1(p1[2], p1[3]);
    IROTATE1(p1[0], p1[1]);
    IROTATE1(p2[1], p2[3]);
    IROTATE1(p2[0], p2[2]);

    /** butterfly **/
    strHSTdec1(p0 + 0, p3 + 0);
    strHSTdec1(p0 + 1, p3 + 1);
    strHSTdec1(p0 + 2, p3 + 2);
    strHSTdec1(p0 + 3, p3 + 3);
    strHSTdec(p0 + 0, p2 + 0, p1 + 0, p3 + 0);
    strHSTdec(p0 + 1, p2 + 1, p1 + 1, p3 + 1);
    strHSTdec(p0 + 2, p2 + 2, p1 + 2, p3 + 2);
    strHSTdec(p0 + 3, p2 + 3, p1 + 3, p3 + 3);

    iTmp0 = (*(p0 +0) + *(p1 +0) + *(p2 +0) + *(p3 +0))>>1;
    iTmp1 = (*(p0 +1) + *(p1 +1) + *(p2 +1) + *(p3 +1))>>1;
    iTmp2 = (*(p0 +2) + *(p1 +2) + *(p2 +2) + *(p3 +2))>>1;
    iTmp3 = (*(p0 +3) + *(p1 +3) + *(p2 +3) + *(p3 +3))>>1;
    iDCL0 = (iTmp0 * 595 + 65536)>>17; /*Approximating 27/5947 */
    iDCL1 = (iTmp1 * 595 + 65536)>>17; 
    iDCL2 = (iTmp2 * 595 + 65536)>>17; 
    iDCL3 = (iTmp3 * 595 + 65536)>>17; 
    if ((abs(iDCL0) < iHPQP && iHPQP > 20) || bHPAbsent) {
        iDCLAlt0 = (*(p0 +0) - *(p1 +0) - *(p2 +0) + *(p3 +0))>>1;
        iDCL0 = ClipDCL (iDCL0, iDCLAlt0);
        DCCompensate (p0 + 0, p2 + 0, p1 + 0, p3 + 0, iDCL0);
    }
    if ((abs(iDCL1) < iHPQP && iHPQP > 20) || bHPAbsent) {
            iDCLAlt1 = (*(p0 +1) - *(p1 +1) - *(p2 +1) + *(p3 +1))>>1;
            iDCL1 = ClipDCL (iDCL1, iDCLAlt1);
        DCCompensate (p0 + 1, p2 + 1, p1 + 1, p3 + 1, iDCL1);
    }
    if ((abs(iDCL2) < iHPQP && iHPQP > 20) || bHPAbsent) {
            iDCLAlt2 = (*(p0 +2) - *(p1 +2) - *(p2 +2) + *(p3 +2))>>1;
            iDCL2 = ClipDCL (iDCL2, iDCLAlt2);
        DCCompensate (p0 + 2, p2 + 2, p1 + 2, p3 + 2, iDCL2);
    }
    if ((abs(iDCL3) < iHPQP && iHPQP > 20) || bHPAbsent) {
            iDCLAlt3 = (*(p0 +3) - *(p1 +3) - *(p2 +3) + *(p3 +3))>>1;
            iDCL3 = ClipDCL (iDCL3, iDCLAlt3);
        DCCompensate (p0 + 3, p2 + 3, p1 + 3, p3 + 3, iDCL3);
    }
}

Void strPost4x4Stage1(PixelI* p, Int iOffset, Int iHPQP, Bool bHPAbsent)
{
    strPost4x4Stage1Split(p, p + 16, iOffset, iHPQP, bHPAbsent);
}

Void strPost4x4Stage1Split_alternate(PixelI *p0, PixelI *p1, Int iOffset)
{
    PixelI *p2 = p0 + 72 - iOffset;
    PixelI *p3 = p1 + 64 - iOffset;
    p0 += 12;
    p1 += 4;

    /** buttefly **/
    strDCT2x2dn(p0 + 0, p2 + 0, p1 + 0, p3 + 0);
    strDCT2x2dn(p0 + 1, p2 + 1, p1 + 1, p3 + 1);
    strDCT2x2dn(p0 + 2, p2 + 2, p1 + 2, p3 + 2);
    strDCT2x2dn(p0 + 3, p2 + 3, p1 + 3, p3 + 3);

    /** bottom right corner: -pi/8 rotation => -pi/8 rotation **/
    invOddOddPost(p3 + 0, p3 + 1, p3 + 2, p3 + 3);
    
    /** anti diagonal corners: rotation by -pi/8 **/
    IROTATE1(p1[2], p1[3]);
    IROTATE1(p1[0], p1[1]);
    IROTATE1(p2[1], p2[3]);
    IROTATE1(p2[0], p2[2]);

    /** butterfly **/
    strHSTdec1_alternate(p0 + 0, p3 + 0);
    strHSTdec1_alternate(p0 + 1, p3 + 1);
    strHSTdec1_alternate(p0 + 2, p3 + 2);
    strHSTdec1_alternate(p0 + 3, p3 + 3);
    strHSTdec(p0 + 0, p2 + 0, p1 + 0, p3 + 0);
    strHSTdec(p0 + 1, p2 + 1, p1 + 1, p3 + 1);
    strHSTdec(p0 + 2, p2 + 2, p1 + 2, p3 + 2);
    strHSTdec(p0 + 3, p2 + 3, p1 + 3, p3 + 3);
}

Void strPost4x4Stage1_alternate(PixelI* p, Int iOffset)
{
    strPost4x4Stage1Split_alternate(p, p + 16, iOffset);
}

/*****************************************************************************************
  Input data offsets:
  (15)(14)|(10+32)(11+32) p0 (15)(14)|(42)(43)
  (13)(12)|( 8+32)( 9+32)    (13)(12)|(40)(41)
  --------+--------------    --------+--------
  ( 5)( 4)|( 0+32) (1+32) p1 ( 5)( 4)|(32)(33)
  ( 7)( 6)|( 2+32) (3+32)    ( 7)( 6)|(34)(35)
*****************************************************************************************/

/*****************************************************************************************
  Input data offsets:
  ( -96)(-32)|(32)( 96) p0
  ( -80)(-16)|(48)(112)
  -----------+------------
  (-128)(-64)|( 0)( 64) p1
  (-112)(-48)|(16)( 80)
*****************************************************************************************/
Void strPost4x4Stage2Split(PixelI* p0, PixelI* p1)
{
    /** buttefly **/
    strDCT2x2dn(p0 - 96, p0 +  96, p1 - 112, p1 + 80);
    strDCT2x2dn(p0 - 32, p0 +  32, p1 -  48, p1 + 16);
    strDCT2x2dn(p0 - 80, p0 + 112, p1 - 128, p1 + 64);
    strDCT2x2dn(p0 - 16, p0 +  48, p1 -  64, p1 +  0);

    /** bottom right corner: -pi/8 rotation => -pi/8 rotation **/
    invOddOddPost(p1 + 0, p1 + 64, p1 + 16, p1 + 80);
    
    /** anti diagonal corners: rotation by -pi/8 **/
    IROTATE1(p0[ 48], p0[  32]);
    IROTATE1(p0[112], p0[  96]);
    IROTATE1(p1[-64], p1[-128]);
    IROTATE1(p1[-48], p1[-112]);
    
    /** butterfly **/
    strHSTdec1(p0 - 96, p1 + 80);
    strHSTdec1(p0 - 32, p1 + 16);
    strHSTdec1(p0 - 80, p1 + 64);
    strHSTdec1(p0 - 16, p1 +  0);

    strHSTdec(p0 - 96, p1 - 112, p0 +  96, p1 + 80);
    strHSTdec(p0 - 32, p1 -  48, p0 +  32, p1 + 16);
    strHSTdec(p0 - 80, p1 - 128, p0 + 112, p1 + 64);
    strHSTdec(p0 - 16, p1 -  64, p0 +  48, p1 +  0);
}

Void strPost4x4Stage2Split_alternate(PixelI* p0, PixelI* p1)
{
    /** buttefly **/
    strDCT2x2dn(p0 - 96, p0 +  96, p1 - 112, p1 + 80);
    strDCT2x2dn(p0 - 32, p0 +  32, p1 -  48, p1 + 16);
    strDCT2x2dn(p0 - 80, p0 + 112, p1 - 128, p1 + 64);
    strDCT2x2dn(p0 - 16, p0 +  48, p1 -  64, p1 +  0);

    /** bottom right corner: -pi/8 rotation => -pi/8 rotation **/
    invOddOddPost(p1 + 0, p1 + 64, p1 + 16, p1 + 80);
    
    /** anti diagonal corners: rotation by -pi/8 **/
    IROTATE1(p0[ 48], p0[  32]);
    IROTATE1(p0[112], p0[  96]);
    IROTATE1(p1[-64], p1[-128]);
    IROTATE1(p1[-48], p1[-112]);
    
    /** butterfly **/
    strHSTdec1_alternate(p0 - 96, p1 + 80);
    strHSTdec1_alternate(p0 - 32, p1 + 16);
    strHSTdec1_alternate(p0 - 80, p1 + 64);
    strHSTdec1_alternate(p0 - 16, p1 +  0);

    strHSTdec(p0 - 96, p1 - 112, p0 +  96, p1 + 80);
    strHSTdec(p0 - 32, p1 -  48, p0 +  32, p1 + 16);
    strHSTdec(p0 - 80, p1 - 128, p0 + 112, p1 + 64);
    strHSTdec(p0 - 16, p1 -  64, p0 +  48, p1 +  0);
}

/** 
    Hadamard+Scale transform
    for some strange reason, breaking up the function into two blocks, strHSTdec1 and strHSTdec
    seems to work faster
**/
static Void strHSTdec1(PixelI *pa, PixelI *pd)
{
    /** different realization : does rescaling as well! **/
    PixelI a, d;
    a = *pa;
    d = *pd;

    a += d;
    d = (a >> 1) - d;
    a += (d * 3 + 0) >> 3;
    d += (a * 3 + 0) >> 4;
    /*a += (d * 3 + 4) >> 3; */

    *pa = a;
    *pd = d;
}

static Void strHSTdec1_alternate(PixelI *pa, PixelI *pd)
{
    /** different realization : does rescaling as well! **/
    PixelI a, d;
    a = *pa;
    d = *pd;

    a += d;
    d = (a >> 1) - d;
    a += (d * 3 + 0) >> 3;
    d += (a * 3 + 0) >> 4;
    /*a += (d * 3 + 4) >> 3; */

    d += (a >> 7);
    d -= (a >> 10);

    *pa = a;
    *pd = d;
}

static Void strHSTdec1_edge (PixelI *pa, PixelI *pd)
{
    /** different realization as compared to scaling operator for 2D case **/
    PixelI a, d;
    a = *pa;
    d = *pd;

    a += d;
    d = (a >> 1) - d;
    a += (d * 3 + 0) >> 3;
    d += (a * 3 + 0) >> 4;

    /*Scaling modification of adding 7/1024 in 2 steps (without multiplication by 7). */
    d += (a >> 7);
    d -= (a >> 10);

    a += (d * 3 + 4) >> 3;
    d -= (a >> 1);
    a += d;
    /* End new operations */

    *pa = a;
    *pd = -d; /* Negative sign needed here for 1D scaling case to ensure correct scaling. */
}

static Void strHSTdec(PixelI *pa, PixelI *pb, PixelI *pc, PixelI *pd)
{
    /** different realization : does rescaling as well! **/
    PixelI a, b, c, d;
    a = *pa;
    b = *pb;
    c = *pc;
    d = *pd;

    b -= c;
    a += (d * 3 + 4) >> 3;

    d -= (b >> 1);
    c = ((a - b) >> 1) - c;
    *pc = d;
    *pd = c;
    *pa = a - c, *pb = b + d;
}

/** Kron(Rotate(pi/8), Rotate(pi/8)) **/
static Void invOddOdd(PixelI *pa, PixelI *pb, PixelI *pc, PixelI *pd)
{
    PixelI a, b, c, d, t1, t2;
    a = *pa;
    b = *pb;
    c = *pc;
    d = *pd;

    /** butterflies **/
    d += a;
    c -= b;
    a -= (t1 = d >> 1);
    b += (t2 = c >> 1);

    /** rotate pi/4 **/
    a -= (b * 3 + 3) >> 3;
    b += (a * 3 + 3) >> 2;
    a -= (b * 3 + 4) >> 3;

    /** butterflies **/
    b -= t2;
    a += t1;
    c += b;
    d -= a;

    /** sign flips **/
    *pa = a;
    *pb = -b;
    *pc = -c;
    *pd = d;
}

/** Kron(Rotate(pi/8), Rotate(pi/8)) **/
static Void invOddOddPost(PixelI *pa, PixelI *pb, PixelI *pc, PixelI *pd)
{
    PixelI a, b, c, d, t1, t2;
    a = *pa;
    b = *pb;
    c = *pc;
    d = *pd;

    /** butterflies **/
    d += a;
    c -= b;
    a -= (t1 = d >> 1);
    b += (t2 = c >> 1);

    /** rotate pi/4 **/
    a -= (b * 3 + 6) >> 3;
    b += (a * 3 + 2) >> 2;
    a -= (b * 3 + 4) >> 3;

    /** butterflies **/
    b -= t2;
    a += t1;
    c += b;
    d -= a;

    *pa = a;
    *pb = b;
    *pc = c;
    *pd = d;
}


/** Kron(Rotate(-pi/8), [1 1; 1 -1]/sqrt(2)) **/
/** [D C A B] => [a b c d] **/
Void invOdd(PixelI *pa, PixelI *pb, PixelI *pc, PixelI *pd)
{
    PixelI a, b, c, d;
    a = *pa;
    b = *pb;
    c = *pc;
    d = *pd;

    /** butterflies **/
    b += d;
    a -= c;
    d -= (b) >> 1;
    c += (a + 1) >> 1;

    /** rotate pi/8 **/
    IROTATE2(a, b);
    IROTATE2(c, d);

    /** butterflies **/
    c -= (b + 1) >> 1;
    d = ((a + 1) >> 1) - d;
    b += c;
    a -= d;

    *pa = a;
    *pb = b;
    *pc = c;
    *pd = d;
}

/*************************************************************************
  Top-level function to inverse tranform possible part of a macroblock
*************************************************************************/
Int  invTransformMacroblock(CWMImageStrCodec * pSC)
{
    const OVERLAP olOverlap = pSC->WMISCP.olOverlap;
    const COLORFORMAT cfColorFormat = pSC->m_param.cfColorFormat;
    /* const BITDEPTH_BITS bdBitDepth = pSC->WMII.bdBitDepth; */
    const Bool left = (pSC->cColumn == 0), right = (pSC->cColumn == pSC->cmbWidth);
    const Bool top = (pSC->cRow == 0), bottom = (pSC->cRow == pSC->cmbHeight);
    const Bool topORbottom = (top || bottom), leftORright = (left || right);
    const Bool topORleft = (top || left), bottomORright = (bottom || right);
    const size_t mbWidth = pSC->cmbWidth, mbX = pSC->cColumn;
    PixelI * p = NULL;/* * pt = NULL; */
    size_t i;
    const size_t iChannels = (cfColorFormat == YUV_420 || cfColorFormat == YUV_422) ? 1 : pSC->m_param.cNumChannels;
    const size_t tScale = pSC->m_Dparam->cThumbnailScale;
    Int j = 0;

    Int qp[MAX_CHANNELS], dcqp[MAX_CHANNELS], iStrength = (1 << pSC->WMII.cPostProcStrength);
    /* ERR_CODE result = ICERR_OK; */

    Bool bHPAbsent = (pSC->WMISCP.sbSubband == SB_NO_HIGHPASS || pSC->WMISCP.sbSubband == SB_DC_ONLY);

    if(pSC->WMII.cPostProcStrength > 0){
        /* threshold for post processing */
        for(i = 0; i < iChannels; i ++){
            qp[i] = pSC->pTile[pSC->cTileColumn].pQuantizerLP[i][pSC->MBInfo.iQIndexLP].iQP * iStrength * (olOverlap == OL_NONE ? 2 : 1);
            dcqp[i] = pSC->pTile[pSC->cTileColumn].pQuantizerDC[i][0].iQP * iStrength;
        }

        if(left) /* a new MB row */
            slideOneMBRow(pSC->pPostProcInfo, pSC->m_param.cNumChannels, mbWidth, top, bottom);  /* previous current row becomes previous row */
    }

    /*================================================================ */
    /* 400_Y, 444_YUV */
    for (i = 0; i < iChannels && tScale < 16; ++i)
    {
        PixelI* const p0 = pSC->p0MBbuffer[i];
        PixelI* const p1 = pSC->p1MBbuffer[i];

        Int iHPQP = 255;
        if (!bHPAbsent)
            iHPQP = pSC->pTile[pSC->cTileColumn].pQuantizerHP[i][pSC->MBInfo.iQIndexHP].iQP;

        /*================================ */
        /* second level inverse transform */
        if (!bottomORright)
        {
            if(pSC->WMII.cPostProcStrength > 0)
                updatePostProcInfo(pSC->pPostProcInfo, p1, mbX, i); /* update postproc info before IDCT */

            strIDCT4x4Stage2(p1);
            if (pSC->m_param.bScaledArith) {
                strNormalizeDec(p1, (i != 0));
            }
        }

        /*================================ */
        /* second level inverse overlap */
        if (OL_TWO == olOverlap)
        {
            if (leftORright && (!topORbottom))
            {
                j = left ? 0 : -128;
                strPost4(p0 + j + 32, p0 + j +  48, p1 + j +  0, p1 + j + 16);
                strPost4(p0 + j + 96, p0 + j + 112, p1 + j + 64, p1 + j + 80);
            }

            if (!leftORright)
            {
                if (topORbottom)
                {
                    p = top ? p1 : p0 + 32;
                    strPost4(p - 128, p - 64, p +  0, p + 64);
                    strPost4(p - 112, p - 48, p + 16, p + 80);
                    p = NULL;
                }
                else
                {
                    strPost4x4Stage2Split(p0, p1);
                }
            }
        }

        if(pSC->WMII.cPostProcStrength > 0)
            postProcMB(pSC->pPostProcInfo, p0, p1, mbX, i, dcqp[i]); /* second stage deblocking */

        /*================================ */
        /* first level inverse transform */
        if(tScale >= 4) /* bypass first level transform for 4:1 and smaller thumbnail */
            continue;

        if (!top)
        {
            for (j = (left ? 32 : -96); j < (right ? 32 : 160); j += 64)
            {
                strIDCT4x4Stage1(p0 + j +  0);
                strIDCT4x4Stage1(p0 + j + 16);
            }
        }

        if (!bottom)
        {
            for (j = (left ? 0 : -128); j < (right ? 0 : 128); j += 64)
            {
                strIDCT4x4Stage1(p1 + j +  0);
                strIDCT4x4Stage1(p1 + j + 16);
            }
        }

        /*================================ */
        /* first level inverse overlap */
        if (OL_NONE != olOverlap)
        {
            if (leftORright)
            {
                j = left ? 0 + 10 : -64 + 14;
                if (!top)
                {
                    p = p0 + 16 + j;
                    strPost4(p +  0, p -  2, p +  6, p +  8);
                    strPost4(p +  1, p -  1, p +  7, p +  9);
                    strPost4(p + 16, p + 14, p + 22, p + 24);
                    strPost4(p + 17, p + 15, p + 23, p + 25);
                    p = NULL;
                }
                if (!bottom)
                {
                    p = p1 + j;
                    strPost4(p + 0, p - 2, p + 6, p + 8);
                    strPost4(p + 1, p - 1, p + 7, p + 9);
                    p = NULL;
                }
                if (!topORbottom)
                {
                    strPost4(p0 + 48 + j + 0, p0 + 48 + j - 2, p1 - 10 + j, p1 - 8 + j);
                    strPost4(p0 + 48 + j + 1, p0 + 48 + j - 1, p1 -  9 + j, p1 - 7 + j);
                }
            }

            if (top)
            {
                for (j = (left ? 0 : -192); j < (right ? -64 : 64); j += 64)
                {
                    p = p1 + j;
                    strPost4(p + 5, p + 4, p + 64, p + 65);
                    strPost4(p + 7, p + 6, p + 66, p + 67);
                    p = NULL;

                    strPost4x4Stage1(p1 + j, 0, iHPQP, bHPAbsent);
                }
            }
            else if (bottom)
            {
                for (j = (left ? 0 : -192); j < (right ? -64 : 64); j += 64)
                {
                    strPost4x4Stage1(p0 + 16 + j, 0, iHPQP, bHPAbsent);
                    strPost4x4Stage1(p0 + 32 + j, 0, iHPQP, bHPAbsent);

                    p = p0 + 48 + j;
                    strPost4(p + 15, p + 14, p + 74, p + 75);
                    strPost4(p + 13, p + 12, p + 72, p + 73);
                    p = NULL;
                }
            }
            else
            {
                for (j = (left ? 0 : -192); j < (right ? -64 : 64); j += 64)
                {
                    strPost4x4Stage1(p0 + 16 + j, 0, iHPQP, bHPAbsent);
                    strPost4x4Stage1(p0 + 32 + j, 0, iHPQP, bHPAbsent);
                    strPost4x4Stage1Split(p0 + 48 + j, p1 + j, 0, iHPQP, bHPAbsent);
                    strPost4x4Stage1(p1 + j, 0, iHPQP, bHPAbsent);
                }
            }
        }
        
        if(pSC->WMII.cPostProcStrength > 0 && (!topORleft))
            postProcBlock(pSC->pPostProcInfo, p0, p1, mbX, i, qp[i]); /* destairing and first stage deblocking */
    }

    /*================================================================ */
    /* 420_UV */
    for (i = 0; i < (YUV_420 == cfColorFormat? 2U : 0U) && tScale < 16; ++i)
    {
        PixelI* const p0 = pSC->p0MBbuffer[1 + i];/*(0 == i ? pSC->pU0 : pSC->pV0); */
        PixelI* const p1 = pSC->p1MBbuffer[1 + i];/*(0 == i ? pSC->pU1 : pSC->pV1); */

        Int iHPQP = 255;
        if (!bHPAbsent)
            iHPQP = pSC->pTile[pSC->cTileColumn].pQuantizerHP[i][pSC->MBInfo.iQIndexHP].iQP;

        /*======================================== */
        /* second level inverse transform (420_UV) */
        if (!bottomORright)
        {
            if (!pSC->m_param.bScaledArith) {
                strDCT2x2dn(p1, p1 + 32, p1 + 16, p1 + 48);
            }
            else {
                strDCT2x2dnDec(p1, p1 + 32, p1 + 16, p1 + 48);
            }
        }
        
        /*======================================== */
        /* second level inverse overlap (420_UV) */
        if (OL_TWO == olOverlap)
        {
            if (leftORright && !topORbottom)
            {
                j = (left ? 0 : -32);
                strPost2(p0 + j + 16, p1 + j);
            }

            if (!leftORright)
            {
                if (topORbottom)
                {
                    p = (top ? p1 : p0 + 16);
                    strPost2(p - 32, p);
                    p = NULL;
                }
                else{
                    strPost2x2(p0 - 16, p0 + 16, p1 - 32, p1);
                }
            }
        }

        /*======================================== */
        /* first level inverse transform (420_UV) */
        if(tScale >= 4) /* bypass first level transform for 4:1 and smaller thumbnail */
            continue;

        if (!top)
        {
            for (j = (left ? 16 : -16); j < (right ? 16 : 48); j += 32)
            {
                strIDCT4x4Stage1(p0 + j);
            }
        }

        if (!bottom)
        {
            for (j = (left ? 0 : -32); j < (right ? 0 : 32); j += 32)
            {
                strIDCT4x4Stage1(p1 + j);
            }
        }

        /*======================================== */
        /* first level inverse overlap (420_UV) */
        if (OL_NONE != olOverlap)
        {
            if(!left && !top)
            {
                if (bottom)
                {
                    for (j = -48; j < (right ? -16 : 16); j += 32)
                    {
                        p = p0 + j;
                        strPost4(p + 15, p + 14, p + 42, p + 43);
                        strPost4(p + 13, p + 12, p + 40, p + 41);
                        p = NULL;
                    }
                }
                else
                {
                    for (j = -48; j < (right ? -16 : 16); j += 32)
                    {
                        strPost4x4Stage1Split(p0 + j, p1 - 16 + j, 32, iHPQP, bHPAbsent);
                    }
                }

                if (right)
                {
                    if (!bottom)
                    {
                        strPost4(p0 - 2 , p0 - 4 , p1 - 28, p1 - 26);
                        strPost4(p0 - 1 , p0 - 3 , p1 - 27, p1 - 25);
                    }

                    strPost4(p0 - 18, p0 - 20, p0 - 12, p0 - 10);
                    strPost4(p0 - 17, p0 - 19, p0 - 11, p0 -  9);
                }
                else
                {
                    strPost4x4Stage1(p0 - 32, 32, iHPQP, bHPAbsent);
                }

                strPost4x4Stage1(p0 - 64, 32, iHPQP, bHPAbsent);
            }
            else if (top)
            {
                for (j = (left ? 0: -64); j < (right ? -32: 0); j += 32)
                {
                    p = p1 + j + 4;
                    strPost4(p + 1, p + 0, p + 28, p + 29);
                    strPost4(p + 3, p + 2, p + 30, p + 31);
                    p = NULL;
                }
            }
            else if (left)
            {
                if (!bottom)
                {
                    strPost4(p0 + 26, p0 + 24, p1 + 0, p1 + 2);
                    strPost4(p0 + 27, p0 + 25, p1 + 1, p1 + 3);
                }

                strPost4(p0 + 10, p0 + 8, p0 + 16, p0 + 18);
                strPost4(p0 + 11, p0 + 9, p0 + 17, p0 + 19);
            }
        }
    }

    /*================================================================ */
    /* 422_UV */
    for (i = 0; i < (YUV_422 == cfColorFormat? 2U : 0U) && tScale < 16; ++i)
    {
        PixelI* const p0 = pSC->p0MBbuffer[1 + i];/*(0 == i ? pSC->pU0 : pSC->pV0); */
        PixelI* const p1 = pSC->p1MBbuffer[1 + i];/*(0 == i ? pSC->pU1 : pSC->pV1); */

        Int iHPQP = 255;
        if (!bHPAbsent)
            iHPQP = pSC->pTile[pSC->cTileColumn].pQuantizerHP[i][pSC->MBInfo.iQIndexHP].iQP;

        /*======================================== */
        /* second level inverse transform (422_UV) */
        if ((!bottomORright) && pSC->m_Dparam->cThumbnailScale < 16)
        {
            /* 1D lossless HT */
            p1[0]  -= ((p1[32] + 1) >> 1);
            p1[32] += p1[0];

            if (!pSC->m_param.bScaledArith) {
                strDCT2x2dn(p1 +  0, p1 + 64, p1 + 16, p1 +  80);
                strDCT2x2dn(p1 + 32, p1 + 96, p1 + 48, p1 + 112);
            }
            else {
                strDCT2x2dnDec(p1 +  0, p1 + 64, p1 + 16, p1 +  80);
                strDCT2x2dnDec(p1 + 32, p1 + 96, p1 + 48, p1 + 112);
            }
        }
        
        /*======================================== */
        /* second level inverse overlap (422_UV) */
        if (OL_TWO == olOverlap)
        {
            if (!bottom)
            {
                if (leftORright)
                {
                    if (!top)
                    {
                        j = (left ? 0 : -64);
                        strPost2(p0 + 48 + j, p1 + j);
                    }

                    j = (left ? 16 : -48);
                    strPost2(p1 + j, p1 + j + 16);
                }
                else
                {
                    if (top)
                    {
                        strPost2(p1 - 64, p1);
                    }
                    else
                    {
                        strPost2x2(p0 - 16, p0 + 48, p1 - 64, p1);
                    }

                    strPost2x2(p1 - 48, p1 + 16, p1 - 32, p1 + 32);
                }
            }
            else if (!leftORright)
            {
                strPost2(p0 - 16, p0 + 48);
            }
        }

        /*======================================== */
        /* first level inverse transform (422_UV) */
        if(tScale >= 4) /* bypass first level transform for 4:1 and smaller thumbnail */
            continue;

        if (!top)
        {
            for (j = (left ? 48 : -16); j < (right ? 48 : 112); j += 64)
            {
                strIDCT4x4Stage1(p0 + j);
            }
        }

        if (!bottom)
        {
            for (j = (left ? 0 : -64); j < (right ? 0 : 64); j += 64)
            {
                strIDCT4x4Stage1(p1 + j + 0);
                strIDCT4x4Stage1(p1 + j + 16);
                strIDCT4x4Stage1(p1 + j + 32);
            }
        }
        
        /*======================================== */
        /* first level inverse overlap (422_UV) */
        if (OL_NONE != olOverlap)
        {
            if (!top)
            {
                if (leftORright)
                {
                    j = (left ? 32 + 10 : -32 + 14);

                    p = p0 + j;
                    strPost4(p + 0, p - 2, p + 6, p + 8);
                    strPost4(p + 1, p - 1, p + 7, p + 9);

                    p = NULL;
                }

                for (j = (left ? 0 : -128); j < (right ? -64 : 0); j += 64)
                {
                    strPost4x4Stage1(p0 + j + 32, 0, iHPQP, bHPAbsent);
                }
            }

            if (!bottom)
            {
                if (leftORright)
                {
                    j = (left ? 0 + 10 : -64 + 14);

                    p = p1 + j;
                    strPost4(p + 0, p - 2, p + 6, p + 8);
                    strPost4(p + 1, p - 1, p + 7, p + 9);

                    p += 16;
                    strPost4(p + 0, p - 2, p + 6, p + 8);
                    strPost4(p + 1, p - 1, p + 7, p + 9);

                    p = NULL;
                }

                for (j = (left ? 0 : -128); j < (right ? -64 : 0); j += 64)
                {
                    strPost4x4Stage1(p1 + j +  0, 0, iHPQP, bHPAbsent);
                    strPost4x4Stage1(p1 + j + 16, 0, iHPQP, bHPAbsent);
                }
            }

            if (topORbottom)
            {
                p = (top ? p1 + 5 : p0 + 48 + 13);
                for (j = (left ? 0 : -128); j < (right ? -64 : 0); j += 64)
                {
                    strPost4(p + j + 0, p + j - 1, p + j + 59, p + j + 60);
                    strPost4(p + j + 2, p + j + 1, p + j + 61, p + j + 62);
                }
                p = NULL;
            }
            else
            {
                if (leftORright)
                {
                    j = (left ? 0 + 0 : -64 + 4);
                    strPost4(p0 + j + 48 + 10 + 0, p0 + j + 48 + 10 - 2, p1 + j + 0, p1 + j + 2);
                    strPost4(p0 + j + 48 + 10 + 1, p0 + j + 48 + 10 - 1, p1 + j + 1, p1 + j + 3);
                }

                for (j = (left ? 0 : -128); j < (right ? -64 : 0); j += 64)
                {
                    strPost4x4Stage1Split(p0 + j + 48, p1 + j + 0, 0, iHPQP, bHPAbsent);
                }
            }
        }
    }    

    return ICERR_OK;
}

Int  invTransformMacroblock_alteredOperators_hard(CWMImageStrCodec * pSC)
{
    const OVERLAP olOverlap = pSC->WMISCP.olOverlap;
    const COLORFORMAT cfColorFormat = pSC->m_param.cfColorFormat;
    /* const BITDEPTH_BITS bdBitDepth = pSC->WMII.bdBitDepth; */
    const Bool left = (pSC->cColumn == 0), right = (pSC->cColumn == pSC->cmbWidth);
    const Bool top = (pSC->cRow == 0), bottom = (pSC->cRow == pSC->cmbHeight);
    const Bool topORbottom = (top || bottom), leftORright = (left || right);
    const Bool topORleft = (top || left), bottomORright = (bottom || right);
    Bool leftAdjacentColumn = (pSC->cColumn == 1), rightAdjacentColumn = (pSC->cColumn == pSC->cmbWidth - 1);
    /* Bool topAdjacentRow =  (pSC->cRow == 1), bottomAdjacentRow = (pSC->cRow == pSC->cmbHeight - 1); */
    const size_t mbWidth = pSC->cmbWidth;
    PixelI * p = NULL;/* * pt = NULL; */
    size_t i;
    const size_t iChannels = (cfColorFormat == YUV_420 || cfColorFormat == YUV_422) ? 1 : pSC->m_param.cNumChannels;
    const size_t tScale = pSC->m_Dparam->cThumbnailScale;
    Int j = 0;

    Int qp[MAX_CHANNELS], dcqp[MAX_CHANNELS], iStrength = (1 << pSC->WMII.cPostProcStrength);
    /* ERR_CODE result = ICERR_OK; */

#define mbX               pSC->mbX
#define mbY               pSC->mbY
#define tileX             pSC->tileX
#define tileY             pSC->tileY
#define bVertTileBoundary pSC->bVertTileBoundary
#define bHoriTileBoundary pSC->bHoriTileBoundary
#define bOneMBLeftVertTB  pSC->bOneMBLeftVertTB
#define bOneMBRightVertTB pSC->bOneMBRightVertTB
#define iPredBefore       pSC->iPredBefore
#define iPredAfter        pSC->iPredAfter

    if (pSC->WMISCP.bUseHardTileBoundaries) {
        /*Add tile location information */
        if (pSC->cColumn == 0) {
            bVertTileBoundary = FALSE;
            tileY = 0;
        }
        bOneMBLeftVertTB = bOneMBRightVertTB = FALSE;
        if(tileY > 0 && tileY <= pSC->WMISCP.cNumOfSliceMinus1H && (pSC->cColumn - 1) == pSC->WMISCP.uiTileY[tileY]) 
            bOneMBRightVertTB = TRUE;
        if(tileY < pSC->WMISCP.cNumOfSliceMinus1H && pSC->cColumn == pSC->WMISCP.uiTileY[tileY + 1]) {
            bVertTileBoundary = TRUE;
            tileY++; 
        }
        else 
            bVertTileBoundary = FALSE;
        if(tileY < pSC->WMISCP.cNumOfSliceMinus1H && (pSC->cColumn + 1) == pSC->WMISCP.uiTileY[tileY + 1]) 
            bOneMBLeftVertTB = TRUE;

        if (pSC->cRow == 0) {
            bHoriTileBoundary = FALSE;
            tileX = 0;
        }
        else if(mbY != pSC->cRow && tileX < pSC->WMISCP.cNumOfSliceMinus1V && pSC->cRow == pSC->WMISCP.uiTileX[tileX + 1]) {
            bHoriTileBoundary = TRUE;
            tileX++; 
        }
        else if(mbY != pSC->cRow)
            bHoriTileBoundary = FALSE;
    }
    else {
        bVertTileBoundary = FALSE;
        bHoriTileBoundary = FALSE;
        bOneMBLeftVertTB = FALSE;
        bOneMBRightVertTB = FALSE;
    }
    mbX = pSC->cColumn, mbY = pSC->cRow;

    if(pSC->WMII.cPostProcStrength > 0){
        /* threshold for post processing */
        for(i = 0; i < iChannels; i ++){
            qp[i] = pSC->pTile[pSC->cTileColumn].pQuantizerLP[i][pSC->MBInfo.iQIndexLP].iQP * iStrength * (olOverlap == OL_NONE ? 2 : 1);
            dcqp[i] = pSC->pTile[pSC->cTileColumn].pQuantizerDC[i][0].iQP * iStrength;
        }

        if(left) /* a new MB row */
            slideOneMBRow(pSC->pPostProcInfo, pSC->m_param.cNumChannels, mbWidth, top, bottom);  /* previous current row becomes previous row */
    }

    /*================================================================ */
    /* 400_Y, 444_YUV */
    for (i = 0; i < iChannels && tScale < 16; ++i)
    {
        PixelI* const p0 = pSC->p0MBbuffer[i];
        PixelI* const p1 = pSC->p1MBbuffer[i];

        /*================================ */
        /* second level inverse transform */
        if (!bottomORright)
        {
            if(pSC->WMII.cPostProcStrength > 0)
                updatePostProcInfo(pSC->pPostProcInfo, p1, mbX, i); /* update postproc info before IDCT */

            strIDCT4x4Stage2(p1);
            if (pSC->m_param.bScaledArith) {
                strNormalizeDec(p1, (i != 0));
            }
        }

        /*================================ */
        /* second level inverse overlap */
        if (OL_TWO == olOverlap)
        {
            /* Corner operations */
            if ((top || bHoriTileBoundary) && (left || bVertTileBoundary))
                strPost4_alternate(p1 + 0, p1 + 64, p1 + 0 + 16, p1 + 64 + 16);
            if ((top || bHoriTileBoundary) && (right || bVertTileBoundary))
                strPost4_alternate(p1 - 128, p1 - 64, p1 - 128 + 16, p1 - 64 + 16); 
            if ((bottom || bHoriTileBoundary) && (left || bVertTileBoundary))
                strPost4_alternate(p0 + 32, p0 + 96, p0 + 32 + 16, p0 + 96 + 16);
            if ((bottom || bHoriTileBoundary) && (right || bVertTileBoundary))
                strPost4_alternate(p0 - 96, p0 - 32, p0 - 96 + 16, p0 - 32 + 16);
            if ((leftORright || bVertTileBoundary) && (!topORbottom  && !bHoriTileBoundary))
            {
                if (left || bVertTileBoundary) {
                    j = 0;
                    strPost4_alternate(p0 + j + 32, p0 + j +  48, p1 + j +  0, p1 + j + 16);
                    strPost4_alternate(p0 + j + 96, p0 + j + 112, p1 + j + 64, p1 + j + 80);
                }
                if (right || bVertTileBoundary) {
                    j = -128;
                    strPost4_alternate(p0 + j + 32, p0 + j +  48, p1 + j +  0, p1 + j + 16);
                    strPost4_alternate(p0 + j + 96, p0 + j + 112, p1 + j + 64, p1 + j + 80);
                }
            }

            if (!leftORright)
            {
                if ((topORbottom || bHoriTileBoundary) && !bVertTileBoundary)
                {
                    if (top || bHoriTileBoundary) {
                        p = p1;
                        strPost4_alternate(p - 128, p - 64, p +  0, p + 64);
                        strPost4_alternate(p - 112, p - 48, p + 16, p + 80);
                        p = NULL;
                    }
                    if (bottom || bHoriTileBoundary) {
                        p = p0 + 32;
                        strPost4_alternate(p - 128, p - 64, p +  0, p + 64);
                        strPost4_alternate(p - 112, p - 48, p + 16, p + 80);
                        p = NULL;
                    }
                }
                
                if (!topORbottom && !bHoriTileBoundary && !bVertTileBoundary)
                    strPost4x4Stage2Split_alternate(p0, p1);
            }
        }

        if(pSC->WMII.cPostProcStrength > 0)
            postProcMB(pSC->pPostProcInfo, p0, p1, mbX, i, dcqp[i]); /* second stage deblocking */

        /*================================ */
        /* first level inverse transform */
        if(tScale >= 4) /* bypass first level transform for 4:1 and smaller thumbnail */
            continue;

        if (!top)
        {
            for (j = (left ? 32 : -96); j < (right ? 32 : 160); j += 64)
            {
                strIDCT4x4Stage1(p0 + j +  0);
                strIDCT4x4Stage1(p0 + j + 16);
            }
        }

        if (!bottom)
        {
            for (j = (left ? 0 : -128); j < (right ? 0 : 128); j += 64)
            {
/*                if(tScale == 2  && bdBitDepth != BD_1){ */
/*                    MIPgen(p1 + j + 0); */
/*                    MIPgen(p1 + j + 16); */
/*                } */
                strIDCT4x4Stage1(p1 + j +  0);
                strIDCT4x4Stage1(p1 + j + 16);
            }
        }

        /*================================ */
        /* first level inverse overlap */
        if (OL_NONE != olOverlap)
        {
            if (leftORright || bVertTileBoundary)
            {
                /* Corner operations */
                if ((top || bHoriTileBoundary) && (left || bVertTileBoundary))
                    strPost4_alternate(p1 + 0, p1 + 1, p1 + 2, p1 + 3);
                if ((top || bHoriTileBoundary) && (right || bVertTileBoundary))
                    strPost4_alternate(p1 - 59, p1 - 60, p1 - 57, p1 - 58); 
                if ((bottom || bHoriTileBoundary) && (left || bVertTileBoundary))
                    strPost4_alternate(p0 + 48 + 10, p0 + 48 + 11, p0 + 48 + 8, p0 + 48 + 9);
                if ((bottom || bHoriTileBoundary) && (right || bVertTileBoundary))
                    strPost4_alternate(p0 - 1, p0 - 2, p0 - 3, p0 - 4);
                if (left || bVertTileBoundary) {
                    j = 0 + 10;
                    if (!top)
                    {
                        p = p0 + 16 + j;
                        strPost4_alternate(p +  0, p -  2, p +  6, p +  8);
                        strPost4_alternate(p +  1, p -  1, p +  7, p +  9);
                        strPost4_alternate(p + 16, p + 14, p + 22, p + 24);
                        strPost4_alternate(p + 17, p + 15, p + 23, p + 25);
                        p = NULL;
                    }
                    if (!bottom)
                    {
                        p = p1 + j;
                        strPost4_alternate(p + 0, p - 2, p + 6, p + 8);
                        strPost4_alternate(p + 1, p - 1, p + 7, p + 9);
                        p = NULL;
                    }
                    if (!topORbottom && !bHoriTileBoundary)
                    {
                        strPost4_alternate(p0 + 48 + j + 0, p0 + 48 + j - 2, p1 - 10 + j, p1 - 8 + j);
                        strPost4_alternate(p0 + 48 + j + 1, p0 + 48 + j - 1, p1 -  9 + j, p1 - 7 + j);
                    }
                }
                if (right || bVertTileBoundary) {
                    j = -64 + 14;
                    if (!top)
                    {
                        p = p0 + 16 + j;
                        strPost4_alternate(p +  0, p -  2, p +  6, p +  8);
                        strPost4_alternate(p +  1, p -  1, p +  7, p +  9);
                        strPost4_alternate(p + 16, p + 14, p + 22, p + 24);
                        strPost4_alternate(p + 17, p + 15, p + 23, p + 25);
                        p = NULL;
                    }
                    if (!bottom)
                    {
                        p = p1 + j;
                        strPost4_alternate(p + 0, p - 2, p + 6, p + 8);
                        strPost4_alternate(p + 1, p - 1, p + 7, p + 9);
                        p = NULL;
                    }
                    if (!topORbottom && !bHoriTileBoundary)
                    {
                        strPost4_alternate(p0 + 48 + j + 0, p0 + 48 + j - 2, p1 - 10 + j, p1 - 8 + j);
                        strPost4_alternate(p0 + 48 + j + 1, p0 + 48 + j - 1, p1 -  9 + j, p1 - 7 + j);
                    }
                }
            }

            if (top || bHoriTileBoundary)
            {
                for (j = (left ? 0 : -192); j < (right ? -64 : 64); j += 64)
                {
                    if (!bVertTileBoundary || j != -64) {
                        p = p1 + j;
                        strPost4_alternate(p + 5, p + 4, p + 64, p + 65);
                        strPost4_alternate(p + 7, p + 6, p + 66, p + 67);
                        p = NULL;

                        strPost4x4Stage1_alternate(p1 + j, 0);
                    }
                }
            }

            if (bottom || bHoriTileBoundary)
            {
                for (j = (left ? 0 : -192); j < (right ? -64 : 64); j += 64)
                {
                    if (!bVertTileBoundary || j != -64) {
                        strPost4x4Stage1_alternate(p0 + 16 + j, 0);
                        strPost4x4Stage1_alternate(p0 + 32 + j, 0);

                        p = p0 + 48 + j;
                        strPost4_alternate(p + 15, p + 14, p + 74, p + 75);
                        strPost4_alternate(p + 13, p + 12, p + 72, p + 73);
                        p = NULL;
                    }
                }
            }

            if (!top && !bottom && !bHoriTileBoundary)
            {
                for (j = (left ? 0 : -192); j < (right ? -64 : 64); j += 64)
                {
                    if (!bVertTileBoundary || j != -64) {
                        strPost4x4Stage1_alternate(p0 + 16 + j, 0);
                        strPost4x4Stage1_alternate(p0 + 32 + j, 0);
                        strPost4x4Stage1Split_alternate(p0 + 48 + j, p1 + j, 0);
                        strPost4x4Stage1_alternate(p1 + j, 0);
                    }
                }
            }
        }
        
        if(pSC->WMII.cPostProcStrength > 0 && (!topORleft))
            postProcBlock(pSC->pPostProcInfo, p0, p1, mbX, i, qp[i]); /* destairing and first stage deblocking */
    }

    /*================================================================ */
    /* 420_UV */
    for (i = 0; i < (YUV_420 == cfColorFormat? 2U : 0U) && tScale < 16; ++i)
    {
        PixelI* const p0 = pSC->p0MBbuffer[1 + i];/*(0 == i ? pSC->pU0 : pSC->pV0); */
        PixelI* const p1 = pSC->p1MBbuffer[1 + i];/*(0 == i ? pSC->pU1 : pSC->pV1); */

        /*======================================== */
        /* second level inverse transform (420_UV) */
        if (!bottomORright)
        {
            if (!pSC->m_param.bScaledArith) {
                strDCT2x2dn(p1, p1 + 32, p1 + 16, p1 + 48);
            }
            else {
                strDCT2x2dnDec(p1, p1 + 32, p1 + 16, p1 + 48);
            }
        }
        
        /*======================================== */
        /* second level inverse overlap (420_UV) */
        if (OL_TWO == olOverlap)
        {
            if ((leftAdjacentColumn || bOneMBRightVertTB) && (top || bHoriTileBoundary)) 
                COMPUTE_CORNER_PRED_DIFF(p1 - 64 + 0, *(p1 - 64 + 32));
            if ((rightAdjacentColumn || bOneMBLeftVertTB) && (top || bHoriTileBoundary))
                iPredBefore[i][0] = *(p1 + 0);
            if ((right || bVertTileBoundary) && (top || bHoriTileBoundary))
                COMPUTE_CORNER_PRED_DIFF(p1 - 64 + 32, iPredBefore[i][0]);
            if ((leftAdjacentColumn || bOneMBRightVertTB) && (bottom || bHoriTileBoundary)) 
                COMPUTE_CORNER_PRED_DIFF(p0 - 64 + 16, *(p0 - 64 + 48));
            if ((rightAdjacentColumn || bOneMBLeftVertTB) && (bottom || bHoriTileBoundary)) 
                iPredBefore[i][1] = *(p0 + 16);
            if ((right || bVertTileBoundary) && (bottom || bHoriTileBoundary))
                COMPUTE_CORNER_PRED_DIFF(p0 - 64 + 48, iPredBefore[i][1]);

            if ((leftORright || bVertTileBoundary) && !topORbottom && !bHoriTileBoundary)
            {
                if (left || bVertTileBoundary)
                    strPost2_alternate(p0 +   0 + 16, p1 +   0);
                if (right || bVertTileBoundary)
                    strPost2_alternate(p0 + -32 + 16, p1 + -32);
            }

            if (!leftORright)
            {
                if ((topORbottom || bHoriTileBoundary) && !bVertTileBoundary)
                {
                    if (top || bHoriTileBoundary) 
                        strPost2_alternate(p1 - 32, p1);
                    if (bottom || bHoriTileBoundary) 
                        strPost2_alternate(p0 + 16 - 32, p0 + 16);
                }
                else if (!topORbottom && !bHoriTileBoundary && !bVertTileBoundary) { 
                    strPost2x2_alternate(p0 - 16, p0 + 16, p1 - 32, p1);
                }
            }
            if ((leftAdjacentColumn || bOneMBRightVertTB) && (top || bHoriTileBoundary)) 
                COMPUTE_CORNER_PRED_ADD(p1 - 64 + 0, *(p1 - 64 + 32));
            if ((rightAdjacentColumn || bOneMBLeftVertTB) && (top || bHoriTileBoundary))
                iPredAfter[i][0] = *(p1 + 0);
            if ((right || bVertTileBoundary) && (top || bHoriTileBoundary))
                COMPUTE_CORNER_PRED_ADD(p1 - 64 + 32, iPredAfter[i][0]);
            if ((leftAdjacentColumn || bOneMBRightVertTB) && (bottom || bHoriTileBoundary)) 
                COMPUTE_CORNER_PRED_ADD(p0 - 64 + 16, *(p0 - 64 + 48));
            if ((rightAdjacentColumn || bOneMBLeftVertTB) && (bottom || bHoriTileBoundary)) 
                iPredAfter[i][1] = *(p0 + 16);
            if ((right || bVertTileBoundary) && (bottom || bHoriTileBoundary))
                COMPUTE_CORNER_PRED_ADD(p0 - 64 + 48, iPredAfter[i][1]);
        }

        /*======================================== */
        /* first level inverse transform (420_UV) */
        if(tScale >= 4) /* bypass first level transform for 4:1 and smaller thumbnail */
            continue;

        if (!top)
        {
            /* In order to allow correction operation of corner chroma overlap operators (fixed) */
            /* processing of left most MB column must be delayed by one MB */
            /* Thus left MB not processed until leftAdjacentColumn = 1 */
            for (j = ((left) ? 48 : ((leftAdjacentColumn || bOneMBRightVertTB) ? -48 : -16)); j < ((right || bVertTileBoundary) ? 16 : 48); j += 32)
            {
                strIDCT4x4Stage1(p0 + j);
            }
        }

        if (!bottom)
        {
            /* In order to allow correction operation of corner chroma overlap operators (fixed) */
            /* processing of left most MB column must be delayed by one MB */
            /* Thus left MB not processed until leftAdjacentColumn = 1 */
            for (j = ((left) ? 32 : ((leftAdjacentColumn || bOneMBRightVertTB) ? -64 : -32)); j < ((right || bVertTileBoundary) ? 0 : 32); j += 32)
            {
                strIDCT4x4Stage1(p1 + j);
            }
        }

        /*======================================== */
        /* first level inverse overlap (420_UV) */
        if (OL_NONE != olOverlap)
        {
            /* Corner operations */
            /* Change because the top-left corner ICT will not have happened until leftAdjacentColumn ==1 */
            if ((top || bHoriTileBoundary) && (leftAdjacentColumn || bOneMBRightVertTB)) 
                strPost4_alternate(p1 - 64 + 0, p1 - 64 + 1, p1 - 64 + 2, p1 - 64 + 3);
            if ((top || bHoriTileBoundary) && (right || bVertTileBoundary)) 
                strPost4_alternate(p1 - 27, p1 - 28, p1 - 25, p1 - 26);
            /* Change because the bottom-left corner ICT will not have happened until leftAdjacentColumn ==1 */
            if ((bottom || bHoriTileBoundary) && (leftAdjacentColumn || bOneMBRightVertTB)) 
                strPost4_alternate(p0 - 64 + 16 + 10, p0 - 64 + 16 + 11, p0 - 64 + 16 + 8, p0 - 64 + 16 + 9);
            if ((bottom || bHoriTileBoundary) && (right || bVertTileBoundary)) 
                strPost4_alternate(p0 - 1, p0 - 2, p0 - 3, p0 - 4);
            if(!left && !top)
            {
                /* Change because the vertical 1-D overlap operations of the left edge pixels cannot be performed until leftAdjacentColumn ==1 */
                if (leftAdjacentColumn || bOneMBRightVertTB)
                {
                    if (!bottom && !bHoriTileBoundary)
                    {
                        strPost4_alternate(p0 - 64 + 26, p0 - 64 + 24, p1 - 64 + 0, p1 - 64 + 2);
                        strPost4_alternate(p0 - 64 + 27, p0 - 64 + 25, p1 - 64 + 1, p1 - 64 + 3);
                    }

                    strPost4_alternate(p0 - 64 + 10, p0 - 64 + 8, p0 - 64 + 16, p0 - 64 + 18);
                    strPost4_alternate(p0 - 64 + 11, p0 - 64 + 9, p0 - 64 + 17, p0 - 64 + 19);
                }
                if (bottom || bHoriTileBoundary)
                {
                    p = p0 + -48;
                    strPost4_alternate(p + 15, p + 14, p + 42, p + 43);
                    strPost4_alternate(p + 13, p + 12, p + 40, p + 41);
                    p = NULL;

                    if (!right && !bVertTileBoundary)
                    {
                        p = p0 + -16;
                        strPost4_alternate(p + 15, p + 14, p + 42, p + 43);
                        strPost4_alternate(p + 13, p + 12, p + 40, p + 41);
                        p = NULL;
                    }
                }
                else
                {
                    strPost4x4Stage1Split_alternate(p0 + -48, p1 - 16 + -48, 32);

                    if (!right && !bVertTileBoundary)
                        strPost4x4Stage1Split_alternate(p0 + -16, p1 - 16 + -16, 32);
                }

                if (right || bVertTileBoundary)
                {
                    if (!bottom && !bHoriTileBoundary)
                    {
                        strPost4_alternate(p0 - 2 , p0 - 4 , p1 - 28, p1 - 26);
                        strPost4_alternate(p0 - 1 , p0 - 3 , p1 - 27, p1 - 25);
                    }

                    strPost4_alternate(p0 - 18, p0 - 20, p0 - 12, p0 - 10);
                    strPost4_alternate(p0 - 17, p0 - 19, p0 - 11, p0 -  9);
                }
                else
                {
                    strPost4x4Stage1_alternate(p0 - 32, 32);
                }

                strPost4x4Stage1_alternate(p0 - 64, 32);
            }

            if (top || bHoriTileBoundary)
            {
                if (!left)
                {
                    p = p1 + -64 + 4;
                    strPost4_alternate(p + 1, p + 0, p + 28, p + 29);
                    strPost4_alternate(p + 3, p + 2, p + 30, p + 31);
                    p = NULL;
                }

                if (!left && !right && !bVertTileBoundary)
                {
                    p = p1 + -32 + 4;
                    strPost4_alternate(p + 1, p + 0, p + 28, p + 29);
                    strPost4_alternate(p + 3, p + 2, p + 30, p + 31);
                    p = NULL;
                }
            }
        }
    }

    /*================================================================ */
    /* 422_UV */
    for (i = 0; i < (YUV_422 == cfColorFormat? 2U : 0U) && tScale < 16; ++i)
    {
        PixelI* const p0 = pSC->p0MBbuffer[1 + i];/*(0 == i ? pSC->pU0 : pSC->pV0); */
        PixelI* const p1 = pSC->p1MBbuffer[1 + i];/*(0 == i ? pSC->pU1 : pSC->pV1); */

        /*======================================== */
        /* second level inverse transform (422_UV) */
        if ((!bottomORright) && pSC->m_Dparam->cThumbnailScale < 16)
        {
            /* 1D lossless HT */
            p1[0]  -= ((p1[32] + 1) >> 1);
            p1[32] += p1[0];

            if (!pSC->m_param.bScaledArith) {
                strDCT2x2dn(p1 +  0, p1 + 64, p1 + 16, p1 +  80);
                strDCT2x2dn(p1 + 32, p1 + 96, p1 + 48, p1 + 112);
            }
            else {
                strDCT2x2dnDec(p1 +  0, p1 + 64, p1 + 16, p1 +  80);
                strDCT2x2dnDec(p1 + 32, p1 + 96, p1 + 48, p1 + 112);
            }
        }
        
        /*======================================== */
        /* second level inverse overlap (422_UV) */
        if (OL_TWO == olOverlap)
        {
            if ((leftAdjacentColumn || bOneMBRightVertTB) && (top || bHoriTileBoundary)) 
                COMPUTE_CORNER_PRED_DIFF(p1 - 128 + 0, *(p1 - 128 + 64));

            if ((rightAdjacentColumn || bOneMBLeftVertTB) && (top || bHoriTileBoundary))
                iPredBefore[i][0] = *(p1 + 0);
            if ((right || bVertTileBoundary) && (top || bHoriTileBoundary))
                COMPUTE_CORNER_PRED_DIFF(p1 - 128 + 64, iPredBefore[i][0]);

            if ((leftAdjacentColumn || bOneMBRightVertTB) && (bottom || bHoriTileBoundary)) 
                COMPUTE_CORNER_PRED_DIFF(p0 - 128 + 48, *(p0 - 128 + 112));

            if ((rightAdjacentColumn || bOneMBLeftVertTB) && (bottom || bHoriTileBoundary)) 
                iPredBefore[i][1] = *(p0 + 48);
            if ((right || bVertTileBoundary) && (bottom || bHoriTileBoundary))
                COMPUTE_CORNER_PRED_DIFF(p0 - 128 + 112, iPredBefore[i][1]);

            if (!bottom)
            {
                if (leftORright || bVertTileBoundary)
                {
                    if (!top && !bHoriTileBoundary)
                    {
                        if (left || bVertTileBoundary)
                            strPost2_alternate(p0 + 48 + 0, p1 + 0);

                        if (right || bVertTileBoundary)
                            strPost2_alternate(p0 + 48 + -64, p1 + -64);
                    }

                    if (left || bVertTileBoundary)
                        strPost2_alternate(p1 + 16, p1 + 16 + 16);

                    if (right || bVertTileBoundary)
                        strPost2_alternate(p1 + -48, p1 + -48 + 16);
                }

                if (!leftORright && !bVertTileBoundary)
                {
                    if (top || bHoriTileBoundary)
                        strPost2_alternate(p1 - 64, p1);
                    else
                        strPost2x2_alternate(p0 - 16, p0 + 48, p1 - 64, p1);

                    strPost2x2_alternate(p1 - 48, p1 + 16, p1 - 32, p1 + 32);
                }
            }
            
            if ((bottom || bHoriTileBoundary) && (!leftORright && !bVertTileBoundary))
                strPost2_alternate(p0 - 16, p0 + 48);

            if ((leftAdjacentColumn || bOneMBRightVertTB) && (top || bHoriTileBoundary)) 
                COMPUTE_CORNER_PRED_ADD(p1 - 128 + 0, *(p1 - 128 + 64));

            if ((rightAdjacentColumn || bOneMBLeftVertTB) && (top || bHoriTileBoundary))
                iPredAfter[i][0] = *(p1 + 0);
            if ((right || bVertTileBoundary) && (top || bHoriTileBoundary))
                COMPUTE_CORNER_PRED_ADD(p1 - 128 + 64, iPredAfter[i][0]);

            if ((leftAdjacentColumn || bOneMBRightVertTB) && (bottom || bHoriTileBoundary)) 
                COMPUTE_CORNER_PRED_ADD(p0 - 128 + 48, *(p0 - 128 + 112));

            if ((rightAdjacentColumn || bOneMBLeftVertTB) && (bottom || bHoriTileBoundary)) 
                iPredAfter[i][1] = *(p0 + 48);
            if ((right || bVertTileBoundary) && (bottom || bHoriTileBoundary))
                COMPUTE_CORNER_PRED_ADD(p0 - 128 + 112, iPredAfter[i][1]);
        }

        /*======================================== */
        /* first level inverse transform (422_UV) */
        if(tScale >= 4) /* bypass first level transform for 4:1 and smaller thumbnail */
            continue;

        if (!top)
        {
            /* Need to delay processing of left column until leftAdjacentColumn = 1 for corner overlap operators */
            /* Since 422 has no vertical downsampling, no top MB delay of processing is necessary */
            for (j = (left ? 112 : ((leftAdjacentColumn || bOneMBRightVertTB) ? -80 : -16)); j < ((right || bVertTileBoundary) ? 48 : 112); j += 64)
            {
                strIDCT4x4Stage1(p0 + j);
            }
        }

        if (!bottom)
        {
            /* Need to delay processing of left column until leftAdjacentColumn = 1 for corner overlap operators */
            /* Since 422 has no vertical downsampling, no top MB delay of processing is necessary */
            for (j = (left ? 64 : ((leftAdjacentColumn || bOneMBRightVertTB) ? -128 : -64)); j < ((right || bVertTileBoundary) ? 0 : 64); j += 64)
            {
                strIDCT4x4Stage1(p1 + j + 0);
                strIDCT4x4Stage1(p1 + j + 16);
                strIDCT4x4Stage1(p1 + j + 32);
            }
        }
        
        /*======================================== */
        /* first level inverse overlap (422_UV) */
        if (OL_NONE != olOverlap)
        {
            /* Corner operations */
            if ((top || bHoriTileBoundary) && (leftAdjacentColumn || bOneMBRightVertTB))
                strPost4_alternate(p1 - 128 + 0, p1 - 128 + 1, p1 - 128 + 2, p1 - 128 + 3);
            if ((top || bHoriTileBoundary) && (right || bVertTileBoundary))
                strPost4_alternate(p1 - 59, p1 - 60, p1 - 57, p1 - 58);
            if ((bottom || bHoriTileBoundary) && (leftAdjacentColumn || bOneMBRightVertTB))
                strPost4_alternate(p0 - 128 + 48 + 10, p0 - 128 + 48 + 11, p0 - 128 + 48 + 8, p0 - 128 + 48 + 9);
            if ((bottom || bHoriTileBoundary) && (right || bVertTileBoundary))
                strPost4_alternate(p0 - 1, p0 - 2, p0 - 3, p0 - 4);
            if (!top)
            {
                /* Need to delay processing of left column until leftAdjacentColumn = 1 for corner overlap operators */
                if (leftAdjacentColumn || bOneMBRightVertTB) {
                    p = p0 + 32 + 10 - 128;
                    strPost4_alternate(p + 0, p - 2, p + 6, p + 8);
                    strPost4_alternate(p + 1, p - 1, p + 7, p + 9);
                    p = NULL;
                }

                if (right || bVertTileBoundary) {
                    p = p0 + -32 + 14;
                    strPost4_alternate(p + 0, p - 2, p + 6, p + 8);
                    strPost4_alternate(p + 1, p - 1, p + 7, p + 9);
                    p = NULL;
                }

                for (j = (left ? 0 : -128); j < ((right || bVertTileBoundary) ? -64 : 0); j += 64)
                    strPost4x4Stage1_alternate(p0 + j + 32, 0);
            }

            if (!bottom)
            {
                /* Need to delay processing of left column until leftAdjacentColumn = 1 for corner overlap operators */
                if (leftAdjacentColumn || bOneMBRightVertTB)
                {
                    p = p1 + 0 + 10 - 128;
                    strPost4_alternate(p + 0, p - 2, p + 6, p + 8);
                    strPost4_alternate(p + 1, p - 1, p + 7, p + 9);
                    p += 16;
                    strPost4_alternate(p + 0, p - 2, p + 6, p + 8);
                    strPost4_alternate(p + 1, p - 1, p + 7, p + 9);
                    p = NULL;
                }

                if (right || bVertTileBoundary)
                {
                    p = p1 + -64 + 14;
                    strPost4_alternate(p + 0, p - 2, p + 6, p + 8);
                    strPost4_alternate(p + 1, p - 1, p + 7, p + 9);
                    p += 16;
                    strPost4_alternate(p + 0, p - 2, p + 6, p + 8);
                    strPost4_alternate(p + 1, p - 1, p + 7, p + 9);
                    p = NULL;
                }

                for (j = (left ? 0 : -128); j < ((right || bVertTileBoundary) ? -64 : 0); j += 64)
                {
                    strPost4x4Stage1_alternate(p1 + j +  0, 0);
                    strPost4x4Stage1_alternate(p1 + j + 16, 0);
                }
            }

            if (topORbottom || bHoriTileBoundary)
            {
                if (top || bHoriTileBoundary) {
                    p = p1 + 5;
                    for (j = (left ? 0 : -128); j < ((right || bVertTileBoundary) ? -64 : 0); j += 64)
                    {
                        strPost4_alternate(p + j + 0, p + j - 1, p + j + 59, p + j + 60);
                        strPost4_alternate(p + j + 2, p + j + 1, p + j + 61, p + j + 62);
                    }
                    p = NULL;
                }

                if (bottom || bHoriTileBoundary) {
                    p = p0 + 48 + 13;
                    for (j = (left ? 0 : -128); j < ((right || bVertTileBoundary) ? -64 : 0); j += 64)
                    {
                        strPost4_alternate(p + j + 0, p + j - 1, p + j + 59, p + j + 60);
                        strPost4_alternate(p + j + 2, p + j + 1, p + j + 61, p + j + 62);
                    }
                    p = NULL;
                }
            }
            else
            {
                /* Need to delay processing of left column until leftAdjacentColumn = 1 for corner overlap operators */
                if (leftAdjacentColumn || bOneMBRightVertTB)
                {
                    j = 0 + 0 - 128;
                    strPost4_alternate(p0 + j + 48 + 10 + 0, p0 + j + 48 + 10 - 2, p1 + j + 0, p1 + j + 2);
                    strPost4_alternate(p0 + j + 48 + 10 + 1, p0 + j + 48 + 10 - 1, p1 + j + 1, p1 + j + 3);
                }

                if (right || bVertTileBoundary)
                {
                    j = -64 + 4;
                    strPost4_alternate(p0 + j + 48 + 10 + 0, p0 + j + 48 + 10 - 2, p1 + j + 0, p1 + j + 2);
                    strPost4_alternate(p0 + j + 48 + 10 + 1, p0 + j + 48 + 10 - 1, p1 + j + 1, p1 + j + 3);
                }

                for (j = (left ? 0 : -128); j < ((right || bVertTileBoundary) ? -64 : 0); j += 64)
                    strPost4x4Stage1Split_alternate(p0 + j + 48, p1 + j + 0, 0);
            }
        }
    }    

    return ICERR_OK;
}

#undef IROTATE1
#undef IROTATE2
#undef max
#undef min
#undef mbX
#undef mbY
#undef tileX
#undef tileY
#undef bVertTileBoundary
#undef bHoriTileBoundary
#undef bOneMBLeftVertTB
#undef bOneMBRightVertTB
#undef iPredBefore
#undef iPredAfter
/* ---- strPredQuantDec.c ---- */
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

/* (merged) #include "strcodec.h" */

#define DEQUANT(iRaw, iQP) ((iRaw) * (iQP))

Void dequantizeBlock4x4(PixelI * pRec, Int * pOrg, const Int * pIndex, Int iQPLP)
{
    Int i;
    
    for(i = 1; i < 16; i ++)
        pRec[pIndex[i]] = DEQUANT(pOrg[i], iQPLP);
}

Void dequantizeBlock2x2(PixelI * pRec, Int * pOrg, Int iQPLP)
{
    pRec[32] = DEQUANT(pOrg[1], iQPLP);
    pRec[16] = DEQUANT(pOrg[2], iQPLP);
    pRec[48] = DEQUANT(pOrg[3], iQPLP);
}

Void dequantizeBlock4x2(PixelI * pRec, Int * pOrg, Int iQPLP)
{
    pRec[ 64] = DEQUANT(pOrg[1], iQPLP);
    pRec[ 16] = DEQUANT(pOrg[2], iQPLP);
    pRec[ 80] = DEQUANT(pOrg[3], iQPLP);
    pRec[ 32] = DEQUANT(pOrg[4], iQPLP);
    pRec[ 96] = DEQUANT(pOrg[5], iQPLP);
    pRec[ 48] = DEQUANT(pOrg[6], iQPLP);
    pRec[112] = DEQUANT(pOrg[7], iQPLP);
}


Int dequantizeMacroblock(CWMImageStrCodec * pSC)
{
    const COLORFORMAT cf = pSC->m_param.cfColorFormat;
    CWMIMBInfo *pMBInfo = &pSC->MBInfo;
    CWMITile * pTile = pSC->pTile + pSC->cTileColumn;
    const size_t iChannels = pSC->m_param.cNumChannels;
    size_t i;

    for(i = 0; i < iChannels; i ++){
        /*dequantize DC */
        pSC->p1MBbuffer[i][0] = DEQUANT(pMBInfo->iBlockDC[i][0], pTile->pQuantizerDC[i]->iQP);

        /* dequantize LP */
        if(pSC->WMISCP.sbSubband != SB_DC_ONLY) {
            if(i == 0 || (cf != YUV_422 && cf != YUV_420))
                dequantizeBlock4x4(pSC->p1MBbuffer[i] , pMBInfo->iBlockDC[i], dctIndex[2], pTile->pQuantizerLP[i][pMBInfo->iQIndexLP].iQP);
            else if(cf == YUV_422)
                dequantizeBlock4x2(pSC->p1MBbuffer[i], pMBInfo->iBlockDC[i], pTile->pQuantizerLP[i][pMBInfo->iQIndexLP].iQP);
            else /* 420 */
                dequantizeBlock2x2(pSC->p1MBbuffer[i], pMBInfo->iBlockDC[i], pTile->pQuantizerLP[i][pMBInfo->iQIndexLP].iQP);
        }
    }

    return ICERR_OK;
}

/* frequency domain inverse DCAC prediction */
Void predDCACDec(CWMImageStrCodec * pSC)
{
    const COLORFORMAT cf = pSC->m_param.cfColorFormat;
    const Int iChannels = (cf == YUV_420 || cf == YUV_422) ? 1 : (Int) pSC->m_param.cNumChannels;
    CWMIMBInfo *pMBInfo = &(pSC->MBInfo);
    size_t mbX = pSC->cColumn;/* mbY = pSC->cRow; */
    Int iDCACPredMode = getDCACPredMode(pSC, mbX);
    Int iDCPredMode = (iDCACPredMode & 0x3);
    Int iADPredMode = (iDCACPredMode & 0xC);
    PixelI * pOrg, * pRef;
    Int ii;

    for(ii = 0; ii < iChannels; ii ++){
        pOrg = pMBInfo->iBlockDC[ii];/*[dcBlkIdx + (i >> 4)]; // current DC block */

        /* DC prediction */
        if(iDCPredMode == 1){ /* predict DC from top */
            pOrg[0] += pSC->PredInfoPrevRow[ii][mbX].iDC;
        }
        else if(iDCPredMode == 0){ /* predict DC from left */
            pOrg[0] += (pSC->PredInfo[ii] + mbX - 1)->iDC;
        }
        else if(iDCPredMode == 2){/* predict DC from top&left */
            pOrg[0] += ((pSC->PredInfo[ii] + mbX - 1)->iDC + (pSC->PredInfoPrevRow[ii] + mbX)->iDC) >> 1;
        }

        /* AD prediction */
        if(iADPredMode == 4){/* predict AD from top */
            pRef = (pSC->PredInfoPrevRow[ii] + mbX)->piAD;
            pOrg[4] += pRef[3], pOrg[8] += pRef[4], pOrg[12] += pRef[5];
        }
        else if(iADPredMode == 0){/* predict AD from left */
            pRef = (pSC->PredInfo[ii] + mbX - 1)->piAD;
            pOrg[1] += pRef[0], pOrg[2] += pRef[1], pOrg[3] += pRef[2];
        }
    }

    if(cf == YUV_420){
        for(ii = 1; ii < 3; ii ++){
            pOrg = pMBInfo->iBlockDC[ii];/*dcBlkIdx + ii]; // current DC block */

            /* DC prediction */
            if(iDCPredMode == 1){ /* predict DC from top */
                pOrg[0] += (pSC->PredInfoPrevRow[ii] + mbX)->iDC;
            }
            else if(iDCPredMode == 0){ /* predict DC from left */
                pOrg[0] += (pSC->PredInfo[ii] + mbX - 1)->iDC;
            }
            else if(iDCPredMode == 2){ /* predict DC from top&left */
                pOrg[0] += (((pSC->PredInfo[ii] + mbX - 1)->iDC + (pSC->PredInfoPrevRow[ii] + mbX)->iDC + 1) >> 1);
            }

            /* AD prediciton */
            if(iADPredMode == 4){/* predict AD from top */
                pOrg[2] += (pSC->PredInfoPrevRow[ii] + mbX)->piAD[1];
            }
            else if(iADPredMode == 0){/* predict AD from left */
                pOrg[1] += (pSC->PredInfo[ii] + mbX - 1)->piAD[0];
            }
        }
    }
    else if(cf == YUV_422){
        for(ii = 1; ii < 3; ii ++){
            pOrg = pMBInfo->iBlockDC[ii];/*[dcBlkIdx + ii]; // current DC block */

            /* DC prediciton */
            if(iDCPredMode == 1){ /* predict DC from top */
                pOrg[0] += (pSC->PredInfoPrevRow[ii] + mbX)->iDC;
            }
            else if(iDCPredMode == 0){ /* predict DC from left */
                pOrg[0] += (pSC->PredInfo[ii] + mbX - 1)->iDC;
            }
            else if(iDCPredMode == 2){ /* predict DC from top&left */
                pOrg[0] += (((pSC->PredInfo[ii] + mbX - 1)->iDC + (pSC->PredInfoPrevRow[ii] + mbX)->iDC + 1) >> 1);
            }

            /* AD prediction */
            if(iADPredMode == 4){/* predict AD from top */
                pOrg[4] += (pSC->PredInfoPrevRow[ii] + mbX)->piAD[4]; /* AC of HT !!! */
                pOrg[2] += (pSC->PredInfoPrevRow[ii] + mbX)->piAD[3];
                pOrg[6] += pOrg[2];
            }
            else if(iADPredMode == 0){/* predict AD from left */
                pOrg[4] += (pSC->PredInfo[ii] + mbX - 1)->piAD[4];  /* AC of HT !!! */
                pOrg[1] += (pSC->PredInfo[ii] + mbX - 1)->piAD[0];
                pOrg[5] += (pSC->PredInfo[ii] + mbX - 1)->piAD[2];
            }
            else if(iDCPredMode == 1){
                pOrg[6] += pOrg[2];
            }
        }
    }

    pMBInfo->iOrientation = 2 - getACPredMode(pMBInfo, cf);
}

/*************************************************************************
    Frequency domain inverse AC prediction
*************************************************************************/
Void predACDec(CWMImageStrCodec * pSC)
{
    const COLORFORMAT cf = pSC->m_param.cfColorFormat;
    const Int iChannels = (cf == YUV_420 || cf == YUV_422) ? 1 : (Int) pSC->m_param.cNumChannels;
    /* size_t mbX = pSC->cColumn, mbY = pSC->cRow; */
    CWMIMBInfo *pMBInfo = &pSC->MBInfo;
    Int iACPredMode = 2 - pMBInfo->iOrientation;
    PixelI * pOrg, * pRef;
    Int i, j;

    /* AC prediction */
    for(i = 0; i < iChannels; i++){
        /* prediction only happens inside MB */
        PixelI* pSrc = pSC->p1MBbuffer[i];/*0 == i ? pSC->pY1 : (1 == i ? pSC->pU1 : pSC->pV1); */

        switch (iACPredMode)
        {
            case 1:
            {
                /* predict from top */
                static U8 blkIdx[] = {1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15};

                for (j = 0; j < sizeof(blkIdx) / sizeof(*blkIdx); ++j)
                {
                    pOrg = pSrc + 16 * blkIdx[j];
                    pRef = pOrg - 16;

                    pOrg[ 2] += pRef[ 2];
                    pOrg[10] += pRef[10];
                    pOrg[ 9] += pRef[ 9];
                }
                break;
            }

            case 0:
                /* predict from left */
                for (j = 64; j < 256; j += 16)
                {
                    pOrg = pSrc + j;
                    pRef = pOrg - 64;

                    pOrg[1] += pRef[1];
                    pOrg[5] += pRef[5];
                    pOrg[6] += pRef[6];
                }
                break;

            default:
                /* no prediction */
                break;
        }
    }

    if(cf == YUV_420){
        for(i = 16; i <= 20; i += 4){
            PixelI* pSrc = pSC->p1MBbuffer[(i >> 2) - 3];/*16 == i ? pSC->pU1 : pSC->pV1; */

            switch (iACPredMode)
            {
                case 1:
                {
                    /* predict from top */
                    for (j = 1; j <= 3; j += 2)
                    {
                        pOrg = pSrc + 16 * j;
                        pRef = pOrg - 16;

                        pOrg[ 2] += pRef[ 2];
                        pOrg[10] += pRef[10];
                        pOrg[ 9] += pRef[ 9];
                    }
                    break;
                }

                case 0:
                    /* predict from left */
                    for (j = 2; j <= 3; ++j)
                    {
                        pOrg = pSrc + 16 * j;
                        pRef = pOrg - 32;

                        pOrg[1] += pRef[1];
                        pOrg[5] += pRef[5];
                        pOrg[6] += pRef[6];
                    }
                    break;

                default:
                    /* no prediction */
                    break;
            }
        }
    }
    else if(cf == YUV_422){
        for(i = 16; i < 32; i += 8){
            PixelI* pSrc = pSC->p1MBbuffer[(i >> 3) - 1];/*16 == i ? pSC->pU1 : pSC->pV1; */

            switch (iACPredMode)
            {
                case 1:
                {
                    /* predict from top */
                    for (j = 2; j < 8; j ++)
                    {
                        pOrg = pSrc + blkOffsetUV_422[j];
                        pRef = pOrg - 16;

                        pOrg[10] += pRef[10];
                        pOrg[ 2] += pRef[ 2];
                        pOrg[ 9] += pRef[ 9];
                    }
                    break;
                }

                case 0:
                    /* predict from left */
                    for (j = 1; j < 8; j += 2)
                    {
                        pOrg = pSrc + blkOffsetUV_422[j];
                        pRef = pOrg - 64;

                        pOrg[1] += pRef[1];
                        pOrg[5] += pRef[5];
                        pOrg[6] += pRef[6];
                    }
                    break;

                default:
                    /* no prediction */
                    break;
            }
        }
    }    
}

/*************************************************************************
    CBP
*************************************************************************/
static int NumOnes(int i)
{
    int retval = 0;
    static const int g_Count[] = { 0,1,1,2, 1,2,2,3, 1,2,2,3, 2,3,3,4 };
    i = i & 0xffff;
    while (i) {
        retval += g_Count[i & 0xf];
        i >>= 4;
    }
    return retval;
}

#define SATURATE32(x) if((unsigned int)(x + 16) >= 32) { if (x < 0) x = -16; else x = 15; }

/* CBP prediction for 16 x 16 MB */
/* block index */
/*  0  1  4  5 */
/*  2  3  6  7 */
/*  8  9 12 13 */
/* 10 11 14 15 */
static Int predCBPCDec(CWMImageStrCodec * pSC, Int iCBP, size_t mbX, size_t mbY, size_t c, CCBPModel *pModel)
{
    Int iNOrig;
    const int iNDiff = AVG_NDIFF;
    size_t c1 = c ? 1 : 0;

    UNREFERENCED_PARAMETER( mbY );

    if (pModel->m_iState[c1] == 0) {
        if(pSC->m_bCtxLeft) {
            if (pSC->m_bCtxTop) {
                iCBP ^= 1;
            }
            else {
                Int iTopCBP  = (pSC->PredInfoPrevRow[c] + mbX)->iCBP;
                iCBP ^= (iTopCBP >> 10) & 1; /* left: top(10) => 0 */
            }
        }
        else {
            Int iLeftCBP = (pSC->PredInfo[c] + mbX - 1)->iCBP;
            iCBP ^= ((iLeftCBP >> 5) & 1); /* left(5) => 0 */
        }

        iCBP ^= (0x02 & (iCBP << 1)); /* 0 => 1 */
        iCBP ^= (0x10 & (iCBP << 3)); /* 1 => 4 */
        iCBP ^= (0x20 & (iCBP << 1)); /* 4 => 5 */

        iCBP ^= ((iCBP & 0x33) << 2);
        iCBP ^= ((iCBP & 0xcc) << 6);
        iCBP ^= ((iCBP & 0x3300) << 2);

    }
    else if (pModel->m_iState[c1] == 2) {
        iCBP ^= 0xffff;
    }

    iNOrig = NumOnes(iCBP);

    pModel->m_iCount0[c1] += iNOrig - iNDiff;
    SATURATE32(pModel->m_iCount0[c1]);

    pModel->m_iCount1[c1] += 16 - iNOrig - iNDiff;
    SATURATE32(pModel->m_iCount1[c1]);

    if (pModel->m_iCount0[c1] < 0) {
        if (pModel->m_iCount0[c1] < pModel->m_iCount1[c1]) {
            pModel->m_iState[c1] = 1;
        }
        else {
            pModel->m_iState[c1] = 2;
        }
    }
    else if (pModel->m_iCount1[c1] < 0) {
        pModel->m_iState[c1] = 2;
    }
    else {
        pModel->m_iState[c1] = 0;
    }
    return iCBP;
}

static Int predCBPC420Dec(CWMImageStrCodec * pSC, Int iCBP, size_t mbX, size_t mbY, size_t c, CCBPModel *pModel)
{
    Int iNOrig;
    const int iNDiff = AVG_NDIFF;

    UNREFERENCED_PARAMETER( mbY );

    if (pModel->m_iState[1] == 0) {
        if(pSC->m_bCtxLeft) {
            if (pSC->m_bCtxTop) {
                iCBP ^= 1;
            }
            else {
                Int iTopCBP  = (pSC->PredInfoPrevRow[c] + mbX)->iCBP;
                iCBP ^= (iTopCBP >> 2) & 1; /* left: top(2) => 0 */
            }
        }
        else {
            Int iLeftCBP = (pSC->PredInfo[c] + mbX - 1)->iCBP;
            iCBP ^= ((iLeftCBP >> 1) & 1); /* left(1) => 0 */
        }

        iCBP ^= (0x02 & (iCBP << 1)); /* 0 => 1 */
        iCBP ^= ((iCBP & 0x3) << 2); /* [0 1] -> [2 3] */
    }
    else if (pModel->m_iState[1] == 2) {
        iCBP ^= 0xf;
    }

    iNOrig = NumOnes(iCBP) * 4;

    pModel->m_iCount0[1] += iNOrig - iNDiff;
    SATURATE32(pModel->m_iCount0[1]);

    pModel->m_iCount1[1] += 16 - iNOrig - iNDiff;
    SATURATE32(pModel->m_iCount1[1]);

    if (pModel->m_iCount0[1] < 0) {
        if (pModel->m_iCount0[1] < pModel->m_iCount1[1]) {
            pModel->m_iState[1] = 1;
        }
        else {
            pModel->m_iState[1] = 2;
        }
    }
    else if (pModel->m_iCount1[1] < 0) {
        pModel->m_iState[1] = 2;
    }
    else {
        pModel->m_iState[1] = 0;
    }

    return iCBP;
}

static Int predCBPC422Dec(CWMImageStrCodec * pSC, Int iCBP, size_t mbX, size_t mbY, size_t c, CCBPModel *pModel)
{
    Int iNOrig;
    const int iNDiff = AVG_NDIFF;

    UNREFERENCED_PARAMETER( mbY );

    if (pModel->m_iState[1] == 0) {
        if(pSC->m_bCtxLeft) {
            if (pSC->m_bCtxTop) {
                iCBP ^= 1;
            }
            else {
                Int iTopCBP  = (pSC->PredInfoPrevRow[c] + mbX)->iCBP;
                iCBP ^= (iTopCBP >> 6) & 1; /* left: top(6) => 0 */
            }
        }
        else {
            Int iLeftCBP = (pSC->PredInfo[c] + mbX - 1)->iCBP;
            iCBP ^= ((iLeftCBP >> 1) & 1); /* left(1) => 0 */
        }
        
        iCBP ^= (iCBP & 0x1) << 1; /* [0]->[1] */
        iCBP ^= (iCBP & 0x3) << 2; /* [0 1]->[2 3] */
        iCBP ^= (iCBP & 0xc) << 2; /* [2 3]->[4 5] */
        iCBP ^= (iCBP & 0x30) << 2; /* [4 5]->[6 7] */
    }
    else if (pModel->m_iState[1] == 2) {
        iCBP ^= 0xff;
    }

    iNOrig = NumOnes(iCBP) * 2;

    pModel->m_iCount0[1] += iNOrig - iNDiff;
    SATURATE32(pModel->m_iCount0[1]);

    pModel->m_iCount1[1] += 16 - iNOrig - iNDiff;
    SATURATE32(pModel->m_iCount1[1]);

    if (pModel->m_iCount0[1] < 0) {
        if (pModel->m_iCount0[1] < pModel->m_iCount1[1]) {
            pModel->m_iState[1] = 1;
        }
        else {
            pModel->m_iState[1] = 2;
        }
    }
    else if (pModel->m_iCount1[1] < 0) {
        pModel->m_iState[1] = 2;
    }
    else {
        pModel->m_iState[1] = 0;
    }

    return iCBP;
}


/* Coded Block Pattern (CBP) prediction */
Void predCBPDec(CWMImageStrCodec *pSC, CCodingContext *pContext)
{
    const COLORFORMAT cf = pSC->m_param.cfColorFormat;
    const size_t iChannels = (cf == YUV_420 || cf == YUV_422) ? 1 : pSC->m_param.cNumChannels;
    size_t i, mbX = pSC->cColumn, mbY = pSC->cRow;
    CWMIMBInfo *pMBInfo = &(pSC->MBInfo);

    for (i = 0; i < iChannels; i++) {
        (pSC->PredInfo[i] + mbX)->iCBP = pMBInfo->iCBP[i] = predCBPCDec(pSC, pMBInfo->iDiffCBP[i], mbX, mbY, i, &pContext->m_aCBPModel); /* Y Channel */
    }

    if (cf == YUV_422){
        (pSC->PredInfo[1] + mbX)->iCBP = pMBInfo->iCBP[1] = predCBPC422Dec(pSC, pMBInfo->iDiffCBP[1], mbX, mbY, 1, &pContext->m_aCBPModel);
        (pSC->PredInfo[2] + mbX)->iCBP = pMBInfo->iCBP[2] = predCBPC422Dec(pSC, pMBInfo->iDiffCBP[2], mbX, mbY, 2, &pContext->m_aCBPModel);
    }
    else if (cf == YUV_420) {
        (pSC->PredInfo[1] + mbX)->iCBP = pMBInfo->iCBP[1] = predCBPC420Dec(pSC, pMBInfo->iDiffCBP[1], mbX, mbY, 1, &pContext->m_aCBPModel);
        (pSC->PredInfo[2] + mbX)->iCBP = pMBInfo->iCBP[2] = predCBPC420Dec(pSC, pMBInfo->iDiffCBP[2], mbX, mbY, 2, &pContext->m_aCBPModel);
    }
    /*} */
}


#undef DEQUANT
#undef SATURATE32
