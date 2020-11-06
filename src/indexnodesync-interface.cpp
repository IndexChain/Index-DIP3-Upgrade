#include "indexnodesync-interface.h"
#include "indexnode-sync.h"

#include "evo/deterministicmns.h"

CZnodeSyncInterface indexnodeSyncInterface;

void CZnodeSyncInterface::Reset()
{
    if (!fEvoIndexnodes)
        indexnodeSync.Reset();
    masternodeSync.Reset();
}

int CZnodeSyncInterface::GetAssetID()
{
    return fEvoIndexnodes ? masternodeSync.GetAssetID() : indexnodeSync.GetAssetID();
}

bool CZnodeSyncInterface::IsBlockchainSynced() {
    return fEvoIndexnodes ? masternodeSync.IsBlockchainSynced() : indexnodeSync.IsBlockchainSynced();
}

bool CZnodeSyncInterface::IsSynced() {
    return fEvoIndexnodes ? masternodeSync.IsSynced() : indexnodeSync.IsSynced();
}

void CZnodeSyncInterface::UpdatedBlockTip(const CBlockIndex * /*pindexNew*/, bool /*fInitialDownload*/, CConnman & /*connman*/)
{
    fEvoIndexnodes = deterministicMNManager->IsDIP3Enforced();
}

void CZnodeSyncInterface::SwitchToNextAsset(CConnman &connman)
{
    fEvoIndexnodes ? masternodeSync.SwitchToNextAsset(connman) : indexnodeSync.SwitchToNextAsset();
}

std::string CZnodeSyncInterface::GetAssetName()
{
    return fEvoIndexnodes ? masternodeSync.GetAssetName() : indexnodeSync.GetAssetName();
}

std::string CZnodeSyncInterface::GetSyncStatus()
{
    return fEvoIndexnodes ? masternodeSync.GetSyncStatus() : indexnodeSync.GetSyncStatus();
}