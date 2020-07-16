#include "util.h"

#include "clientversion.h"
#include "primitives/transaction.h"
#include "random.h"
#include "sync.h"
#include "utilstrencodings.h"
#include "utilmoneystr.h"
#include "test/test_bitcoin.h"

#include <stdint.h>
#include <vector>

#include "chainparams.h"
#include "consensus/consensus.h"
#include "consensus/validation.h"
#include "key.h"
#include "validation.h"
#include "miner.h"
#include "pubkey.h"
#include "random.h"
#include "txdb.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "rpc/server.h"
#include "rpc/register.h"
#include "zerocoin.h"
#include "sigma.h"

#include "test/fixtures.h"
#include "test/testutil.h"

#include "wallet/db.h"
#include "wallet/wallet.h"
#include "wallet/walletexcept.h"

#include <boost/filesystem.hpp>
#include <boost/test/unit_test.hpp>
#include <boost/thread.hpp>

BOOST_FIXTURE_TEST_SUITE(zerocoin_tests3_v3, ZerocoinTestingSetup200)

BOOST_AUTO_TEST_CASE(zerocoin_mintspend_v3)
{
    sigma::CSigmaState *sigmaState = sigma::CSigmaState::GetState();
    string denomination;
    std::vector<string> denominations = {"0.1", "0.5", "1", "10", "100"};

    // Create 400-200+1 = 201 new empty blocks. // consensus.nMintV3SigmaStartBlock = 400
    CreateAndProcessEmptyBlocks(201, scriptPubKey);

    const auto& sigmaParams = sigma::Params::get_default();
    for(int i = 0; i < 5; i++)
    {
        denomination = denominations[i];
        sigma::CoinDenomination denom;
        sigma::StringToDenomination(denomination, denom);
        string stringError;
        //Make sure that transactions get to mempool
        pwalletMain->SetBroadcastTransactions(true);

        //Verify Mint is successful
        {
            std::vector<sigma::PrivateCoin> privCoins(1, sigma::PrivateCoin(sigmaParams, denom));

            CWalletTx wtx;
            vector<CHDMint> vDMints;
            auto vecSend = CWallet::CreateSigmaMintRecipients(privCoins, vDMints);
            stringError = pwalletMain->MintAndStoreSigma(vecSend, privCoins, vDMints, wtx);

            BOOST_CHECK_MESSAGE(stringError == "", "Mint Failed");
        }

        //Verify Mint gets in the mempool
        BOOST_CHECK_MESSAGE(mempool.size() == 1, "Mint was not added to mempool");

        int previousHeight = chainActive.Height();
        CBlock b = CreateAndProcessBlock(scriptPubKey);
        BOOST_CHECK_MESSAGE(previousHeight + 1 == chainActive.Height(), "Block not added to chain");

        // Generate address
        CPubKey newKey;
        BOOST_CHECK_MESSAGE(pwalletMain->GetKeyFromPool(newKey), "Fail to get new address");

        const CBitcoinAddress randomAddr(newKey.GetID());

        CAmount nAmount;
        DenominationToInteger(denom, nAmount);

        std::vector<CRecipient> recipients = {
                {GetScriptForDestination(randomAddr.Get()), nAmount, true},
        };

        previousHeight = chainActive.Height();
        //Add 5 more blocks and verify that Mint can not be spent until 6 blocks verification
        for (int i = 0; i < 5; i++)
        {
            {
                CWalletTx wtx;
                BOOST_CHECK_THROW(pwalletMain->SpendSigma(recipients, wtx), WalletError); //this must throw as 6 blocks have not passed yet,
            }

            CBlock b = CreateAndProcessBlock(scriptPubKey);
        }
        BOOST_CHECK_MESSAGE(previousHeight + 5 == chainActive.Height(), "Block not added to chain");

        {
            CWalletTx wtx;
            BOOST_CHECK_THROW(pwalletMain->SpendSigma(recipients, wtx), WalletError); //this must throw as it has to have at least two mint coins with at least 6 confirmation
        }


        {
            std::vector<sigma::PrivateCoin> privCoins(1, sigma::PrivateCoin(sigmaParams, denom));

            CWalletTx wtx;
            vector<CHDMint> vDMints;
            auto vecSend = CWallet::CreateSigmaMintRecipients(privCoins, vDMints);
            stringError = pwalletMain->MintAndStoreSigma(vecSend, privCoins, vDMints, wtx);

            BOOST_CHECK_MESSAGE(stringError == "", "Mint Failed");
        }

        BOOST_CHECK_MESSAGE(mempool.size() == 1, "Mint was not added to mempool");

        previousHeight = chainActive.Height();
        b = CreateAndProcessBlock(scriptPubKey);
        BOOST_CHECK_MESSAGE(previousHeight + 1 == chainActive.Height(), "Block not added to chain");


        previousHeight = chainActive.Height();
        //Add 5 more blocks and verify that Mint can not be spent until 6 blocks verification
        for (int i = 0; i < 5; i++)
        {
            {
                CWalletTx wtx;
                BOOST_CHECK_THROW(pwalletMain->SpendSigma(recipients, wtx), WalletError); //this must throw as 6 blocks have not passed yet,
            }
            CBlock b = CreateAndProcessBlock(scriptPubKey);
        }

        BOOST_CHECK_MESSAGE(previousHeight + 5 == chainActive.Height(), "Block not added to chain");

        {
            CWalletTx wtx;
            BOOST_CHECK_NO_THROW(pwalletMain->SpendSigma(recipients, wtx));
        }

        // Verify spend got into mempool
        BOOST_CHECK_MESSAGE(mempool.size() == 1, "Spend was not added to mempool");

        b = CreateBlock(scriptPubKey);
        previousHeight = chainActive.Height();
        mempool.clear();
        BOOST_CHECK_MESSAGE(mempool.size() == 0, "Mempool not cleared");

        // Delete usedCoinSerials since we deleted the mempool
        sigma::CSigmaState *sigmaState = sigma::CSigmaState::GetState();
        sigmaState->containers.usedCoinSerials.clear();
        sigmaState->mempoolCoinSerials.clear();
        {
            CWalletTx wtx;
            BOOST_CHECK_NO_THROW(pwalletMain->SpendSigma(recipients, wtx));
        }

        {
            CWalletTx wtx;
            BOOST_CHECK_THROW(pwalletMain->SpendSigma(recipients, wtx), WalletError);
        }

        BOOST_CHECK_MESSAGE(ProcessBlock(b), "ProcessBlock failed although valid spend inside");
        BOOST_CHECK_MESSAGE(previousHeight + 1 == chainActive.Height(), "Block not added to chain");

        //Confirm that on disconnect block transaction is returned to mempool
        DisconnectBlocks(1);

        LOCK(cs_main);
        {
            CValidationState state;
            const CChainParams& chainparams = Params();
            InvalidateBlock(state, chainparams, mapBlockIndex[b.GetHash()]);
        }

        //This mint is just to create a block with the new hash
        {
            std::vector<sigma::PrivateCoin> privCoins(1, sigma::PrivateCoin(sigmaParams, denom));

            CWalletTx wtx;
            vector<CHDMint> vDMints;
            auto vecSend = CWallet::CreateSigmaMintRecipients(privCoins, vDMints);
            stringError = pwalletMain->MintAndStoreSigma(vecSend, privCoins, vDMints, wtx);

            BOOST_CHECK_MESSAGE(stringError == "", "Mint Failed");
        }
        b = CreateAndProcessBlock(scriptPubKey);
        mempool.clear();

    }
    sigmaState->Reset();
}

BOOST_AUTO_TEST_SUITE_END()
