#include "util.h"

#include <stdint.h>
#include <vector>

#include "chainparams.h"
#include "key.h"
#include "validation.h"
#include "pubkey.h"
#include "txdb.h"
#include "txmempool.h"
#include "sigma.h"

#include "test/fixtures.h"
#include "test/testutil.h"

#include "wallet/db.h"
#include "wallet/wallet.h"
#include "wallet/walletexcept.h"

#include <boost/filesystem.hpp>
#include <boost/test/unit_test.hpp>
#include <boost/thread.hpp>

BOOST_FIXTURE_TEST_SUITE(sigma_mintspend, ZerocoinTestingSetup200)

/*
* 1. Create one denomination pair and check it can't be spend till 6 conf of mint
* 2. Make one more mint of denom pair and check it can't be spend till 6 conf
* 3. Create two spend transactions using same mint
* 4. Double spend with previous spend in last block
*/
BOOST_AUTO_TEST_CASE(sigma_mintspend_test)
{
    sigma::CSigmaState *sigmaState = sigma::CSigmaState::GetState();
    string denomination;
    std::vector<string> denominations = {"0.05", "0.1", "0.5", "1", "10", "25", "100"};

    // Create 400-200+1 = 201 new empty blocks. // consensus.nMintV3SigmaStartBlock = 400
    CreateAndProcessEmptyBlocks(201, scriptPubKey);

    // foreach denom from denominations
    for(auto denomination : denominations)
    {
        string stringError;
        // Make sure that transactions get to mempool
        pwalletMain->SetBroadcastTransactions(true);

        sigma::CoinDenomination denom;
        BOOST_CHECK_MESSAGE(StringToDenomination(denomination, denom), "Unable to convert denomination string to value.");

        // Verify Mint is successful
        std::vector<sigma::PrivateCoin> privCoins;
        const auto& sigmaParams = sigma::Params::get_default();
        privCoins.push_back(sigma::PrivateCoin(sigmaParams, denom));
        vector<CHDMint> vDMints;
        auto vecSend = CWallet::CreateSigmaMintRecipients(privCoins, vDMints);
        {
            CWalletTx wtx;
            stringError = pwalletMain->MintAndStoreSigma(vecSend, privCoins, vDMints, wtx);
        }
        BOOST_CHECK_MESSAGE(stringError == "", "Create Mint Failed");

        // Verify Mint gets in the mempool
        BOOST_CHECK_MESSAGE(mempool.size() == 1, "Mint was not added to mempool");

        int previousHeight = chainActive.Height();
        CBlock b = CreateAndProcessBlock(scriptPubKey);
        BOOST_CHECK_MESSAGE(previousHeight + 1 == chainActive.Height(), "Block not added to chain");

        previousHeight = chainActive.Height();

        // Generate address
        CPubKey newKey;
        BOOST_CHECK_MESSAGE(pwalletMain->GetKeyFromPool(newKey), "Fail to get new address");

        const CBitcoinAddress randomAddr(newKey.GetID());

        CAmount nValue;
        DenominationToInteger(denom, nValue);

        std::vector<CRecipient> recipients = {
                {GetScriptForDestination(randomAddr.Get()), nValue, true},
        };

        // Add 5 more blocks and verify that Mint can not be spent until 6 blocks verification
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

        privCoins.clear();
        privCoins.push_back(sigma::PrivateCoin(sigmaParams, denom));
        vDMints.clear();
        vecSend = CWallet::CreateSigmaMintRecipients(privCoins, vDMints);
        {
            CWalletTx wtx;
            stringError = pwalletMain->MintAndStoreSigma(vecSend, privCoins, vDMints, wtx);
        }
        BOOST_CHECK_MESSAGE(stringError == "", "Create Mint Failed");

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

        //Verify spend got into mempool
        BOOST_CHECK_MESSAGE(mempool.size() == 1, "Spend was not added to mempool");

        b = CreateBlock(scriptPubKey);
        previousHeight = chainActive.Height();
        BOOST_CHECK_MESSAGE(ProcessBlock(b), "ProcessBlock failed although valid spend inside");
        BOOST_CHECK_MESSAGE(previousHeight + 1 == chainActive.Height(), "Block not added to chain");

        BOOST_CHECK_MESSAGE(mempool.size() == 0, "Mempool not cleared");

        {
            CWalletTx wtx;
            BOOST_CHECK_NO_THROW(pwalletMain->SpendSigma(recipients, wtx));
        }

        //Verify spend got into mempool
        BOOST_CHECK_MESSAGE(mempool.size() == 1, "Spend was not added to mempool");

        b = CreateBlock(scriptPubKey);
        previousHeight = chainActive.Height();
        BOOST_CHECK_MESSAGE(ProcessBlock(b), "ProcessBlock failed although valid spend inside");
        BOOST_CHECK_MESSAGE(previousHeight + 1 == chainActive.Height(), "Block not added to chain");

        BOOST_CHECK_MESSAGE(mempool.size() == 0, "Mempool not cleared");

        //Temporary disable usedCoinSerials check to force double spend in mempool
        auto tempSerials = sigmaState->containers.usedCoinSerials;
        sigmaState->containers.usedCoinSerials.clear();

        {
            CWalletTx wtx;
            BOOST_CHECK_THROW(pwalletMain->SpendSigma(recipients, wtx), WalletError);
        }

        sigmaState->containers.usedCoinSerials = tempSerials;

        mempool.clear();
        previousHeight = chainActive.Height();
        BOOST_CHECK_MESSAGE(ProcessBlock(b), "ProcessBlock failed");
        //This test confirms that a block containing a double spend is rejected and not added in the chain
        BOOST_CHECK_MESSAGE(previousHeight == chainActive.Height(), "Double spend - Block added to chain even though same spend in previous block");

        mempool.clear();
        sigmaState->Reset();
    }
}
BOOST_AUTO_TEST_SUITE_END()
