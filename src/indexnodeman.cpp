// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activeindexnode.h"
#include "addrman.h"
//#include "governance.h"
#include "indexnode-payments.h"
#include "indexnode-sync.h"
#include "indexnodeman.h"
#include "netfulfilledman.h"
#include "darksend.h"
#include "netmessagemaker.h"
#include "net.h"
#include "net_processing.h"
#include "util.h"
#include "txmempool.h"

#define cs_vNodes (g_connman->cs_vNodes)
#define vNodes (g_connman->vNodes)

/**
 * PRNG initialized from secure entropy based RNG
 */
class InsecureRand
{
private:
    uint32_t nRz;
    uint32_t nRw;
    bool fDeterministic;

public:
    InsecureRand(bool _fDeterministic = false);

    /**
     * MWC RNG of George Marsaglia
     * This is intended to be fast. It has a period of 2^59.3, though the
     * least significant 16 bits only have a period of about 2^30.1.
     *
     * @return random value < nMax
     */
    int64_t operator()(int64_t nMax)
    {
        nRz = 36969 * (nRz & 65535) + (nRz >> 16);
        nRw = 18000 * (nRw & 65535) + (nRw >> 16);
        return ((nRw << 16) + nRz) % nMax;
    }
};

InsecureRand::InsecureRand(bool _fDeterministic)
        : nRz(11),
          nRw(11),
          fDeterministic(_fDeterministic)
{
    // The seed values have some unlikely fixed points which we avoid.
    if(fDeterministic) return;
    uint32_t nTmp;
    do {
        GetRandBytes((unsigned char*)&nTmp, 4);
    } while (nTmp == 0 || nTmp == 0x9068ffffU);
    nRz = nTmp;
    do {
        GetRandBytes((unsigned char*)&nTmp, 4);
    } while (nTmp == 0 || nTmp == 0x464fffffU);
    nRw = nTmp;
}

/** Znode manager */
CZnodeMan mnodeman;

const std::string CZnodeMan::SERIALIZATION_VERSION_STRING = "CZnodeMan-Version-4";

struct CompareLastPaidBlock
{
    bool operator()(const std::pair<int, CZnode*>& t1,
                    const std::pair<int, CZnode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->vin < t2.second->vin);
    }
};

struct CompareScoreMN
{
    bool operator()(const std::pair<int64_t, CZnode*>& t1,
                    const std::pair<int64_t, CZnode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->vin < t2.second->vin);
    }
};

CZnodeIndex::CZnodeIndex()
    : nSize(0),
      mapIndex(),
      mapReverseIndex()
{}

bool CZnodeIndex::Get(int nIndex, CTxIn& vinZnode) const
{
    rindex_m_cit it = mapReverseIndex.find(nIndex);
    if(it == mapReverseIndex.end()) {
        return false;
    }
    vinZnode = it->second;
    return true;
}

int CZnodeIndex::GetZnodeIndex(const CTxIn& vinZnode) const
{
    index_m_cit it = mapIndex.find(vinZnode);
    if(it == mapIndex.end()) {
        return -1;
    }
    return it->second;
}

void CZnodeIndex::AddZnodeVIN(const CTxIn& vinZnode)
{
    index_m_it it = mapIndex.find(vinZnode);
    if(it != mapIndex.end()) {
        return;
    }
    int nNextIndex = nSize;
    mapIndex[vinZnode] = nNextIndex;
    mapReverseIndex[nNextIndex] = vinZnode;
    ++nSize;
}

void CZnodeIndex::Clear()
{
    mapIndex.clear();
    mapReverseIndex.clear();
    nSize = 0;
}
struct CompareByAddr

{
    bool operator()(const CZnode* t1,
                    const CZnode* t2) const
    {
        return t1->addr < t2->addr;
    }
};

void CZnodeIndex::RebuildIndex()
{
    nSize = mapIndex.size();
    for(index_m_it it = mapIndex.begin(); it != mapIndex.end(); ++it) {
        mapReverseIndex[it->second] = it->first;
    }
}

CZnodeMan::CZnodeMan() : cs(),
  vZnodes(),
  mAskedUsForZnodeList(),
  mWeAskedForZnodeList(),
  mWeAskedForZnodeListEntry(),
  mWeAskedForVerification(),
  mMnbRecoveryRequests(),
  mMnbRecoveryGoodReplies(),
  listScheduledMnbRequestConnections(),
  nLastIndexRebuildTime(0),
  indexZnodes(),
  indexZnodesOld(),
  fIndexRebuilt(false),
  fZnodesAdded(false),
  fZnodesRemoved(false),
//  vecDirtyGovernanceObjectHashes(),
  nLastWatchdogVoteTime(0),
  mapSeenZnodeBroadcast(),
  mapSeenZnodePing(),
  nDsqCount(0)
{}

bool CZnodeMan::Add(CZnode &mn)
{
    LOCK(cs);

    CZnode *pmn = Find(mn.vin);
    if (pmn == NULL) {
        LogPrint("indexnode", "CZnodeMan::Add -- Adding new Znode: addr=%s, %i now\n", mn.addr.ToString(), size() + 1);
        vZnodes.push_back(mn);
        indexZnodes.AddZnodeVIN(mn.vin);
        fZnodesAdded = true;
        return true;
    }

    return false;
}

void CZnodeMan::AskForMN(CNode* pnode, const CTxIn &vin)
{
    if(!pnode) return;

    LOCK(cs);

    std::map<COutPoint, std::map<CNetAddr, int64_t> >::iterator it1 = mWeAskedForZnodeListEntry.find(vin.prevout);
    if (it1 != mWeAskedForZnodeListEntry.end()) {
        std::map<CNetAddr, int64_t>::iterator it2 = it1->second.find(pnode->addr);
        if (it2 != it1->second.end()) {
            if (GetTime() < it2->second) {
                // we've asked recently, should not repeat too often or we could get banned
                return;
            }
            // we asked this node for this outpoint but it's ok to ask again already
            LogPrintf("CZnodeMan::AskForMN -- Asking same peer %s for missing indexnode entry again: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
        } else {
            // we already asked for this outpoint but not this node
            LogPrintf("CZnodeMan::AskForMN -- Asking new peer %s for missing indexnode entry: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
        }
    } else {
        // we never asked any node for this outpoint
        LogPrintf("CZnodeMan::AskForMN -- Asking peer %s for missing indexnode entry for the first time: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
    }
    mWeAskedForZnodeListEntry[vin.prevout][pnode->addr] = GetTime() + DSEG_UPDATE_SECONDS;

    g_connman->PushMessage(pnode, CNetMsgMaker(LEGACY_INDEXNODES_PROTOCOL_VERSION).Make(NetMsgType::DSEG, vin));
}

void CZnodeMan::Check()
{
    LOCK(cs);

//    LogPrint("indexnode", "CZnodeMan::Check -- nLastWatchdogVoteTime=%d, IsWatchdogActive()=%d\n", nLastWatchdogVoteTime, IsWatchdogActive());

    BOOST_FOREACH(CZnode& mn, vZnodes) {
        mn.Check();
    }
}

void CZnodeMan::CheckAndRemove()
{
    if(!indexnodeSync.IsZnodeListSynced()) return;

    LogPrintf("CZnodeMan::CheckAndRemove\n");

    {
        // Need LOCK2 here to ensure consistent locking order because code below locks cs_main
        // in CheckMnbAndUpdateZnodeList()
        LOCK2(cs_main, cs);

        Check();

        // Remove spent indexnodes, prepare structures and make requests to reasure the state of inactive ones
        std::vector<CZnode>::iterator it = vZnodes.begin();
        std::vector<std::pair<int, CZnode> > vecZnodeRanks;
        // ask for up to MNB_RECOVERY_MAX_ASK_ENTRIES indexnode entries at a time
        int nAskForMnbRecovery = MNB_RECOVERY_MAX_ASK_ENTRIES;
        while(it != vZnodes.end()) {
            CZnodeBroadcast mnb = CZnodeBroadcast(*it);
            uint256 hash = mnb.GetHash();
            // If collateral was spent ...
            if ((*it).IsOutpointSpent()) {
                LogPrint("indexnode", "CZnodeMan::CheckAndRemove -- Removing Znode: %s  addr=%s  %i now\n", (*it).GetStateString(), (*it).addr.ToString(), size() - 1);

                // erase all of the broadcasts we've seen from this txin, ...
                mapSeenZnodeBroadcast.erase(hash);
                mWeAskedForZnodeListEntry.erase((*it).vin.prevout);

                // and finally remove it from the list
//                it->FlagGovernanceItemsAsDirty();
                it = vZnodes.erase(it);
                fZnodesRemoved = true;
            } else {
                bool fAsk = pCurrentBlockIndex &&
                            (nAskForMnbRecovery > 0) &&
                            indexnodeSync.IsSynced() &&
                            it->IsNewStartRequired() &&
                            !IsMnbRecoveryRequested(hash);
                if(fAsk) {
                    // this mn is in a non-recoverable state and we haven't asked other nodes yet
                    std::set<CNetAddr> setRequested;
                    // calulate only once and only when it's needed
                    if(vecZnodeRanks.empty()) {
                        int nRandomBlockHeight = GetRandInt(pCurrentBlockIndex->nHeight);
                        vecZnodeRanks = GetZnodeRanks(nRandomBlockHeight);
                    }
                    bool fAskedForMnbRecovery = false;
                    // ask first MNB_RECOVERY_QUORUM_TOTAL indexnodes we can connect to and we haven't asked recently
                    for(int i = 0; setRequested.size() < MNB_RECOVERY_QUORUM_TOTAL && i < (int)vecZnodeRanks.size(); i++) {
                        // avoid banning
                        if(mWeAskedForZnodeListEntry.count(it->vin.prevout) && mWeAskedForZnodeListEntry[it->vin.prevout].count(vecZnodeRanks[i].second.addr)) continue;
                        // didn't ask recently, ok to ask now
                        CService addr = vecZnodeRanks[i].second.addr;
                        setRequested.insert(addr);
                        listScheduledMnbRequestConnections.push_back(std::make_pair(addr, hash));
                        fAskedForMnbRecovery = true;
                    }
                    if(fAskedForMnbRecovery) {
                        LogPrint("indexnode", "CZnodeMan::CheckAndRemove -- Recovery initiated, indexnode=%s\n", it->vin.prevout.ToStringShort());
                        nAskForMnbRecovery--;
                    }
                    // wait for mnb recovery replies for MNB_RECOVERY_WAIT_SECONDS seconds
                    mMnbRecoveryRequests[hash] = std::make_pair(GetTime() + MNB_RECOVERY_WAIT_SECONDS, setRequested);
                }
                ++it;
            }
        }

        // proces replies for INDEXNODE_NEW_START_REQUIRED indexnodes
        LogPrint("indexnode", "CZnodeMan::CheckAndRemove -- mMnbRecoveryGoodReplies size=%d\n", (int)mMnbRecoveryGoodReplies.size());
        std::map<uint256, std::vector<CZnodeBroadcast> >::iterator itMnbReplies = mMnbRecoveryGoodReplies.begin();
        while(itMnbReplies != mMnbRecoveryGoodReplies.end()){
            if(mMnbRecoveryRequests[itMnbReplies->first].first < GetTime()) {
                // all nodes we asked should have replied now
                if(itMnbReplies->second.size() >= MNB_RECOVERY_QUORUM_REQUIRED) {
                    // majority of nodes we asked agrees that this mn doesn't require new mnb, reprocess one of new mnbs
                    LogPrint("indexnode", "CZnodeMan::CheckAndRemove -- reprocessing mnb, indexnode=%s\n", itMnbReplies->second[0].vin.prevout.ToStringShort());
                    // mapSeenZnodeBroadcast.erase(itMnbReplies->first);
                    int nDos;
                    itMnbReplies->second[0].fRecovery = true;
                    CheckMnbAndUpdateZnodeList(NULL, itMnbReplies->second[0], nDos);
                }
                LogPrint("indexnode", "CZnodeMan::CheckAndRemove -- removing mnb recovery reply, indexnode=%s, size=%d\n", itMnbReplies->second[0].vin.prevout.ToStringShort(), (int)itMnbReplies->second.size());
                mMnbRecoveryGoodReplies.erase(itMnbReplies++);
            } else {
                ++itMnbReplies;
            }
        }
    }
    {
        // no need for cm_main below
        LOCK2(cs_main, cs);

        std::map<uint256, std::pair< int64_t, std::set<CNetAddr> > >::iterator itMnbRequest = mMnbRecoveryRequests.begin();
        while(itMnbRequest != mMnbRecoveryRequests.end()){
            // Allow this mnb to be re-verified again after MNB_RECOVERY_RETRY_SECONDS seconds
            // if mn is still in INDEXNODE_NEW_START_REQUIRED state.
            if(GetTime() - itMnbRequest->second.first > MNB_RECOVERY_RETRY_SECONDS) {
                mMnbRecoveryRequests.erase(itMnbRequest++);
            } else {
                ++itMnbRequest;
            }
        }

        // check who's asked for the Znode list
        std::map<CNetAddr, int64_t>::iterator it1 = mAskedUsForZnodeList.begin();
        while(it1 != mAskedUsForZnodeList.end()){
            if((*it1).second < GetTime()) {
                mAskedUsForZnodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check who we asked for the Znode list
        it1 = mWeAskedForZnodeList.begin();
        while(it1 != mWeAskedForZnodeList.end()){
            if((*it1).second < GetTime()){
                mWeAskedForZnodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check which Znodes we've asked for
        std::map<COutPoint, std::map<CNetAddr, int64_t> >::iterator it2 = mWeAskedForZnodeListEntry.begin();
        while(it2 != mWeAskedForZnodeListEntry.end()){
            std::map<CNetAddr, int64_t>::iterator it3 = it2->second.begin();
            while(it3 != it2->second.end()){
                if(it3->second < GetTime()){
                    it2->second.erase(it3++);
                } else {
                    ++it3;
                }
            }
            if(it2->second.empty()) {
                mWeAskedForZnodeListEntry.erase(it2++);
            } else {
                ++it2;
            }
        }

        std::map<CNetAddr, CZnodeVerification>::iterator it3 = mWeAskedForVerification.begin();
        while(it3 != mWeAskedForVerification.end()){
            if(it3->second.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS) {
                mWeAskedForVerification.erase(it3++);
            } else {
                ++it3;
            }
        }

        // NOTE: do not expire mapSeenZnodeBroadcast entries here, clean them on mnb updates!

        // remove expired mapSeenZnodePing
        std::map<uint256, CZnodePing>::iterator it4 = mapSeenZnodePing.begin();
        while(it4 != mapSeenZnodePing.end()){
            if((*it4).second.IsExpired()) {
                LogPrint("indexnode", "CZnodeMan::CheckAndRemove -- Removing expired Znode ping: hash=%s\n", (*it4).second.GetHash().ToString());
                mapSeenZnodePing.erase(it4++);
            } else {
                ++it4;
            }
        }

        // remove expired mapSeenZnodeVerification
        std::map<uint256, CZnodeVerification>::iterator itv2 = mapSeenZnodeVerification.begin();
        while(itv2 != mapSeenZnodeVerification.end()){
            if((*itv2).second.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS){
                LogPrint("indexnode", "CZnodeMan::CheckAndRemove -- Removing expired Znode verification: hash=%s\n", (*itv2).first.ToString());
                mapSeenZnodeVerification.erase(itv2++);
            } else {
                ++itv2;
            }
        }

        LogPrintf("CZnodeMan::CheckAndRemove -- %s\n", ToString());

        if(fZnodesRemoved) {
            CheckAndRebuildZnodeIndex();
        }
    }

    if(fZnodesRemoved) {
        NotifyZnodeUpdates();
    }
}

void CZnodeMan::Clear()
{
    LOCK(cs);
    vZnodes.clear();
    mAskedUsForZnodeList.clear();
    mWeAskedForZnodeList.clear();
    mWeAskedForZnodeListEntry.clear();
    mapSeenZnodeBroadcast.clear();
    mapSeenZnodePing.clear();
    nDsqCount = 0;
    nLastWatchdogVoteTime = 0;
    indexZnodes.Clear();
    indexZnodesOld.Clear();
}

int CZnodeMan::CountZnodes(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? znpayments.GetMinZnodePaymentsProto() : nProtocolVersion;

    BOOST_FOREACH(CZnode& mn, vZnodes) {
        if(mn.nProtocolVersion < nProtocolVersion) continue;
        nCount++;
    }

    return nCount;
}

int CZnodeMan::CountEnabled(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? znpayments.GetMinZnodePaymentsProto() : nProtocolVersion;

    BOOST_FOREACH(CZnode& mn, vZnodes) {
        if(mn.nProtocolVersion < nProtocolVersion || !mn.IsEnabled()) continue;
        nCount++;
    }

    return nCount;
}

/* Only IPv4 indexnodes are allowed in 12.1, saving this for later
int CZnodeMan::CountByIP(int nNetworkType)
{
    LOCK(cs);
    int nNodeCount = 0;

    BOOST_FOREACH(CZnode& mn, vZnodes)
        if ((nNetworkType == NET_IPV4 && mn.addr.IsIPv4()) ||
            (nNetworkType == NET_TOR  && mn.addr.IsTor())  ||
            (nNetworkType == NET_IPV6 && mn.addr.IsIPv6())) {
                nNodeCount++;
        }

    return nNodeCount;
}
*/

void CZnodeMan::DsegUpdate(CNode* pnode)
{
    LOCK(cs);

    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(!(pnode->addr.IsRFC1918() || pnode->addr.IsLocal())) {
            std::map<CNetAddr, int64_t>::iterator it = mWeAskedForZnodeList.find(pnode->addr);
            if(it != mWeAskedForZnodeList.end() && GetTime() < (*it).second) {
                LogPrintf("CZnodeMan::DsegUpdate -- we already asked %s for the list; skipping...\n", pnode->addr.ToString());
                return;
            }
        }
    }
    
    g_connman->PushMessage(pnode, CNetMsgMaker(LEGACY_INDEXNODES_PROTOCOL_VERSION).Make(NetMsgType::DSEG, CTxIn()));
    int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
    mWeAskedForZnodeList[pnode->addr] = askAgain;

    LogPrint("indexnode", "CZnodeMan::DsegUpdate -- asked %s for the list\n", pnode->addr.ToString());
}

CZnode* CZnodeMan::Find(const std::string &txHash, const std::string &outputIndex)
{
    LOCK(cs);

    BOOST_FOREACH(CZnode& mn, vZnodes)
    {
        COutPoint outpoint = mn.vin.prevout;

        if(txHash==outpoint.hash.ToString().substr(0,64) &&
           outputIndex==to_string(outpoint.n))
            return &mn;
    }
    return NULL;
}

CZnode* CZnodeMan::Find(const CScript &payee)
{
    LOCK(cs);

    BOOST_FOREACH(CZnode& mn, vZnodes)
    {
        if(GetScriptForDestination(mn.pubKeyCollateralAddress.GetID()) == payee)
            return &mn;
    }
    return NULL;
}

CZnode* CZnodeMan::Find(const CTxIn &vin)
{
    LOCK(cs);

    BOOST_FOREACH(CZnode& mn, vZnodes)
    {
        if(mn.vin.prevout == vin.prevout)
            return &mn;
    }
    return NULL;
}

CZnode* CZnodeMan::Find(const CPubKey &pubKeyZnode)
{
    LOCK(cs);

    BOOST_FOREACH(CZnode& mn, vZnodes)
    {
        if(mn.pubKeyZnode == pubKeyZnode)
            return &mn;
    }
    return NULL;
}

bool CZnodeMan::Get(const CPubKey& pubKeyZnode, CZnode& indexnode)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    CZnode* pMN = Find(pubKeyZnode);
    if(!pMN)  {
        return false;
    }
    indexnode = *pMN;
    return true;
}

bool CZnodeMan::Get(const CTxIn& vin, CZnode& indexnode)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    CZnode* pMN = Find(vin);
    if(!pMN)  {
        return false;
    }
    indexnode = *pMN;
    return true;
}

indexnode_info_t CZnodeMan::GetZnodeInfo(const CTxIn& vin)
{
    indexnode_info_t info;
    LOCK(cs);
    CZnode* pMN = Find(vin);
    if(!pMN)  {
        return info;
    }
    info = pMN->GetInfo();
    return info;
}

indexnode_info_t CZnodeMan::GetZnodeInfo(const CPubKey& pubKeyZnode)
{
    indexnode_info_t info;
    LOCK(cs);
    CZnode* pMN = Find(pubKeyZnode);
    if(!pMN)  {
        return info;
    }
    info = pMN->GetInfo();
    return info;
}

bool CZnodeMan::Has(const CTxIn& vin)
{
    LOCK(cs);
    CZnode* pMN = Find(vin);
    return (pMN != NULL);
}

char* CZnodeMan::GetNotQualifyReason(CZnode& mn, int nBlockHeight, bool fFilterSigTime, int nMnCount)
{
    if (!mn.IsValidForPayment()) {
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'not valid for payment'");
        return reasonStr;
    }
    // //check protocol version
    if (mn.nProtocolVersion < znpayments.GetMinZnodePaymentsProto()) {
        // LogPrintf("Invalid nProtocolVersion!\n");
        // LogPrintf("mn.nProtocolVersion=%s!\n", mn.nProtocolVersion);
        // LogPrintf("znpayments.GetMinZnodePaymentsProto=%s!\n", znpayments.GetMinZnodePaymentsProto());
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'Invalid nProtocolVersion', nProtocolVersion=%d", mn.nProtocolVersion);
        return reasonStr;
    }
    //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
    if (znpayments.IsScheduled(mn, nBlockHeight)) {
        // LogPrintf("znpayments.IsScheduled!\n");
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'is scheduled'");
        return reasonStr;
    }
    //it's too new, wait for a cycle
    if (fFilterSigTime && mn.sigTime + (nMnCount * 2.6 * 60) > GetAdjustedTime()) {
        // LogPrintf("it's too new, wait for a cycle!\n");
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'too new', sigTime=%s, will be qualifed after=%s",
                DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime).c_str(), DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime + (nMnCount * 2.6 * 60)).c_str());
        return reasonStr;
    }
    //make sure it has at least as many confirmations as there are indexnodes
    if (mn.GetCollateralAge() < nMnCount) {
        // LogPrintf("mn.GetCollateralAge()=%s!\n", mn.GetCollateralAge());
        // LogPrintf("nMnCount=%s!\n", nMnCount);
        char* reasonStr = new char[256];
        sprintf(reasonStr, "false: 'collateralAge < znCount', collateralAge=%d, znCount=%d", mn.GetCollateralAge(), nMnCount);
        return reasonStr;
    }
    return NULL;
}

//
// Deterministically select the oldest/best indexnode to pay on the network
//
CZnode* CZnodeMan::GetNextZnodeInQueueForPayment(bool fFilterSigTime, int& nCount)
{
    if(!pCurrentBlockIndex) {
        nCount = 0;
        return NULL;
    }
    return GetNextZnodeInQueueForPayment(pCurrentBlockIndex->nHeight, fFilterSigTime, nCount);
}

CZnode* CZnodeMan::GetNextZnodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCount)
{
    // Need LOCK2 here to ensure consistent locking order because the GetBlockHash call below locks cs_main
    LOCK2(cs_main, mempool.cs);
    LOCK(cs);

    CZnode *pBestZnode = NULL;
    std::vector<std::pair<int, CZnode*> > vecZnodeLastPaid;

    /*
        Make a vector with all of the last paid times
    */
    int nMnCount = CountEnabled();
    int index = 0;
    BOOST_FOREACH(CZnode &mn, vZnodes)
    {
        index += 1;
        // LogPrintf("index=%s, mn=%s\n", index, mn.ToString());
        /*if (!mn.IsValidForPayment()) {
            LogPrint("indexnodeman", "Znode, %s, addr(%s), not-qualified: 'not valid for payment'\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString());
            continue;
        }
        // //check protocol version
        if (mn.nProtocolVersion < znpayments.GetMinZnodePaymentsProto()) {
            // LogPrintf("Invalid nProtocolVersion!\n");
            // LogPrintf("mn.nProtocolVersion=%s!\n", mn.nProtocolVersion);
            // LogPrintf("znpayments.GetMinZnodePaymentsProto=%s!\n", znpayments.GetMinZnodePaymentsProto());
            LogPrint("indexnodeman", "Znode, %s, addr(%s), not-qualified: 'invalid nProtocolVersion'\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString());
            continue;
        }
        //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
        if (znpayments.IsScheduled(mn, nBlockHeight)) {
            // LogPrintf("znpayments.IsScheduled!\n");
            LogPrint("indexnodeman", "Znode, %s, addr(%s), not-qualified: 'IsScheduled'\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString());
            continue;
        }
        //it's too new, wait for a cycle
        if (fFilterSigTime && mn.sigTime + (nMnCount * 2.6 * 60) > GetAdjustedTime()) {
            // LogPrintf("it's too new, wait for a cycle!\n");
            LogPrint("indexnodeman", "Znode, %s, addr(%s), not-qualified: 'it's too new, wait for a cycle!', sigTime=%s, will be qualifed after=%s\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString(), DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime).c_str(), DateTimeStrFormat("%Y-%m-%d %H:%M UTC", mn.sigTime + (nMnCount * 2.6 * 60)).c_str());
            continue;
        }
        //make sure it has at least as many confirmations as there are indexnodes
        if (mn.GetCollateralAge() < nMnCount) {
            // LogPrintf("mn.GetCollateralAge()=%s!\n", mn.GetCollateralAge());
            // LogPrintf("nMnCount=%s!\n", nMnCount);
            LogPrint("indexnodeman", "Znode, %s, addr(%s), not-qualified: 'mn.GetCollateralAge() < nMnCount', CollateralAge=%d, nMnCount=%d\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString(), mn.GetCollateralAge(), nMnCount);
            continue;
        }*/
        char* reasonStr = GetNotQualifyReason(mn, nBlockHeight, fFilterSigTime & (Params().NetworkIDString() != CBaseChainParams::REGTEST), nMnCount);
        if (reasonStr != NULL) {
            LogPrint("indexnodeman", "Znode, %s, addr(%s), qualify %s\n",
                     mn.vin.prevout.ToStringShort(), CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString(), reasonStr);
            delete [] reasonStr;
            continue;
        }
        vecZnodeLastPaid.push_back(std::make_pair(mn.GetLastPaidBlock(), &mn));
    }
    nCount = (int)vecZnodeLastPaid.size();

    //when the network is in the process of upgrading, don't penalize nodes that recently restarted
    if(fFilterSigTime && nCount < nMnCount / 3) {
        // LogPrintf("Need Return, nCount=%s, nMnCount/3=%s\n", nCount, nMnCount/3);
        return GetNextZnodeInQueueForPayment(nBlockHeight, false, nCount);
    }

    // Sort them low to high
    sort(vecZnodeLastPaid.begin(), vecZnodeLastPaid.end(), CompareLastPaidBlock());

    uint256 blockHash;
    if(!GetBlockHash(blockHash, nBlockHeight - 101)) {
        LogPrintf("CZnode::GetNextZnodeInQueueForPayment -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", nBlockHeight - 101);
        return NULL;
    }
    // Look at 1/10 of the oldest nodes (by last payment), calculate their scores and pay the best one
    //  -- This doesn't look at who is being paid in the +8-10 blocks, allowing for double payments very rarely
    //  -- 1/100 payments should be a double payment on mainnet - (1/(3000/10))*2
    //  -- (chance per block * chances before IsScheduled will fire)
    int nTenthNetwork = nMnCount/10;
    int nCountTenth = 0;
    arith_uint256 nHighest = 0;
    BOOST_FOREACH (PAIRTYPE(int, CZnode*)& s, vecZnodeLastPaid){
        arith_uint256 nScore = s.second->CalculateScore(blockHash);
        if(nScore > nHighest){
            nHighest = nScore;
            pBestZnode = s.second;
        }
        nCountTenth++;
        if(nCountTenth >= nTenthNetwork) break;
    }
    return pBestZnode;
}

CZnode* CZnodeMan::FindRandomNotInVec(const std::vector<CTxIn> &vecToExclude, int nProtocolVersion)
{
    LOCK(cs);

    nProtocolVersion = nProtocolVersion == -1 ? znpayments.GetMinZnodePaymentsProto() : nProtocolVersion;

    int nCountEnabled = CountEnabled(nProtocolVersion);
    int nCountNotExcluded = nCountEnabled - vecToExclude.size();

    LogPrintf("CZnodeMan::FindRandomNotInVec -- %d enabled indexnodes, %d indexnodes to choose from\n", nCountEnabled, nCountNotExcluded);
    if(nCountNotExcluded < 1) return NULL;

    // fill a vector of pointers
    std::vector<CZnode*> vpZnodesShuffled;
    BOOST_FOREACH(CZnode &mn, vZnodes) {
        vpZnodesShuffled.push_back(&mn);
    }

    InsecureRand insecureRand;
    // shuffle pointers
    std::random_shuffle(vpZnodesShuffled.begin(), vpZnodesShuffled.end(), insecureRand);
    bool fExclude;

    // loop through
    BOOST_FOREACH(CZnode* pmn, vpZnodesShuffled) {
        if(pmn->nProtocolVersion < nProtocolVersion || !pmn->IsEnabled()) continue;
        fExclude = false;
        BOOST_FOREACH(const CTxIn &txinToExclude, vecToExclude) {
            if(pmn->vin.prevout == txinToExclude.prevout) {
                fExclude = true;
                break;
            }
        }
        if(fExclude) continue;
        // found the one not in vecToExclude
        LogPrint("indexnode", "CZnodeMan::FindRandomNotInVec -- found, indexnode=%s\n", pmn->vin.prevout.ToStringShort());
        return pmn;
    }

    LogPrint("indexnode", "CZnodeMan::FindRandomNotInVec -- failed\n");
    return NULL;
}

int CZnodeMan::GetZnodeRank(const CTxIn& vin, int nBlockHeight, int nMinProtocol, bool fOnlyActive)
{
    std::vector<std::pair<int64_t, CZnode*> > vecZnodeScores;

    //make sure we know about this block
    uint256 blockHash = uint256();
    if(!GetBlockHash(blockHash, nBlockHeight)) return -1;

    LOCK(cs);

    // scan for winner
    BOOST_FOREACH(CZnode& mn, vZnodes) {
        if(mn.nProtocolVersion < nMinProtocol) continue;
        if(fOnlyActive) {
            if(!mn.IsEnabled()) continue;
        }
        else {
            if(!mn.IsValidForPayment()) continue;
        }
        int64_t nScore = mn.CalculateScore(blockHash).GetCompact(false);

        vecZnodeScores.push_back(std::make_pair(nScore, &mn));
    }

    sort(vecZnodeScores.rbegin(), vecZnodeScores.rend(), CompareScoreMN());

    int nRank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CZnode*)& scorePair, vecZnodeScores) {
        nRank++;
        if(scorePair.second->vin.prevout == vin.prevout) return nRank;
    }

    return -1;
}

std::vector<std::pair<int, CZnode> > CZnodeMan::GetZnodeRanks(int nBlockHeight, int nMinProtocol)
{
    std::vector<std::pair<int64_t, CZnode*> > vecZnodeScores;
    std::vector<std::pair<int, CZnode> > vecZnodeRanks;

    //make sure we know about this block
    uint256 blockHash = uint256();
    if(!GetBlockHash(blockHash, nBlockHeight)) return vecZnodeRanks;

    LOCK(cs);

    // scan for winner
    BOOST_FOREACH(CZnode& mn, vZnodes) {

        if(mn.nProtocolVersion < nMinProtocol || !mn.IsEnabled()) continue;

        int64_t nScore = mn.CalculateScore(blockHash).GetCompact(false);

        vecZnodeScores.push_back(std::make_pair(nScore, &mn));
    }

    sort(vecZnodeScores.rbegin(), vecZnodeScores.rend(), CompareScoreMN());

    int nRank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CZnode*)& s, vecZnodeScores) {
        nRank++;
        vecZnodeRanks.push_back(std::make_pair(nRank, *s.second));
    }

    return vecZnodeRanks;
}

CZnode* CZnodeMan::GetZnodeByRank(int nRank, int nBlockHeight, int nMinProtocol, bool fOnlyActive)
{
    std::vector<std::pair<int64_t, CZnode*> > vecZnodeScores;

    LOCK(cs);

    uint256 blockHash;
    if(!GetBlockHash(blockHash, nBlockHeight)) {
        LogPrintf("CZnode::GetZnodeByRank -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", nBlockHeight);
        return NULL;
    }

    // Fill scores
    BOOST_FOREACH(CZnode& mn, vZnodes) {

        if(mn.nProtocolVersion < nMinProtocol) continue;
        if(fOnlyActive && !mn.IsEnabled()) continue;

        int64_t nScore = mn.CalculateScore(blockHash).GetCompact(false);

        vecZnodeScores.push_back(std::make_pair(nScore, &mn));
    }

    sort(vecZnodeScores.rbegin(), vecZnodeScores.rend(), CompareScoreMN());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CZnode*)& s, vecZnodeScores){
        rank++;
        if(rank == nRank) {
            return s.second;
        }
    }

    return NULL;
}

void CZnodeMan::ProcessZnodeConnections()
{
    //we don't care about this for regtest
    if(Params().NetworkIDString() == CBaseChainParams::REGTEST) return;

    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes) {
        if(pnode->fZnode) {
            if(darkSendPool.pSubmittedToZnode != NULL && pnode->addr == darkSendPool.pSubmittedToZnode->addr) continue;
            // LogPrintf("Closing Znode connection: peer=%d, addr=%s\n", pnode->id, pnode->addr.ToString());
            pnode->fDisconnect = true;
        }
    }
}

std::pair<CService, std::set<uint256> > CZnodeMan::PopScheduledMnbRequestConnection()
{
    LOCK(cs);
    if(listScheduledMnbRequestConnections.empty()) {
        return std::make_pair(CService(), std::set<uint256>());
    }

    std::set<uint256> setResult;

    listScheduledMnbRequestConnections.sort();
    std::pair<CService, uint256> pairFront = listScheduledMnbRequestConnections.front();

    // squash hashes from requests with the same CService as the first one into setResult
    std::list< std::pair<CService, uint256> >::iterator it = listScheduledMnbRequestConnections.begin();
    while(it != listScheduledMnbRequestConnections.end()) {
        if(pairFront.first == it->first) {
            setResult.insert(it->second);
            it = listScheduledMnbRequestConnections.erase(it);
        } else {
            // since list is sorted now, we can be sure that there is no more hashes left
            // to ask for from this addr
            break;
        }
    }
    return std::make_pair(pairFront.first, setResult);
}


void CZnodeMan::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{

//    LogPrint("indexnode", "CZnodeMan::ProcessMessage, strCommand=%s\n", strCommand);
    if(fLiteMode) return; // disable all Index specific functionality
    if(!indexnodeSync.IsBlockchainSynced()) return;

    if (strCommand == NetMsgType::MNANNOUNCE) { //Znode Broadcast
        CZnodeBroadcast mnb;
        vRecv >> mnb;

        pfrom->setAskFor.erase(mnb.GetHash());

        LogPrintf("MNANNOUNCE -- Znode announce, indexnode=%s\n", mnb.vin.prevout.ToStringShort());

        int nDos = 0;

        if (CheckMnbAndUpdateZnodeList(pfrom, mnb, nDos)) {
            // use announced Znode as a peer
            g_connman->AddNewAddress(CAddress(mnb.addr, NODE_NETWORK), pfrom->addr, 2*60*60);
        } else if(nDos > 0) {
            Misbehaving(pfrom->GetId(), nDos);
        }

        if(fZnodesAdded) {
            NotifyZnodeUpdates();
        }
    } else if (strCommand == NetMsgType::MNPING) { //Znode Ping

        CZnodePing mnp;
        vRecv >> mnp;

        uint256 nHash = mnp.GetHash();

        pfrom->setAskFor.erase(nHash);

        LogPrint("indexnode", "MNPING -- Znode ping, indexnode=%s\n", mnp.vin.prevout.ToStringShort());

        // Need LOCK2 here to ensure consistent locking order because the CheckAndUpdate call below locks cs_main
        LOCK2(cs_main, cs);

        if(mapSeenZnodePing.count(nHash)) return; //seen
        mapSeenZnodePing.insert(std::make_pair(nHash, mnp));

        LogPrint("indexnode", "MNPING -- Znode ping, indexnode=%s new\n", mnp.vin.prevout.ToStringShort());

        // see if we have this Znode
        CZnode* pmn = mnodeman.Find(mnp.vin);

        // too late, new MNANNOUNCE is required
        if(pmn && pmn->IsNewStartRequired()) return;

        int nDos = 0;
        if(mnp.CheckAndUpdate(pmn, false, nDos)) return;

        if(nDos > 0) {
            // if anything significant failed, mark that node
            Misbehaving(pfrom->GetId(), nDos);
        } else if(pmn != NULL) {
            // nothing significant failed, mn is a known one too
            return;
        }

        // something significant is broken or mn is unknown,
        // we might have to ask for a indexnode entry once
        AskForMN(pfrom, mnp.vin);

    } else if (strCommand == NetMsgType::DSEG) { //Get Znode list or specific entry
        // Ignore such requests until we are fully synced.
        // We could start processing this after indexnode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!indexnodeSync.IsSynced()) return;

        CTxIn vin;
        vRecv >> vin;

        LogPrint("indexnode", "DSEG -- Znode list, indexnode=%s\n", vin.prevout.ToStringShort());

        LOCK(cs);

        if(vin == CTxIn()) { //only should ask for this once
            //local network
            bool isLocal = (pfrom->addr.IsRFC1918() || pfrom->addr.IsLocal());

            if(!isLocal && Params().NetworkIDString() == CBaseChainParams::MAIN) {
                std::map<CNetAddr, int64_t>::iterator i = mAskedUsForZnodeList.find(pfrom->addr);
                if (i != mAskedUsForZnodeList.end()){
                    int64_t t = (*i).second;
                    if (GetTime() < t) {
                        Misbehaving(pfrom->GetId(), 34);
                        LogPrintf("DSEG -- peer already asked me for the list, peer=%d\n", pfrom->id);
                        return;
                    }
                }
                int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
                mAskedUsForZnodeList[pfrom->addr] = askAgain;
            }
        } //else, asking for a specific node which is ok

        int nInvCount = 0;

        BOOST_FOREACH(CZnode& mn, vZnodes) {
            if (vin != CTxIn() && vin != mn.vin) continue; // asked for specific vin but we are not there yet
            if (Params().NetworkIDString() != CBaseChainParams::REGTEST)
                if (mn.addr.IsRFC1918() || mn.addr.IsLocal()) continue; // do not send local network indexnode
            if (mn.IsUpdateRequired()) continue; // do not send outdated indexnodes

            LogPrint("indexnode", "DSEG -- Sending Znode entry: indexnode=%s  addr=%s\n", mn.vin.prevout.ToStringShort(), mn.addr.ToString());
            CZnodeBroadcast mnb = CZnodeBroadcast(mn);
            uint256 hash = mnb.GetHash();
            pfrom->PushInventory(CInv(MSG_INDEXNODE_ANNOUNCE, hash));
            pfrom->PushInventory(CInv(MSG_INDEXNODE_PING, mn.lastPing.GetHash()));
            nInvCount++;

            if (!mapSeenZnodeBroadcast.count(hash)) {
                mapSeenZnodeBroadcast.insert(std::make_pair(hash, std::make_pair(GetTime(), mnb)));
            }

            if (vin == mn.vin) {
                LogPrintf("DSEG -- Sent 1 Znode inv to peer %d\n", pfrom->id);
                return;
            }
        }

        if(vin == CTxIn()) {
            g_connman->PushMessage(pfrom, CNetMsgMaker(LEGACY_INDEXNODES_PROTOCOL_VERSION).Make(NetMsgType::SYNCSTATUSCOUNT, INDEXNODE_SYNC_LIST, nInvCount));
            LogPrintf("DSEG -- Sent %d Znode invs to peer %d\n", nInvCount, pfrom->id);
            return;
        }
        // smth weird happen - someone asked us for vin we have no idea about?
        LogPrint("indexnode", "DSEG -- No invs sent to peer %d\n", pfrom->id);

    } else if (strCommand == NetMsgType::MNVERIFY) { // Znode Verify

        // Need LOCK2 here to ensure consistent locking order because the all functions below call GetBlockHash which locks cs_main
        LOCK2(cs_main, cs);

        CZnodeVerification mnv;
        vRecv >> mnv;

        if(mnv.vchSig1.empty()) {
            // CASE 1: someone asked me to verify myself /IP we are using/
            SendVerifyReply(pfrom, mnv);
        } else if (mnv.vchSig2.empty()) {
            // CASE 2: we _probably_ got verification we requested from some indexnode
            ProcessVerifyReply(pfrom, mnv);
        } else {
            // CASE 3: we _probably_ got verification broadcast signed by some indexnode which verified another one
            ProcessVerifyBroadcast(pfrom, mnv);
        }
    }
}

// Verification of indexnodes via unique direct requests.

void CZnodeMan::DoFullVerificationStep()
{
    if(activeZnode.vin == CTxIn()) return;
    if(!indexnodeSync.IsSynced()) return;

    std::vector<std::pair<int, CZnode> > vecZnodeRanks = GetZnodeRanks(pCurrentBlockIndex->nHeight - 1, MIN_POSE_PROTO_VERSION);

    std::vector<CAddress> vAddr;
    int nCount = 0;

    {
    LOCK2(cs_main, cs);

    int nMyRank = -1;
    int nRanksTotal = (int)vecZnodeRanks.size();

    // send verify requests only if we are in top MAX_POSE_RANK
    std::vector<std::pair<int, CZnode> >::iterator it = vecZnodeRanks.begin();
    while(it != vecZnodeRanks.end()) {
        if(it->first > MAX_POSE_RANK) {
            LogPrint("indexnode", "CZnodeMan::DoFullVerificationStep -- Must be in top %d to send verify request\n",
                        (int)MAX_POSE_RANK);
            return;
        }
        if(it->second.vin == activeZnode.vin) {
            nMyRank = it->first;
            LogPrint("indexnode", "CZnodeMan::DoFullVerificationStep -- Found self at rank %d/%d, verifying up to %d indexnodes\n",
                        nMyRank, nRanksTotal, (int)MAX_POSE_CONNECTIONS);
            break;
        }
        ++it;
    }

    // edge case: list is too short and this indexnode is not enabled
    if(nMyRank == -1) return;

    // send verify requests to up to MAX_POSE_CONNECTIONS indexnodes
    // starting from MAX_POSE_RANK + nMyRank and using MAX_POSE_CONNECTIONS as a step
    int nOffset = MAX_POSE_RANK + nMyRank - 1;
    if(nOffset >= (int)vecZnodeRanks.size()) return;

    std::vector<CZnode*> vSortedByAddr;
    BOOST_FOREACH(CZnode& mn, vZnodes) {
        vSortedByAddr.push_back(&mn);
    }

    sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

    it = vecZnodeRanks.begin() + nOffset;
    while(it != vecZnodeRanks.end()) {
        if(it->second.IsPoSeVerified() || it->second.IsPoSeBanned()) {
            LogPrint("indexnode", "CZnodeMan::DoFullVerificationStep -- Already %s%s%s indexnode %s address %s, skipping...\n",
                        it->second.IsPoSeVerified() ? "verified" : "",
                        it->second.IsPoSeVerified() && it->second.IsPoSeBanned() ? " and " : "",
                        it->second.IsPoSeBanned() ? "banned" : "",
                        it->second.vin.prevout.ToStringShort(), it->second.addr.ToString());
            nOffset += MAX_POSE_CONNECTIONS;
            if(nOffset >= (int)vecZnodeRanks.size()) break;
            it += MAX_POSE_CONNECTIONS;
            continue;
        }
        LogPrint("indexnode", "CZnodeMan::DoFullVerificationStep -- Verifying indexnode %s rank %d/%d address %s\n",
                    it->second.vin.prevout.ToStringShort(), it->first, nRanksTotal, it->second.addr.ToString());
        CAddress addr = CAddress(it->second.addr, NODE_NETWORK);
        if(CheckVerifyRequestAddr(addr, *g_connman)) {
            vAddr.push_back(addr);
            if((int)vAddr.size() >= MAX_POSE_CONNECTIONS) break;
        }
        nOffset += MAX_POSE_CONNECTIONS;
        if(nOffset >= (int)vecZnodeRanks.size()) break;
        it += MAX_POSE_CONNECTIONS;
    }

    } // LOCK2(cs_main, cs)

    for (const auto& addr : vAddr) {
        PrepareVerifyRequest(addr, *g_connman);
    }

    LogPrint("indexnode", "CZnodeMan::DoFullVerificationStep -- Sent verification requests to %d indexnodes\n", nCount);
}

// This function tries to find indexnodes with the same addr,
// find a verified one and ban all the other. If there are many nodes
// with the same addr but none of them is verified yet, then none of them are banned.
// It could take many times to run this before most of the duplicate nodes are banned.

void CZnodeMan::CheckSameAddr()
{
    if(!indexnodeSync.IsSynced() || vZnodes.empty()) return;

    std::vector<CZnode*> vBan;
    std::vector<CZnode*> vSortedByAddr;

    {
        LOCK(cs);

        CZnode* pprevZnode = NULL;
        CZnode* pverifiedZnode = NULL;

        BOOST_FOREACH(CZnode& mn, vZnodes) {
            vSortedByAddr.push_back(&mn);
        }

        sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

        BOOST_FOREACH(CZnode* pmn, vSortedByAddr) {
            // check only (pre)enabled indexnodes
            if(!pmn->IsEnabled() && !pmn->IsPreEnabled()) continue;
            // initial step
            if(!pprevZnode) {
                pprevZnode = pmn;
                pverifiedZnode = pmn->IsPoSeVerified() ? pmn : NULL;
                continue;
            }
            // second+ step
            if(pmn->addr == pprevZnode->addr) {
                if(pverifiedZnode) {
                    // another indexnode with the same ip is verified, ban this one
                    vBan.push_back(pmn);
                } else if(pmn->IsPoSeVerified()) {
                    // this indexnode with the same ip is verified, ban previous one
                    vBan.push_back(pprevZnode);
                    // and keep a reference to be able to ban following indexnodes with the same ip
                    pverifiedZnode = pmn;
                }
            } else {
                pverifiedZnode = pmn->IsPoSeVerified() ? pmn : NULL;
            }
            pprevZnode = pmn;
        }
    }

    // ban duplicates
    BOOST_FOREACH(CZnode* pmn, vBan) {
        LogPrintf("CZnodeMan::CheckSameAddr -- increasing PoSe ban score for indexnode %s\n", pmn->vin.prevout.ToStringShort());
        pmn->IncreasePoSeBanScore();
    }
}

bool CZnodeMan::CheckVerifyRequestAddr(const CAddress& addr, CConnman& connman)
{
    if(netfulfilledman.HasFulfilledRequest(addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request")) {
        // we already asked for verification, not a good idea to do this too often, skip it
        LogPrint("indexnode", "CZnodeMan::%s -- too many requests, skipping... addr=%s\n", __func__, addr.ToString());
        return false;
    }

    return !connman.IsMasternodeOrDisconnectRequested(addr);
}

void CZnodeMan::PrepareVerifyRequest(const CAddress& addr, CConnman& connman)
{
    int nHeight;
    {
        LOCK(cs_main);
        nHeight = chainActive.Height();
    }

    connman.AddPendingMasternode(addr);
    // use random nonce, store it and require node to reply with correct one later
    CZnodeVerification mnv(addr, GetRandInt(999999), nHeight - 1);
    LOCK(cs_mapPendingMNV);
    mapPendingMNV.insert(std::make_pair(addr, std::make_pair(GetTime(), mnv)));
    LogPrintf("CZnodeMan::%s -- verifying node using nonce %d addr=%s\n", __func__, mnv.nonce, addr.ToString());
}

void CZnodeMan::ProcessPendingMnvRequests(CConnman& connman)
{
    LOCK(cs_mapPendingMNV);

    std::map<CService, std::pair<int64_t, CZnodeVerification> >::iterator itPendingMNV = mapPendingMNV.begin();

    while (itPendingMNV != mapPendingMNV.end()) {
        bool fDone = connman.ForNode(itPendingMNV->first, [&](CNode* pnode) {
            netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request");
            // use random nonce, store it and require node to reply with correct one later
            mWeAskedForVerification[pnode->addr] = itPendingMNV->second.second;
            LogPrint("indexnode", "-- verifying node using nonce %d addr=%s\n", itPendingMNV->second.second.nonce, pnode->addr.ToString());
            CNetMsgMaker msgMaker(LEGACY_INDEXNODES_PROTOCOL_VERSION);
            connman.PushMessage(pnode, msgMaker.Make(NetMsgType::MNVERIFY, itPendingMNV->second.second));
            return true;
        });

        int64_t nTimeAdded = itPendingMNV->second.first;
        if (fDone || (GetTime() - nTimeAdded > 15)) {
            if (!fDone) {
                LogPrint("indexnode", "CZnodeMan::%s -- failed to connect to %s\n", __func__, itPendingMNV->first.ToString());
            }
            mapPendingMNV.erase(itPendingMNV++);
        } else {
            ++itPendingMNV;
        }
    }
}

void CZnodeMan::SendVerifyReply(CNode* pnode, CZnodeVerification& mnv)
{
    // only indexnodes can sign this, why would someone ask regular node?
    if(!fMasternodeMode) {
        // do not ban, malicious node might be using my IP
        // and trying to confuse the node which tries to verify it
        return;
    }

    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-reply")) {
//        // peer should not ask us that often
        LogPrintf("ZnodeMan::SendVerifyReply -- ERROR: peer already asked me recently, peer=%d\n", pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        LogPrintf("ZnodeMan::SendVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

    std::string strMessage = strprintf("%s%d%s", activeZnode.service.ToString(), mnv.nonce, blockHash.ToString());

    if(!darkSendSigner.SignMessage(strMessage, mnv.vchSig1, activeZnode.keyZnode)) {
        LogPrintf("ZnodeMan::SendVerifyReply -- SignMessage() failed\n");
        return;
    }

    std::string strError;

    if(!darkSendSigner.VerifyMessage(activeZnode.pubKeyZnode, mnv.vchSig1, strMessage, strError)) {
        LogPrintf("ZnodeMan::SendVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
        return;
    }

    g_connman->PushMessage(pnode, CNetMsgMaker(LEGACY_INDEXNODES_PROTOCOL_VERSION).Make(NetMsgType::MNVERIFY, mnv));
    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-reply");
}

void CZnodeMan::ProcessVerifyReply(CNode* pnode, CZnodeVerification& mnv)
{
    std::string strError;

    // did we even ask for it? if that's the case we should have matching fulfilled request
    if(!netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request")) {
        LogPrintf("CZnodeMan::ProcessVerifyReply -- ERROR: we didn't ask for verification of %s, peer=%d\n", pnode->addr.ToString(), pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nonce for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nonce != mnv.nonce) {
        LogPrintf("CZnodeMan::ProcessVerifyReply -- ERROR: wrong nounce: requested=%d, received=%d, peer=%d\n",
                    mWeAskedForVerification[pnode->addr].nonce, mnv.nonce, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nBlockHeight for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nBlockHeight != mnv.nBlockHeight) {
        LogPrintf("CZnodeMan::ProcessVerifyReply -- ERROR: wrong nBlockHeight: requested=%d, received=%d, peer=%d\n",
                    mWeAskedForVerification[pnode->addr].nBlockHeight, mnv.nBlockHeight, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("ZnodeMan::ProcessVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

//    // we already verified this address, why node is spamming?
    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-done")) {
        LogPrintf("CZnodeMan::ProcessVerifyReply -- ERROR: already verified %s recently\n", pnode->addr.ToString());
        Misbehaving(pnode->id, 20);
        return;
    }

    {
        LOCK(cs);

        CZnode* prealZnode = NULL;
        std::vector<CZnode*> vpZnodesToBan;
        std::vector<CZnode>::iterator it = vZnodes.begin();
        std::string strMessage1 = strprintf("%s%d%s", pnode->addr.ToString(), mnv.nonce, blockHash.ToString());
        while(it != vZnodes.end()) {
            if(CAddress(it->addr, NODE_NETWORK) == pnode->addr) {
                if(darkSendSigner.VerifyMessage(it->pubKeyZnode, mnv.vchSig1, strMessage1, strError)) {
                    // found it!
                    prealZnode = &(*it);
                    if(!it->IsPoSeVerified()) {
                        it->DecreasePoSeBanScore();
                    }
                    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-done");

                    // we can only broadcast it if we are an activated indexnode
                    if(activeZnode.vin == CTxIn()) continue;
                    // update ...
                    mnv.addr = it->addr;
                    mnv.vin1 = it->vin;
                    mnv.vin2 = activeZnode.vin;
                    std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(), mnv.nonce, blockHash.ToString(),
                                            mnv.vin1.prevout.ToStringShort(), mnv.vin2.prevout.ToStringShort());
                    // ... and sign it
                    if(!darkSendSigner.SignMessage(strMessage2, mnv.vchSig2, activeZnode.keyZnode)) {
                        LogPrintf("ZnodeMan::ProcessVerifyReply -- SignMessage() failed\n");
                        return;
                    }

                    std::string strError;

                    if(!darkSendSigner.VerifyMessage(activeZnode.pubKeyZnode, mnv.vchSig2, strMessage2, strError)) {
                        LogPrintf("ZnodeMan::ProcessVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
                        return;
                    }

                    mWeAskedForVerification[pnode->addr] = mnv;
                    mnv.Relay();

                } else {
                    vpZnodesToBan.push_back(&(*it));
                }
            }
            ++it;
        }
        // no real indexnode found?...
        if(!prealZnode) {
            // this should never be the case normally,
            // only if someone is trying to game the system in some way or smth like that
            LogPrintf("CZnodeMan::ProcessVerifyReply -- ERROR: no real indexnode found for addr %s\n", pnode->addr.ToString());
            Misbehaving(pnode->id, 20);
            return;
        }
        LogPrintf("CZnodeMan::ProcessVerifyReply -- verified real indexnode %s for addr %s\n",
                    prealZnode->vin.prevout.ToStringShort(), pnode->addr.ToString());
        // increase ban score for everyone else
        BOOST_FOREACH(CZnode* pmn, vpZnodesToBan) {
            pmn->IncreasePoSeBanScore();
            LogPrint("indexnode", "CZnodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                        prealZnode->vin.prevout.ToStringShort(), pnode->addr.ToString(), pmn->nPoSeBanScore);
        }
        LogPrintf("CZnodeMan::ProcessVerifyBroadcast -- PoSe score increased for %d fake indexnodes, addr %s\n",
                    (int)vpZnodesToBan.size(), pnode->addr.ToString());
    }
}

void CZnodeMan::ProcessVerifyBroadcast(CNode* pnode, const CZnodeVerification& mnv)
{
    std::string strError;

    if(mapSeenZnodeVerification.find(mnv.GetHash()) != mapSeenZnodeVerification.end()) {
        // we already have one
        return;
    }
    mapSeenZnodeVerification[mnv.GetHash()] = mnv;

    // we don't care about history
    if(mnv.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS) {
        LogPrint("indexnode", "ZnodeMan::ProcessVerifyBroadcast -- Outdated: current block %d, verification block %d, peer=%d\n",
                    pCurrentBlockIndex->nHeight, mnv.nBlockHeight, pnode->id);
        return;
    }

    if(mnv.vin1.prevout == mnv.vin2.prevout) {
        LogPrint("indexnode", "ZnodeMan::ProcessVerifyBroadcast -- ERROR: same vins %s, peer=%d\n",
                    mnv.vin1.prevout.ToStringShort(), pnode->id);
        // that was NOT a good idea to cheat and verify itself,
        // ban the node we received such message from
        Misbehaving(pnode->id, 100);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("ZnodeMan::ProcessVerifyBroadcast -- Can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

    int nRank = GetZnodeRank(mnv.vin2, mnv.nBlockHeight, MIN_POSE_PROTO_VERSION);

    if (nRank == -1) {
        LogPrint("indexnode", "CZnodeMan::ProcessVerifyBroadcast -- Can't calculate rank for indexnode %s\n",
                    mnv.vin2.prevout.ToStringShort());
        return;
    }

    if(nRank > MAX_POSE_RANK) {
        LogPrint("indexnode", "CZnodeMan::ProcessVerifyBroadcast -- Mastrernode %s is not in top %d, current rank %d, peer=%d\n",
                    mnv.vin2.prevout.ToStringShort(), (int)MAX_POSE_RANK, nRank, pnode->id);
        return;
    }

    {
        LOCK(cs);

        std::string strMessage1 = strprintf("%s%d%s", mnv.addr.ToString(), mnv.nonce, blockHash.ToString());
        std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(), mnv.nonce, blockHash.ToString(),
                                mnv.vin1.prevout.ToStringShort(), mnv.vin2.prevout.ToStringShort());

        CZnode* pmn1 = Find(mnv.vin1);
        if(!pmn1) {
            LogPrintf("CZnodeMan::ProcessVerifyBroadcast -- can't find indexnode1 %s\n", mnv.vin1.prevout.ToStringShort());
            return;
        }

        CZnode* pmn2 = Find(mnv.vin2);
        if(!pmn2) {
            LogPrintf("CZnodeMan::ProcessVerifyBroadcast -- can't find indexnode2 %s\n", mnv.vin2.prevout.ToStringShort());
            return;
        }

        if(pmn1->addr != mnv.addr) {
            LogPrintf("CZnodeMan::ProcessVerifyBroadcast -- addr %s do not match %s\n", mnv.addr.ToString(), pnode->addr.ToString());
            return;
        }

        if(darkSendSigner.VerifyMessage(pmn1->pubKeyZnode, mnv.vchSig1, strMessage1, strError)) {
            LogPrintf("ZnodeMan::ProcessVerifyBroadcast -- VerifyMessage() for indexnode1 failed, error: %s\n", strError);
            return;
        }

        if(darkSendSigner.VerifyMessage(pmn2->pubKeyZnode, mnv.vchSig2, strMessage2, strError)) {
            LogPrintf("ZnodeMan::ProcessVerifyBroadcast -- VerifyMessage() for indexnode2 failed, error: %s\n", strError);
            return;
        }

        if(!pmn1->IsPoSeVerified()) {
            pmn1->DecreasePoSeBanScore();
        }
        mnv.Relay();

        LogPrintf("CZnodeMan::ProcessVerifyBroadcast -- verified indexnode %s for addr %s\n",
                    pmn1->vin.prevout.ToStringShort(), pnode->addr.ToString());

        // increase ban score for everyone else with the same addr
        int nCount = 0;
        BOOST_FOREACH(CZnode& mn, vZnodes) {
            if(mn.addr != mnv.addr || mn.vin.prevout == mnv.vin1.prevout) continue;
            mn.IncreasePoSeBanScore();
            nCount++;
            LogPrint("indexnode", "CZnodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                        mn.vin.prevout.ToStringShort(), mn.addr.ToString(), mn.nPoSeBanScore);
        }
        LogPrintf("CZnodeMan::ProcessVerifyBroadcast -- PoSe score incresed for %d fake indexnodes, addr %s\n",
                    nCount, pnode->addr.ToString());
    }
}

std::string CZnodeMan::ToString() const
{
    std::ostringstream info;

    info << "Znodes: " << (int)vZnodes.size() <<
            ", peers who asked us for Znode list: " << (int)mAskedUsForZnodeList.size() <<
            ", peers we asked for Znode list: " << (int)mWeAskedForZnodeList.size() <<
            ", entries in Znode list we asked for: " << (int)mWeAskedForZnodeListEntry.size() <<
            ", indexnode index size: " << indexZnodes.GetSize() <<
            ", nDsqCount: " << (int)nDsqCount;

    return info.str();
}

void CZnodeMan::UpdateZnodeList(CZnodeBroadcast mnb)
{
    try {
        LogPrintf("CZnodeMan::UpdateZnodeList\n");
        LOCK2(cs_main, cs);
        mapSeenZnodePing.insert(std::make_pair(mnb.lastPing.GetHash(), mnb.lastPing));
        mapSeenZnodeBroadcast.insert(std::make_pair(mnb.GetHash(), std::make_pair(GetTime(), mnb)));

        LogPrintf("CZnodeMan::UpdateZnodeList -- indexnode=%s  addr=%s\n", mnb.vin.prevout.ToStringShort(), mnb.addr.ToString());

        CZnode *pmn = Find(mnb.vin);
        if (pmn == NULL) {
            CZnode mn(mnb);
            if (Add(mn)) {
                indexnodeSync.AddedZnodeList();
            }
        } else {
            CZnodeBroadcast mnbOld = mapSeenZnodeBroadcast[CZnodeBroadcast(*pmn).GetHash()].second;
            if (pmn->UpdateFromNewBroadcast(mnb)) {
                indexnodeSync.AddedZnodeList();
                mapSeenZnodeBroadcast.erase(mnbOld.GetHash());
            }
        }
    } catch (const std::exception &e) {
        PrintExceptionContinue(std::current_exception(), "UpdateZnodeList");
    }
}

bool CZnodeMan::CheckMnbAndUpdateZnodeList(CNode* pfrom, CZnodeBroadcast mnb, int& nDos)
{
    // Need LOCK2 here to ensure consistent locking order because the SimpleCheck call below locks cs_main
    LOCK(cs_main);

    {
        LOCK(cs);
        nDos = 0;
        LogPrint("indexnode", "CZnodeMan::CheckMnbAndUpdateZnodeList -- indexnode=%s\n", mnb.vin.prevout.ToStringShort());

        uint256 hash = mnb.GetHash();
        if (mapSeenZnodeBroadcast.count(hash) && !mnb.fRecovery) { //seen
            LogPrint("indexnode", "CZnodeMan::CheckMnbAndUpdateZnodeList -- indexnode=%s seen\n", mnb.vin.prevout.ToStringShort());
            // less then 2 pings left before this MN goes into non-recoverable state, bump sync timeout
            if (GetTime() - mapSeenZnodeBroadcast[hash].first > INDEXNODE_NEW_START_REQUIRED_SECONDS - INDEXNODE_MIN_MNP_SECONDS * 2) {
                LogPrint("indexnode", "CZnodeMan::CheckMnbAndUpdateZnodeList -- indexnode=%s seen update\n", mnb.vin.prevout.ToStringShort());
                mapSeenZnodeBroadcast[hash].first = GetTime();
                indexnodeSync.AddedZnodeList();
            }
            // did we ask this node for it?
            if (pfrom && IsMnbRecoveryRequested(hash) && GetTime() < mMnbRecoveryRequests[hash].first) {
                LogPrint("indexnode", "CZnodeMan::CheckMnbAndUpdateZnodeList -- mnb=%s seen request\n", hash.ToString());
                if (mMnbRecoveryRequests[hash].second.count(pfrom->addr)) {
                    LogPrint("indexnode", "CZnodeMan::CheckMnbAndUpdateZnodeList -- mnb=%s seen request, addr=%s\n", hash.ToString(), pfrom->addr.ToString());
                    // do not allow node to send same mnb multiple times in recovery mode
                    mMnbRecoveryRequests[hash].second.erase(pfrom->addr);
                    // does it have newer lastPing?
                    if (mnb.lastPing.sigTime > mapSeenZnodeBroadcast[hash].second.lastPing.sigTime) {
                        // simulate Check
                        CZnode mnTemp = CZnode(mnb);
                        mnTemp.Check();
                        LogPrint("indexnode", "CZnodeMan::CheckMnbAndUpdateZnodeList -- mnb=%s seen request, addr=%s, better lastPing: %d min ago, projected mn state: %s\n", hash.ToString(), pfrom->addr.ToString(), (GetTime() - mnb.lastPing.sigTime) / 60, mnTemp.GetStateString());
                        if (mnTemp.IsValidStateForAutoStart(mnTemp.nActiveState)) {
                            // this node thinks it's a good one
                            LogPrint("indexnode", "CZnodeMan::CheckMnbAndUpdateZnodeList -- indexnode=%s seen good\n", mnb.vin.prevout.ToStringShort());
                            mMnbRecoveryGoodReplies[hash].push_back(mnb);
                        }
                    }
                }
            }
            return true;
        }
        mapSeenZnodeBroadcast.insert(std::make_pair(hash, std::make_pair(GetTime(), mnb)));

        LogPrint("indexnode", "CZnodeMan::CheckMnbAndUpdateZnodeList -- indexnode=%s new\n", mnb.vin.prevout.ToStringShort());

        if (!mnb.SimpleCheck(nDos)) {
            LogPrint("indexnode", "CZnodeMan::CheckMnbAndUpdateZnodeList -- SimpleCheck() failed, indexnode=%s\n", mnb.vin.prevout.ToStringShort());
            return false;
        }

        // search Znode list
        CZnode *pmn = Find(mnb.vin);
        if (pmn) {
            CZnodeBroadcast mnbOld = mapSeenZnodeBroadcast[CZnodeBroadcast(*pmn).GetHash()].second;
            if (!mnb.Update(pmn, nDos)) {
                LogPrint("indexnode", "CZnodeMan::CheckMnbAndUpdateZnodeList -- Update() failed, indexnode=%s\n", mnb.vin.prevout.ToStringShort());
                return false;
            }
            if (hash != mnbOld.GetHash()) {
                mapSeenZnodeBroadcast.erase(mnbOld.GetHash());
            }
        }
    } // end of LOCK(cs);

    if(mnb.CheckOutpoint(nDos)) {
        Add(mnb);
        indexnodeSync.AddedZnodeList();
        // if it matches our Znode privkey...
        if(fMasternodeMode && mnb.pubKeyZnode == activeZnode.pubKeyZnode) {
            mnb.nPoSeBanScore = -INDEXNODE_POSE_BAN_MAX_SCORE;
            if(mnb.nProtocolVersion == LEGACY_INDEXNODES_PROTOCOL_VERSION) {
                // ... and PROTOCOL_VERSION, then we've been remotely activated ...
                LogPrintf("CZnodeMan::CheckMnbAndUpdateZnodeList -- Got NEW Znode entry: indexnode=%s  sigTime=%lld  addr=%s\n",
                            mnb.vin.prevout.ToStringShort(), mnb.sigTime, mnb.addr.ToString());
                activeZnode.ManageState();
            } else {
                // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
                // but also do not ban the node we get this message from
                LogPrintf("CZnodeMan::CheckMnbAndUpdateZnodeList -- wrong PROTOCOL_VERSION, re-activate your MN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", mnb.nProtocolVersion, LEGACY_INDEXNODES_PROTOCOL_VERSION);
                return false;
            }
        }
        mnb.RelayZNode();
    } else {
        LogPrintf("CZnodeMan::CheckMnbAndUpdateZnodeList -- Rejected Znode entry: %s  addr=%s\n", mnb.vin.prevout.ToStringShort(), mnb.addr.ToString());
        return false;
    }

    return true;
}

void CZnodeMan::UpdateLastPaid()
{
    LOCK(cs);
    if(fLiteMode) return;
    if(!pCurrentBlockIndex) {
        // LogPrintf("CZnodeMan::UpdateLastPaid, pCurrentBlockIndex=NULL\n");
        return;
    }

    static bool IsFirstRun = true;
    // Do full scan on first run or if we are not a indexnode
    // (MNs should update this info on every block, so limited scan should be enough for them)
    int nMaxBlocksToScanBack = (IsFirstRun || !fMasternodeMode) ? znpayments.GetStorageLimit() : LAST_PAID_SCAN_BLOCKS;

    LogPrint("znpayments", "CZnodeMan::UpdateLastPaid -- nHeight=%d, nMaxBlocksToScanBack=%d, IsFirstRun=%s\n",
                             pCurrentBlockIndex->nHeight, nMaxBlocksToScanBack, IsFirstRun ? "true" : "false");

    BOOST_FOREACH(CZnode& mn, vZnodes) {
        mn.UpdateLastPaid(pCurrentBlockIndex, nMaxBlocksToScanBack);
    }

    // every time is like the first time if winners list is not synced
    IsFirstRun = !indexnodeSync.IsWinnersListSynced();
}

void CZnodeMan::CheckAndRebuildZnodeIndex()
{
    LOCK(cs);

    if(GetTime() - nLastIndexRebuildTime < MIN_INDEX_REBUILD_TIME) {
        return;
    }

    if(indexZnodes.GetSize() <= MAX_EXPECTED_INDEX_SIZE) {
        return;
    }

    if(indexZnodes.GetSize() <= int(vZnodes.size())) {
        return;
    }

    indexZnodesOld = indexZnodes;
    indexZnodes.Clear();
    for(size_t i = 0; i < vZnodes.size(); ++i) {
        indexZnodes.AddZnodeVIN(vZnodes[i].vin);
    }

    fIndexRebuilt = true;
    nLastIndexRebuildTime = GetTime();
}

void CZnodeMan::UpdateWatchdogVoteTime(const CTxIn& vin)
{
    LOCK(cs);
    CZnode* pMN = Find(vin);
    if(!pMN)  {
        return;
    }
    pMN->UpdateWatchdogVoteTime();
    nLastWatchdogVoteTime = GetTime();
}

bool CZnodeMan::IsWatchdogActive()
{
    LOCK(cs);
    // Check if any indexnodes have voted recently, otherwise return false
    return (GetTime() - nLastWatchdogVoteTime) <= INDEXNODE_WATCHDOG_MAX_SECONDS;
}

void CZnodeMan::CheckZnode(const CTxIn& vin, bool fForce)
{
    LOCK(cs);
    CZnode* pMN = Find(vin);
    if(!pMN)  {
        return;
    }
    pMN->Check(fForce);
}

void CZnodeMan::CheckZnode(const CPubKey& pubKeyZnode, bool fForce)
{
    LOCK(cs);
    CZnode* pMN = Find(pubKeyZnode);
    if(!pMN)  {
        return;
    }
    pMN->Check(fForce);
}

int CZnodeMan::GetZnodeState(const CTxIn& vin)
{
    LOCK(cs);
    CZnode* pMN = Find(vin);
    if(!pMN)  {
        return CZnode::INDEXNODE_NEW_START_REQUIRED;
    }
    return pMN->nActiveState;
}

int CZnodeMan::GetZnodeState(const CPubKey& pubKeyZnode)
{
    LOCK(cs);
    CZnode* pMN = Find(pubKeyZnode);
    if(!pMN)  {
        return CZnode::INDEXNODE_NEW_START_REQUIRED;
    }
    return pMN->nActiveState;
}

bool CZnodeMan::IsZnodePingedWithin(const CTxIn& vin, int nSeconds, int64_t nTimeToCheckAt)
{
    LOCK(cs);
    CZnode* pMN = Find(vin);
    if(!pMN) {
        return false;
    }
    return pMN->IsPingedWithin(nSeconds, nTimeToCheckAt);
}

void CZnodeMan::SetZnodeLastPing(const CTxIn& vin, const CZnodePing& mnp)
{
    LOCK2(cs_main, cs);
    CZnode* pMN = Find(vin);
    if(!pMN)  {
        return;
    }
    pMN->lastPing = mnp;
    mapSeenZnodePing.insert(std::make_pair(mnp.GetHash(), mnp));

    CZnodeBroadcast mnb(*pMN);
    uint256 hash = mnb.GetHash();
    if(mapSeenZnodeBroadcast.count(hash)) {
        mapSeenZnodeBroadcast[hash].second.lastPing = mnp;
    }
}

void CZnodeMan::UpdatedBlockTip(const CBlockIndex *pindex)
{
    pCurrentBlockIndex = pindex;
    LogPrint("indexnode", "CZnodeMan::UpdatedBlockTip -- pCurrentBlockIndex->nHeight=%d\n", pCurrentBlockIndex->nHeight);

    CheckSameAddr();

    if(fMasternodeMode) {
        // normal wallet does not need to update this every block, doing update on rpc call should be enough
        UpdateLastPaid();
    }
}

void CZnodeMan::NotifyZnodeUpdates()
{
    // Avoid double locking
    bool fZnodesAddedLocal = false;
    bool fZnodesRemovedLocal = false;
    {
        LOCK(cs);
        fZnodesAddedLocal = fZnodesAdded;
        fZnodesRemovedLocal = fZnodesRemoved;
    }

    if(fZnodesAddedLocal) {
//        governance.CheckZnodeOrphanObjects();
//        governance.CheckZnodeOrphanVotes();
    }
    if(fZnodesRemovedLocal) {
//        governance.UpdateCachesAndClean();
    }

    LOCK(cs);
    fZnodesAdded = false;
    fZnodesRemoved = false;
}
