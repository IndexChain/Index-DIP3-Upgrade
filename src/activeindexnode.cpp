// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activeindexnode.h"
#include "consensus/consensus.h"
#include "indexnode.h"
#include "indexnode-sync.h"
#include "indexnode-payments.h"
#include "indexnodeman.h"
#include "protocol.h"
#include "netbase.h"

// TODO: upgrade to new dash, remove this hack
#define vNodes (g_connman->vNodes)
#define cs_vNodes (g_connman->cs_vNodes)

extern CWallet *pwalletMain;

// Keep track of the active Znode
CActiveZnode activeZnode;

void CActiveZnode::ManageState() {
    LogPrint("indexnode", "CActiveZnode::ManageState -- Start\n");
    if (!fMasternodeMode) {
        LogPrint("indexnode", "CActiveZnode::ManageState -- Not a indexnode, returning\n");
        return;
    }

    if (Params().NetworkIDString() != CBaseChainParams::REGTEST && !indexnodeSync.IsBlockchainSynced()) {
        nState = ACTIVE_INDEXNODE_SYNC_IN_PROCESS;
        LogPrintf("CActiveZnode::ManageState -- %s: %s\n", GetStateString(), GetStatus());
        return;
    }

    if (nState == ACTIVE_INDEXNODE_SYNC_IN_PROCESS) {
        nState = ACTIVE_INDEXNODE_INITIAL;
    }

    LogPrint("indexnode", "CActiveZnode::ManageState -- status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);

    if (eType == INDEXNODE_UNKNOWN) {
        ManageStateInitial();
    }

    if (eType == INDEXNODE_REMOTE) {
        ManageStateRemote();
    } else if (eType == INDEXNODE_LOCAL) {
        // Try Remote Start first so the started local indexnode can be restarted without recreate indexnode broadcast.
        ManageStateRemote();
        if (nState != ACTIVE_INDEXNODE_STARTED)
            ManageStateLocal();
    }

    SendZnodePing();
}

std::string CActiveZnode::GetStateString() const {
    switch (nState) {
        case ACTIVE_INDEXNODE_INITIAL:
            return "INITIAL";
        case ACTIVE_INDEXNODE_SYNC_IN_PROCESS:
            return "SYNC_IN_PROCESS";
        case ACTIVE_INDEXNODE_INPUT_TOO_NEW:
            return "INPUT_TOO_NEW";
        case ACTIVE_INDEXNODE_NOT_CAPABLE:
            return "NOT_CAPABLE";
        case ACTIVE_INDEXNODE_STARTED:
            return "STARTED";
        default:
            return "UNKNOWN";
    }
}

std::string CActiveZnode::GetStatus() const {
    switch (nState) {
        case ACTIVE_INDEXNODE_INITIAL:
            return "Node just started, not yet activated";
        case ACTIVE_INDEXNODE_SYNC_IN_PROCESS:
            return "Sync in progress. Must wait until sync is complete to start Znode";
        case ACTIVE_INDEXNODE_INPUT_TOO_NEW:
            return strprintf("Znode input must have at least %d confirmations",
                             Params().GetConsensus().nZnodeMinimumConfirmations);
        case ACTIVE_INDEXNODE_NOT_CAPABLE:
            return "Not capable indexnode: " + strNotCapableReason;
        case ACTIVE_INDEXNODE_STARTED:
            return "Znode successfully started";
        default:
            return "Unknown";
    }
}

std::string CActiveZnode::GetTypeString() const {
    std::string strType;
    switch (eType) {
        case INDEXNODE_UNKNOWN:
            strType = "UNKNOWN";
            break;
        case INDEXNODE_REMOTE:
            strType = "REMOTE";
            break;
        case INDEXNODE_LOCAL:
            strType = "LOCAL";
            break;
        default:
            strType = "UNKNOWN";
            break;
    }
    return strType;
}

bool CActiveZnode::SendZnodePing() {
    if (!fPingerEnabled) {
        LogPrint("indexnode",
                 "CActiveZnode::SendZnodePing -- %s: indexnode ping service is disabled, skipping...\n",
                 GetStateString());
        return false;
    }

    if (!mnodeman.Has(vin)) {
        strNotCapableReason = "Znode not in indexnode list";
        nState = ACTIVE_INDEXNODE_NOT_CAPABLE;
        LogPrintf("CActiveZnode::SendZnodePing -- %s: %s\n", GetStateString(), strNotCapableReason);
        return false;
    }

    CZnodePing mnp(vin);
    if (!mnp.Sign(keyZnode, pubKeyZnode)) {
        LogPrintf("CActiveZnode::SendZnodePing -- ERROR: Couldn't sign Znode Ping\n");
        return false;
    }

    // Update lastPing for our indexnode in Znode list
    if (mnodeman.IsZnodePingedWithin(vin, INDEXNODE_MIN_MNP_SECONDS, mnp.sigTime)) {
        LogPrintf("CActiveZnode::SendZnodePing -- Too early to send Znode Ping\n");
        return false;
    }

    mnodeman.SetZnodeLastPing(vin, mnp);

    LogPrintf("CActiveZnode::SendZnodePing -- Relaying ping, collateral=%s\n", vin.ToString());
    mnp.Relay();

    return true;
}

void CActiveZnode::ManageStateInitial() {
    LogPrint("indexnode", "CActiveZnode::ManageStateInitial -- status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);

    // Check that our local network configuration is correct
    if (!fListen) {
        // listen option is probably overwritten by smth else, no good
        nState = ACTIVE_INDEXNODE_NOT_CAPABLE;
        strNotCapableReason = "Znode must accept connections from outside. Make sure listen configuration option is not overwritten by some another parameter.";
        LogPrintf("CActiveZnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    bool fFoundLocal = false;
    {
        LOCK(cs_vNodes);

        // First try to find whatever local address is specified by externalip option
        fFoundLocal = GetLocal(service) && CZnode::IsValidNetAddr(service);
        if (!fFoundLocal) {
            // nothing and no live connections, can't do anything for now
            if (vNodes.empty()) {
                nState = ACTIVE_INDEXNODE_NOT_CAPABLE;
                strNotCapableReason = "Can't detect valid external address. Will retry when there are some connections available.";
                LogPrintf("CActiveZnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
                return;
            }
            // We have some peers, let's try to find our local address from one of them
            BOOST_FOREACH(CNode * pnode, vNodes)
            {
                if (pnode->fSuccessfullyConnected && pnode->addr.IsIPv4()) {
                    fFoundLocal = GetLocal(service, &pnode->addr) && CZnode::IsValidNetAddr(service);
                    if (fFoundLocal) break;
                }
            }
        }
    }
        
    if(Params().NetworkIDString() == CBaseChainParams::REGTEST) {
        std::string const & serv = GetArg("-externalip", "");
        if(!serv.empty()) {
            if (Lookup(serv.c_str(), service, 0, false))
                fFoundLocal = true;
        }

    }
    if(!fFoundLocal)
    {
        LOCK(cs_vNodes);

        // First try to find whatever local address is specified by externalip option
        fFoundLocal = GetLocal(service) && CZnode::IsValidNetAddr(service);
        if (!fFoundLocal) {
            // nothing and no live connections, can't do anything for now
            if (vNodes.empty()) {
                nState = ACTIVE_INDEXNODE_NOT_CAPABLE;
                strNotCapableReason = "Can't detect valid external address. Will retry when there are some connections available.";
                LogPrintf("CActiveZnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
                return;
            }
            // We have some peers, let's try to find our local address from one of them
            BOOST_FOREACH(CNode * pnode, vNodes)
            {
                if (pnode->fSuccessfullyConnected && pnode->addr.IsIPv4()) {
                    fFoundLocal = GetLocal(service, &pnode->addr) && CZnode::IsValidNetAddr(service);
                    if (fFoundLocal) break;
                }
            }
        }
    }

    if (!fFoundLocal) {
        nState = ACTIVE_INDEXNODE_NOT_CAPABLE;
        strNotCapableReason = "Can't detect valid external address. Please consider using the externalip configuration option if problem persists. Make sure to use IPv4 address only.";
        LogPrintf("CActiveZnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (service.GetPort() != mainnetDefaultPort) {
            nState = ACTIVE_INDEXNODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("Invalid port: %u - only %d is supported on mainnet.", service.GetPort(),
                                            mainnetDefaultPort);
            LogPrintf("CActiveZnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
    } else if (service.GetPort() == mainnetDefaultPort) {
        nState = ACTIVE_INDEXNODE_NOT_CAPABLE;
        strNotCapableReason = strprintf("Invalid port: %u - %d is only supported on mainnet.", service.GetPort(),
                                        mainnetDefaultPort);
        LogPrintf("CActiveZnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    LogPrintf("CActiveZnode::ManageStateInitial -- Checking inbound connection to '%s'\n", service.ToString());
    //TODO
    if (!g_connman->OpenMasternodeConnection(CAddress(service, NODE_NETWORK))) {
        nState = ACTIVE_INDEXNODE_NOT_CAPABLE;
        strNotCapableReason = "Could not connect to " + service.ToString();
        LogPrintf("CActiveZnode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    // Default to REMOTE
    eType = INDEXNODE_REMOTE;

    // Check if wallet funds are available
    if (!pwalletMain) {
        LogPrintf("CActiveZnode::ManageStateInitial -- %s: Wallet not available\n", GetStateString());
        return;
    }

    if (pwalletMain->IsLocked()) {
        LogPrintf("CActiveZnode::ManageStateInitial -- %s: Wallet is locked\n", GetStateString());
        return;
    }

    if (pwalletMain->GetBalance() < INDEXNODE_COIN_REQUIRED * COIN) {
        LogPrintf("CActiveZnode::ManageStateInitial -- %s: Wallet balance is < 1000 XZC\n", GetStateString());
        return;
    }

    // Choose coins to use
    CPubKey pubKeyCollateral;
    CKey keyCollateral;

    // If collateral is found switch to LOCAL mode
    if (pwalletMain->GetZnodeVinAndKeys(vin, pubKeyCollateral, keyCollateral)) {
        eType = INDEXNODE_LOCAL;
    }

    LogPrint("indexnode", "CActiveZnode::ManageStateInitial -- End status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);
}

void CActiveZnode::ManageStateRemote() {
    LogPrint("indexnode",
             "CActiveZnode::ManageStateRemote -- Start status = %s, type = %s, pinger enabled = %d, pubKeyZnode.GetID() = %s\n",
             GetStatus(), fPingerEnabled, GetTypeString(), pubKeyZnode.GetID().ToString());

    mnodeman.CheckZnode(pubKeyZnode);
    indexnode_info_t infoMn = mnodeman.GetZnodeInfo(pubKeyZnode);

    if (infoMn.fInfoValid) {
        if (infoMn.nProtocolVersion < MIN_INDEXNODE_PAYMENT_PROTO_VERSION_1
                || infoMn.nProtocolVersion > MIN_INDEXNODE_PAYMENT_PROTO_VERSION_2) {
            nState = ACTIVE_INDEXNODE_NOT_CAPABLE;
            strNotCapableReason = "Invalid protocol version";
            LogPrintf("CActiveZnode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if (service != infoMn.addr) {
            nState = ACTIVE_INDEXNODE_NOT_CAPABLE;
            // LogPrintf("service: %s\n", service.ToString());
            // LogPrintf("infoMn.addr: %s\n", infoMn.addr.ToString());
            strNotCapableReason = "Broadcasted IP doesn't match our external address. Make sure you issued a new broadcast if IP of this indexnode changed recently.";
            LogPrintf("CActiveZnode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if (!CZnode::IsValidStateForAutoStart(infoMn.nActiveState)) {
            nState = ACTIVE_INDEXNODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("Znode in %s state", CZnode::StateToString(infoMn.nActiveState));
            LogPrintf("CActiveZnode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if (nState != ACTIVE_INDEXNODE_STARTED) {
            LogPrintf("CActiveZnode::ManageStateRemote -- STARTED!\n");
            vin = infoMn.vin;
            service = infoMn.addr;
            fPingerEnabled = true;
            nState = ACTIVE_INDEXNODE_STARTED;
        }
    } else {
        nState = ACTIVE_INDEXNODE_NOT_CAPABLE;
        strNotCapableReason = "Znode not in indexnode list";
        LogPrintf("CActiveZnode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
    }
}

void CActiveZnode::ManageStateLocal() {
    LogPrint("indexnode", "CActiveZnode::ManageStateLocal -- status = %s, type = %s, pinger enabled = %d\n",
             GetStatus(), GetTypeString(), fPingerEnabled);
    if (nState == ACTIVE_INDEXNODE_STARTED) {
        return;
    }

    // Choose coins to use
    CPubKey pubKeyCollateral;
    CKey keyCollateral;

    if (pwalletMain->GetZnodeVinAndKeys(vin, pubKeyCollateral, keyCollateral)) {
        int nInputAge = GetInputAge(vin);
        if (nInputAge < Params().GetConsensus().nZnodeMinimumConfirmations) {
            nState = ACTIVE_INDEXNODE_INPUT_TOO_NEW;
            strNotCapableReason = strprintf(_("%s - %d confirmations"), GetStatus(), nInputAge);
            LogPrintf("CActiveZnode::ManageStateLocal -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }

        {
            LOCK(pwalletMain->cs_wallet);
            pwalletMain->LockCoin(vin.prevout);
        }

        CZnodeBroadcast mnb;
        std::string strError;
        if (!CZnodeBroadcast::Create(vin, service, keyCollateral, pubKeyCollateral, keyZnode,
                                     pubKeyZnode, strError, mnb)) {
            nState = ACTIVE_INDEXNODE_NOT_CAPABLE;
            strNotCapableReason = "Error creating mastenode broadcast: " + strError;
            LogPrintf("CActiveZnode::ManageStateLocal -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }

        fPingerEnabled = true;
        nState = ACTIVE_INDEXNODE_STARTED;

        //update to indexnode list
        LogPrintf("CActiveZnode::ManageStateLocal -- Update Znode List\n");
        mnodeman.UpdateZnodeList(mnb);
        mnodeman.NotifyZnodeUpdates();

        //send to all peers
        LogPrintf("CActiveZnode::ManageStateLocal -- Relay broadcast, vin=%s\n", vin.ToString());
        mnb.RelayZNode();
    }
}
