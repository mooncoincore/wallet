// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <primitives/block.h>
#include <uint256.h>
#include <util.h>



typedef int64_t int64;
typedef uint64_t uint64;

unsigned int GetNextWorkRequired_V1(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
	bool fTestNet = gArgs.GetBoolArg("-testnet", false);
    bool fDebug = gArgs.GetBoolArg("-debug", false); 
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();
	int64 nTargetTimespan = 8 * 60 * 60; // Mooncoin: every 8 hours
    int64 nTargetSpacing = 90; // Mooncoin: 1 minutes
    int64 nInterval = nTargetTimespan / nTargetSpacing;

    // Genesis block
    if (pindexLast == NULL)
       return nProofOfWorkLimit;
    
    
    // Only change once per difficulty adjustment interval
    if ((pindexLast->nHeight+1) % nInterval != 0)
    {
        if (params.fPowAllowMinDifficultyBlocks)
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 2* 10 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2)
			{
            return nProofOfWorkLimit;
			}else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % nInterval != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
				
				if (fTestNet){
				   return 504365040;
	               }else{
	               return pindexLast->nBits;
	               }
            }
        }
    if (fTestNet){
       return pindexLast->nBits;
    }else{
       return pindexLast->nBits;
	}
	
    }

    // Go back by what we want to be DifficultyAdjustmentInterval worth of blocks
    // Mooncoin: This fixes an issue where a 51% attack can change difficulty at will.
    // Go back the full period unless it's the first retarget after genesis. Code courtesy of Art Forz
    int blockstogoback = nInterval - 1;
    if ((pindexLast->nHeight+1) != nInterval)
        blockstogoback = nInterval;

    // Go back by what we want to be DifficultyAdjustmentInterval worth of blocks
    const CBlockIndex* pindexFirst = pindexLast;
    for (int i = 0; pindexFirst && i < blockstogoback; i++)
        pindexFirst = pindexFirst->pprev;
    assert(pindexFirst);

    // Limit adjustment step
    int64 nActualTimespan = pindexLast->GetBlockTime() - pindexFirst->GetBlockTime();
    //LogPrintf("  nActualTimespan = %d  before bounds\n", nActualTimespan);

       if(pindexLast->nHeight+1 > 10000)
        {
                if (nActualTimespan < nTargetTimespan/4)
                        nActualTimespan = nTargetTimespan/4;
                if (nActualTimespan > nTargetTimespan*4)
                        nActualTimespan = nTargetTimespan*4;
        }
        else if(pindexLast->nHeight+1 > 5000)
        {
                if (nActualTimespan < nTargetTimespan/8)
                        nActualTimespan = nTargetTimespan/8;
                if (nActualTimespan > nTargetTimespan*4)
                        nActualTimespan = nTargetTimespan*4;
        }
        else
        {
                if (nActualTimespan < nTargetTimespan/16)
                        nActualTimespan = nTargetTimespan/16;
                if (nActualTimespan > nTargetTimespan*4)
                        nActualTimespan = nTargetTimespan*4;
        }

    // Retarget
    arith_uint256 bnNew;
    arith_uint256 bnOld;
    bnNew.SetCompact(pindexLast->nBits);
    bnOld = bnNew;
    bnNew *= nActualTimespan;
    bnNew /= nTargetTimespan;

    if (bnNew > UintToArith256(params.powLimit))
        bnNew = UintToArith256(params.powLimit);

    /// debug print
	if(fDebug){
	LogPrintf("GetNextWorkRequired_V1_RETARGET ()\n");
		if (fTestNet) {
	  // testnet build fast
	    LogPrintf("GetNextWorkRequired_V1_RETARGET::Params().TargetTimespan() = %d    nActualTimespan = %d\n", params.nPowTargetSpacing, nActualTimespan);
		LogPrintf("GetNextWorkRequired_V1_RETARGET::Before: %08x  %s\n", pindexLast->nBits, bnOld.ToString());
		LogPrintf("GetNextWorkRequired_V1_RETARGET::After:  %08x  %s\n", nProofOfWorkLimit, nProofOfWorkLimit);
       }else {
	LogPrintf("GetNextWorkRequired_V1_RETARGET::Params().TargetTimespan() = %d    nActualTimespan = %d\n", params.nPowTargetSpacing, nActualTimespan);
	LogPrintf("GetNextWorkRequired_V1_RETARGET::Before: %08x  %s\n", pindexLast->nBits, bnOld.ToString());
	LogPrintf("GetNextWorkRequired_V1_RETARGET::After:  %08x  %s\n", bnNew.GetCompact(), bnNew.ToString());
		}
	}
	
    if (fTestNet){
       return nProofOfWorkLimit; //testnet build
    }else{
       return bnNew.GetCompact();
	}

}

unsigned int KimotoGravityWell(const CBlockIndex* pindexLast, uint64_t TargetBlocksSpacingSeconds, uint64_t PastBlocksMin, uint64_t PastBlocksMax, const Consensus::Params& params) {
    
	
	bool fTestNet = gArgs.GetBoolArg("-testnet", false);
    bool fDebug = gArgs.GetBoolArg("-debug", false);
	const arith_uint256 bnProofOfWorkLimit = UintToArith256(params.powLimit);
    
    /* current difficulty formula, megacoin - kimoto gravity well */
    const CBlockIndex  *BlockLastSolved                             = pindexLast;
    const CBlockIndex  *BlockReading                                = pindexLast;
    //const CBlockHeader *BlockCreating                               = pblock;
     //                   BlockCreating                               = BlockCreating;
    uint64_t                              PastBlocksMass                       = 0;
	int64_t                               PastRateActualSeconds                = 0;
    int64_t                               PastRateTargetSeconds                = 0;
    double                                PastRateAdjustmentRatio              = double(1);
    arith_uint256                         PastDifficultyAverage;
    arith_uint256                         PastDifficultyAveragePrev;
	arith_uint256                         BlockReadingDifficulty;
    double                                EventHorizonDeviation;
    double                                EventHorizonDeviationFast;
    double                                EventHorizonDeviationSlow;

    if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0 || (uint64)BlockLastSolved->nHeight < PastBlocksMin) { 
	return bnProofOfWorkLimit.GetCompact(); }
    int64 LatestBlockTime = BlockLastSolved->GetBlockTime();
    
  	for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
		if (PastBlocksMax > 0 && i > PastBlocksMax) { break; }
		PastBlocksMass++;
		
		if (i == 1)	{  
		PastDifficultyAverage.SetCompact(BlockReading->nBits);
		}
		else{ 
         BlockReadingDifficulty.SetCompact(BlockReading->nBits);
            if (BlockReadingDifficulty > PastDifficultyAveragePrev) {
                PastDifficultyAverage = PastDifficultyAveragePrev + ((BlockReadingDifficulty - PastDifficultyAveragePrev) / i);
            } else {
                PastDifficultyAverage = PastDifficultyAveragePrev - ((PastDifficultyAveragePrev - BlockReadingDifficulty) / i);
            }

		}
		//LogPrintf("PastDifficultyAveragePrev.GetCompact() = %I PastDifficultyAverage.GetCompact() = %I  \n", PastDifficultyAveragePrev.GetCompact(), PastDifficultyAverage.GetCompact());
		
		PastDifficultyAveragePrev = PastDifficultyAverage;
		
		if (LatestBlockTime < BlockReading->GetBlockTime()) {
                        if (BlockReading->nHeight > 67500) LatestBlockTime = BlockReading->GetBlockTime();
                }
		//LogPrintf("BlockLastSolved->GetBlockTime() = %d  BlockReading->GetBlockTime() = %d \n", BlockLastSolved->GetBlockTime(), BlockReading->GetBlockTime());
		PastRateActualSeconds			= LatestBlockTime - BlockReading->GetBlockTime();
		//LogPrintf("TargetBlocksSpacingSeconds = %d  PastBlocksMass = %d \n", TargetBlocksSpacingSeconds, PastBlocksMass);
		PastRateTargetSeconds			= TargetBlocksSpacingSeconds * PastBlocksMass;
		PastRateAdjustmentRatio			= double(1);
		if (BlockReading->nHeight > 67500) {
                  if (PastRateActualSeconds < 1) { PastRateActualSeconds = 1; }
                } else {
                  if (PastRateActualSeconds < 0) { PastRateActualSeconds = 0; }
                }

		if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
		//LogPrintf("double(PastRateTargetSeconds) = %d  double(PastRateActualSeconds) = %d \n", double(PastRateTargetSeconds), double(PastRateActualSeconds));	
		PastRateAdjustmentRatio			= double(PastRateTargetSeconds) / double(PastRateActualSeconds);
		//LogPrintf("InIf PastRateAdjustmentRatio = %d  \n", PastRateAdjustmentRatio);
		}
		EventHorizonDeviation			= 1 + (0.7084 * pow((double(PastBlocksMass)/double(144)), -1.228));
		//LogPrintf("EventHorizonDeviation = %d \n", EventHorizonDeviation);
		EventHorizonDeviationFast		= EventHorizonDeviation;
		EventHorizonDeviationSlow		= 1 / EventHorizonDeviation;
		//LogPrintf("EventHorizonDeviationSlow = %d \n", EventHorizonDeviationSlow);
		
		if (PastBlocksMass >= PastBlocksMin) {
			if ((PastRateAdjustmentRatio <= EventHorizonDeviationSlow) || (PastRateAdjustmentRatio >= EventHorizonDeviationFast)) { assert(BlockReading); break; }
		}
		if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
		BlockReading = BlockReading->pprev;
	}
	
	
	arith_uint256 bnNew(PastDifficultyAverage);
	//LogPrintf("PastDifficultyAverage.GetCompact() = %d \n", PastDifficultyAverage.GetCompact());
	//LogPrintf("(uint32_t)PastRateActualSeconds = %I \n", (uint32_t)PastRateActualSeconds);
	//LogPrintf("(uint64_t)PastRateActualSeconds = %I  \n", (uint64_t)PastRateActualSeconds);
	//LogPrintf("(uint32_t)PastRateTargetSeconds = %I \n", (uint32_t)PastRateTargetSeconds);
	//LogPrintf("(uint64_t)PastRateTargetSeconds = %I  \n", (uint64_t)PastRateTargetSeconds);
	//LogPrintf("bnNew.GetCompact()d = %d \n", bnNew.GetCompact());
	//LogPrintf("bnNew.GetCompact() = %08x \n", bnNew.GetCompact());
	if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
		bnNew *= PastRateActualSeconds;
		//LogPrintf("bnNew.GetCompact()d (bnNew1 *= PastRateActualSeconds %d) = %d \n", PastRateActualSeconds, bnNew.GetCompact());
	    //LogPrintf("bnNew.GetCompact() (bnNew1 *= PastRateActualSeconds %d) = %08x \n",  PastRateActualSeconds, bnNew.GetCompact());
		bnNew /= PastRateTargetSeconds;
		//LogPrintf("bnNew.GetCompact()d (bnNew /= PastRateTargetSeconds %d) = %d \n", PastRateTargetSeconds, bnNew.GetCompact() );
	    //LogPrintf("bnNew.GetCompact() (bnNew /= PastRateTargetSeconds %d) = %08x \n", PastRateTargetSeconds, bnNew.GetCompact());
	}
	//LogPrintf("bnNew.GetCompact() %I <> bnProofOfWorkLimit.GetCompact() %I PastDifficultyAverage.GetCompact() %I \n", bnNew.GetCompact(), bnProofOfWorkLimit.GetCompact(), PastDifficultyAverage.GetCompact());
	
    if (bnNew.GetCompact() > bnProofOfWorkLimit.GetCompact()) { bnNew = bnProofOfWorkLimit; }
 
    if(fDebug){
		/// debug print
		LogPrintf("Difficulty Retarget - KGW Wormhole\n");
		LogPrintf("PastRateAdjustmentRatio = %g\n", PastRateAdjustmentRatio);
		LogPrintf("Before: %08x  %s\n", BlockLastSolved->nBits, arith_uint256().SetCompact(BlockLastSolved->nBits).ToString().c_str()); 
		LogPrintf("After:  %08x  %s\n", bnNew.GetCompact(), bnNew.ToString().c_str());
    }
    

       if (fTestNet) {
	  // testnet build fast
	  //LogPrintf("KimotoGravityWell Testnet return bnProofOfWorkLimit.GetCompact\n");
       return bnProofOfWorkLimit.GetCompact(); 
       }
        else {
	 //LogPrintf("KimotoGravityWell Testnet return bnNew.GetCompact\n");
	  if (pindexLast->nHeight+1 == 1099915) { return 471071101;}
       return bnNew.GetCompact();
		}


}

unsigned int GetNextWorkRequired_V2(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{

    static const int64_t        BlocksTargetSpacing                        = 90;
    unsigned int                TimeDaySeconds                             = 60 * 60 * 24;
    int64_t                     PastSecondsMin                             = TimeDaySeconds * 0.25;
    int64_t                     PastSecondsMax                             = TimeDaySeconds * 7;
    uint64_t                    PastBlocksMin                              = PastSecondsMin / BlocksTargetSpacing;
    uint64_t                    PastBlocksMax                              = PastSecondsMax / BlocksTargetSpacing;

	return KimotoGravityWell(pindexLast, BlocksTargetSpacing, PastBlocksMin, PastBlocksMax, params);
}



unsigned int static GetNextWorkRequired_V3(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
	bool fTestNet = gArgs.GetBoolArg("-testnet", false);
    //bool fDebug = gArgs.GetBoolArg("-debug", false);
    const arith_uint256 bnProofOfWorkLimit = UintToArith256(params.powLimit);
    const unsigned int nProofOfWorkLimit = bnProofOfWorkLimit.GetCompact();
    // DigiShield difficulty retarget system
    
    int blockstogoback = 0;
    int64_t nTargetSpacing = 1 * 90;             // target 90 sec
    int64_t retargetTimespan = nTargetSpacing;
    int64_t retargetSpacing = nTargetSpacing;
    int64_t retargetInterval = retargetTimespan / retargetSpacing;
	
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
       if (fTestNet){
	  return nProofOfWorkLimit;
	  }else{
		  
		  return pindexLast->nBits;
	  }
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
    int64_t nActualTimespan = pindexLast->GetBlockTime() - pindexFirst->GetBlockTime();
    //LogPrintf("  nActualTimespan = %g before bounds\n", nActualTimespan);

    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);

    if (nActualTimespan < (retargetTimespan - (retargetTimespan/4)) ) nActualTimespan = (retargetTimespan - (retargetTimespan/4));
    if (nActualTimespan > (retargetTimespan + (retargetTimespan/2)) ) nActualTimespan = (retargetTimespan + (retargetTimespan/2));

    // Retarget
    bnNew *= nActualTimespan;
    bnNew /= retargetTimespan;

    /// debug print
    //LogPrintf("DigiShield RETARGET \n");
    //LogPrintf("retargetTimespan = %g    nActualTimespan = %g \n", retargetTimespan, nActualTimespan);
    //LogPrintf("Before: %08x  %s\n", pindexLast->nBits, arith_uint256().SetCompact(pindexLast->nBits).ToString().c_str());
    //LogPrintf("After:  %08x  %s\n", bnNew.GetCompact(), bnNew.ToString().c_str());

    if (bnNew > bnProofOfWorkLimit)
        bnNew = bnProofOfWorkLimit;

    if (fTestNet){
		return nProofOfWorkLimit;
	}else{
	
		return bnNew.GetCompact();
	}
}

unsigned int static GetNextWorkRequired_V4(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params) {
 
 	bool fTestNet = gArgs.GetBoolArg("-testnet", false);
    //bool fDebug = gArgs.GetBoolArg("-debug", false);
    // current difficulty formula, ERC3 - DUAL_KGW3, written by Christian Knoepke - apfelbaum@email.de
    const CBlockIndex *BlockLastSolved = pindexLast;
    const CBlockIndex *BlockReading = pindexLast;
    bool kgwdebug=false;
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
	if(kgwdebug){
    LogPrintf("RETURN::kgwdebug::BLOCK %d ((int64_t)BlockLastSolved->nHeight < PastBlocksMin) bnPowLimit.GetCompact() %I  %08x %s\n",pindexLast->nHeight+1, bnPowLimit.GetCompact(), bnPowLimit.GetCompact(), bnPowLimit.ToString().c_str()); 
			}
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
	
    // DUAL KGW3 increased rapidly the Diff if Blocktime to last block under Blocktime/6 sec   15seconds.	
    if(kgwdebug){ LogPrintf("START::kgwdebug::BLOCK %d nActualTimespanshort = %d Blocktime/6 = %d \n" , pindexLast->nHeight+1, nActualTimespanshort, Blocktime/6 ); }
    if(nActualTimespanshort < Blocktime/6){  
        if(kgwdebug){ LogPrintf("ISSHORT::kgwdebug::BLOCK %d PREDIFF: bnNew.GetCompact() %I %08x %s bnNew first  \n", pindexLast->nHeight+1, bnNew.GetCompact(), bnNew.GetCompact(), bnNew.ToString().c_str() ); }
	    const int64_t nLongShortNew1 = 85;  
        const int64_t nLongShortNew2 = 100; 
        bnNew = bnNew * nLongShortNew1;	
        bnNew = bnNew / nLongShortNew2;	
	if(kgwdebug){ LogPrintf("ISSHORT::kgwdebug::BLOCK %d POSTDIFF: bnNew.GetCompact() %I %08x %s bnNew second \n" , pindexLast->nHeight+1 , bnNew.GetCompact(), bnNew.GetCompact(), bnNew.ToString().c_str() ); }
    }

    //BitBreak BitSend
    const int64_t nLongTimeLimit = 60 * 60; //mooncoin: 60 minutes
    if(kgwdebug){
	LogPrintf("kgwdebug::BLOCK %d (pblock->nTime - pindexLast->GetBlockTime()) = %d (max: %d) PREDIFF bnNew.GetCompact() %I %08x %s \n", pindexLast->nHeight+1,(pblock->nTime - pindexLast->GetBlockTime()), nLongTimeLimit, bnNew.GetCompact(), bnNew.GetCompact(), bnNew.ToString().c_str());
    }

    // Reduce difficulty if current block generation time has already exceeded maximum time limit.
	if ((pblock->nTime - pindexLast->GetBlockTime()) > nLongTimeLimit){
	bnNew = bnPowLimit/15;
       	if(kgwdebug){
           	LogPrintf("ISLONG::kgwdebug::BLOCK %d bnNew.GetCompact() %I Maximum block time hit - adjusted diff to %08x %s\n",pindexLast->nHeight+1, bnNew.GetCompact(), bnNew.GetCompact(), bnNew.ToString().c_str()); 
	           }
    }

    // Debug
    if (bnNew > bnPowLimit){
        //LogPrintf("BNNEWLARGE::BLOCK %d We wanted to set diff to %08x bnNew.GetCompact() %I but bnPowLimit is %08x  bnPowLimit.GetCompact() %I \n", pindexLast->nHeight+1, bnNew.GetCompact(), bnNew.GetCompact(), bnPowLimit.GetCompact(), bnPowLimit.GetCompact());
        bnNew = bnPowLimit;
    }
  
  if(kgwdebug){
           	LogPrintf("RETURN::kgwdebug::BLOCK %d bnNew.GetCompact() %I Maximum block time hit - adjusted diff to %08x %s\n",pindexLast->nHeight+1, bnNew.GetCompact(), bnNew.GetCompact(), bnNew.ToString().c_str()); 
			}
  
  
if (fTestNet){
  return bnNew.GetCompact();
}else{
  if (pindexLast->nHeight+1 == 1349150) { return 454022324; }
  if (pindexLast->nHeight+1 == 1500773) { return 469796712; }
  if (pindexLast->nHeight+1 == 1511216) { return 453654908; }
  if (pindexLast->nHeight+1 == 1527334) { return 455454971; }
  if (pindexLast->nHeight+1 == 1585972) { return 469833684; }
  return bnNew.GetCompact();
}


}

unsigned int CalculateNextWorkRequired_v5(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
	// Place-holder for new digishield
    
    const int64_t retargetTimespan = params.nPowTargetTimespan;
    const int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    int64_t nModulatedTimespan = nActualTimespan;
    int64_t nMaxTimespan;
    int64_t nMinTimespan;

    // amplitude filter - thanks to daft27 for this code
    nModulatedTimespan = retargetTimespan + (nModulatedTimespan - retargetTimespan) / 8;

    nMinTimespan = retargetTimespan - (retargetTimespan / 4);
    nMaxTimespan = retargetTimespan + (retargetTimespan / 2);

    // Limit adjustment step
    if (nModulatedTimespan < nMinTimespan)
        nModulatedTimespan = nMinTimespan;
    else if (nModulatedTimespan > nMaxTimespan)
        nModulatedTimespan = nMaxTimespan;

    // Retarget
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    bnNew *= nModulatedTimespan;
    bnNew /= retargetTimespan;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

unsigned int static GetNextWorkRequired_V5(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params) {
	    // Place-holder for new digishield
		
arith_uint256 nProofOfWorkLimit = UintToArith256(params.powLimit);

arith_uint256 nSecondDiff; nSecondDiff.SetCompact(0);
arith_uint256 nAverageDiff; nAverageDiff.SetCompact(0);
arith_uint256 nCurrentDiff; nCurrentDiff.SetCompact(0);
int64_t PastBlocksMax = 14;
int64_t BlockFoundinSec = 0;
int64_t BlockFoundCount = 0;
int64_t TargetSpacing = 90;

const CBlockIndex *BlockReadingFirst = pindexLast;
const CBlockIndex *BlockReadingSecond = pindexLast->pprev;

// 1) search for a recent(last 14 blocks) diff that is a success (success = hash found in +-20% of 90s )
//  1a) set an average success diff or set to last diff
    for (unsigned int i = 1; PastBlocksMax >= i ; i++) {    
	BlockFoundinSec = BlockReadingFirst->GetBlockTime() - BlockReadingSecond->GetBlockTime();
		
		if (BlockFoundinSec > 72 && BlockFoundinSec < 108)
		{
			BlockFoundCount++;
			nSecondDiff.SetCompact(BlockReadingSecond->nBits);
			if (BlockFoundCount > 1) {
				if(nSecondDiff >= nAverageDiff)
					nSecondDiff = ((nSecondDiff - nAverageDiff) / BlockFoundCount) + nAverageDiff;
				else
					nSecondDiff = nAverageDiff - ((nAverageDiff - nSecondDiff) / BlockFoundCount);
				}
			
		}
		nAverageDiff = nSecondDiff;
		
        if (BlockReadingFirst->pprev == NULL) { assert(BlockReadingFirst); break; }
		if (BlockReadingSecond->pprev == NULL) { assert(BlockReadingSecond); break; }
        BlockReadingFirst = BlockReadingFirst->pprev;
		BlockReadingSecond = BlockReadingSecond->pprev;
    }

//2) set Current diff to result from 1
    int nModHeight = pindexLast->nHeight - 1;
	const CBlockIndex *pindexNxLast = pindexLast->GetAncestor(nModHeight);
    assert(pindexNxLast);
	arith_uint256 pindexLastDiff; pindexLastDiff.SetCompact(pindexLast->nBits);
	arith_uint256 LowNxLastDiff; LowNxLastDiff.SetCompact(pindexNxLast->nBits);
	arith_uint256 HighNxLastDiff; HighNxLastDiff.SetCompact(pindexNxLast->nBits);
	int64_t LowModulation = 63; //90 * .7 = 30% smaller
	int64_t HighModulation = 117; //90 * 1.3 = 30% larger
	LowNxLastDiff *= LowModulation;
	LowNxLastDiff /= TargetSpacing;
	HighNxLastDiff *= HighModulation;
	HighNxLastDiff /= TargetSpacing;
	
    if(BlockFoundCount == 0 ){
		nCurrentDiff.SetCompact(pindexLast->nBits);
	}else if (pindexLastDiff < LowNxLastDiff && (pindexLast->GetBlockTime() - pindexNxLast->GetBlockTime()) < 108){
		nCurrentDiff.SetCompact(pindexLast->nBits);
	}else if (pindexLastDiff > HighNxLastDiff && (pindexLast->GetBlockTime() - pindexNxLast->GetBlockTime()) < 108){
		nCurrentDiff.SetCompact(pindexLast->nBits);
	}else{
		nCurrentDiff = nAverageDiff;
	}



//3) Check for edge cases and handle
    //hash time too short
    if (pblock->GetBlockTime() < pindexLast->GetBlockTime() + 45)
    {
	    arith_uint256 shNew;
		int64_t shModulation = 54; //90 * .6 = 40% smaller
		shNew = nCurrentDiff;
		shNew *= shModulation;
		shNew /= TargetSpacing;

		return shNew.GetCompact();
	}
    // Special rules for Digishield edge cases
	arith_uint256 rcNew;
	int64_t rcModulation = 90; 
    if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*14)
    {
        // If the new block's timestamp is more than 14* nTargetSpacing(90s) = minutes(21m)
        // then allow mining of a low-difficulty block.
		nProofOfWorkLimit =  nProofOfWorkLimit / 15;
        return nProofOfWorkLimit.GetCompact();
    }else if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*7){
		// If the new block's timestamp is more than 7* nTargetSpacing(90s) = minutes(10.5m)
        // then reduce difficulty block by 65%.
		rcModulation = 149; //90(TargetSpacing) * 1.655 = 65% larger
		rcNew = nCurrentDiff;
		rcNew *= rcModulation;
		rcNew /= TargetSpacing;

		if (rcNew > nProofOfWorkLimit){rcNew = nProofOfWorkLimit;}
		return rcNew.GetCompact();
		
	}
	else if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*5){
		// If the new block's timestamp is more than 5* nTargetSpacing(90s) = minutes(7.5m)
        // then reduce difficulty block by 45%.
		rcModulation = 131; //90(TargetSpacing) * 1.455 = 45% larger
		rcNew = nCurrentDiff;
		rcNew *= rcModulation;
		rcNew /= TargetSpacing;

		if (rcNew > nProofOfWorkLimit){rcNew = nProofOfWorkLimit;}
		return rcNew.GetCompact();
		
	}else if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*4){
		// If the new block's timestamp is more than 4* nTargetSpacing(90s) = minutes(6m)
        // then reduce difficulty block by 30%.
		
        rcModulation = 117; //90(TargetSpacing) * 1.3 = 30% larger
		rcNew = nCurrentDiff;
		rcNew *= rcModulation;
		rcNew /= TargetSpacing;

		if (rcNew > nProofOfWorkLimit){rcNew = nProofOfWorkLimit;}
		return rcNew.GetCompact();
		
	}else if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*3){
		// If the new block's timestamp is more than 3* nTargetSpacing(90s) = minutes(4.5m)
        // then reduce difficulty block by 15%.
		
		rcModulation = 104; //90(TargetSpacing) * 1.155 = 15.5% larger
		rcNew = nCurrentDiff;
		rcNew *= rcModulation;
		rcNew /= TargetSpacing;

		if (rcNew > nProofOfWorkLimit){rcNew = nProofOfWorkLimit;}
		return rcNew.GetCompact();
	}

    
	int nHeightFirst = pindexLast->nHeight - 90;
	const CBlockIndex* pindexFirst = pindexLast->GetAncestor(nHeightFirst);
    assert(pindexFirst);
    const int64_t retargetTimespan = params.nPowTargetTimespan;
    const int64_t nActualTimespan = pindexLast->GetBlockTime() - pindexFirst->GetBlockTime();
    int64_t nModulatedTimespan = nActualTimespan;



    // amplitude filter - thanks to daft27 for this code
    nModulatedTimespan = retargetTimespan + (nModulatedTimespan - (retargetTimespan * 90)) / 90;

	
    // Limit adjustment step
	if (nModulatedTimespan > 45 )
        nModulatedTimespan = 81;
    else if (nModulatedTimespan >= 45 && nModulatedTimespan < 90)
        nModulatedTimespan = 87;
    else if (nModulatedTimespan >= 90 && nModulatedTimespan <= 180)
        nModulatedTimespan = 93;
	else if (nModulatedTimespan < 180)
		nModulatedTimespan = 96;

    // Retarget
    arith_uint256 bnNew;
    bnNew = nCurrentDiff;
    bnNew *= nModulatedTimespan;
    bnNew /= retargetTimespan;

    if (bnNew > nProofOfWorkLimit)
        bnNew = nProofOfWorkLimit;

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
	//bool fTestNet = gArgs.GetBoolArg("-testnet", false);
    bool fDebug = gArgs.GetBoolArg("-debug", false);
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);
 
    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit)){
      if (!fDebug){
        return false;
      } else {
        return error("CheckProofOfWork(): nBits below minimum work");
      }
    }

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget){
      if (!fDebug){
        return false;
      } else {
        return error("CheckProofOfWork(): hash doesn't match nBits (hash %s bnTarget %s)", hash.ToString().c_str(), bnTarget.ToString().c_str());
      }
    }

    return true;
}

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
   
   int DiffMode = params.DiffMode;

   if (pindexLast->nHeight+1 >= params.PowV2) { DiffMode = 2; }
   if (pindexLast->nHeight+1 >= params.PowV3) { DiffMode = 3; } 
   if ((pindexLast->nHeight+1 >= params.PowV4) && 
       (pindexLast->nHeight+1 < params.PowV4 + 8)) { DiffMode = 4; } // 8 was the smoothing period
   if (pindexLast->nHeight+1 >= params.PowV4 + 8)  { DiffMode = 5; }
   if (pindexLast->nHeight+1 >= params.PowV5 )  { DiffMode = 6; }
   //actions

   if (DiffMode == 1) { return GetNextWorkRequired_V1(pindexLast, pblock, params); }
   if (DiffMode == 2) { return GetNextWorkRequired_V2(pindexLast, pblock, params); }
   if (DiffMode == 3) { return GetNextWorkRequired_V3(pindexLast, pblock, params); }
   if (DiffMode == 4) { return 0x1e0fffff; }
   if (DiffMode == 5) { return GetNextWorkRequired_V4(pindexLast, pblock, params); }
   if (DiffMode == 6) { return GetNextWorkRequired_V5(pindexLast, pblock, params); }
   return GetNextWorkRequired_V4(pindexLast, pblock, params); 
}
