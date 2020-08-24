// Copyright (c) 2020 The Noir Developers
// Copyright (c) 2014-2018 The BlackCoin Developers
// Copyright (c) 2011-2013 The PPCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Stake cache by Qtum
// Copyright (c) 2016-2018 The Qtum developers

#ifndef NOIR_POS_H
#define NOIR_POS_H

#include "pos.h"
#include "txdb.h"
#include "validation.h"
#include "arith_uint256.h"
#include "consensus/validation.h"
#include "hash.h"
#include "timedata.h"
#include "chainparams.h"
#include "script/sign.h"
#include <stdint.h>

using namespace std;

/** Compute the hash modifier for proof-of-stake */
uint256 ComputeStakeModifier(const CBlockIndex* pindexPrev, const uint256& kernel);

struct CStakeCache{
    CStakeCache(uint32_t blockFromTime_, CAmount amount_) : blockFromTime(blockFromTime_), amount(amount_){
    }
    uint32_t blockFromTime;
    CAmount amount;
};
// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx);
bool CheckStakeBlockTimestamp(int64_t nTimeBlock);
// Wrapper around CheckStakeKernelHash()
// Also checks existence of kernel input and min age
// Convenient for searching a kernel
bool CheckKernel(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t nTimeBlock, const COutPoint& prevout, CCoinsViewCache& view);
bool CheckKernel(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t nTimeBlock, const COutPoint& prevout, CCoinsViewCache& view, const std::map<COutPoint, CStakeCache>& cache);
bool CheckStakeKernelHash(const CBlockIndex* pindexPrev, unsigned int nBits, unsigned int nBlockTime, const Coin* txPrev, const COutPoint& prevout, unsigned int nTimeTx, bool fPrintProofOfStake = false);
bool CheckProofOfStake(CBlockIndex* pindexPrev, const CTransaction& tx, unsigned int nBlockTime, unsigned int nBits, CValidationState &state, CCoinsViewCache& view);
void CacheKernel(std::map<COutPoint, CStakeCache>& cache, const COutPoint& prevout, CBlockIndex* pindexPrev, CCoinsViewCache& view);
bool VerifySignature(const Coin& coin, const uint256 txFromHash, const CTransaction& txTo, unsigned int nIn, unsigned int flags);
#endif // NOIR_POS_H