#ifndef INDEXNODESYNC_INTERFACE_H
#define INDEXNODESYNC_INTERFACE_H

#include "masternode-sync.h"

/**
 * Class for getting sync status with either version of indexnodes (legacy and evo)
 * This is temporary measure, remove it when transition to evo indexnodes is done on mainnet
 */

class CZnodeSyncInterface {
private:
    // is it evo mode?
    bool fEvoZnodes;

public:
    CZnodeSyncInterface() : fEvoZnodes(false) {}

    bool IsFailed() { return GetAssetID() == MASTERNODE_SYNC_FAILED; }
    bool IsBlockchainSynced();
    bool IsSynced();

    int GetAssetID();

    void Reset();
    void SwitchToNextAsset(CConnman& connman);

    std::string GetAssetName();
    std::string GetSyncStatus();

    void UpdatedBlockTip(const CBlockIndex *pindexNew, bool fInitialDownload, CConnman& connman);
};

extern CZnodeSyncInterface indexnodeSyncInterface;

#endif