/* jxr_sys.c - jxrlib JPEG XR decoder (see jxr_int.h) */
#include "jxr_int.h"
/* ---- adapthuff.c ---- */
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

#ifdef MEM_TRACE
#define TRACE_MALLOC    1
#define TRACE_NEW       0
#define TRACE_HEAP      0
/* (merged) #include "memtrace.h" */
#endif

/* Huffman lookup tables */
static const short g4HuffLookupTable[40] = {
  19,19,19,19,27,27,27,27,10,10,10,10,10,10,10,10,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  0,0,0,0,0,0,0,0 };

static const short g5HuffLookupTable[2][42] = {{
  28,28,36,36,19,19,19,19,10,10,10,10,10,10,10,10,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  0,0,0,0,0,0,0,0,0,0 },
  {
  11,11,11,11,19,19,19,19,27,27,27,27,35,35,35,35,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  0,0,0,0,0,0,0,0,0,0 }};

static const short g6HuffLookupTable[4][44] = {{
  13,29,44,44,19,19,19,19,34,34,34,34,34,34,34,34,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  0,0,0,0,0,0,0,0,0,0,0,0 },
  {
  12,12,28,28,43,43,43,43,2,2,2,2,2,2,2,2,
  18,18,18,18,18,18,18,18,34,34,34,34,34,34,34,34,
  0,0,0,0,0,0,0,0,0,0,0,0 },
  {
  4,4,12,12,43,43,43,43,18,18,18,18,18,18,18,18,
  26,26,26,26,26,26,26,26,34,34,34,34,34,34,34,34,
  0,0,0,0,0,0,0,0,0,0,0,0 },
  {
  5,13,36,36,43,43,43,43,18,18,18,18,18,18,18,18,
  25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,25,
  0,0,0,0,0,0,0,0,0,0,0,0 }};

static const short g7HuffLookupTable[2][46] = {{
  45,53,36,36,27,27,27,27,2,2,2,2,2,2,2,2,
  10,10,10,10,10,10,10,10,18,18,18,18,18,18,18,18,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
  {
  -32736,37,28,28,19,19,19,19,10,10,10,10,10,10,10,10,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  5,6,0,0,0,0,0,0,0,0,0,0,0,0 }};

static const short g8HuffLookupTable[2][48] = {{
  53,21,28,28,11,11,11,11,43,43,43,43,59,59,59,59,
  2,2,2,2,2,2,2,2,34,34,34,34,34,34,34,34,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 },
  {
  52,52,20,20,3,3,3,3,11,11,11,11,27,27,27,27,
  35,35,35,35,43,43,43,43,58,58,58,58,58,58,58,58,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 }};

static const short g9HuffLookupTable[2][50] = {{
  13,29,37,61,20,20,68,68,3,3,3,3,51,51,51,51,
  41,41,41,41,41,41,41,41,41,41,41,41,41,41,41,41,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0 },
  {
  -32736,53,28,28,11,11,11,11,19,19,19,19,43,43,43,43,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  -32734,4,7,8,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0 }};

static const short g12HuffLookupTable[5][56] = {{
  -32736,5,76,76,37,53,69,85,43,43,43,43,91,91,91,91,
  57,57,57,57,57,57,57,57,57,57,57,57,57,57,57,57,
  -32734,1,2,3,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0 },
  {
  -32736,85,13,53,4,4,36,36,43,43,43,43,67,67,67,67,
  75,75,75,75,91,91,91,91,58,58,58,58,58,58,58,58,
  2,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0 },
  {
  -32736,37,92,92,11,11,11,11,43,43,43,43,59,59,59,59,
  67,67,67,67,75,75,75,75,2,2,2,2,2,2,2,2,
  -32734,-32732,2,3,6,10,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0 },
  {
  -32736,29,37,69,3,3,3,3,43,43,43,43,59,59,59,59,
  75,75,75,75,91,91,91,91,10,10,10,10,10,10,10,10,
  -32734,10,2,6,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0 },
  {
  -32736,93,28,28,60,60,76,76,3,3,3,3,43,43,43,43,
  9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
  -32734,-32732,-32730,2,4,8,6,10,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0 }};

/**********************************************************************
  Allocation and dellocation
**********************************************************************/
Void Clean (CAdaptiveHuffman *pAdHuff)
{
    if (pAdHuff == NULL)
        return;
    free (pAdHuff);
}

CAdaptiveHuffman *Allocate (Int iNSymbols, CODINGMODE cm)
{
    CAdaptiveHuffman *pAdHuff = (CAdaptiveHuffman *) malloc (sizeof (CAdaptiveHuffman));

    UNREFERENCED_PARAMETER(cm);

    if (pAdHuff == NULL)
        return NULL;
    if (iNSymbols > 255 || iNSymbols <= 0)
        goto ErrorExit;

    memset (pAdHuff, 0, sizeof (CAdaptiveHuffman));
    pAdHuff->m_iNSymbols = iNSymbols;

    pAdHuff->m_pDelta = NULL;
    pAdHuff->m_iDiscriminant = pAdHuff->m_iUpperBound = pAdHuff->m_iLowerBound = 0;

    return pAdHuff;

ErrorExit:
    Clean (pAdHuff);
    return NULL;
}

/**********************************************************************
  Adapt Huffman table
**********************************************************************/
/* Alphabet size = 4 */
/* g_Index4Table (encoder only) removed */
static const Int g4CodeTable[] = {
    4,
    1, 1,
    1, 2,
    0, 3,
    1, 3
};

/* Alphabet size = 5 */
/* g_Index5Table (encoder only) removed */
static const Int g5CodeTable[] = {
    5,
    1, 1,
    1, 2,
    1, 3,
    0, 4,
    1, 4,

    5,
    1, 1,
    0, 3,
    1, 3,
    2, 3,
    3, 3,
};
static const Int g5DeltaTable[] = { 0,-1,0,1,1 };

/* Alphabet size = 6 */
/* g_Index6Table (encoder only) removed */
static const Int g6CodeTable[] = {
    6,
    1, 1,
    0, 5,
    1, 3,
    1, 5,
    1, 2,
    1, 4,

    6,
    1,  2,
    0,  4,
    2,  2,
    1,  4,
    3,  2,
    1,  3,

    6,
    0,  4,
    1,  4,
    1,  2,
    2,  2,
    3,  2,
    1,  3,

    6,
    0, 5,
    1, 5,
    1, 2,
    1, 1,
    1, 4,
    1, 3
};
static const Int g6DeltaTable[] = {
    -1, 1, 1, 1, 0, 1,
    -2, 0, 0, 2, 0, 0,
    -1,-1, 0, 1,-2, 0
};

/* Alphabet size = 7 */
/* g_Index7Table (encoder only) removed */
static const Int g7CodeTable[] = {
    7,
    1, 2,
    2, 2,
    3, 2,
    1, 3,
    1, 4,
    0, 5,
    1, 5,

    7,
    1, 1,
    1, 2,
    1, 3,
    1, 4,
    1, 5,
    0, 6,
    1, 6
};
static const Int g7DeltaTable[] = { 1,0,-1,-1,-1,-1,-1 };

/* Alphabet size = 8 */
/* g_Index8Table (encoder only) removed */
static const Int g8CodeTable[] = {
    8,
    2, 2,
    1, 3,
    1, 5,
    1, 4,
    3, 2,
    2, 3,
    0, 5,
    3, 3,

    8,
    1, 3,
    2, 3,
    1, 4,
    3, 3,
    4, 3,
    5, 3,
    0, 4,
    3, 2
};
/* g8DeltaTable (encoder only) removed */

/* g_Index9Table (encoder only) removed */
static const Int g9CodeTable[] = {
    9,
    2, 3,
    0, 5,
    2, 4,
    1, 5,
    2, 5,
    1, 1,
    3, 3,
    3, 5,
    3, 4,

    9,
    1, 1,
    1, 3,
    2, 3,
    1, 4,
    1, 6,
    3, 3,
    1, 5,
    0, 7,
    1, 7,
};
static const Int g9DeltaTable[] = { 2,2,1,1,-1,-2,-2,-2,-3 };

/* Alphabet size = 12 */
/* g_Index12Table (encoder only) removed */
static const Int g12CodeTable[] = {
    12,  
    1, 5,
    1, 6,
    0, 7,
    1, 7,
    4, 5,
    2, 3,
    5, 5,
    1, 1,
    6, 5,
    1, 4,
    7, 5,
    3, 3,

    12,
    2, 4,
    2, 5,
    0, 6,
    1, 6,
    3, 4,
    2, 3,
    3, 5,
    3, 2,
    3, 3,
    4, 3,
    1, 5,
    5, 3,

    12,
    3, 2,
    1, 3,
    0, 7,
    1, 7,
    1, 5,
    2, 3,
    2, 7,
    3, 3,
    4, 3,
    5, 3,
    3, 7,
    1, 4,

    12,
    1, 3,
    3, 2,
    0, 7,
    1, 5,
    2, 5,
    2, 3,
    1, 7,
    3, 3,
    3, 5,
    4, 3,
    1, 6,
    5, 3,

    12,
    2, 3,
    1, 1,
    1, 7,
    1, 4,
    2, 7,
    3, 3,
    0, 8,
    2, 4,
    3, 7,
    3, 4,
    1, 8,
    1, 5
};
static const Int g12DeltaTable[] = {
    1, 1, 1, 1, 1, 0, 0,-1, 2, 1, 0, 0,
    2, 2,-1,-1,-1, 0,-2,-1, 0, 0,-2,-1,
   -1, 1, 0, 2, 0, 0, 0, 0,-2, 0, 1, 1,
    0, 1, 0, 1,-2, 0,-1,-1,-2,-1,-2,-2
};

/**********************************************************************
  Adapt fixed length codes based on discriminant
**********************************************************************/
static const Int THRESHOLD = 8;
static const Int MEMORY = 8;
 
Void AdaptDiscriminant (CAdaptiveHuffman *pAdHuff)
{
    Int iSym = pAdHuff->m_iNSymbols, t, dL, dH;
    const Int *pCodes, *pDelta = NULL;
    Bool bChange = FALSE;
    static const Int gMaxTables[] = { 0,0,0,0, 1,2, 4,2, 2,2, 0,0,5 };
    static const Int gSecondDisc[]= { 0,0,0,0, 0,0, 1,0, 0,0, 0,0,1 };

    if (!pAdHuff->m_bInitialize) {
        pAdHuff->m_bInitialize = 1;
        pAdHuff->m_iDiscriminant = pAdHuff->m_iDiscriminant1 = 0;
        pAdHuff->m_iTableIndex = gSecondDisc[iSym];/*(gMaxTables[iSym] - 1) >> 1; */
    }

    dL = dH = pAdHuff->m_iDiscriminant;
    if (gSecondDisc[iSym]) {
        dH = pAdHuff->m_iDiscriminant1;
    }

    if (dL < pAdHuff->m_iLowerBound) {
        pAdHuff->m_iTableIndex--;
        bChange = TRUE;
    }
    else if (dH > pAdHuff->m_iUpperBound) {
        pAdHuff->m_iTableIndex++;
        bChange = TRUE;
    }
    if (bChange) {
    /** if initialization is fixed, we can exit on !bChange **/
        pAdHuff->m_iDiscriminant = 0;
        pAdHuff->m_iDiscriminant1 = 0;
    }
    {
        if (pAdHuff->m_iDiscriminant < -THRESHOLD * MEMORY)
            pAdHuff->m_iDiscriminant = -THRESHOLD * MEMORY;
        else if (pAdHuff->m_iDiscriminant > THRESHOLD * MEMORY)
            pAdHuff->m_iDiscriminant = THRESHOLD * MEMORY;

        if (pAdHuff->m_iDiscriminant1 < -THRESHOLD * MEMORY)
            pAdHuff->m_iDiscriminant1 = -THRESHOLD * MEMORY;
        else if (pAdHuff->m_iDiscriminant1 > THRESHOLD * MEMORY)
            pAdHuff->m_iDiscriminant1 = THRESHOLD * MEMORY;
    }

    t = pAdHuff->m_iTableIndex;
    assert (t >= 0);
    assert (t < gMaxTables[iSym]);

    /*pAdHuff->m_iDiscriminant >>= 1; */
    pAdHuff->m_iLowerBound = (t == 0) ? (-1 << 31) : -THRESHOLD;
    pAdHuff->m_iUpperBound = (t == gMaxTables[iSym] - 1) ? (1 << 30) : THRESHOLD;

    switch (iSym) {
        case 4:
            pCodes = g4CodeTable;
            pAdHuff->m_hufDecTable = (short *) g4HuffLookupTable;
            break;
        case 5:
            pCodes = g5CodeTable + (iSym * 2 + 1) * t;
            pDelta = g5DeltaTable;
            pAdHuff->m_hufDecTable = g5HuffLookupTable[t];
            break;
        case 6:
            pCodes = g6CodeTable + (iSym * 2 + 1) * t;
            pAdHuff->m_pDelta1 = g6DeltaTable + iSym * (t - (t + 1 == gMaxTables[iSym]));
            pDelta = g6DeltaTable + (t - 1 + (t == 0)) * iSym;
            pAdHuff->m_hufDecTable = g6HuffLookupTable[t];
            break;
        case 7:
            pCodes = g7CodeTable + (iSym * 2 + 1) * t;
            pDelta = g7DeltaTable;
            pAdHuff->m_hufDecTable = g7HuffLookupTable[t];
            break;
        case 8:
            /*printf ("%d ", t); */
            pCodes = g8CodeTable;/* + (iSym * 2 + 1) * t; */
            /*pDelta = g8DeltaTable; */
            pAdHuff->m_hufDecTable = g8HuffLookupTable[0];
            break;
        case 9:
            pCodes = g9CodeTable + (iSym * 2 + 1) * t;
            pDelta = g9DeltaTable;
            pAdHuff->m_hufDecTable = g9HuffLookupTable[t];
            break;
        case 12:
            pCodes = g12CodeTable + (iSym * 2 + 1) * t;
            pAdHuff->m_pDelta1 = g12DeltaTable + iSym * (t - (t + 1 == gMaxTables[iSym]));
            pDelta = g12DeltaTable + (t - 1 + (t == 0)) * iSym;
            pAdHuff->m_hufDecTable = g12HuffLookupTable[t];
            break;
        default:
            assert (0); /* undefined fixed length table */
            return;
    }

    pAdHuff->m_pTable = pCodes;
    pAdHuff->m_pDelta = pDelta;
}


#undef TRACE_MALLOC
#undef TRACE_NEW
#undef TRACE_HEAP
/* ---- image.c ---- */
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
/* #include "xplatform_image.h" */

#ifdef MEM_TRACE
#define TRACE_MALLOC    1
#define TRACE_NEW       0
#define TRACE_HEAP      0
/* (merged) #include "memtrace.h" */
#endif

#include <stdlib.h>
#include <string.h>

#if !(defined(__ANSI__))
/* Desktop */
/* removed: #include <windows.h> */
#else
/* ANSI */
#include <time.h>
#endif  

Int grgiZigzagInv4x4_lowpass [] = {
    0, 1, 4, 5, 2, 8, 6, 9,
    3, 12, 10, 7, 13, 11, 14, 15
};

Int grgiZigzagInv4x4H [] = {
    0, 1, 4, 5, 2, 8, 6, 9,
    3, 12, 10, 7, 13, 11, 14, 15
};
Int grgiZigzagInv4x4V [] = {
    0, 4, 8, 5,  1, 12, 9, 6,  2, 13, 3, 15,  7, 10, 14, 11
};
    
const Int gSignificantRunBin[] = { 
    -1,-1,-1,-1,
    2,2,2,
    1,1,1,1,
    0,0,0,0    
};

const Int gSignificantRunFixedLength[] = {
    0,0,1,1,3,
    0,0,1,1,2,
    0,0,0,0,1,
};

/*************************************************************************
    UpdateModelMB : update adaptive model at end of macroblock
    (for lowest resolution only)
*************************************************************************/
#define MODELWEIGHT 70/*90 */

Void UpdateModelMB (COLORFORMAT cf, Int iChannels, Int iLaplacianMean[], CAdaptiveModel *pModel)
{
    Int j;
    static const Int aWeight0[3] = { 240/*DC*/, 12/*LP*/, 1 };
    static const Int aWeight1[3][MAX_CHANNELS] = {
        { 0,240,120,80, 60,48,40,34, 30,27,24,22, 20,18,17,16 },
        { 0,12,6,4,     3,2,2,2,     2,1,1,1,     1,1,1,1 },
        { 0,16,8,5,     4,3,3,2,     2,2,2,1,     1,1,1,1 }
    };
    static const Int aWeight2[6] = { 120,37,2,/*420*/ 120,18,1/*422*/ };

    iLaplacianMean[0] *= aWeight0[pModel->m_band - BAND_DC];
    if (cf == YUV_420) {
        iLaplacianMean[1] *= aWeight2[pModel->m_band - BAND_DC];
    }
    else if (cf == YUV_422) {
        iLaplacianMean[1] *= aWeight2[3 + (pModel->m_band) - BAND_DC];
    }
    else {
        iLaplacianMean[1] *= aWeight1[pModel->m_band - BAND_DC][iChannels - 1];
        if (pModel->m_band == BAND_AC)
            iLaplacianMean[1] >>= 4;
    }

    for (j = 0; j < 2; j++) {
        Int iLM = iLaplacianMean[j];
        Int iMS = pModel->m_iFlcState[j];
        Int iDelta = (iLM - MODELWEIGHT) >> 2;

        if (iDelta <= -8) {
            iDelta += 4;
            if (iDelta < -16)
                iDelta = -16;
            iMS += iDelta;
            if (iMS < -8) {
                if (pModel->m_iFlcBits[j] == 0)
                    iMS = -8;
                else {
                    iMS = 0;
                    pModel->m_iFlcBits[j]--;
                }
            }
        }
        else if (iDelta >= 8) {
            iDelta -= 4;
            if (iDelta > 15)
                iDelta = 15;
            iMS += iDelta;
            if (iMS > 8) {
                if (pModel->m_iFlcBits[j] >= 15) {
                    pModel->m_iFlcBits[j] = 15;
                    iMS = 8;
                }
                else {
                    iMS = 0;
                    pModel->m_iFlcBits[j]++;
                }
            }
        }
        pModel->m_iFlcState[j] = iMS;
        if (cf == Y_ONLY)
            break;
    }
}


Void ResetCodingContext(CCodingContext *pContext)
{
    /* reset bit reduction models */
    memset (&(pContext->m_aModelAC), 0, sizeof(CAdaptiveModel));  
    pContext->m_aModelAC.m_band = BAND_AC;

    memset (&(pContext->m_aModelLP), 0, sizeof(CAdaptiveModel));  
    pContext->m_aModelLP.m_band = BAND_LP;
    pContext->m_aModelLP.m_iFlcBits[0] = pContext->m_aModelLP.m_iFlcBits[1] = 4;

    memset (&(pContext->m_aModelDC), 0, sizeof(CAdaptiveModel));  
    pContext->m_aModelDC.m_band = BAND_DC;
    pContext->m_aModelDC.m_iFlcBits[0] = pContext->m_aModelDC.m_iFlcBits[1] = 8;

    /* reset CBP models */
    pContext->m_iCBPCountMax = pContext->m_iCBPCountZero = 1;

    pContext->m_aCBPModel.m_iCount0[0] = pContext->m_aCBPModel.m_iCount0[1] = -4;
    pContext->m_aCBPModel.m_iCount1[0] = pContext->m_aCBPModel.m_iCount1[1] = 4;
    pContext->m_aCBPModel.m_iState[0] = pContext->m_aCBPModel.m_iState[1] = 0;
}

/*************************************************************************
    Initialize zigzag scan parameters
*************************************************************************/
Void InitZigzagScan(CCodingContext * pContext)
{
    if (NULL != pContext) {
        Int i;
        for (i=0; i<16; i++) {
            pContext->m_aScanLowpass[i].uScan = grgiZigzagInv4x4_lowpass[i];
            pContext->m_aScanHoriz[i].uScan = dctIndex[0][grgiZigzagInv4x4H[i]];
            pContext->m_aScanVert[i].uScan  = dctIndex[0][grgiZigzagInv4x4V[i]];
        }
    }
}

#undef TRACE_MALLOC
#undef TRACE_NEW
#undef TRACE_HEAP
#undef MODELWEIGHT
/* ---- strcodec.c ---- */
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
/* (merged) #include "perfTimer.h" */

#ifdef MEM_TRACE
#define TRACE_MALLOC    1
#define TRACE_NEW       0
#define TRACE_HEAP      0
/* (merged) #include "memtrace.h" */
#endif

/*================================================================ */
/* Quantization index tables */
/*================================================================ */
const int blkOffset[16] = {0, 64, 16, 80, 128, 192, 144, 208, 32, 96, 48, 112, 160, 224, 176, 240};
const int blkOffsetUV[4] = {0, 32, 16, 48};
const int blkOffsetUV_422[8] = {0, 64, 16, 80, 32, 96, 48, 112};

const int dctIndex[3][16] = { /** permutation matrix tailored to the transform, nothing to do with ZZS **/
    {0,5,1,6, 10,12,8,14, 2,4,3,7, 9,13,11,15}, /*AC 444 */
    {0,5,1,6, 10,12,8,14, 2,4,3,7, 9,13,11,15}, /*AC 420 */
    {0,128,64,208, 32,240,48,224, 16,192,80,144, 112,176,96,160 }, /*DC 444 */
};

/*================================================================ */
/* Color conversion index table */
/*================================================================ */
const U8 idxCC[16][16] =
{
    {0x00, 0x01, 0x05, 0x04,  0x40, 0x41, 0x45, 0x44,  0x80, 0x81, 0x85, 0x84,  0xc0, 0xc1, 0xc5, 0xc4, },
    {0x02, 0x03, 0x07, 0x06,  0x42, 0x43, 0x47, 0x46,  0x82, 0x83, 0x87, 0x86,  0xc2, 0xc3, 0xc7, 0xc6, },
    {0x0a, 0x0b, 0x0f, 0x0e,  0x4a, 0x4b, 0x4f, 0x4e,  0x8a, 0x8b, 0x8f, 0x8e,  0xca, 0xcb, 0xcf, 0xce, },
    {0x08, 0x09, 0x0d, 0x0c,  0x48, 0x49, 0x4d, 0x4c,  0x88, 0x89, 0x8d, 0x8c,  0xc8, 0xc9, 0xcd, 0xcc, },

    {0x10, 0x11, 0x15, 0x14,  0x50, 0x51, 0x55, 0x54,  0x90, 0x91, 0x95, 0x94,  0xd0, 0xd1, 0xd5, 0xd4, },
    {0x12, 0x13, 0x17, 0x16,  0x52, 0x53, 0x57, 0x56,  0x92, 0x93, 0x97, 0x96,  0xd2, 0xd3, 0xd7, 0xd6, },
    {0x1a, 0x1b, 0x1f, 0x1e,  0x5a, 0x5b, 0x5f, 0x5e,  0x9a, 0x9b, 0x9f, 0x9e,  0xda, 0xdb, 0xdf, 0xde, },
    {0x18, 0x19, 0x1d, 0x1c,  0x58, 0x59, 0x5d, 0x5c,  0x98, 0x99, 0x9d, 0x9c,  0xd8, 0xd9, 0xdd, 0xdc, },

    {0x20, 0x21, 0x25, 0x24,  0x60, 0x61, 0x65, 0x64,  0xa0, 0xa1, 0xa5, 0xa4,  0xe0, 0xe1, 0xe5, 0xe4, },
    {0x22, 0x23, 0x27, 0x26,  0x62, 0x63, 0x67, 0x66,  0xa2, 0xa3, 0xa7, 0xa6,  0xe2, 0xe3, 0xe7, 0xe6, },
    {0x2a, 0x2b, 0x2f, 0x2e,  0x6a, 0x6b, 0x6f, 0x6e,  0xaa, 0xab, 0xaf, 0xae,  0xea, 0xeb, 0xef, 0xee, },
    {0x28, 0x29, 0x2d, 0x2c,  0x68, 0x69, 0x6d, 0x6c,  0xa8, 0xa9, 0xad, 0xac,  0xe8, 0xe9, 0xed, 0xec, },

    {0x30, 0x31, 0x35, 0x34,  0x70, 0x71, 0x75, 0x74,  0xb0, 0xb1, 0xb5, 0xb4,  0xf0, 0xf1, 0xf5, 0xf4, },
    {0x32, 0x33, 0x37, 0x36,  0x72, 0x73, 0x77, 0x76,  0xb2, 0xb3, 0xb7, 0xb6,  0xf2, 0xf3, 0xf7, 0xf6, },
    {0x3a, 0x3b, 0x3f, 0x3e,  0x7a, 0x7b, 0x7f, 0x7e,  0xba, 0xbb, 0xbf, 0xbe,  0xfa, 0xfb, 0xff, 0xfe, },
    {0x38, 0x39, 0x3d, 0x3c,  0x78, 0x79, 0x7d, 0x7c,  0xb8, 0xb9, 0xbd, 0xbc,  0xf8, 0xf9, 0xfd, 0xfc, },
};

const U8 idxCC_420[8][8] =
{
    {0x00, 0x01, 0x05, 0x04,  0x20, 0x21, 0x25, 0x24, },
    {0x02, 0x03, 0x07, 0x06,  0x22, 0x23, 0x27, 0x26, },
    {0x0a, 0x0b, 0x0f, 0x0e,  0x2a, 0x2b, 0x2f, 0x2e, },
    {0x08, 0x09, 0x0d, 0x0c,  0x28, 0x29, 0x2d, 0x2c, },

    {0x10, 0x11, 0x15, 0x14,  0x30, 0x31, 0x35, 0x34, },
    {0x12, 0x13, 0x17, 0x16,  0x32, 0x33, 0x37, 0x36, },
    {0x1a, 0x1b, 0x1f, 0x1e,  0x3a, 0x3b, 0x3f, 0x3e, },
    {0x18, 0x19, 0x1d, 0x1c,  0x38, 0x39, 0x3d, 0x3c, },
};

/*************************************************************************
    gGDISignature
*************************************************************************/
const Char gGDISignature[] = {'W', 'M', 'P', 'H', 'O', 'T', 'O', '\0'};

/* check if enough memory allocated for the image buffer */
Int checkImageBuffer(CWMImageStrCodec * pSC, size_t cWidth, size_t cRows)
{
    const BITDEPTH_BITS bd = pSC->WMISCP.bYUVData ?
        BD_32S : pSC->WMII.bdBitDepth;
    const COLORFORMAT cf = pSC->WMISCP.bYUVData ?
        pSC->m_param.cfColorFormat : pSC->WMII.cfColorFormat;
    size_t cBytes;
    Bool bLessThan64Bit = sizeof(void *) < 8;

    if(cf == YUV_420)
        cRows = (cRows + 1) / 2;
    if(cRows > pSC->WMIBI.cLine)
        return ICERR_ERROR;

    if(cf == YUV_422 || cf == YUV_420)
        cWidth = (cWidth + 1) / 2;

    if (bLessThan64Bit && (cWidth >> ((sizeof(size_t) * 8 - 5)))) {
        /** potential overflow - 32 bit pointers insufficient to address cache **/
        /** this uses 2 macroblock row constraint, which is tighter than ensuring rollover doesn't occur below **/
        return ICERR_ERROR;
    }

    cBytes = pSC->WMISCP.bYUVData ? cWidth * sizeof(PixelI) *
        (cf == YUV_420 ? 6 : (cf == YUV_422 ? 4 : (cf == YUV_444 ? 3 : 1))) :
        (bd == BD_1 ? (pSC->WMII.cBitsPerUnit * cWidth + 7) / 8 : (pSC->WMII.cBitsPerUnit + 7) / 8 * cWidth);

    return (cBytes > pSC->WMIBI.cbStride ? ICERR_ERROR : ICERR_OK);
}

Void writeQPIndex(BitIOInfo * pIO, U8 uiIndex, U32 cBits)
{
    if(uiIndex == 0)
        putBit16(pIO, 1, 1); /* default QP */
    else{
        putBit16(pIO, 0, 1); /* non default QP */
        putBit16(pIO, uiIndex - 1, cBits);
    }
}

U8 readQPIndex(BitIOInfo * pIO, U32 cBits)
{
    if(getBit16(pIO, 1))
        return 0; /* default QP */

    return (U8) getBit16(pIO, cBits) + 1;
}

Void getTilePos(CWMImageStrCodec* pSC, size_t mbX, size_t mbY)
{   
    if(mbX == 0){ /* left image boundary */
        pSC->cTileColumn = 0;
    }
    else if(pSC->cTileColumn < pSC->WMISCP.cNumOfSliceMinus1V && mbX == pSC->WMISCP.uiTileX[pSC->cTileColumn + 1]){ /* left tile boundary */
        pSC->cTileColumn ++;
    }

    if(mbY == 0){ /* top image boundary */
        pSC->cTileRow = 0;
    }
    else if(pSC->cTileRow < pSC->WMISCP.cNumOfSliceMinus1H && mbY == pSC->WMISCP.uiTileY[pSC->cTileRow + 1]){ /* top tile boundary */
        pSC->cTileRow ++;
    }

    pSC->m_bCtxLeft = (mbX == pSC->WMISCP.uiTileX[pSC->cTileColumn]);
    pSC->m_bCtxTop  = (mbY == pSC->WMISCP.uiTileY[pSC->cTileRow]);

    pSC->m_bResetContext = pSC->m_bResetRGITotals = (((mbX - pSC->WMISCP.uiTileX[pSC->cTileColumn]) & 0xf) == 0);
    if(pSC->cTileColumn == pSC->WMISCP.cNumOfSliceMinus1V){ /* last tile column */
        if(mbX + 1 == pSC->cmbWidth)
            pSC->m_bResetContext = TRUE;
    }
    else if(mbX + 1 == pSC->WMISCP.uiTileX[pSC->cTileColumn + 1])
        pSC->m_bResetContext = TRUE;
}

/*================================================================ */
/* utility functions for 2 macro block rows */
/*================================================================ */
Void initMRPtr(CWMImageStrCodec* pSC)
{
    size_t j, jend = (pSC->m_pNextSC != NULL);

    for (j = 0; j <= jend; j++) {
        memcpy (pSC->p0MBbuffer, pSC->a0MBbuffer, sizeof (pSC->p0MBbuffer));
        memcpy (pSC->p1MBbuffer, pSC->a1MBbuffer, sizeof (pSC->p1MBbuffer));
        pSC = pSC->m_pNextSC;
    }
}

Void advanceMRPtr(CWMImageStrCodec* pSC)
{
    const COLORFORMAT cf = pSC->m_param.cfColorFormat;
    const int cpChroma = cblkChromas[cf] * 16;
    size_t i, j, jend = (pSC->m_pNextSC != NULL);

    assert(pSC->m_bSecondary == FALSE);
    for (j = 0; j <= jend; j++) {
        int cpStride = 16 * 16;
        for (i = 0; i < pSC->m_param.cNumChannels; i++) {
            pSC->pPlane[i] = pSC->p0MBbuffer[i];

            pSC->p0MBbuffer[i] += cpStride;
            pSC->p1MBbuffer[i] += cpStride;

            cpStride = cpChroma;
        }
        pSC = pSC->m_pNextSC;
    }
}

/* advance to next MB row */
Void advanceOneMBRow(CWMImageStrCodec *pSC)
{
    size_t i, j, jend = (pSC->m_pNextSC != NULL);
    CWMIPredInfo *pPredInfo;

    for (j = 0; j <= jend; j++) {
        for(i = 0; i < pSC->m_param.cNumChannels; i ++){  /* swap current row and previous row */
            pPredInfo = pSC->PredInfo[i];
            pSC->PredInfo[i] = pSC->PredInfoPrevRow[i];
            pSC->PredInfoPrevRow[i] = pPredInfo;
        }
        pSC = pSC->m_pNextSC;
    }
}

Void swapMRPtr(CWMImageStrCodec* pSC)
{
    PixelI *pTemp[MAX_CHANNELS];
    size_t j, jend = (pSC->m_pNextSC != NULL);

    for (j = 0; j <= jend; j++) {
        memcpy (pTemp, pSC->a0MBbuffer, sizeof (pSC->a0MBbuffer));
        memcpy (pSC->a0MBbuffer, pSC->a1MBbuffer, sizeof (pSC->a0MBbuffer));
        memcpy (pSC->a1MBbuffer, pTemp, sizeof (pSC->a0MBbuffer));
        pSC = pSC->m_pNextSC;
    }
}

/*================================================================ */
/* Empty function to fill slot */
/*================================================================ */
Int IDPEmpty(CWMImageStrCodec* pSC)
{
    UNREFERENCED_PARAMETER( pSC );

    return ICERR_OK;
}

ERR WMPAlloc(void** ppv, size_t cb)
{
    *ppv = calloc(1, cb);
    return *ppv ? WMP_errSuccess : WMP_errOutOfMemory;
}

ERR WMPFree(void** ppv)
{
    if (*ppv)
    {
        free(*ppv);
        *ppv = NULL;
    }

    return WMP_errSuccess;
}

/*================================================================ */
/* Streaming I/O functions */
/*================================================================ */
/* (file streams are not used: FLASHNT decodes from memory) */

/*---------------------------------------------------------------- */
ERR CreateWS_Memory(struct WMPStream** ppWS, void* pv, size_t cb)
{
    ERR err = WMP_errSuccess;
    struct WMPStream* pWS = NULL;

    Call(WMPAlloc((void** )ppWS, sizeof(**ppWS)));
    pWS = *ppWS;

    pWS->state.buf.pbBuf = pv;
    pWS->state.buf.cbBuf = cb;
    pWS->state.buf.cbCur = 0;

    pWS->Close = CloseWS_Memory;
    pWS->EOS = EOSWS_Memory;

    pWS->Read = ReadWS_Memory;
    pWS->Write = WriteWS_Memory;

    pWS->SetPos = SetPosWS_Memory;
    pWS->GetPos = GetPosWS_Memory;

Cleanup:    
    return err;
}

ERR CloseWS_Memory(struct WMPStream** ppWS)
{
    ERR err = WMP_errSuccess;

    Call(WMPFree((void**)ppWS));
    
Cleanup:    
    return err;
}

Bool EOSWS_Memory(struct WMPStream* pWS)
{
    return pWS->state.buf.cbBuf <= pWS->state.buf.cbCur;
}

ERR ReadWS_Memory(struct WMPStream* pWS, void* pv, size_t cb)
{
    ERR err = WMP_errSuccess;

/*    FailIf(pWS->state.buf.cbBuf < pWS->state.buf.cbCur, WMP_errBufferOverflow); */
    if(pWS->state.buf.cbBuf < pWS->state.buf.cbCur)
        return err;

    FailIf(pWS->state.buf.cbCur + cb < pWS->state.buf.cbCur, WMP_errBufferOverflow);
    if (pWS->state.buf.cbBuf < pWS->state.buf.cbCur + cb)
    {
        cb = pWS->state.buf.cbBuf - pWS->state.buf.cbCur;
    }

    memcpy(pv, pWS->state.buf.pbBuf + pWS->state.buf.cbCur, cb);
    pWS->state.buf.cbCur += cb;

Cleanup:
    return err;
}

ERR WriteWS_Memory(struct WMPStream* pWS, const void* pv, size_t cb)
{
    ERR err = WMP_errSuccess;

    FailIf(pWS->state.buf.cbCur + cb < pWS->state.buf.cbCur, WMP_errBufferOverflow);
    FailIf(pWS->state.buf.cbBuf < pWS->state.buf.cbCur + cb, WMP_errBufferOverflow);

    memcpy(pWS->state.buf.pbBuf + pWS->state.buf.cbCur, pv, cb);
    pWS->state.buf.cbCur += cb;

Cleanup:
    return err;
}

ERR SetPosWS_Memory(struct WMPStream* pWS, size_t offPos)
{
    ERR err = WMP_errSuccess;

    /*While the following condition is possibly useful, failure occurs */
    /*at the end of a file since packets beyond the end may be accessed */
    /*FailIf(pWS->state.buf.cbBuf < offPos, WMP_errBufferOverflow); */
    pWS->state.buf.cbCur = offPos;

/*Cleanup: */
    return err;
}

ERR GetPosWS_Memory(struct WMPStream* pWS, size_t* poffPos)
{
    *poffPos = pWS->state.buf.cbCur;

    return WMP_errSuccess;
}

/*================================================================= */
/* Linked list based WMPStream */
/* - for indefinite size, multiple stream out */
/* - reads not supported in this mode */
/*================================================================= */
ERR CreateWS_List(struct WMPStream** ppWS)
{
    ERR err = WMP_errSuccess;
    struct WMPStream* pWS = NULL;

    Call(WMPAlloc((void** )ppWS, sizeof(**ppWS) + PACKETLENGTH + sizeof(void *)));
    pWS = *ppWS;

    pWS->state.buf.pbBuf = (U8 *)pWS + sizeof(**ppWS) + sizeof(void *); /* first buffer points here */

    memset(pWS->state.buf.pbBuf - sizeof(void *), 0, sizeof(void *));
    pWS->state.buf.cbBuf = PACKETLENGTH;
    pWS->state.buf.cbCur = 0;
    pWS->state.buf.cbBufCount = 0;

    pWS->Close = CloseWS_List;
    pWS->EOS = NULL; /* doesn't get called */

    pWS->Read = ReadWS_List;
    pWS->Write = WriteWS_List;

    pWS->SetPos = SetPosWS_List;
    pWS->GetPos = GetPosWS_List;

    /*printf ("create buffer %d: %x\n", pWS->state.buf.cbBufCount, pWS->state.buf.pbBuf); */

Cleanup:    
    return err;
}

ERR CloseWS_List(struct WMPStream** ppWS)
{
    ERR err = WMP_errSuccess;

    if (ppWS) {
        U8 *pBuf = (U8 *)(ppWS[0] + 1); /* pointer to buffer */
        U8 *pNext = (U8 *)(((void **)pBuf)[0]);
        while (pNext) {
/*struct WMPStream *pWS = ppWS[0]; */
            pBuf = pNext;
            pNext = (U8 *)(((void **)(pBuf))[0]);
/*printf ("delete buffer    %x\n", pBuf); */
            Call(WMPFree((void**)&pBuf));
        }
    }
    Call(WMPFree((void**)ppWS));
    
Cleanup:    
    return err;
}

ERR ReadWS_List(struct WMPStream* pWS, void* pv, size_t cb)
{
    ERR err = WMP_errSuccess;

    FailIf(pWS->state.buf.cbCur + cb < pWS->state.buf.cbCur, WMP_errBufferOverflow);
    if (pWS->state.buf.cbBuf < pWS->state.buf.cbCur + PACKETLENGTH * pWS->state.buf.cbBufCount + cb)
    {
        cb = pWS->state.buf.cbBuf - pWS->state.buf.cbCur - PACKETLENGTH * pWS->state.buf.cbBufCount;
    }

    while (cb) {
        size_t cl = PACKETLENGTH - pWS->state.buf.cbCur;
        if (cl > cb)
            cl = cb;
        memcpy(pv, pWS->state.buf.pbBuf + pWS->state.buf.cbCur, cl);
        pWS->state.buf.cbCur += cl;
        pv = (void *)((U8 *)pv + cl);
        cb -= cl;
        if (pWS->state.buf.cbCur == PACKETLENGTH) {
            pWS->state.buf.pbBuf = (U8 *)((void **)(pWS->state.buf.pbBuf - sizeof(void *)))[0] + sizeof(void *);
            pWS->state.buf.cbCur = 0;
            pWS->state.buf.cbBufCount++;

    /*printf ("read buffer %d: %x\n", pWS->state.buf.cbBufCount, pWS->state.buf.pbBuf); */
        }
    }

Cleanup:
    return err;
}

ERR WriteWS_List(struct WMPStream* pWS, const void* pv, size_t cb)
{
    ERR err = WMP_errSuccess;

    FailIf(pWS->state.buf.cbCur + cb < pWS->state.buf.cbCur, WMP_errBufferOverflow);
    FailIf(pWS->state.buf.cbBuf < pWS->state.buf.cbCur + cb, WMP_errBufferOverflow);

    while (cb) {
        size_t cl = PACKETLENGTH - pWS->state.buf.cbCur;
        if (cl > cb)
            cl = cb;
        memcpy(pWS->state.buf.pbBuf + pWS->state.buf.cbCur, pv, cl);
        pWS->state.buf.cbCur += cl;
        pv = (const void *)((U8 *)pv + cl);
        cb -= cl;
        if (pWS->state.buf.cbCur == PACKETLENGTH) { /* allocate next packet in list */
            U8 *pBuf  = NULL;
            void **pPtrLoc = (void **)(pWS->state.buf.pbBuf - sizeof(void *));
            Call(WMPAlloc((void **)&pBuf, PACKETLENGTH + sizeof(void *)));
            pPtrLoc[0] = (void *)pBuf;
            pWS->state.buf.pbBuf = pBuf + sizeof(void *);
            pWS->state.buf.cbBuf += PACKETLENGTH;
            memset(pBuf, 0, sizeof(void *));
            pWS->state.buf.cbCur = 0;
            pWS->state.buf.cbBufCount++;

    /*printf ("create buffer %d: %x\n", pWS->state.buf.cbBufCount, pWS->state.buf.pbBuf); */
        }
    }

Cleanup:
    return err;
}

ERR SetPosWS_List(struct WMPStream* pWS, size_t offPos)
{
    ERR err = WMP_errSuccess;

    /* get the first buffer */
    U8 *pBuf = (U8 *)(pWS + 1); /* pointer to buffer */
    pWS->state.buf.cbCur = 0;
    pWS->state.buf.cbBufCount = 0;

    while (offPos >= PACKETLENGTH && pBuf != NULL) {
        pBuf = (U8 *)(((void **)pBuf)[0]);
        offPos -= PACKETLENGTH;
        pWS->state.buf.cbBufCount++;
    }

    if (pBuf == NULL)
        goto Cleanup;

    pWS->state.buf.cbCur = offPos;
    pWS->state.buf.pbBuf = pBuf + sizeof(void *);
    /*printf ("seek buffer %d: %x\n", pWS->state.buf.cbBufCount, pWS->state.buf.pbBuf); */

Cleanup:
    return err;
}

ERR GetPosWS_List(struct WMPStream* pWS, size_t* poffPos)
{
    *poffPos = pWS->state.buf.cbCur + PACKETLENGTH * pWS->state.buf.cbBufCount;

    return WMP_errSuccess;
}

/*================================================================ */
/* Simple BitIO access functions */
/*================================================================ */
/* init SimpleBitIO */
ERR attach_SB(SimpleBitIO* pSB, struct WMPStream* pWS)
{
    pSB->pWS = pWS;
    pSB->cbRead = 0;
    pSB->bAccumulator = 0;
    pSB->cBitLeft = 0;

    return WMP_errSuccess;
}

/* extract upto 32bit from input stream */
U32 getBit32_SB(SimpleBitIO* pSB, U32 cBits)
{
    U32 rc = 0;

    while (pSB->cBitLeft < cBits)
    {
        rc <<= pSB->cBitLeft;
        rc |= pSB->bAccumulator >> (8 - pSB->cBitLeft);

        cBits -= pSB->cBitLeft;

        pSB->pWS->Read(pSB->pWS, &pSB->bAccumulator, 1);
        pSB->cbRead++;
        pSB->cBitLeft = 8;
    }

    rc <<= cBits;
    rc |= pSB->bAccumulator >> (8 - cBits);
    pSB->bAccumulator <<= cBits;
    pSB->cBitLeft -= cBits;

    return rc;
}

/* ignore input to byte boundary */
Void flushToByte_SB(SimpleBitIO* pSB)
{
    pSB->bAccumulator = 0;
    pSB->cBitLeft = 0;
}

/* return read byte count */
U32 getByteRead_SB(SimpleBitIO* pSB)
{
    return pSB->cbRead;
}

ERR detach_SB(SimpleBitIO* pSB)
{
    assert(0 == pSB->cBitLeft);
    pSB->pWS = NULL;

    return WMP_errSuccess;
}

/*================================================================ */
/* Memory access functions */
/*================================================================ */
#if (defined(JXR_WIN32_INTRINSICS) && !defined(UNDER_CE)) || (defined(UNDER_CE) && defined(_ARM_))
/* WinCE ARM and Desktop x86 */
#else
/* other platform */
#ifdef _BIG__ENDIAN_
#define jxr_bswap32(x)  (x)
#else /* _BIG__ENDIAN_ */
U32 jxr_bswap32(U32 bits)
{
    U32 r = (bits & 0xffu) << 24;
    r |= (bits << 8) & 0xff0000u;
    r |= ((bits >> 8) & 0xff00u);
    r |= ((bits >> 24) & 0xffu);

    return r;
}
#endif /* _BIG__ENDIAN_ */
#endif

U32 load4BE(void* pv)
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

#define LOAD16 load4BE

#ifdef _BIG__ENDIAN_
#define WRITESWAP_ENDIAN(a) ((a)>>16)
#else /* _BIG__ENDIAN_ */
#define WRITESWAP_ENDIAN(a)	jxr_bswap32(a)
#endif /* _BIG__ENDIAN_ */

/*================================================================ */
/* Bit I/O functions */
/*================================================================ */
Int allocateBitIOInfo(CWMImageStrCodec* pSC)
{
    U32 cNumBitIO;
    SUBBAND sbSubband = pSC->WMISCP.sbSubband;

    pSC->cSB = (sbSubband == SB_DC_ONLY ? 1 : (sbSubband == SB_NO_HIGHPASS ? 2 : (sbSubband == SB_NO_FLEXBITS ? 3 : 4)));

    /* # of additional BitIOs other than pSC->pIOHeader */
    if (!pSC->m_param.bIndexTable) { /* pure streaming mode, no index table, no additional BitIO! */
        assert (pSC->WMISCP.bfBitstreamFormat == SPATIAL && pSC->WMISCP.cNumOfSliceMinus1H + pSC->WMISCP.cNumOfSliceMinus1V == 0);
        cNumBitIO = 0;
    }
    else if(pSC->WMISCP.bfBitstreamFormat == SPATIAL)
        cNumBitIO = pSC->WMISCP.cNumOfSliceMinus1V + 1;
    else
        cNumBitIO = (pSC->WMISCP.cNumOfSliceMinus1V + 1) * pSC->cSB;

    if(cNumBitIO > MAX_TILES * 4)
        return ICERR_ERROR;

    /* allocate additional BitIos */
    if(cNumBitIO > 0){
        U32 i = 0;
        size_t cb = sizeof(BitIOInfo) * cNumBitIO + (PACKETLENGTH * 4 - 1) + PACKETLENGTH * 4 * cNumBitIO;
        U8* pb = (U8*)malloc(cb);

        if (NULL == pb) return ICERR_ERROR;
        memset(pb, 0, cb);

        pSC->m_ppBitIO = (BitIOInfo**)pb;
        pb += sizeof(BitIOInfo) * cNumBitIO;

        pb = (U8*)ALIGNUP(pb, PACKETLENGTH * 4) + PACKETLENGTH * 2;
        for (i = 0; i < cNumBitIO; ++i){
            pSC->m_ppBitIO[i] = (BitIOInfo*)pb;
            pb += PACKETLENGTH * 4;
        }

        /* allocate index table */
        if(cNumBitIO > MAX_TILES * 4 || pSC->WMISCP.cNumOfSliceMinus1H >= MAX_TILES)
            return ICERR_ERROR;
        pSC->pIndexTable = malloc(cNumBitIO * (pSC->WMISCP.cNumOfSliceMinus1H + 1) * sizeof(size_t));
        if(NULL == pSC->pIndexTable) return ICERR_ERROR;
    }

    pSC->cNumBitIO = cNumBitIO;

    return ICERR_OK;
}

Int setBitIOPointers(CWMImageStrCodec* pSC)
{
    if(pSC->cNumBitIO > 0){
        U32 i;

        for(i = 0; i <= pSC->WMISCP.cNumOfSliceMinus1V; i ++){
            CCodingContext * pContext = &pSC->m_pCodingContext[i];
            if(pSC->WMISCP.bfBitstreamFormat == SPATIAL){
                pContext->m_pIODC = pContext->m_pIOLP = pContext->m_pIOAC = pContext->m_pIOFL = pSC->m_ppBitIO[i];
            }
            else{
                U32 j = pSC->cSB;

                pContext->m_pIODC = pSC->m_ppBitIO[i * j];
                if(j > 1)
                    pContext->m_pIOLP = pSC->m_ppBitIO[i * j + 1];
                if(j > 2)
                    pContext->m_pIOAC = pSC->m_ppBitIO[i * j + 2];
                if(j > 3)
                    pContext->m_pIOFL = pSC->m_ppBitIO[i * j + 3];
            }
        }
    }
    else{ /* streamimg mode */
        CCodingContext * pContext = &pSC->m_pCodingContext[0];
        pContext->m_pIODC = pContext->m_pIOLP = pContext->m_pIOAC = pContext->m_pIOFL = pSC->pIOHeader;
    }

    return ICERR_OK;
}

Int allocateTileInfo(CWMImageStrCodec * pSC)
{
    size_t i;

    if(pSC->WMISCP.cNumOfSliceMinus1V >= MAX_TILES)
        return ICERR_ERROR;
    pSC->pTile = (CWMITile *)malloc((pSC->WMISCP.cNumOfSliceMinus1V + 1) * sizeof(CWMITile));
    if(pSC->pTile == NULL)
        return ICERR_ERROR;
    memset(pSC->pTile, 0, (pSC->WMISCP.cNumOfSliceMinus1V + 1) * sizeof(CWMITile));

    for(i = 0; i <= pSC->WMISCP.cNumOfSliceMinus1V; i ++)
        pSC->pTile[i].cNumQPHP = pSC->pTile[i].cNumQPLP = 1, pSC->pTile[i].cBitsHP = pSC->pTile[i].cBitsLP = 0;

    return ICERR_OK;
}

Void freeTileInfo(CWMImageStrCodec * pSC)
{
    size_t iTile;

    if((pSC->m_param.uQPMode & 1) != 0) /* not DC uniform */
        for(iTile = 0; iTile <= pSC->WMISCP.cNumOfSliceMinus1V; iTile ++)
            freeQuantizer(pSC->pTile[iTile].pQuantizerDC);
    else
        freeQuantizer(pSC->pTile[0].pQuantizerDC);

    if(pSC->WMISCP.sbSubband != SB_DC_ONLY) {
        if((pSC->m_param.uQPMode & 2) != 0) /* not LP uniform */
            for(iTile = 0; iTile <= pSC->WMISCP.cNumOfSliceMinus1V; iTile ++)
                freeQuantizer(pSC->pTile[iTile].pQuantizerLP);
        else
            freeQuantizer(pSC->pTile[0].pQuantizerLP);
    }

    if(pSC->WMISCP.sbSubband != SB_DC_ONLY && pSC->WMISCP.sbSubband != SB_NO_HIGHPASS) {
        if((pSC->m_param.uQPMode & 4) != 0) /* not HP uniform */
            for(iTile = 0; iTile <= pSC->WMISCP.cNumOfSliceMinus1V; iTile ++)
                freeQuantizer(pSC->pTile[iTile].pQuantizerHP);
        else
            freeQuantizer(pSC->pTile[0].pQuantizerHP);
    }

    if(pSC->pTile != NULL)
        free(pSC->pTile);
}

Int allocateQuantizer(CWMIQuantizer * pQuantizer[MAX_CHANNELS], size_t cChannel, size_t cQP)
{
    size_t iCh;
    
    if(cQP > 16 || cChannel > MAX_CHANNELS)
        return ICERR_ERROR;
    pQuantizer[0] = (CWMIQuantizer *)malloc(cQP * sizeof(CWMIQuantizer) * cChannel);
    if(pQuantizer[0] == NULL)
        return ICERR_ERROR;

    for(iCh = 1; iCh < cChannel; iCh ++)
        pQuantizer[iCh] = pQuantizer[iCh - 1] + cQP;

    return ICERR_OK;
}

Void freeQuantizer(CWMIQuantizer * pQuantizer[MAX_CHANNELS])
{
    if(pQuantizer[0] != NULL)
        free(pQuantizer[0]);
}

Void formatQuantizer(CWMIQuantizer * pQuantizer[MAX_CHANNELS], U8 cChMode, size_t cCh, size_t iPos, Bool bShiftedUV,
                     Bool bScaledArith)
{
    size_t iCh;
    
    for(iCh = 0; iCh < cCh; iCh ++){
        if(iCh > 0) {
            if(cChMode == 0) /* uniform */
                pQuantizer[iCh][iPos] = pQuantizer[0][iPos];
            else if(cChMode == 1) /* mixed */
                pQuantizer[iCh][iPos] = pQuantizer[1][iPos];
        }
        remapQP(pQuantizer[iCh] + iPos, (iCh > 0 && bShiftedUV == TRUE) ? SHIFTZERO - 1 : SHIFTZERO, bScaledArith);
    }
}

Void setUniformQuantizer(CWMImageStrCodec * pSC, size_t sb)
{
    size_t iCh, iTile;

    for(iCh = 0; iCh < pSC->m_param.cNumChannels; iCh ++)
        for(iTile = 1; iTile <= pSC->WMISCP.cNumOfSliceMinus1V; iTile ++)
            if(sb == 0) /* DC */
                pSC->pTile[iTile].pQuantizerDC[iCh] = pSC->pTile[0].pQuantizerDC[iCh];
            else if(sb == 1) /* LP */
                pSC->pTile[iTile].pQuantizerLP[iCh] = pSC->pTile[0].pQuantizerLP[iCh];
            else /* HP */
                pSC->pTile[iTile].pQuantizerHP[iCh] = pSC->pTile[0].pQuantizerHP[iCh];
}

Void useDCQuantizer(CWMImageStrCodec * pSC, size_t iTile)
{
   size_t iCh;

    for(iCh = 0; iCh < pSC->m_param.cNumChannels; iCh ++)
        pSC->pTile[iTile].pQuantizerLP[iCh][0] = *pSC->pTile[iTile].pQuantizerDC[iCh];
}

Void useLPQuantizer(CWMImageStrCodec * pSC, size_t cQP, size_t iTile)
{
   size_t iCh, iQP;

    for(iCh = 0; iCh < pSC->m_param.cNumChannels; iCh ++)
        for(iQP = 0; iQP < cQP; iQP ++)
            pSC->pTile[iTile].pQuantizerHP[iCh][iQP] = pSC->pTile[iTile].pQuantizerLP[iCh][iQP];
}

U8 dquantBits(U8 cQP)
{
    return (cQP < 2 ? 0 : (cQP < 4 ? 1 : (cQP < 6 ? 2 : (cQP < 10 ? 3 : 4))));
}

#ifndef ARMOPT_BITIO
U32 peekBit16(BitIOInfo* pIO, U32 cBits)
{
    PEEKBIT16(pIO, cBits);
}

U32 flushBit16(BitIOInfo* pIO, U32 cBits)
{
    FLUSHBIT16(pIO, cBits);
}

U32 getBit16(BitIOInfo* pIO, U32 cBits)
{
    U32 uiRet = peekBit16(pIO, cBits);
    flushBit16(pIO, cBits);

    return uiRet;
}

U32 getBool16(BitIOInfo* pIO)
{
    U32 uiRet = peekBit16(pIO, 1);
    flushBit16(pIO, 1);
    return uiRet;
}

/** this function returns cBits if zero is read, or a signed value if first cBits are not all zero **/
I32 getBit16s(BitIOInfo* pIO, U32 cBits)
{
    U32 uiRet = peekBit16(pIO, cBits + 1);
    if (uiRet < 2) {
        flushBit16(pIO, cBits);
        return 0;
    }
    else {
        flushBit16(pIO, cBits + 1);
        if (uiRet & 1)
            return (-(I32)(uiRet >> 1));
        else
            return (I32)(uiRet >> 1);
    }
}

U32 getBit32(BitIOInfo* pIO, U32 cBits)
{
    U32 uiRet = 0;

    assert(0 <= (I32)cBits && cBits <= 32);

    if (16 < cBits)
    {
        uiRet = getBit16(pIO, 16);
        cBits -= 16;
        uiRet <<= cBits;
    }

    uiRet |= getBit16(pIO, cBits);

    return uiRet;
}

U32 flushToByte(BitIOInfo* pIO)
{
    return flushBit16(pIO, (16 - pIO->cBitsUsed) & 7);
}
#endif  /* ARMOPT_BITIO */

/*---------------------------------------------------------------- */
Void putBit16z(BitIOInfo* pIO, U32 uiBits, U32 cBits)
{
    assert(cBits <= 16);
    assert(0 == uiBits >> cBits);

    pIO->uiAccumulator = (pIO->uiAccumulator << cBits) | uiBits;
    pIO->cBitsUsed += cBits;

    *(U16*)pIO->pbCurrent = (U16)WRITESWAP_ENDIAN(pIO->uiAccumulator << (32 - pIO->cBitsUsed));

    pIO->pbCurrent = MASKPTR(pIO->pbCurrent + ((pIO->cBitsUsed >> 3) & 2), pIO->iMask);
    pIO->cBitsUsed &= 16 - 1;
}

Void putBit16(BitIOInfo* pIO, U32 uiBits, U32 cBits)
{
    assert(cBits <= 16);

    uiBits &= ~(-1 << cBits);
    putBit16z(pIO, uiBits, cBits);
}

Void putBit32(BitIOInfo* pIO, U32 uiBits, U32 cBits)
{
    assert(0 <= (I32)cBits && cBits <= 32);

    if (16 < cBits)
    {
        putBit16(pIO, uiBits >> (cBits - 16), 16);
        cBits -= 16;
    }

    putBit16(pIO, uiBits, cBits);
}

Void fillToByte(BitIOInfo* pIO)
{
    putBit16z(pIO, 0, (16 - pIO->cBitsUsed) & 7);
}

/*---------------------------------------------------------------- */
U32 getBit16_S(CWMImageStrCodec* pSC, BitIOInfo* pIO, U32 cBits)
{
    U32 rc = getBit16(pIO, cBits);
    readIS_L1(pSC, pIO);

    return rc;
}

U32 putBit16_S(CWMImageStrCodec* pSC, BitIOInfo* pIO, U32 uiBits, U32 cBits)
{
    putBit16(pIO, uiBits, cBits);
    writeIS_L1(pSC, pIO);

    return 0;
}


/*---------------------------------------------------------------- */
/* Query buffered data size held in BitIOInfo */
/* Write() for Enc, Read() for Dec */
/*---------------------------------------------------------------- */
U32 getSizeRead(BitIOInfo* pIO)
{
    return (U32)(UINTPTR_T)(pIO->pbStart + PACKETLENGTH * 2 - pIO->pbCurrent) - pIO->cBitsUsed / 8;
}

U32 getSizeWrite(BitIOInfo* pIO)
{
    return (U32)(UINTPTR_T)(pIO->pbCurrent + (pIO->pbStart <= pIO->pbCurrent ? 0 : PACKETLENGTH * 2) - pIO->pbStart) + pIO->cBitsUsed / 8;
}

/*---------------------------------------------------------------- */
/* Query stream offset from attached BitIO object for dec */
/*---------------------------------------------------------------- */
U32 getPosRead(BitIOInfo* pIO)
{
    size_t cbCached = (pIO->pbStart + PACKETLENGTH * 2 - pIO->pbCurrent) - pIO->cBitsUsed / 8;
    return (U32)(pIO->offRef - cbCached);
}

/*================================================================ */
/* Block I/O functions */
/*================================================================ */
#ifndef ARMOPT_BITIO
ERR attachISRead(BitIOInfo* pIO, struct WMPStream* pWS, CWMImageStrCodec* pSC)
{
    UNREFERENCED_PARAMETER( pSC );

    pWS->GetPos(pWS, &pIO->offRef);

    pIO->pbStart = (U8*)pIO - PACKETLENGTH * 2;
    pIO->pbCurrent = pIO->pbStart;

    PERFTIMER_STOP(pSC->m_fMeasurePerf, pSC->m_ptEncDecPerf);
    pWS->SetPos(pWS, pIO->offRef);
    pWS->Read(pWS, pIO->pbStart, PACKETLENGTH * 2);
    PERFTIMER_START(pSC->m_fMeasurePerf, pSC->m_ptEncDecPerf);
    pIO->offRef += PACKETLENGTH * 2;

    pIO->uiAccumulator = load4BE(pIO->pbStart);
    
    pIO->cBitsUsed = 0;
    pIO->iMask = ~(PACKETLENGTH * 2);
    pIO->iMask &= ~1;

    pIO->pWS = pWS;
    return WMP_errSuccess;
}

ERR readIS(CWMImageStrCodec* pSC, BitIOInfo* pIO)
{
    ERR err = WMP_errSuccess;

    UNREFERENCED_PARAMETER( pSC );

    if (PACKET1(pIO->pbStart, pIO->pbCurrent, PACKETLENGTH))
    {
        struct WMPStream *pWS = pIO->pWS;

        PERFTIMER_STOP(pSC->m_fMeasurePerf, pSC->m_ptEncDecPerf);
        /*Call(0 != pIO->pWS->Read(pIO->pWS, pIO->pbStart, PACKETLENGTH)); */
        /* TODO: add error checking code */
        pWS->SetPos(pWS, pIO->offRef);
        pWS->Read(pWS, pIO->pbStart, PACKETLENGTH);
        pIO->offRef += PACKETLENGTH;
        PERFTIMER_START(pSC->m_fMeasurePerf, pSC->m_ptEncDecPerf);

        /* make shadow copy for first 4B */
        pIO->uiShadow = *(U32*)pIO->pbStart;

        /* reposition pbPacket pointer */
        pIO->pbStart = MASKPTR(pIO->pbStart + PACKETLENGTH, pIO->iMask);
    }

/*Cleanup: */
    return err;
}

ERR detachISRead(CWMImageStrCodec* pSC, BitIOInfo* pIO)
{
    ERR err = WMP_errSuccess;

    struct WMPStream* pWS = pIO->pWS;
    size_t cbRemain = 0;

    /* we can ONLY detach IStream at byte boundary */
    flushToByte(pIO);
    assert(0 == (pIO->cBitsUsed % 8));
    Call(readIS_L1(pSC, pIO));

    /* set stream to right offset, undo buffering */
    cbRemain = (pIO->pbStart + PACKETLENGTH * 2) - (pIO->pbCurrent + pIO->cBitsUsed / 8);
    pWS->SetPos(pWS, pIO->offRef - cbRemain);

    pIO->pWS = NULL;
Cleanup:
    return err;
}
#endif  /* ARMOPT_BITIO */

/*---------------------------------------------------------------- */
ERR attachISWrite(BitIOInfo* pIO, struct WMPStream* pWS)
{
    pWS->GetPos(pWS, &pIO->offRef);

    pIO->pbStart = (U8*)pIO - PACKETLENGTH * 2;
    pIO->pbCurrent = pIO->pbStart;

    pIO->uiAccumulator = 0;
    pIO->cBitsUsed = 0;
    pIO->iMask = ~(PACKETLENGTH * 2);

    pIO->pWS = pWS;
    return WMP_errSuccess;
}

/* write out packet if we have >=1 packet data filled */
ERR writeIS(CWMImageStrCodec* pSC, BitIOInfo* pIO)
{
    ERR err = WMP_errSuccess;

    UNREFERENCED_PARAMETER( pSC );

    if (PACKET1(pIO->pbStart, pIO->pbCurrent, PACKETLENGTH))
    {
        PERFTIMER_STOP(pSC->m_fMeasurePerf, pSC->m_ptEncDecPerf);
        err = pIO->pWS->Write(pIO->pWS, pIO->pbStart, PACKETLENGTH);
        PERFTIMER_START(pSC->m_fMeasurePerf, pSC->m_ptEncDecPerf);
        Call(err);

        /* reposition pbStart pointer */
        pIO->pbStart = MASKPTR(pIO->pbStart + PACKETLENGTH, pIO->iMask);
    }

Cleanup:
    return err;
}

/* write out partially filled buffer and detach bitIO from IStream */
ERR detachISWrite(CWMImageStrCodec* pSC, BitIOInfo* pIO)
{
    ERR err = WMP_errSuccess;

    /* we can ONLY detach IStream at byte boundary */
    assert(0 == (pIO->cBitsUsed % 8));
    Call(writeIS_L1(pSC, pIO));

    PERFTIMER_STOP(pSC->m_fMeasurePerf, pSC->m_ptEncDecPerf);
    err = pIO->pWS->Write(pIO->pWS, pIO->pbStart, pIO->pbCurrent + pIO->cBitsUsed / 8 - pIO->pbStart);
    PERFTIMER_START(pSC->m_fMeasurePerf, pSC->m_ptEncDecPerf);
    Call(err);

    pIO->pWS = NULL;
Cleanup:
    return err;
}

/*========================= */
/* Performance Measurement */
/*========================= */
#ifndef DISABLE_PERF_MEASUREMENT

void OutputIndivPerfTimer(struct PERFTIMERSTATE *pPerfTimer,
                          char *pszTimerName,
                          char *pszDescription,
                          float fltMegaPixels)
{
    PERFTIMERRESULTS    rResults;
    Bool                fResult;

    fResult = FALSE;
    printf("%s (%s): ", pszTimerName, pszDescription);
    if (pPerfTimer)
    {
        fResult = PerfTimerGetResults(pPerfTimer, &rResults);
        if (fResult)
        {
            printf("%.3f milliseconds, %.6f MP/sec\n", (float)rResults.iElapsedTime / 1000000,
                1000000000 * fltMegaPixels / rResults.iElapsedTime);
            if (rResults.iZeroTimeIntervals > 0)
            {
                printf("   *** WARNING: %d time intervals were measured as zero. "
                    "This perf timer has insufficient precision!\n\n",
                    (int) rResults.iZeroTimeIntervals);
            }
        }
    }
    if (FALSE == fResult)
        printf("Results not available!\n");
}


void OutputPerfTimerReport(CWMImageStrCodec *pState)
{
    float               fltMegaPixels;

    assert(pState->m_fMeasurePerf);

    printf("***************************************************************************\n");
    printf("* Perf Report\n");
    printf("***************************************************************************\n\n");
    
    fltMegaPixels = (float)pState->WMII.cWidth * pState->WMII.cHeight / 1000000;
    printf("Image Width = %d, Height = %d, total MegaPixels = %.1f MP\n",
        (int) pState->WMII.cWidth, (int) pState->WMII.cHeight, fltMegaPixels);

    OutputIndivPerfTimer(pState->m_ptEncDecPerf, "m_ptEncDecPerf", "excl I/O", fltMegaPixels);
    OutputIndivPerfTimer(pState->m_ptEndToEndPerf, "m_ptEndToEndPerf", "incl I/O", fltMegaPixels);
}

#endif /* DISABLE_PERF_MEASUREMENT */

#undef TRACE_MALLOC
#undef TRACE_NEW
#undef TRACE_HEAP
#undef LOAD16
#undef WRITESWAP_ENDIAN
/* ---- strPredQuant.c ---- */
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

#define ORIENT_WEIGHT 4

/* reciprocal (pMantissa, exponent) lookup table */
typedef struct tagQPManExp
{
    int iMan;
    int iExp;
} QPManExp;

static QPManExp gs_QPRecipTable[32] = {
    {0x0, 0}, /* 0, invalid */
    {0x0, 0}, /* 1, lossless */
    {0x0, 1}, /* 2 */
    {0xaaaaaaab, 1},
    {0x0, 2}, /* 4 */
    {0xcccccccd, 2},
    {0xaaaaaaab, 2},
    {0x92492493, 2},
    {0x0, 3}, /* 8 */
    {0xe38e38e4, 3},
    {0xcccccccd, 3},
    {0xba2e8ba3, 3},
    {0xaaaaaaab, 3},
    {0x9d89d89e, 3},
    {0x92492493, 3},
    {0x88888889, 3},
    {0x0, 4}, /* 16 */
    {0xf0f0f0f1, 4},
    {0xe38e38e4, 4},
    {0xd79435e6, 4},
    {0xcccccccd, 4},
    {0xc30c30c4, 4},
    {0xba2e8ba3, 4},
    {0xb21642c9, 4},
    {0xaaaaaaab, 4},
    {0xa3d70a3e, 4},
    {0x9d89d89e, 4},
    {0x97b425ee, 4},
    {0x92492493, 4},
    {0x8d3dcb09, 4},
    {0x88888889, 4},
    {0x84210843, 4},
};

/*************************************************************************
    QPRemapping
*************************************************************************/

Void remapQP(CWMIQuantizer * pQP, I32 iShift, Bool bScaledArith)
{
    U8 uiQPIndex = pQP->iIndex;

    if(uiQPIndex == 0) /* Lossless mode! */
        pQP->iQP = 1, pQP->iMan = pQP->iExp = pQP->iOffset = 0;
    else if (!bScaledArith) {
        I32 man = 0, exp = 0;
        const I32 ciShift = SHIFTZERO - (SHIFTZERO + QPFRACBITS); /* == -QPFRACBITS */

        if (pQP->iIndex < 32)
            man = (pQP->iIndex + 3) >> 2, exp = ciShift + 2;
        else if (pQP->iIndex < 48)
            man = (16 + (pQP->iIndex & 0xf) + 1) >> 1, exp = ((pQP->iIndex >> 4) - 1) + 1 + ciShift;
        else
            man = 16 + (pQP->iIndex & 0xf), exp = ((pQP->iIndex >> 4) - 1) + ciShift;

        pQP->iQP = man << exp;
        pQP->iMan = gs_QPRecipTable[man].iMan;
        pQP->iExp = gs_QPRecipTable[man].iExp + exp;
        pQP->iOffset = ((pQP->iQP * 3 + 1) >> 3);
#if defined(WMP_OPT_QT)
        pQP->f1_QP = 1.0f / pQP->iQP;
        pQP->d1_QP = 1.0 / pQP->iQP;
#endif    
    }
    else {
        I32 man = 0, exp = 0;

        if(pQP->iIndex < 16)
            man = pQP->iIndex, exp = iShift;
        else
            man = 16 + (pQP->iIndex & 0xf), exp = ((pQP->iIndex >> 4) - 1) + iShift;
        
        pQP->iQP = man << exp;
        pQP->iMan = gs_QPRecipTable[man].iMan;
        pQP->iExp = gs_QPRecipTable[man].iExp + exp;
        pQP->iOffset = ((pQP->iQP * 3 + 1) >> 3);
#if defined(WMP_OPT_QT)
        pQP->f1_QP = 1.0f / pQP->iQP;
        pQP->d1_QP = 1.0 / pQP->iQP;
#endif    
    }
}

/* allocate PredInfo buffers */
Int allocatePredInfo(CWMImageStrCodec *pSC)
{
    size_t i, j;
    /* COLORFORMAT cf = pSC->m_param.cfColorFormat; */
    const size_t mbWidth = pSC->cmbWidth;
    const size_t iChannels = pSC->m_param.cNumChannels;
    CWMIPredInfo* pMemory;
    Bool b32Bit = sizeof(size_t) == 4;
    
    if(b32Bit) /* integer overlow/underflow check for 32-bit system */
        if(((mbWidth >> 16) * iChannels * 2 * sizeof(CWMIPredInfo)) & 0xffff0000)
            return ICERR_ERROR;    
    pMemory = (CWMIPredInfo *)malloc(mbWidth * iChannels * 2 * sizeof(CWMIPredInfo));
    if (pMemory == NULL)
        return ICERR_ERROR;

    pSC->pPredInfoMemory = pMemory;
    for(i = 0; i < iChannels; i ++){
        pSC->PredInfo[i] = pMemory;
        pMemory += mbWidth;
        pSC->PredInfoPrevRow[i] = pMemory;
        pMemory += mbWidth;
        
        for(j = 0; j < mbWidth; j ++){
            pSC->PredInfo[i][j].piAD = pSC->PredInfo[i][j].iAD;
            pSC->PredInfoPrevRow[i][j].piAD = pSC->PredInfoPrevRow[i][j].iAD;
        }
    }
    
    return ICERR_OK;
}

/* clear PredInfo buffers */
Void freePredInfo(CWMImageStrCodec *pSC)
{
    if (pSC->pPredInfoMemory)
        free (pSC->pPredInfoMemory);
    pSC->pPredInfoMemory = NULL;
}

/* get AC prediction mode: 0(from left) 1(from top) 2(none) */
Int getACPredMode(CWMIMBInfo * pMBInfo, COLORFORMAT cf)
{
    /*Int blkIdx = (cf == Y_ONLY ? 16 : (cf == YUV_420 ? 24 : (cf == YUV_422 ? 32 : 48))); */
    PixelI * pCoeffs = pMBInfo->iBlockDC[0];
    Int StrH = abs(pCoeffs[1]) + abs(pCoeffs[2]) + abs(pCoeffs[3]);
    Int StrV = abs(pCoeffs[4]) + abs(pCoeffs[8]) + abs(pCoeffs[12]);

    if(cf != Y_ONLY && cf != NCOMPONENT){
        PixelI * pCoeffsU = pMBInfo->iBlockDC[1];
        PixelI * pCoeffsV = pMBInfo->iBlockDC[2];

        StrH += abs(pCoeffsU[1]) + abs(pCoeffsV[1]);
        if(cf == YUV_420){
            StrV += abs(pCoeffsU[2]) + abs(pCoeffsV[2]);
        }
        else if (cf == YUV_422){
            StrV += abs(pCoeffsU[2]) + abs(pCoeffsV[2]) + abs(pCoeffsU[6]) + abs(pCoeffsV[6]);
            StrH += abs(pCoeffsU[5]) + abs(pCoeffsV[5]);
        }
        else { /* YUV_444 or CMYK */
            StrV += abs(pCoeffsU[4]) + abs(pCoeffsV[4]);
        }
    }

    return (StrH * ORIENT_WEIGHT < StrV ? 1 : (StrV * ORIENT_WEIGHT < StrH ? 0 : 2));
}

/* get DCAC prediction mode: 0(from left) 1(from top) 2(none) */
Int getDCACPredMode(CWMImageStrCodec *pSC, size_t mbX)
{
    Int iDCMode, iADMode = 2;  /* DC: 0(left) 1(top) 2(mean) 3(no) */
                               /* AD: 0(left) 1(top) 2(no) */

    if(pSC->m_bCtxLeft && pSC->m_bCtxTop){ /* topleft corner, no prediction */
        iDCMode = 3;
    }
    else if(pSC->m_bCtxLeft){
        iDCMode = 1; /* left column, predict from top */
    }
    else if(pSC->m_bCtxTop){
        iDCMode = 0; /* top row, predict from left */
    }
    else{
        COLORFORMAT cf = pSC->m_param.cfColorFormat;
        Int iL = pSC->PredInfo[0][mbX - 1].iDC, iT = pSC->PredInfoPrevRow[0][mbX].iDC, iTL = pSC->PredInfoPrevRow[0][mbX - 1].iDC;
        Int StrH, StrV;

        if(cf == Y_ONLY || cf == NCOMPONENT){ /* CMYK uses YUV metric */
            StrH = abs(iTL - iL);
            StrV = abs(iTL - iT);
        }
        else{
            CWMIPredInfo * pTU = pSC->PredInfoPrevRow[1] + mbX, * pLU = pSC->PredInfo[1] + mbX - 1, * pTLU = pTU - 1;
            CWMIPredInfo * pTV = pSC->PredInfoPrevRow[2] + mbX, * pLV = pSC->PredInfo[2] + mbX - 1, * pTLV = pTV - 1;
            Int scale = (cf == YUV_420 ? 8 : (cf == YUV_422 ? 4 : 2));

            StrH = abs(iTL - iL) * scale + abs(pTLU->iDC - pLU->iDC) + abs(pTLV->iDC - pLV->iDC);
            StrV = abs(iTL - iT) * scale + abs(pTLU->iDC - pTU->iDC) + abs(pTLV->iDC - pTV->iDC);
        }
        iDCMode = (StrH * ORIENT_WEIGHT < StrV ? 1 : (StrV * ORIENT_WEIGHT < StrH ? 0 : 2));
    }

    if(iDCMode == 1 && pSC->MBInfo.iQIndexLP == pSC->PredInfoPrevRow[0][mbX].iQPIndex)
        iADMode = 1;
    if(iDCMode == 0 && pSC->MBInfo.iQIndexLP == pSC->PredInfo[0][mbX - 1].iQPIndex)
        iADMode = 0;

    return (iDCMode + (iADMode << 2));
}

Void copyAC(PixelI * src, PixelI * dst)
{
    /* first row of ACs */
    dst[0] = src[1];
    dst[1] = src[2];
    dst[2] = src[3];

    /* first column of ACs */
    dst[3] = src[4];
    dst[4] = src[8];
    dst[5] = src[12];
}

/* info of current MB to be saved for future prediction */
Void updatePredInfo(CWMImageStrCodec *pSC, CWMIMBInfo * pMBInfo, size_t mbX, COLORFORMAT cf)
{
    CWMIPredInfo *pPredInfo;
    PixelI * p;
    Int  i, iChannels = (cf == YUV_420 || cf == YUV_422) ? 1 : (Int) pSC->m_param.cNumChannels;

    for(i = 0; i < iChannels; i ++){
        pPredInfo = pSC->PredInfo[i] + mbX;
        p = pMBInfo->iBlockDC[i];/*[dcBlkIdx + i]; */

        /* DC of DC block */
        pPredInfo->iDC = p[0];

        /* QP Index */
        pPredInfo->iQPIndex = pMBInfo->iQIndexLP;
        
        /* first row and first column of ACs of DC block */
        copyAC(p, pPredInfo->piAD);
    }

    if(cf == YUV_420){ /* 420 UV channels */
        for(i = 1U; i < 3U; i ++){
            pPredInfo = pSC->PredInfo[i] + mbX;
            p = pMBInfo->iBlockDC[i];/*[dcBlkIdx + i]; */
            
            /* DC of DC block */
            pPredInfo->iDC = p[0];
            
            /* QP Index */
            pPredInfo->iQPIndex = pMBInfo->iQIndexLP;
            /* first row and first column of ACs of DC block */
            pPredInfo->piAD[0] = p[1];
            pPredInfo->piAD[1] = p[2];
        }
    }
    else if(cf == YUV_422){ /* 420 UV channels */
        for(i = 1U; i < 3U; i ++){
            pPredInfo = pSC->PredInfo[i] + mbX;

            /* QP Index */
            pPredInfo->iQPIndex = pMBInfo->iQIndexLP;

            p = pMBInfo->iBlockDC[i];/*[dcBlkIdx + i]; */

            /* DC of DC block */
            pPredInfo->iDC = p[0];
            
            /* first row and first column of ACs of first DC block */
            pPredInfo->piAD[0] = p[1];
            pPredInfo->piAD[1] = p[2];
            /* first row and first column of ACs of second DC block */
            pPredInfo->piAD[2] = p[5];
            pPredInfo->piAD[3] = p[6];
            pPredInfo->piAD[4] = p[4]; /*AC of 1D HT!!! */
        }
    }
}

#undef ORIENT_WEIGHT
/* ---- strTransform.c ---- */
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

/** need to swap b and c **/
/** rounding behavior: [0 0 0 0] <-> [+ - - -]
    [+ + + +] <-> [+3/4 - - -]
    [- - - -] <-> [- - - -] **/
Void strDCT2x2dn(PixelI *pa, PixelI *pb, PixelI *pc, PixelI *pd)
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

    *pa = a;
    *pb = b;
    *pc = c;
    *pd = d;
}

Void strDCT2x2up(PixelI *pa, PixelI *pb, PixelI *pc, PixelI *pd)
{
    PixelI a, b, c, d, C, t;
    a = *pa;
    b = *pb;
    C = *pc;
    d = *pd;
  
    a += d;
    b -= C;
    t = ((a - b + 1) >> 1);
    c = t - d;
    d = t - C;
    a -= d;
    b += c;

    *pa = a;
    *pb = b;
    *pc = c;
    *pd = d;
}

Void FOURBUTTERFLY_HARDCODED1(PixelI *p)
{
    strDCT2x2dn(&p[0], &p[4], &p[8], &p[12]);
    strDCT2x2dn(&p[1], &p[5], &p[9], &p[13]);
    strDCT2x2dn(&p[2], &p[6], &p[10], &p[14]);
    strDCT2x2dn(&p[3], &p[7], &p[11], &p[15]);
}

