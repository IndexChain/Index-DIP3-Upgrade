// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INDEXNODEMAN_H
#define INDEXNODEMAN_H

#include "indexnode.h"
#include "sync.h"

using namespace std;

class CZnodeMan;

extern CZnodeMan mnodeman;

/**
 * Provides a forward and reverse index between MN vin's and integers.
 *
 * This mapping is normally add-only and is expected to be permanent
 * It is only rebuilt if the size of the index exceeds the expected maximum number
 * of MN's and the current number of known MN's.
 *
 * The external interface to this index is provided via delegation by CZnodeMan
 */
class CZnodeIndex
{
public: // Types
    typedef std::map<CTxIn,int> index_m_t;

    typedef index_m_t::iterator index_m_it;

    typedef index_m_t::const_iterator index_m_cit;

    typedef std::map<int,CTxIn> rindex_m_t;

    typedef rindex_m_t::iterator rindex_m_it;

    typedef rindex_m_t::const_iterator rindex_m_cit;

private:
    int                  nSize;

    index_m_t            mapIndex;

    rindex_m_t           mapReverseIndex;

public:
    CZnodeIndex();

    int GetSize() const {
        return nSize;
    }

    /// Retrieve indexnode vin by index
    bool Get(int nIndex, CTxIn& vinZnode) const;

    /// Get index of a indexnode vin
    int GetZnodeIndex(const CTxIn& vinZnode) const;

    void AddZnodeVIN(const CTxIn& vinZnode);

    void Clear();

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(mapIndex);
        if(ser_action.ForRead()) {
            RebuildIndex();
        }
    }

private:
    void RebuildIndex();

};

class CZnodeMan
{
public:
    typedef std::map<CTxIn,int> index_m_t;

    typedef index_m_t::iterator index_m_it;

    typedef index_m_t::const_iterator index_m_cit;

private:
    static const int MAX_EXPECTED_INDEX_SIZE = 30000;

    /// Only allow 1 index rebuild per hour
    static const int64_t MIN_INDEX_REBUILD_TIME = 3600;

    static const std::string SERIALIZATION_VERSION_STRING;

    static const int DSEG_UPDATE_SECONDS        = 3 * 60 * 60;

    static const int LAST_PAID_SCAN_BLOCKS      = 100;

    static const int MIN_POSE_PROTO_VERSION     = 70203;
    static const int MAX_POSE_CONNECTIONS       = 10;
    static const int MAX_POSE_RANK              = 10;
    static const int MAX_POSE_BLOCKS            = 10;

    static const int MNB_RECOVERY_QUORUM_TOTAL      = 10;
    static const int MNB_RECOVERY_QUORUM_REQUIRED   = 6;
    static const int MNB_RECOVERY_MAX_ASK_ENTRIES   = 10;
    static const int MNB_RECOVERY_WAIT_SECONDS      = 60;
    static const int MNB_RECOVERY_RETRY_SECONDS     = 3 * 60 * 60;


    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    // Keep track of current block index
    const CBlockIndex *pCurrentBlockIndex;

    // map to hold all MNs
    std::vector<CZnode> vIndexnodes;
    // who's asked for the Znode list and the last time
    std::map<CNetAddr, int64_t> mAskedUsForZnodeList;
    // who we asked for the Znode list and the last time
    std::map<CNetAddr, int64_t> mWeAskedForZnodeList;
    // which Indexnodes we've asked for
    std::map<COutPoint, std::map<CNetAddr, int64_t> > mWeAskedForZnodeListEntry;
    // who we asked for the indexnode verification
    std::map<CNetAddr, CZnodeVerification> mWeAskedForVerification;

    // these maps are used for indexnode recovery from INDEXNODE_NEW_START_REQUIRED state
    std::map<uint256, std::pair< int64_t, std::set<CNetAddr> > > mMnbRecoveryRequests;
    std::map<uint256, std::vector<CZnodeBroadcast> > mMnbRecoveryGoodReplies;
    std::list< std::pair<CService, uint256> > listScheduledMnbRequestConnections;
    std::map<CService, std::pair<int64_t, CZnodeVerification> > mapPendingMNV;
    CCriticalSection cs_mapPendingMNV;

    int64_t nLastIndexRebuildTime;

    CZnodeIndex indexIndexnodes;

    CZnodeIndex indexIndexnodesOld;

    /// Set when index has been rebuilt, clear when read
    bool fIndexRebuilt;

    /// Set when indexnodes are added, cleared when CGovernanceManager is notified
    bool fIndexnodesAdded;

    /// Set when indexnodes are removed, cleared when CGovernanceManager is notified
    bool fIndexnodesRemoved;

    std::vector<uint256> vecDirtyGovernanceObjectHashes;

    int64_t nLastWatchdogVoteTime;

    friend class CZnodeSync;

public:
    // Keep track of all broadcasts I've seen
    std::map<uint256, std::pair<int64_t, CZnodeBroadcast> > mapSeenZnodeBroadcast;
    // Keep track of all pings I've seen
    std::map<uint256, CZnodePing> mapSeenZnodePing;
    // Keep track of all verifications I've seen
    std::map<uint256, CZnodeVerification> mapSeenZnodeVerification;
    // keep track of dsq count to prevent indexnodes from gaming darksend queue
    int64_t nDsqCount;


    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        LOCK(cs);
        std::string strVersion;
        if(ser_action.ForRead()) {
            READWRITE(strVersion);
        }
        else {
            strVersion = SERIALIZATION_VERSION_STRING; 
            READWRITE(strVersion);
        }

        READWRITE(vIndexnodes);
        READWRITE(mAskedUsForZnodeList);
        READWRITE(mWeAskedForZnodeList);
        READWRITE(mWeAskedForZnodeListEntry);
        READWRITE(mMnbRecoveryRequests);
        READWRITE(mMnbRecoveryGoodReplies);
        READWRITE(nLastWatchdogVoteTime);
        READWRITE(nDsqCount);

        READWRITE(mapSeenZnodeBroadcast);
        READWRITE(mapSeenZnodePing);
        READWRITE(indexIndexnodes);
        if(ser_action.ForRead() && (strVersion != SERIALIZATION_VERSION_STRING)) {
            Clear();
        }
    }

    CZnodeMan();

    /// Add an entry
    bool Add(CZnode &mn);

    /// Ask (source) node for mnb
    void AskForMN(CNode *pnode, const CTxIn &vin);
    void AskForMnb(CNode *pnode, const uint256 &hash);

    /// Check all Indexnodes
    void Check();

    /// Check all Indexnodes and remove inactive
    void CheckAndRemove();

    /// Clear Znode vector
    void Clear();

    /// Count Indexnodes filtered by nProtocolVersion.
    /// Znode nProtocolVersion should match or be above the one specified in param here.
    int CountIndexnodes(int nProtocolVersion = -1);
    /// Count enabled Indexnodes filtered by nProtocolVersion.
    /// Znode nProtocolVersion should match or be above the one specified in param here.
    int CountEnabled(int nProtocolVersion = -1);

    /// Count Indexnodes by network type - NET_IPV4, NET_IPV6, NET_TOR
    // int CountByIP(int nNetworkType);

    void DsegUpdate(CNode* pnode);

    /// Find an entry
    CZnode* Find(const std::string &txHash, const std::string &outputIndex);
    CZnode* Find(const CScript &payee);
    CZnode* Find(const CTxIn& vin);
    CZnode* Find(const CPubKey& pubKeyZnode);

    /// Versions of Find that are safe to use from outside the class
    bool Get(const CPubKey& pubKeyZnode, CZnode& indexnode);
    bool Get(const CTxIn& vin, CZnode& indexnode);

    /// Retrieve indexnode vin by index
    bool Get(int nIndex, CTxIn& vinZnode, bool& fIndexRebuiltOut) {
        LOCK(cs);
        fIndexRebuiltOut = fIndexRebuilt;
        return indexIndexnodes.Get(nIndex, vinZnode);
    }

    bool GetIndexRebuiltFlag() {
        LOCK(cs);
        return fIndexRebuilt;
    }

    /// Get index of a indexnode vin
    int GetZnodeIndex(const CTxIn& vinZnode) {
        LOCK(cs);
        return indexIndexnodes.GetZnodeIndex(vinZnode);
    }

    /// Get old index of a indexnode vin
    int GetZnodeIndexOld(const CTxIn& vinZnode) {
        LOCK(cs);
        return indexIndexnodesOld.GetZnodeIndex(vinZnode);
    }

    /// Get indexnode VIN for an old index value
    bool GetZnodeVinForIndexOld(int nZnodeIndex, CTxIn& vinZnodeOut) {
        LOCK(cs);
        return indexIndexnodesOld.Get(nZnodeIndex, vinZnodeOut);
    }

    /// Get index of a indexnode vin, returning rebuild flag
    int GetZnodeIndex(const CTxIn& vinZnode, bool& fIndexRebuiltOut) {
        LOCK(cs);
        fIndexRebuiltOut = fIndexRebuilt;
        return indexIndexnodes.GetZnodeIndex(vinZnode);
    }

    void ClearOldZnodeIndex() {
        LOCK(cs);
        indexIndexnodesOld.Clear();
        fIndexRebuilt = false;
    }

    bool Has(const CTxIn& vin);

    indexnode_info_t GetZnodeInfo(const CTxIn& vin);

    indexnode_info_t GetZnodeInfo(const CPubKey& pubKeyZnode);

    char* GetNotQualifyReason(CZnode& mn, int nBlockHeight, bool fFilterSigTime, int nMnCount);

    /// Find an entry in the indexnode list that is next to be paid
    CZnode* GetNextZnodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCount);
    /// Same as above but use current block height
    CZnode* GetNextZnodeInQueueForPayment(bool fFilterSigTime, int& nCount);

    /// Find a random entry
    CZnode* FindRandomNotInVec(const std::vector<CTxIn> &vecToExclude, int nProtocolVersion = -1);

    std::vector<CZnode> GetFullZnodeVector() { LOCK(cs); return vIndexnodes; }

    std::vector<std::pair<int, CZnode> > GetZnodeRanks(int nBlockHeight = -1, int nMinProtocol=0);
    int GetZnodeRank(const CTxIn &vin, int nBlockHeight, int nMinProtocol=0, bool fOnlyActive=true);
    CZnode* GetZnodeByRank(int nRank, int nBlockHeight, int nMinProtocol=0, bool fOnlyActive=true);

    void ProcessZnodeConnections();
    std::pair<CService, std::set<uint256> > PopScheduledMnbRequestConnection();

    void ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);

    void DoFullVerificationStep();
    void CheckSameAddr();
    bool CheckVerifyRequestAddr(const CAddress& addr, CConnman& connman);
    void PrepareVerifyRequest(const CAddress& addr, CConnman& connman);
    void ProcessPendingMnvRequests(CConnman& connman);
    void SendVerifyReply(CNode* pnode, CZnodeVerification& mnv);
    void ProcessVerifyReply(CNode* pnode, CZnodeVerification& mnv);
    void ProcessVerifyBroadcast(CNode* pnode, const CZnodeVerification& mnv);

    /// Return the number of (unique) Indexnodes
    int size() { return vIndexnodes.size(); }

    std::string ToString() const;

    /// Update indexnode list and maps using provided CZnodeBroadcast
    void UpdateZnodeList(CZnodeBroadcast mnb);
    /// Perform complete check and only then update list and maps
    bool CheckMnbAndUpdateZnodeList(CNode* pfrom, CZnodeBroadcast mnb, int& nDos);
    bool IsMnbRecoveryRequested(const uint256& hash) { return mMnbRecoveryRequests.count(hash); }

    void UpdateLastPaid();

    void CheckAndRebuildZnodeIndex();

    void AddDirtyGovernanceObjectHash(const uint256& nHash)
    {
        LOCK(cs);
        vecDirtyGovernanceObjectHashes.push_back(nHash);
    }

    std::vector<uint256> GetAndClearDirtyGovernanceObjectHashes()
    {
        LOCK(cs);
        std::vector<uint256> vecTmp = vecDirtyGovernanceObjectHashes;
        vecDirtyGovernanceObjectHashes.clear();
        return vecTmp;;
    }

    bool IsWatchdogActive();
    void UpdateWatchdogVoteTime(const CTxIn& vin);
    bool AddGovernanceVote(const CTxIn& vin, uint256 nGovernanceObjectHash);
    void RemoveGovernanceObject(uint256 nGovernanceObjectHash);

    void CheckZnode(const CTxIn& vin, bool fForce = false);
    void CheckZnode(const CPubKey& pubKeyZnode, bool fForce = false);

    int GetZnodeState(const CTxIn& vin);
    int GetZnodeState(const CPubKey& pubKeyZnode);

    bool IsZnodePingedWithin(const CTxIn& vin, int nSeconds, int64_t nTimeToCheckAt = -1);
    void SetZnodeLastPing(const CTxIn& vin, const CZnodePing& mnp);

    void UpdatedBlockTip(const CBlockIndex *pindex);

    /**
     * Called to notify CGovernanceManager that the indexnode index has been updated.
     * Must be called while not holding the CZnodeMan::cs mutex
     */
    void NotifyZnodeUpdates();
    
    CCriticalSection & GetCS() {return cs;}

};

#endif
