// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pow.h"
#include "validation.h"
#include "arith_uint256.h"
#include "chain.h"
#include "primitives/block.h"
#include "consensus/consensus.h"
#include "uint256.h"
#include <iostream>
#include "util.h"
#include "chainparams.h"
#include "libzerocoin/bitcoin_bignum/bignum.h"
#include "utilstrencodings.h"
#include "fixed.h"

static CBigNum bnProofOfWorkLimit(~arith_uint256(0) >> 8);

double GetDifficultyHelper(unsigned int nBits) {
    int nShift = (nBits >> 24) & 0xff;
    double dDiff = (double) 0x0000ffff / (double) (nBits & 0x00ffffff);

    while (nShift < 29) {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29) {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    if (!pindexLast || pindexLast->nHeight < params.nDifficultyAdjustStartBlock)
        return params.nFixedDifficulty;

    if (params.IsTestnet()) {
        // If the new block's timestamp is more than nTargetSpacing*6
        // then allow mining of a min-difficulty block
        if (pblock->nTime > pindexLast->nTime + params.nPowTargetTimespan * 1) {
            return params.nFixedDifficulty;
        }
    }


    const uint32_t BlocksTargetSpacing =
        (params.nMTPFiveMinutesStartBlock == 0) || (params.nMTPFiveMinutesStartBlock > 0 && pindexLast->nHeight >= params.nMTPFiveMinutesStartBlock) ?
            params.nPowTargetSpacingMTP : params.nPowTargetSpacing;
    unsigned int TimeDaySeconds = 60 * 60 * 24;
    int64_t PastSecondsMin = TimeDaySeconds * 0.25; // 21600
    int64_t PastSecondsMax = TimeDaySeconds * 7;// 604800
    uint32_t PastBlocksMin = PastSecondsMin / BlocksTargetSpacing; // 36 blocks
    uint32_t StartingPoWBlock = 0;

    if ((pindexLast->nHeight + 1 - StartingPoWBlock) % params.DifficultyAdjustmentInterval() != 0) // Retarget every nInterval blocks
    {
        return pindexLast->nBits;
    }
    //TODO port over idx diff algo
    return pindexLast->nBits;
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nPowTargetTimespan/4)
        nActualTimespan = params.nPowTargetTimespan/4;
    if (nActualTimespan > params.nPowTargetTimespan*4)
        nActualTimespan = params.nPowTargetTimespan*4;

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespan;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);
    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;
    return true;
}