//Extra utils to help understand info on chain
#include "blockinfo.h"

CTransactionRef GetBlockRewardTransaction(const CBlock& block){
    return block.vtx[block.IsProofOfStake()];
}

CTxOut GetStakeTXOut(const CTxIn& txin)
{
    CTransactionRef prevTx;
    uint256 hashBlock;
    if (GetTransaction(txin.prevout.hash, prevTx, Params().GetConsensus(), hashBlock, true)) {
        CTxOut const & txOut = prevTx->vout.at(txin.prevout.n);
        return txOut;
    }
    return CTxOut();
}

std::string GetBlockRewardWinner(const CBlock& block)
{
    std::string rewardWinner = "";
    CTransactionRef RewardTX = GetBlockRewardTransaction(block);
    CTxDestination dstAddr;
    const CTxOut& txout = block.IsProofOfStake() ? GetStakeTXOut(RewardTX->vin[0]) : RewardTX->vout[0];
    if(txout != CTxOut() && ExtractDestination(txout.scriptPubKey, dstAddr))
        rewardWinner = CBitcoinAddress(dstAddr).ToString();
    return rewardWinner;
}

CAmount GetTXInputAmount(const std::vector<CTxIn> vin){
    CAmount nValueIn = 0;
    //Cycle through inputs as we may have many inputs staking
    for(CTxIn txin : vin)
        nValueIn += GetStakeTXOut(txin).nValue;

    return nValueIn;
}

CAmount GetBlockInputCoins(const CBlock& block)
{
    return GetTXInputAmount(GetBlockRewardTransaction(block)->vin);;
}

CAmount GetCoinbaseReward(const CBlock& block)
{
    CTransactionRef coinbaseTX = GetBlockRewardTransaction(block);
    CAmount BlockReward = 0;
    if(block.IsProofOfStake())
        BlockReward = coinbaseTX->GetValueOut() - GetTXInputAmount(coinbaseTX->vin);
    else
        BlockReward = coinbaseTX->GetValueOut();
    return BlockReward;
}

float GetBlockInput(const CBlock& block)
{
    return ValueFromAmount(GetBlockInputCoins(block)).get_real();
}