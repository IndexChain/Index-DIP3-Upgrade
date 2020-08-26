// Copyright (c) 2020 The Noir Developers
// Copyright (c) 2014-2018 The BlackCoin Developers
// Copyright (c) 2011-2013 The PPCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Stake cache by Qtum
// Copyright (c) 2016-2018 The Qtum developers

#include "pos.h"

#include "chain.h"
#include "chainparams.h"
#include "clientversion.h"
#include "coins.h"
#include "consensus/consensus.h"
#include <consensus/merkle.h>
#include "hash.h"
#include "uint256.h"
#include "primitives/transaction.h"
#include <stdio.h>
#include <wallet/wallet.h>
#include "util.h"
// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
uint256 ComputeStakeModifier(const CBlockIndex* pindexPrev, const uint256& kernel)
{
    if (!pindexPrev)
        return uint256(); // genesis block's modifier is 0

    CHashWriter ss(SER_GETHASH, 0);
    ss << kernel << pindexPrev->nStakeModifier;
    return ss.GetHash();
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx)
{
    return (nTimeBlock == nTimeTx);
}

// Simplified version of CheckCoinStakeTimestamp() to check header-only timestamp
bool CheckStakeBlockTimestamp(int64_t nTimeBlock)
{
   return CheckCoinStakeTimestamp(nTimeBlock, nTimeBlock);
}

// BlackCoin kernel protocol v3
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.nTime + txPrev.vout.hash + txPrev.vout.n + nTime) < bnTarget * nWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coins one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                   future proof-of-stake
//   txPrev.nTime: slightly scrambles computation
//   txPrev.vout.hash: hash of txPrev, to reduce the chance of nodes
//                     generating coinstake at the same time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   nTime: current timestamp
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
bool CheckStakeKernelHash(const CBlockIndex* pindexPrev, unsigned int nBits, unsigned int nBlockTime, const Coin* txPrev, const COutPoint& prevout, unsigned int nTimeTx, bool fPrintProofOfStake)
{
      if ((nTimeTx < nBlockTime) && !(txPrev->nHeight <= Params().GetConsensus().nFirstPOSBlock))  // Transaction timestamp violation
        return error("CheckStakeKernelHash() : nTime violation");
        // return error("CheckStakeKernelHash() : nTime violation");

    // Base target
    arith_uint256 bnTarget;
    bnTarget.SetCompact(nBits);

    // Weighted target
    int64_t nValueIn = txPrev->out.nValue;
    if (nValueIn == 0)
        return error("CheckStakeKernelHash() : nValueIn = 0");
    arith_uint256 bnWeight = arith_uint256(nValueIn);
    bnTarget *= bnWeight;

    uint256 nStakeModifier = pindexPrev->nStakeModifier;

    // Calculate hash
    CHashWriter ss(SER_GETHASH, 0);
    ss << nStakeModifier;
    ss << nBlockTime << prevout.hash << prevout.n << nTimeTx;

    uint256 hashProofOfStake = ss.GetHash();

    if (fPrintProofOfStake)
    {
        LogPrintf("CheckStakeKernelHash() : nStakeModifier=%s, txPrev.nTime=%u, txPrev.vout.hash=%s, txPrev.vout.n=%u, nTime=%u, hashProof=%s\n",
            nStakeModifier.GetHex().c_str(),
            nBlockTime, prevout.hash.ToString(), prevout.n, nTimeTx,
            hashProofOfStake.ToString());
    }

    // Now check if proof-of-stake hash meets target protocol
    if (UintToArith256(hashProofOfStake) > bnTarget){
        return false;
    }
    if (fDebug && !fPrintProofOfStake)
    {
        LogPrintf("CheckStakeKernelHash() : nStakeModifier=%s, txPrev.nTime=%u, txPrev.vout.hash=%s, txPrev.vout.n=%u, nTime=%u, hashProof=%s\n",
            nStakeModifier.GetHex().c_str(),
            nBlockTime, prevout.hash.ToString(), prevout.n, nTimeTx,
            hashProofOfStake.ToString());
    }

    return true;
}
bool GetStakeCoin(const COutPoint& prevout, Coin& coinPrev, CBlockIndex*& blockFrom, CBlockIndex* pindexPrev, CValidationState& state, CCoinsViewCache& view)
{
    // Get the coin
    if(!view.GetCoin(prevout, coinPrev)){
        return state.Invalid(false, REJECT_INVALID, "stake-prevout-not-exist", strprintf("CheckProofOfStake() : Stake prevout does not exist %s", prevout.hash.ToString()));
    }

    // Check that the coin is mature
    int nHeight = pindexPrev->nHeight + 1;
    if(nHeight - coinPrev.nHeight < COINBASE_MATURITY){
        return state.Invalid(false, REJECT_INVALID, "stake-prevout-not-mature", strprintf("CheckProofOfStake() : Stake prevout is not mature, expecting %i and only matured to %i", COINBASE_MATURITY, nHeight - coinPrev.nHeight));
    }

    // Get the block header from the coin
    blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
    if(!blockFrom) {
        return state.Invalid(false, REJECT_INVALID, "stake-prevout-not-loaded", strprintf("CheckProofOfStake() : Block at height %i for prevout can not be loaded", coinPrev.nHeight));
    }

    return true;
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CBlockIndex* pindexPrev, const CTransaction& tx, unsigned int nBlockTime, unsigned int nBits, CValidationState &state, CCoinsViewCache& view)
{

    if (!tx.IsCoinStake())
        return error("CheckProofOfStake() : called on non-coinstake %s", tx.GetHash().ToString());

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn& txin = tx.vin[0];

    // First try finding the previous transaction in database
    Coin coinTxPrev;
    CBlockIndex* blockTxFrom = 0;

    // First try finding the previous transaction in database
    CTransactionRef txPrev;
    uint256 hashBlock = uint256();
    if (!GetTransaction(txin.prevout.hash, txPrev, Params().GetConsensus(), hashBlock, true) || !GetStakeCoin(txin.prevout, coinTxPrev, blockTxFrom, pindexPrev, state, view))
        return error("CheckProofOfStake() : fail to get prevout %s", txin.prevout.hash.ToString());


    // Verify inputs
    if (txin.prevout.hash != txPrev->GetHash())
        return state.DoS(100, error("CheckProofOfStake() : coinstake input does not match previous output %s != %s", txin.prevout.hash.GetHex(),coinTxPrev.out.GetHash().ToString()));

    // Verify signature
    if (!VerifySignature(coinTxPrev, txin.prevout.hash, tx, 0, SCRIPT_VERIFY_NONE))
       return state.DoS(100, error("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx.GetHash().ToString()));


    unsigned int nTime = pindexPrev->GetBlockTime();

    if (!CheckStakeKernelHash(pindexPrev, nBits, nTime,&coinTxPrev, txin.prevout, nBlockTime, fDebug))
       return state.Invalid(false, REJECT_INVALID,"CheckProofOfStake() : INFO: check kernel failed on coinstake %s", tx.GetHash().ToString()); // may occur during initial download or if behind on block chain sync

    return true;
}

bool VerifySignature(const Coin& coin, const uint256 txFromHash, const CTransaction& txTo, unsigned int nIn, unsigned int flags)
{
    TransactionSignatureChecker checker(&txTo, nIn, 0);

    const CTxIn& txin = txTo.vin[nIn];
//    if (txin.prevout.n >= txFrom.vout.size())
//        return false;
//    const CTxOut& txout = txFrom.vout[txin.prevout.n];

    const CTxOut& txout = coin.out;

    if (txin.prevout.hash != txFromHash)
        return false;

    return VerifyScript(txin.scriptSig, txout.scriptPubKey, NULL, flags, checker);
}

bool CheckKernel(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t nTimeBlock, const COutPoint& prevout, CCoinsViewCache& view)
{
    std::map<COutPoint, CStakeCache> tmp;
    return CheckKernel(pindexPrev, nBits, nTimeBlock, prevout, view, tmp);
}

bool CheckKernel(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t nTimeBlock, const COutPoint& prevout, CCoinsViewCache& view, const std::map<COutPoint, CStakeCache>& cache)
{
    uint256 hashProofOfStake, targetProofOfStake;
    auto it=cache.find(prevout);
    *pBlockTime = pindexPrev->GetBlockTime();
    if(nTime < *pBlockTime) return false;
    if(it == cache.end()) {
        //not found in cache (shouldn't happen during staking, only during verification which does not use cache)
        Coin coinPrev;
        if(!view.GetCoin(prevout, coinPrev)){
            if(!GetSpentCoinFromMainChain(pindexPrev, prevout, &coinPrev)) {
                return error("CheckKernel(): Could not find coin and it was not at the tip");
            }
        }

        if(pindexPrev->nHeight + 1 - coinPrev.nHeight < COINBASE_MATURITY){
            return error("CheckKernel(): Coin not matured");
        }
        CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
        if(!blockFrom) {
            return error("CheckKernel(): Could not find block");
        }
        if(coinPrev.IsSpent()){
            return error("CheckKernel(): Coin is spent");
        }

        return CheckStakeKernelHash(pindexPrev, nBits, blockFrom->nTime,&coinPrev, prevout, nTimeBlock);

    }
    /*else{
        //found in cache
        const CStakeCache& stake = it->second;
        if(CheckStakeKernelHash(pindexPrev, nBits, stake.blockFromTime, stake., prevout,
                                    nTimeBlock)){
            //Cache could potentially cause false positive stakes in the event of deep reorgs, so check without cache also
            return CheckKernel(pindexPrev, nBits, nTimeBlock, prevout, view);
        }
    }*/
    return false;
}

void CacheKernel(std::map<COutPoint, CStakeCache>& cache, const COutPoint& prevout, CBlockIndex* pindexPrev, CCoinsViewCache& view){
    if(cache.find(prevout) != cache.end()){
        //already in cache
        return;
    }

    Coin coinPrev;
    if(!view.GetCoin(prevout, coinPrev)){
        return;
    }

    if(pindexPrev->nHeight + 1 - coinPrev.nHeight < COINBASE_MATURITY){
        return;
    }
    CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
    if(!blockFrom) {
        return;
    }

    CStakeCache c(blockFrom->nTime, coinPrev.out.nValue);
    cache.insert({prevout, c});
}