// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/merkle.h>

#include <tinyformat.h>
#include <util.h>
#include <utilstrencodings.h>

#include <assert.h>

#include <chainparamsseeds.h>

static CBlock CreateGenesisBlock(const char* pszTimestamp, const CScript& genesisOutputScript, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    CMutableTransaction txNew;
    txNew.nVersion = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << 486604799 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.nTime    = nTime;
    genesis.nBits    = nBits;
    genesis.nNonce   = nNonce;
    genesis.nVersion = nVersion;
    genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
    genesis.hashPrevBlock.SetNull();
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

/**
 * Build the genesis block. Note that the output of its generation
 * transaction cannot be spent since it did not originally exist in the
 * database.
 *
 * CBlock(hash=000000000019d6, ver=1, hashPrevBlock=00000000000000, hashMerkleRoot=4a5e1e, nTime=1231006505, nBits=1d00ffff, nNonce=2083236893, vtx=1)
 *   CTransaction(hash=4a5e1e, ver=1, vin.size=1, vout.size=1, nLockTime=0)
 *     CTxIn(COutPoint(000000, -1), coinbase 04ffff001d0104455468652054696d65732030332f4a616e2f32303039204368616e63656c6c6f72206f6e206272696e6b206f66207365636f6e64206261696c6f757420666f722062616e6b73)
 *     CTxOut(nValue=50.00000000, scriptPubKey=0x5F1DF16B2B704C8A578D0B)
 *   vMerkleTree: 4a5e1e
 */
static CBlock CreateGenesisBlock(uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    const char* pszTimestamp = "3:56AM - MEN WALK ON MOON. ASTRONAUTS LAND ON PLAIN; COLLECT ROCKS, PLANT FLAG - July 21st, 1969";
    const CScript genesisOutputScript = CScript() << ParseHex("047aca981aef583b72a14d79afd688c344860db3502da3cd75d3ff6d5481f47617252e38854a57d83d41a9b644e51b92f80c11bd29f4daca185ba89ae58b6da51a") << OP_CHECKSIG;
    return CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTime, nNonce, nBits, nVersion, genesisReward);
}

void CChainParams::UpdateVersionBitsParameters(Consensus::DeploymentPos d, int64_t nStartTime, int64_t nTimeout)
{
    consensus.vDeployments[d].nStartTime = nStartTime;
    consensus.vDeployments[d].nTimeout = nTimeout;
}

/**
 * Main network
 */
/**
 * What makes a good checkpoint block?
 * + Is surrounded by blocks with reasonable timestamps
 *   (no blocks before with a timestamp after, none after with
 *    timestamp before)
 * + Contains no strange transactions
 */

class CMainParams : public CChainParams {
public:
    CMainParams() {
        strNetworkID = "main";
        consensus.nSubsidyHalvingInterval = 840000;
        consensus.BIP16Height = 0; // 
        consensus.BIP34Height = 710000;
        consensus.BIP34Hash = uint256S("0x638aa7bcca0f85e05cbf639b37b0b979e9878a67b79408937f900e11865ef88e");
        consensus.BIP65Height = 1793913; // 1188350 mooncoin-0.13.9 height set to 1793913 as it was never enforced before
        consensus.BIP66Height = 1793913; // 1184549 mooncoin-0.13.9 height set to 1793913 as it was never enforced before
		consensus.DiffMode = 1;    // initial block diffmode
        consensus.PowV2 = 26850;   //after this block
        consensus.PowV3 = 1100000; //after this block
        consensus.PowV4 = 1250000; //after this block
		consensus.PowV5 = 8250000; //set to future plans for digishield
        consensus.powLimit = uint256S("0x00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"); 
        consensus.nPowTargetTimespan = 8 * 60 * 60; // 3.5 days
        consensus.nPowTargetSpacing = 1.5 * 60;
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.fPowNoRetargeting = false;
        consensus.nRuleChangeActivationThreshold = 5040; // 75% of 6720
        consensus.nMinerConfirmationWindow = 6720; // 40 * 24 * 7 = 6,720 blocks, or one week
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 1199145601; // January 1, 2008
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = 1230767999; // December 31, 2008

        // Deployment of BIP68, BIP112, and BIP113.
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = 1517097600; // January 28, 2018
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = 1553990400; // March 31st, 2019

        // Deployment of SegWit (BIP141, BIP143, and BIP147)
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].bit = 1;
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nStartTime = 1517097600; // January 28, 2018
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nTimeout = 1553990400; // March 31st, 2019

        // The best chain should have at least this much work.
        consensus.nMinimumChainWork = uint256S("0x0000000000000000000000000000000000000000000000001b4e64c4d144645e"); 

        // By default assume that the signatures in ancestors of this block are valid.
        consensus.defaultAssumeValid = uint256S("0xb87d10af80ab9e966fcb1c0c95cac25c3c74a22e7f9fd04606e992741dfaa38b"); //1712500

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 32-bit integer with any alignment.
         */
        pchMessageStart[0] = 0xf9;
        pchMessageStart[1] = 0xf7;
        pchMessageStart[2] = 0xc0;
        pchMessageStart[3] = 0xe8;
        nDefaultPort = 44664;
        nPruneAfterHeight = 100000;

        genesis = CreateGenesisBlock(1388158603, 1767251, 0x1e0ffff0, 1, 88 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0xbd687cbbac4842724d4b1dfd2e1a6ce35d765db05635b3ebd3810ba66ac5aee9"));
        assert(genesis.hashMerkleRoot == uint256S("0xb3aaaed7565d0594128fffa5f5dee01df8eb24de4245365cb8df7ad0c6e93266"));

        // Note that of those which support the service bits prefix, most only support a subset of
        // possible options.
        // This is fine at runtime as we'll fall back to using them as a oneshot if they don't support the
        // service bits we want, but we should get them updated to support all service bits wanted by any
        // release ASAP to avoid it where possible.
        vSeeds.emplace_back("dnsseed01.moonypool.com");
        vSeeds.emplace_back("dnsseed02.moonypool.com");
        vSeeds.emplace_back("dnsseed03.moonypool.com");


        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,3);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,22);
        base58Prefixes[SCRIPT_ADDRESS2] = std::vector<unsigned char>(1,50);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,131);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x88, 0xB2, 0x1E};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x88, 0xAD, 0xE4};

        bech32_hrp = "moon";

        vFixedSeeds = std::vector<SeedSpec6>(pnSeed6_main, pnSeed6_main + ARRAYLEN(pnSeed6_main));

        fDefaultConsistencyChecks = false;
        fRequireStandard = true;
        fMineBlocksOnDemand = false;

        checkpointData = {
            {
				{      0, uint256S("0xbd687cbbac4842724d4b1dfd2e1a6ce35d765db05635b3ebd3810ba66ac5aee9")},
                {  26700, uint256S("0x513f2d61fc89206109d4547b100e0e4ac0a12339957c1ccd8b927a0ff641cdfe")},
                {  66600, uint256S("0x881a7edf40cb76722a89abe3a9b32b5af59928b48d178c1e52dd888a69d289c9")},
                {  66618, uint256S("0x1bf88ab406ff6ea9f43d8fff61ada69189443de12d143f2ebb7ddb019b54dee2")},
                { 1000000, uint256S("0xd806ec36632672626e7a8e764948ea09ff69cf0360edc427008accf68b8fcf6d")},
                { 1100000, uint256S("0x6988ca75c6bcba6253f4f775788785e71e1efae72c05d678386c3053b2c15863")},
                { 1110000, uint256S("0x084bb5c11d303ad2e061c81921fbde3f07197898e63a59836399218bf97eabc1")},
				{ 1112500, uint256S("0xe78124b931c6628967b756a1200bf9b5917693536eaac520f876dfe2b35e6637")},
				{ 1112513, uint256S("0x5f4fdf9124764fa74652d0e2cbdc170b407c5c96e93d9b78626eaea98c496601")},
				{ 1212505, uint256S("0xe45b3ba0d0f98922fa848d277917833071629523a10ce72ef5db7884f39c78c3")},
				{ 1312500, uint256S("0xef1b810abf761cc054d89a1d1191b73d29d898abb62f72a57c3caf6cbf9547d8")},
				{ 1412503, uint256S("0x7965ed48962d2959ce054970c4ea64094ebc9e0af42dca14a77157e33f04179c")},
				{ 1512502, uint256S("0x93bcbe8ff0f938628b5fdd29b46c6d7d859d440bba175e8118a4377be5e67897")},
				{ 1612501, uint256S("0x6e3713dea2fa389ded6e9140068874d49131440147a6385eb4ffcc00595a52af")},
				{ 1712500, uint256S("0xb87d10af80ab9e966fcb1c0c95cac25c3c74a22e7f9fd04606e992741dfaa38b")},
            }
        };

        chainTxData = ChainTxData{
            // Data from rpc: getchaintxstats 4096 a601455787cb65ffc325dda4751a99cf01d1567799ec4b04f45bb05f9ef0cbde
            /* nTime    */ 1573748022,
            /* nTxCount */ 2816499,
            /* dTxRate  */ 0.3125
        };

        /* disable fallback fee on mainnet */
        m_fallback_fee_enabled = false;
    }
};

/**
 * Testnet (v3)
 */
class CTestNetParams : public CChainParams {
public:
    CTestNetParams() {
        strNetworkID = "test";
        consensus.nSubsidyHalvingInterval = 840000;
        consensus.BIP16Height = 0; // always enforce P2SH BIP16 on testnet
        consensus.BIP34Height = 400000;
        consensus.BIP34Hash = uint256S("0xf12cbe7f98f64f7a00e3ed2c833e6c03bbd33faaeb432437e96320087b44c6f9");
        consensus.BIP65Height = 1023735; // 1022985 + 750 = 1023735
        consensus.BIP66Height = 1023735; // 1022985 + 750 = 1023735 :: "hash": "f94990b4e1089c7f98f9161471ce4012f34f31a1dddc8a7c68ae5189f847bac9"
	    consensus.DiffMode = 1;    // initial block diffmode
        consensus.PowV2 = 50;   //after this block
        consensus.PowV3 = 1100000; //after this block
        consensus.PowV4 = 1250000; //after this block
		consensus.PowV5 = 8588750; //set to future
        consensus.powLimit = uint256S("0x00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nPowTargetTimespan = 8 * 60 * 60; // 8 hours
        consensus.nPowTargetSpacing = 1.5 * 60;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fPowNoRetargeting = false;
        consensus.nRuleChangeActivationThreshold = 6048; // 75% for testchains
        consensus.nMinerConfirmationWindow = 8064; // nPowTargetTimespan / nPowTargetSpacing
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 1199145601; // January 1, 2008
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = 1230767999; // December 31, 2008

        // Deployment of BIP68, BIP112, and BIP113.
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = 1542604429; // Sun Nov 18 21:13:49 PST 2018
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = 1543553929; // Thu Nov 29 20:58:49 PST 2018

        // Deployment of SegWit (BIP141, BIP143, and BIP147)
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].bit = 1;
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nStartTime = 1542604429; // Sun Nov 18 21:13:49 PST 2018
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nTimeout = 1543553929; // Thu Nov 29 20:58:49 PST 2018

        // The best chain should have at least this much work.
        consensus.nMinimumChainWork = uint256S("0x0000000000000000000000000000000000000000000000000001967ee66a8b28"); 

        // By default assume that the signatures in ancestors of this block are valid.
        consensus.defaultAssumeValid = uint256S("0x4f870d3dd78b80aebcddd4b35551f28105cd51ccea1d0cec10290b24f5f60907"); // 1524769

        pchMessageStart[0] = 0xf3;
        pchMessageStart[1] = 0xd2;
        pchMessageStart[2] = 0xc8;
        pchMessageStart[3] = 0xf1;
        nDefaultPort = 14664;
        nPruneAfterHeight = 1000;

        genesis = CreateGenesisBlock(1540452872, 1543106876, 0x1e0ffff0, 1, 88 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
		
        assert(consensus.hashGenesisBlock == uint256S("0xae5047b8182d7e9c7f6cdefdb12bc08a95ca7884126e56b15b5082a5ce18e4d7"));
        assert(genesis.hashMerkleRoot == uint256S("0xb3aaaed7565d0594128fffa5f5dee01df8eb24de4245365cb8df7ad0c6e93266"));

        vFixedSeeds.clear();
        vSeeds.clear();
        // nodes with support for servicebits filtering should be at the top
        vSeeds.emplace_back("testnet-seed01.moonypool.com");
		vSeeds.emplace_back("testnet-seed02.moonypool.com");


        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,113);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SCRIPT_ADDRESS2] = std::vector<unsigned char>(1,50);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,241);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "moont";

        vFixedSeeds = std::vector<SeedSpec6>(pnSeed6_test, pnSeed6_test + ARRAYLEN(pnSeed6_test));

        fDefaultConsistencyChecks = false;
        fRequireStandard = false;
        fMineBlocksOnDemand = false;

        checkpointData = (CCheckpointData) {
            {
                {0, uint256S("0xae5047b8182d7e9c7f6cdefdb12bc08a95ca7884126e56b15b5082a5ce18e4d7")},
				{10983, uint256S("0x69bc203faa0a93efa3e11c9d8b260d48ff49eb54eb877196f433642190008994")},
				{133658, uint256S("0xca85d40cfaa1e19ea2f2a6982a89538022c6eb58737f293fdbb6ff522b8c31b1")},
				{186531, uint256S("0xcd345adfaca4cf2fd067888d5392bb961697e40e7786b8e9bf9f7723320a6354")},
				{253366, uint256S("0xc057d1926b822b865a29a77c1c2c71dc5c9b5b35ac42d30005ed05fb8ddd2b30")},
				{332184, uint256S("0x2852b3d257277ee3a7045de59e69a4940a0b07aa17507088c83aa34526ede70a")},
				{384254, uint256S("0x561a12ecbca0592ca572089cbcd87d974a1d396d93b69fcdaf74e97d241581aa")},
				{384418, uint256S("0x370d144c6e265183fcab51f8b960a0ad9d87cee6b16f3bbd2c201f16edfba9ad")},
				{594398, uint256S("0x2629ced17214cbbee8bcdf6e764ff5d591382af7d4724e715ad073c97b648706")},
				{650504, uint256S("0xc28ae82187062144ee8ff547269f4349459ea43e5c9acfa4e4f1d710a725da00")},
				{700501, uint256S("0x0dee1b499737d8a4bbff808fd8c4b8db4884029fbec7cc5608cffbe4d14b28ca")},
				{800504, uint256S("0x6afc0c1a69f1163eb31317c0e804bb1b74ffe07a1cfe19e85c204b6a0943ee3c")},
				{900504, uint256S("0x7fd5a418500928cf3efc777d830b1c332375f1916f37d4324f0db0faedd5d3b5")},
				{1000502, uint256S("0x336b7db51e049057a8d93d5bec666fad502026e5f2e8c4e67265a3e8c6f6fa8b")},
				{1100547, uint256S("0xad475bf4449d7b2294d37374bdcec5f70b972ae188abc1ba480518c92a2e01b9")},
				{1200506, uint256S("0x6dd82e370ff21c503cd67606eacf2d318942d9e9c708fde45bd74a0b86ff5d84")},
				{1300501, uint256S("0x2b2c1b41b7068f231306c5983f31d6e5c0b86920028d416f1bf8c52c173869fc")},
				{1400501, uint256S("0x5d86152c570a575b53bf552d573dcfa66ff721dba37ba06b8dc1e08e762d0089")},
				{1500502, uint256S("0x5ac72cb8cfbb1b13e862a7368ce599fc502768c79278b758f17d9825c134c9d3")},
				{1524769, uint256S("0x4f870d3dd78b80aebcddd4b35551f28105cd51ccea1d0cec10290b24f5f60907")},
            }
        };

        chainTxData = ChainTxData{
            // Data from rpc: getchaintxstats 4096 438b7e1c86f58b4e62e8cf2d0f9f256e3dddaebc5d1cc568e633a38e0db6c025
            /* nTime    */ 1570367303,
            /* nTxCount */ 1527970,
            /* dTxRate  */ 1.907
        };

        /* enable fallback fee on testnet */
        m_fallback_fee_enabled = false;
    }
};

/**
 * Regression test
 */
class CRegTestParams : public CChainParams {
public:
    CRegTestParams() {
        strNetworkID = "regtest";
        consensus.nSubsidyHalvingInterval = 150;
        consensus.BIP16Height = 0; // always enforce P2SH BIP16 on regtest
        consensus.BIP34Height = 100000000; // BIP34 has not activated on regtest (far in the future so block v1 are not rejected in tests)
        consensus.BIP34Hash = uint256();
        consensus.BIP65Height = 1351; // BIP65 activated on regtest (Used in rpc activation tests)
        consensus.BIP66Height = 1251; // BIP66 activated on regtest (Used in rpc activation tests)
	    consensus.DiffMode = 1;    // initial block diffmode
        consensus.PowV2 = 20;   //after this block
        consensus.PowV3 = 320; //after this block
        consensus.PowV4 = 640; //after this block
		consensus.PowV5 = 960; //set to future
        consensus.powLimit = uint256S("7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nPowTargetTimespan = 8 * 60 * 60; // pre-digishield: 8 hours
        consensus.nPowTargetSpacing = 1;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fPowNoRetargeting = true;
        consensus.nRuleChangeActivationThreshold = 540; // 75% for testchains
        consensus.nMinerConfirmationWindow = 720; // Faster than normal for regtest (2,520 instead of 10,080)
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].bit = 1;
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nStartTime = Consensus::BIP9Deployment::ALWAYS_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;

        // The best chain should have at least this much work.
        consensus.nMinimumChainWork = uint256S("0x00");

        // By default assume that the signatures in ancestors of this block are valid.
        consensus.defaultAssumeValid = uint256S("0x00");

        pchMessageStart[0] = 0xf5;
        pchMessageStart[1] = 0xd6;
        pchMessageStart[2] = 0xc4;
        pchMessageStart[3] = 0xf7;
        nDefaultPort = 13664;
        nPruneAfterHeight = 1000;

        genesis = CreateGenesisBlock(1548669445, 0, 0x207fffff, 1, 88 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x58e19a3e3dc18af86de5fe24fa508479975986bf703ad09aa138d4a53af33147"));
        assert(genesis.hashMerkleRoot == uint256S("0xb3aaaed7565d0594128fffa5f5dee01df8eb24de4245365cb8df7ad0c6e93266"));

        vFixedSeeds.clear(); //!< Regtest mode doesn't have any fixed seeds.
        vSeeds.clear();      //!< Regtest mode doesn't have any DNS seeds.

        fDefaultConsistencyChecks = true;
        fRequireStandard = false;
        fMineBlocksOnDemand = true; 

        checkpointData = {
            {
                {0, uint256S("0x58e19a3e3dc18af86de5fe24fa508479975986bf703ad09aa138d4a53af33147")},
            }
        };

        chainTxData = ChainTxData{
            0,
            0,
            0
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,112);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,195);
        base58Prefixes[SCRIPT_ADDRESS2] = std::vector<unsigned char>(1,57);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,238);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "moonr";

        /* enable fallback fee on regtest */
        m_fallback_fee_enabled = true;
    }
};

static std::unique_ptr<CChainParams> globalChainParams;

const CChainParams &Params() {
    assert(globalChainParams);
    return *globalChainParams;
}

std::unique_ptr<CChainParams> CreateChainParams(const std::string& chain)
{
    if (chain == CBaseChainParams::MAIN)
        return std::unique_ptr<CChainParams>(new CMainParams());
    else if (chain == CBaseChainParams::TESTNET)
        return std::unique_ptr<CChainParams>(new CTestNetParams());
    else if (chain == CBaseChainParams::REGTEST)
        return std::unique_ptr<CChainParams>(new CRegTestParams());
    throw std::runtime_error(strprintf("%s: Unknown chain %s.", __func__, chain));
}

void SelectParams(const std::string& network)
{
    SelectBaseParams(network);
    globalChainParams = CreateChainParams(network);
}

void UpdateVersionBitsParameters(Consensus::DeploymentPos d, int64_t nStartTime, int64_t nTimeout)
{
    globalChainParams->UpdateVersionBitsParameters(d, nStartTime, nTimeout);
}

