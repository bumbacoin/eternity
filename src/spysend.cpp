// Copyright (c) 2016 The Eternity developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "spysend.h"
#include "main.h"
#include "init.h"
#include "util.h"
#include "eternitynodeman.h"
#include "script/sign.h"
#include "instantx.h"
#include "ui_interface.h"
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/lexical_cast.hpp>

#include <algorithm>
#include <boost/assign/list_of.hpp>
#include <openssl/rand.h>

using namespace std;
using namespace boost;

// The main object for accessing Spysend
CSpysendPool spySendPool;
// A helper object for signing messages from Eternitynodes
CSpySendSigner spySendSigner;
// The current Spysends in progress on the network
std::vector<CSpysendQueue> vecSpysendQueue;
// Keep track of the used Eternitynodes
std::vector<CTxIn> vecEternitynodesUsed;
// Keep track of the scanning errors I've seen
map<uint256, CSpysendBroadcastTx> mapSpysendBroadcastTxes;
// Keep track of the active Eternitynode
CActiveEternitynode activeEternitynode;

void CSpysendPool::ProcessMessageSpysend(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if(fLiteMode) return; //disable all Spysend/Eternitynode related functionality
    if(!eternitynodeSync.IsBlockchainSynced()) return;

    if (strCommand == "dsa") { //Spysend Accept Into Pool

        int errorID;

        if (pfrom->nVersion < MIN_POOL_PEER_PROTO_VERSION) {
            errorID = ERR_VERSION;
            LogPrintf("dsa -- incompatible version! \n");
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_REJECTED, errorID);

            return;
        }

        if(!fEternityNode){
            errorID = ERR_NOT_A_MN;
            LogPrintf("dsa -- not a Eternitynode! \n");
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_REJECTED, errorID);

            return;
        }

        int nDenom;
        CTransaction txCollateral;
        vRecv >> nDenom >> txCollateral;

        CEternitynode* pen = mnodeman.Find(activeEternitynode.vin);
        if(pen == NULL)
        {
            errorID = ERR_MN_LIST;
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_REJECTED, errorID);
            return;
        }

        if(sessionUsers == 0) {
            if(pen->nLastDsq != 0 &&
                pen->nLastDsq + mnodeman.CountEnabled(MIN_POOL_PEER_PROTO_VERSION)/5 > mnodeman.nDsqCount){
                LogPrintf("dsa -- last dsq too recent, must wait. %s \n", pfrom->addr.ToString());
                errorID = ERR_RECENT;
                pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_REJECTED, errorID);
                return;
            }
        }

        if(!IsCompatibleWithSession(nDenom, txCollateral, errorID))
        {
            LogPrintf("dsa -- not compatible with existing transactions! \n");
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_REJECTED, errorID);
            return;
        } else {
            LogPrintf("dsa -- is compatible, please submit! \n");
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_ACCEPTED, errorID);
            return;
        }

    } else if (strCommand == "dsq") { //Spysend Queue
        TRY_LOCK(cs_spysend, lockRecv);
        if(!lockRecv) return;

        if (pfrom->nVersion < MIN_POOL_PEER_PROTO_VERSION) {
            return;
        }

        CSpysendQueue dsq;
        vRecv >> dsq;

        CService addr;
        if(!dsq.GetAddress(addr)) return;
        if(!dsq.CheckSignature()) return;

        if(dsq.IsExpired()) return;

        CEternitynode* pen = mnodeman.Find(dsq.vin);
        if(pen == NULL) return;

        // if the queue is ready, submit if we can
        if(dsq.ready) {
            if(!pSubmittedToEternitynode) return;
            if((CNetAddr)pSubmittedToEternitynode->addr != (CNetAddr)addr){
                LogPrintf("dsq - message doesn't match current Eternitynode - %s != %s\n", pSubmittedToEternitynode->addr.ToString(), addr.ToString());
                return;
            }

            if(state == POOL_STATUS_QUEUE){
                LogPrint("spysend", "Spysend queue is ready - %s\n", addr.ToString());
                PrepareSpysendDenominate();
            }
        } else {
            BOOST_FOREACH(CSpysendQueue q, vecSpysendQueue){
                if(q.vin == dsq.vin) return;
            }

            LogPrint("spysend", "dsq last %d last2 %d count %d\n", pen->nLastDsq, pen->nLastDsq + mnodeman.size()/5, mnodeman.nDsqCount);
            //don't allow a few nodes to dominate the queuing process
            if(pen->nLastDsq != 0 &&
                pen->nLastDsq + mnodeman.CountEnabled(MIN_POOL_PEER_PROTO_VERSION)/5 > mnodeman.nDsqCount){
                LogPrint("spysend", "dsq -- Eternitynode sending too many dsq messages. %s \n", pen->addr.ToString());
                return;
            }
            mnodeman.nDsqCount++;
            pen->nLastDsq = mnodeman.nDsqCount;
            pen->allowFreeTx = true;

            LogPrint("spysend", "dsq - new Spysend queue object - %s\n", addr.ToString());
            vecSpysendQueue.push_back(dsq);
            dsq.Relay();
            dsq.time = GetTime();
        }

    } else if (strCommand == "dsi") { //SpySend vIn
        int errorID;

        if (pfrom->nVersion < MIN_POOL_PEER_PROTO_VERSION) {
            LogPrintf("dsi -- incompatible version! \n");
            errorID = ERR_VERSION;
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_REJECTED, errorID);

            return;
        }

        if(!fEternityNode){
            LogPrintf("dsi -- not a Eternitynode! \n");
            errorID = ERR_NOT_A_MN;
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_REJECTED, errorID);

            return;
        }

        std::vector<CTxIn> in;
        int64_t nAmount;
        CTransaction txCollateral;
        std::vector<CTxOut> out;
        vRecv >> in >> nAmount >> txCollateral >> out;

        //do we have enough users in the current session?
        if(!IsSessionReady()){
            LogPrintf("dsi -- session not complete! \n");
            errorID = ERR_SESSION;
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_REJECTED, errorID);
            return;
        }

        //do we have the same denominations as the current session?
        if(!IsCompatibleWithEntries(out))
        {
            LogPrintf("dsi -- not compatible with existing transactions! \n");
            errorID = ERR_EXISTING_TX;
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_REJECTED, errorID);
            return;
        }

        //check it like a transaction
        {
            int64_t nValueIn = 0;
            int64_t nValueOut = 0;
            bool missingTx = false;

            CValidationState state;
            CMutableTransaction tx;

            BOOST_FOREACH(const CTxOut o, out){
                nValueOut += o.nValue;
                tx.vout.push_back(o);

                if(o.scriptPubKey.size() != 25){
                    LogPrintf("dsi - non-standard pubkey detected! %s\n", o.scriptPubKey.ToString());
                    errorID = ERR_NON_STANDARD_PUBKEY;
                    pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_REJECTED, errorID);
                    return;
                }
                if(!o.scriptPubKey.IsNormalPaymentScript()){
                    LogPrintf("dsi - invalid script! %s\n", o.scriptPubKey.ToString());
                    errorID = ERR_INVALID_SCRIPT;
                    pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_REJECTED, errorID);
                    return;
                }
            }

            BOOST_FOREACH(const CTxIn i, in){
                tx.vin.push_back(i);

                LogPrint("spysend", "dsi -- tx in %s\n", i.ToString());

                CTransaction tx2;
                uint256 hash;
                if(GetTransaction(i.prevout.hash, tx2, hash, true)){
                    if(tx2.vout.size() > i.prevout.n) {
                        nValueIn += tx2.vout[i.prevout.n].nValue;
                    }
                } else{
                    missingTx = true;
                }
            }

            if (nValueIn > SPYSEND_POOL_MAX) {
                LogPrintf("dsi -- more than Spysend pool max! %s\n", tx.ToString());
                errorID = ERR_MAXIMUM;
                pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_REJECTED, errorID);
                return;
            }

            if(!missingTx){
                if (nValueIn-nValueOut > nValueIn*.01) {
                    LogPrintf("dsi -- fees are too high! %s\n", tx.ToString());
                    errorID = ERR_FEES;
                    pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_REJECTED, errorID);
                    return;
                }
            } else {
                LogPrintf("dsi -- missing input tx! %s\n", tx.ToString());
                errorID = ERR_MISSING_TX;
                pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_REJECTED, errorID);
                return;
            }

            {
                LOCK(cs_main);
                if(!AcceptableInputs(mempool, state, CTransaction(tx), false, NULL, false, true)) {
                    LogPrintf("dsi -- transaction not valid! \n");
                    errorID = ERR_INVALID_TX;
                    pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_REJECTED, errorID);
                    return;
                }
            }
        }

        if(AddEntry(in, nAmount, txCollateral, out, errorID)){
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_ACCEPTED, errorID);
            Check();

            RelayStatus(sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_RESET);
        } else {
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_REJECTED, errorID);
        }

    } else if (strCommand == "dssu") { //Spysend status update
        if (pfrom->nVersion < MIN_POOL_PEER_PROTO_VERSION) {
            return;
        }

        if(!pSubmittedToEternitynode) return;
        if((CNetAddr)pSubmittedToEternitynode->addr != (CNetAddr)pfrom->addr){
            //LogPrintf("dssu - message doesn't match current Eternitynode - %s != %s\n", pSubmittedToEternitynode->addr.ToString(), pfrom->addr.ToString());
            return;
        }

        int sessionIDMessage;
        int state;
        int entriesCount;
        int accepted;
        int errorID;
        vRecv >> sessionIDMessage >> state >> entriesCount >> accepted >> errorID;

        LogPrint("spysend", "dssu - state: %i entriesCount: %i accepted: %i error: %s \n", state, entriesCount, accepted, GetMessageByID(errorID));

        if((accepted != 1 && accepted != 0) && sessionID != sessionIDMessage){
            LogPrintf("dssu - message doesn't match current Spysend session %d %d\n", sessionID, sessionIDMessage);
            return;
        }

        StatusUpdate(state, entriesCount, accepted, errorID, sessionIDMessage);

    } else if (strCommand == "dss") { //Spysend Sign Final Tx

        if (pfrom->nVersion < MIN_POOL_PEER_PROTO_VERSION) {
            return;
        }

        vector<CTxIn> sigs;
        vRecv >> sigs;

        bool success = false;
        int count = 0;

        BOOST_FOREACH(const CTxIn item, sigs)
        {
            if(AddScriptSig(item)) success = true;
            LogPrint("spysend", " -- sigs count %d %d\n", (int)sigs.size(), count);
            count++;
        }

        if(success){
            spySendPool.Check();
            RelayStatus(spySendPool.sessionID, spySendPool.GetState(), spySendPool.GetEntriesCount(), ETERNITYNODE_RESET);
        }
    } else if (strCommand == "dsf") { //Spysend Final tx
        if (pfrom->nVersion < MIN_POOL_PEER_PROTO_VERSION) {
            return;
        }

        if(!pSubmittedToEternitynode) return;
        if((CNetAddr)pSubmittedToEternitynode->addr != (CNetAddr)pfrom->addr){
            //LogPrintf("dsc - message doesn't match current Eternitynode - %s != %s\n", pSubmittedToEternitynode->addr.ToString(), pfrom->addr.ToString());
            return;
        }

        int sessionIDMessage;
        CTransaction txNew;
        vRecv >> sessionIDMessage >> txNew;

        if(sessionID != sessionIDMessage){
            LogPrint("spysend", "dsf - message doesn't match current Spysend session %d %d\n", sessionID, sessionIDMessage);
            return;
        }

        //check to see if input is spent already? (and probably not confirmed)
        SignFinalTransaction(txNew, pfrom);

    } else if (strCommand == "dsc") { //Spysend Complete

        if (pfrom->nVersion < MIN_POOL_PEER_PROTO_VERSION) {
            return;
        }

        if(!pSubmittedToEternitynode) return;
        if((CNetAddr)pSubmittedToEternitynode->addr != (CNetAddr)pfrom->addr){
            //LogPrintf("dsc - message doesn't match current Eternitynode - %s != %s\n", pSubmittedToEternitynode->addr.ToString(), pfrom->addr.ToString());
            return;
        }

        int sessionIDMessage;
        bool error;
        int errorID;
        vRecv >> sessionIDMessage >> error >> errorID;

        if(sessionID != sessionIDMessage){
            LogPrint("spysend", "dsc - message doesn't match current Spysend session %d %d\n", spySendPool.sessionID, sessionIDMessage);
            return;
        }

        spySendPool.CompletedTransaction(error, errorID);
    }

}

int randomizeList (int i) { return std::rand()%i;}

void CSpysendPool::Reset(){
    cachedLastSuccess = 0;
    lastNewBlock = 0;
    txCollateral = CMutableTransaction();
    vecEternitynodesUsed.clear();
    UnlockCoins();
    SetNull();
}

void CSpysendPool::SetNull(){

    // MN side
    sessionUsers = 0;
    vecSessionCollateral.clear();

    // Client side
    entriesCount = 0;
    lastEntryAccepted = 0;
    countEntriesAccepted = 0;
    sessionFoundEternitynode = false;

    // Both sides
    state = POOL_STATUS_IDLE;
    sessionID = 0;
    sessionDenom = 0;
    entries.clear();
    finalTransaction.vin.clear();
    finalTransaction.vout.clear();
    lastTimeChanged = GetTimeMillis();

    // -- seed random number generator (used for ordering output lists)
    unsigned int seed = 0;
    RAND_bytes((unsigned char*)&seed, sizeof(seed));
    std::srand(seed);
}

bool CSpysendPool::SetCollateralAddress(std::string strAddress){
    CBitcoinAddress address;
    if (!address.SetString(strAddress))
    {
        LogPrintf("CSpysendPool::SetCollateralAddress - Invalid Spysend collateral address\n");
        return false;
    }
    collateralPubKey = GetScriptForDestination(address.Get());
    return true;
}

//
// Unlock coins after Spysend fails or succeeds
//
void CSpysendPool::UnlockCoins(){
    while(true) {
        TRY_LOCK(pwalletMain->cs_wallet, lockWallet);
        if(!lockWallet) {MilliSleep(50); continue;}
        BOOST_FOREACH(CTxIn v, lockedCoins)
                pwalletMain->UnlockCoin(v.prevout);
        break;
    }

    lockedCoins.clear();
}

std::string CSpysendPool::GetStatus()
{
    static int showingSpySendMessage = 0;
    showingSpySendMessage += 10;
    std::string suffix = "";

    if(chainActive.Tip()->nHeight - cachedLastSuccess < minBlockSpacing || !eternitynodeSync.IsBlockchainSynced()) {
        return strAutoDenomResult;
    }
    switch(state) {
        case POOL_STATUS_IDLE:
            return _("Spysend is idle.");
        case POOL_STATUS_ACCEPTING_ENTRIES:
            if(entriesCount == 0) {
                showingSpySendMessage = 0;
                return strAutoDenomResult;
            } else if (lastEntryAccepted == 1) {
                if(showingSpySendMessage % 10 > 8) {
                    lastEntryAccepted = 0;
                    showingSpySendMessage = 0;
                }
                return _("Spysend request complete:") + " " + _("Your transaction was accepted into the pool!");
            } else {
                std::string suffix = "";
                if(     showingSpySendMessage % 70 <= 40) return strprintf(_("Submitted following entries to eternitynode: %u / %d"), entriesCount, GetMaxPoolTransactions());
                else if(showingSpySendMessage % 70 <= 50) suffix = ".";
                else if(showingSpySendMessage % 70 <= 60) suffix = "..";
                else if(showingSpySendMessage % 70 <= 70) suffix = "...";
                return strprintf(_("Submitted to eternitynode, waiting for more entries ( %u / %d ) %s"), entriesCount, GetMaxPoolTransactions(), suffix);
            }
        case POOL_STATUS_SIGNING:
            if(     showingSpySendMessage % 70 <= 40) return _("Found enough users, signing ...");
            else if(showingSpySendMessage % 70 <= 50) suffix = ".";
            else if(showingSpySendMessage % 70 <= 60) suffix = "..";
            else if(showingSpySendMessage % 70 <= 70) suffix = "...";
            return strprintf(_("Found enough users, signing ( waiting %s )"), suffix);
        case POOL_STATUS_TRANSMISSION:
            return _("Transmitting final transaction.");
        case POOL_STATUS_FINALIZE_TRANSACTION:
            return _("Finalizing transaction.");
        case POOL_STATUS_ERROR:
            return _("Spysend request incomplete:") + " " + lastMessage + " " + _("Will retry...");
        case POOL_STATUS_SUCCESS:
            return _("Spysend request complete:") + " " + lastMessage;
        case POOL_STATUS_QUEUE:
            if(     showingSpySendMessage % 70 <= 30) suffix = ".";
            else if(showingSpySendMessage % 70 <= 50) suffix = "..";
            else if(showingSpySendMessage % 70 <= 70) suffix = "...";
            return strprintf(_("Submitted to eternitynode, waiting in queue %s"), suffix);;
       default:
            return strprintf(_("Unknown state: id = %u"), state);
    }
}

//
// Check the Spysend progress and send client updates if a Eternitynode
//
void CSpysendPool::Check()
{
    if(fEternityNode) LogPrint("spysend", "CSpysendPool::Check() - entries count %lu\n", entries.size());
    //printf("CSpysendPool::Check() %d - %d - %d\n", state, anonTx.CountEntries(), GetTimeMillis()-lastTimeChanged);

    if(fEternityNode) {
        LogPrint("spysend", "CSpysendPool::Check() - entries count %lu\n", entries.size());

        // If entries is full, then move on to the next phase
        if(state == POOL_STATUS_ACCEPTING_ENTRIES && (int)entries.size() >= GetMaxPoolTransactions())
        {
            LogPrint("spysend", "CSpysendPool::Check() -- TRYING TRANSACTION \n");
            UpdateState(POOL_STATUS_FINALIZE_TRANSACTION);
        }
    }

    // create the finalized transaction for distribution to the clients
    if(state == POOL_STATUS_FINALIZE_TRANSACTION) {
        LogPrint("spysend", "CSpysendPool::Check() -- FINALIZE TRANSACTIONS\n");
        UpdateState(POOL_STATUS_SIGNING);

        if (fEternityNode) {
            CMutableTransaction txNew;

            // make our new transaction
            for(unsigned int i = 0; i < entries.size(); i++){
                BOOST_FOREACH(const CTxOut& v, entries[i].vout)
                    txNew.vout.push_back(v);

                BOOST_FOREACH(const CTxDSIn& s, entries[i].sev)
                    txNew.vin.push_back(s);
            }

            // shuffle the outputs for improved anonymity
            std::random_shuffle ( txNew.vin.begin(),  txNew.vin.end(),  randomizeList);
            std::random_shuffle ( txNew.vout.begin(), txNew.vout.end(), randomizeList);


            LogPrint("spysend", "Transaction 1: %s\n", txNew.ToString());
            finalTransaction = txNew;

            // request signatures from clients
            RelayFinalTransaction(sessionID, finalTransaction);
        }
    }

    // If we have all of the signatures, try to compile the transaction
    if(fEternityNode && state == POOL_STATUS_SIGNING && SignaturesComplete()) {
        LogPrint("spysend", "CSpysendPool::Check() -- SIGNING\n");
        UpdateState(POOL_STATUS_TRANSMISSION);

        CheckFinalTransaction();
    }

    // reset if we're here for 10 seconds
    if((state == POOL_STATUS_ERROR || state == POOL_STATUS_SUCCESS) && GetTimeMillis()-lastTimeChanged >= 10000) {
        LogPrint("spysend", "CSpysendPool::Check() -- timeout, RESETTING\n");
        UnlockCoins();
        SetNull();
        if(fEternityNode) RelayStatus(sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_RESET);
    }
}

void CSpysendPool::CheckFinalTransaction()
{
    if (!fEternityNode) return; // check and relay final tx only on eternitynode

    CWalletTx txNew = CWalletTx(pwalletMain, finalTransaction);

    LOCK2(cs_main, pwalletMain->cs_wallet);
    {
        LogPrint("spysend", "Transaction 2: %s\n", txNew.ToString());

        // See if the transaction is valid
        if (!txNew.AcceptToMemoryPool(false, true, true))
        {
            LogPrintf("CSpysendPool::Check() - CommitTransaction : Error: Transaction not valid\n");
            SetNull();

            // not much we can do in this case
            UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);
            RelayCompletedTransaction(sessionID, true, ERR_INVALID_TX);
            return;
        }

        LogPrintf("CSpysendPool::Check() -- IS ETERNITY -- TRANSMITTING SPYSEND\n");

        // sign a message

        int64_t sigTime = GetAdjustedTime();
        std::string strMessage = txNew.GetHash().ToString() + boost::lexical_cast<std::string>(sigTime);
        std::string strError = "";
        std::vector<unsigned char> vchSig;
        CKey key2;
        CPubKey pubkey2;

        if(!spySendSigner.SetKey(strEternityNodePrivKey, strError, key2, pubkey2))
        {
            LogPrintf("CSpysendPool::Check() - ERROR: Invalid Eternitynodeprivkey: '%s'\n", strError);
            return;
        }

        if(!spySendSigner.SignMessage(strMessage, strError, vchSig, key2)) {
            LogPrintf("CSpysendPool::Check() - Sign message failed\n");
            return;
        }

        if(!spySendSigner.VerifyMessage(pubkey2, vchSig, strMessage, strError)) {
            LogPrintf("CSpysendPool::Check() - Verify message failed\n");
            return;
        }

        if(!mapSpysendBroadcastTxes.count(txNew.GetHash())){
            CSpysendBroadcastTx dstx;
            dstx.tx = txNew;
            dstx.vin = activeEternitynode.vin;
            dstx.vchSig = vchSig;
            dstx.sigTime = sigTime;

            mapSpysendBroadcastTxes.insert(make_pair(txNew.GetHash(), dstx));
        }

        CInv inv(MSG_DSTX, txNew.GetHash());
        RelayInv(inv);

        // Tell the clients it was successful
        RelayCompletedTransaction(sessionID, false, MSG_SUCCESS);

        // Randomly charge clients
        ChargeRandomFees();

        // Reset
        LogPrint("spysend", "CSpysendPool::Check() -- COMPLETED -- RESETTING\n");
        SetNull();
        RelayStatus(sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_RESET);
    }
}

//
// Charge clients a fee if they're abusive
//
// Why bother? Spysend uses collateral to ensure abuse to the process is kept to a minimum.
// The submission and signing stages in Spysend are completely separate. In the cases where
// a client submits a transaction then refused to sign, there must be a cost. Otherwise they
// would be able to do this over and over again and bring the mixing to a hault.
//
// How does this work? Messages to Eternitynodes come in via "dsi", these require a valid collateral
// transaction for the client to be able to enter the pool. This transaction is kept by the Eternitynode
// until the transaction is either complete or fails.
//
void CSpysendPool::ChargeFees(){
    if(!fEternityNode) return;

    //we don't need to charge collateral for every offence.
    int offences = 0;
    int r = rand()%100;
    if(r > 33) return;

    if(state == POOL_STATUS_ACCEPTING_ENTRIES){
        BOOST_FOREACH(const CTransaction& txCollateral, vecSessionCollateral) {
            bool found = false;
            BOOST_FOREACH(const CSpySendEntry& v, entries) {
                if(v.collateral == txCollateral) {
                    found = true;
                }
            }

            // This queue entry didn't send us the promised transaction
            if(!found){
                LogPrintf("CSpysendPool::ChargeFees -- found uncooperative node (didn't send transaction). Found offence.\n");
                offences++;
            }
        }
    }

    if(state == POOL_STATUS_SIGNING) {
        // who didn't sign?
        BOOST_FOREACH(const CSpySendEntry v, entries) {
            BOOST_FOREACH(const CTxDSIn s, v.sev) {
                if(!s.fHasSig){
                    LogPrintf("CSpysendPool::ChargeFees -- found uncooperative node (didn't sign). Found offence\n");
                    offences++;
                }
            }
        }
    }

    r = rand()%100;
    int target = 0;

    //mostly offending?
    if(offences >= Params().PoolMaxTransactions()-1 && r > 33) return;

    //everyone is an offender? That's not right
    if(offences >= Params().PoolMaxTransactions()) return;

    //charge one of the offenders randomly
    if(offences > 1) target = 50;

    //pick random client to charge
    r = rand()%100;

    if(state == POOL_STATUS_ACCEPTING_ENTRIES){
        BOOST_FOREACH(const CTransaction& txCollateral, vecSessionCollateral) {
            bool found = false;
            BOOST_FOREACH(const CSpySendEntry& v, entries) {
                if(v.collateral == txCollateral) {
                    found = true;
                }
            }

            // This queue entry didn't send us the promised transaction
            if(!found && r > target){
                LogPrintf("CSpysendPool::ChargeFees -- found uncooperative node (didn't send transaction). charging fees.\n");

                CWalletTx wtxCollateral = CWalletTx(pwalletMain, txCollateral);

                // Broadcast
                if (!wtxCollateral.AcceptToMemoryPool(true))
                {
                    // This must not fail. The transaction has already been signed and recorded.
                    LogPrintf("CSpysendPool::ChargeFees() : Error: Transaction not valid");
                }
                wtxCollateral.RelayWalletTransaction();
                return;
            }
        }
    }

    if(state == POOL_STATUS_SIGNING) {
        // who didn't sign?
        BOOST_FOREACH(const CSpySendEntry v, entries) {
            BOOST_FOREACH(const CTxDSIn s, v.sev) {
                if(!s.fHasSig && r > target){
                    LogPrintf("CSpysendPool::ChargeFees -- found uncooperative node (didn't sign). charging fees.\n");

                    CWalletTx wtxCollateral = CWalletTx(pwalletMain, v.collateral);

                    // Broadcast
                    if (!wtxCollateral.AcceptToMemoryPool(false))
                    {
                        // This must not fail. The transaction has already been signed and recorded.
                        LogPrintf("CSpysendPool::ChargeFees() : Error: Transaction not valid");
                    }
                    wtxCollateral.RelayWalletTransaction();
                    return;
                }
            }
        }
    }
}

// charge the collateral randomly
//  - Spysend is completely free, to pay miners we randomly pay the collateral of users.
void CSpysendPool::ChargeRandomFees(){
    if(fEternityNode) {
        int i = 0;

        BOOST_FOREACH(const CTransaction& txCollateral, vecSessionCollateral) {
            int r = rand()%100;

            /*
                Collateral Fee Charges:

                Being that Spysend has "no fees" we need to have some kind of cost associated
                with using it to stop abuse. Otherwise it could serve as an attack vector and
                allow endless transaction that would bloat Eternity and make it unusable. To
                stop these kinds of attacks 1 in 10 successful transactions are charged. This
                adds up to a cost of 0.001DRK per transaction on average.
            */
            if(r <= 10)
            {
                LogPrintf("CSpysendPool::ChargeRandomFees -- charging random fees. %u\n", i);

                CWalletTx wtxCollateral = CWalletTx(pwalletMain, txCollateral);

                // Broadcast
                if (!wtxCollateral.AcceptToMemoryPool(true))
                {
                    // This must not fail. The transaction has already been signed and recorded.
                    LogPrintf("CSpysendPool::ChargeRandomFees() : Error: Transaction not valid");
                }
                wtxCollateral.RelayWalletTransaction();
            }
        }
    }
}

//
// Check for various timeouts (queue objects, Spysend, etc)
//
void CSpysendPool::CheckTimeout(){
    if(!fEnableSpysend && !fEternityNode) return;

    // catching hanging sessions
    if(!fEternityNode) {
        switch(state) {
            case POOL_STATUS_TRANSMISSION:
                LogPrint("spysend", "CSpysendPool::CheckTimeout() -- Session complete -- Running Check()\n");
                Check();
                break;
            case POOL_STATUS_ERROR:
                LogPrint("spysend", "CSpysendPool::CheckTimeout() -- Pool error -- Running Check()\n");
                Check();
                break;
            case POOL_STATUS_SUCCESS:
                LogPrint("spysend", "CSpysendPool::CheckTimeout() -- Pool success -- Running Check()\n");
                Check();
                break;
        }
    }

    // check Spysend queue objects for timeouts
    int c = 0;
    vector<CSpysendQueue>::iterator it = vecSpysendQueue.begin();
    while(it != vecSpysendQueue.end()){
        if((*it).IsExpired()){
            LogPrint("spysend", "CSpysendPool::CheckTimeout() : Removing expired queue entry - %d\n", c);
            it = vecSpysendQueue.erase(it);
        } else ++it;
        c++;
    }

    int addLagTime = 0;
    if(!fEternityNode) addLagTime = 10000; //if we're the client, give the server a few extra seconds before resetting.

    if(state == POOL_STATUS_ACCEPTING_ENTRIES || state == POOL_STATUS_QUEUE){
        c = 0;

        // check for a timeout and reset if needed
        vector<CSpySendEntry>::iterator it2 = entries.begin();
        while(it2 != entries.end()){
            if((*it2).IsExpired()){
                LogPrint("spysend", "CSpysendPool::CheckTimeout() : Removing expired entry - %d\n", c);
                it2 = entries.erase(it2);
                if(entries.size() == 0){
                    UnlockCoins();
                    SetNull();
                }
                if(fEternityNode){
                    RelayStatus(sessionID, GetState(), GetEntriesCount(), ETERNITYNODE_RESET);
                }
            } else ++it2;
            c++;
        }

        if(GetTimeMillis()-lastTimeChanged >= (SPYSEND_QUEUE_TIMEOUT*1000)+addLagTime){
            UnlockCoins();
            SetNull();
        }
    } else if(GetTimeMillis()-lastTimeChanged >= (SPYSEND_QUEUE_TIMEOUT*1000)+addLagTime){
        LogPrint("spysend", "CSpysendPool::CheckTimeout() -- Session timed out (%ds) -- resetting\n", SPYSEND_QUEUE_TIMEOUT);
        UnlockCoins();
        SetNull();

        UpdateState(POOL_STATUS_ERROR);
        lastMessage = _("Session timed out.");
    }

    if(state == POOL_STATUS_SIGNING && GetTimeMillis()-lastTimeChanged >= (SPYSEND_SIGNING_TIMEOUT*1000)+addLagTime ) {
            LogPrint("spysend", "CSpysendPool::CheckTimeout() -- Session timed out (%ds) -- restting\n", SPYSEND_SIGNING_TIMEOUT);
            ChargeFees();
            UnlockCoins();
            SetNull();

            UpdateState(POOL_STATUS_ERROR);
            lastMessage = _("Signing timed out.");
    }
}

//
// Check for complete queue
//
void CSpysendPool::CheckForCompleteQueue(){
    if(!fEnableSpysend && !fEternityNode) return;

    /* Check to see if we're ready for submissions from clients */
    //
    // After receiving multiple dsa messages, the queue will switch to "accepting entries"
    // which is the active state right before merging the transaction
    //
    if(state == POOL_STATUS_QUEUE && sessionUsers == GetMaxPoolTransactions()) {
        UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);

        CSpysendQueue dsq;
        dsq.nDenom = sessionDenom;
        dsq.vin = activeEternitynode.vin;
        dsq.time = GetTime();
        dsq.ready = true;
        dsq.Sign();
        dsq.Relay();
    }
}

// check to see if the signature is valid
bool CSpysendPool::SignatureValid(const CScript& newSig, const CTxIn& newVin){
    CMutableTransaction txNew;
    txNew.vin.clear();
    txNew.vout.clear();

    int found = -1;
    CScript sigPubKey = CScript();
    unsigned int i = 0;

    BOOST_FOREACH(CSpySendEntry& e, entries) {
        BOOST_FOREACH(const CTxOut& out, e.vout)
            txNew.vout.push_back(out);

        BOOST_FOREACH(const CTxDSIn& s, e.sev){
            txNew.vin.push_back(s);

            if(s == newVin){
                found = i;
                sigPubKey = s.prevPubKey;
            }
            i++;
        }
    }

    if(found >= 0){ //might have to do this one input at a time?
        int n = found;
        txNew.vin[n].scriptSig = newSig;
        LogPrint("spysend", "CSpysendPool::SignatureValid() - Sign with sig %s\n", newSig.ToString().substr(0,24));
        if (!VerifyScript(txNew.vin[n].scriptSig, sigPubKey, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC, MutableTransactionSignatureChecker(&txNew, n))){
            LogPrint("spysend", "CSpysendPool::SignatureValid() - Signing - Error signing input %u\n", n);
            return false;
        }
    }

    LogPrint("spysend", "CSpysendPool::SignatureValid() - Signing - Successfully validated input\n");
    return true;
}

// check to make sure the collateral provided by the client is valid
bool CSpysendPool::IsCollateralValid(const CTransaction& txCollateral){
    if(txCollateral.vout.size() < 1) return false;
    if(txCollateral.nLockTime != 0) return false;

    int64_t nValueIn = 0;
    int64_t nValueOut = 0;
    bool missingTx = false;

    BOOST_FOREACH(const CTxOut o, txCollateral.vout){
        nValueOut += o.nValue;

        if(!o.scriptPubKey.IsNormalPaymentScript()){
            LogPrintf ("CSpysendPool::IsCollateralValid - Invalid Script %s\n", txCollateral.ToString());
            return false;
        }
    }

    BOOST_FOREACH(const CTxIn i, txCollateral.vin){
        CTransaction tx2;
        uint256 hash;
        if(GetTransaction(i.prevout.hash, tx2, hash, true)){
            if(tx2.vout.size() > i.prevout.n) {
                nValueIn += tx2.vout[i.prevout.n].nValue;
            }
        } else{
            missingTx = true;
        }
    }

    if(missingTx){
        LogPrint("spysend", "CSpysendPool::IsCollateralValid - Unknown inputs in collateral transaction - %s\n", txCollateral.ToString());
        return false;
    }

    //collateral transactions are required to pay out SPYSEND_COLLATERAL as a fee to the miners
    if(nValueIn - nValueOut < SPYSEND_COLLATERAL) {
        LogPrint("spysend", "CSpysendPool::IsCollateralValid - did not include enough fees in transaction %d\n%s\n", nValueOut-nValueIn, txCollateral.ToString());
        return false;
    }

    LogPrint("spysend", "CSpysendPool::IsCollateralValid %s\n", txCollateral.ToString());

    {
        LOCK(cs_main);
        CValidationState state;
        if(!AcceptableInputs(mempool, state, txCollateral, true, NULL)){
            if(fDebug) LogPrintf ("CSpysendPool::IsCollateralValid - didn't pass IsAcceptable\n");
            return false;
        }
    }

    return true;
}


//
// Add a clients transaction to the pool
//
bool CSpysendPool::AddEntry(const std::vector<CTxIn>& newInput, const int64_t& nAmount, const CTransaction& txCollateral, const std::vector<CTxOut>& newOutput, int& errorID){
    if (!fEternityNode) return false;

    BOOST_FOREACH(CTxIn in, newInput) {
        if (in.prevout.IsNull() || nAmount < 0) {
            LogPrint("spysend", "CSpysendPool::AddEntry - input not valid!\n");
            errorID = ERR_INVALID_INPUT;
            sessionUsers--;
            return false;
        }
    }

    if (!IsCollateralValid(txCollateral)){
        LogPrint("spysend", "CSpysendPool::AddEntry - collateral not valid!\n");
        errorID = ERR_INVALID_COLLATERAL;
        sessionUsers--;
        return false;
    }

    if((int)entries.size() >= GetMaxPoolTransactions()){
        LogPrint("spysend", "CSpysendPool::AddEntry - entries is full!\n");
        errorID = ERR_ENTRIES_FULL;
        sessionUsers--;
        return false;
    }

    BOOST_FOREACH(CTxIn in, newInput) {
        LogPrint("spysend", "looking for vin -- %s\n", in.ToString());
        BOOST_FOREACH(const CSpySendEntry& v, entries) {
            BOOST_FOREACH(const CTxDSIn& s, v.sev){
                if((CTxIn)s == in) {
                    LogPrint("spysend", "CSpysendPool::AddEntry - found in vin\n");
                    errorID = ERR_ALREADY_HAVE;
                    sessionUsers--;
                    return false;
                }
            }
        }
    }

    CSpySendEntry v;
    v.Add(newInput, nAmount, txCollateral, newOutput);
    entries.push_back(v);

    LogPrint("spysend", "CSpysendPool::AddEntry -- adding %s\n", newInput[0].ToString());
    errorID = MSG_ENTRIES_ADDED;

    return true;
}

bool CSpysendPool::AddScriptSig(const CTxIn& newVin){
    LogPrint("spysend", "CSpysendPool::AddScriptSig -- new sig  %s\n", newVin.scriptSig.ToString().substr(0,24));


    BOOST_FOREACH(const CSpySendEntry& v, entries) {
        BOOST_FOREACH(const CTxDSIn& s, v.sev){
            if(s.scriptSig == newVin.scriptSig) {
                LogPrint("spysend", "CSpysendPool::AddScriptSig - already exists\n");
                return false;
            }
        }
    }

    if(!SignatureValid(newVin.scriptSig, newVin)){
        LogPrint("spysend", "CSpysendPool::AddScriptSig - Invalid Sig\n");
        return false;
    }

    LogPrint("spysend", "CSpysendPool::AddScriptSig -- sig %s\n", newVin.ToString());

    BOOST_FOREACH(CTxIn& vin, finalTransaction.vin){
        if(newVin.prevout == vin.prevout && vin.nSequence == newVin.nSequence){
            vin.scriptSig = newVin.scriptSig;
            vin.prevPubKey = newVin.prevPubKey;
            LogPrint("spysend", "CSpySendPool::AddScriptSig -- adding to finalTransaction  %s\n", newVin.scriptSig.ToString().substr(0,24));
        }
    }
    for(unsigned int i = 0; i < entries.size(); i++){
        if(entries[i].AddSig(newVin)){
            LogPrint("spysend", "CSpySendPool::AddScriptSig -- adding  %s\n", newVin.scriptSig.ToString().substr(0,24));
            return true;
        }
    }

    LogPrintf("CSpysendPool::AddScriptSig -- Couldn't set sig!\n" );
    return false;
}

// Check to make sure everything is signed
bool CSpysendPool::SignaturesComplete(){
    BOOST_FOREACH(const CSpySendEntry& v, entries) {
        BOOST_FOREACH(const CTxDSIn& s, v.sev){
            if(!s.fHasSig) return false;
        }
    }
    return true;
}

//
// Execute a Spysend denomination via a Eternitynode.
// This is only ran from clients
//
void CSpysendPool::SendSpysendDenominate(std::vector<CTxIn>& vin, std::vector<CTxOut>& vout, int64_t amount){

    if(fEternityNode) {
        LogPrintf("CSpysendPool::SendSpysendDenominate() - Spysend from a Eternitynode is not supported currently.\n");
        return;
    }

    if(txCollateral == CMutableTransaction()){
        LogPrintf ("CSpysendPool:SendSpysendDenominate() - Spysend collateral not set");
        return;
    }

    // lock the funds we're going to use
    BOOST_FOREACH(CTxIn in, txCollateral.vin)
        lockedCoins.push_back(in);

    BOOST_FOREACH(CTxIn in, vin)
        lockedCoins.push_back(in);

    //BOOST_FOREACH(CTxOut o, vout)
    //    LogPrintf(" vout - %s\n", o.ToString());


    // we should already be connected to a Eternitynode
    if(!sessionFoundEternitynode){
        LogPrintf("CSpysendPool::SendSpysendDenominate() - No Eternitynode has been selected yet.\n");
        UnlockCoins();
        SetNull();
        return;
    }

    if (!CheckDiskSpace()) {
        UnlockCoins();
        SetNull();
        fEnableSpysend = false;
        LogPrintf("CSpysendPool::SendSpysendDenominate() - Not enough disk space, disabling Spysend.\n");
        return;
    }

    UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);

    LogPrintf("CSpysendPool::SendSpysendDenominate() - Added transaction to pool.\n");

    ClearLastMessage();

    //check it against the memory pool to make sure it's valid
    {
        int64_t nValueOut = 0;

        CValidationState state;
        CMutableTransaction tx;

        BOOST_FOREACH(const CTxOut& o, vout){
            nValueOut += o.nValue;
            tx.vout.push_back(o);
        }

        BOOST_FOREACH(const CTxIn& i, vin){
            tx.vin.push_back(i);

            LogPrint("spysend", "dsi -- tx in %s\n", i.ToString());
        }

        LogPrintf("Submitting tx %s\n", tx.ToString());

        while(true){
            TRY_LOCK(cs_main, lockMain);
            if(!lockMain) { MilliSleep(50); continue;}
            if(!AcceptableInputs(mempool, state, CTransaction(tx), false, NULL, false, true)){
                LogPrintf("dsi -- transaction not valid! %s \n", tx.ToString());
                UnlockCoins();
                SetNull();
                return;
            }
            break;
        }
    }

    // store our entry for later use
    CSpySendEntry e;
    e.Add(vin, amount, txCollateral, vout);
    entries.push_back(e);

    RelayIn(entries[0].sev, entries[0].amount, txCollateral, entries[0].vout);
    Check();
}

// Incoming message from Eternitynode updating the progress of Spysend
//    newAccepted:  -1 mean's it'n not a "transaction accepted/not accepted" message, just a standard update
//                  0 means transaction was not accepted
//                  1 means transaction was accepted

bool CSpysendPool::StatusUpdate(int newState, int newEntriesCount, int newAccepted, int& errorID, int newSessionID){
    if(fEternityNode) return false;
    if(state == POOL_STATUS_ERROR || state == POOL_STATUS_SUCCESS) return false;

    UpdateState(newState);
    entriesCount = newEntriesCount;

    if(errorID != MSG_NOERR) strAutoDenomResult = _("Eternitynode:") + " " + GetMessageByID(errorID);

    if(newAccepted != -1) {
        lastEntryAccepted = newAccepted;
        countEntriesAccepted += newAccepted;
        if(newAccepted == 0){
            UpdateState(POOL_STATUS_ERROR);
            lastMessage = GetMessageByID(errorID);
        }

        if(newAccepted == 1 && newSessionID != 0) {
            sessionID = newSessionID;
            LogPrintf("CSpysendPool::StatusUpdate - set sessionID to %d\n", sessionID);
            sessionFoundEternitynode = true;
        }
    }

    if(newState == POOL_STATUS_ACCEPTING_ENTRIES){
        if(newAccepted == 1){
            LogPrintf("CSpysendPool::StatusUpdate - entry accepted! \n");
            sessionFoundEternitynode = true;
            //wait for other users. Eternitynode will report when ready
            UpdateState(POOL_STATUS_QUEUE);
        } else if (newAccepted == 0 && sessionID == 0 && !sessionFoundEternitynode) {
            LogPrintf("CSpysendPool::StatusUpdate - entry not accepted by Eternitynode \n");
            UnlockCoins();
            UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);
            DoAutomaticDenominating(); //try another Eternitynode
        }
        if(sessionFoundEternitynode) return true;
    }

    return true;
}

//
// After we receive the finalized transaction from the Eternitynode, we must
// check it to make sure it's what we want, then sign it if we agree.
// If we refuse to sign, it's possible we'll be charged collateral
//
bool CSpysendPool::SignFinalTransaction(CTransaction& finalTransactionNew, CNode* node){
    if(fEternityNode) return false;

    finalTransaction = finalTransactionNew;
    LogPrintf("CSpysendPool::SignFinalTransaction %s", finalTransaction.ToString());

    vector<CTxIn> sigs;

    //make sure my inputs/outputs are present, otherwise refuse to sign
    BOOST_FOREACH(const CSpySendEntry e, entries) {
        BOOST_FOREACH(const CTxDSIn s, e.sev) {
            /* Sign my transaction and all outputs */
            int mine = -1;
            CScript prevPubKey = CScript();
            CTxIn vin = CTxIn();

            for(unsigned int i = 0; i < finalTransaction.vin.size(); i++){
                if(finalTransaction.vin[i] == s){
                    mine = i;
                    prevPubKey = s.prevPubKey;
                    vin = s;
                }
            }

            if(mine >= 0){ //might have to do this one input at a time?
                int foundOutputs = 0;
                CAmount nValue1 = 0;
                CAmount nValue2 = 0;

                for(unsigned int i = 0; i < finalTransaction.vout.size(); i++){
                    BOOST_FOREACH(const CTxOut& o, e.vout) {
                        if(finalTransaction.vout[i] == o){
                            foundOutputs++;
                            nValue1 += finalTransaction.vout[i].nValue;
                        }
                    }
                }

                BOOST_FOREACH(const CTxOut o, e.vout)
                    nValue2 += o.nValue;

                int targetOuputs = e.vout.size();
                if(foundOutputs < targetOuputs || nValue1 != nValue2) {
                    // in this case, something went wrong and we'll refuse to sign. It's possible we'll be charged collateral. But that's
                    // better then signing if the transaction doesn't look like what we wanted.
                    LogPrintf("CSpysendPool::Sign - My entries are not correct! Refusing to sign. %d entries %d target. \n", foundOutputs, targetOuputs);
                    UnlockCoins();
                    SetNull();

                    return false;
                }

                const CKeyStore& keystore = *pwalletMain;

                LogPrint("spysend", "CSpysendPool::Sign - Signing my input %i\n", mine);
                if(!SignSignature(keystore, prevPubKey, finalTransaction, mine, int(SIGHASH_ALL|SIGHASH_ANYONECANPAY))) { // changes scriptSig
                    LogPrint("spysend", "CSpysendPool::Sign - Unable to sign my own transaction! \n");
                    // not sure what to do here, it will timeout...?
                }

                sigs.push_back(finalTransaction.vin[mine]);
                LogPrint("spysend", " -- dss %d %d %s\n", mine, (int)sigs.size(), finalTransaction.vin[mine].scriptSig.ToString());
            }

        }

        LogPrint("spysend", "CSpysendPool::Sign - txNew:\n%s", finalTransaction.ToString());
    }

	// push all of our signatures to the Eternitynode
	if(sigs.size() > 0 && node != NULL)
	    node->PushMessage("dss", sigs);


    return true;
}

void CSpysendPool::NewBlock()
{
    LogPrint("spysend", "CSpysendPool::NewBlock \n");

    //we we're processing lots of blocks, we'll just leave
    if(GetTime() - lastNewBlock < 10) return;
    lastNewBlock = GetTime();

    spySendPool.CheckTimeout();
}

// Spysend transaction was completed (failed or successful)
void CSpysendPool::CompletedTransaction(bool error, int errorID)
{
    if(fEternityNode) return;

    if(error){
        LogPrintf("CompletedTransaction -- error \n");
        UpdateState(POOL_STATUS_ERROR);

        Check();
        UnlockCoins();
        SetNull();
    } else {
        LogPrintf("CompletedTransaction -- success \n");
        UpdateState(POOL_STATUS_SUCCESS);

        UnlockCoins();
        SetNull();

        // To avoid race conditions, we'll only let SS run once per block
        cachedLastSuccess = chainActive.Tip()->nHeight;
    }
    lastMessage = GetMessageByID(errorID);
}

void CSpysendPool::ClearLastMessage()
{
    lastMessage = "";
}

//
// Passively run Spysend in the background to anonymize funds based on the given configuration.
//
// This does NOT run by default for daemons, only for QT.
//
bool CSpysendPool::DoAutomaticDenominating(bool fDryRun)
{
    if(!fEnableSpysend) return false;
    if(fEternityNode) return false;
    if(state == POOL_STATUS_ERROR || state == POOL_STATUS_SUCCESS) return false;
    if(GetEntriesCount() > 0) {
        strAutoDenomResult = _("Mixing in progress...");
        return false;
    }

    TRY_LOCK(cs_spysend, lockDS);
    if(!lockDS) {
        strAutoDenomResult = _("Lock is already in place.");
        return false;
    }

    if(!eternitynodeSync.IsBlockchainSynced()) {
        strAutoDenomResult = _("Can't mix while sync in progress.");
        return false;
    }

    if (!fDryRun && pwalletMain->IsLocked()){
        strAutoDenomResult = _("Wallet is locked.");
        return false;
    }

    if(chainActive.Tip()->nHeight - cachedLastSuccess < minBlockSpacing) {
        LogPrintf("CSpysendPool::DoAutomaticDenominating - Last successful Spysend action was too recent\n");
        strAutoDenomResult = _("Last successful Spysend action was too recent.");
        return false;
    }

    if(mnodeman.size() == 0){
        LogPrint("spysend", "CSpysendPool::DoAutomaticDenominating - No Eternitynodes detected\n");
        strAutoDenomResult = _("No Eternitynodes detected.");
        return false;
    }

    // ** find the coins we'll use
    std::vector<CTxIn> vCoins;
    CAmount nValueMin = CENT;
    CAmount nValueIn = 0;

    CAmount nOnlyDenominatedBalance;
    CAmount nBalanceNeedsDenominated;

    // should not be less than fees in SPYSEND_COLLATERAL + few (lets say 5) smallest denoms
    CAmount nLowestDenom = SPYSEND_COLLATERAL + spySendDenominations[spySendDenominations.size() - 1]*5;

    // if there are no SS collateral inputs yet
    if(!pwalletMain->HasCollateralInputs())
        // should have some additional amount for them
        nLowestDenom += SPYSEND_COLLATERAL*4;

    CAmount nBalanceNeedsAnonymized = nAnonymizeEternityAmount*COIN - pwalletMain->GetAnonymizedBalance();

    // if balanceNeedsAnonymized is more than pool max, take the pool max
    if(nBalanceNeedsAnonymized > SPYSEND_POOL_MAX) nBalanceNeedsAnonymized = SPYSEND_POOL_MAX;

    // if balanceNeedsAnonymized is more than non-anonymized, take non-anonymized
    CAmount nAnonymizableBalance = pwalletMain->GetAnonymizableBalance();
    if(nBalanceNeedsAnonymized > nAnonymizableBalance) nBalanceNeedsAnonymized = nAnonymizableBalance;

    if(nBalanceNeedsAnonymized < nLowestDenom)
    {
        LogPrintf("DoAutomaticDenominating : No funds detected in need of denominating \n");
        strAutoDenomResult = _("No funds detected in need of denominating.");
        return false;
    }

    LogPrint("spysend", "DoAutomaticDenominating : nLowestDenom=%d, nBalanceNeedsAnonymized=%d\n", nLowestDenom, nBalanceNeedsAnonymized);

    // select coins that should be given to the pool
    if (!pwalletMain->SelectCoinsDark(nValueMin, nBalanceNeedsAnonymized, vCoins, nValueIn, 0, nSpysendRounds))
    {
        nValueIn = 0;
        vCoins.clear();

        if (pwalletMain->SelectCoinsDark(nValueMin, 9999999*COIN, vCoins, nValueIn, -2, 0))
        {
            nOnlyDenominatedBalance = pwalletMain->GetDenominatedBalance(true) + pwalletMain->GetDenominatedBalance() - pwalletMain->GetAnonymizedBalance();
            nBalanceNeedsDenominated = nBalanceNeedsAnonymized - nOnlyDenominatedBalance;

            if(nBalanceNeedsDenominated > nValueIn) nBalanceNeedsDenominated = nValueIn;

            if(nBalanceNeedsDenominated < nLowestDenom) return false; // most likely we just waiting for denoms to confirm
            if(!fDryRun) return CreateDenominated(nBalanceNeedsDenominated);

            return true;
        } else {
            LogPrintf("DoAutomaticDenominating : Can't denominate - no compatible inputs left\n");
            strAutoDenomResult = _("Can't denominate: no compatible inputs left.");
            return false;
        }

    }

    if(fDryRun) return true;

    nOnlyDenominatedBalance = pwalletMain->GetDenominatedBalance(true) + pwalletMain->GetDenominatedBalance() - pwalletMain->GetAnonymizedBalance();
    nBalanceNeedsDenominated = nBalanceNeedsAnonymized - nOnlyDenominatedBalance;

    //check if we have should create more denominated inputs
    if(nBalanceNeedsDenominated > nOnlyDenominatedBalance) return CreateDenominated(nBalanceNeedsDenominated);

    //check if we have the collateral sized inputs
    if(!pwalletMain->HasCollateralInputs()) return !pwalletMain->HasCollateralInputs(false) && MakeCollateralAmounts();

    std::vector<CTxOut> vOut;

    // initial phase, find a Eternitynode
    if(!sessionFoundEternitynode){
        // Clean if there is anything left from previous session
        UnlockCoins();
        SetNull();

        int nUseQueue = rand()%100;
        UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);

        if(pwalletMain->GetDenominatedBalance(true) > 0){ //get denominated unconfirmed inputs
            LogPrintf("DoAutomaticDenominating -- Found unconfirmed denominated outputs, will wait till they confirm to continue.\n");
            strAutoDenomResult = _("Found unconfirmed denominated outputs, will wait till they confirm to continue.");
            return false;
        }

        //check our collateral nad create new if needed
        std::string strReason;
        CValidationState state;
        if(txCollateral == CMutableTransaction()){
            if(!pwalletMain->CreateCollateralTransaction(txCollateral, strReason)){
                LogPrintf("% -- create collateral error:%s\n", __func__, strReason);
                return false;
            }
        } else {
            if(!IsCollateralValid(txCollateral)) {
                LogPrintf("%s -- invalid collateral, recreating...\n", __func__);
                if(!pwalletMain->CreateCollateralTransaction(txCollateral, strReason)){
                    LogPrintf("%s -- create collateral error: %s\n", __func__, strReason);
                    return false;
                }
            }
        }

        //if we've used 90% of the Eternitynode list then drop all the oldest first
        int nThreshold = (int)(mnodeman.CountEnabled(MIN_POOL_PEER_PROTO_VERSION) * 0.9);
        LogPrint("spysend", "Checking vecEternitynodesUsed size %d threshold %d\n", (int)vecEternitynodesUsed.size(), nThreshold);
        while((int)vecEternitynodesUsed.size() > nThreshold){
            vecEternitynodesUsed.erase(vecEternitynodesUsed.begin());
            LogPrint("spysend", "  vecEternitynodesUsed size %d threshold %d\n", (int)vecEternitynodesUsed.size(), nThreshold);
        }

        //don't use the queues all of the time for mixing
        if(nUseQueue > 33){

            // Look through the queues and see if anything matches
            BOOST_FOREACH(CSpysendQueue& dsq, vecSpysendQueue){
                CService addr;
                if(dsq.time == 0) continue;

                if(!dsq.GetAddress(addr)) continue;
                if(dsq.IsExpired()) continue;

                int protocolVersion;
                if(!dsq.GetProtocolVersion(protocolVersion)) continue;
                if(protocolVersion < MIN_POOL_PEER_PROTO_VERSION) continue;

                //non-denom's are incompatible
                if((dsq.nDenom & (1 << 4))) continue;

                bool fUsed = false;
                //don't reuse Eternitynodes
                BOOST_FOREACH(CTxIn usedVin, vecEternitynodesUsed){
                    if(dsq.vin == usedVin) {
                        fUsed = true;
                        break;
                    }
                }
                if(fUsed) continue;

                std::vector<CTxIn> vTempCoins;
                std::vector<COutput> vTempCoins2;
                // Try to match their denominations if possible
                if (!pwalletMain->SelectCoinsByDenominations(dsq.nDenom, nValueMin, nBalanceNeedsAnonymized, vTempCoins, vTempCoins2, nValueIn, 0, nSpysendRounds)){
                    LogPrintf("DoAutomaticDenominating --- Couldn't match denominations %d\n", dsq.nDenom);
                    continue;
                }

                CEternitynode* pen = mnodeman.Find(dsq.vin);
                if(pen == NULL)
                {
                    LogPrintf("DoAutomaticDenominating --- dsq vin %s is not in eternitynode list!", dsq.vin.ToString());
                    continue;
                }

                LogPrintf("DoAutomaticDenominating --- attempt to connect to eternitynode from queue %s\n", pen->addr.ToString());
                lastTimeChanged = GetTimeMillis();
                // connect to Eternitynode and submit the queue request
                CNode* pnode = ConnectNode((CAddress)addr, NULL, true);
                if(pnode != NULL)
                {
                    pSubmittedToEternitynode = pen;
                    vecEternitynodesUsed.push_back(dsq.vin);
                    sessionDenom = dsq.nDenom;

                    pnode->PushMessage("dsa", sessionDenom, txCollateral);
                    LogPrintf("DoAutomaticDenominating --- connected (from queue), sending dsa for %d - %s\n", sessionDenom, pnode->addr.ToString());
                    strAutoDenomResult = _("Mixing in progress...");
                    dsq.time = 0; //remove node
                    return true;
                } else {
                    LogPrintf("DoAutomaticDenominating --- error connecting \n");
                    strAutoDenomResult = _("Error connecting to Eternitynode.");
                    dsq.time = 0; //remove node
                    continue;
                }
            }
        }

        // do not initiate queue if we are a liquidity proveder to avoid useless inter-mixing
        if(nLiquidityProvider) return false;

        int i = 0;

        // otherwise, try one randomly
        while(i < 10)
        {
            CEternitynode* pen = mnodeman.FindRandomNotInVec(vecEternitynodesUsed, MIN_POOL_PEER_PROTO_VERSION);
            if(pen == NULL)
            {
                LogPrintf("DoAutomaticDenominating --- Can't find random eternitynode!\n");
                strAutoDenomResult = _("Can't find random Eternitynode.");
                return false;
            }

            if(pen->nLastDsq != 0 &&
                pen->nLastDsq + mnodeman.CountEnabled(MIN_POOL_PEER_PROTO_VERSION)/5 > mnodeman.nDsqCount){
                i++;
                continue;
            }

            lastTimeChanged = GetTimeMillis();
            LogPrintf("DoAutomaticDenominating --- attempt %d connection to Eternitynode %s\n", i, pen->addr.ToString());
            CNode* pnode = ConnectNode((CAddress)pen->addr, NULL, true);
            if(pnode != NULL){
                pSubmittedToEternitynode = pen;
                vecEternitynodesUsed.push_back(pen->vin);

                std::vector<CAmount> vecAmounts;
                pwalletMain->ConvertList(vCoins, vecAmounts);
                // try to get a single random denom out of vecAmounts
                while(sessionDenom == 0)
                    sessionDenom = GetDenominationsByAmounts(vecAmounts);

                pnode->PushMessage("dsa", sessionDenom, txCollateral);
                LogPrintf("DoAutomaticDenominating --- connected, sending dsa for %d\n", sessionDenom);
                strAutoDenomResult = _("Mixing in progress...");
                return true;
            } else {
                vecEternitynodesUsed.push_back(pen->vin); // postpone MN we wasn't able to connect to
                i++;
                continue;
            }
        }

        strAutoDenomResult = _("No compatible Eternitynode found.");
        return false;
    }

    strAutoDenomResult = _("Mixing in progress...");
    return false;
}


bool CSpysendPool::PrepareSpysendDenominate()
{
    std::string strError = "";
    // Submit transaction to the pool if we get here
    // Try to use only inputs with the same number of rounds starting from lowest number of rounds possible
    for(int i = 0; i < nSpysendRounds; i++) {
        strError = pwalletMain->PrepareSpysendDenominate(i, i+1);
        LogPrintf("DoAutomaticDenominating : Running Spysend denominate for %d rounds. Return '%s'\n", i, strError);
        if(strError == "") return true;
    }

    // We failed? That's strange but let's just make final attempt and try to mix everything
    strError = pwalletMain->PrepareSpysendDenominate(0, nSpysendRounds);
    LogPrintf("DoAutomaticDenominating : Running Spysend denominate for all rounds. Return '%s'\n", strError);
    if(strError == "") return true;

    // Should never actually get here but just in case
    strAutoDenomResult = strError;
    LogPrintf("DoAutomaticDenominating : Error running denominate, %s\n", strError);
    return false;
}

bool CSpysendPool::SendRandomPaymentToSelf()
{
    int64_t nBalance = pwalletMain->GetBalance();
    int64_t nPayment = (nBalance*0.35) + (rand() % nBalance);

    if(nPayment > nBalance) nPayment = nBalance-(0.1*COIN);

    // make our change address
    CReserveKey reservekey(pwalletMain);

    CScript scriptChange;
    CPubKey vchPubKey;
    assert(reservekey.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
    scriptChange = GetScriptForDestination(vchPubKey.GetID());

    CWalletTx wtx;
    int64_t nFeeRet = 0;
    std::string strFail = "";
    vector< pair<CScript, int64_t> > vecSend;

    // ****** Add fees ************ /
    vecSend.push_back(make_pair(scriptChange, nPayment));

    CCoinControl *coinControl=NULL;
    bool success = pwalletMain->CreateTransaction(vecSend, wtx, reservekey, nFeeRet, strFail, coinControl, ONLY_DENOMINATED);
    if(!success){
        LogPrintf("SendRandomPaymentToSelf: Error - %s\n", strFail);
        return false;
    }

    pwalletMain->CommitTransaction(wtx, reservekey);

    LogPrintf("SendRandomPaymentToSelf Success: tx %s\n", wtx.GetHash().GetHex());

    return true;
}

// Split up large inputs or create fee sized inputs
bool CSpysendPool::MakeCollateralAmounts()
{
    CWalletTx wtx;
    int64_t nFeeRet = 0;
    std::string strFail = "";
    vector< pair<CScript, int64_t> > vecSend;
    CCoinControl *coinControl = NULL;

    // make our collateral address
    CReserveKey reservekeyCollateral(pwalletMain);
    // make our change address
    CReserveKey reservekeyChange(pwalletMain);

    CScript scriptCollateral;
    CPubKey vchPubKey;
    assert(reservekeyCollateral.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
    scriptCollateral = GetScriptForDestination(vchPubKey.GetID());

    vecSend.push_back(make_pair(scriptCollateral, SPYSEND_COLLATERAL*4));

    // try to use non-denominated and not mn-like funds
    bool success = pwalletMain->CreateTransaction(vecSend, wtx, reservekeyChange,
            nFeeRet, strFail, coinControl, ONLY_NONDENOMINATED_NOT1000IFMN);
    if(!success){
        // if we failed (most likeky not enough funds), try to use all coins instead -
        // MN-like funds should not be touched in any case and we can't mix denominated without collaterals anyway
        LogPrintf("MakeCollateralAmounts: ONLY_NONDENOMINATED_NOT1000IFMN Error - %s\n", strFail);
        success = pwalletMain->CreateTransaction(vecSend, wtx, reservekeyChange,
                nFeeRet, strFail, coinControl, ONLY_NOT1000IFMN);
        if(!success){
            LogPrintf("MakeCollateralAmounts: ONLY_NOT1000IFMN Error - %s\n", strFail);
            reservekeyCollateral.ReturnKey();
            return false;
        }
    }

    reservekeyCollateral.KeepKey();

    LogPrintf("MakeCollateralAmounts: tx %s\n", wtx.GetHash().GetHex());

    // use the same cachedLastSuccess as for SS mixinx to prevent race
    if(!pwalletMain->CommitTransaction(wtx, reservekeyChange)) {
        LogPrintf("MakeCollateralAmounts: CommitTransaction failed!\n");
        return false;
    }

    cachedLastSuccess = chainActive.Tip()->nHeight;

    return true;
}

// Create denominations
bool CSpysendPool::CreateDenominated(int64_t nTotalValue)
{
    CWalletTx wtx;
    int64_t nFeeRet = 0;
    std::string strFail = "";
    vector< pair<CScript, int64_t> > vecSend;
    int64_t nValueLeft = nTotalValue;

    // make our collateral address
    CReserveKey reservekeyCollateral(pwalletMain);
    // make our change address
    CReserveKey reservekeyChange(pwalletMain);
    // make our denom addresses
    CReserveKey reservekeyDenom(pwalletMain);

    CScript scriptCollateral;
    CPubKey vchPubKey;
    assert(reservekeyCollateral.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
    scriptCollateral = GetScriptForDestination(vchPubKey.GetID());

    // ****** Add collateral outputs ************ /
    if(!pwalletMain->HasCollateralInputs()) {
        vecSend.push_back(make_pair(scriptCollateral, SPYSEND_COLLATERAL*4));
        nValueLeft -= SPYSEND_COLLATERAL*4;
    }

    // ****** Add denoms ************ /
    BOOST_REVERSE_FOREACH(int64_t v, spySendDenominations){
        int nOutputs = 0;

        // add each output up to 10 times until it can't be added again
        while(nValueLeft - v >= SPYSEND_COLLATERAL && nOutputs <= 10) {
            CScript scriptDenom;
            CPubKey vchPubKey;
            //use a unique change address
            assert(reservekeyDenom.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
            scriptDenom = GetScriptForDestination(vchPubKey.GetID());
            // TODO: do not keep reservekeyDenom here
            reservekeyDenom.KeepKey();

            vecSend.push_back(make_pair(scriptDenom, v));

            //increment outputs and subtract denomination amount
            nOutputs++;
            nValueLeft -= v;
            LogPrintf("CreateDenominated1 %d\n", nValueLeft);
        }

        if(nValueLeft == 0) break;
    }
    LogPrintf("CreateDenominated2 %d\n", nValueLeft);

    // if we have anything left over, it will be automatically send back as change - there is no need to send it manually

    CCoinControl *coinControl=NULL;
    bool success = pwalletMain->CreateTransaction(vecSend, wtx, reservekeyChange,
            nFeeRet, strFail, coinControl, ONLY_NONDENOMINATED_NOT1000IFMN);
    if(!success){
        LogPrintf("CreateDenominated: Error - %s\n", strFail);
        // TODO: return reservekeyDenom here
        reservekeyCollateral.ReturnKey();
        return false;
    }

    // TODO: keep reservekeyDenom here
    reservekeyCollateral.KeepKey();

    // use the same cachedLastSuccess as for SS mixinx to prevent race
    if(pwalletMain->CommitTransaction(wtx, reservekeyChange))
        cachedLastSuccess = chainActive.Tip()->nHeight;
    else
        LogPrintf("CreateDenominated: CommitTransaction failed!\n");

    LogPrintf("CreateDenominated: tx %s\n", wtx.GetHash().GetHex());

    return true;
}

bool CSpysendPool::IsCompatibleWithEntries(std::vector<CTxOut>& vout)
{
    if(GetDenominations(vout) == 0) return false;

    BOOST_FOREACH(const CSpySendEntry v, entries) {
        LogPrintf(" IsCompatibleWithEntries %d %d\n", GetDenominations(vout), GetDenominations(v.vout));
/*
        BOOST_FOREACH(CTxOut o1, vout)
            LogPrintf(" vout 1 - %s\n", o1.ToString());

        BOOST_FOREACH(CTxOut o2, v.vout)
            LogPrintf(" vout 2 - %s\n", o2.ToString());
*/
        if(GetDenominations(vout) != GetDenominations(v.vout)) return false;
    }

    return true;
}

bool CSpysendPool::IsCompatibleWithSession(int64_t nDenom, CTransaction txCollateral, int& errorID)
{
    if(nDenom == 0) return false;

    LogPrintf("CSpysendPool::IsCompatibleWithSession - sessionDenom %d sessionUsers %d\n", sessionDenom, sessionUsers);

    if (!unitTest && !IsCollateralValid(txCollateral)){
        LogPrint("spysend", "CSpysendPool::IsCompatibleWithSession - collateral not valid!\n");
        errorID = ERR_INVALID_COLLATERAL;
        return false;
    }

    if(sessionUsers < 0) sessionUsers = 0;

    if(sessionUsers == 0) {
        sessionID = 1 + (rand() % 999999);
        sessionDenom = nDenom;
        sessionUsers++;
        lastTimeChanged = GetTimeMillis();

        if(!unitTest){
            //broadcast that I'm accepting entries, only if it's the first entry through
            CSpysendQueue dsq;
            dsq.nDenom = nDenom;
            dsq.vin = activeEternitynode.vin;
            dsq.time = GetTime();
            dsq.Sign();
            dsq.Relay();
        }

        UpdateState(POOL_STATUS_QUEUE);
        vecSessionCollateral.push_back(txCollateral);
        return true;
    }

    if((state != POOL_STATUS_ACCEPTING_ENTRIES && state != POOL_STATUS_QUEUE) || sessionUsers >= GetMaxPoolTransactions()){
        if((state != POOL_STATUS_ACCEPTING_ENTRIES && state != POOL_STATUS_QUEUE)) errorID = ERR_MODE;
        if(sessionUsers >= GetMaxPoolTransactions()) errorID = ERR_QUEUE_FULL;
        LogPrintf("CSpysendPool::IsCompatibleWithSession - incompatible mode, return false %d %d\n", state != POOL_STATUS_ACCEPTING_ENTRIES, sessionUsers >= GetMaxPoolTransactions());
        return false;
    }

    if(nDenom != sessionDenom) {
        errorID = ERR_DENOM;
        return false;
    }

    LogPrintf("CSpySendPool::IsCompatibleWithSession - compatible\n");

    sessionUsers++;
    lastTimeChanged = GetTimeMillis();
    vecSessionCollateral.push_back(txCollateral);

    return true;
}

//create a nice string to show the denominations
void CSpysendPool::GetDenominationsToString(int nDenom, std::string& strDenom){
    // Function returns as follows:
    //
    // bit 0 - 100DRK+1 ( bit on if present )
    // bit 1 - 10DRK+1
    // bit 2 - 1DRK+1
    // bit 3 - .1DRK+1
    // bit 3 - non-denom


    strDenom = "";

    if(nDenom & (1 << 0)) {
        if(strDenom.size() > 0) strDenom += "+";
        strDenom += "100";
    }

    if(nDenom & (1 << 1)) {
        if(strDenom.size() > 0) strDenom += "+";
        strDenom += "10";
    }

    if(nDenom & (1 << 2)) {
        if(strDenom.size() > 0) strDenom += "+";
        strDenom += "1";
    }

    if(nDenom & (1 << 3)) {
        if(strDenom.size() > 0) strDenom += "+";
        strDenom += "0.1";
    }
}

int CSpysendPool::GetDenominations(const std::vector<CTxDSOut>& vout){
    std::vector<CTxOut> vout2;

    BOOST_FOREACH(CTxDSOut out, vout)
        vout2.push_back(out);

    return GetDenominations(vout2);
}

// return a bitshifted integer representing the denominations in this list
int CSpysendPool::GetDenominations(const std::vector<CTxOut>& vout, bool fSingleRandomDenom){
    std::vector<pair<int64_t, int> > denomUsed;

    // make a list of denominations, with zero uses
    BOOST_FOREACH(int64_t d, spySendDenominations)
        denomUsed.push_back(make_pair(d, 0));

    // look for denominations and update uses to 1
    BOOST_FOREACH(CTxOut out, vout){
        bool found = false;
        BOOST_FOREACH (PAIRTYPE(int64_t, int)& s, denomUsed){
            if (out.nValue == s.first){
                s.second = 1;
                found = true;
            }
        }
        if(!found) return 0;
    }

    int denom = 0;
    int c = 0;
    // if the denomination is used, shift the bit on.
    // then move to the next
    BOOST_FOREACH (PAIRTYPE(int64_t, int)& s, denomUsed) {
        int bit = (fSingleRandomDenom ? rand()%2 : 1) * s.second;
        denom |= bit << c++;
        if(fSingleRandomDenom && bit) break; // use just one random denomination
    }

    // Function returns as follows:
    //
    // bit 0 - 100DRK+1 ( bit on if present )
    // bit 1 - 10DRK+1
    // bit 2 - 1DRK+1
    // bit 3 - .1DRK+1

    return denom;
}


int CSpysendPool::GetDenominationsByAmounts(std::vector<int64_t>& vecAmount){
    CScript e = CScript();
    std::vector<CTxOut> vout1;

    // Make outputs by looping through denominations, from small to large
    BOOST_REVERSE_FOREACH(int64_t v, vecAmount){
        CTxOut o(v, e);
        vout1.push_back(o);
    }

    return GetDenominations(vout1, true);
}

int CSpysendPool::GetDenominationsByAmount(int64_t nAmount, int nDenomTarget){
    CScript e = CScript();
    int64_t nValueLeft = nAmount;

    std::vector<CTxOut> vout1;

    // Make outputs by looping through denominations, from small to large
    BOOST_REVERSE_FOREACH(int64_t v, spySendDenominations){
        if(nDenomTarget != 0){
            bool fAccepted = false;
            if((nDenomTarget & (1 << 0)) &&      v == ((100*COIN)+100000)) {fAccepted = true;}
            else if((nDenomTarget & (1 << 1)) && v == ((10*COIN) +10000)) {fAccepted = true;}
            else if((nDenomTarget & (1 << 2)) && v == ((1*COIN)  +1000)) {fAccepted = true;}
            else if((nDenomTarget & (1 << 3)) && v == ((.1*COIN) +100)) {fAccepted = true;}
            if(!fAccepted) continue;
        }

        int nOutputs = 0;

        // add each output up to 10 times until it can't be added again
        while(nValueLeft - v >= 0 && nOutputs <= 10) {
            CTxOut o(v, e);
            vout1.push_back(o);
            nValueLeft -= v;
            nOutputs++;
        }
        LogPrintf("GetDenominationsByAmount --- %d nOutputs %d\n", v, nOutputs);
    }

    return GetDenominations(vout1);
}

std::string CSpysendPool::GetMessageByID(int messageID) {
    switch (messageID) {
    case ERR_ALREADY_HAVE: return _("Already have that input.");
    case ERR_DENOM: return _("No matching denominations found for mixing.");
    case ERR_ENTRIES_FULL: return _("Entries are full.");
    case ERR_EXISTING_TX: return _("Not compatible with existing transactions.");
    case ERR_FEES: return _("Transaction fees are too high.");
    case ERR_INVALID_COLLATERAL: return _("Collateral not valid.");
    case ERR_INVALID_INPUT: return _("Input is not valid.");
    case ERR_INVALID_SCRIPT: return _("Invalid script detected.");
    case ERR_INVALID_TX: return _("Transaction not valid.");
    case ERR_MAXIMUM: return _("Value more than Spysend pool maximum allows.");
    case ERR_MN_LIST: return _("Not in the Eternitynode list.");
    case ERR_MODE: return _("Incompatible mode.");
    case ERR_NON_STANDARD_PUBKEY: return _("Non-standard public key detected.");
    case ERR_NOT_A_MN: return _("This is not a Eternitynode.");
    case ERR_QUEUE_FULL: return _("Eternitynode queue is full.");
    case ERR_RECENT: return _("Last Spysend was too recent.");
    case ERR_SESSION: return _("Session not complete!");
    case ERR_MISSING_TX: return _("Missing input transaction information.");
    case ERR_VERSION: return _("Incompatible version.");
    case MSG_SUCCESS: return _("Transaction created successfully.");
    case MSG_ENTRIES_ADDED: return _("Your entries added successfully.");
    case MSG_NOERR:
    default:
        return "";
    }
}

bool CSpySendSigner::IsVinAssociatedWithPubkey(CTxIn& vin, CPubKey& pubkey){
    CScript payee2;
    payee2 = GetScriptForDestination(pubkey.GetID());

    CTransaction txVin;
    uint256 hash;
    if(GetTransaction(vin.prevout.hash, txVin, hash, true)){
        BOOST_FOREACH(CTxOut out, txVin.vout){
            if(out.nValue == 1000*COIN){
                if(out.scriptPubKey == payee2) return true;
            }
        }
    }

    return false;
}

bool CSpySendSigner::SetKey(std::string strSecret, std::string& errorMessage, CKey& key, CPubKey& pubkey){
    CBitcoinSecret vchSecret;
    bool fGood = vchSecret.SetString(strSecret);

    if (!fGood) {
        errorMessage = _("Invalid private key.");
        return false;
    }

    key = vchSecret.GetKey();
    pubkey = key.GetPubKey();

    return true;
}

bool CSpySendSigner::SignMessage(std::string strMessage, std::string& errorMessage, vector<unsigned char>& vchSig, CKey key)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    if (!key.SignCompact(ss.GetHash(), vchSig)) {
        errorMessage = _("Signing failed.");
        return false;
    }

    return true;
}

bool CSpySendSigner::VerifyMessage(CPubKey pubkey, vector<unsigned char>& vchSig, std::string strMessage, std::string& errorMessage)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    CPubKey pubkey2;
    if (!pubkey2.RecoverCompact(ss.GetHash(), vchSig)) {
        errorMessage = _("Error recovering public key.");
        return false;
    }

    if (pubkey2.GetID() != pubkey.GetID()) {
        errorMessage = strprintf("keys don't match - input: %s, recovered: %s, message: %s, sig: %s\n",
                    pubkey.GetID().ToString(), pubkey2.GetID().ToString(), strMessage,
                    EncodeBase64(&vchSig[0], vchSig.size()));
        return false;
    }

    return true;
}

bool CSpysendQueue::Sign()
{
    if(!fEternityNode) return false;

    std::string strMessage = vin.ToString() + boost::lexical_cast<std::string>(nDenom) + boost::lexical_cast<std::string>(time) + boost::lexical_cast<std::string>(ready);

    CKey key2;
    CPubKey pubkey2;
    std::string errorMessage = "";

    if(!spySendSigner.SetKey(strEternityNodePrivKey, errorMessage, key2, pubkey2))
    {
        LogPrintf("CSpysendQueue():Relay - ERROR: Invalid Eternitynodeprivkey: '%s'\n", errorMessage);
        return false;
    }

    if(!spySendSigner.SignMessage(strMessage, errorMessage, vchSig, key2)) {
        LogPrintf("CSpysendQueue():Relay - Sign message failed");
        return false;
    }

    if(!spySendSigner.VerifyMessage(pubkey2, vchSig, strMessage, errorMessage)) {
        LogPrintf("CSpysendQueue():Relay - Verify message failed");
        return false;
    }

    return true;
}

bool CSpysendQueue::Relay()
{

    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes){
        // always relay to everyone
        pnode->PushMessage("dsq", (*this));
    }

    return true;
}

bool CSpysendQueue::CheckSignature()
{
    CEternitynode* pen = mnodeman.Find(vin);

    if(pen != NULL)
    {
        std::string strMessage = vin.ToString() + boost::lexical_cast<std::string>(nDenom) + boost::lexical_cast<std::string>(time) + boost::lexical_cast<std::string>(ready);

        std::string errorMessage = "";
        if(!spySendSigner.VerifyMessage(pen->pubkey2, vchSig, strMessage, errorMessage)){
            return error("CSpysendQueue::CheckSignature() - Got bad Eternitynode address signature %s \n", vin.ToString().c_str());
        }

        return true;
    }

    return false;
}


void CSpysendPool::RelayFinalTransaction(const int sessionID, const CTransaction& txNew)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        pnode->PushMessage("dsf", sessionID, txNew);
    }
}

void CSpysendPool::RelayIn(const std::vector<CTxDSIn>& vin, const int64_t& nAmount, const CTransaction& txCollateral, const std::vector<CTxDSOut>& vout)
{
    if(!pSubmittedToEternitynode) return;

    std::vector<CTxIn> vin2;
    std::vector<CTxOut> vout2;

    BOOST_FOREACH(CTxDSIn in, vin)
        vin2.push_back(in);

    BOOST_FOREACH(CTxDSOut out, vout)
        vout2.push_back(out);

    CNode* pnode = FindNode(pSubmittedToEternitynode->addr);
    if(pnode != NULL) {
        LogPrintf("RelayIn - found master, relaying message - %s \n", pnode->addr.ToString());
        pnode->PushMessage("dsi", vin2, nAmount, txCollateral, vout2);
    }
}

void CSpysendPool::RelayStatus(const int sessionID, const int newState, const int newEntriesCount, const int newAccepted, const int errorID)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
        pnode->PushMessage("dssu", sessionID, newState, newEntriesCount, newAccepted, errorID);
}

void CSpysendPool::RelayCompletedTransaction(const int sessionID, const bool error, const int errorID)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
        pnode->PushMessage("dsc", sessionID, error, errorID);
}

//TODO: Rename/move to core
void ThreadCheckSpySendPool()
{
    if(fLiteMode) return; //disable all Spysend/Eternitynode related functionality

    // Make this thread recognisable as the wallet flushing thread
    RenameThread("eternity-spysend");

    unsigned int c = 0;

    while (true)
    {
        MilliSleep(1000);
        //LogPrintf("ThreadCheckSpySendPool::check timeout\n");

        // try to sync from all available nodes, one step at a time
        eternitynodeSync.Process();

        if(eternitynodeSync.IsBlockchainSynced()) {

            c++;

            // check if we should activate or ping every few minutes,
            // start right after sync is considered to be done
            if(c % ETERNITYNODE_PING_SECONDS == 1) activeEternitynode.ManageStatus();

            if(c % 60 == 0)
            {
                mnodeman.CheckAndRemove();
                mnodeman.ProcessEternitynodeConnections();
                eternitynodePayments.CleanPaymentList();
                CleanTransactionLocksList();
            }

            //if(c % ETERNITYNODES_DUMP_SECONDS == 0) DumpEternitynodes();

            spySendPool.CheckTimeout();
            spySendPool.CheckForCompleteQueue();

            if(spySendPool.GetState() == POOL_STATUS_IDLE && c % 15 == 0){
                spySendPool.DoAutomaticDenominating();
            }
        }
    }
}
