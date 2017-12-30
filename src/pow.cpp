// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pow.h"

#include "arith_uint256.h"
#include "chain.h"
#include "chainparams.h"
#include "primitives/block.h"
#include "bignum.h"
#include "uint256.h"
#include "util.h"

typedef int64_t int64;
typedef uint64_t uint64;

unsigned int GetNextWorkRequired_V1(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{

    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    // Genesis block
    if (pindexLast == NULL)
        return nProofOfWorkLimit;

    // Only change once per difficulty adjustment interval
    if ((pindexLast->nHeight+1) % params.DifficultyAdjustmentInterval() != 0)
    {
        if (params.fPowAllowMinDifficultyBlocks)
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 2* 10 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // Go back by what we want to be 14 days worth of blocks
    // Mooncoin: This fixes an issue where a 51% attack can change difficulty at will.
    // Go back the full period unless it's the first retarget after genesis. Code courtesy of Art Forz
    int blockstogoback = params.DifficultyAdjustmentInterval()-1;
    if ((pindexLast->nHeight+1) != params.DifficultyAdjustmentInterval())
        blockstogoback = params.DifficultyAdjustmentInterval();

    // Go back by what we want to be 14 days worth of blocks
    const CBlockIndex* pindexFirst = pindexLast;
    for (int i = 0; pindexFirst && i < blockstogoback; i++)
        pindexFirst = pindexFirst->pprev;
    assert(pindexFirst);

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - pindexFirst->GetBlockTime();
    LogPrintf("  nActualTimespan = %d  before bounds\n", nActualTimespan);

       if(pindexLast->nHeight+1 > 10000)
        {
                if (nActualTimespan < params.nPowTargetSpacing/4)
                        nActualTimespan = params.nPowTargetSpacing/4;
                if (nActualTimespan > params.nPowTargetSpacing*4)
                        nActualTimespan = params.nPowTargetSpacing*4;
        }
        else if(pindexLast->nHeight+1 > 5000)
        {
                if (nActualTimespan < params.nPowTargetSpacing/8)
                        nActualTimespan = params.nPowTargetSpacing/8;
                if (nActualTimespan > params.nPowTargetSpacing*4)
                        nActualTimespan = params.nPowTargetSpacing*4;
        }
        else
        {
                if (nActualTimespan < params.nPowTargetSpacing/16)
                        nActualTimespan = params.nPowTargetSpacing/16;
                if (nActualTimespan > params.nPowTargetSpacing*4)
                        nActualTimespan = params.nPowTargetSpacing*4;
        }

    // Retarget
    CBigNum bnNew;
    CBigNum bnOld;
    bnNew.SetCompact(pindexLast->nBits);
    bnOld = bnNew;
    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetSpacing;

    if (bnNew > nProofOfWorkLimit)
        bnNew = nProofOfWorkLimit;

    /// debug print
	if(fDebug){
		LogPrintf("GetNextWorkRequired RETARGET\n");
		LogPrintf("Params().TargetTimespan() = %d    nActualTimespan = %d\n", params.nPowTargetSpacing, nActualTimespan);
		LogPrintf("Before: %08x  %s\n", pindexLast->nBits, bnOld.ToString());
		LogPrintf("After:  %08x  %s\n", bnNew.GetCompact(), bnNew.ToString());
	}
	
    return bnNew.GetCompact();
}

unsigned int KimotoGravityWell(const CBlockIndex* pindexLast, const CBlockHeader *pblock, uint64_t TargetBlocksSpacingSeconds, uint64_t PastBlocksMin, uint64_t PastBlocksMax, const Consensus::Params& params) {

    CBigNum bnProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    /* current difficulty formula, megacoin - kimoto gravity well */
    const CBlockIndex  *BlockLastSolved                             = pindexLast;
    const CBlockIndex  *BlockReading                                = pindexLast;
    const CBlockHeader *BlockCreating                               = pblock;
                        BlockCreating                               = BlockCreating;
    uint64                                PastBlocksMass                       = 0;
    int64                                 PastRateActualSeconds                = 0;
    int64                                 PastRateTargetSeconds                = 0;
    double                                PastRateAdjustmentRatio              = double(1);
    CBigNum                               PastDifficultyAverage;
    CBigNum                               PastDifficultyAveragePrev;
    double                                EventHorizonDeviation;
    double                                EventHorizonDeviationFast;
    double                                EventHorizonDeviationSlow;

    if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0 || (uint64)BlockLastSolved->nHeight < PastBlocksMin) { return bnProofOfWorkLimit.GetCompact(); }

  	for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
		if (PastBlocksMax > 0 && i > PastBlocksMax) { break; }
		PastBlocksMass++;
		
		if (i == 1)	{ PastDifficultyAverage.SetCompact(BlockReading->nBits); }
		else		{ PastDifficultyAverage = ((CBigNum().SetCompact(BlockReading->nBits) - PastDifficultyAveragePrev) / i) + PastDifficultyAveragePrev; }
		PastDifficultyAveragePrev = PastDifficultyAverage;
		
		PastRateActualSeconds			= BlockLastSolved->GetBlockTime() - BlockReading->GetBlockTime();
		PastRateTargetSeconds			= TargetBlocksSpacingSeconds * PastBlocksMass;
		PastRateAdjustmentRatio			= double(1);
		if (PastRateActualSeconds < 0) { PastRateActualSeconds = 0; }
		if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
		PastRateAdjustmentRatio			= double(PastRateTargetSeconds) / double(PastRateActualSeconds);
		}
		EventHorizonDeviation			= 1 + (0.7084 * pow((double(PastBlocksMass)/double(144)), -1.228));
		EventHorizonDeviationFast		= EventHorizonDeviation;
		EventHorizonDeviationSlow		= 1 / EventHorizonDeviation;
		
		if (PastBlocksMass >= PastBlocksMin) {
			if ((PastRateAdjustmentRatio <= EventHorizonDeviationSlow) || (PastRateAdjustmentRatio >= EventHorizonDeviationFast)) { assert(BlockReading); break; }
		}
		if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
		BlockReading = BlockReading->pprev;
	}
	
	CBigNum bnNew(PastDifficultyAverage);
	if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
		bnNew *= PastRateActualSeconds;
		bnNew /= PastRateTargetSeconds;
	}
    if (bnNew > bnProofOfWorkLimit) { bnNew = bnProofOfWorkLimit; }
 
    if(fDebug){
		/// debug print
		LogPrintf("Difficulty Retarget - KGW Wormhole\n");
		LogPrintf("PastRateAdjustmentRatio = %g\n", PastRateAdjustmentRatio);
		LogPrintf("Before: %08x  %s\n", BlockLastSolved->nBits, CBigNum().SetCompact(BlockLastSolved->nBits).getuint256().ToString().c_str());
		LogPrintf("After:  %08x  %s\n", bnNew.GetCompact(), bnNew.getuint256().ToString().c_str());
    }
	
    return bnNew.GetCompact();
}

unsigned int GetNextWorkRequired_V2(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    static const int64_t        BlocksTargetSpacing                        = 90;
    unsigned int                TimeDaySeconds                             = 60 * 60 * 24;
    int64_t                     PastSecondsMin                             = TimeDaySeconds * 0.25;
    int64_t                     PastSecondsMax                             = TimeDaySeconds * 7;
    uint64_t                    PastBlocksMin                              = PastSecondsMin / BlocksTargetSpacing;
    uint64_t                    PastBlocksMax                              = PastSecondsMax / BlocksTargetSpacing;

    return KimotoGravityWell(pindexLast, pblock, BlocksTargetSpacing, PastBlocksMin, PastBlocksMax, params);
}

unsigned int static DigiShield(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    const arith_uint256 bnProofOfWorkLimit = UintToArith256(params.powLimit);
    const unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();
    // DigiShield difficulty retarget system
    bool fTestNet = false;
    int blockstogoback = 0;
    int64 nTargetSpacing = 1 * 90;             // target 90 sec
    int64 retargetTimespan = nTargetSpacing;
    int64 retargetSpacing = nTargetSpacing;
    int64 retargetInterval = retargetTimespan / retargetSpacing;
	
    // Genesis block
    if (pindexLast == NULL) return nProofOfWorkLimit;

    // Only change once per interval
    if ((pindexLast->nHeight+1) % retargetInterval != 0){
      // Special difficulty rule for testnet:
        if (fTestNet){
            // If the new block's timestamp is more than 2* 10 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->nTime > pindexLast->nTime + retargetSpacing*2)
                return nProofOfWorkLimit;
        else {
            // Return the last non-special-min-difficulty-rules-block
            const CBlockIndex* pindex = pindexLast;
            while (pindex->pprev && pindex->nHeight % retargetInterval != 0 && pindex->nBits == nProofOfWorkLimit)
            pindex = pindex->pprev;
        return pindex->nBits;
        }
      }
      return pindexLast->nBits;
    }

    // DigiByte: This fixes an issue where a 51% attack can change difficulty at will.
    // Go back the full period unless it's the first retarget after genesis. Code courtesy of Art Forz
    blockstogoback = retargetInterval-1;
    if ((pindexLast->nHeight+1) != retargetInterval) blockstogoback = retargetInterval;

    // Go back by what we want to be 14 days worth of blocks
    const CBlockIndex* pindexFirst = pindexLast;
    for (int i = 0; pindexFirst && i < blockstogoback; i++)
        pindexFirst = pindexFirst->pprev;
    assert(pindexFirst);

    // Limit adjustment step
    int64 nActualTimespan = pindexLast->GetBlockTime() - pindexFirst->GetBlockTime();
    LogPrintf("  nActualTimespan = %g before bounds\n", nActualTimespan);

    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);

    if (nActualTimespan < (retargetTimespan - (retargetTimespan/4)) ) nActualTimespan = (retargetTimespan - (retargetTimespan/4));
    if (nActualTimespan > (retargetTimespan + (retargetTimespan/2)) ) nActualTimespan = (retargetTimespan + (retargetTimespan/2));

    // Retarget
    bnNew *= nActualTimespan;
    bnNew /= retargetTimespan;

    /// debug print
    LogPrintf("DigiShield RETARGET \n");
    LogPrintf("retargetTimespan = %g    nActualTimespan = %g \n", retargetTimespan, nActualTimespan);
    LogPrintf("Before: %08x  %s\n", pindexLast->nBits, CBigNum().SetCompact(pindexLast->nBits).getuint256().ToString().c_str());
    LogPrintf("After:  %08x  %s\n", bnNew.GetCompact(), bnNew.ToString().c_str());

    if (bnNew > bnProofOfWorkLimit)
        bnNew = bnProofOfWorkLimit;

    return bnNew.GetCompact();
}

unsigned int static DUAL_KGW3(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params) {

    // current difficulty formula, ERC3 - DUAL_KGW3, written by Christian Knoepke - apfelbaum@email.de
    const CBlockIndex *BlockLastSolved = pindexLast;
    const CBlockIndex *BlockReading = pindexLast;
    bool kgwdebug=true;
    int64_t PastBlocksMass = 0;
    int64_t PastRateActualSeconds = 0;
    int64_t PastRateTargetSeconds = 0;
    double PastRateAdjustmentRatio = double(1);
    arith_uint256 PastDifficultyAverage;
    arith_uint256 PastDifficultyAveragePrev;
    double EventHorizonDeviation;
    double EventHorizonDeviationFast;
    double EventHorizonDeviationSlow;
	
    //DUAL_KGW3 SETUP
    static const int64_t Blocktime = 90;
    static const unsigned int timeDaySeconds = 86400;
    int64_t pastSecondsMin = timeDaySeconds * 0.025;
    int64_t pastSecondsMax = timeDaySeconds * 7;
    int64_t PastBlocksMin = pastSecondsMin / Blocktime;
    int64_t PastBlocksMax = pastSecondsMax / Blocktime;
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
	
    if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0 || 
        (int64_t)BlockLastSolved->nHeight < PastBlocksMin) {
        return bnPowLimit.GetCompact(); 
    }

    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if (PastBlocksMax > 0 && i > PastBlocksMax) { break; }
        PastBlocksMass++;
        PastDifficultyAverage.SetCompact(BlockReading->nBits);
        if (i > 1) {
            if(PastDifficultyAverage >= PastDifficultyAveragePrev)
                PastDifficultyAverage = ((PastDifficultyAverage - PastDifficultyAveragePrev) / i) + PastDifficultyAveragePrev;
            else
                PastDifficultyAverage = PastDifficultyAveragePrev - ((PastDifficultyAveragePrev - PastDifficultyAverage) / i);
        }
        PastDifficultyAveragePrev = PastDifficultyAverage;
        PastRateActualSeconds = BlockLastSolved->GetBlockTime() - BlockReading->GetBlockTime();
        PastRateTargetSeconds = Blocktime * PastBlocksMass;
        PastRateAdjustmentRatio = double(1);
        if (PastRateActualSeconds < 0) { PastRateActualSeconds = 0; }
        if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
            PastRateAdjustmentRatio = double(PastRateTargetSeconds) / double(PastRateActualSeconds);
        }
        EventHorizonDeviation = 1 + (0.7084 * pow((double(PastBlocksMass)/double(72)), -1.228));  //28.2 and 144 possible
        EventHorizonDeviationFast = EventHorizonDeviation;
        EventHorizonDeviationSlow = 1 / EventHorizonDeviation;

        if (PastBlocksMass >= PastBlocksMin) {
                if ((PastRateAdjustmentRatio <= EventHorizonDeviationSlow) || (PastRateAdjustmentRatio >= EventHorizonDeviationFast))
                { assert(BlockReading); break; }
        }
        if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
        BlockReading = BlockReading->pprev;
    }
	
    //KGW Original
    arith_uint256 kgw_dual1(PastDifficultyAverage);
    arith_uint256 kgw_dual2;
    kgw_dual2.SetCompact(pindexLast->nBits);
    if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
         kgw_dual1 *= PastRateActualSeconds;
         kgw_dual1 /= PastRateTargetSeconds;
    }
    int64_t nActualTime1 = pindexLast->GetBlockTime() - pindexLast->pprev->GetBlockTime();
    int64_t nActualTimespanshort = nActualTime1;	

    if(nActualTime1 < 0) { nActualTime1 = Blocktime; }

    if (nActualTime1 < Blocktime / 3)
        nActualTime1 = Blocktime / 3;
    if (nActualTime1 > Blocktime * 3)
        nActualTime1 = Blocktime * 3;
    kgw_dual2 *= nActualTime1;
    kgw_dual2 /= Blocktime;
	
    //Fusion from Retarget and Classic KGW3 (BitSend=)	
    arith_uint256 bnNew;
    bnNew = ((kgw_dual2 + kgw_dual1)/2);
	
    // DUAL KGW3 increased rapidly the Diff if Blocktime to last block under Blocktime/6 sec.	
    if(kgwdebug){ LogPrintf("nActualTimespanshort = %d \n", nActualTimespanshort ); }
    if(nActualTimespanshort < Blocktime/6){  
        if(kgwdebug){ LogPrintf("PREDIFF:  %08x %s bnNew first  \n", bnNew.GetCompact(), bnNew.ToString().c_str()); }
	const int nLongShortNew1 = 85;
        const int nLongShortNew2 = 100;
        bnNew = bnNew * nLongShortNew1;	
        bnNew = bnNew / nLongShortNew2;	
	if(kgwdebug){ LogPrintf("POSTDIFF: %08x %s bnNew second \n", bnNew.GetCompact(), bnNew.ToString().c_str() ); }
    }

    //BitBreak BitSend
    const int nLongTimeLimit = 60 * 60; //mooncoin: 60 minutes
    if(kgwdebug){
	LogPrintf("BLOCK %d (max: %d) PREDIFF %08x %s\n", BlockReading->nHeight, nLongTimeLimit, bnNew.GetCompact(), bnNew.ToString().c_str());
    }

    // Reduce difficulty if current block generation time has already exceeded maximum time limit.
    if ((pblock-> nTime - pindexLast->GetBlockTime()) > nLongTimeLimit){
	bnNew = bnPowLimit/15;
       	if(kgwdebug)
           LogPrintf("<BSD> Maximum block time hit - adjusted diff to %08x %s\n", bnNew.GetCompact(), bnNew.ToString().c_str()); 
    }

    // Debug
    if (bnNew > bnPowLimit){
        LogPrintf("* We wanted to set diff to %08x but bnPowLimit is %08x \n", bnNew.GetCompact(), bnPowLimit.GetCompact());
        bnNew = bnPowLimit;
    }

    return bnNew.GetCompact();
}

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
   int DiffMode = 1;
   int AlgoSmoothingPeriod = 8;
   //Determine diff retarget scheme
   if (pindexLast->nHeight+1 < 26850) { DiffMode = 1; }
   if (pindexLast->nHeight+1 >= 26850) { DiffMode = 2; }
   if (pindexLast->nHeight+1 >= 1100000) { DiffMode = 3; }
   if ((pindexLast->nHeight+1 >= AlgoForkHeight) && 
       (pindexLast->nHeight+1 < AlgoForkHeight + AlgoSmoothingPeriod)) { 
       //AlgoSwitch = true;
       DiffMode = 4; 
   }
   if (pindexLast->nHeight+1 >= AlgoForkHeight + AlgoSmoothingPeriod) { 
       DiffMode = 5;
   }
   //actions
   if (DiffMode == 1) { return GetNextWorkRequired_V1(pindexLast, pblock, params); }
   if (DiffMode == 2) { return GetNextWorkRequired_V2(pindexLast, pblock, params); }
   if (DiffMode == 3) { return DigiShield(pindexLast, pblock, params); }
   if (DiffMode == 4) { return 0x1e0fffff; }
   if (DiffMode == 5) { return DUAL_KGW3(pindexLast, pblock, params); }
   return DUAL_KGW3(pindexLast, pblock, params); 
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params, bool beGentle)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);
 
    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit)){
      if (beGentle){
        return false;
      } else {
        return error("CheckProofOfWork(): nBits below minimum work");
      }
    }

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget){
      if (beGentle){
        return false;
      } else {
        return error("CheckProofOfWork(): hash doesn't match nBits (hash %s bnTarget %s)", hash.ToString().c_str(), bnTarget.ToString().c_str());
      }
    }

    return true;
}
