 // Copyright (c) 2017-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_TX_VERIFY_PATCH_H
#define BITCOIN_CONSENSUS_TX_VERIFY_PATCH_H

#include <amount.h>

#include <stdint.h>
#include <vector>


/** Transaction validation functions */

/** Context-independent validity checks */
bool CheckCoinbaseLength(const CTransaction& tx);

/** Context-dependent validity checks */
bool ContextualWitnessNonceSize(const CBlock& block, const CBlockIndex* pindexPrev);

