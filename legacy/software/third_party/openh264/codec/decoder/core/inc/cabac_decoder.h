/*!
 * \copy
 *     Copyright (c)  2009-2013, Cisco Systems
 *     All rights reserved.
 *
 *     Redistribution and use in source and binary forms, with or without
 *     modification, are permitted provided that the following conditions
 *     are met:
 *
 *        * Redistributions of source code must retain the above copyright
 *          notice, this list of conditions and the following disclaimer.
 *
 *        * Redistributions in binary form must reproduce the above copyright
 *          notice, this list of conditions and the following disclaimer in
 *          the documentation and/or other materials provided with the
 *          distribution.
 *
 *     THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *     "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *     LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *     FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *     COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *     INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *     BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *     LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *     CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *     LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *     ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *     POSSIBILITY OF SUCH DAMAGE.
 *
 * \file    cabac_decoder.h
 *
 * \brief   Interfaces introduced for cabac decoder
 *
 * \date    10/10/2014 Created
 *
 *************************************************************************************
 */
#ifndef WELS_CABAC_DECODER_H__
#define WELS_CABAC_DECODER_H__

#include "decoder_context.h"
#include "error_code.h"
#include "wels_common_defs.h"
namespace WelsDec {
static const uint8_t g_kRenormTable256[256] = {
  6, 6, 6, 6, 6, 6, 6, 6,
  5, 5, 5, 5, 5, 5, 5, 5,
  4, 4, 4, 4, 4, 4, 4, 4,
  4, 4, 4, 4, 4, 4, 4, 4,
  3, 3, 3, 3, 3, 3, 3, 3,
  3, 3, 3, 3, 3, 3, 3, 3,
  3, 3, 3, 3, 3, 3, 3, 3,
  3, 3, 3, 3, 3, 3, 3, 3,
  2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1
};


//1. CABAC context initialization
void WelsCabacGlobalInit (PWelsDecoderContext pCabacCtx);
void WelsCabacContextInit (PWelsDecoderContext  pCtx, uint8_t eSliceType, int32_t iCabacInitIdc, int32_t iQp);

//2. decoding Engine initialization
int32_t InitCabacDecEngineFromBS (PWelsCabacDecEngine pDecEngine, SBitStringAux* pBsAux);
void RestoreCabacDecEngineToBS (PWelsCabacDecEngine pDecEngine, SBitStringAux* pBsAux);
//3. actual decoding
int32_t Read32BitsCabac (PWelsCabacDecEngine pDecEngine, uint32_t& uiValue, int32_t& iNumBitsRead);
int32_t DecodeBinCabac (PWelsCabacDecEngine pDecEngine, PWelsCabacCtx pBinCtx, uint32_t& uiBit);
int32_t DecodeBypassCabac (PWelsCabacDecEngine pDecEngine, uint32_t& uiBinVal);
int32_t  DecodeTerminateCabac (PWelsCabacDecEngine pDecEngine, uint32_t& uiBinVal);

//4. unary parsing
int32_t DecodeUnaryBinCabac (PWelsCabacDecEngine pDecEngine, PWelsCabacCtx pBinCtx, int32_t iCtxOffset,
                             uint32_t& uiSymVal);

//5. EXGk parsing
int32_t DecodeExpBypassCabac (PWelsCabacDecEngine pDecEngine, int32_t iCount, uint32_t& uiSymVal);
uint32_t DecodeUEGLevelCabac (PWelsCabacDecEngine pDecEngine, PWelsCabacCtx pBinCtx, uint32_t& uiBinVal);
int32_t DecodeUEGMvCabac (PWelsCabacDecEngine pDecEngine, PWelsCabacCtx pBinCtx, uint32_t iMaxC,  uint32_t& uiCode);

#define WELS_CABAC_HALF    0x01FE
#define WELS_CABAC_QUARTER 0x0100
#define WELS_CABAC_FALSE_RETURN(iErrorInfo) \
if(iErrorInfo) { \
  return iErrorInfo; \
}

#ifdef WELS_CABAC_INLINE_HOT
static inline int32_t Read32BitsCabacFast (PWelsCabacDecEngine pDecEngine, uint32_t& uiValue,
                                           int32_t& iNumBitsRead) {
  intX_t iLeftBytes = pDecEngine->pBuffEnd - pDecEngine->pBuffCurr;
  iNumBitsRead = 0;
  uiValue = 0;
  if (iLeftBytes <= 0) {
    return GENERATE_ERROR_NO (ERR_LEVEL_MB_DATA, ERR_CABAC_NO_BS_TO_READ);
  }
  if (iLeftBytes >= 4) {
    uiValue = ((pDecEngine->pBuffCurr[0] << 24) | (pDecEngine->pBuffCurr[1] << 16) |
               (pDecEngine->pBuffCurr[2] << 8) | pDecEngine->pBuffCurr[3]);
    pDecEngine->pBuffCurr += 4;
    iNumBitsRead = 32;
    return ERR_NONE;
  }
  if (iLeftBytes == 3) {
    uiValue = ((pDecEngine->pBuffCurr[0] << 16) | (pDecEngine->pBuffCurr[1] << 8) |
               pDecEngine->pBuffCurr[2]);
    pDecEngine->pBuffCurr += 3;
    iNumBitsRead = 24;
    return ERR_NONE;
  }
  if (iLeftBytes == 2) {
    uiValue = ((pDecEngine->pBuffCurr[0] << 8) | pDecEngine->pBuffCurr[1]);
    pDecEngine->pBuffCurr += 2;
    iNumBitsRead = 16;
    return ERR_NONE;
  }
  uiValue = pDecEngine->pBuffCurr[0];
  pDecEngine->pBuffCurr += 1;
  iNumBitsRead = 8;
  return ERR_NONE;
}

static inline int32_t DecodeBinCabacFast (PWelsCabacDecEngine pDecEngine, PWelsCabacCtx pBinCtx,
                                          uint32_t& uiBinVal) {
  uint32_t uiState = pBinCtx->uiState;
  uiBinVal = pBinCtx->uiMPS;
  uint64_t uiOffset = pDecEngine->uiOffset;
  uint64_t uiRange = pDecEngine->uiRange;
  int32_t iRenorm = 1;
  uint32_t uiRangeLPS = g_kuiCabacRangeLps[uiState][ (uiRange >> 6) & 0x03];
  uiRange -= uiRangeLPS;
  if (uiOffset >= (uiRange << pDecEngine->iBitsLeft)) {
    uiOffset -= (uiRange << pDecEngine->iBitsLeft);
    uiBinVal ^= 0x0001;
    if (!uiState) {
      pBinCtx->uiMPS ^= 0x01;
    }
    pBinCtx->uiState = g_kuiStateTransTable[uiState][0];
    iRenorm = g_kRenormTable256[uiRangeLPS];
    uiRange = (uiRangeLPS << iRenorm);
  } else {
    pBinCtx->uiState = g_kuiStateTransTable[uiState][1];
    if (uiRange >= WELS_CABAC_QUARTER) {
      pDecEngine->uiRange = uiRange;
      return ERR_NONE;
    }
    uiRange <<= 1;
  }
  pDecEngine->uiRange = uiRange;
  pDecEngine->iBitsLeft -= iRenorm;
  if (pDecEngine->iBitsLeft > 0) {
    pDecEngine->uiOffset = uiOffset;
    return ERR_NONE;
  }
  uint32_t uiVal = 0;
  int32_t iNumBitsRead = 0;
  int32_t iErrorInfo = Read32BitsCabacFast (pDecEngine, uiVal, iNumBitsRead);
  pDecEngine->uiOffset = (uiOffset << iNumBitsRead) | uiVal;
  pDecEngine->iBitsLeft += iNumBitsRead;
  if (iErrorInfo && pDecEngine->iBitsLeft < 0) {
    return iErrorInfo;
  }
  return ERR_NONE;
}

static inline int32_t DecodeBypassCabacFast (PWelsCabacDecEngine pDecEngine, uint32_t& uiBinVal) {
  int32_t iBitsLeft = pDecEngine->iBitsLeft;
  uint64_t uiOffset = pDecEngine->uiOffset;
  if (iBitsLeft <= 0) {
    uint32_t uiVal = 0;
    int32_t iNumBitsRead = 0;
    int32_t iErrorInfo = Read32BitsCabacFast (pDecEngine, uiVal, iNumBitsRead);
    uiOffset = (uiOffset << iNumBitsRead) | uiVal;
    iBitsLeft = iNumBitsRead;
    if (iErrorInfo && iBitsLeft == 0) {
      return iErrorInfo;
    }
  }
  --iBitsLeft;
  uint64_t uiRangeValue = (pDecEngine->uiRange << iBitsLeft);
  pDecEngine->iBitsLeft = iBitsLeft;
  if (uiOffset >= uiRangeValue) {
    pDecEngine->uiOffset = uiOffset - uiRangeValue;
    uiBinVal = 1;
    return ERR_NONE;
  }
  pDecEngine->uiOffset = uiOffset;
  uiBinVal = 0;
  return ERR_NONE;
}

static inline int32_t DecodeTerminateCabacFast (PWelsCabacDecEngine pDecEngine, uint32_t& uiBinVal) {
  uint64_t uiRange = pDecEngine->uiRange - 2;
  uint64_t uiOffset = pDecEngine->uiOffset;
  if (uiOffset >= (uiRange << pDecEngine->iBitsLeft)) {
    uiBinVal = 1;
    return ERR_NONE;
  }
  uiBinVal = 0;
  if (uiRange < WELS_CABAC_QUARTER) {
    int32_t iRenorm = g_kRenormTable256[uiRange];
    pDecEngine->uiRange = (uiRange << iRenorm);
    pDecEngine->iBitsLeft -= iRenorm;
    if (pDecEngine->iBitsLeft < 0) {
      uint32_t uiVal = 0;
      int32_t iNumBitsRead = 0;
      int32_t iErrorInfo = Read32BitsCabacFast (pDecEngine, uiVal, iNumBitsRead);
      pDecEngine->uiOffset = (pDecEngine->uiOffset << iNumBitsRead) | uiVal;
      pDecEngine->iBitsLeft += iNumBitsRead;
      if (iErrorInfo && pDecEngine->iBitsLeft < 0) {
        return iErrorInfo;
      }
    }
  } else {
    pDecEngine->uiRange = uiRange;
  }
  return ERR_NONE;
}

static inline int32_t DecodeUnaryBinCabacFast (PWelsCabacDecEngine pDecEngine, PWelsCabacCtx pBinCtx,
                                              int32_t iCtxOffset, uint32_t& uiSymVal) {
  uiSymVal = 0;
  int32_t iErrorInfo = DecodeBinCabacFast (pDecEngine, pBinCtx, uiSymVal);
  if (iErrorInfo || uiSymVal == 0) {
    return iErrorInfo;
  }
  uint32_t uiCode = 0;
  pBinCtx += iCtxOffset;
  uiSymVal = 0;
  do {
    iErrorInfo = DecodeBinCabacFast (pDecEngine, pBinCtx, uiCode);
    if (iErrorInfo) {
      return iErrorInfo;
    }
    ++uiSymVal;
  } while (uiCode != 0);
  return ERR_NONE;
}

static inline int32_t DecodeExpBypassCabacFast (PWelsCabacDecEngine pDecEngine, int32_t iCount,
                                                uint32_t& uiSymVal) {
  uint32_t uiCode = 0;
  int32_t iSymTmp = 0;
  int32_t iSymTmp2 = 0;
  uiSymVal = 0;
  do {
    int32_t iErrorInfo = DecodeBypassCabacFast (pDecEngine, uiCode);
    if (iErrorInfo) {
      return iErrorInfo;
    }
    if (uiCode == 1) {
      iSymTmp += (1 << iCount);
      ++iCount;
    }
  } while (uiCode != 0 && iCount != 16);
  if (iCount == 16) {
    return GENERATE_ERROR_NO (ERR_LEVEL_MB_DATA, ERR_CABAC_UNEXPECTED_VALUE);
  }
  while (iCount--) {
    int32_t iErrorInfo = DecodeBypassCabacFast (pDecEngine, uiCode);
    if (iErrorInfo) {
      return iErrorInfo;
    }
    if (uiCode == 1) {
      iSymTmp2 |= (1 << iCount);
    }
  }
  uiSymVal = (uint32_t) (iSymTmp + iSymTmp2);
  return ERR_NONE;
}

static inline uint32_t DecodeUEGLevelCabacFast (PWelsCabacDecEngine pDecEngine, PWelsCabacCtx pBinCtx,
                                                uint32_t& uiCode) {
  uiCode = 0;
  int32_t iErrorInfo = DecodeBinCabacFast (pDecEngine, pBinCtx, uiCode);
  if (iErrorInfo || uiCode == 0) {
    return (uint32_t)iErrorInfo;
  }
  uint32_t uiTmp = 0;
  uint32_t uiCount = 1;
  uiCode = 0;
  do {
    iErrorInfo = DecodeBinCabacFast (pDecEngine, pBinCtx, uiTmp);
    if (iErrorInfo) {
      return (uint32_t)iErrorInfo;
    }
    ++uiCode;
    ++uiCount;
  } while (uiTmp != 0 && uiCount != 13);
  if (uiTmp != 0) {
    iErrorInfo = DecodeExpBypassCabacFast (pDecEngine, 0, uiTmp);
    if (iErrorInfo) {
      return (uint32_t)iErrorInfo;
    }
    uiCode += uiTmp + 1;
  }
  return ERR_NONE;
}

static inline int32_t DecodeUEGMvCabacFast (PWelsCabacDecEngine pDecEngine, PWelsCabacCtx pBinCtx,
                                            uint32_t iMaxBin, uint32_t& uiCode) {
  (void)iMaxBin;
  int32_t iErrorInfo = DecodeBinCabacFast (pDecEngine, pBinCtx, uiCode);
  if (iErrorInfo || uiCode == 0) {
    return iErrorInfo;
  }
  uint32_t uiTmp = 0;
  uint32_t uiCount = 1;
  uiCode = 0;
  do {
    const uint32_t uiCtx = (uiCount < 3) ? uiCount : 3;
    iErrorInfo = DecodeBinCabacFast (pDecEngine, pBinCtx + uiCtx, uiTmp);
    if (iErrorInfo) {
      return iErrorInfo;
    }
    ++uiCode;
    ++uiCount;
  } while (uiTmp != 0 && uiCount != 8);
  if (uiTmp != 0) {
    iErrorInfo = DecodeExpBypassCabacFast (pDecEngine, 3, uiTmp);
    if (iErrorInfo) {
      return iErrorInfo;
    }
    uiCode += (uiTmp + 1);
  }
  return ERR_NONE;
}

#ifndef WELS_CABAC_DECODER_NO_ALIAS
#define Read32BitsCabac Read32BitsCabacFast
#define DecodeBinCabac DecodeBinCabacFast
#define DecodeBypassCabac DecodeBypassCabacFast
#define DecodeTerminateCabac DecodeTerminateCabacFast
#define DecodeUnaryBinCabac DecodeUnaryBinCabacFast
#define DecodeExpBypassCabac DecodeExpBypassCabacFast
#define DecodeUEGLevelCabac DecodeUEGLevelCabacFast
#define DecodeUEGMvCabac DecodeUEGMvCabacFast
#endif
#endif
}
#endif
