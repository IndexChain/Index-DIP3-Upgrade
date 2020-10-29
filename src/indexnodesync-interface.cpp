#include "indexnodesync-interface.h"
#include "indexnode-sync.h"

#include "evo/deterministicmns.h"

CZnodeSyncInterface indexnodeSyncInterface;

void CZnodeSyncInterface::Reset()
{
    if (!fEvoZnodes)
        indexnodeSync.Reset();
    masternodeSync.Reset();
}

int CZnodeSyncInterface::GetAssetID()
{
    return fEvoZnodes ? masternodeSync.GetAssetID() : indexnodeSync.GetAssetID();
}

bool CZnodeSyncInterface::IsBlockchainSynced() {
    return fEvoZnodes ? masternodeSync.IsBlockchainSynced() : indexnodeSync.IsBlockchainSynced();
}

bool CZnodeSyncInterface::IsSynced() {
    return fEvoZnodes ? masternodeSync.IsSynced() : indexnodeSync.IsSynced();
}

void CZnodeSyncInterface::UpdatedBlockTip(const CBlockIndex * /*pindexNew*/, bool /*fInitialDownload*/, CConnman & /*connman*/)
{
    fEvoZnodes = deterministicMNManager->IsDIP3Enforced();
}

void CZnodeSyncInterface::SwitchToNextAsset(CConnman &connman)
{
    fEvoZnodes ? masternodeSync.SwitchToNextAsset(connman) : indexnodeSync.SwitchToNextAsset();
}

std::string CZnodeSyncInterface::GetAssetName()
{
    return fEvoZnodes ? masternodeSync.GetAssetName() : indexnodeSync.GetAssetName();
}

std::string CZnodeSyncInterface::GetSyncStatus()
{
    return fEvoZnodes ? masternodeSync.GetSyncStatus() : indexnodeSync.GetSyncStatus();
}