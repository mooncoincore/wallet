 // Copyright (c) 2017-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_TX_VERIFY_PATCH_H
#define BITCOIN_CONSENSUS_TX_VERIFY_PATCH_H

#include <amount.h>

#include <stdint.h>
#include <vector>
#include <clientversion.h>
#include <checkqueue.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <key.h>
#include <keystore.h>
#include <validation.h>
#include <policy/policy.h>
#include <script/script.h>
#include <script/sign.h>
#include <script/script_error.h>
#include <script/standard.h>
#include <utilstrencodings.h>

#include <map>
#include <string>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>


#include <univalue.h>

/** Transaction validation functions */

/** Context-independent validity checks */
bool CheckCoinbaseLength(const CTransaction& tx);

/** Context-dependent validity checks */
bool ContextualWitnessNonceSize(const CBlock& block, const CBlockIndex* pindexPrev);

#endif // BITCOIN_CONSENSUS_TX_VERIFY_PATCH_H