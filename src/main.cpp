// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2016-2017 The Mooncoin Core developers (barrystyle https://github.com/mooncoindev until 0.13.3)
// Copyright (c) 2017 The Mooncoin Core developers (https://github.com/mooncoincore)
//
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"
#include "addrman.h"
#include "arith_uint256.h"
#include "blockencodings.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "checkqueue.h"
#include "consensus/consensus.h"
#include "consensus/merkle.h"
#include "consensus/validation.h"
#include "hash.h"
#include "init.h"
#include "merkleblock.h"
#include "net.h"
#include "policy/fees.h"
#include "policy/policy.h"
#include "pow.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "random.h"
#include "script/script.h"
#include "script/sigcache.h"
#include "script/standard.h"
#include "tinyformat.h"
#include "txdb.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "undo.h"
#include "util.h"
#include "utilmoneystr.h"
#include "utilstrencodings.h"
#include "validationinterface.h"
#include "versionbits.h"

#include <atomic>
#include <sstream>

#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/math/distributions/poisson.hpp>
#include <boost/thread.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_int.hpp>

using namespace std;

#if defined(NDEBUG)
# error "Mooncoin cannot be compiled without assertions."
#endif

/**
 * Global state
 */

CCriticalSection cs_main;

BlockMap mapBlockIndex;
CChain chainActive;
CBlockIndex *pindexBestHeader = NULL;
int64_t nTimeBestReceived = 0;
CWaitableCriticalSection csBestBlock;
CConditionVariable cvBlockChange;
int nScriptCheckThreads = 0;
bool fImporting = false;
bool fReindex = false;
bool fTxIndex = false;
bool fHavePruned = false;
bool fPruneMode = false;
bool fIsBareMultisigStd = DEFAULT_PERMIT_BAREMULTISIG;
bool fRequireStandard = true;
bool fCheckBlockIndex = false;
bool fCheckpointsEnabled = DEFAULT_CHECKPOINTS_ENABLED;
size_t nCoinCacheUsage = 5000 * 300;
uint64_t nPruneTarget = 0;
int64_t nMaxTipAge = DEFAULT_MAX_TIP_AGE;
bool fEnableReplacement = DEFAULT_ENABLE_REPLACEMENT;

//baz's own
int AlgoForkHeight = 1250000;
bool AlgoSwitch = false;
double LastBlocks = -1;
double ClientUptime = -1;
int WarpSync = 64;

CFeeRate minRelayTxFee = CFeeRate(DEFAULT_MIN_RELAY_TX_FEE);
CAmount maxTxFee = DEFAULT_TRANSACTION_MAXFEE;

CTxMemPool mempool(::minRelayTxFee);
FeeFilterRounder filterRounder(::minRelayTxFee);

struct IteratorComparator
{
    template<typename I>
    bool operator()(const I& a, const I& b)
    {
        return &(*a) < &(*b);
    }
};

struct COrphanTx {
    CTransaction tx;
    NodeId fromPeer;
    int64_t nTimeExpire;
};
map<uint256, COrphanTx> mapOrphanTransactions GUARDED_BY(cs_main);
map<COutPoint, set<map<uint256, COrphanTx>::iterator, IteratorComparator>> mapOrphanTransactionsByPrev GUARDED_BY(cs_main);
void EraseOrphansFor(NodeId peer) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/**
 * Returns true if there are nRequired or more blocks of minVersion or above
 * in the last Consensus::Params::nMajorityWindow blocks, starting at pstart and going backwards.
 */
static bool IsSuperMajority(int minVersion, const CBlockIndex* pstart, unsigned nRequired, const Consensus::Params& consensusParams);
static void CheckBlockIndex(const Consensus::Params& consensusParams);

/** Constant stuff for coinbase transactions we create: */
CScript COINBASE_FLAGS;

const string strMessageMagic = "Mooncoin Signed Message:\n";

// Internal stuff
namespace {

    struct CBlockIndexWorkComparator
    {
        bool operator()(CBlockIndex *pa, CBlockIndex *pb) const {
            // First sort by most total work, ...
            if (pa->nChainWork > pb->nChainWork) return false;
            if (pa->nChainWork < pb->nChainWork) return true;

            // ... then by earliest time received, ...
            if (pa->nSequenceId < pb->nSequenceId) return false;
            if (pa->nSequenceId > pb->nSequenceId) return true;

            // Use pointer address as tie breaker (should only happen with blocks
            // loaded from disk, as those all have id 0).
            if (pa < pb) return false;
            if (pa > pb) return true;

            // Identical blocks.
            return false;
        }
    };

    CBlockIndex *pindexBestInvalid;

    /**
     * The set of all CBlockIndex entries with BLOCK_VALID_TRANSACTIONS (for itself and all ancestors) and
     * as good as our current tip or better. Entries may be failed, though, and pruning nodes may be
     * missing the data for the block.
     */
    set<CBlockIndex*, CBlockIndexWorkComparator> setBlockIndexCandidates;
    /** Number of nodes with fSyncStarted. */
    int nSyncStarted = 0;
    /** All pairs A->B, where A (or one of its ancestors) misses transactions, but B has transactions.
     * Pruned nodes may have entries where B is missing data.
     */
    multimap<CBlockIndex*, CBlockIndex*> mapBlocksUnlinked;

    CCriticalSection cs_LastBlockFile;
    std::vector<CBlockFileInfo> vinfoBlockFile;
    int nLastBlockFile = 0;
    /** Global flag to indicate we should check to see if there are
     *  block/undo files that should be deleted.  Set on startup
     *  or if we allocate more file space when we're in prune mode
     */
    bool fCheckForPruning = false;

    /**
     * Every received block is assigned a unique and increasing identifier, so we
     * know which one to give priority in case of a fork.
     */
    CCriticalSection cs_nBlockSequenceId;
    /** Blocks loaded from disk are assigned id 0, so start the counter at 1. */
    uint32_t nBlockSequenceId = 1;

    /**
     * Sources of received blocks, saved to be able to send them reject
     * messages or ban them when processing happens afterwards. Protected by
     * cs_main.
     * Set mapBlockSource[hash].second to false if the node should not be
     * punished if the block is invalid.
     */
    map<uint256, std::pair<NodeId, bool>> mapBlockSource;

    /**
     * Filter for transactions that were recently rejected by
     * AcceptToMemoryPool. These are not rerequested until the chain tip
     * changes, at which point the entire filter is reset. Protected by
     * cs_main.
     *
     * Without this filter we'd be re-requesting txs from each of our peers,
     * increasing bandwidth consumption considerably. For instance, with 100
     * peers, half of which relay a tx we don't accept, that might be a 50x
     * bandwidth increase. A flooding attacker attempting to roll-over the
     * filter using minimum-sized, 60byte, transactions might manage to send
     * 1000/sec if we have fast peers, so we pick 120,000 to give our peers a
     * two minute window to send invs to us.
     *
     * Decreasing the false positive rate is fairly cheap, so we pick one in a
     * million to make it highly unlikely for users to have issues with this
     * filter.
     *
     * Memory used: 1.3 MB
     */
    boost::scoped_ptr<CRollingBloomFilter> recentRejects;
    uint256 hashRecentRejectsChainTip;

    /** Blocks that are in flight, and that are in the queue to be downloaded. Protected by cs_main. */
    struct QueuedBlock {
        uint256 hash;
        CBlockIndex* pindex;                                     //!< Optional.
        bool fValidatedHeaders;                                  //!< Whether this block has validated headers at the time of request.
        std::unique_ptr<PartiallyDownloadedBlock> partialBlock;  //!< Optional, used for CMPCTBLOCK downloads
    };
    map<uint256, pair<NodeId, list<QueuedBlock>::iterator> > mapBlocksInFlight;

    /** Stack of nodes which we have set to announce using compact blocks */
    list<NodeId> lNodesAnnouncingHeaderAndIDs;

    /** Number of preferable block download peers. */
    int nPreferredDownload = 0;

    /** Dirty block index entries. */
    set<CBlockIndex*> setDirtyBlockIndex;

    /** Dirty block file entries. */
    set<int> setDirtyFileInfo;

    /** Number of peers from which we're downloading blocks. */
    int nPeersWithValidatedDownloads = 0;

    /** Relay map, protected by cs_main. */
    typedef std::map<uint256, std::shared_ptr<const CTransaction>> MapRelay;
    MapRelay mapRelay;
    /** Expiration-time ordered list of (expire time, relay map entry) pairs, protected by cs_main). */
    std::deque<std::pair<int64_t, MapRelay::iterator>> vRelayExpiration;
} // anon namespace

//////////////////////////////////////////////////////////////////////////////
//
// Registration of network node signals.
//

namespace {

struct CBlockReject {
    unsigned char chRejectCode;
    string strRejectReason;
    uint256 hashBlock;
};

/**
 * Maintain validation-specific state about nodes, protected by cs_main, instead
 * by CNode's own locks. This simplifies asynchronous operation, where
 * processing of incoming data is done after the ProcessMessage call returns,
 * and we're no longer holding the node's locks.
 */
struct CNodeState {
    //! The peer's address
    CService address;
    //! Whether we have a fully established connection.
    bool fCurrentlyConnected;
    //! Accumulated misbehaviour score for this peer.
    int nMisbehavior;
    //! Whether this peer should be disconnected and banned (unless whitelisted).
    bool fShouldBan;
    //! String name of this peer (debugging/logging purposes).
    std::string name;
    //! List of asynchronously-determined block rejections to notify this peer about.
    std::vector<CBlockReject> rejects;
    //! The best known block we know this peer has announced.
    CBlockIndex *pindexBestKnownBlock;
    //! The hash of the last unknown block this peer has announced.
    uint256 hashLastUnknownBlock;
    //! The last full block we both have.
    CBlockIndex *pindexLastCommonBlock;
    //! The best header we have sent our peer.
    CBlockIndex *pindexBestHeaderSent;
    //! Length of current-streak of unconnecting headers announcements
    int nUnconnectingHeaders;
    //! Whether we've started headers synchronization with this peer.
    bool fSyncStarted;
    //! Since when we're stalling block download progress (in microseconds), or 0.
    int64_t nStallingSince;
    list<QueuedBlock> vBlocksInFlight;
    //! When the first entry in vBlocksInFlight started downloading. Don't care when vBlocksInFlight is empty.
    int64_t nDownloadingSince;
    int nBlocksInFlight;
    int nBlocksInFlightValidHeaders;
    //! Whether we consider this a preferred download peer.
    bool fPreferredDownload;
    //! Whether this peer wants invs or headers (when possible) for block announcements.
    bool fPreferHeaders;
    //! Whether this peer wants invs or cmpctblocks (when possible) for block announcements.
    bool fPreferHeaderAndIDs;
    /**
      * Whether this peer will send us cmpctblocks if we request them.
      * This is not used to gate request logic, as we really only care about fSupportsDesiredCmpctVersion,
      * but is used as a flag to "lock in" the version of compact blocks (fWantsCmpctWitness) we send.
      */
    bool fProvidesHeaderAndIDs;
    //! Whether this peer can give us witnesses
    bool fHaveWitness;
    //! Whether this peer wants witnesses in cmpctblocks/blocktxns
    bool fWantsCmpctWitness;
    /**
     * If we've announced NODE_WITNESS to this peer: whether the peer sends witnesses in cmpctblocks/blocktxns,
     * otherwise: whether this peer sends non-witnesses in cmpctblocks/blocktxns.
     */
    bool fSupportsDesiredCmpctVersion;

    CNodeState() {
        fCurrentlyConnected = false;
        nMisbehavior = 0;
        fShouldBan = false;
        pindexBestKnownBlock = NULL;
        hashLastUnknownBlock.SetNull();
        pindexLastCommonBlock = NULL;
        pindexBestHeaderSent = NULL;
        nUnconnectingHeaders = 0;
        fSyncStarted = false;
        nStallingSince = 0;
        nDownloadingSince = 0;
        nBlocksInFlight = 0;
        nBlocksInFlightValidHeaders = 0;
        fPreferredDownload = false;
        fPreferHeaders = false;
        fPreferHeaderAndIDs = false;
        fProvidesHeaderAndIDs = false;
        fHaveWitness = false;
        fWantsCmpctWitness = false;
        fSupportsDesiredCmpctVersion = false;
    }
};

/** Map maintaining per-node state. Requires cs_main. */
map<NodeId, CNodeState> mapNodeState;

// Requires cs_main.
CNodeState *State(NodeId pnode) {
    map<NodeId, CNodeState>::iterator it = mapNodeState.find(pnode);
    if (it == mapNodeState.end())
        return NULL;
    return &it->second;
}

int GetHeight()
{
    LOCK(cs_main);
    return chainActive.Height();
}

void UpdatePreferredDownload(CNode* node, CNodeState* state)
{
    nPreferredDownload -= state->fPreferredDownload;

    // Whether this node should be marked as a preferred download node.
    state->fPreferredDownload = (!node->fInbound || node->fWhitelisted) && !node->fOneShot && !node->fClient;

    nPreferredDownload += state->fPreferredDownload;
}

void InitializeNode(NodeId nodeid, const CNode *pnode) {
    LOCK(cs_main);
    CNodeState &state = mapNodeState.insert(std::make_pair(nodeid, CNodeState())).first->second;
    state.name = pnode->addrName;
    state.address = pnode->addr;
}

void FinalizeNode(NodeId nodeid) {
    LOCK(cs_main);
    CNodeState *state = State(nodeid);

    if (state->fSyncStarted)
        nSyncStarted--;

    if (state->nMisbehavior == 0 && state->fCurrentlyConnected) {
        AddressCurrentlyConnected(state->address);
    }

    BOOST_FOREACH(const QueuedBlock& entry, state->vBlocksInFlight) {
        mapBlocksInFlight.erase(entry.hash);
    }
    EraseOrphansFor(nodeid);
    nPreferredDownload -= state->fPreferredDownload;
    nPeersWithValidatedDownloads -= (state->nBlocksInFlightValidHeaders != 0);
    assert(nPeersWithValidatedDownloads >= 0);

    mapNodeState.erase(nodeid);

    if (mapNodeState.empty()) {
        // Do a consistency check after the last peer is removed.
        assert(mapBlocksInFlight.empty());
        assert(nPreferredDownload == 0);
        assert(nPeersWithValidatedDownloads == 0);
    }
}

// Requires cs_main.
// Returns a bool indicating whether we requested this block.
// Also used if a block was /not/ received and timed out or started with another peer
bool MarkBlockAsReceived(const uint256& hash) {
    map<uint256, pair<NodeId, list<QueuedBlock>::iterator> >::iterator itInFlight = mapBlocksInFlight.find(hash);
    if (itInFlight != mapBlocksInFlight.end()) {
        CNodeState *state = State(itInFlight->second.first);
        state->nBlocksInFlightValidHeaders -= itInFlight->second.second->fValidatedHeaders;
        if (state->nBlocksInFlightValidHeaders == 0 && itInFlight->second.second->fValidatedHeaders) {
            // Last validated block on the queue was received.
            nPeersWithValidatedDownloads--;
        }
        if (state->vBlocksInFlight.begin() == itInFlight->second.second) {
            // First block on the queue was received, update the start download time for the next one
            state->nDownloadingSince = std::max(state->nDownloadingSince, GetTimeMicros());
        }
        state->vBlocksInFlight.erase(itInFlight->second.second);
        state->nBlocksInFlight--;
        state->nStallingSince = 0;
        mapBlocksInFlight.erase(itInFlight);
        return true;
    }
    return false;
}

// Requires cs_main.
// returns false, still setting pit, if the block was already in flight from the same peer
// pit will only be valid as long as the same cs_main lock is being held
bool MarkBlockAsInFlight(NodeId nodeid, const uint256& hash, const Consensus::Params& consensusParams, CBlockIndex *pindex = NULL, list<QueuedBlock>::iterator **pit = NULL) {
    CNodeState *state = State(nodeid);
    assert(state != NULL);

    // Short-circuit most stuff in case its from the same node
    map<uint256, pair<NodeId, list<QueuedBlock>::iterator> >::iterator itInFlight = mapBlocksInFlight.find(hash);
    if (itInFlight != mapBlocksInFlight.end() && itInFlight->second.first == nodeid) {
        *pit = &itInFlight->second.second;
        return false;
    }

    // Make sure it's not listed somewhere already.
    MarkBlockAsReceived(hash);

    list<QueuedBlock>::iterator it = state->vBlocksInFlight.insert(state->vBlocksInFlight.end(),
            {hash, pindex, pindex != NULL, std::unique_ptr<PartiallyDownloadedBlock>(pit ? new PartiallyDownloadedBlock(&mempool) : NULL)});
    state->nBlocksInFlight++;
    state->nBlocksInFlightValidHeaders += it->fValidatedHeaders;
    if (state->nBlocksInFlight == 1) {
        // We're starting a block download (batch) from this peer.
        state->nDownloadingSince = GetTimeMicros();
    }
    if (state->nBlocksInFlightValidHeaders == 1 && pindex != NULL) {
        nPeersWithValidatedDownloads++;
    }
    itInFlight = mapBlocksInFlight.insert(std::make_pair(hash, std::make_pair(nodeid, it))).first;
    if (pit)
        *pit = &itInFlight->second.second;
    return true;
}

/** Check whether the last unknown block a peer advertised is not yet known. */
void ProcessBlockAvailability(NodeId nodeid) {
    CNodeState *state = State(nodeid);
    assert(state != NULL);

    if (!state->hashLastUnknownBlock.IsNull()) {
        BlockMap::iterator itOld = mapBlockIndex.find(state->hashLastUnknownBlock);
        if (itOld != mapBlockIndex.end() && itOld->second->nChainWork > 0) {
            if (state->pindexBestKnownBlock == NULL || itOld->second->nChainWork >= state->pindexBestKnownBlock->nChainWork)
                state->pindexBestKnownBlock = itOld->second;
            state->hashLastUnknownBlock.SetNull();
        }
    }
}

/** Update tracking information about which blocks a peer is assumed to have. */
void UpdateBlockAvailability(NodeId nodeid, const uint256 &hash) {
    CNodeState *state = State(nodeid);
    assert(state != NULL);

    ProcessBlockAvailability(nodeid);

    BlockMap::iterator it = mapBlockIndex.find(hash);
    if (it != mapBlockIndex.end() && it->second->nChainWork > 0) {
        // An actually better block was announced.
        if (state->pindexBestKnownBlock == NULL || it->second->nChainWork >= state->pindexBestKnownBlock->nChainWork)
            state->pindexBestKnownBlock = it->second;
    } else {
        // An unknown block was announced; just assume that the latest one is the best one.
        state->hashLastUnknownBlock = hash;
    }
}

void MaybeSetPeerAsAnnouncingHeaderAndIDs(const CNodeState* nodestate, CNode* pfrom) {
    if (!nodestate->fSupportsDesiredCmpctVersion) {
        // Never ask from peers who can't provide witnesses.
        return;
    }
    if (nodestate->fProvidesHeaderAndIDs) {
        for (std::list<NodeId>::iterator it = lNodesAnnouncingHeaderAndIDs.begin(); it != lNodesAnnouncingHeaderAndIDs.end(); it++) {
            if (*it == pfrom->GetId()) {
                lNodesAnnouncingHeaderAndIDs.erase(it);
                lNodesAnnouncingHeaderAndIDs.push_back(pfrom->GetId());
                return;
            }
        }
        bool fAnnounceUsingCMPCTBLOCK = false;
        uint64_t nCMPCTBLOCKVersion = (nLocalServices & NODE_WITNESS) ? 2 : 1;
        if (lNodesAnnouncingHeaderAndIDs.size() >= 3) {
            // As per BIP152, we only get 3 of our peers to announce
            // blocks using compact encodings.
            CNode* pnodeStop = FindNode(lNodesAnnouncingHeaderAndIDs.front());
            if (pnodeStop) {
                pnodeStop->PushMessage(NetMsgType::SENDCMPCT, fAnnounceUsingCMPCTBLOCK, nCMPCTBLOCKVersion);
            }
            lNodesAnnouncingHeaderAndIDs.pop_front();
        }
        fAnnounceUsingCMPCTBLOCK = true;
        pfrom->PushMessage(NetMsgType::SENDCMPCT, fAnnounceUsingCMPCTBLOCK, nCMPCTBLOCKVersion);
        lNodesAnnouncingHeaderAndIDs.push_back(pfrom->GetId());
    }
}

// Requires cs_main
bool CanDirectFetch(const Consensus::Params &consensusParams)
{
    return chainActive.Tip()->GetBlockTime() > GetAdjustedTime() - consensusParams.nPowTargetSpacing * 20;
}

// Requires cs_main
bool PeerHasHeader(CNodeState *state, CBlockIndex *pindex)
{
    if (state->pindexBestKnownBlock && pindex == state->pindexBestKnownBlock->GetAncestor(pindex->nHeight))
        return true;
    if (state->pindexBestHeaderSent && pindex == state->pindexBestHeaderSent->GetAncestor(pindex->nHeight))
        return true;
    return false;
}

/** Find the last common ancestor two blocks have.
 *  Both pa and pb must be non-NULL. */
CBlockIndex* LastCommonAncestor(CBlockIndex* pa, CBlockIndex* pb) {
    if (pa->nHeight > pb->nHeight) {
        pa = pa->GetAncestor(pb->nHeight);
    } else if (pb->nHeight > pa->nHeight) {
        pb = pb->GetAncestor(pa->nHeight);
    }

    while (pa != pb && pa && pb) {
        pa = pa->pprev;
        pb = pb->pprev;
    }

    // Eventually all chain branches meet at the genesis block.
    assert(pa == pb);
    return pa;
}

/** Update pindexLastCommonBlock and add not-in-flight missing successors to vBlocks, until it has
 *  at most count entries. */
void FindNextBlocksToDownload(NodeId nodeid, unsigned int count, std::vector<CBlockIndex*>& vBlocks, NodeId& nodeStaller, const Consensus::Params& consensusParams) {
    if (count == 0)
        return;

    vBlocks.reserve(vBlocks.size() + count);
    CNodeState *state = State(nodeid);
    assert(state != NULL);

    // Make sure pindexBestKnownBlock is up to date, we'll need it.
    ProcessBlockAvailability(nodeid);

    if (state->pindexBestKnownBlock == NULL || state->pindexBestKnownBlock->nChainWork < chainActive.Tip()->nChainWork) {
        // This peer has nothing interesting.
        return;
    }

    if (state->pindexLastCommonBlock == NULL) {
        // Bootstrap quickly by guessing a parent of our best tip is the forking point.
        // Guessing wrong in either direction is not a problem.
        state->pindexLastCommonBlock = chainActive[std::min(state->pindexBestKnownBlock->nHeight, chainActive.Height())];
    }

    // If the peer reorganized, our previous pindexLastCommonBlock may not be an ancestor
    // of its current tip anymore. Go back enough to fix that.
    state->pindexLastCommonBlock = LastCommonAncestor(state->pindexLastCommonBlock, state->pindexBestKnownBlock);
    if (state->pindexLastCommonBlock == state->pindexBestKnownBlock)
        return;

    std::vector<CBlockIndex*> vToFetch;
    CBlockIndex *pindexWalk = state->pindexLastCommonBlock;
    // Never fetch further than the best block we know the peer has, or more than BLOCK_DOWNLOAD_WINDOW + 1 beyond the last
    // linked block we have in common with this peer. The +1 is so we can detect stalling, namely if we would be able to
    // download that next block if the window were 1 larger.
    int nWindowEnd = state->pindexLastCommonBlock->nHeight + BLOCK_DOWNLOAD_WINDOW;
    int nMaxHeight = std::min<int>(state->pindexBestKnownBlock->nHeight, nWindowEnd + 1);
    NodeId waitingfor = -1;
    while (pindexWalk->nHeight < nMaxHeight) {
        // Read up to 128 (or more, if more blocks than that are needed) successors of pindexWalk (towards
        // pindexBestKnownBlock) into vToFetch. We fetch 128, because CBlockIndex::GetAncestor may be as expensive
        // as iterating over ~100 CBlockIndex* entries anyway.
        int nToFetch = std::min(nMaxHeight - pindexWalk->nHeight, std::max<int>(count - vBlocks.size(), 128));
        vToFetch.resize(nToFetch);
        pindexWalk = state->pindexBestKnownBlock->GetAncestor(pindexWalk->nHeight + nToFetch);
        vToFetch[nToFetch - 1] = pindexWalk;
        for (unsigned int i = nToFetch - 1; i > 0; i--) {
            vToFetch[i - 1] = vToFetch[i]->pprev;
        }

        // Iterate over those blocks in vToFetch (in forward direction), adding the ones that
        // are not yet downloaded and not in flight to vBlocks. In the mean time, update
        // pindexLastCommonBlock as long as all ancestors are already downloaded, or if it's
        // already part of our chain (and therefore don't need it even if pruned).
        BOOST_FOREACH(CBlockIndex* pindex, vToFetch) {
            if (!pindex->IsValid(BLOCK_VALID_TREE)) {
                // We consider the chain that this peer is on invalid.
                return;
            }
            if (!State(nodeid)->fHaveWitness && IsWitnessEnabled(pindex->pprev, consensusParams)) {
                // We wouldn't download this block or its descendants from this peer.
                return;
            }
            if (pindex->nStatus & BLOCK_HAVE_DATA || chainActive.Contains(pindex)) {
                if (pindex->nChainTx)
                    state->pindexLastCommonBlock = pindex;
            } else if (mapBlocksInFlight.count(pindex->GetBlockHash()) == 0) {
                // The block is not already downloaded, and not yet in flight.
                if (pindex->nHeight > nWindowEnd) {
                    // We reached the end of the window.
                    if (vBlocks.size() == 0 && waitingfor != nodeid) {
                        // We aren't able to fetch anything, but we would be if the download window was one larger.
                        nodeStaller = waitingfor;
                    }
                    return;
                }
                vBlocks.push_back(pindex);
                if (vBlocks.size() == count) {
                    return;
                }
            } else if (waitingfor == -1) {
                // This is the first already-in-flight block.
                waitingfor = mapBlocksInFlight[pindex->GetBlockHash()].first;
            }
        }
    }
}

} // anon namespace

bool GetNodeStateStats(NodeId nodeid, CNodeStateStats &stats) {
    LOCK(cs_main);
    CNodeState *state = State(nodeid);
    if (state == NULL)
        return false;
    stats.nMisbehavior = state->nMisbehavior;
    stats.nSyncHeight = state->pindexBestKnownBlock ? state->pindexBestKnownBlock->nHeight : -1;
    stats.nCommonHeight = state->pindexLastCommonBlock ? state->pindexLastCommonBlock->nHeight : -1;
    BOOST_FOREACH(const QueuedBlock& queue, state->vBlocksInFlight) {
        if (queue.pindex)
            stats.vHeightInFlight.push_back(queue.pindex->nHeight);
    }
    return true;
}

void RegisterNodeSignals(CNodeSignals& nodeSignals)
{
    nodeSignals.GetHeight.connect(&GetHeight);
    nodeSignals.ProcessMessages.connect(&ProcessMessages);
    nodeSignals.SendMessages.connect(&SendMessages);
    nodeSignals.InitializeNode.connect(&InitializeNode);
    nodeSignals.FinalizeNode.connect(&FinalizeNode);
}

void UnregisterNodeSignals(CNodeSignals& nodeSignals)
{
    nodeSignals.GetHeight.disconnect(&GetHeight);
    nodeSignals.ProcessMessages.disconnect(&ProcessMessages);
    nodeSignals.SendMessages.disconnect(&SendMessages);
    nodeSignals.InitializeNode.disconnect(&InitializeNode);
    nodeSignals.FinalizeNode.disconnect(&FinalizeNode);
}

CBlockIndex* FindForkInGlobalIndex(const CChain& chain, const CBlockLocator& locator)
{
    // Find the first block the caller has in the main chain
    BOOST_FOREACH(const uint256& hash, locator.vHave) {
        BlockMap::iterator mi = mapBlockIndex.find(hash);
        if (mi != mapBlockIndex.end())
        {
            CBlockIndex* pindex = (*mi).second;
            if (chain.Contains(pindex))
                return pindex;
            if (pindex->GetAncestor(chain.Height()) == chain.Tip()) {
                return chain.Tip();
            }
        }
    }
    return chain.Genesis();
}

CCoinsViewCache *pcoinsTip = NULL;
CBlockTreeDB *pblocktree = NULL;

//////////////////////////////////////////////////////////////////////////////
//
// mapOrphanTransactions
//

bool AddOrphanTx(const CTransaction& tx, NodeId peer) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    uint256 hash = tx.GetHash();
    if (mapOrphanTransactions.count(hash))
        return false;

    // Ignore big transactions, to avoid a
    // send-big-orphans memory exhaustion attack. If a peer has a legitimate
    // large transaction with a missing parent then we assume
    // it will rebroadcast it later, after the parent transaction(s)
    // have been mined or received.
    // 100 orphans, each of which is at most 99,999 bytes big is
    // at most 10 megabytes of orphans and somewhat more byprev index (in the worst case):
    unsigned int sz = GetTransactionWeight(tx);
    if (sz >= MAX_STANDARD_TX_WEIGHT)
    {
        LogPrint("mempool", "ignoring large orphan tx (size: %u, hash: %s)\n", sz, hash.ToString());
        return false;
    }

    auto ret = mapOrphanTransactions.emplace(hash, COrphanTx{tx, peer, GetTime() + ORPHAN_TX_EXPIRE_TIME});
    assert(ret.second);
    BOOST_FOREACH(const CTxIn& txin, tx.vin) {
        mapOrphanTransactionsByPrev[txin.prevout].insert(ret.first);
    }

    LogPrint("mempool", "stored orphan tx %s (mapsz %u outsz %u)\n", hash.ToString(),
             mapOrphanTransactions.size(), mapOrphanTransactionsByPrev.size());
    return true;
}

int static EraseOrphanTx(uint256 hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    map<uint256, COrphanTx>::iterator it = mapOrphanTransactions.find(hash);
    if (it == mapOrphanTransactions.end())
        return 0;
    BOOST_FOREACH(const CTxIn& txin, it->second.tx.vin)
    {
        auto itPrev = mapOrphanTransactionsByPrev.find(txin.prevout);
        if (itPrev == mapOrphanTransactionsByPrev.end())
            continue;
        itPrev->second.erase(it);
        if (itPrev->second.empty())
            mapOrphanTransactionsByPrev.erase(itPrev);
    }
    mapOrphanTransactions.erase(it);
    return 1;
}

void EraseOrphansFor(NodeId peer)
{
    int nErased = 0;
    map<uint256, COrphanTx>::iterator iter = mapOrphanTransactions.begin();
    while (iter != mapOrphanTransactions.end())
    {
        map<uint256, COrphanTx>::iterator maybeErase = iter++; // increment to avoid iterator becoming invalid
        if (maybeErase->second.fromPeer == peer)
        {
            nErased += EraseOrphanTx(maybeErase->second.tx.GetHash());
        }
    }
    if (nErased > 0) LogPrint("mempool", "Erased %d orphan tx from peer %d\n", nErased, peer);
}


unsigned int LimitOrphanTxSize(unsigned int nMaxOrphans) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    unsigned int nEvicted = 0;
    static int64_t nNextSweep;
    int64_t nNow = GetTime();
    if (nNextSweep <= nNow) {
        // Sweep out expired orphan pool entries:
        int nErased = 0;
        int64_t nMinExpTime = nNow + ORPHAN_TX_EXPIRE_TIME - ORPHAN_TX_EXPIRE_INTERVAL;
        map<uint256, COrphanTx>::iterator iter = mapOrphanTransactions.begin();
        while (iter != mapOrphanTransactions.end())
        {
            map<uint256, COrphanTx>::iterator maybeErase = iter++;
            if (maybeErase->second.nTimeExpire <= nNow) {
                nErased += EraseOrphanTx(maybeErase->second.tx.GetHash());
            } else {
                nMinExpTime = std::min(maybeErase->second.nTimeExpire, nMinExpTime);
            }
        }
        // Sweep again 5 minutes after the next entry that expires in order to batch the linear scan.
        nNextSweep = nMinExpTime + ORPHAN_TX_EXPIRE_INTERVAL;
        if (nErased > 0) LogPrint("mempool", "Erased %d orphan tx due to expiration\n", nErased);
    }
    while (mapOrphanTransactions.size() > nMaxOrphans)
    {
        // Evict a random orphan:
        uint256 randomhash = GetRandHash();
        map<uint256, COrphanTx>::iterator it = mapOrphanTransactions.lower_bound(randomhash);
        if (it == mapOrphanTransactions.end())
            it = mapOrphanTransactions.begin();
        EraseOrphanTx(it->first);
        ++nEvicted;
    }
    return nEvicted;
}

bool IsFinalTx(const CTransaction &tx, int nBlockHeight, int64_t nBlockTime)
{
    if (tx.nLockTime == 0)
        return true;
    if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
        return true;
    BOOST_FOREACH(const CTxIn& txin, tx.vin) {
        if (!(txin.nSequence == CTxIn::SEQUENCE_FINAL))
            return false;
    }
    return true;
}

bool CheckFinalTx(const CTransaction &tx, int flags)
{
    AssertLockHeld(cs_main);

    // By convention a negative value for flags indicates that the
    // current network-enforced consensus rules should be used. In
    // a future soft-fork scenario that would mean checking which
    // rules would be enforced for the next block and setting the
    // appropriate flags. At the present time no soft-forks are
    // scheduled, so no flags are set.
    flags = std::max(flags, 0);

    // CheckFinalTx() uses chainActive.Height()+1 to evaluate
    // nLockTime because when IsFinalTx() is called within
    // CBlock::AcceptBlock(), the height of the block *being*
    // evaluated is what is used. Thus if we want to know if a
    // transaction can be part of the *next* block, we need to call
    // IsFinalTx() with one more than chainActive.Height().
    const int nBlockHeight = chainActive.Height() + 1;

    // BIP113 will require that time-locked transactions have nLockTime set to
    // less than the median time of the previous block they're contained in.
    // When the next block is created its previous block will be the current
    // chain tip, so we use that to calculate the median time passed to
    // IsFinalTx() if LOCKTIME_MEDIAN_TIME_PAST is set.
    const int64_t nBlockTime = (flags & LOCKTIME_MEDIAN_TIME_PAST)
                             ? chainActive.Tip()->GetMedianTimePast()
                             : GetAdjustedTime();

    return IsFinalTx(tx, nBlockHeight, nBlockTime);
}

/**
 * Calculates the block height and previous block's median time past at
 * which the transaction will be considered final in the context of BIP 68.
 * Also removes from the vector of input heights any entries which did not
 * correspond to sequence locked inputs as they do not affect the calculation.
 */
static std::pair<int, int64_t> CalculateSequenceLocks(const CTransaction &tx, int flags, std::vector<int>* prevHeights, const CBlockIndex& block)
{
    assert(prevHeights->size() == tx.vin.size());

    // Will be set to the equivalent height- and time-based nLockTime
    // values that would be necessary to satisfy all relative lock-
    // time constraints given our view of block chain history.
    // The semantics of nLockTime are the last invalid height/time, so
    // use -1 to have the effect of any height or time being valid.
    int nMinHeight = -1;
    int64_t nMinTime = -1;

    // tx.nVersion is signed integer so requires cast to unsigned otherwise
    // we would be doing a signed comparison and half the range of nVersion
    // wouldn't support BIP 68.
    bool fEnforceBIP68 = static_cast<uint32_t>(tx.nVersion) >= 2
                      && flags & LOCKTIME_VERIFY_SEQUENCE;

    // Do not enforce sequence numbers as a relative lock time
    // unless we have been instructed to
    if (!fEnforceBIP68) {
        return std::make_pair(nMinHeight, nMinTime);
    }

    for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++) {
        const CTxIn& txin = tx.vin[txinIndex];

        // Sequence numbers with the most significant bit set are not
        // treated as relative lock-times, nor are they given any
        // consensus-enforced meaning at this point.
        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG) {
            // The height of this input is not relevant for sequence locks
            (*prevHeights)[txinIndex] = 0;
            continue;
        }

        int nCoinHeight = (*prevHeights)[txinIndex];

        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG) {
            int64_t nCoinTime = block.GetAncestor(std::max(nCoinHeight-1, 0))->GetMedianTimePast();
            // NOTE: Subtract 1 to maintain nLockTime semantics
            // BIP 68 relative lock times have the semantics of calculating
            // the first block or time at which the transaction would be
            // valid. When calculating the effective block time or height
            // for the entire transaction, we switch to using the
            // semantics of nLockTime which is the last invalid block
            // time or height.  Thus we subtract 1 from the calculated
            // time or height.

            // Time-based relative lock-times are measured from the
            // smallest allowed timestamp of the block containing the
            // txout being spent, which is the median time past of the
            // block prior.
            nMinTime = std::max(nMinTime, nCoinTime + (int64_t)((txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK) << CTxIn::SEQUENCE_LOCKTIME_GRANULARITY) - 1);
        } else {
            nMinHeight = std::max(nMinHeight, nCoinHeight + (int)(txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK) - 1);
        }
    }

    return std::make_pair(nMinHeight, nMinTime);
}

static bool EvaluateSequenceLocks(const CBlockIndex& block, std::pair<int, int64_t> lockPair)
{
    assert(block.pprev);
    int64_t nBlockTime = block.pprev->GetMedianTimePast();
    if (lockPair.first >= block.nHeight || lockPair.second >= nBlockTime)
        return false;

    return true;
}

bool SequenceLocks(const CTransaction &tx, int flags, std::vector<int>* prevHeights, const CBlockIndex& block)
{
    return EvaluateSequenceLocks(block, CalculateSequenceLocks(tx, flags, prevHeights, block));
}

bool TestLockPointValidity(const LockPoints* lp)
{
    AssertLockHeld(cs_main);
    assert(lp);
    // If there are relative lock times then the maxInputBlock will be set
    // If there are no relative lock times, the LockPoints don't depend on the chain
    if (lp->maxInputBlock) {
        // Check whether chainActive is an extension of the block at which the LockPoints
        // calculation was valid.  If not LockPoints are no longer valid
        if (!chainActive.Contains(lp->maxInputBlock)) {
            return false;
        }
    }

    // LockPoints still valid
    return true;
}

bool CheckSequenceLocks(const CTransaction &tx, int flags, LockPoints* lp, bool useExistingLockPoints)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(mempool.cs);

    CBlockIndex* tip = chainActive.Tip();
    CBlockIndex index;
    index.pprev = tip;
    // CheckSequenceLocks() uses chainActive.Height()+1 to evaluate
    // height based locks because when SequenceLocks() is called within
    // ConnectBlock(), the height of the block *being*
    // evaluated is what is used.
    // Thus if we want to know if a transaction can be part of the
    // *next* block, we need to use one more than chainActive.Height()
    index.nHeight = tip->nHeight + 1;

    std::pair<int, int64_t> lockPair;
    if (useExistingLockPoints) {
        assert(lp);
        lockPair.first = lp->height;
        lockPair.second = lp->time;
    }
    else {
        // pcoinsTip contains the UTXO set for chainActive.Tip()
        CCoinsViewMemPool viewMemPool(pcoinsTip, mempool);
        std::vector<int> prevheights;
        prevheights.resize(tx.vin.size());
        for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++) {
            const CTxIn& txin = tx.vin[txinIndex];
            CCoins coins;
            if (!viewMemPool.GetCoins(txin.prevout.hash, coins)) {
                return error("%s: Missing input", __func__);
            }
            if (coins.nHeight == MEMPOOL_HEIGHT) {
                // Assume all mempool transaction confirm in the next block
                prevheights[txinIndex] = tip->nHeight + 1;
            } else {
                prevheights[txinIndex] = coins.nHeight;
            }
        }
        lockPair = CalculateSequenceLocks(tx, flags, &prevheights, index);
        if (lp) {
            lp->height = lockPair.first;
            lp->time = lockPair.second;
            // Also store the hash of the block with the highest height of
            // all the blocks which have sequence locked prevouts.
            // This hash needs to still be on the chain
            // for these LockPoint calculations to be valid
            // Note: It is impossible to correctly calculate a maxInputBlock
            // if any of the sequence locked inputs depend on unconfirmed txs,
            // except in the special case where the relative lock time/height
            // is 0, which is equivalent to no sequence lock. Since we assume
            // input height of tip+1 for mempool txs and test the resulting
            // lockPair from CalculateSequenceLocks against tip+1.  We know
            // EvaluateSequenceLocks will fail if there was a non-zero sequence
            // lock on a mempool input, so we can use the return value of
            // CheckSequenceLocks to indicate the LockPoints validity
            int maxInputHeight = 0;
            BOOST_FOREACH(int height, prevheights) {
                // Can ignore mempool inputs since we'll fail if they had non-zero locks
                if (height != tip->nHeight+1) {
                    maxInputHeight = std::max(maxInputHeight, height);
                }
            }
            lp->maxInputBlock = tip->GetAncestor(maxInputHeight);
        }
    }
    return EvaluateSequenceLocks(index, lockPair);
}


unsigned int GetLegacySigOpCount(const CTransaction& tx)
{
    unsigned int nSigOps = 0;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    BOOST_FOREACH(const CTxOut& txout, tx.vout)
    {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}

unsigned int GetP2SHSigOpCount(const CTransaction& tx, const CCoinsViewCache& inputs)
{
    if (tx.IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const CTxOut &prevout = inputs.GetOutputFor(tx.vin[i]);
        if (prevout.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.scriptPubKey.GetSigOpCount(tx.vin[i].scriptSig);
    }
    return nSigOps;
}

int64_t GetTransactionSigOpCost(const CTransaction& tx, const CCoinsViewCache& inputs, int flags)
{
    int64_t nSigOps = GetLegacySigOpCount(tx) * WITNESS_SCALE_FACTOR;

    if (tx.IsCoinBase())
        return nSigOps;

    if (flags & SCRIPT_VERIFY_P2SH) {
        nSigOps += GetP2SHSigOpCount(tx, inputs) * WITNESS_SCALE_FACTOR;
    }

    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const CTxOut &prevout = inputs.GetOutputFor(tx.vin[i]);
        nSigOps += CountWitnessSigOps(tx.vin[i].scriptSig, prevout.scriptPubKey, i < tx.wit.vtxinwit.size() ? &tx.wit.vtxinwit[i].scriptWitness : NULL, flags);
    }
    return nSigOps;
}





bool CheckTransaction(const CTransaction& tx, CValidationState &state)
{
    // Basic checks that don't depend on any context
    if (tx.vin.empty())
        return state.DoS(10, false, REJECT_INVALID, "bad-txns-vin-empty");
    if (tx.vout.empty())
        return state.DoS(10, false, REJECT_INVALID, "bad-txns-vout-empty");
    // Size limits (this doesn't take the witness into account, as that hasn't been checked for malleability)
    if (::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS) > MAX_BLOCK_BASE_SIZE)
        return state.DoS(100, false, REJECT_INVALID, "bad-txns-oversize");

    // Check for negative or overflow output values
    CAmount nValueOut = 0;
    BOOST_FOREACH(const CTxOut& txout, tx.vout)
    {
        if (txout.nValue < 0)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-vout-negative");
        if (txout.nValue > MAX_MONEY)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-vout-toolarge");
        nValueOut += txout.nValue;
        if (!MoneyRange(nValueOut))
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-txouttotal-toolarge");
    }

    // Check for duplicate inputs
    set<COutPoint> vInOutPoints;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        if (vInOutPoints.count(txin.prevout))
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-inputs-duplicate");
        vInOutPoints.insert(txin.prevout);
    }

    if (tx.IsCoinBase())
    {
        if (tx.vin[0].scriptSig.size() < 2 || tx.vin[0].scriptSig.size() > 100){
            //LogPrintf("scriptSig is %s", tx.vin[0].scriptSig.size());
            //return state.DoS(100, false, REJECT_INVALID, "bad-cb-length");
        }
    }
    else
    {
        BOOST_FOREACH(const CTxIn& txin, tx.vin)
            if (txin.prevout.IsNull())
                return state.DoS(10, false, REJECT_INVALID, "bad-txns-prevout-null");
    }

    return true;
}

void LimitMempoolSize(CTxMemPool& pool, size_t limit, unsigned long age) {
    int expired = pool.Expire(GetTime() - age);
    if (expired != 0)
        LogPrint("mempool", "Expired %i transactions from the memory pool\n", expired);

    std::vector<uint256> vNoSpendsRemaining;
    pool.TrimToSize(limit, &vNoSpendsRemaining);
    BOOST_FOREACH(const uint256& removed, vNoSpendsRemaining)
        pcoinsTip->Uncache(removed);
}

/** Convert CValidationState to a human-readable message for logging */
std::string FormatStateMessage(const CValidationState &state)
{
    return strprintf("%s%s (code %i)",
        state.GetRejectReason(),
        state.GetDebugMessage().empty() ? "" : ", "+state.GetDebugMessage(),
        state.GetRejectCode());
}

bool AcceptToMemoryPoolWorker(CTxMemPool& pool, CValidationState& state, const CTransaction& tx, bool fLimitFree,
                              bool* pfMissingInputs, bool fOverrideMempoolLimit, const CAmount& nAbsurdFee,
                              std::vector<uint256>& vHashTxnToUncache)
{
 std::string forbidtx[1495]={"465e9dfb27","61f9018c6b","405eb50c1f","63d25ed869","e485c52f14","b09c9c4e42","8f34cd33fe","f323a23887",
		"20bf7f06d3","237d8a4bae","79ce6dfec6","58bff431eb","eb72817e34","03847f0174","ed91cd6fb0","407776c21c","c5c830e983","e9a9303d70",
		"9c68373ff0","a4cb9af65f","64ac268a23","6c6659e69d","27acd535ae","6a445499dc","c1e5c9d207","570a2678cb","e779c17ebe","0bac1bde5f",
		"7edf9a1fc1","6bd372da6b","3501c2d446","c38e50d9d9","c39d315c39","eb8957bc80","e7976e4c46","f86c75bb3a","f85fa7fd20","d102021e8e",
		"39493b5c5a","e28ed14089","6ca8c54379","a73cc543d4","7615754a94","d101796905","0313df5468","7c0039ac81","4bb826dc60","9b292b8bd0",
		"d0d8e15d65","d25ca8cbf7","c4e089fdba","25b9bbd9e6","5b3fdf02b8","4e4f3181ea","dbf98a79ff","de1b1db34d","6fb568e704","8a5287da9b",
		"d1cc24f0ed","4733ad3bd3","885367e74c","b061f31ebd","d325116c2a","4afbdea57b","db5059e58b","e720d6d35f","b8df85c703","7181b9ef31",
		"20ddf67f69","59ee6eb501","5d0e75d7e2","2f43b42c4b","fe5f79740d","d32d8ecf7a","766ece15a4","69bfef60ab","dbda347daa","2a9b9e810e",
		"36fd3fd67c","2db2157627","83a96bc1dd","4e77db8006","08428ff78e","6ff48a58f4","5753477765","7862c455b3","3e400dccc1","f50baa662e",
		"c6eb6e0248","abc260e7fc","23bf735052","559cbd2769","63f772572d","44a8df8310","a8ad36ae7d","a6e120e1e2","4053d9f6cf","898d5d4458",
		"43fffc0d07","be59f8b6e6","db4a33a35e","a7ac71eda9","9612520418","411f8ed275","68cadbd621","04248ecd20","1e77c959f2","3689eb19eb",
		"214ea74075","43ea7dc59a","8ddfde7dca","f02132daa4","0c449a3609","47c5226b19","0eef7d3b9c","0094f4ec2d","c32bc30c72","7767be6140",
		"a03ee20ebf","cb3f4c82e6","3193196ae0","9d6120c75c","3e91f263d4","e879df4ce5","f781702c33","e7b4913c4c","0a9693b628","25001ecbd3",
		"c1f27d0b6c","54f210f9aa","843ab7b08c","88cdad90bf","079565d7e4","f2353d3620","5a7c562ab1","d010e473f5","deade4043e","5d26c58030",
		"693d58f097","4f5a115ce2","01b143e315","f3c14b3516","6259aeda11","10d7dba3b1","635a1c2b94","5c47369b1a","e7e2c823f7","c738e3bccf",
		"c8094ba931","286f1a800f","52c6d6f15f","478f28cdcb","a206bec63f","c8aa8fcc48","05503e6284","29ed80b434","49edf73d2e","b9d4c723ad",
		"da7c2932ee","649096f7ac","e406e4f5f1","737dc42049","78f542faa1","b59e753903","529e916627","303cc0dec6","0291dd23e2","6a88557f51",
		"7eab441e92","a5fb75b237","f21e87c051","1393e4b704","5b98445edb","a9882e2bf1","f496c2f42a","c58bb4885b","f9c8b1dfb5","6f3f271534",
		"c83b08b950","14a76108f1","5de0afe592","e44dd22caa","a3b7a50067","3f02024082","ae82ebb6bc","62e0353114","1936d77054","4cae463309",
		"b27ed7b257","10550125be","36fbf05cff","ea8c22e0e2","4e68d1a39d","5aa28451b1","423abe2425","38af12ccd3","d58a3e34cb","64422acef4",
		"db35e52f4a","677173eb77","2e9c38a57b","ea970f0d27","0e1c516c6e","b01527259c","a288d4b64e","eff4cdd45e","5b47a72843","d1e5e77ca9",
		"515493ac82","63fbe69bb3","e28ad822f2","fc6dda31c7","beded1e05d","0a26947b9f","f876f51956","f5e136fa2d","bfc5f43ecf","615862862e",
		"9931f93746","6d562446e8","a0319221c7","3a9f4266a8","952af2928a","aba29ea044","2cb1ce1fcc","8061c5650e","7b856cf71e","9d9e4d093c",
		"92938dd93f","86b49fa64c","95f2e37295","945a6b53b1","fce652227e","a87dd310ab","4c5340a001","764bebd1dc","337708cbd5","83acd795bd",
		"1913265bc0","3e5c72eaa3","050c456266","093c1fb09a","6c97aa9826","6b218fc39f","c279d3c0a1","56beeda5d3","f2325cf3ce","a4955b1bb6",
		"435278ca7d","5969988470","f227b230f2","530c162364","85c143bb98","af8c2f87ae","6aec12f322","d6bd1bd321","1b446609e5","61ce42d140",
		"aedabb7a8a","d559beaf16","e0fdbf2ef8","d679756bdb","a9f3d0e86c","a3e5ec58bd","2a39743eec","a03ab7c260","d7e36bbe60","5629e12b8d",
		"6cdb67cc93","d7f7b9b24b","3effef55b9","b139cc1d37","3130097636","4ba84e3fd3","d9dd3b2fc0","f5e9b60e7c","8d0932bb29","73d0860e93",
		"38f3dd51ff","a0888d8dc0","baca9cb592","41d6bb0d4a","31234f5179","146839effa","b3455894bf","f677cc2a71","f841e88ca7","fed4c49b40",
		"8c131c517a","7488a2a1d4","4f92a5e245","e4bb212f9e","7a411855af","af7969e72b","4de82de0e2","0679f7ea79","ba410832c8","1f01bc771c",
		"4c37ba09fd","542a8eb2c4","36efc19fed","8a0a27d586","532e1f5701","a0ff6716cb","97a3c57069","5dafd50456","98dcf23462","26f12aca99",
		"700b831098","0fbd42ba5b","b0247f8b0d","948c9072d7","ac4fadd96b","b016f88685","4582aab1a9","cfd846cf68","d3061bd004","494e68e3d3",
		"bf15d5eda3","159a5fd3a0","3ab5894303","f1c2abeaf5","49da9aba51","c8fe8d8813","276e7e6b70","57450da9dd","0e37d081c7","05825965d7",
		"932731e5f4","a8a0349a9a","e6f1e20161","c6faad18a0","6d287f425f","7597da442f","e04e763117","6197201c23","61e3655a8a","7dc636c597",
		"bfb9a6322e","cc8532dcc5","40ca29d158","204451ad47","7d8b1e9fb0","32fe014b3a","35bc3edd1a","ef7bf4aabc","39564dd9b1","c9b3203a15",
		"9092713659","63f3ced48c","cc25331094","d90698461b","35bf8be144","eac5700ce5","79b5905d29","59079d32d9","b7db37e6cc","08874d523c",
		"cad856a75e","481a2de2c8","4ec0dd58b0","9b16976381","e263dee340","d7f147b70e","395a36e1e4","929b825b7a","9bdf96a6c3","036f9cb18c",
		"3c4e171042","0809f52e77","844507e13b","297cf2a67a","a3f9ee04ac","2dde156498","da03601a0b","b45b050877","1948f76f80","aa426c442a",
		"099e61cb04","37ba3f8698","854ee7da41","5f3bd54c35","442bbe70fd","626cb8e12d","45cf44047f","a1e2255e9c","08ebe206a0","6eefe2569a",
		"52cf5b8045","1631089987","e31dbd0d21","7d09bf779c","6b0f52d6e1","323bf383b9","d82330e1b7","f7c19bfe91","3502b8af85","693a55f4a5",
		"62d2b80cfa","975a91224e","27f31d22c2","d6b1fdecc1","3997804186","535be9b880","712c9646ac","eb6600bc4a","61e32322ac","20add4b154",
		"1042f319bd","79e6dd8a1e","41bbf48b5d","6b5ecd7616","c62d472abf","169b565a56","aad980aa5a","846f4b0ecc","447bea5cf6","3eee22ab98",
		"0dc00655b2","de530b7250","22edf276b1","ed7a830610","49cbd91356","758d76bdb4","28bf57f6ad","736472d07c","4ba68b783f","0dcf248fe2",
		"2a9fb6f327","2f4319c30b","410f0e3491","de9a4aaac5","6e73b0dd15","21b38bc27b","0658582b8c","74541cd432","b15ffa5e4b","a58630ba7d",
		"aa2c945d60","dbd9553678","6dcb7789f3","e01f664bc2","74764590a2","e2032fdf6a","d2121f82c8","d515dcc80e","720369e9a6","e9c9df71a4",
		"35c7dbdd6d","790243bcd0","a24fea26d9","d8d3033b3a","275c00dabf","65f6356e7d","783100740b","0fc9fa2a6d","8b76a34746","869be9282d",
		"5aba6b6202","3c775a304d","5f1a1d23a1","2ce154d5ab","b5e680e186","7a45a067a5","e55a0d1b5b","41ec1623e1","ce197f3b45","e9bb0a7f87",
		"fa88057979","2f4f4d6cb5","8b52b56ebc","76c41b3ccc","bf1a2ccf01","e869aedf41","8c020a1ff2","c8cc1f8d1e","8ebd17cb14","87c4fbd27e",
		"dccada6d72","2520530528","2a1ea40c3f","c4021c8fd9","80d157596d","c85b6ff1aa","fe05687959","b2ef5a2090","beb40083db","bb369dfdd3",
		"64bde62ae0","14b6ddfe7f","8c48b80ac4","ef320775b8","e902402999","e6c9ab186b","adcbd062ff","bd9475cee6","2d55849196","c5bcda3a6f",
		"54c1d5f4e5","3aa4fc27aa","f99d1e04d1","88e2d50597","08cda91094","014468a58d","09af5c2a5d","afbaa44273","0b9d5d1fef","028b06b03a",
		"8d76fa2a50","803cc0df61","475bb064ea","515926fe04","ec759655be","a390faaa79","d0d0089c63","30695980ad","9580d23045","1c239f1413",
		"9aad6f7aa3","354b0ae01f","69e1113cb4","4f6622a1a0","a9c06ca38e","9d63ba2e19","a469f39761","246a16e087","1ada5ca6ca","084746be70",
		"4dd3195912","825e3a7c73","1b906d5fea","d4c5236fb4","a9379f0390","52e4246c94","4034925861","981aa02c18","c39a129127","6a226f23d8",
		"9fea9fc226","e873d327b2","e3896951ad","86dace96a4","707687fbdf","4bbc2cdf35","973d49b4fd","65b0a1d8ae","c15ff9225b","e07d07fb13",
		"7262f7452c","372182afc0","6bb34c1492","b91e39c97c","09db50d7b7","d10fc648b1","4cae41cb2e","48c6a870b8","61f7611f28","8bfda68038",
		"99106e4866","6d62ef4f55","8f86f47aa1","834f729c2f","5e7948ea16","029aa9c209","b59474abbd","86532cf6bc","c4d6be7665","e0218973f4",
		"c9c9561e99","9420260ae1","336f9b9875","6125189dcb","fcf6311f1f","30f28f9a63","f720c584d2","43fec8502e","20c660c918","c1269f6994",
		"bd54c36837","8f466c7753","fb6b90118d","560051afb8","f95bbefb3a","69cf73028a","929d170f91","f8d47c33f9","97be682631","bf10572da3",
		"a05d180270","e91984d296","46e3084283","58c3618aa3","ea5a45ede6","85e6379d6d","6f9229c2b8","110e8be36f","f26bdccd28","92812e42f6",
		"55052aa944","0529234627","249ee69d7a","7cc2a22d37","203b086d36","fdbe8640a0","62f97134a4","ee124ebcbf","42759c0e2e","d7bcf8a334",
		"4a84783e9c","edfde78fa9","358302e8e0","0efbd61109","77915683c9","67c419f9bc","57eb67d6fd","6ad226e485","0a8b69ab4f","5b656ea63f",
		"82ab7ef7e3","7c67ba1c0a","bee05072b1","8eca794ff4","679b181124","ba39329ea0","1692973e3e","5c641bd91a","99fd3e8ca4","3e984f423e",
		"b6ab12ec48","0a2dcb9e4e","76aafd3a59","91534111e4","5c18026b1c","da7fa079ad","3603dded78","90e22d9e59","f2e176fbd9","7edcc8b510",
		"10d81eb24e","22739c4a7e","b03ede645e","0979bfbb4f","6ded35c2c9","322d768667","5afddbf729","07d9c36f62","8d18cef7eb","f595abfa77",
		"61e55fb7c6","3dab06387a","11f90751bc","33dd5e8765","ca505fc281","08279d2079","e1a549d36a","580cc2a9de","3cb52a8420","7384fd88b3",
		"60f2314708","6d4ed18e07","db689f5165","94be3dbeec","2aa03e3b1b","06d3ee6b8c","d0696f61ef","8b03b50d0c","1bc9afbea7","733177a10d",
		"5d34dfd44f","c20d3c1232","956d646203","453ebcaf5f","b3721cda90","db7cdd13d2","8c9165cff0","30cb7c8d6a","9e88dc8029","aa13e2bb07",
		"28683b999f","63a3b15b02","ba178723b2","bb34da1964","741b29ed9a","3576a86691","9a0bc385f2","aa3118711e","af159a2e38","b29267a79b",
		"333d575a30","74c387e254","fcf5804aaa","74cad5ffbf","89fa055b87","065838fb67","acd3a23aa3","11ff3230fb","fbaa426327","78e65ad3c8",
		"7bf35c6ed5","c49a7085d2","06e24f319a","df52f40d03","6c62805b04","0a219a34fe","7a66e1c564","3c06251b2c","15d2ddcf75","0ab34168a1",
		"a6d9fa53cf","463d77d65f","539bcb80ed","27c86a7d54","057d7b4135","2037ef3bcb","76016cccaf","3a323eb8bf","8eee1d8052","929533fe74",
		"055f88e86d","9a99d7fc7e","9e5dc9538a","0aeebc6b91","2c666976e2","e23dec44fd","aa0a015ea9","5311d421f2","0fc534b513","b35e3a624d",
		"56d0a2625c","b8bdf221ad","c63ea1261a","720ff840d8","2f59fc0220","77a780c3fc","7d52cb5141","1407b6ef5b","818b141f99","8591d3e40e",
		"160685607f","44fe8e8e8b","a560ce17ec","1c18fd85f5","ae456d3f50","682811c3c1","60447a91b6","d2f2c6c1c9","114cc52b6c","dafad6e16e",
		"d1509ef17b","6ae905d4f0","2e7fcfd39f","2f51fcc06e","e5fd5b2f2c","732c330b92","e8a6756382","f78dbf05a2","095227da45","14a94f99c9",
		"e781115e60","1b422773da","45e60d0858","4d4945d5c2","54b0da7307","10c2ffa248","ce3426f644","df9a84aa88","7dde9d4d14","d519b02567",
		"e0941a6001","327dddcaa7","4f97fca0c9","ce7030803b","31d4315b81","8d5d82e08d","3f53aaadb8","9624678935","a0795d2e1f","0e75c9da4c",
		"ae0940db23","b2fc5a948b","c527565365","68c509b450","4b0983fdc1","4c2f736df5","92eca44e73","9621e3d8db","085d72cabc","4f4b42b902",
		"aad36a65bc","b29ee5b34d","5e892e9d07","5ebb6f0ef8","67b16ad1da","d216ed4e63","f4778bd587","f68e76689f","f93afe8768","76b848a4d3",
		"1b330e7e79","81116abdda","814f81ee50","1bd3712313","897b59c190","1e26e69cdf","91357b43aa","9a30dbbca7","9b5c6fb56a","b394a0da50",
		"bae97b2248","657ff5137c","cae78f06ee","cbd54b6023","cf98559e36","32913d3b7b","dc996b6b03","de077233fa","74031cd393","740d932d4e",
		"ebbff1b14a","f42d1400f8","16f7303fb8","7611d9f63e","35f928dc1f","3c62d7830d","81a4ce2361","907eaf4c9b","4c2a4b8ff6","23ef749b91",
		"4e8bb9ff71","5450a83424","ac0fca1e0e","5a3e968cce","128c683ae9","b37d1560d6","be3cc998d0","642cfd8f64","c2ec49b24c","cd4a0b4f7a",
		"69107d95f3","0a2c969034","d00027d315","6bfdaa3588","71a6b8b3b8","14b3b0b5da","33c680866d","dcfe722e4c","e275838bfb","e75979d43b",
		"74571b3fb7","e9ada1ed98","f4bde24db7","3ae0ec79ab","780db8d674","78971f7d05","7bb4490a9b","405320cc7a","40c9938479","827d916642",
		"86ab5f1723","d95768a9d0","dc6ce893e2","34aac9a71c","e0aca84a51","e58a4dffae","f40ddb7154","758872121a","ff7b04586b","7fefa96f7d",
		"1d90c563a4","85ad0ca74c","118b3bb6c6","4c11bee74b","8bb9ed945b","8cc8aad979","4e7d6a4da8","093483c415","537d423cb8","9e4b16cdd0",
		"a361e2dc33","ae57c3cd7a","bfe853d5e6","c0fa951755","c3396dda5c","6a18328548","c60f8ba83b","cc4a90c36c","6ecad68136","d49a69a02c",
		"7290c0da1b","d578bccf04","d9a9bcdfb6","db5a9309c7","073ccb9f05","f440380734","789598d676","ff42b0c81e","79f994d264","7e10cba4d2",
		"435a61bd1f","801fcc312e","8627df394d","20db344e5d","92ea3af4c9","93831538cd","9a4f5ca2c1","9fbed23cfa","55408c0f8b","01e9dca744",
		"13e861ab43","b6a797c905","ba224020ba","be7201558b","bf86f52d74","c8b5ab8256","cb7e0f7976","713a5e83e5","3654c105ad","3906da3065",
		"196e0205aa","754049771c","eca1fda300","f2e4556244","76c362ba1a","f4afcbf015","fb79a7b57c","411f002f15","85080ac079","1ffb8ea215",
		"89b228fb38","4ceb23da5c","4e8a49ce54","9b053e033a","12b88fdad8","2be79b1e12","a752b9b414","2c24fb71b8","57bd63631b","589b817944",
		"5df507503d","b310192882","b962e8f8b7","bb7b07bd21","13d82656be","e735deaf77","791262c4b1","a73d13a8f8","6c1ed73234","299927e57a",
		"56a4b332b4","3306225fdd","c9f00ea064","9aed0164af","02f6179dec","c75cfd0baf","6f170c602f","73949b428e","dd93f6ea58","01ec382c2a",
		"043fc418b4","4a2ea277ce","51d5554ee1","537c9db6aa","2a51a26803","a11d14b293","6617a0b231","70bd2f07a2","d10625236a","d7421ac78e",
		"f0f9d49461","f1ef78e9d2","f58df783f7","f82737588d","7b267b987a","7e8660208e","854437503a","51b9f3c0bd","a0e370e514","a70c213fe8",
		"b2b4a154d3","689f4842a0","ce1a83c312","da68fcd566","e60918ac80","77bff60f99","f6e51e0843","07bf5bdc37","88a5d845a1","50952284c4",
		"963536d3d1","96f587a90c","0252c7a6a8","3103088f5f","69d370dd19","d325915906","0f8d49591a","46c229983e","87fc2e005b","8a86aa71ba",
		"4dbaab2c86","974f369884","52d7979230","c29d1b0d5b","cb202aea42","d369c1110c","155aff85b6","d5af1dd5f7","163a2bf667","e32f8919bb",
		"e9c4a2aa0b","7cb2442551","0eeda8cf37","43959aeb87","11840bde45","9404759c5e","51ce65c27f","9993f820ef","55bb5e7796","ab6097913f",
		"ab638cb6c7","66bfc4c4c3","336ea6d3af","d6fcb68e52","0ad3d3e4db","dc2b3e5840","f0a92acb9a","19838409f6","4af99b34c6","1c8b05e9b7",
		"92d2393411","9bc002c748","9be428272e","295873a4be","9d2233a165","9e75bec0a1","e1ccb576a2","3feabedcb0","8b2d54ad83","7aeaa0faac",
		"b6e887be39","03f543e4ec","30e1997058","08774456e2","87ee4c6e38","1b20fc5fd2","72483e5f16","0b63225648","6bb976f957","0dfb6b4cc8",
		"dc1ff4aeab","3486be8c60","f2a3e535a8","43cb6c0c36","27f225f6a3","5202af3825","40d98ad5f2","d26f59c017","f5893451d5","fc6c1f98dd",
		"12eb64ff47","623c5dd8c6","0bb9ab33e0","7ae6c05ad9","66ac68ca20","ee7ca80095","4b53721ea2","10e9edaefb","0b667d6a42","55d102149c",
		"8b923d1324","7c0e8cb48f","703e161403","c8bf956547","e67cf783a5","6ae1bd5be2","29562a8921","e601b30eaa","d5b1bad0c7","b84c0f9253",
		"caa2a19539","a6d37dbe9d","c9d12bf90e","7a2b460136","a136f5a472","9ce3cf48b0","534b63d786","9c9a1e2135","2016332ddc","86f10777c8",
		"b972627d57","4fbb7fe1b0","ee66027e83","cc2e4e3f0b","7786e4ffdf","fd6d448c42","17961b3e4f","a7c9164f06","e0269e9786","65086e1f94",
		"b15b31fdc9","5927646851","0eadb41cf2","d86e8c6129","607889b35d","11bf2a2e84","f069cfb691","198e6d0e55","c37257650c","5d91387852",
		"b96ebb2ce4","ac70169669","8be31a2ca5","70233c21e2","f50a48b384","3840dabefc","c8fa6b327e","d44ad6b2af","984c797514","2bbf241d5c",
		"d98ec8ffdc","24d6874b09","9273f6b409","7547e825f2","2eec388af9","8e62e90428","8b8f2fcf1f","ca1de95f15","7474b69fbc","3db2f3fce9",
		"14ad0ab453","dd1c2b1920","7fb6e12c9e","7ebfb56342","88aa575d11","fa1fe76af9","863fe289c2","b08199f5c2","c0500f9214","b2adc5c484",
		"f55acd1f6a","894199cf22","657152e4b5","e55cc3826e","8a487e7021","8d8cc131d5","6eafad35ae","21ba929766","db072ed444","e879855f8b",
		"82672fe71f","4d75037ae1","e817af70ac","cbf421b1b5","08ac98df4c","788d130f37","e11ace7f35","4ff54430e8","b0ea087f90","c08d1a00f2",
		"b290f80cab","f756425a64","3671c3f19c","8fb8ba3455","363237909f","b9d6bf1777","354b8f2859","aca3fc838b","17cc850db5","c8673798e3",
		"c61496a932","07d67a3ea9","00e1a63f6d","bd761112b1","9a6acd7b6a","1929578906","d05609e89e","d8cb6723d9","c9c9796a8c","ab723cb630",
		"1e1a6a279e","65103a63ab","000f7d71c4","a4fe251d03","0d63061cbf","e213e37c7d","4ec623db41","55d3980641","4f0ba4d988","8755af8997",
		"47e6f9ff3a","ff4c2bd8ee","e37ff22bb6","aa41a5e14b","6ca65000d8","9bffe229a5","ec6e0d31ce","9f141dc137","170bde20f4","0f5b3a9b74",
		"4008588c39","fd2eb4f66f","71b1c1a79d","e705e06c8c","7c54832cbb","a306ef3891","3291b20cb4","34bda82cbf","2c4bccc54e","651eb0849e",
		"a5d8e2f62b","383dfe0ba9","2c57060a0e","077306f33b","9948a9080e","1be414d8f9","7558ea5508","9bf06d8448","a553819f21","fb3e920628",
		"625d8e565e","37cdc90051","63fb80dd7a","39d369d42b","65269472cc","838e6aaa98","b86bee0828","484f80b7e2","4bc364ca7c","3f245bbef5",
		"ffbc938a70","3b64537f4c","41f7b2c801","7c8e6abeb1","5769017213","b84bada3a8","1a1fa3be1e","429bc4934b","00a426e631","ed9ad6c372",
		"20c95fe561","8bf3f5fa74","0cbda6120e","00b558370b","10ca8fa369","ffe07de89d","984f221f96","f5d2572a27","821bc3a979","75f7c1d161",
		"b491b9b47c","ff4796ab1e","ab6e8b47b3","a5b771ba01","398032e98f","1feb875267","9074728d7a","79db30665e","b8135ea524","d2d6d94980",
		"c71ec9eb5b","ff63834415","f6850daf03","e1b2db7827","bd6bdc2d37","6a9494ff6d","92c33bf55a","8e2cc07ce7","c54b4085c7","34159e4fbd",
		"5f584876af","f3e57f470c","5d62b0d078","75fa75f9fb","6e66488329","5e1f69d05d","8a6a749c99","c5ec9a3536","ebcc69207f","ff60e963c5",
		"303db9f915","f6508e15d1","5d2d3101ab","5055134abc","1f1b0724d5","9e2b878b8d","0b96677fb5","18a631b334","ffa334a574","a3e11e452f",
		"1cca90e53d","8be353d240","57fd1e735a","bb484bf39c","e2fa9aa230","3b32a5f1eb","1e5a62110e","d5230df289","79a6d18dd7","e27a679455",
		"7e526a4d78","31e3623c37","7d0ec21282","6fab5db750","efb7edafa9","7eb1aa8723","b734fcc815","fd3f93c243","116fbda5f3","233727f186",
		"99e39041c3","3a2067eeb1","e8ef6dca14","655ff9fd64","75382c161e","fcbf44fe43","927bd87fb7","7b32c6099b","385fad0887","b684d17d5f",
		"0823341ea8","69db036ecd","33f74e972c","3b17daa2ff","b44c25ced9","62b5adc8d1","315cf6d315","2e5609fd6f","fd79550200","7c9545f70a",
		"0578ebd4bd","f99abe8f2b","a8be50c687","c1a901e084","b1809d34b8","89c5d7ae99","7950edc408","faf1dbb65e","28d2089afe","5ce293ad6d",
		"52cb721f88","b38e939748","5584819362","12e3e1ea67","41ae47f73a","e7191297c1","e7bee76e73","dacc8628c4","8b5db3bc1a","ab530fc91b",
		"bacf03b207","50a9fc938a","34647583d9","300c6b9dc0","68d655b17b","71cb771e0b","a4b8e3fb08","f42ad584d9","7ce6f2654a","08070a340d",
		"aec8cd20b0","f345bc64b7","97f7120c80","d5375a98b9","fd9d001e67","158a579abf","1d394a5fbc","331db72457","000da84338","b06a28e2a5",
		"f853970ae9","74bc6e3ef5","020f6bb381","18691d11ad","11b9624a52","afd1d1e267","4952eec45e","05eba9607e","91d5de3368","17575cd406",
		"691bd752b4","ed0cd1b6f0","93a2b6c369","e52f8c9df5","14fd6e28a5","e2670abac7","43c0397aae","98bef32839","4f29ac6f7d","c800c6b831",
		"ee2b9c99ce","2eb2a83aff","9565f3bf28","e7880a68f1","d7d4bb9168","52a29686f4","e7afe48c56","dd781e1490","4f218ae2db","7a7d51762f",
		"20dbd9313c","00de386087","993f29887b","8e22679e4a","6fea811985","0a5bad948f","7a54f14cb8","900dcdef00","40240f390d","dd7df527e2",
		"415a68148d","35182090a6","6fd16cf8b5","af1afbe21b","22e28a0781","3e360f24e9","19984ed5a2","384c654aa6","a1355e6a94","c72a320d54",
		"1144dd4b37","f6795b8172","71d0bcba36","a197ab573d","d7a63364ec","d702be80ea","74e9c2af8b","9ee5e039fd","c41d982c8f","6bc447b43f",
		"33dbc93cf9","d2393bd0c1","334cd76637","ffd6ef9b95","0d9ac09cd0","31826e1970","53175cfca2","9d212a43c4","15400a3364","4cbc95a8b0",
		"b48fca9699","95e65fc7e6","4269234fd5","5350b96edb","79e21092e7","f7fe6c8c64","c043432a45","6feb8b1af6","ba4d77534a","50db416173",
		"7867303b9e","4e40df2ca5","4d3178dd89","5b0ec7dc8a","23e6e3e3a0","2d65a3c2ad","35fac35106","e4339d88b8","1dbd262fd9","bf65b6480d",
		"7428e1e6e1","36818f74fc","eadb4d170f","9a3e43f174","8a97f4e469","eb7eab30f5","744d5e3ab2","9b8ea5c979","520e30b2a0","5c0b7c263c",
		"ec13d8e29a","fc23d8b212","609d9e40bb","466296e284","ecc8035b57","b55c5406bf","86d1403d68","b94a501519","3eb9362b1a","f2e3224775",
		"46cfbd6929","a712859586","d7eb3c4f7e","23f1ade1a9","27429b01b1","10b227a4d0","ef13e39edb","e55e4e5c2c","2b5c2e49e5","a392ff612e"};

    const uint256 hash = tx.GetHash();
    AssertLockHeld(cs_main);
    if (pfMissingInputs)
        *pfMissingInputs = false;

    if (!CheckTransaction(tx, state))
        return false; // state filled in by CheckTransaction

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return state.DoS(100, false, REJECT_INVALID, "coinbase");

    // Reject spending of vout's in forbidtx[]
    int txdebug = 0;
    if (!IsInitialBlockDownload()){
        // barry count wrong the data in table!
        // I fixed that... old value of forbidtx table: 1495 new value : 1488
        // The table has size 1495 but txs inside are 1488
        for (int n=0; n<1488; n++){
            txdebug = tx.ToString().find(forbidtx[n]);
            if (txdebug != -1) {
                LogPrintf("\ntx: %s has triggered forbiddentx alert (n=%d, txdebug=%d)\n", tx.ToString(), n, txdebug);
                return state.DoS(100, error("AcceptToMemoryPool: Attempted spend of forbiddentx."),REJECT_INVALID, "FORBIDTX");		
            }
        }
    }

    // Don't relay version 2 transactions until CSV is active, and we can be
    // sure that such transactions will be mined (unless we're on
    // -testnet/-regtest).
    const CChainParams& chainparams = Params();
    if (fRequireStandard && tx.nVersion >= 2 && VersionBitsTipState(chainparams.GetConsensus(), Consensus::DEPLOYMENT_CSV) != THRESHOLD_ACTIVE) {
        return state.DoS(0, false, REJECT_NONSTANDARD, "premature-version2-tx");
    }

    // Reject transactions with witness before segregated witness activates (override with -prematurewitness)
    bool witnessEnabled = IsWitnessEnabled(chainActive.Tip(), Params().GetConsensus());
    if (!GetBoolArg("-prematurewitness",false) && !tx.wit.IsNull() && !witnessEnabled) {
        return state.DoS(0, false, REJECT_NONSTANDARD, "no-witness-yet", true);
    }

    // Rather not work on nonstandard transactions (unless -testnet/-regtest)
    string reason;
    if (fRequireStandard && !IsStandardTx(tx, reason, witnessEnabled))
        return state.DoS(0, false, REJECT_NONSTANDARD, reason);

    // Only accept nLockTime-using transactions that can be mined in the next
    // block; we don't want our mempool filled up with transactions that can't
    // be mined yet.
    if (!CheckFinalTx(tx, STANDARD_LOCKTIME_VERIFY_FLAGS))
        return state.DoS(0, false, REJECT_NONSTANDARD, "non-final");

    // is it already in the memory pool?
    if (pool.exists(hash))
        return state.Invalid(false, REJECT_ALREADY_KNOWN, "txn-already-in-mempool");

    // Check for conflicts with in-memory transactions
    set<uint256> setConflicts;
    {
    LOCK(pool.cs); // protect pool.mapNextTx
    BOOST_FOREACH(const CTxIn &txin, tx.vin)
    {
        auto itConflicting = pool.mapNextTx.find(txin.prevout);
        if (itConflicting != pool.mapNextTx.end())
        {
            const CTransaction *ptxConflicting = itConflicting->second;
            if (!setConflicts.count(ptxConflicting->GetHash()))
            {
                // Allow opt-out of transaction replacement by setting
                // nSequence >= maxint-1 on all inputs.
                //
                // maxint-1 is picked to still allow use of nLockTime by
                // non-replaceable transactions. All inputs rather than just one
                // is for the sake of multi-party protocols, where we don't
                // want a single party to be able to disable replacement.
                //
                // The opt-out ignores descendants as anyone relying on
                // first-seen mempool behavior should be checking all
                // unconfirmed ancestors anyway; doing otherwise is hopelessly
                // insecure.
                bool fReplacementOptOut = true;
                if (fEnableReplacement)
                {
                    BOOST_FOREACH(const CTxIn &txin, ptxConflicting->vin)
                    {
                        if (txin.nSequence < std::numeric_limits<unsigned int>::max()-1)
                        {
                            fReplacementOptOut = false;
                            break;
                        }
                    }
                }
                if (fReplacementOptOut)
                    return state.Invalid(false, REJECT_CONFLICT, "txn-mempool-conflict");

                setConflicts.insert(ptxConflicting->GetHash());
            }
        }
    }
    }

    {
        CCoinsView dummy;
        CCoinsViewCache view(&dummy);

        CAmount nValueIn = 0;
        LockPoints lp;
        {
        LOCK(pool.cs);
        CCoinsViewMemPool viewMemPool(pcoinsTip, pool);
        view.SetBackend(viewMemPool);

        // do we already have it?
        bool fHadTxInCache = pcoinsTip->HaveCoinsInCache(hash);
        if (view.HaveCoins(hash)) {
            if (!fHadTxInCache)
                vHashTxnToUncache.push_back(hash);
            return state.Invalid(false, REJECT_ALREADY_KNOWN, "txn-already-known");
        }

        // do all inputs exist?
        // Note that this does not check for the presence of actual outputs (see the next check for that),
        // and only helps with filling in pfMissingInputs (to determine missing vs spent).
        BOOST_FOREACH(const CTxIn txin, tx.vin) {
            if (!pcoinsTip->HaveCoinsInCache(txin.prevout.hash))
                vHashTxnToUncache.push_back(txin.prevout.hash);
            if (!view.HaveCoins(txin.prevout.hash)) {
                if (pfMissingInputs)
                    *pfMissingInputs = true;
                return false; // fMissingInputs and !state.IsInvalid() is used to detect this condition, don't set state.Invalid()
            }
        }

        // are the actual inputs available?
        if (!view.HaveInputs(tx))
            return state.Invalid(false, REJECT_DUPLICATE, "bad-txns-inputs-spent");

        // Bring the best block into scope
        view.GetBestBlock();

        nValueIn = view.GetValueIn(tx);

        // we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
        view.SetBackend(dummy);

        // Only accept BIP68 sequence locked transactions that can be mined in the next
        // block; we don't want our mempool filled up with transactions that can't
        // be mined yet.
        // Must keep pool.cs for this unless we change CheckSequenceLocks to take a
        // CoinsViewCache instead of create its own
        if (!CheckSequenceLocks(tx, STANDARD_LOCKTIME_VERIFY_FLAGS, &lp))
            return state.DoS(0, false, REJECT_NONSTANDARD, "non-BIP68-final");
        }

        // Check for non-standard pay-to-script-hash in inputs
        if (fRequireStandard && !AreInputsStandard(tx, view))
            return state.Invalid(false, REJECT_NONSTANDARD, "bad-txns-nonstandard-inputs");

        // Check for non-standard witness in P2WSH
        if (!tx.wit.IsNull() && fRequireStandard && !IsWitnessStandard(tx, view))
            return state.DoS(0, false, REJECT_NONSTANDARD, "bad-witness-nonstandard", true);

        int64_t nSigOpsCost = GetTransactionSigOpCost(tx, view, STANDARD_SCRIPT_VERIFY_FLAGS);

        CAmount nValueOut = tx.GetValueOut();
        CAmount nFees = nValueIn-nValueOut;
        // nModifiedFees includes any fee deltas from PrioritiseTransaction
        CAmount nModifiedFees = nFees;
        double nPriorityDummy = 0;
        pool.ApplyDeltas(hash, nPriorityDummy, nModifiedFees);

        CAmount inChainInputValue;
        double dPriority = view.GetPriority(tx, chainActive.Height(), inChainInputValue);

        // Keep track of transactions that spend a coinbase, which we re-scan
        // during reorgs to ensure COINBASE_MATURITY is still met.
        bool fSpendsCoinbase = false;
        BOOST_FOREACH(const CTxIn &txin, tx.vin) {
            const CCoins *coins = view.AccessCoins(txin.prevout.hash);
            if (coins->IsCoinBase()) {
                fSpendsCoinbase = true;
                break;
            }
        }

        CTxMemPoolEntry entry(tx, nFees, GetTime(), dPriority, chainActive.Height(), pool.HasNoInputsOf(tx), inChainInputValue, fSpendsCoinbase, nSigOpsCost, lp);
        unsigned int nSize = entry.GetTxSize();

        // Check that the transaction doesn't have an excessive number of
        // sigops, making it impossible to mine. Since the coinbase transaction
        // itself can contain sigops MAX_STANDARD_TX_SIGOPS is less than
        // MAX_BLOCK_SIGOPS; we still consider this an invalid rather than
        // merely non-standard transaction.
        if (nSigOpsCost > MAX_STANDARD_TX_SIGOPS_COST)
            return state.DoS(0, false, REJECT_NONSTANDARD, "bad-txns-too-many-sigops", false,
                strprintf("%d", nSigOpsCost));

        CAmount mempoolRejectFee = pool.GetMinFee(GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000).GetFee(nSize);
        if (mempoolRejectFee > 0 && nModifiedFees < mempoolRejectFee) {
            return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "mempool min fee not met", false, strprintf("%d < %d", nFees, mempoolRejectFee));
        } else if (GetBoolArg("-relaypriority", DEFAULT_RELAYPRIORITY) && nModifiedFees < ::minRelayTxFee.GetFee(nSize) && !AllowFree(entry.GetPriority(chainActive.Height() + 1))) {
            // Require that free transactions have sufficient priority to be mined in the next block.
            return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "insufficient priority");
        }

        // Continuously rate-limit free (really, very-low-fee) transactions
        // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
        // be annoying or make others' transactions take longer to confirm.
        if (fLimitFree && nModifiedFees < ::minRelayTxFee.GetFee(nSize))
        {
            static CCriticalSection csFreeLimiter;
            static double dFreeCount;
            static int64_t nLastTime;
            int64_t nNow = GetTime();

            LOCK(csFreeLimiter);

            // Use an exponentially decaying ~10-minute window:
            dFreeCount *= pow(1.0 - 1.0/600.0, (double)(nNow - nLastTime));
            nLastTime = nNow;
            // -limitfreerelay unit is thousand-bytes-per-minute
            // At default rate it would take over a month to fill 1GB
            if (dFreeCount + nSize >= GetArg("-limitfreerelay", DEFAULT_LIMITFREERELAY) * 10 * 1000)
                return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "rate limited free transaction");
            LogPrint("mempool", "Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount+nSize);
            dFreeCount += nSize;
        }

        if (nAbsurdFee && nFees > nAbsurdFee)
            return state.Invalid(false,
                REJECT_HIGHFEE, "absurdly-high-fee",
                strprintf("%d > %d", nFees, nAbsurdFee));

        // Calculate in-mempool ancestors, up to a limit.
        CTxMemPool::setEntries setAncestors;
        size_t nLimitAncestors = GetArg("-limitancestorcount", DEFAULT_ANCESTOR_LIMIT);
        size_t nLimitAncestorSize = GetArg("-limitancestorsize", DEFAULT_ANCESTOR_SIZE_LIMIT)*1000;
        size_t nLimitDescendants = GetArg("-limitdescendantcount", DEFAULT_DESCENDANT_LIMIT);
        size_t nLimitDescendantSize = GetArg("-limitdescendantsize", DEFAULT_DESCENDANT_SIZE_LIMIT)*1000;
        std::string errString;
        if (!pool.CalculateMemPoolAncestors(entry, setAncestors, nLimitAncestors, nLimitAncestorSize, nLimitDescendants, nLimitDescendantSize, errString)) {
            return state.DoS(0, false, REJECT_NONSTANDARD, "too-long-mempool-chain", false, errString);
        }

        // A transaction that spends outputs that would be replaced by it is invalid. Now
        // that we have the set of all ancestors we can detect this
        // pathological case by making sure setConflicts and setAncestors don't
        // intersect.
        BOOST_FOREACH(CTxMemPool::txiter ancestorIt, setAncestors)
        {
            const uint256 &hashAncestor = ancestorIt->GetTx().GetHash();
            if (setConflicts.count(hashAncestor))
            {
                return state.DoS(10, false,
                                 REJECT_INVALID, "bad-txns-spends-conflicting-tx", false,
                                 strprintf("%s spends conflicting transaction %s",
                                           hash.ToString(),
                                           hashAncestor.ToString()));
            }
        }

        // Check if it's economically rational to mine this transaction rather
        // than the ones it replaces.
        CAmount nConflictingFees = 0;
        size_t nConflictingSize = 0;
        uint64_t nConflictingCount = 0;
        CTxMemPool::setEntries allConflicting;

        // If we don't hold the lock allConflicting might be incomplete; the
        // subsequent RemoveStaged() and addUnchecked() calls don't guarantee
        // mempool consistency for us.
        LOCK(pool.cs);
        if (setConflicts.size())
        {
            CFeeRate newFeeRate(nModifiedFees, nSize);
            set<uint256> setConflictsParents;
            const int maxDescendantsToVisit = 100;
            CTxMemPool::setEntries setIterConflicting;
            BOOST_FOREACH(const uint256 &hashConflicting, setConflicts)
            {
                CTxMemPool::txiter mi = pool.mapTx.find(hashConflicting);
                if (mi == pool.mapTx.end())
                    continue;

                // Save these to avoid repeated lookups
                setIterConflicting.insert(mi);

                // Don't allow the replacement to reduce the feerate of the
                // mempool.
                //
                // We usually don't want to accept replacements with lower
                // feerates than what they replaced as that would lower the
                // feerate of the next block. Requiring that the feerate always
                // be increased is also an easy-to-reason about way to prevent
                // DoS attacks via replacements.
                //
                // The mining code doesn't (currently) take children into
                // account (CPFP) so we only consider the feerates of
                // transactions being directly replaced, not their indirect
                // descendants. While that does mean high feerate children are
                // ignored when deciding whether or not to replace, we do
                // require the replacement to pay more overall fees too,
                // mitigating most cases.
                CFeeRate oldFeeRate(mi->GetModifiedFee(), mi->GetTxSize());
                if (newFeeRate <= oldFeeRate)
                {
                    return state.DoS(0, false,
                            REJECT_INSUFFICIENTFEE, "insufficient fee", false,
                            strprintf("rejecting replacement %s; new feerate %s <= old feerate %s",
                                  hash.ToString(),
                                  newFeeRate.ToString(),
                                  oldFeeRate.ToString()));
                }

                BOOST_FOREACH(const CTxIn &txin, mi->GetTx().vin)
                {
                    setConflictsParents.insert(txin.prevout.hash);
                }

                nConflictingCount += mi->GetCountWithDescendants();
            }
            // This potentially overestimates the number of actual descendants
            // but we just want to be conservative to avoid doing too much
            // work.
            if (nConflictingCount <= maxDescendantsToVisit) {
                // If not too many to replace, then calculate the set of
                // transactions that would have to be evicted
                BOOST_FOREACH(CTxMemPool::txiter it, setIterConflicting) {
                    pool.CalculateDescendants(it, allConflicting);
                }
                BOOST_FOREACH(CTxMemPool::txiter it, allConflicting) {
                    nConflictingFees += it->GetModifiedFee();
                    nConflictingSize += it->GetTxSize();
                }
            } else {
                return state.DoS(0, false,
                        REJECT_NONSTANDARD, "too many potential replacements", false,
                        strprintf("rejecting replacement %s; too many potential replacements (%d > %d)\n",
                            hash.ToString(),
                            nConflictingCount,
                            maxDescendantsToVisit));
            }

            for (unsigned int j = 0; j < tx.vin.size(); j++)
            {
                // We don't want to accept replacements that require low
                // feerate junk to be mined first. Ideally we'd keep track of
                // the ancestor feerates and make the decision based on that,
                // but for now requiring all new inputs to be confirmed works.
                if (!setConflictsParents.count(tx.vin[j].prevout.hash))
                {
                    // Rather than check the UTXO set - potentially expensive -
                    // it's cheaper to just check if the new input refers to a
                    // tx that's in the mempool.
                    if (pool.mapTx.find(tx.vin[j].prevout.hash) != pool.mapTx.end())
                        return state.DoS(0, false,
                                         REJECT_NONSTANDARD, "replacement-adds-unconfirmed", false,
                                         strprintf("replacement %s adds unconfirmed input, idx %d",
                                                  hash.ToString(), j));
                }
            }

            // The replacement must pay greater fees than the transactions it
            // replaces - if we did the bandwidth used by those conflicting
            // transactions would not be paid for.
            if (nModifiedFees < nConflictingFees)
            {
                return state.DoS(0, false,
                                 REJECT_INSUFFICIENTFEE, "insufficient fee", false,
                                 strprintf("rejecting replacement %s, less fees than conflicting txs; %s < %s",
                                          hash.ToString(), FormatMoney(nModifiedFees), FormatMoney(nConflictingFees)));
            }

            // Finally in addition to paying more fees than the conflicts the
            // new transaction must pay for its own bandwidth.
            CAmount nDeltaFees = nModifiedFees - nConflictingFees;
            if (nDeltaFees < ::minRelayTxFee.GetFee(nSize))
            {
                return state.DoS(0, false,
                        REJECT_INSUFFICIENTFEE, "insufficient fee", false,
                        strprintf("rejecting replacement %s, not enough additional fees to relay; %s < %s",
                              hash.ToString(),
                              FormatMoney(nDeltaFees),
                              FormatMoney(::minRelayTxFee.GetFee(nSize))));
            }
        }

        unsigned int scriptVerifyFlags = STANDARD_SCRIPT_VERIFY_FLAGS;
        if (!Params().RequireStandard()) {
            scriptVerifyFlags = GetArg("-promiscuousmempoolflags", scriptVerifyFlags);
        }

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        PrecomputedTransactionData txdata(tx);
        if (!CheckInputs(tx, state, view, true, scriptVerifyFlags, true, txdata)) {
            // SCRIPT_VERIFY_CLEANSTACK requires SCRIPT_VERIFY_WITNESS, so we
            // need to turn both off, and compare against just turning off CLEANSTACK
            // to see if the failure is specifically due to witness validation.
            if (tx.wit.IsNull() && CheckInputs(tx, state, view, true, scriptVerifyFlags & ~(SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_CLEANSTACK), true, txdata) &&
                !CheckInputs(tx, state, view, true, scriptVerifyFlags & ~SCRIPT_VERIFY_CLEANSTACK, true, txdata)) {
                // Only the witness is missing, so the transaction itself may be fine.
                state.SetCorruptionPossible();
            }
            return false;
        }

        // Check again against just the consensus-critical mandatory script
        // verification flags, in case of bugs in the standard flags that cause
        // transactions to pass as valid when they're actually invalid. For
        // instance the STRICTENC flag was incorrectly allowing certain
        // CHECKSIG NOT scripts to pass, even though they were invalid.
        //
        // There is a similar check in CreateNewBlock() to prevent creating
        // invalid blocks, however allowing such transactions into the mempool
        // can be exploited as a DoS attack.
        if (!CheckInputs(tx, state, view, true, MANDATORY_SCRIPT_VERIFY_FLAGS, true, txdata))
        {
            return error("%s: BUG! PLEASE REPORT THIS! ConnectInputs failed against MANDATORY but not STANDARD flags %s, %s",
                __func__, hash.ToString(), FormatStateMessage(state));
        }

        // Remove conflicting transactions from the mempool
        BOOST_FOREACH(const CTxMemPool::txiter it, allConflicting)
        {
            LogPrint("mempool", "replacing tx %s with %s for %s MOON additional fees, %d delta bytes\n",
                    it->GetTx().GetHash().ToString(),
                    hash.ToString(),
                    FormatMoney(nModifiedFees - nConflictingFees),
                    (int)nSize - (int)nConflictingSize);
        }
        pool.RemoveStaged(allConflicting, false);

        // Store transaction in memory
        pool.addUnchecked(hash, entry, setAncestors, !IsInitialBlockDownload());

        // trim mempool and check if tx was trimmed
        if (!fOverrideMempoolLimit) {
            LimitMempoolSize(pool, GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000, GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60);
            if (!pool.exists(hash))
                return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "mempool full");
        }
    }

    SyncWithWallets(tx, NULL, NULL);

    return true;
}

bool AcceptToMemoryPool(CTxMemPool& pool, CValidationState &state, const CTransaction &tx, bool fLimitFree,
                        bool* pfMissingInputs, bool fOverrideMempoolLimit, const CAmount nAbsurdFee)
{
    std::vector<uint256> vHashTxToUncache;
    bool res = AcceptToMemoryPoolWorker(pool, state, tx, fLimitFree, pfMissingInputs, fOverrideMempoolLimit, nAbsurdFee, vHashTxToUncache);
    if (!res) {
        BOOST_FOREACH(const uint256& hashTx, vHashTxToUncache)
            pcoinsTip->Uncache(hashTx);
    }
    return res;
}

/** Return transaction in txOut, and if it was found inside a block, its hash is placed in hashBlock */
bool GetTransaction(const uint256 &hash, CTransaction &txOut, const Consensus::Params& consensusParams, uint256 &hashBlock, bool fAllowSlow)
{
    CBlockIndex *pindexSlow = NULL;

    LOCK(cs_main);

    std::shared_ptr<const CTransaction> ptx = mempool.get(hash);
    if (ptx)
    {
        txOut = *ptx;
        return true;
    }

    if (fTxIndex) {
        CDiskTxPos postx;
        if (pblocktree->ReadTxIndex(hash, postx)) {
            CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
            if (file.IsNull())
                return error("%s: OpenBlockFile failed", __func__);
            CBlockHeader header;
            try {
                file >> header;
                fseek(file.Get(), postx.nTxOffset, SEEK_CUR);
                file >> txOut;
            } catch (const std::exception& e) {
                return error("%s: Deserialize or I/O error - %s", __func__, e.what());
            }
            hashBlock = header.GetHash();
            if (txOut.GetHash() != hash)
                return error("%s: txid mismatch", __func__);
            return true;
        }
    }

    if (fAllowSlow) { // use coin database to locate block that contains transaction, and scan it
        int nHeight = -1;
        {
            const CCoinsViewCache& view = *pcoinsTip;
            const CCoins* coins = view.AccessCoins(hash);
            if (coins)
                nHeight = coins->nHeight;
        }
        if (nHeight > 0)
            pindexSlow = chainActive[nHeight];
    }

    if (pindexSlow) {
        CBlock block;
        if (ReadBlockFromDisk(block, pindexSlow, consensusParams)) {
            BOOST_FOREACH(const CTransaction &tx, block.vtx) {
                if (tx.GetHash() == hash) {
                    txOut = tx;
                    hashBlock = pindexSlow->GetBlockHash();
                    return true;
                }
            }
        }
    }

    return false;
}






//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

bool WriteBlockToDisk(const CBlock& block, CDiskBlockPos& pos, const CMessageHeader::MessageStartChars& messageStart)
{
    // Open history file to append
    CAutoFile fileout(OpenBlockFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("WriteBlockToDisk: OpenBlockFile failed");

    // Write index header
    unsigned int nSize = fileout.GetSerializeSize(block);
    fileout << FLATDATA(messageStart) << nSize;

    // Write block
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0)
        return error("WriteBlockToDisk: ftell failed");
    pos.nPos = (unsigned int)fileOutPos;
    fileout << block;

    return true;
}

bool ReadBlockFromDisk(CBlock& block, const CDiskBlockPos& pos, const Consensus::Params& consensusParams)
{
    block.SetNull();

    // Open history file to read
    CAutoFile filein(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("ReadBlockFromDisk: OpenBlockFile failed for %s", pos.ToString());

    // Read block
    try {
        filein >> block;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s at %s", __func__, e.what(), pos.ToString());
    }
    
    // WarpSync check every 1/32th blocks pow hash (baz)
    int syncheight = GetHeight();
    if (syncheight > 1112500) { WarpSync=1; }
    if (syncheight % WarpSync == 0) {
        // Check proof of work matches claimed amount
        if (!CheckProofOfWork(block.GetPoWHash(AlgoSwitch), block.nBits, consensusParams, true)){
            if(WarpSync!=1){ LogPrintf("** HALTNG WarpSync DUE TO ERRORS ENCOUNTERED\n"); }
            //if(AlgoSwitch){
                //LogPrintf("** FAILED ON BALLOON (unrecoverable)\n");
                //return error("ReadBlockFromDisk: Errors in block header at %s (balloon pow)", pos.ToString());
            //} else {
                // could be failed balloon block
                //if (!CheckProofOfWork(block.GetPoWHash(true), block.nBits, consensusParams, false)){
                    LogPrintf("** FAILED ON SCRYPT\n");
                    return error("ReadBlockFromDisk: Errors in block header at %s (scrypt pow)", pos.ToString());
                //} else {
                    //LogPrintf("** ALGO SWITCHED TO BALLOON\n");
                    //AlgoSwitch=true;
                //}
	        //}
        }
    }
    return true;
}

bool ReadBlockFromDisk(CBlock& block, const CBlockIndex* pindex, const Consensus::Params& consensusParams)
{
    if (!ReadBlockFromDisk(block, pindex->GetBlockPos(), consensusParams))
        return false;
    if (block.GetHash() != pindex->GetBlockHash())
        return error("ReadBlockFromDisk(CBlock&, CBlockIndex*): GetHash() doesn't match index for %s at %s",
                pindex->ToString(), pindex->GetBlockPos().ToString());
    return true;
}

int static generateMTRandom(unsigned int s, int range)
{
    boost::random::mt19937 gen(s);
    boost::uniform_int<> dist(1, range);
    return dist(gen);
}

CAmount GetBlockSubsidy(int nHeight, uint256 prevHash)
{
    CAmount nSubsidy = 29531 * COIN; // the lunar cycle is 29.53059 days, so we rounded it up

    std::string cseed_str = prevHash.ToString().substr(7,7);
    const char* cseed = cseed_str.c_str();
    long seed = hex2long(cseed);

	// cases for block 1 - 384400
	if(nHeight <= 100000) {
                nSubsidy = (1 + generateMTRandom(seed, 1999999)) * COIN;
        } else if(nHeight > 193076 && nHeight < 203158) {
                nSubsidy = 2519841 * COIN; // for _roughly_ one week, the cost of the Apollo program will be paid back -- 25.4bn MOON!
        } else if(nHeight <= 203518) {
                nSubsidy = (1 + generateMTRandom(seed, 999999)) * COIN;
        } else if(nHeight <= 250000) {
                nSubsidy = (1 + generateMTRandom(seed, 599999)) * COIN;
        } else if(nHeight <= 300000) {
                nSubsidy = (1 + generateMTRandom(seed, 349999)) * COIN;
        } else if(nHeight <= 350000) {
                nSubsidy = (1 + generateMTRandom(seed, 174999)) * COIN;
        } else if(nHeight <= 375000) {
                nSubsidy = (1 + generateMTRandom(seed, 99999)) * COIN;
        } else if(nHeight <= 384400) {
                nSubsidy = (1 + generateMTRandom(seed, 49999)) * COIN;
    }
	
	// cases for block 384401 - 1099999
 	if (nHeight % 29531 == 0) {
                // a prize for ever lunar cycle
                nSubsidy = nSubsidy * 2;
    }

	// cases for block 1100000+
	if (nHeight > 1099999) {
				nSubsidy = floor( 19697202017 / (floor(nHeight/100000)*100000) ) * COIN;
    }
    
	// cases for block 1250000+
	if (nHeight > 1249999) {
				nSubsidy = floor( floor(0.29531 * 19697202017) / (floor(nHeight/100000)*100000) ) * COIN;
	}

	// case for block 5432100000 onwards
	if (nHeight > 2147483647) {             // 2147483647 (was 5432099999)
				nSubsidy = 0 * COIN;
	}	
	
    return nSubsidy;
}

unsigned int GetTickCounts()
{
        struct timeval tv;
        if(gettimeofday(&tv, NULL) != 0)
                return 0;

        return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

bool IsInitialBlockDownload()
{
    // Great place to kick off synctimer
    if(ClientUptime==-1){
       ClientUptime=GetTickCounts();
    }

    const CChainParams& chainParams = Params();

    // Once this function has returned false, it must remain false.
    static std::atomic<bool> latchToFalse{false};
    // Optimization: pre-test latch before taking the lock.
    if (latchToFalse.load(std::memory_order_relaxed))
        return false;

    LOCK(cs_main);
    if (latchToFalse.load(std::memory_order_relaxed))
        return false;
    if (fImporting || fReindex)
        return true;
    if (chainActive.Tip()->GetBlockTime() < (GetTime() - nMaxTipAge))
        return true;
    latchToFalse.store(true, std::memory_order_relaxed);
    return false;
}

bool fLargeWorkForkFound = false;
bool fLargeWorkInvalidChainFound = false;
CBlockIndex *pindexBestForkTip = NULL, *pindexBestForkBase = NULL;

static void AlertNotify(const std::string& strMessage)
{
    uiInterface.NotifyAlertChanged();
    std::string strCmd = GetArg("-alertnotify", "");
    if (strCmd.empty()) return;

    // Alert text should be plain ascii coming from a trusted source, but to
    // be safe we first strip anything not in safeChars, then add single quotes around
    // the whole string before passing it to the shell:
    std::string singleQuote("'");
    std::string safeStatus = SanitizeString(strMessage);
    safeStatus = singleQuote+safeStatus+singleQuote;
    boost::replace_all(strCmd, "%s", safeStatus);

    boost::thread t(runCommand, strCmd); // thread runs free
}

void CheckForkWarningConditions()
{
    AssertLockHeld(cs_main);
    // Before we get past initial download, we cannot reliably alert about forks
    // (we assume we don't get stuck on a fork before finishing our initial sync)
    if (IsInitialBlockDownload())
        return;

    // If our best fork is no longer within 72 blocks (+/- 12 hours if no one mines it)
    // of our head, drop it
    if (pindexBestForkTip && chainActive.Height() - pindexBestForkTip->nHeight >= 72)
        pindexBestForkTip = NULL;

    if (pindexBestForkTip || (pindexBestInvalid && pindexBestInvalid->nChainWork > chainActive.Tip()->nChainWork + (GetBlockProof(*chainActive.Tip()) * 6)))
    {
        if (!fLargeWorkForkFound && pindexBestForkBase)
        {
            std::string warning = std::string("'Warning: Large-work fork detected, forking after block ") +
                pindexBestForkBase->phashBlock->ToString() + std::string("'");
            AlertNotify(warning);
        }
        if (pindexBestForkTip && pindexBestForkBase)
        {
            LogPrintf("%s: Warning: Large valid fork found\n  forking the chain at height %d (%s)\n  lasting to height %d (%s).\nChain state database corruption likely.\n", __func__,
                   pindexBestForkBase->nHeight, pindexBestForkBase->phashBlock->ToString(),
                   pindexBestForkTip->nHeight, pindexBestForkTip->phashBlock->ToString());
            fLargeWorkForkFound = true;
        }
        else
        {
            LogPrintf("%s: Warning: Found invalid chain at least ~6 blocks longer than our best chain.\nChain state database corruption likely.\n", __func__);
            fLargeWorkInvalidChainFound = true;
        }
    }
    else
    {
        fLargeWorkForkFound = false;
        fLargeWorkInvalidChainFound = false;
    }
}

void CheckForkWarningConditionsOnNewFork(CBlockIndex* pindexNewForkTip)
{
    AssertLockHeld(cs_main);
    // If we are on a fork that is sufficiently large, set a warning flag
    CBlockIndex* pfork = pindexNewForkTip;
    CBlockIndex* plonger = chainActive.Tip();
    while (pfork && pfork != plonger)
    {
        while (plonger && plonger->nHeight > pfork->nHeight)
            plonger = plonger->pprev;
        if (pfork == plonger)
            break;
        pfork = pfork->pprev;
    }

    // We define a condition where we should warn the user about as a fork of at least 7 blocks
    // with a tip within 72 blocks (+/- 12 hours if no one mines it) of ours
    // We use 7 blocks rather arbitrarily as it represents just under 10% of sustained network
    // hash rate operating on the fork.
    // or a chain that is entirely longer than ours and invalid (note that this should be detected by both)
    // We define it this way because it allows us to only store the highest fork tip (+ base) which meets
    // the 7-block condition and from this always have the most-likely-to-cause-warning fork
    if (pfork && (!pindexBestForkTip || (pindexBestForkTip && pindexNewForkTip->nHeight > pindexBestForkTip->nHeight)) &&
            pindexNewForkTip->nChainWork - pfork->nChainWork > (GetBlockProof(*pfork) * 7) &&
            chainActive.Height() - pindexNewForkTip->nHeight < 72)
    {
        pindexBestForkTip = pindexNewForkTip;
        pindexBestForkBase = pfork;
    }

    CheckForkWarningConditions();
}

// Requires cs_main.
void Misbehaving(NodeId pnode, int howmuch)
{
    if (howmuch == 0)
        return;

    CNodeState *state = State(pnode);
    if (state == NULL)
        return;

    state->nMisbehavior += howmuch;
    int banscore = GetArg("-banscore", DEFAULT_BANSCORE_THRESHOLD);
    if (state->nMisbehavior >= banscore && state->nMisbehavior - howmuch < banscore)
    {
        LogPrintf("%s: %s (%d -> %d) BAN THRESHOLD EXCEEDED\n", __func__, state->name, state->nMisbehavior-howmuch, state->nMisbehavior);
        state->fShouldBan = true;
    } else
        LogPrintf("%s: %s (%d -> %d)\n", __func__, state->name, state->nMisbehavior-howmuch, state->nMisbehavior);
}

void static InvalidChainFound(CBlockIndex* pindexNew)
{
    if (!pindexBestInvalid || pindexNew->nChainWork > pindexBestInvalid->nChainWork)
        pindexBestInvalid = pindexNew;

    LogPrintf("%s: invalid block=%s  height=%d  log2_work=%.8g  date=%s\n", __func__,
      pindexNew->GetBlockHash().ToString(), pindexNew->nHeight,
      log(pindexNew->nChainWork.getdouble())/log(2.0), DateTimeStrFormat("%Y-%m-%d %H:%M:%S",
      pindexNew->GetBlockTime()));
    CBlockIndex *tip = chainActive.Tip();
    assert (tip);
    LogPrintf("%s:  current best=%s  height=%d  log2_work=%.8g  date=%s\n", __func__,
      tip->GetBlockHash().ToString(), chainActive.Height(), log(tip->nChainWork.getdouble())/log(2.0),
      DateTimeStrFormat("%Y-%m-%d %H:%M:%S", tip->GetBlockTime()));
    CheckForkWarningConditions();
}

void static InvalidBlockFound(CBlockIndex *pindex, const CValidationState &state) {
    int nDoS = 0;
    if (state.IsInvalid(nDoS)) {
        std::map<uint256, std::pair<NodeId, bool>>::iterator it = mapBlockSource.find(pindex->GetBlockHash());
        if (it != mapBlockSource.end() && State(it->second.first)) {
            assert (state.GetRejectCode() < REJECT_INTERNAL); // Blocks are never rejected with internal reject codes
            CBlockReject reject = {(unsigned char)state.GetRejectCode(), state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), pindex->GetBlockHash()};
            State(it->second.first)->rejects.push_back(reject);
            if (nDoS > 0 && it->second.second)
                Misbehaving(it->second.first, nDoS);
        }
    }
    if (!state.CorruptionPossible()) {
        pindex->nStatus |= BLOCK_FAILED_VALID;
        setDirtyBlockIndex.insert(pindex);
        setBlockIndexCandidates.erase(pindex);
        InvalidChainFound(pindex);
    }
}

void UpdateCoins(const CTransaction& tx, CCoinsViewCache& inputs, CTxUndo &txundo, int nHeight)
{
    // mark inputs spent
    if (!tx.IsCoinBase()) {
        txundo.vprevout.reserve(tx.vin.size());
        BOOST_FOREACH(const CTxIn &txin, tx.vin) {
            CCoinsModifier coins = inputs.ModifyCoins(txin.prevout.hash);
            unsigned nPos = txin.prevout.n;

            if (nPos >= coins->vout.size() || coins->vout[nPos].IsNull())
                assert(false);
            // mark an outpoint spent, and construct undo information
            txundo.vprevout.push_back(CTxInUndo(coins->vout[nPos]));
            coins->Spend(nPos);
            if (coins->vout.size() == 0) {
                CTxInUndo& undo = txundo.vprevout.back();
                undo.nHeight = coins->nHeight;
                undo.fCoinBase = coins->fCoinBase;
                undo.nVersion = coins->nVersion;
            }
        }
    }
    // add outputs
    inputs.ModifyNewCoins(tx.GetHash(), tx.IsCoinBase())->FromTx(tx, nHeight);
}

void UpdateCoins(const CTransaction& tx, CCoinsViewCache& inputs, int nHeight)
{
    CTxUndo txundo;
    UpdateCoins(tx, inputs, txundo, nHeight);
}

bool CScriptCheck::operator()() {
    const CScript &scriptSig = ptxTo->vin[nIn].scriptSig;
    const CScriptWitness *witness = (nIn < ptxTo->wit.vtxinwit.size()) ? &ptxTo->wit.vtxinwit[nIn].scriptWitness : NULL;
    if (!VerifyScript(scriptSig, scriptPubKey, witness, nFlags, CachingTransactionSignatureChecker(ptxTo, nIn, amount, cacheStore, *txdata), &error)) {
        return false;
    }
    return true;
}

int GetSpendHeight(const CCoinsViewCache& inputs)
{
    LOCK(cs_main);
    CBlockIndex* pindexPrev = mapBlockIndex.find(inputs.GetBestBlock())->second;
    return pindexPrev->nHeight + 1;
}

namespace Consensus {
bool CheckTxInputs(const CTransaction& tx, CValidationState& state, const CCoinsViewCache& inputs, int nSpendHeight)
{
        // This doesn't trigger the DoS code on purpose; if it did, it would make it easier
        // for an attacker to attempt to split the network.
        if (!inputs.HaveInputs(tx))
            return state.Invalid(false, 0, "", "Inputs unavailable");

        CAmount nValueIn = 0;
        CAmount nFees = 0;
        for (unsigned int i = 0; i < tx.vin.size(); i++)
        {
            const COutPoint &prevout = tx.vin[i].prevout;
            const CCoins *coins = inputs.AccessCoins(prevout.hash);
            assert(coins);

            // If prev is coinbase, check that it's matured
            if (coins->IsCoinBase()) {
                if (nSpendHeight - coins->nHeight < COINBASE_MATURITY)
                    return state.Invalid(false,
                        REJECT_INVALID, "bad-txns-premature-spend-of-coinbase",
                        strprintf("tried to spend coinbase at depth %d", nSpendHeight - coins->nHeight));
            }

            // Check for negative or overflow input values
            nValueIn += coins->vout[prevout.n].nValue;
            if (!MoneyRange(coins->vout[prevout.n].nValue) || !MoneyRange(nValueIn))
                return state.DoS(100, false, REJECT_INVALID, "bad-txns-inputvalues-outofrange");

        }

        if (nValueIn < tx.GetValueOut())
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-in-belowout", false,
                strprintf("value in (%s) < value out (%s)", FormatMoney(nValueIn), FormatMoney(tx.GetValueOut())));

        // Tally transaction fees
        CAmount nTxFee = nValueIn - tx.GetValueOut();
        if (nTxFee < 0)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-fee-negative");
        nFees += nTxFee;
        if (!MoneyRange(nFees))
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-fee-outofrange");
    return true;
}
}// namespace Consensus

bool CheckInputs(const CTransaction& tx, CValidationState &state, const CCoinsViewCache &inputs, bool fScriptChecks, unsigned int flags, bool cacheStore, PrecomputedTransactionData& txdata, std::vector<CScriptCheck> *pvChecks)
{
    if (!tx.IsCoinBase())
    {
        if (!Consensus::CheckTxInputs(tx, state, inputs, GetSpendHeight(inputs)))
            return false;

        if (pvChecks)
            pvChecks->reserve(tx.vin.size());

        // The first loop above does all the inexpensive checks.
        // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
        // Helps prevent CPU exhaustion attacks.

        // Skip ECDSA signature verification when connecting blocks before the
        // last block chain checkpoint. Assuming the checkpoints are valid this
        // is safe because block merkle hashes are still computed and checked,
        // and any change will be caught at the next checkpoint. Of course, if
        // the checkpoint is for a chain that's invalid due to false scriptSigs
        // this optimization would allow an invalid chain to be accepted.
        if (fScriptChecks) {
            for (unsigned int i = 0; i < tx.vin.size(); i++) {
                const COutPoint &prevout = tx.vin[i].prevout;
                const CCoins* coins = inputs.AccessCoins(prevout.hash);
                assert(coins);

                // Verify signature
                CScriptCheck check(*coins, tx, i, flags, cacheStore, &txdata);
                if (pvChecks) {
                    pvChecks->push_back(CScriptCheck());
                    check.swap(pvChecks->back());
                } else if (!check()) {
                    if (flags & STANDARD_NOT_MANDATORY_VERIFY_FLAGS) {
                        // Check whether the failure was caused by a
                        // non-mandatory script verification check, such as
                        // non-standard DER encodings or non-null dummy
                        // arguments; if so, don't trigger DoS protection to
                        // avoid splitting the network between upgraded and
                        // non-upgraded nodes.
                        CScriptCheck check2(*coins, tx, i,
                                flags & ~STANDARD_NOT_MANDATORY_VERIFY_FLAGS, cacheStore, &txdata);
                        if (check2())
                            return state.Invalid(false, REJECT_NONSTANDARD, strprintf("non-mandatory-script-verify-flag (%s)", ScriptErrorString(check.GetScriptError())));
                    }
                    // Failures of other flags indicate a transaction that is
                    // invalid in new blocks, e.g. a invalid P2SH. We DoS ban
                    // such nodes as they are not following the protocol. That
                    // said during an upgrade careful thought should be taken
                    // as to the correct behavior - we may want to continue
                    // peering with non-upgraded nodes even after soft-fork
                    // super-majority signaling has occurred.
                    return state.DoS(100,false, REJECT_INVALID, strprintf("mandatory-script-verify-flag-failed (%s)", ScriptErrorString(check.GetScriptError())));
                }
            }
        }
    }

    return true;
}

namespace {

bool UndoWriteToDisk(const CBlockUndo& blockundo, CDiskBlockPos& pos, const uint256& hashBlock, const CMessageHeader::MessageStartChars& messageStart)
{
    // Open history file to append
    CAutoFile fileout(OpenUndoFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s: OpenUndoFile failed", __func__);

    // Write index header
    unsigned int nSize = fileout.GetSerializeSize(blockundo);
    fileout << FLATDATA(messageStart) << nSize;

    // Write undo data
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0)
        return error("%s: ftell failed", __func__);
    pos.nPos = (unsigned int)fileOutPos;
    fileout << blockundo;

    // calculate & write checksum
    CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
    hasher << hashBlock;
    hasher << blockundo;
    fileout << hasher.GetHash();

    return true;
}

bool UndoReadFromDisk(CBlockUndo& blockundo, const CDiskBlockPos& pos, const uint256& hashBlock)
{
    // Open history file to read
    CAutoFile filein(OpenUndoFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("%s: OpenUndoFile failed", __func__);

    // Read block
    uint256 hashChecksum;
    try {
        filein >> blockundo;
        filein >> hashChecksum;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }

    // Verify checksum
    CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
    hasher << hashBlock;
    hasher << blockundo;
    if (hashChecksum != hasher.GetHash())
        return error("%s: Checksum mismatch", __func__);

    return true;
}

/** Abort with a message */
bool AbortNode(const std::string& strMessage, const std::string& userMessage="")
{
    strMiscWarning = strMessage;
    LogPrintf("*** %s\n", strMessage);
    uiInterface.ThreadSafeMessageBox(
        userMessage.empty() ? _("Error: A fatal internal error occurred, see debug.log for details") : userMessage,
        "", CClientUIInterface::MSG_ERROR);
    StartShutdown();
    return false;
}

bool AbortNode(CValidationState& state, const std::string& strMessage, const std::string& userMessage="")
{
    AbortNode(strMessage, userMessage);
    return state.Error(strMessage);
}

} // anon namespace

/**
 * Apply the undo operation of a CTxInUndo to the given chain state.
 * @param undo The undo object.
 * @param view The coins view to which to apply the changes.
 * @param out The out point that corresponds to the tx input.
 * @return True on success.
 */
static bool ApplyTxInUndo(const CTxInUndo& undo, CCoinsViewCache& view, const COutPoint& out)
{
    bool fClean = true;

    CCoinsModifier coins = view.ModifyCoins(out.hash);
    if (undo.nHeight != 0) {
        // undo data contains height: this is the last output of the prevout tx being spent
        if (!coins->IsPruned())
            fClean = fClean && error("%s: undo data overwriting existing transaction", __func__);
        coins->Clear();
        coins->fCoinBase = undo.fCoinBase;
        coins->nHeight = undo.nHeight;
        coins->nVersion = undo.nVersion;
    } else {
        if (coins->IsPruned())
            fClean = fClean && error("%s: undo data adding output to missing transaction", __func__);
    }
    if (coins->IsAvailable(out.n))
        fClean = fClean && error("%s: undo data overwriting existing output", __func__);
    if (coins->vout.size() < out.n+1)
        coins->vout.resize(out.n+1);
    coins->vout[out.n] = undo.txout;

    return fClean;
}

bool DisconnectBlock(const CBlock& block, CValidationState& state, const CBlockIndex* pindex, CCoinsViewCache& view, bool* pfClean)
{
    assert(pindex->GetBlockHash() == view.GetBestBlock());

    if (pfClean)
        *pfClean = false;

    bool fClean = true;

    CBlockUndo blockUndo;
    CDiskBlockPos pos = pindex->GetUndoPos();
    if (pos.IsNull())
        return error("DisconnectBlock(): no undo data available");
    if (!UndoReadFromDisk(blockUndo, pos, pindex->pprev->GetBlockHash()))
        return error("DisconnectBlock(): failure reading undo data");

    if (blockUndo.vtxundo.size() + 1 != block.vtx.size())
        return error("DisconnectBlock(): block and undo data inconsistent");

    // undo transactions in reverse order
    for (int i = block.vtx.size() - 1; i >= 0; i--) {
        const CTransaction &tx = block.vtx[i];
        uint256 hash = tx.GetHash();

        // Check that all outputs are available and match the outputs in the block itself
        // exactly.
        {
        CCoinsModifier outs = view.ModifyCoins(hash);
        outs->ClearUnspendable();

        CCoins outsBlock(tx, pindex->nHeight);
        // The CCoins serialization does not serialize negative numbers.
        // No network rules currently depend on the version here, so an inconsistency is harmless
        // but it must be corrected before txout nversion ever influences a network rule.
        if (outsBlock.nVersion < 0)
            outs->nVersion = outsBlock.nVersion;
        if (*outs != outsBlock)
            fClean = fClean && error("DisconnectBlock(): added transaction mismatch? database corrupted");

        // remove outputs
        outs->Clear();
        }

        // restore inputs
        if (i > 0) { // not coinbases
            const CTxUndo &txundo = blockUndo.vtxundo[i-1];
            if (txundo.vprevout.size() != tx.vin.size())
                return error("DisconnectBlock(): transaction and undo data inconsistent");
            for (unsigned int j = tx.vin.size(); j-- > 0;) {
                const COutPoint &out = tx.vin[j].prevout;
                const CTxInUndo &undo = txundo.vprevout[j];
                if (!ApplyTxInUndo(undo, view, out))
                    fClean = false;
            }
        }
    }

    // move best block pointer to prevout block
    view.SetBestBlock(pindex->pprev->GetBlockHash());

    if (pfClean) {
        *pfClean = fClean;
        return true;
    }

    return fClean;
}

void static FlushBlockFile(bool fFinalize = false)
{
    LOCK(cs_LastBlockFile);

    CDiskBlockPos posOld(nLastBlockFile, 0);

    FILE *fileOld = OpenBlockFile(posOld);
    if (fileOld) {
        if (fFinalize)
            TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }

    fileOld = OpenUndoFile(posOld);
    if (fileOld) {
        if (fFinalize)
            TruncateFile(fileOld, vinfoBlockFile[nLastBlockFile].nUndoSize);
        FileCommit(fileOld);
        fclose(fileOld);
    }
}

bool FindUndoPos(CValidationState &state, int nFile, CDiskBlockPos &pos, unsigned int nAddSize);

static CCheckQueue<CScriptCheck> scriptcheckqueue(128);

void ThreadScriptCheck() {
    RenameThread("mooncoin-scriptch");
    scriptcheckqueue.Thread();
}

// Protected by cs_main
VersionBitsCache versionbitscache;

int32_t ComputeBlockVersion(const CBlockIndex* pindexPrev, const Consensus::Params& params)
{
    LOCK(cs_main);
    int32_t nVersion = VERSIONBITS_TOP_BITS;

    for (int i = 0; i < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; i++) {
        ThresholdState state = VersionBitsState(pindexPrev, params, (Consensus::DeploymentPos)i, versionbitscache);
        if (state == THRESHOLD_LOCKED_IN || state == THRESHOLD_STARTED) {
            nVersion |= VersionBitsMask(params, (Consensus::DeploymentPos)i);
        }
    }

    return nVersion;
}

/**
 * Threshold condition checker that triggers when unknown versionbits are seen on the network.
 */
class WarningBitsConditionChecker : public AbstractThresholdConditionChecker
{
private:
    int bit;

public:
    WarningBitsConditionChecker(int bitIn) : bit(bitIn) {}

    int64_t BeginTime(const Consensus::Params& params) const { return 0; }
    int64_t EndTime(const Consensus::Params& params) const { return std::numeric_limits<int64_t>::max(); }
    int Period(const Consensus::Params& params) const { return params.nMinerConfirmationWindow; }
    int Threshold(const Consensus::Params& params) const { return params.nRuleChangeActivationThreshold; }

    bool Condition(const CBlockIndex* pindex, const Consensus::Params& params) const
    {
        return ((pindex->nVersion & VERSIONBITS_TOP_MASK) == VERSIONBITS_TOP_BITS) &&
               ((pindex->nVersion >> bit) & 1) != 0 &&
               ((ComputeBlockVersion(pindex->pprev, params) >> bit) & 1) == 0;
    }
};

// Protected by cs_main
static ThresholdConditionCache warningcache[VERSIONBITS_NUM_BITS];

static int64_t nTimeCheck = 0;
static int64_t nTimeForks = 0;
static int64_t nTimeVerify = 0;
static int64_t nTimeConnect = 0;
static int64_t nTimeIndex = 0;
static int64_t nTimeCallbacks = 0;
static int64_t nTimeTotal = 0;

bool ConnectBlock(const CBlock& block, CValidationState& state, CBlockIndex* pindex,
                  CCoinsViewCache& view, const CChainParams& chainparams, bool fJustCheck)
{
    AssertLockHeld(cs_main);

    int64_t nTimeStart = GetTimeMicros();

    // Check it again in case a previous version let a bad block in
    if (!CheckBlock(block, state, chainparams.GetConsensus(), !fJustCheck, !fJustCheck))
        return error("%s: Consensus::CheckBlock: %s", __func__, FormatStateMessage(state));

    // verify that the view's current state corresponds to the previous block
    uint256 hashPrevBlock = pindex->pprev == NULL ? uint256() : pindex->pprev->GetBlockHash();
    assert(hashPrevBlock == view.GetBestBlock());

    // Special case for the genesis block, skipping connection of its transactions
    // (its coinbase is unspendable)
    if (block.GetHash() == chainparams.GetConsensus().hashGenesisBlock) {
        if (!fJustCheck)
            view.SetBestBlock(pindex->GetBlockHash());
        return true;
    }

    bool fScriptChecks = true;
    if (fCheckpointsEnabled) {
        CBlockIndex *pindexLastCheckpoint = Checkpoints::GetLastCheckpoint(chainparams.Checkpoints());
        if (pindexLastCheckpoint && pindexLastCheckpoint->GetAncestor(pindex->nHeight) == pindex) {
            // This block is an ancestor of a checkpoint: disable script checks
            fScriptChecks = false;
        }
    }

    int64_t nTime1 = GetTimeMicros(); nTimeCheck += nTime1 - nTimeStart;
    LogPrint("bench", "    - Sanity checks: %.2fms [%.2fs]\n", 0.001 * (nTime1 - nTimeStart), nTimeCheck * 0.000001);

    // Do not allow blocks that contain transactions which 'overwrite' older transactions,
    // unless those are already completely spent.
    // If such overwrites are allowed, coinbases and transactions depending upon those
    // can be duplicated to remove the ability to spend the first instance -- even after
    // being sent to another address.
    // See BIP30 and http://r6.ca/blog/20120206T005236Z.html for more information.
    // This logic is not necessary for memory pool transactions, as AcceptToMemoryPool
    // already refuses previously-known transaction ids entirely.
    // This rule was originally applied all blocks whose timestamp was after October 1, 2012, 0:00 UTC.
    // Now that the whole chain is irreversibly beyond that time it is applied to all blocks,
    // this prevents exploiting the issue against nodes in their initial block download.
    bool fEnforceBIP30 = true;
    if (fEnforceBIP30) {
        BOOST_FOREACH(const CTransaction& tx, block.vtx) {
            const CCoins* coins = view.AccessCoins(tx.GetHash());
            if (coins && !coins->IsPruned())
                return state.DoS(100, error("ConnectBlock() : tried to overwrite transaction"),
                                 REJECT_INVALID, "bad-txns-BIP30");
        }
    }

    // BIP16 didn't become active until Oct 1 2012
    int64_t nBIP16SwitchTime = 1349049600;
    bool fStrictPayToScriptHash = (pindex->GetBlockTime() >= nBIP16SwitchTime);

    unsigned int flags = fStrictPayToScriptHash ? SCRIPT_VERIFY_P2SH : SCRIPT_VERIFY_NONE;

    // Start enforcing the DERSIG (BIP66) rules, for block.nVersion=3 blocks,
    // when 75% of the network has upgraded:
    if (block.nVersion >= 3 && IsSuperMajority(3, pindex->pprev, chainparams.GetConsensus().nMajorityEnforceBlockUpgrade, chainparams.GetConsensus())) {
        flags |= SCRIPT_VERIFY_DERSIG;
    }

    // Start enforcing CHECKLOCKTIMEVERIFY, (BIP65) for block.nVersion=4
    // blocks, when 75% of the network has upgraded:
    if (block.nVersion >= 4 && IsSuperMajority(4, pindex->pprev, chainparams.GetConsensus().nMajorityEnforceBlockUpgrade, chainparams.GetConsensus())) {
        flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
    }

    // Start enforcing BIP68 (sequence locks) and BIP112 (CHECKSEQUENCEVERIFY) using versionbits logic.
    int nLockTimeFlags = 0;
    if (VersionBitsState(pindex->pprev, chainparams.GetConsensus(), Consensus::DEPLOYMENT_CSV, versionbitscache) == THRESHOLD_ACTIVE) {
        flags |= SCRIPT_VERIFY_CHECKSEQUENCEVERIFY;
        nLockTimeFlags |= LOCKTIME_VERIFY_SEQUENCE;
    }

    // Start enforcing WITNESS rules using versionbits logic.
    if (IsWitnessEnabled(pindex->pprev, chainparams.GetConsensus())) {
        flags |= SCRIPT_VERIFY_WITNESS;
        flags |= SCRIPT_VERIFY_NULLDUMMY;
    }

    int64_t nTime2 = GetTimeMicros(); nTimeForks += nTime2 - nTime1;
    LogPrint("bench", "    - Fork checks: %.2fms [%.2fs]\n", 0.001 * (nTime2 - nTime1), nTimeForks * 0.000001);

    CBlockUndo blockundo;

    CCheckQueueControl<CScriptCheck> control(fScriptChecks && nScriptCheckThreads ? &scriptcheckqueue : NULL);

    std::vector<uint256> vOrphanErase;
    std::vector<int> prevheights;
    CAmount nFees = 0;
    int nInputs = 0;
    int64_t nSigOpsCost = 0;
    CDiskTxPos pos(pindex->GetBlockPos(), GetSizeOfCompactSize(block.vtx.size()));
    std::vector<std::pair<uint256, CDiskTxPos> > vPos;
    vPos.reserve(block.vtx.size());
    blockundo.vtxundo.reserve(block.vtx.size() - 1);
    std::vector<PrecomputedTransactionData> txdata;
    txdata.reserve(block.vtx.size()); // Required so that pointers to individual PrecomputedTransactionData don't get invalidated
    for (unsigned int i = 0; i < block.vtx.size(); i++)
    {
        printf(" block.vtx[i] = %u \n",  block.vtx[i]);
        const CTransaction &tx = block.vtx[i];

        nInputs += tx.vin.size();

        if (!tx.IsCoinBase())
        {
            if (!view.HaveInputs(tx))
                return state.DoS(100, error("ConnectBlock(): inputs missing/spent"),
                                 REJECT_INVALID, "bad-txns-inputs-missingorspent");

            // Check that transaction is BIP68 final
            // BIP68 lock checks (as opposed to nLockTime checks) must
            // be in ConnectBlock because they require the UTXO set
            prevheights.resize(tx.vin.size());
            for (size_t j = 0; j < tx.vin.size(); j++) {
                prevheights[j] = view.AccessCoins(tx.vin[j].prevout.hash)->nHeight;
            }

            // Which orphan pool entries must we evict?
            for (size_t j = 0; j < tx.vin.size(); j++) {
                auto itByPrev = mapOrphanTransactionsByPrev.find(tx.vin[j].prevout);
                if (itByPrev == mapOrphanTransactionsByPrev.end()) continue;
                for (auto mi = itByPrev->second.begin(); mi != itByPrev->second.end(); ++mi) {
                    const CTransaction& orphanTx = (*mi)->second.tx;
                    const uint256& orphanHash = orphanTx.GetHash();
                    vOrphanErase.push_back(orphanHash);
                }
            }

            if (!SequenceLocks(tx, nLockTimeFlags, &prevheights, *pindex)) {
                return state.DoS(100, error("%s: contains a non-BIP68-final transaction", __func__),
                                 REJECT_INVALID, "bad-txns-nonfinal");
            }
        }

        // GetTransactionSigOpCost counts 3 types of sigops:
        // * legacy (always)
        // * p2sh (when P2SH enabled in flags and excludes coinbase)
        // * witness (when witness enabled in flags and excludes coinbase)
        nSigOpsCost += GetTransactionSigOpCost(tx, view, flags);
        if (nSigOpsCost > MAX_BLOCK_SIGOPS_COST)
            return state.DoS(100, error("ConnectBlock(): too many sigops"),
                             REJECT_INVALID, "bad-blk-sigops");

        txdata.emplace_back(tx);
        if (!tx.IsCoinBase())
        {
            nFees += view.GetValueIn(tx)-tx.GetValueOut();

            std::vector<CScriptCheck> vChecks;
            bool fCacheResults = fJustCheck; /* Don't cache results if we're actually connecting blocks (still consult the cache, though) */
            if (!CheckInputs(tx, state, view, fScriptChecks, flags, fCacheResults, txdata[i], nScriptCheckThreads ? &vChecks : NULL))
                return error("ConnectBlock(): CheckInputs on %s failed with %s",
                    tx.GetHash().ToString(), FormatStateMessage(state));
            control.Add(vChecks);
        }

        CTxUndo undoDummy;
        if (i > 0) {
            blockundo.vtxundo.push_back(CTxUndo());
        }
        UpdateCoins(tx, view, i == 0 ? undoDummy : blockundo.vtxundo.back(), pindex->nHeight);

        vPos.push_back(std::make_pair(tx.GetHash(), pos));
        pos.nTxOffset += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);
    }
    int64_t nTime3 = GetTimeMicros(); nTimeConnect += nTime3 - nTime2;
    LogPrint("bench", "      - Connect %u transactions: %.2fms (%.3fms/tx, %.3fms/txin) [%.2fs]\n", (unsigned)block.vtx.size(), 0.001 * (nTime3 - nTime2), 0.001 * (nTime3 - nTime2) / block.vtx.size(), nInputs <= 1 ? 0 : 0.001 * (nTime3 - nTime2) / (nInputs-1), nTimeConnect * 0.000001);

    // Grab previous block hash
    uint256 prevHash;
    if(pindex->pprev)
    {
        prevHash = pindex->pprev->GetBlockHash();
    }
   
    CAmount blockReward = nFees + GetBlockSubsidy(pindex->nHeight, prevHash);
    //Logging Added .13.9.1
    //printf("blockReward = %u \n", blockReward);
   // printf("nFees = %u \n", nFees);
   // printf("pindex->nHeight = %u \n", pindex->nHeight);
   // printf("prevHash = %u \n", prevHash.ToString().c_str());
   // printf("block.vtx[0].GetValueOut() = %u \n", block.vtx[0].GetValueOut());
   // printf("block.vtx[0].ToString() = %u \n", block.vtx[0].ToString());
    
    
    if (block.vtx[0].GetValueOut() > blockReward)
        return state.DoS(100,
                         error("ConnectBlock(): coinbase pays too much (actual=%d vs limit=%d)",
                               block.vtx[0].GetValueOut(), blockReward),
                               REJECT_INVALID, "bad-cb-amount");

    if (!control.Wait())
        return state.DoS(100, false);
    int64_t nTime4 = GetTimeMicros(); nTimeVerify += nTime4 - nTime2;
    LogPrint("bench", "    - Verify %u txins: %.2fms (%.3fms/txin) [%.2fs]\n", nInputs - 1, 0.001 * (nTime4 - nTime2), nInputs <= 1 ? 0 : 0.001 * (nTime4 - nTime2) / (nInputs-1), nTimeVerify * 0.000001);

    if (fJustCheck)
        return true;

    // Write undo information to disk
    if (pindex->GetUndoPos().IsNull() || !pindex->IsValid(BLOCK_VALID_SCRIPTS))
    {
        if (pindex->GetUndoPos().IsNull()) {
            CDiskBlockPos pos;
            if (!FindUndoPos(state, pindex->nFile, pos, ::GetSerializeSize(blockundo, SER_DISK, CLIENT_VERSION) + 40))
                return error("ConnectBlock(): FindUndoPos failed");
            if (!UndoWriteToDisk(blockundo, pos, pindex->pprev->GetBlockHash(), chainparams.MessageStart()))
                return AbortNode(state, "Failed to write undo data");

            // update nUndoPos in block index
            pindex->nUndoPos = pos.nPos;
            pindex->nStatus |= BLOCK_HAVE_UNDO;
        }

        pindex->RaiseValidity(BLOCK_VALID_SCRIPTS);
        setDirtyBlockIndex.insert(pindex);
    }

    if (fTxIndex)
        if (!pblocktree->WriteTxIndex(vPos))
            return AbortNode(state, "Failed to write transaction index");

    // add this block to the view's block chain
    view.SetBestBlock(pindex->GetBlockHash());

    int64_t nTime5 = GetTimeMicros(); nTimeIndex += nTime5 - nTime4;
    LogPrint("bench", "    - Index writing: %.2fms [%.2fs]\n", 0.001 * (nTime5 - nTime4), nTimeIndex * 0.000001);

    // Watch for changes to the previous coinbase transaction.
    static uint256 hashPrevBestCoinBase;
    GetMainSignals().UpdatedTransaction(hashPrevBestCoinBase);
    hashPrevBestCoinBase = block.vtx[0].GetHash();

    // Erase orphan transactions include or precluded by this block
    if (vOrphanErase.size()) {
        int nErased = 0;
        BOOST_FOREACH(uint256 &orphanHash, vOrphanErase) {
            nErased += EraseOrphanTx(orphanHash);
        }
        LogPrint("mempool", "Erased %d orphan tx included or conflicted by block\n", nErased);
    }

    int64_t nTime6 = GetTimeMicros(); nTimeCallbacks += nTime6 - nTime5;
    LogPrint("bench", "    - Callbacks: %.2fms [%.2fs]\n", 0.001 * (nTime6 - nTime5), nTimeCallbacks * 0.000001);

    return true;
}

enum FlushStateMode {
    FLUSH_STATE_NONE,
    FLUSH_STATE_IF_NEEDED,
    FLUSH_STATE_PERIODIC,
    FLUSH_STATE_ALWAYS
};

/**
 * Update the on-disk chain state.
 * The caches and indexes are flushed depending on the mode we're called with
 * if they're too large, if it's been a while since the last write,
 * or always and in all cases if we're in prune mode and are deleting files.
 */
bool static FlushStateToDisk(CValidationState &state, FlushStateMode mode) {
    const CChainParams& chainparams = Params();
    LOCK2(cs_main, cs_LastBlockFile);
    static int64_t nLastWrite = 0;
    static int64_t nLastFlush = 0;
    static int64_t nLastSetChain = 0;
    std::set<int> setFilesToPrune;
    bool fFlushForPrune = false;
    try {
    if (fPruneMode && fCheckForPruning && !fReindex) {
        FindFilesToPrune(setFilesToPrune, chainparams.PruneAfterHeight());
        fCheckForPruning = false;
        if (!setFilesToPrune.empty()) {
            fFlushForPrune = true;
            if (!fHavePruned) {
                pblocktree->WriteFlag("prunedblockfiles", true);
                fHavePruned = true;
            }
        }
    }
    int64_t nNow = GetTimeMicros();
    // Avoid writing/flushing immediately after startup.
    if (nLastWrite == 0) {
        nLastWrite = nNow;
    }
    if (nLastFlush == 0) {
        nLastFlush = nNow;
    }
    if (nLastSetChain == 0) {
        nLastSetChain = nNow;
    }
    size_t cacheSize = pcoinsTip->DynamicMemoryUsage();
    // The cache is large and close to the limit, but we have time now (not in the middle of a block processing).
    bool fCacheLarge = mode == FLUSH_STATE_PERIODIC && cacheSize * (10.0/9) > nCoinCacheUsage;
    // The cache is over the limit, we have to write now.
    bool fCacheCritical = mode == FLUSH_STATE_IF_NEEDED && cacheSize > nCoinCacheUsage;
    // It's been a while since we wrote the block index to disk. Do this frequently, so we don't need to redownload after a crash.
    bool fPeriodicWrite = mode == FLUSH_STATE_PERIODIC && nNow > nLastWrite + (int64_t)DATABASE_WRITE_INTERVAL * 1000000;
    // It's been very long since we flushed the cache. Do this infrequently, to optimize cache usage.
    bool fPeriodicFlush = mode == FLUSH_STATE_PERIODIC && nNow > nLastFlush + (int64_t)DATABASE_FLUSH_INTERVAL * 1000000;
    // Combine all conditions that result in a full cache flush.
    bool fDoFullFlush = (mode == FLUSH_STATE_ALWAYS) || fCacheLarge || fCacheCritical || fPeriodicFlush || fFlushForPrune;
    // Write blocks and block index to disk.
    if (fDoFullFlush || fPeriodicWrite) {
        // Depend on nMinDiskSpace to ensure we can write block index
        if (!CheckDiskSpace(0))
            return state.Error("out of disk space");
        // First make sure all block and undo data is flushed to disk.
        FlushBlockFile();
        // Then update all block file information (which may refer to block and undo files).
        {
            std::vector<std::pair<int, const CBlockFileInfo*> > vFiles;
            vFiles.reserve(setDirtyFileInfo.size());
            for (set<int>::iterator it = setDirtyFileInfo.begin(); it != setDirtyFileInfo.end(); ) {
                vFiles.push_back(make_pair(*it, &vinfoBlockFile[*it]));
                setDirtyFileInfo.erase(it++);
            }
            std::vector<const CBlockIndex*> vBlocks;
            vBlocks.reserve(setDirtyBlockIndex.size());
            for (set<CBlockIndex*>::iterator it = setDirtyBlockIndex.begin(); it != setDirtyBlockIndex.end(); ) {
                vBlocks.push_back(*it);
                setDirtyBlockIndex.erase(it++);
            }
            if (!pblocktree->WriteBatchSync(vFiles, nLastBlockFile, vBlocks)) {
                return AbortNode(state, "Files to write to block index database");
            }
        }
        // Finally remove any pruned files
        if (fFlushForPrune)
            UnlinkPrunedFiles(setFilesToPrune);
        nLastWrite = nNow;
    }
    // Flush best chain related state. This can only be done if the blocks / block index write was also done.
    if (fDoFullFlush) {
        // Typical CCoins structures on disk are around 128 bytes in size.
        // Pushing a new one to the database can cause it to be written
        // twice (once in the log, and once in the tables). This is already
        // an overestimation, as most will delete an existing entry or
        // overwrite one. Still, use a conservative safety factor of 2.
        if (!CheckDiskSpace(128 * 2 * 2 * pcoinsTip->GetCacheSize()))
            return state.Error("out of disk space");
        // Flush the chainstate (which may refer to block index entries).
        if (!pcoinsTip->Flush())
            return AbortNode(state, "Failed to write to coin database");
        nLastFlush = nNow;
    }
    if (fDoFullFlush || ((mode == FLUSH_STATE_ALWAYS || mode == FLUSH_STATE_PERIODIC) && nNow > nLastSetChain + (int64_t)DATABASE_WRITE_INTERVAL * 1000000)) {
        // Update best block in wallet (so we can detect restored wallets).
        GetMainSignals().SetBestChain(chainActive.GetLocator());
        nLastSetChain = nNow;
    }
    } catch (const std::runtime_error& e) {
        return AbortNode(state, std::string("System error while flushing: ") + e.what());
    }
    return true;
}

void FlushStateToDisk() {
    CValidationState state;
    FlushStateToDisk(state, FLUSH_STATE_ALWAYS);
}

void PruneAndFlush() {
    CValidationState state;
    fCheckForPruning = true;
    FlushStateToDisk(state, FLUSH_STATE_NONE);
}

/** Update chainActive and related internal data structures. */
void static UpdateTip(CBlockIndex *pindexNew, const CChainParams& chainParams) {
    chainActive.SetTip(pindexNew);

    string algoName = "scrypt";

    // New best block
    nTimeBestReceived = GetTime();
    mempool.AddTransactionsUpdated(1);

    cvBlockChange.notify_all();

    static bool fWarned = false;
    std::vector<std::string> warningMessages;
    if (!IsInitialBlockDownload())
    {
        int nUpgraded = 0;
        const CBlockIndex* pindex = chainActive.Tip();
        for (int bit = 0; bit < VERSIONBITS_NUM_BITS; bit++) {
            WarningBitsConditionChecker checker(bit);
            ThresholdState state = checker.GetStateFor(pindex, chainParams.GetConsensus(), warningcache[bit]);
            if (state == THRESHOLD_ACTIVE || state == THRESHOLD_LOCKED_IN) {
                if (state == THRESHOLD_ACTIVE) {
                    strMiscWarning = strprintf(_("Warning: unknown new rules activated (versionbit %i)"), bit);
                    if (!fWarned) {
                        AlertNotify(strMiscWarning);
                        fWarned = true;
                    }
                } else {
                    warningMessages.push_back(strprintf("unknown new rules are about to activate (versionbit %i)", bit));
                }
            }
        }
        // Check the version of the last 100 blocks to see if we need to upgrade:
        for (int i = 0; i < 100 && pindex != NULL; i++)
        {
            int32_t nExpectedVersion = ComputeBlockVersion(pindex->pprev, chainParams.GetConsensus());
            if (pindex->nVersion > VERSIONBITS_LAST_OLD_BLOCK_VERSION && (pindex->nVersion & ~nExpectedVersion) != 0)
                ++nUpgraded;
            pindex = pindex->pprev;
        }
        if (nUpgraded > 0)
            warningMessages.push_back(strprintf("%d of last 100 blocks have unexpected version", nUpgraded));
        if (nUpgraded > 100/2)
        {
            // strMiscWarning is read by GetWarnings(), called by Qt and the JSON-RPC code to warn the user:
            strMiscWarning = _("Warning: Unknown block versions being mined! It's possible unknown rules are in effect");
            if (!fWarned) {
                AlertNotify(strMiscWarning);
                fWarned = true;
            }
        }
    }

    // time the last 1000 blocks imported
    if(chainActive.Height() % 1000 == 0){
     if(LastBlocks!=-1){
        LogPrintf("** last 1000 blks took %ds to sync (%ds since init)\n", ((GetTickCounts()-LastBlocks)/1000), ((GetTickCounts()-ClientUptime)/1000));
        LastBlocks = GetTickCounts();
     } else {
        LastBlocks = GetTickCounts();
     }
    }
    
    // debug for algo switcher
    //if(AlgoSwitch)
       //algoName="balloon";
    
    if((chainActive.Height() % 250 == 0 && chainActive.Height() < 1125000) || (chainActive.Height() > 1125000)){
     LogPrintf("%s: hash=%s.. height=%d diff=%08x algo=%s ver=0x%08x log2_work=%.8g tx=%lu date='%s' prog=%f cache=%.1fMiB(%utx)", __func__,
       chainActive.Tip()->GetBlockHash().ToString().substr(0,9), chainActive.Height(), chainActive.Tip()->nBits, algoName, chainActive.Tip()->nVersion,
       log(chainActive.Tip()->nChainWork.getdouble())/log(2.0), (unsigned long)chainActive.Tip()->nChainTx,
       DateTimeStrFormat("%Y-%m-%d %H:%M:%S", chainActive.Tip()->GetBlockTime()),
       Checkpoints::GuessVerificationProgress(chainParams.Checkpoints(), chainActive.Tip()), pcoinsTip->DynamicMemoryUsage() * (1.0 / (1<<20)), pcoinsTip->GetCacheSize());
     if (!warningMessages.empty())
       LogPrintf(" warning='%s'", boost::algorithm::join(warningMessages, ", "));
     LogPrintf("\n");
    }

}

/** Disconnect chainActive's tip. You probably want to call mempool.removeForReorg and manually re-limit mempool size after this, with cs_main held. */
bool static DisconnectTip(CValidationState& state, const CChainParams& chainparams, bool fBare = false)
{
    CBlockIndex *pindexDelete = chainActive.Tip();
    assert(pindexDelete);
    // Read block from disk.
    CBlock block;
    if (!ReadBlockFromDisk(block, pindexDelete, chainparams.GetConsensus()))
        return AbortNode(state, "Failed to read block");
    // Apply the block atomically to the chain state.
    int64_t nStart = GetTimeMicros();
    {
        CCoinsViewCache view(pcoinsTip);
        if (!DisconnectBlock(block, state, pindexDelete, view))
            return error("DisconnectTip(): DisconnectBlock %s failed", pindexDelete->GetBlockHash().ToString());
        assert(view.Flush());
    }
    LogPrint("bench", "- Disconnect block: %.2fms\n", (GetTimeMicros() - nStart) * 0.001);
    // Write the chain state to disk, if necessary.
    if (!FlushStateToDisk(state, FLUSH_STATE_IF_NEEDED))
        return false;

    if (!fBare) {
        // Resurrect mempool transactions from the disconnected block.
        std::vector<uint256> vHashUpdate;
        BOOST_FOREACH(const CTransaction &tx, block.vtx) {
            // ignore validation errors in resurrected transactions
            list<CTransaction> removed;
            CValidationState stateDummy;
            if (tx.IsCoinBase() || !AcceptToMemoryPool(mempool, stateDummy, tx, false, NULL, true)) {
                mempool.removeRecursive(tx, removed);
            } else if (mempool.exists(tx.GetHash())) {
                vHashUpdate.push_back(tx.GetHash());
            }
        }
        // AcceptToMemoryPool/addUnchecked all assume that new mempool entries have
        // no in-mempool children, which is generally not true when adding
        // previously-confirmed transactions back to the mempool.
        // UpdateTransactionsFromBlock finds descendants of any transactions in this
        // block that were added back and cleans up the mempool state.
        mempool.UpdateTransactionsFromBlock(vHashUpdate);
    }

    // Update chainActive and related variables.
    UpdateTip(pindexDelete->pprev, chainparams);
    // Let wallets know transactions went from 1-confirmed to
    // 0-confirmed or conflicted:
    BOOST_FOREACH(const CTransaction &tx, block.vtx) {
        SyncWithWallets(tx, pindexDelete->pprev, NULL);
    }
    return true;
}

static int64_t nTimeReadFromDisk = 0;
static int64_t nTimeConnectTotal = 0;
static int64_t nTimeFlush = 0;
static int64_t nTimeChainState = 0;
static int64_t nTimePostConnect = 0;

/**
 * Connect a new block to chainActive. pblock is either NULL or a pointer to a CBlock
 * corresponding to pindexNew, to bypass loading it again from disk.
 */
bool static ConnectTip(CValidationState& state, const CChainParams& chainparams, CBlockIndex* pindexNew, const CBlock* pblock)
{
    assert(pindexNew->pprev == chainActive.Tip());
    // Read block from disk.
    int64_t nTime1 = GetTimeMicros();
    CBlock block;
    if (!pblock) {
        if (!ReadBlockFromDisk(block, pindexNew, chainparams.GetConsensus()))
            return AbortNode(state, "Failed to read block");
        pblock = &block;
    }
    // Apply the block atomically to the chain state.
    int64_t nTime2 = GetTimeMicros(); nTimeReadFromDisk += nTime2 - nTime1;
    int64_t nTime3;
    LogPrint("bench", "  - Load block from disk: %.2fms [%.2fs]\n", (nTime2 - nTime1) * 0.001, nTimeReadFromDisk * 0.000001);
    {
        CCoinsViewCache view(pcoinsTip);
        bool rv = ConnectBlock(*pblock, state, pindexNew, view, chainparams);
        GetMainSignals().BlockChecked(*pblock, state);
        if (!rv) {
            if (state.IsInvalid())
                InvalidBlockFound(pindexNew, state);
            return error("ConnectTip(): ConnectBlock %s failed", pindexNew->GetBlockHash().ToString());
        }
        mapBlockSource.erase(pindexNew->GetBlockHash());
        nTime3 = GetTimeMicros(); nTimeConnectTotal += nTime3 - nTime2;
        LogPrint("bench", "  - Connect total: %.2fms [%.2fs]\n", (nTime3 - nTime2) * 0.001, nTimeConnectTotal * 0.000001);
        assert(view.Flush());
    }
    int64_t nTime4 = GetTimeMicros(); nTimeFlush += nTime4 - nTime3;
    LogPrint("bench", "  - Flush: %.2fms [%.2fs]\n", (nTime4 - nTime3) * 0.001, nTimeFlush * 0.000001);
    // Write the chain state to disk, if necessary.
    if (!FlushStateToDisk(state, FLUSH_STATE_IF_NEEDED))
        return false;
    int64_t nTime5 = GetTimeMicros(); nTimeChainState += nTime5 - nTime4;
    LogPrint("bench", "  - Writing chainstate: %.2fms [%.2fs]\n", (nTime5 - nTime4) * 0.001, nTimeChainState * 0.000001);
    // Remove conflicting transactions from the mempool.
    list<CTransaction> txConflicted;
    mempool.removeForBlock(pblock->vtx, pindexNew->nHeight, txConflicted, !IsInitialBlockDownload());
    // Update chainActive & related variables.
    UpdateTip(pindexNew, chainparams);
    // Tell wallet about transactions that went from mempool
    // to conflicted:
    BOOST_FOREACH(const CTransaction &tx, txConflicted) {
        SyncWithWallets(tx, pindexNew, NULL);
    }
    // ... and about transactions that got confirmed:
    BOOST_FOREACH(const CTransaction &tx, pblock->vtx) {
        SyncWithWallets(tx, pindexNew, pblock);
    }

    int64_t nTime6 = GetTimeMicros(); nTimePostConnect += nTime6 - nTime5; nTimeTotal += nTime6 - nTime1;
    LogPrint("bench", "  - Connect postprocess: %.2fms [%.2fs]\n", (nTime6 - nTime5) * 0.001, nTimePostConnect * 0.000001);
    LogPrint("bench", "- Connect block: %.2fms [%.2fs]\n", (nTime6 - nTime1) * 0.001, nTimeTotal * 0.000001);
    return true;
}

/**
 * Return the tip of the chain with the most work in it, that isn't
 * known to be invalid (it's however far from certain to be valid).
 */
static CBlockIndex* FindMostWorkChain() {
    do {
        CBlockIndex *pindexNew = NULL;

        // Find the best candidate header.
        {
            std::set<CBlockIndex*, CBlockIndexWorkComparator>::reverse_iterator it = setBlockIndexCandidates.rbegin();
            if (it == setBlockIndexCandidates.rend())
                return NULL;
            pindexNew = *it;
        }

        // Check whether all blocks on the path between the currently active chain and the candidate are valid.
        // Just going until the active chain is an optimization, as we know all blocks in it are valid already.
        CBlockIndex *pindexTest = pindexNew;
        bool fInvalidAncestor = false;
        while (pindexTest && !chainActive.Contains(pindexTest)) {
            assert(pindexTest->nChainTx || pindexTest->nHeight == 0);

            // Pruned nodes may have entries in setBlockIndexCandidates for
            // which block files have been deleted.  Remove those as candidates
            // for the most work chain if we come across them; we can't switch
            // to a chain unless we have all the non-active-chain parent blocks.
            bool fFailedChain = pindexTest->nStatus & BLOCK_FAILED_MASK;
            bool fMissingData = !(pindexTest->nStatus & BLOCK_HAVE_DATA);
            if (fFailedChain || fMissingData) {
                // Candidate chain is not usable (either invalid or missing data)
                if (fFailedChain && (pindexBestInvalid == NULL || pindexNew->nChainWork > pindexBestInvalid->nChainWork))
                    pindexBestInvalid = pindexNew;
                CBlockIndex *pindexFailed = pindexNew;
                // Remove the entire chain from the set.
                while (pindexTest != pindexFailed) {
                    if (fFailedChain) {
                        pindexFailed->nStatus |= BLOCK_FAILED_CHILD;
                    } else if (fMissingData) {
                        // If we're missing data, then add back to mapBlocksUnlinked,
                        // so that if the block arrives in the future we can try adding
                        // to setBlockIndexCandidates again.
                        mapBlocksUnlinked.insert(std::make_pair(pindexFailed->pprev, pindexFailed));
                    }
                    setBlockIndexCandidates.erase(pindexFailed);
                    pindexFailed = pindexFailed->pprev;
                }
                setBlockIndexCandidates.erase(pindexTest);
                fInvalidAncestor = true;
                break;
            }
            pindexTest = pindexTest->pprev;
        }
        if (!fInvalidAncestor)
            return pindexNew;
    } while(true);
}

/** Delete all entries in setBlockIndexCandidates that are worse than the current tip. */
static void PruneBlockIndexCandidates() {
    // Note that we can't delete the current block itself, as we may need to return to it later in case a
    // reorganization to a better block fails.
    std::set<CBlockIndex*, CBlockIndexWorkComparator>::iterator it = setBlockIndexCandidates.begin();
    while (it != setBlockIndexCandidates.end() && setBlockIndexCandidates.value_comp()(*it, chainActive.Tip())) {
        setBlockIndexCandidates.erase(it++);
    }
    // Either the current tip or a successor of it we're working towards is left in setBlockIndexCandidates.
    assert(!setBlockIndexCandidates.empty());
}

/**
 * Try to make some progress towards making pindexMostWork the active block.
 * pblock is either NULL or a pointer to a CBlock corresponding to pindexMostWork.
 */
static bool ActivateBestChainStep(CValidationState& state, const CChainParams& chainparams, CBlockIndex* pindexMostWork, const CBlock* pblock, bool& fInvalidFound)
{
    AssertLockHeld(cs_main);
    const CBlockIndex *pindexOldTip = chainActive.Tip();
    const CBlockIndex *pindexFork = chainActive.FindFork(pindexMostWork);

    // Disconnect active blocks which are no longer in the best chain.
    bool fBlocksDisconnected = false;
    while (chainActive.Tip() && chainActive.Tip() != pindexFork) {
        if (!DisconnectTip(state, chainparams))
            return false;
        fBlocksDisconnected = true;
    }

    // Build list of new blocks to connect.
    std::vector<CBlockIndex*> vpindexToConnect;
    bool fContinue = true;
    int nHeight = pindexFork ? pindexFork->nHeight : -1;
    while (fContinue && nHeight != pindexMostWork->nHeight) {
        // Don't iterate the entire list of potential improvements toward the best tip, as we likely only need
        // a few blocks along the way.
        int nTargetHeight = std::min(nHeight + 32, pindexMostWork->nHeight);
        vpindexToConnect.clear();
        vpindexToConnect.reserve(nTargetHeight - nHeight);
        CBlockIndex *pindexIter = pindexMostWork->GetAncestor(nTargetHeight);
        while (pindexIter && pindexIter->nHeight != nHeight) {
            vpindexToConnect.push_back(pindexIter);
            pindexIter = pindexIter->pprev;
        }
        nHeight = nTargetHeight;

        // Connect new blocks.
        BOOST_REVERSE_FOREACH(CBlockIndex *pindexConnect, vpindexToConnect) {
            if (!ConnectTip(state, chainparams, pindexConnect, pindexConnect == pindexMostWork ? pblock : NULL)) {
                if (state.IsInvalid()) {
                    // The block violates a consensus rule.
                    if (!state.CorruptionPossible())
                        InvalidChainFound(vpindexToConnect.back());
                    state = CValidationState();
                    fInvalidFound = true;
                    fContinue = false;
                    break;
                } else {
                    // A system error occurred (disk space, database error, ...).
                    return false;
                }
            } else {
                PruneBlockIndexCandidates();
                if (!pindexOldTip || chainActive.Tip()->nChainWork > pindexOldTip->nChainWork) {
                    // We're in a better position than we were. Return temporarily to release the lock.
                    fContinue = false;
                    break;
                }
            }
        }
    }

    if (fBlocksDisconnected) {
        mempool.removeForReorg(pcoinsTip, chainActive.Tip()->nHeight + 1, STANDARD_LOCKTIME_VERIFY_FLAGS);
        LimitMempoolSize(mempool, GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000, GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60);
    }
    mempool.check(pcoinsTip);

    // Callbacks/notifications for a new best chain.
    if (fInvalidFound)
        CheckForkWarningConditionsOnNewFork(vpindexToConnect.back());
    else
        CheckForkWarningConditions();

    return true;
}

static void NotifyHeaderTip() {
    bool fNotify = false;
    bool fInitialBlockDownload = false;
    static CBlockIndex* pindexHeaderOld = NULL;
    CBlockIndex* pindexHeader = NULL;
    {
        LOCK(cs_main);
        if (!setBlockIndexCandidates.empty()) {
            pindexHeader = *setBlockIndexCandidates.rbegin();
        }
        if (pindexHeader != pindexHeaderOld) {
            fNotify = true;
            fInitialBlockDownload = IsInitialBlockDownload();
            pindexHeaderOld = pindexHeader;
        }
    }
    // Send block tip changed notifications without cs_main
    if (fNotify) {
        uiInterface.NotifyHeaderTip(fInitialBlockDownload, pindexHeader);
    }
}

/**
 * Make the best chain active, in multiple steps. The result is either failure
 * or an activated best chain. pblock is either NULL or a pointer to a block
 * that is already loaded (to avoid loading it again from disk).
 */
bool ActivateBestChain(CValidationState &state, const CChainParams& chainparams, const CBlock *pblock) {
    CBlockIndex *pindexMostWork = NULL;
    CBlockIndex *pindexNewTip = NULL;
    do {
        boost::this_thread::interruption_point();
        if (ShutdownRequested())
            break;

        const CBlockIndex *pindexFork;
        bool fInitialDownload;
        int nNewHeight;
        {
            LOCK(cs_main);
            CBlockIndex *pindexOldTip = chainActive.Tip();
            if (pindexMostWork == NULL) {
                pindexMostWork = FindMostWorkChain();
            }

            // Whether we have anything to do at all.
            if (pindexMostWork == NULL || pindexMostWork == chainActive.Tip())
                return true;

            bool fInvalidFound = false;
            if (!ActivateBestChainStep(state, chainparams, pindexMostWork, pblock && pblock->GetHash() == pindexMostWork->GetBlockHash() ? pblock : NULL, fInvalidFound))
                return false;

            if (fInvalidFound) {
                // Wipe cache, we may need another branch now.
                pindexMostWork = NULL;
            }
            pindexNewTip = chainActive.Tip();
            pindexFork = chainActive.FindFork(pindexOldTip);
            fInitialDownload = IsInitialBlockDownload();
            nNewHeight = chainActive.Height();
        }
        // When we reach this point, we switched to a new tip (stored in pindexNewTip).

        // Notifications/callbacks that can run without cs_main
        // Always notify the UI if a new block tip was connected
        if (pindexFork != pindexNewTip) {
            uiInterface.NotifyBlockTip(fInitialDownload, pindexNewTip);

            if (!fInitialDownload) {
                // Find the hashes of all blocks that weren't previously in the best chain.
                std::vector<uint256> vHashes;
                CBlockIndex *pindexToAnnounce = pindexNewTip;
                while (pindexToAnnounce != pindexFork) {
                    vHashes.push_back(pindexToAnnounce->GetBlockHash());
                    pindexToAnnounce = pindexToAnnounce->pprev;
                    if (vHashes.size() == MAX_BLOCKS_TO_ANNOUNCE) {
                        // Limit announcements in case of a huge reorganization.
                        // Rely on the peer's synchronization mechanism in that case.
                        break;
                    }
                }
                // Relay inventory, but don't relay old inventory during initial block download.
                {
                    LOCK(cs_vNodes);
                    BOOST_FOREACH(CNode* pnode, vNodes) {
                        if (nNewHeight > (pnode->nStartingHeight != -1 ? pnode->nStartingHeight - 2000 : 0)) {
                            BOOST_REVERSE_FOREACH(const uint256& hash, vHashes) {
                                pnode->PushBlockHash(hash);
                            }
                        }
                    }
                }
                // Notify external listeners about the new tip.
                if (!vHashes.empty()) {
                    GetMainSignals().UpdatedBlockTip(pindexNewTip);
                }
            }
        }
    } while (pindexNewTip != pindexMostWork);
    CheckBlockIndex(chainparams.GetConsensus());

    // Write changes periodically to disk, after relay.
    if (!FlushStateToDisk(state, FLUSH_STATE_PERIODIC)) {
        return false;
    }

    return true;
}

bool InvalidateBlock(CValidationState& state, const CChainParams& chainparams, CBlockIndex *pindex)
{
    AssertLockHeld(cs_main);

    // Mark the block itself as invalid.
    pindex->nStatus |= BLOCK_FAILED_VALID;
    setDirtyBlockIndex.insert(pindex);
    setBlockIndexCandidates.erase(pindex);

    while (chainActive.Contains(pindex)) {
        CBlockIndex *pindexWalk = chainActive.Tip();
        pindexWalk->nStatus |= BLOCK_FAILED_CHILD;
        setDirtyBlockIndex.insert(pindexWalk);
        setBlockIndexCandidates.erase(pindexWalk);
        // ActivateBestChain considers blocks already in chainActive
        // unconditionally valid already, so force disconnect away from it.
        if (!DisconnectTip(state, chainparams)) {
            mempool.removeForReorg(pcoinsTip, chainActive.Tip()->nHeight + 1, STANDARD_LOCKTIME_VERIFY_FLAGS);
            return false;
        }
    }

    LimitMempoolSize(mempool, GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000, GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60);

    // The resulting new best tip may not be in setBlockIndexCandidates anymore, so
    // add it again.
    BlockMap::iterator it = mapBlockIndex.begin();
    while (it != mapBlockIndex.end()) {
        if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) && it->second->nChainTx && !setBlockIndexCandidates.value_comp()(it->second, chainActive.Tip())) {
            setBlockIndexCandidates.insert(it->second);
        }
        it++;
    }

    InvalidChainFound(pindex);
    mempool.removeForReorg(pcoinsTip, chainActive.Tip()->nHeight + 1, STANDARD_LOCKTIME_VERIFY_FLAGS);
    uiInterface.NotifyBlockTip(IsInitialBlockDownload(), pindex->pprev);
    return true;
}

bool ResetBlockFailureFlags(CBlockIndex *pindex) {
    AssertLockHeld(cs_main);

    int nHeight = pindex->nHeight;

    // Remove the invalidity flag from this block and all its descendants.
    BlockMap::iterator it = mapBlockIndex.begin();
    while (it != mapBlockIndex.end()) {
        if (!it->second->IsValid() && it->second->GetAncestor(nHeight) == pindex) {
            it->second->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(it->second);
            if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) && it->second->nChainTx && setBlockIndexCandidates.value_comp()(chainActive.Tip(), it->second)) {
                setBlockIndexCandidates.insert(it->second);
            }
            if (it->second == pindexBestInvalid) {
                // Reset invalid block marker if it was pointing to one of those.
                pindexBestInvalid = NULL;
            }
        }
        it++;
    }

    // Remove the invalidity flag from all ancestors too.
    while (pindex != NULL) {
        if (pindex->nStatus & BLOCK_FAILED_MASK) {
            pindex->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(pindex);
        }
        pindex = pindex->pprev;
    }
    return true;
}

CBlockIndex* AddToBlockIndex(const CBlockHeader& block)
{
    // Check for duplicate
    uint256 hash = block.GetHash();
    BlockMap::iterator it = mapBlockIndex.find(hash);
    if (it != mapBlockIndex.end())
        return it->second;

    // Construct new block index object
    CBlockIndex* pindexNew = new CBlockIndex(block);
    assert(pindexNew);
    // We assign the sequence id to blocks only when the full data is available,
    // to avoid miners withholding blocks but broadcasting headers, to get a
    // competitive advantage.
    pindexNew->nSequenceId = 0;
    BlockMap::iterator mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);
    BlockMap::iterator miPrev = mapBlockIndex.find(block.hashPrevBlock);
    if (miPrev != mapBlockIndex.end())
    {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
        pindexNew->BuildSkip();
    }
    pindexNew->nChainWork = (pindexNew->pprev ? pindexNew->pprev->nChainWork : 0) + GetBlockProof(*pindexNew);
    pindexNew->RaiseValidity(BLOCK_VALID_TREE);
    if (pindexBestHeader == NULL || pindexBestHeader->nChainWork < pindexNew->nChainWork)
        pindexBestHeader = pindexNew;

    setDirtyBlockIndex.insert(pindexNew);

    return pindexNew;
}

/** Mark a block as having its data received and checked (up to BLOCK_VALID_TRANSACTIONS). */
bool ReceivedBlockTransactions(const CBlock &block, CValidationState& state, CBlockIndex *pindexNew, const CDiskBlockPos& pos)
{
    pindexNew->nTx = block.vtx.size();
    pindexNew->nChainTx = 0;
    pindexNew->nFile = pos.nFile;
    pindexNew->nDataPos = pos.nPos;
    pindexNew->nUndoPos = 0;
    pindexNew->nStatus |= BLOCK_HAVE_DATA;
    if (IsWitnessEnabled(pindexNew->pprev, Params().GetConsensus())) {
        pindexNew->nStatus |= BLOCK_OPT_WITNESS;
    }
    pindexNew->RaiseValidity(BLOCK_VALID_TRANSACTIONS);
    setDirtyBlockIndex.insert(pindexNew);

    if (pindexNew->pprev == NULL || pindexNew->pprev->nChainTx) {
        // If pindexNew is the genesis block or all parents are BLOCK_VALID_TRANSACTIONS.
        deque<CBlockIndex*> queue;
        queue.push_back(pindexNew);

        // Recursively process any descendant blocks that now may be eligible to be connected.
        while (!queue.empty()) {
            CBlockIndex *pindex = queue.front();
            queue.pop_front();
            pindex->nChainTx = (pindex->pprev ? pindex->pprev->nChainTx : 0) + pindex->nTx;
            {
                LOCK(cs_nBlockSequenceId);
                pindex->nSequenceId = nBlockSequenceId++;
            }
            if (chainActive.Tip() == NULL || !setBlockIndexCandidates.value_comp()(pindex, chainActive.Tip())) {
                setBlockIndexCandidates.insert(pindex);
            }
            std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> range = mapBlocksUnlinked.equal_range(pindex);
            while (range.first != range.second) {
                std::multimap<CBlockIndex*, CBlockIndex*>::iterator it = range.first;
                queue.push_back(it->second);
                range.first++;
                mapBlocksUnlinked.erase(it);
            }
        }
    } else {
        if (pindexNew->pprev && pindexNew->pprev->IsValid(BLOCK_VALID_TREE)) {
            mapBlocksUnlinked.insert(std::make_pair(pindexNew->pprev, pindexNew));
        }
    }

    return true;
}

bool FindBlockPos(CValidationState &state, CDiskBlockPos &pos, unsigned int nAddSize, unsigned int nHeight, uint64_t nTime, bool fKnown = false)
{
    LOCK(cs_LastBlockFile);

    unsigned int nFile = fKnown ? pos.nFile : nLastBlockFile;
    if (vinfoBlockFile.size() <= nFile) {
        vinfoBlockFile.resize(nFile + 1);
    }

    if (!fKnown) {
        while (vinfoBlockFile[nFile].nSize + nAddSize >= MAX_BLOCKFILE_SIZE) {
            nFile++;
            if (vinfoBlockFile.size() <= nFile) {
                vinfoBlockFile.resize(nFile + 1);
            }
        }
        pos.nFile = nFile;
        pos.nPos = vinfoBlockFile[nFile].nSize;
    }

    if ((int)nFile != nLastBlockFile) {
        if (!fKnown) {
            LogPrintf("Leaving block file %i: %s\n", nLastBlockFile, vinfoBlockFile[nLastBlockFile].ToString());
        }
        FlushBlockFile(!fKnown);
        nLastBlockFile = nFile;
    }

    vinfoBlockFile[nFile].AddBlock(nHeight, nTime);
    if (fKnown)
        vinfoBlockFile[nFile].nSize = std::max(pos.nPos + nAddSize, vinfoBlockFile[nFile].nSize);
    else
        vinfoBlockFile[nFile].nSize += nAddSize;

    if (!fKnown) {
        unsigned int nOldChunks = (pos.nPos + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        unsigned int nNewChunks = (vinfoBlockFile[nFile].nSize + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
        if (nNewChunks > nOldChunks) {
            if (fPruneMode)
                fCheckForPruning = true;
            if (CheckDiskSpace(nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos)) {
                FILE *file = OpenBlockFile(pos);
                if (file) {
                    LogPrintf("Pre-allocating up to position 0x%x in blk%05u.dat\n", nNewChunks * BLOCKFILE_CHUNK_SIZE, pos.nFile);
                    AllocateFileRange(file, pos.nPos, nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos);
                    fclose(file);
                }
            }
            else
                return state.Error("out of disk space");
        }
    }

    setDirtyFileInfo.insert(nFile);
    return true;
}

bool FindUndoPos(CValidationState &state, int nFile, CDiskBlockPos &pos, unsigned int nAddSize)
{
    pos.nFile = nFile;

    LOCK(cs_LastBlockFile);

    unsigned int nNewSize;
    pos.nPos = vinfoBlockFile[nFile].nUndoSize;
    nNewSize = vinfoBlockFile[nFile].nUndoSize += nAddSize;
    setDirtyFileInfo.insert(nFile);

    unsigned int nOldChunks = (pos.nPos + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    unsigned int nNewChunks = (nNewSize + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
    if (nNewChunks > nOldChunks) {
        if (fPruneMode)
            fCheckForPruning = true;
        if (CheckDiskSpace(nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos)) {
            FILE *file = OpenUndoFile(pos);
            if (file) {
                LogPrintf("Pre-allocating up to position 0x%x in rev%05u.dat\n", nNewChunks * UNDOFILE_CHUNK_SIZE, pos.nFile);
                AllocateFileRange(file, pos.nPos, nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos);
                fclose(file);
            }
        }
        else
            return state.Error("out of disk space");
    }

    return true;
}

bool CheckBlockHeader(const CBlockHeader& block, CValidationState& state, const Consensus::Params& consensusParams, bool fCheckPOW)
{
    // WarpSync check every 1/32th blocks pow hash (baz)
    int syncheight = GetHeight();
    if (syncheight > 1112500) { WarpSync=1; }
    if (syncheight % WarpSync == 0) {
        // Check proof of work matches claimed amount
        if (fCheckPOW && !CheckProofOfWork(block.GetPoWHash(AlgoSwitch), block.nBits, consensusParams, true)){
            if(WarpSync!=1){ LogPrintf("** HALTNG WarpSync DUE TO ERRORS ENCOUNTERED\n"); }
            //if(AlgoSwitch){
                //LogPrintf("** FAILED ON BALLOON (unrecoverable)\n");
                //return state.DoS(50, false, REJECT_INVALID, "high-hash", false, "proof of work failed (balloon pow)");
            //} else {
                // could be failed balloon block
                //if (fCheckPOW && !CheckProofOfWork(block.GetPoWHash(true), block.nBits, consensusParams, false)){
                    LogPrintf("** FAILED ON SCRYPT\n");
                    return state.DoS(50, false, REJECT_INVALID, "high-hash", false, "proof of work failed (scrypt pow)");
                //} else {
                    //LogPrintf("** ALGO SWITCHED TO BALLOON\n");
                    //AlgoSwitch=true;				
                //}
            //}
        }
    }
    return true;
}

bool CheckBlock(const CBlock& block, CValidationState& state, const Consensus::Params& consensusParams, bool fCheckPOW, bool fCheckMerkleRoot)
{
    // These are checks that are independent of context.

    if (block.fChecked)
        return true;

    // Check that the header is valid (particularly PoW).  This is mostly
    // redundant with the call in AcceptBlockHeader.
    if (!CheckBlockHeader(block, state, consensusParams, fCheckPOW))
        return false;

    // Check the merkle root.
    if (fCheckMerkleRoot) {
        bool mutated;
        uint256 hashMerkleRoot2 = BlockMerkleRoot(block, &mutated);
        if (block.hashMerkleRoot != hashMerkleRoot2)
            return state.DoS(100, false, REJECT_INVALID, "bad-txnmrklroot", true, "hashMerkleRoot mismatch");

        // Check for merkle tree malleability (CVE-2012-2459): repeating sequences
        // of transactions in a block without affecting the merkle root of a block,
        // while still invalidating it.
        if (mutated)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-duplicate", true, "duplicate transaction");
    }

    // All potential-corruption validation must be done before we do any
    // transaction validation, as otherwise we may mark the header as invalid
    // because we receive the wrong transactions for it.
    // Note that witness malleability is checked in ContextualCheckBlock, so no
    // checks that use witness data may be performed here.

    // Size limits
    if (block.vtx.empty() || block.vtx.size() > MAX_BLOCK_BASE_SIZE || ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS) > MAX_BLOCK_BASE_SIZE)
        return state.DoS(100, false, REJECT_INVALID, "bad-blk-length", false, "size limits failed");

    // First transaction must be coinbase, the rest must not be
    if (block.vtx.empty() || !block.vtx[0].IsCoinBase())
        return state.DoS(100, false, REJECT_INVALID, "bad-cb-missing", false, "first tx is not coinbase");
    for (unsigned int i = 1; i < block.vtx.size(); i++)
        if (block.vtx[i].IsCoinBase())
            return state.DoS(100, false, REJECT_INVALID, "bad-cb-multiple", false, "more than one coinbase");

    // Check transactions
    BOOST_FOREACH(const CTransaction& tx, block.vtx)
        if (!CheckTransaction(tx, state))
            return state.Invalid(false, state.GetRejectCode(), state.GetRejectReason(),
                                 strprintf("Transaction check failed (tx hash %s) %s", tx.GetHash().ToString(), state.GetDebugMessage()));

    unsigned int nSigOps = 0;
    BOOST_FOREACH(const CTransaction& tx, block.vtx)
    {
        nSigOps += GetLegacySigOpCount(tx);
    }
    if (nSigOps * WITNESS_SCALE_FACTOR > MAX_BLOCK_SIGOPS_COST)
        return state.DoS(100, false, REJECT_INVALID, "bad-blk-sigops", false, "out-of-bounds SigOpCount");

    if (fCheckPOW && fCheckMerkleRoot)
        block.fChecked = true;

    return true;
}

static bool CheckIndexAgainstCheckpoint(const CBlockIndex* pindexPrev, CValidationState& state, const CChainParams& chainparams, const uint256& hash)
{
    if (*pindexPrev->phashBlock == chainparams.GetConsensus().hashGenesisBlock)
        return true;

    int nHeight = pindexPrev->nHeight+1;
    // Don't accept any forks from the main chain prior to last checkpoint
    CBlockIndex* pcheckpoint = Checkpoints::GetLastCheckpoint(chainparams.Checkpoints());
    if (pcheckpoint && nHeight < pcheckpoint->nHeight)
        return state.DoS(100, error("%s: forked chain older than last checkpoint (height %d)", __func__, nHeight));

    return true;
}

bool IsWitnessEnabled(const CBlockIndex* pindexPrev, const Consensus::Params& params)
{
    LOCK(cs_main);
    return (VersionBitsState(pindexPrev, params, Consensus::DEPLOYMENT_SEGWIT, versionbitscache) == THRESHOLD_ACTIVE);
}

// Compute at which vout of the block's coinbase transaction the witness
// commitment occurs, or -1 if not found.
static int GetWitnessCommitmentIndex(const CBlock& block)
{
    int commitpos = -1;
    for (size_t o = 0; o < block.vtx[0].vout.size(); o++) {
        if (block.vtx[0].vout[o].scriptPubKey.size() >= 38 && block.vtx[0].vout[o].scriptPubKey[0] == OP_RETURN && block.vtx[0].vout[o].scriptPubKey[1] == 0x24 && block.vtx[0].vout[o].scriptPubKey[2] == 0xaa && block.vtx[0].vout[o].scriptPubKey[3] == 0x21 && block.vtx[0].vout[o].scriptPubKey[4] == 0xa9 && block.vtx[0].vout[o].scriptPubKey[5] == 0xed) {
            commitpos = o;
        }
    }
    return commitpos;
}

void UpdateUncommittedBlockStructures(CBlock& block, const CBlockIndex* pindexPrev, const Consensus::Params& consensusParams)
{
    int commitpos = GetWitnessCommitmentIndex(block);
    static const std::vector<unsigned char> nonce(32, 0x00);
    if (commitpos != -1 && IsWitnessEnabled(pindexPrev, consensusParams) && block.vtx[0].wit.IsEmpty()) {
        block.vtx[0].wit.vtxinwit.resize(1);
        block.vtx[0].wit.vtxinwit[0].scriptWitness.stack.resize(1);
        block.vtx[0].wit.vtxinwit[0].scriptWitness.stack[0] = nonce;
    }
}

std::vector<unsigned char> GenerateCoinbaseCommitment(CBlock& block, const CBlockIndex* pindexPrev, const Consensus::Params& consensusParams)
{
    std::vector<unsigned char> commitment;
    int commitpos = GetWitnessCommitmentIndex(block);
    std::vector<unsigned char> ret(32, 0x00);
    if (consensusParams.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nTimeout != 0) {
        if (commitpos == -1) {
            uint256 witnessroot = BlockWitnessMerkleRoot(block, NULL);
            CHash256().Write(witnessroot.begin(), 32).Write(&ret[0], 32).Finalize(witnessroot.begin());
            CTxOut out;
            out.nValue = 0;
            out.scriptPubKey.resize(38);
            out.scriptPubKey[0] = OP_RETURN;
            out.scriptPubKey[1] = 0x24;
            out.scriptPubKey[2] = 0xaa;
            out.scriptPubKey[3] = 0x21;
            out.scriptPubKey[4] = 0xa9;
            out.scriptPubKey[5] = 0xed;
            memcpy(&out.scriptPubKey[6], witnessroot.begin(), 32);
            commitment = std::vector<unsigned char>(out.scriptPubKey.begin(), out.scriptPubKey.end());
            const_cast<std::vector<CTxOut>*>(&block.vtx[0].vout)->push_back(out);
            block.vtx[0].UpdateHash();
        }
    }
    UpdateUncommittedBlockStructures(block, pindexPrev, consensusParams);
    return commitment;
}

bool ContextualCheckBlockHeader(const CBlockHeader& block, CValidationState& state, const Consensus::Params& consensusParams, CBlockIndex * const pindexPrev, int64_t nAdjustedTime)
{
    // Check timestamp against prev
    if (block.GetBlockTime() <= pindexPrev->GetMedianTimePast())
        return state.Invalid(false, REJECT_INVALID, "time-too-old", "block's timestamp is too early");

    // Check timestamp
    if (block.GetBlockTime() > nAdjustedTime + 2 * 60 * 60)
        return state.Invalid(false, REJECT_INVALID, "time-too-new", "block timestamp too far in the future");

    return true;
}

bool ContextualCheckBlock(const CBlock& block, CValidationState& state, CBlockIndex * const pindexPrev)
{
    const int nHeight = pindexPrev == NULL ? 0 : pindexPrev->nHeight + 1;
    const Consensus::Params& consensusParams = Params().GetConsensus();

    // Start enforcing BIP113 (Median Time Past) using versionbits logic.
    int nLockTimeFlags = 0;
    if (VersionBitsState(pindexPrev, consensusParams, Consensus::DEPLOYMENT_CSV, versionbitscache) == THRESHOLD_ACTIVE) {
        nLockTimeFlags |= LOCKTIME_MEDIAN_TIME_PAST;
    }

    int64_t nLockTimeCutoff = (nLockTimeFlags & LOCKTIME_MEDIAN_TIME_PAST)
                              ? pindexPrev->GetMedianTimePast()
                              : block.GetBlockTime();

    // Check that all transactions are finalized
    BOOST_FOREACH(const CTransaction& tx, block.vtx) {
        if (!IsFinalTx(tx, nHeight, nLockTimeCutoff)) {
            return state.DoS(10, false, REJECT_INVALID, "bad-txns-nonfinal", false, "non-final transaction");
        }
    }

    // Validation for witness commitments.
    // * We compute the witness hash (which is the hash including witnesses) of all the block's transactions, except the
    //   coinbase (where 0x0000....0000 is used instead).
    // * The coinbase scriptWitness is a stack of a single 32-byte vector, containing a witness nonce (unconstrained).
    // * We build a merkle tree with all those witness hashes as leaves (similar to the hashMerkleRoot in the block header).
    // * There must be at least one output whose scriptPubKey is a single 36-byte push, the first 4 bytes of which are
    //   {0xaa, 0x21, 0xa9, 0xed}, and the following 32 bytes are SHA256^2(witness root, witness nonce). In case there are
    //   multiple, the last one is used.
    bool fHaveWitness = false;
    if (IsWitnessEnabled(pindexPrev, consensusParams)) {
        int commitpos = GetWitnessCommitmentIndex(block);
        if (commitpos != -1) {
            bool malleated = false;
            uint256 hashWitness = BlockWitnessMerkleRoot(block, &malleated);
            // The malleation check is ignored; as the transaction tree itself
            // already does not permit it, it is impossible to trigger in the
            // witness tree.
            if (block.vtx[0].wit.vtxinwit.size() != 1 || block.vtx[0].wit.vtxinwit[0].scriptWitness.stack.size() != 1 || block.vtx[0].wit.vtxinwit[0].scriptWitness.stack[0].size() != 32) {
                return state.DoS(100, error("%s : invalid witness nonce size", __func__), REJECT_INVALID, "bad-witness-nonce-size", true);
            }
            CHash256().Write(hashWitness.begin(), 32).Write(&block.vtx[0].wit.vtxinwit[0].scriptWitness.stack[0][0], 32).Finalize(hashWitness.begin());
            if (memcmp(hashWitness.begin(), &block.vtx[0].vout[commitpos].scriptPubKey[6], 32)) {
                return state.DoS(100, error("%s : witness merkle commitment mismatch", __func__), REJECT_INVALID, "bad-witness-merkle-match", true);
            }
            fHaveWitness = true;
        }
    }

    // No witness data is allowed in blocks that don't commit to witness data, as this would otherwise leave room for spam
    if (!fHaveWitness) {
        for (size_t i = 0; i < block.vtx.size(); i++) {
            if (!block.vtx[i].wit.IsNull()) {
                return state.DoS(100, error("%s : unexpected witness data found", __func__), REJECT_INVALID, "unexpected-witness", true);
            }
        }
    }

    // After the coinbase witness nonce and commitment are verified,
    // we can check if the block weight passes (before we've checked the
    // coinbase witness, it would be possible for the weight to be too
    // large by filling up the coinbase witness, which doesn't change
    // the block hash, so we couldn't mark the block as permanently
    // failed).
    if (GetBlockWeight(block) > MAX_BLOCK_WEIGHT) {
        return state.DoS(100, error("ContextualCheckBlock(): weight limit failed"), REJECT_INVALID, "bad-blk-weight");
    }

    return true;
}

static bool AcceptBlockHeader(const CBlockHeader& block, CValidationState& state, const CChainParams& chainparams, CBlockIndex** ppindex=NULL)
{
    AssertLockHeld(cs_main);
    // Check for duplicate
    uint256 hash = block.GetHash();
    BlockMap::iterator miSelf = mapBlockIndex.find(hash);
    CBlockIndex *pindex = NULL;
    if (hash != chainparams.GetConsensus().hashGenesisBlock) {

        if (miSelf != mapBlockIndex.end()) {
            // Block header is already known.
            pindex = miSelf->second;
            if (ppindex)
                *ppindex = pindex;
            if (pindex->nStatus & BLOCK_FAILED_MASK)
                return state.Invalid(error("%s: block %s is marked invalid", __func__, hash.ToString()), 0, "duplicate");
            return true;
        }

        if (!CheckBlockHeader(block, state, chainparams.GetConsensus()))
            return error("%s: Consensus::CheckBlockHeader: %s, %s", __func__, hash.ToString(), FormatStateMessage(state));

        // Get prev block index
        CBlockIndex* pindexPrev = NULL;
        BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
        if (mi == mapBlockIndex.end())
            return state.DoS(10, error("%s: prev block not found", __func__), 0, "bad-prevblk");
        pindexPrev = (*mi).second;
        if (pindexPrev->nStatus & BLOCK_FAILED_MASK)
            return state.DoS(100, error("%s: prev block invalid", __func__), REJECT_INVALID, "bad-prevblk");

        assert(pindexPrev);
        if (fCheckpointsEnabled && !CheckIndexAgainstCheckpoint(pindexPrev, state, chainparams, hash))
            return error("%s: CheckIndexAgainstCheckpoint(): %s", __func__, state.GetRejectReason().c_str());

        // part of the warp mechanism (baz)
        //if(GetHeight()>1112500){
        //  if (!ContextualCheckBlockHeader(block, state, chainparams.GetConsensus(), pindexPrev, GetAdjustedTime()))
        //      return error("%s: Consensus::ContextualCheckBlockHeader: %s, %s", __func__, hash.ToString(), FormatStateMessage(state));
        //}
    }
    if (pindex == NULL)
        pindex = AddToBlockIndex(block);

    if (ppindex)
        *ppindex = pindex;

    return true;
}

/** Store block on disk. If dbp is non-NULL, the file is known to already reside on disk */
static bool AcceptBlock(const CBlock& block, CValidationState& state, const CChainParams& chainparams, CBlockIndex** ppindex, bool fRequested, const CDiskBlockPos* dbp, bool* fNewBlock)
{
    if (fNewBlock) *fNewBlock = false;
    AssertLockHeld(cs_main);

    CBlockIndex *pindexDummy = NULL;
    CBlockIndex *&pindex = ppindex ? *ppindex : pindexDummy;

    if (!AcceptBlockHeader(block, state, chainparams, &pindex))
        return false;

    // Try to process all requested blocks that we don't have, but only
    // process an unrequested block if it's new and has enough work to
    // advance our tip, and isn't too many blocks ahead.
    bool fAlreadyHave = pindex->nStatus & BLOCK_HAVE_DATA;
    bool fHasMoreWork = (chainActive.Tip() ? pindex->nChainWork > chainActive.Tip()->nChainWork : true);
    // Blocks that are too out-of-order needlessly limit the effectiveness of
    // pruning, because pruning will not delete block files that contain any
    // blocks which are too close in height to the tip.  Apply this test
    // regardless of whether pruning is enabled; it should generally be safe to
    // not process unrequested blocks.
    bool fTooFarAhead = (pindex->nHeight > int(chainActive.Height() + MIN_BLOCKS_TO_KEEP));

    // TODO: deal better with return value and error conditions for duplicate
    // and unrequested blocks.
    if (fAlreadyHave) return true;
    if (!fRequested) {  // If we didn't ask for it:
        if (pindex->nTx != 0) return true;  // This is a previously-processed block that was pruned
        if (!fHasMoreWork) return true;     // Don't process less-work chains
        if (fTooFarAhead) return true;      // Block height is too high
    }
    if (fNewBlock) *fNewBlock = true;

    if ((!CheckBlock(block, state, chainparams.GetConsensus(), GetAdjustedTime())) || !ContextualCheckBlock(block, state, pindex->pprev)) {
        if (state.IsInvalid() && !state.CorruptionPossible()) {
            pindex->nStatus |= BLOCK_FAILED_VALID;
            setDirtyBlockIndex.insert(pindex);
        }
        return error("%s: %s", __func__, FormatStateMessage(state));
    }

    int nHeight = pindex->nHeight;

    // Write block to history file
    try {
        unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
        CDiskBlockPos blockPos;
        if (dbp != NULL)
            blockPos = *dbp;
        if (!FindBlockPos(state, blockPos, nBlockSize+8, nHeight, block.GetBlockTime(), dbp != NULL))
            return error("AcceptBlock(): FindBlockPos failed");
        if (dbp == NULL)
            if (!WriteBlockToDisk(block, blockPos, chainparams.MessageStart()))
                AbortNode(state, "Failed to write block");
        if (!ReceivedBlockTransactions(block, state, pindex, blockPos))
            return error("AcceptBlock(): ReceivedBlockTransactions failed");
    } catch (const std::runtime_error& e) {
        return AbortNode(state, std::string("System error: ") + e.what());
    }

    if (fCheckForPruning)
        FlushStateToDisk(state, FLUSH_STATE_NONE); // we just allocated more disk space for block files

    return true;
}

static bool IsSuperMajority(int minVersion, const CBlockIndex* pstart, unsigned nRequired, const Consensus::Params& consensusParams)
{
    unsigned int nFound = 0;
    for (int i = 0; i < consensusParams.nMajorityWindow && nFound < nRequired && pstart != NULL; i++)
    {
        if (pstart->nVersion >= minVersion)
            ++nFound;
        pstart = pstart->pprev;
    }
    return (nFound >= nRequired);
}


bool ProcessNewBlock(CValidationState& state, const CChainParams& chainparams, CNode* pfrom, const CBlock* pblock, bool fForceProcessing, const CDiskBlockPos* dbp, bool fMayBanPeerIfInvalid)
{
    {
        LOCK(cs_main);
        bool fRequested = MarkBlockAsReceived(pblock->GetHash());
        fRequested |= fForceProcessing;

        // Store to disk
        CBlockIndex *pindex = NULL;
        bool fNewBlock = false;
        bool ret = AcceptBlock(*pblock, state, chainparams, &pindex, fRequested, dbp, &fNewBlock);
        if (pindex && pfrom) {
            mapBlockSource[pindex->GetBlockHash()] = std::make_pair(pfrom->GetId(), fMayBanPeerIfInvalid);
            if (fNewBlock) pfrom->nLastBlockTime = GetTime();
        }
        CheckBlockIndex(chainparams.GetConsensus());
        if (!ret)
            return error("%s: AcceptBlock FAILED", __func__);
    }

    NotifyHeaderTip();

    if (!ActivateBestChain(state, chainparams, pblock))
        return error("%s: ActivateBestChain failed", __func__);

    return true;
}

bool TestBlockValidity(CValidationState& state, const CChainParams& chainparams, const CBlock& block, CBlockIndex* pindexPrev, bool fCheckPOW, bool fCheckMerkleRoot)
{
    AssertLockHeld(cs_main);
    assert(pindexPrev && pindexPrev == chainActive.Tip());
    if (fCheckpointsEnabled && !CheckIndexAgainstCheckpoint(pindexPrev, state, chainparams, block.GetHash()))
        return error("%s: CheckIndexAgainstCheckpoint(): %s", __func__, state.GetRejectReason().c_str());

    CCoinsViewCache viewNew(pcoinsTip);
    CBlockIndex indexDummy(block);
    indexDummy.pprev = pindexPrev;
    indexDummy.nHeight = pindexPrev->nHeight + 1;

    // NOTE: CheckBlockHeader is called by CheckBlock
    if (!ContextualCheckBlockHeader(block, state, chainparams.GetConsensus(), pindexPrev, GetAdjustedTime()))
        return error("%s: Consensus::ContextualCheckBlockHeader: %s", __func__, FormatStateMessage(state));
    if (!CheckBlock(block, state, chainparams.GetConsensus(), fCheckPOW, fCheckMerkleRoot))
        return error("%s: Consensus::CheckBlock: %s", __func__, FormatStateMessage(state));
    if (!ContextualCheckBlock(block, state, pindexPrev))
        return error("%s: Consensus::ContextualCheckBlock: %s", __func__, FormatStateMessage(state));
    if (!ConnectBlock(block, state, &indexDummy, viewNew, chainparams, true))
        return false;
    assert(state.IsValid());

    return true;
}

/**
 * BLOCK PRUNING CODE
 */

/* Calculate the amount of disk space the block & undo files currently use */
uint64_t CalculateCurrentUsage()
{
    uint64_t retval = 0;
    BOOST_FOREACH(const CBlockFileInfo &file, vinfoBlockFile) {
        retval += file.nSize + file.nUndoSize;
    }
    return retval;
}

/* Prune a block file (modify associated database entries)*/
void PruneOneBlockFile(const int fileNumber)
{
    for (BlockMap::iterator it = mapBlockIndex.begin(); it != mapBlockIndex.end(); ++it) {
        CBlockIndex* pindex = it->second;
        if (pindex->nFile == fileNumber) {
            pindex->nStatus &= ~BLOCK_HAVE_DATA;
            pindex->nStatus &= ~BLOCK_HAVE_UNDO;
            pindex->nFile = 0;
            pindex->nDataPos = 0;
            pindex->nUndoPos = 0;
            setDirtyBlockIndex.insert(pindex);

            // Prune from mapBlocksUnlinked -- any block we prune would have
            // to be downloaded again in order to consider its chain, at which
            // point it would be considered as a candidate for
            // mapBlocksUnlinked or setBlockIndexCandidates.
            std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> range = mapBlocksUnlinked.equal_range(pindex->pprev);
            while (range.first != range.second) {
                std::multimap<CBlockIndex *, CBlockIndex *>::iterator it = range.first;
                range.first++;
                if (it->second == pindex) {
                    mapBlocksUnlinked.erase(it);
                }
            }
        }
    }

    vinfoBlockFile[fileNumber].SetNull();
    setDirtyFileInfo.insert(fileNumber);
}


void UnlinkPrunedFiles(std::set<int>& setFilesToPrune)
{
    for (set<int>::iterator it = setFilesToPrune.begin(); it != setFilesToPrune.end(); ++it) {
        CDiskBlockPos pos(*it, 0);
        boost::filesystem::remove(GetBlockPosFilename(pos, "blk"));
        boost::filesystem::remove(GetBlockPosFilename(pos, "rev"));
        LogPrintf("Prune: %s deleted blk/rev (%05u)\n", __func__, *it);
    }
}

/* Calculate the block/rev files that should be deleted to remain under target*/
void FindFilesToPrune(std::set<int>& setFilesToPrune, uint64_t nPruneAfterHeight)
{
    LOCK2(cs_main, cs_LastBlockFile);
    if (chainActive.Tip() == NULL || nPruneTarget == 0) {
        return;
    }
    if ((uint64_t)chainActive.Tip()->nHeight <= nPruneAfterHeight) {
        return;
    }

    unsigned int nLastBlockWeCanPrune = chainActive.Tip()->nHeight - MIN_BLOCKS_TO_KEEP;
    uint64_t nCurrentUsage = CalculateCurrentUsage();
    // We don't check to prune until after we've allocated new space for files
    // So we should leave a buffer under our target to account for another allocation
    // before the next pruning.
    uint64_t nBuffer = BLOCKFILE_CHUNK_SIZE + UNDOFILE_CHUNK_SIZE;
    uint64_t nBytesToPrune;
    int count=0;

    if (nCurrentUsage + nBuffer >= nPruneTarget) {
        for (int fileNumber = 0; fileNumber < nLastBlockFile; fileNumber++) {
            nBytesToPrune = vinfoBlockFile[fileNumber].nSize + vinfoBlockFile[fileNumber].nUndoSize;

            if (vinfoBlockFile[fileNumber].nSize == 0)
                continue;

            if (nCurrentUsage + nBuffer < nPruneTarget)  // are we below our target?
                break;

            // don't prune files that could have a block within MIN_BLOCKS_TO_KEEP of the main chain's tip but keep scanning
            if (vinfoBlockFile[fileNumber].nHeightLast > nLastBlockWeCanPrune)
                continue;

            PruneOneBlockFile(fileNumber);
            // Queue up the files for removal
            setFilesToPrune.insert(fileNumber);
            nCurrentUsage -= nBytesToPrune;
            count++;
        }
    }

    LogPrint("prune", "Prune: target=%dMiB actual=%dMiB diff=%dMiB max_prune_height=%d removed %d blk/rev pairs\n",
           nPruneTarget/1024/1024, nCurrentUsage/1024/1024,
           ((int64_t)nPruneTarget - (int64_t)nCurrentUsage)/1024/1024,
           nLastBlockWeCanPrune, count);
}

bool CheckDiskSpace(uint64_t nAdditionalBytes)
{
    uint64_t nFreeBytesAvailable = boost::filesystem::space(GetDataDir()).available;

    // Check for nMinDiskSpace bytes (currently 50MB)
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
        return AbortNode("Disk space is low!", _("Error: Disk space is low!"));

    return true;
}

FILE* OpenDiskFile(const CDiskBlockPos &pos, const char *prefix, bool fReadOnly)
{
    if (pos.IsNull())
        return NULL;
    boost::filesystem::path path = GetBlockPosFilename(pos, prefix);
    boost::filesystem::create_directories(path.parent_path());
    FILE* file = fopen(path.string().c_str(), "rb+");
    if (!file && !fReadOnly)
        file = fopen(path.string().c_str(), "wb+");
    if (!file) {
        LogPrintf("Unable to open file %s\n", path.string());
        return NULL;
    }
    if (pos.nPos) {
        if (fseek(file, pos.nPos, SEEK_SET)) {
            LogPrintf("Unable to seek to position %u of %s\n", pos.nPos, path.string());
            fclose(file);
            return NULL;
        }
    }
    return file;
}

FILE* OpenBlockFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "blk", fReadOnly);
}

FILE* OpenUndoFile(const CDiskBlockPos &pos, bool fReadOnly) {
    return OpenDiskFile(pos, "rev", fReadOnly);
}

boost::filesystem::path GetBlockPosFilename(const CDiskBlockPos &pos, const char *prefix)
{
    return GetDataDir() / "blocks" / strprintf("%s%05u.dat", prefix, pos.nFile);
}

CBlockIndex * InsertBlockIndex(uint256 hash)
{
    if (hash.IsNull())
        return NULL;

    // Return existing
    BlockMap::iterator mi = mapBlockIndex.find(hash);
    if (mi != mapBlockIndex.end())
        return (*mi).second;

    // Create new
    CBlockIndex* pindexNew = new CBlockIndex();
    if (!pindexNew)
        throw runtime_error(std::string(__func__) + ": new CBlockIndex failed");
    mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);

    return pindexNew;
}

bool static LoadBlockIndexDB()
{
    const CChainParams& chainparams = Params();
    if (!pblocktree->LoadBlockIndexGuts(InsertBlockIndex))
        return false;

    boost::this_thread::interruption_point();

    // Calculate nChainWork
    vector<pair<int, CBlockIndex*> > vSortedByHeight;
    vSortedByHeight.reserve(mapBlockIndex.size());
    BOOST_FOREACH(const PAIRTYPE(uint256, CBlockIndex*)& item, mapBlockIndex)
    {
        CBlockIndex* pindex = item.second;
        vSortedByHeight.push_back(make_pair(pindex->nHeight, pindex));
    }
    sort(vSortedByHeight.begin(), vSortedByHeight.end());
    BOOST_FOREACH(const PAIRTYPE(int, CBlockIndex*)& item, vSortedByHeight)
    {
        CBlockIndex* pindex = item.second;
        pindex->nChainWork = (pindex->pprev ? pindex->pprev->nChainWork : 0) + GetBlockProof(*pindex);
        // We can link the chain of blocks for which we've received transactions at some point.
        // Pruned nodes may have deleted the block.
        if (pindex->nTx > 0) {
            if (pindex->pprev) {
                if (pindex->pprev->nChainTx) {
                    pindex->nChainTx = pindex->pprev->nChainTx + pindex->nTx;
                } else {
                    pindex->nChainTx = 0;
                    mapBlocksUnlinked.insert(std::make_pair(pindex->pprev, pindex));
                }
            } else {
                pindex->nChainTx = pindex->nTx;
            }
        }
        if (pindex->IsValid(BLOCK_VALID_TRANSACTIONS) && (pindex->nChainTx || pindex->pprev == NULL))
            setBlockIndexCandidates.insert(pindex);
        if (pindex->nStatus & BLOCK_FAILED_MASK && (!pindexBestInvalid || pindex->nChainWork > pindexBestInvalid->nChainWork))
            pindexBestInvalid = pindex;
        if (pindex->pprev)
            pindex->BuildSkip();
        if (pindex->IsValid(BLOCK_VALID_TREE) && (pindexBestHeader == NULL || CBlockIndexWorkComparator()(pindexBestHeader, pindex)))
            pindexBestHeader = pindex;
    }

    // Load block file info
    pblocktree->ReadLastBlockFile(nLastBlockFile);
    vinfoBlockFile.resize(nLastBlockFile + 1);
    LogPrintf("%s: last block file = %i\n", __func__, nLastBlockFile);
    for (int nFile = 0; nFile <= nLastBlockFile; nFile++) {
        pblocktree->ReadBlockFileInfo(nFile, vinfoBlockFile[nFile]);
    }
    LogPrintf("%s: last block file info: %s\n", __func__, vinfoBlockFile[nLastBlockFile].ToString());
    for (int nFile = nLastBlockFile + 1; true; nFile++) {
        CBlockFileInfo info;
        if (pblocktree->ReadBlockFileInfo(nFile, info)) {
            vinfoBlockFile.push_back(info);
        } else {
            break;
        }
    }

    // Check presence of blk files
    LogPrintf("Checking all blk files are present...\n");
    set<int> setBlkDataFiles;
    BOOST_FOREACH(const PAIRTYPE(uint256, CBlockIndex*)& item, mapBlockIndex)
    {
        CBlockIndex* pindex = item.second;
        if (pindex->nStatus & BLOCK_HAVE_DATA) {
            setBlkDataFiles.insert(pindex->nFile);
        }
    }
    for (std::set<int>::iterator it = setBlkDataFiles.begin(); it != setBlkDataFiles.end(); it++)
    {
        CDiskBlockPos pos(*it, 0);
        if (CAutoFile(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION).IsNull()) {
            return false;
        }
    }

    // Check whether we have ever pruned block & undo files
    pblocktree->ReadFlag("prunedblockfiles", fHavePruned);
    if (fHavePruned)
        LogPrintf("LoadBlockIndexDB(): Block files have previously been pruned\n");

    // Check whether we need to continue reindexing
    bool fReindexing = false;
    pblocktree->ReadReindexing(fReindexing);
    fReindex |= fReindexing;

    // Check whether we have a transaction index
    pblocktree->ReadFlag("txindex", fTxIndex);
    LogPrintf("%s: transaction index %s\n", __func__, fTxIndex ? "enabled" : "disabled");

    // Load pointer to end of best chain
    BlockMap::iterator it = mapBlockIndex.find(pcoinsTip->GetBestBlock());
    if (it == mapBlockIndex.end())
        return true;
    chainActive.SetTip(it->second);

    PruneBlockIndexCandidates();

    LogPrintf("%s: hashBestChain=%s height=%d date=%s progress=%f\n", __func__,
        chainActive.Tip()->GetBlockHash().ToString(), chainActive.Height(),
        DateTimeStrFormat("%Y-%m-%d %H:%M:%S", chainActive.Tip()->GetBlockTime()),
        Checkpoints::GuessVerificationProgress(chainparams.Checkpoints(), chainActive.Tip()));

    return true;
}

CVerifyDB::CVerifyDB()
{
    uiInterface.ShowProgress(_("Verifying blocks..."), 0);
}

CVerifyDB::~CVerifyDB()
{
    uiInterface.ShowProgress("", 100);
}

bool CVerifyDB::VerifyDB(const CChainParams& chainparams, CCoinsView *coinsview, int nCheckLevel, int nCheckDepth)
{
    LOCK(cs_main);
    if (chainActive.Tip() == NULL || chainActive.Tip()->pprev == NULL)
        return true;

    // Verify blocks in the best chain
    if (nCheckDepth <= 0)
        // Mooncoin: suffices until year 10214. Didn't x4 value due to integer wrap around and upstream compatibility.
        nCheckDepth = std::numeric_limits<int>::max();
    if (nCheckDepth > chainActive.Height())
        nCheckDepth = chainActive.Height();
    nCheckLevel = std::max(0, std::min(4, nCheckLevel));
    LogPrintf("Verifying last %i blocks at level %i\n", nCheckDepth, nCheckLevel);
    CCoinsViewCache coins(coinsview);
    CBlockIndex* pindexState = chainActive.Tip();
    CBlockIndex* pindexFailure = NULL;
    int nGoodTransactions = 0;
    CValidationState state;
    int reportDone = 0;
    LogPrintf("[0%]...");
    for (CBlockIndex* pindex = chainActive.Tip(); pindex && pindex->pprev; pindex = pindex->pprev)
    {
        boost::this_thread::interruption_point();
        int percentageDone = std::max(1, std::min(99, (int)(((double)(chainActive.Height() - pindex->nHeight)) / (double)nCheckDepth * (nCheckLevel >= 4 ? 50 : 100))));
        if (reportDone < percentageDone/10) {
            // report every 10% step
            LogPrintf("[%d%%]...", percentageDone);
            reportDone = percentageDone/10;
        }
        uiInterface.ShowProgress(_("Verifying blocks..."), percentageDone);
        if (pindex->nHeight < chainActive.Height()-nCheckDepth)
            break;
        if (fPruneMode && !(pindex->nStatus & BLOCK_HAVE_DATA)) {
            // If pruning, only go back as far as we have data.
            LogPrintf("VerifyDB(): block verification stopping at height %d (pruning, no data)\n", pindex->nHeight);
            break;
        }
        CBlock block;
        // check level 0: read from disk
        if (!ReadBlockFromDisk(block, pindex, chainparams.GetConsensus()))
            return error("VerifyDB(): *** ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
        // check level 1: verify block validity
        if (nCheckLevel >= 1 && !CheckBlock(block, state, chainparams.GetConsensus()))
            return error("%s: *** found bad block at %d, hash=%s (%s)\n", __func__, 
                         pindex->nHeight, pindex->GetBlockHash().ToString(), FormatStateMessage(state));
        // check level 2: verify undo validity
        if (nCheckLevel >= 2 && pindex) {
            CBlockUndo undo;
            CDiskBlockPos pos = pindex->GetUndoPos();
            if (!pos.IsNull()) {
                if (!UndoReadFromDisk(undo, pos, pindex->pprev->GetBlockHash()))
                    return error("VerifyDB(): *** found bad undo data at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
            }
        }
        // check level 3: check for inconsistencies during memory-only disconnect of tip blocks
        if (nCheckLevel >= 3 && pindex == pindexState && (coins.DynamicMemoryUsage() + pcoinsTip->DynamicMemoryUsage()) <= nCoinCacheUsage) {
            bool fClean = true;
            if (!DisconnectBlock(block, state, pindex, coins, &fClean))
                return error("VerifyDB(): *** irrecoverable inconsistency in block data at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
            pindexState = pindex->pprev;
            if (!fClean) {
                nGoodTransactions = 0;
                pindexFailure = pindex;
            } else
                nGoodTransactions += block.vtx.size();
        }
        if (ShutdownRequested())
            return true;
    }
    if (pindexFailure)
        return error("VerifyDB(): *** coin database inconsistencies found (last %i blocks, %i good transactions before that)\n", chainActive.Height() - pindexFailure->nHeight + 1, nGoodTransactions);

    // check level 4: try reconnecting blocks
    if (nCheckLevel >= 4) {
        CBlockIndex *pindex = pindexState;
        while (pindex != chainActive.Tip()) {
            boost::this_thread::interruption_point();
            uiInterface.ShowProgress(_("Verifying blocks..."), std::max(1, std::min(99, 100 - (int)(((double)(chainActive.Height() - pindex->nHeight)) / (double)nCheckDepth * 50))));
            pindex = chainActive.Next(pindex);
            CBlock block;
            if (!ReadBlockFromDisk(block, pindex, chainparams.GetConsensus()))
                return error("VerifyDB(): *** ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
            if (!ConnectBlock(block, state, pindex, coins, chainparams))
                return error("VerifyDB(): *** found unconnectable block at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
        }
    }

    LogPrintf("[DONE].\n");
    LogPrintf("No coin database inconsistencies in last %i blocks (%i transactions)\n", chainActive.Height() - pindexState->nHeight, nGoodTransactions);

    return true;
}

bool RewindBlockIndex(const CChainParams& params)
{
    LOCK(cs_main);

    int nHeight = 1;
    while (nHeight <= chainActive.Height()) {
        if (IsWitnessEnabled(chainActive[nHeight - 1], params.GetConsensus()) && !(chainActive[nHeight]->nStatus & BLOCK_OPT_WITNESS)) {
            break;
        }
        nHeight++;
    }

    // nHeight is now the height of the first insufficiently-validated block, or tipheight + 1
    CValidationState state;
    CBlockIndex* pindex = chainActive.Tip();
    while (chainActive.Height() >= nHeight) {
        if (fPruneMode && !(chainActive.Tip()->nStatus & BLOCK_HAVE_DATA)) {
            // If pruning, don't try rewinding past the HAVE_DATA point;
            // since older blocks can't be served anyway, there's
            // no need to walk further, and trying to DisconnectTip()
            // will fail (and require a needless reindex/redownload
            // of the blockchain).
            break;
        }
        if (!DisconnectTip(state, params, true)) {
            return error("RewindBlockIndex: unable to disconnect block at height %i", pindex->nHeight);
        }
        // Occasionally flush state to disk.
        if (!FlushStateToDisk(state, FLUSH_STATE_PERIODIC))
            return false;
    }

    // Reduce validity flag and have-data flags.
    // We do this after actual disconnecting, otherwise we'll end up writing the lack of data
    // to disk before writing the chainstate, resulting in a failure to continue if interrupted.
    for (BlockMap::iterator it = mapBlockIndex.begin(); it != mapBlockIndex.end(); it++) {
        CBlockIndex* pindexIter = it->second;

        // Note: If we encounter an insufficiently validated block that
        // is on chainActive, it must be because we are a pruning node, and
        // this block or some successor doesn't HAVE_DATA, so we were unable to
        // rewind all the way.  Blocks remaining on chainActive at this point
        // must not have their validity reduced.
        if (IsWitnessEnabled(pindexIter->pprev, params.GetConsensus()) && !(pindexIter->nStatus & BLOCK_OPT_WITNESS) && !chainActive.Contains(pindexIter)) {
            // Reduce validity
            pindexIter->nStatus = std::min<unsigned int>(pindexIter->nStatus & BLOCK_VALID_MASK, BLOCK_VALID_TREE) | (pindexIter->nStatus & ~BLOCK_VALID_MASK);
            // Remove have-data flags.
            pindexIter->nStatus &= ~(BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO);
            // Remove storage location.
            pindexIter->nFile = 0;
            pindexIter->nDataPos = 0;
            pindexIter->nUndoPos = 0;
            // Remove various other things
            pindexIter->nTx = 0;
            pindexIter->nChainTx = 0;
            pindexIter->nSequenceId = 0;
            // Make sure it gets written.
            setDirtyBlockIndex.insert(pindexIter);
            // Update indexes
            setBlockIndexCandidates.erase(pindexIter);
            std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> ret = mapBlocksUnlinked.equal_range(pindexIter->pprev);
            while (ret.first != ret.second) {
                if (ret.first->second == pindexIter) {
                    mapBlocksUnlinked.erase(ret.first++);
                } else {
                    ++ret.first;
                }
            }
        } else if (pindexIter->IsValid(BLOCK_VALID_TRANSACTIONS) && pindexIter->nChainTx) {
            setBlockIndexCandidates.insert(pindexIter);
        }
    }

    PruneBlockIndexCandidates();

    CheckBlockIndex(params.GetConsensus());

    if (!FlushStateToDisk(state, FLUSH_STATE_ALWAYS)) {
        return false;
    }

    return true;
}

void UnloadBlockIndex()
{
    LOCK(cs_main);
    setBlockIndexCandidates.clear();
    chainActive.SetTip(NULL);
    pindexBestInvalid = NULL;
    pindexBestHeader = NULL;
    mempool.clear();
    mapOrphanTransactions.clear();
    mapOrphanTransactionsByPrev.clear();
    nSyncStarted = 0;
    mapBlocksUnlinked.clear();
    vinfoBlockFile.clear();
    nLastBlockFile = 0;
    nBlockSequenceId = 1;
    mapBlockSource.clear();
    mapBlocksInFlight.clear();
    nPreferredDownload = 0;
    setDirtyBlockIndex.clear();
    setDirtyFileInfo.clear();
    mapNodeState.clear();
    recentRejects.reset(NULL);
    versionbitscache.Clear();
    for (int b = 0; b < VERSIONBITS_NUM_BITS; b++) {
        warningcache[b].clear();
    }

    BOOST_FOREACH(BlockMap::value_type& entry, mapBlockIndex) {
        delete entry.second;
    }
    mapBlockIndex.clear();
    fHavePruned = false;
}

bool LoadBlockIndex()
{
    // Load block index from databases
    if (!fReindex && !LoadBlockIndexDB())
        return false;
    return true;
}

bool InitBlockIndex(const CChainParams& chainparams) 
{
    LOCK(cs_main);

    // Initialize global variables that cannot be constructed at startup.
    recentRejects.reset(new CRollingBloomFilter(120000, 0.000001));

    // Check whether we're already initialized
    if (chainActive.Genesis() != NULL)
        return true;

    // Use the provided setting for -txindex in the new database
    fTxIndex = GetBoolArg("-txindex", DEFAULT_TXINDEX);
    pblocktree->WriteFlag("txindex", fTxIndex);
    LogPrintf("Initializing databases...\n");

    // Only add the genesis block if not reindexing (in which case we reuse the one already on disk)
    if (!fReindex) {
        try {
            CBlock &block = const_cast<CBlock&>(chainparams.GenesisBlock());
            // Start new block file
            unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
            CDiskBlockPos blockPos;
            CValidationState state;
            if (!FindBlockPos(state, blockPos, nBlockSize+8, 0, block.GetBlockTime()))
                return error("LoadBlockIndex(): FindBlockPos failed");
            if (!WriteBlockToDisk(block, blockPos, chainparams.MessageStart()))
                return error("LoadBlockIndex(): writing genesis block to disk failed");
            CBlockIndex *pindex = AddToBlockIndex(block);
            if (!ReceivedBlockTransactions(block, state, pindex, blockPos))
                return error("LoadBlockIndex(): genesis block not accepted");
            // Force a chainstate write so that when we VerifyDB in a moment, it doesn't check stale data
            return FlushStateToDisk(state, FLUSH_STATE_ALWAYS);
        } catch (const std::runtime_error& e) {
            return error("LoadBlockIndex(): failed to initialize block database: %s", e.what());
        }
    }

    return true;
}

bool LoadExternalBlockFile(const CChainParams& chainparams, FILE* fileIn, CDiskBlockPos *dbp)
{
    // Map of disk positions for blocks with unknown parent (only used for reindex)
    static std::multimap<uint256, CDiskBlockPos> mapBlocksUnknownParent;
    int64_t nStart = GetTimeMillis();

    int nLoaded = 0;
    try {
        // This takes over fileIn and calls fclose() on it in the CBufferedFile destructor
        CBufferedFile blkdat(fileIn, 2*MAX_BLOCK_SERIALIZED_SIZE, MAX_BLOCK_SERIALIZED_SIZE+8, SER_DISK, CLIENT_VERSION);
        uint64_t nRewind = blkdat.GetPos();
        while (!blkdat.eof()) {
            boost::this_thread::interruption_point();

            blkdat.SetPos(nRewind);
            nRewind++; // start one byte further next time, in case of failure
            blkdat.SetLimit(); // remove former limit
            unsigned int nSize = 0;
            try {
                // locate a header
                unsigned char buf[MESSAGE_START_SIZE];
                blkdat.FindByte(chainparams.MessageStart()[0]);
                nRewind = blkdat.GetPos()+1;
                blkdat >> FLATDATA(buf);
                if (memcmp(buf, chainparams.MessageStart(), MESSAGE_START_SIZE))
                    continue;
                // read size
                blkdat >> nSize;
                if (nSize < 80 || nSize > MAX_BLOCK_SERIALIZED_SIZE)
                    continue;
            } catch (const std::exception&) {
                // no valid block header found; don't complain
                break;
            }
            try {
                // read block
                uint64_t nBlockPos = blkdat.GetPos();
                if (dbp)
                    dbp->nPos = nBlockPos;
                blkdat.SetLimit(nBlockPos + nSize);
                blkdat.SetPos(nBlockPos);
                CBlock block;
                blkdat >> block;
                nRewind = blkdat.GetPos();

                // detect out of order blocks, and store them for later
                uint256 hash = block.GetHash();
                if (hash != chainparams.GetConsensus().hashGenesisBlock && mapBlockIndex.find(block.hashPrevBlock) == mapBlockIndex.end()) {
                    LogPrint("reindex", "%s: Out of order block %s, parent %s not known\n", __func__, hash.ToString(),
                            block.hashPrevBlock.ToString());
                    if (dbp)
                        mapBlocksUnknownParent.insert(std::make_pair(block.hashPrevBlock, *dbp));
                    continue;
                }

                // process in case the block isn't known yet
                if (mapBlockIndex.count(hash) == 0 || (mapBlockIndex[hash]->nStatus & BLOCK_HAVE_DATA) == 0) {
                    LOCK(cs_main);
                    CValidationState state;
                    if (AcceptBlock(block, state, chainparams, NULL, true, dbp, NULL))
                        nLoaded++;
                    if (state.IsError())
                        break;
                } else if (hash != chainparams.GetConsensus().hashGenesisBlock && mapBlockIndex[hash]->nHeight % 1000 == 0) {
                    LogPrint("reindex", "Block Import: already had block %s at height %d\n", hash.ToString(), mapBlockIndex[hash]->nHeight);
                }

                // Activate the genesis block so normal node progress can continue
                if (hash == chainparams.GetConsensus().hashGenesisBlock) {
                    CValidationState state;
                    if (!ActivateBestChain(state, chainparams)) {
                        break;
                    }
                }

                NotifyHeaderTip();

                // Recursively process earlier encountered successors of this block
                deque<uint256> queue;
                queue.push_back(hash);
                while (!queue.empty()) {
                    uint256 head = queue.front();
                    queue.pop_front();
                    std::pair<std::multimap<uint256, CDiskBlockPos>::iterator, std::multimap<uint256, CDiskBlockPos>::iterator> range = mapBlocksUnknownParent.equal_range(head);
                    while (range.first != range.second) {
                        std::multimap<uint256, CDiskBlockPos>::iterator it = range.first;
                        if (ReadBlockFromDisk(block, it->second, chainparams.GetConsensus()))
                        {
                            LogPrint("reindex", "%s: Processing out of order child %s of %s\n", __func__, block.GetHash().ToString(),
                                    head.ToString());
                            LOCK(cs_main);
                            CValidationState dummy;
                            if (AcceptBlock(block, dummy, chainparams, NULL, true, &it->second, NULL))
                            {
                                nLoaded++;
                                queue.push_back(block.GetHash());
                            }
                        }
                        range.first++;
                        mapBlocksUnknownParent.erase(it);
                        NotifyHeaderTip();
                    }
                }
            } catch (const std::exception& e) {
                LogPrintf("%s: Deserialize or I/O error - %s\n", __func__, e.what());
            }
        }
    } catch (const std::runtime_error& e) {
        AbortNode(std::string("System error: ") + e.what());
    }
    if (nLoaded > 0)
        LogPrintf("Loaded %i blocks from external file in %dms\n", nLoaded, GetTimeMillis() - nStart);
    return nLoaded > 0;
}

void static CheckBlockIndex(const Consensus::Params& consensusParams)
{
    if (!fCheckBlockIndex) {
        return;
    }

    LOCK(cs_main);

    // During a reindex, we read the genesis block and call CheckBlockIndex before ActivateBestChain,
    // so we have the genesis block in mapBlockIndex but no active chain.  (A few of the tests when
    // iterating the block tree require that chainActive has been initialized.)
    if (chainActive.Height() < 0) {
        assert(mapBlockIndex.size() <= 1);
        return;
    }

    // Build forward-pointing map of the entire block tree.
    std::multimap<CBlockIndex*,CBlockIndex*> forward;
    for (BlockMap::iterator it = mapBlockIndex.begin(); it != mapBlockIndex.end(); it++) {
        forward.insert(std::make_pair(it->second->pprev, it->second));
    }

    assert(forward.size() == mapBlockIndex.size());

    std::pair<std::multimap<CBlockIndex*,CBlockIndex*>::iterator,std::multimap<CBlockIndex*,CBlockIndex*>::iterator> rangeGenesis = forward.equal_range(NULL);
    CBlockIndex *pindex = rangeGenesis.first->second;
    rangeGenesis.first++;
    assert(rangeGenesis.first == rangeGenesis.second); // There is only one index entry with parent NULL.

    // Iterate over the entire block tree, using depth-first search.
    // Along the way, remember whether there are blocks on the path from genesis
    // block being explored which are the first to have certain properties.
    size_t nNodes = 0;
    int nHeight = 0;
    CBlockIndex* pindexFirstInvalid = NULL; // Oldest ancestor of pindex which is invalid.
    CBlockIndex* pindexFirstMissing = NULL; // Oldest ancestor of pindex which does not have BLOCK_HAVE_DATA.
    CBlockIndex* pindexFirstNeverProcessed = NULL; // Oldest ancestor of pindex for which nTx == 0.
    CBlockIndex* pindexFirstNotTreeValid = NULL; // Oldest ancestor of pindex which does not have BLOCK_VALID_TREE (regardless of being valid or not).
    CBlockIndex* pindexFirstNotTransactionsValid = NULL; // Oldest ancestor of pindex which does not have BLOCK_VALID_TRANSACTIONS (regardless of being valid or not).
    CBlockIndex* pindexFirstNotChainValid = NULL; // Oldest ancestor of pindex which does not have BLOCK_VALID_CHAIN (regardless of being valid or not).
    CBlockIndex* pindexFirstNotScriptsValid = NULL; // Oldest ancestor of pindex which does not have BLOCK_VALID_SCRIPTS (regardless of being valid or not).
    while (pindex != NULL) {
        nNodes++;
        if (pindexFirstInvalid == NULL && pindex->nStatus & BLOCK_FAILED_VALID) pindexFirstInvalid = pindex;
        if (pindexFirstMissing == NULL && !(pindex->nStatus & BLOCK_HAVE_DATA)) pindexFirstMissing = pindex;
        if (pindexFirstNeverProcessed == NULL && pindex->nTx == 0) pindexFirstNeverProcessed = pindex;
        if (pindex->pprev != NULL && pindexFirstNotTreeValid == NULL && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TREE) pindexFirstNotTreeValid = pindex;
        if (pindex->pprev != NULL && pindexFirstNotTransactionsValid == NULL && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TRANSACTIONS) pindexFirstNotTransactionsValid = pindex;
        if (pindex->pprev != NULL && pindexFirstNotChainValid == NULL && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_CHAIN) pindexFirstNotChainValid = pindex;
        if (pindex->pprev != NULL && pindexFirstNotScriptsValid == NULL && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_SCRIPTS) pindexFirstNotScriptsValid = pindex;

        // Begin: actual consistency checks.
        if (pindex->pprev == NULL) {
            // Genesis block checks.
            assert(pindex->GetBlockHash() == consensusParams.hashGenesisBlock); // Genesis block's hash must match.
            assert(pindex == chainActive.Genesis()); // The current active chain's genesis block must be this block.
        }
        if (pindex->nChainTx == 0) assert(pindex->nSequenceId == 0);  // nSequenceId can't be set for blocks that aren't linked
        // VALID_TRANSACTIONS is equivalent to nTx > 0 for all nodes (whether or not pruning has occurred).
        // HAVE_DATA is only equivalent to nTx > 0 (or VALID_TRANSACTIONS) if no pruning has occurred.
        if (!fHavePruned) {
            // If we've never pruned, then HAVE_DATA should be equivalent to nTx > 0
            assert(!(pindex->nStatus & BLOCK_HAVE_DATA) == (pindex->nTx == 0));
            assert(pindexFirstMissing == pindexFirstNeverProcessed);
        } else {
            // If we have pruned, then we can only say that HAVE_DATA implies nTx > 0
            if (pindex->nStatus & BLOCK_HAVE_DATA) assert(pindex->nTx > 0);
        }
        if (pindex->nStatus & BLOCK_HAVE_UNDO) assert(pindex->nStatus & BLOCK_HAVE_DATA);
        assert(((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TRANSACTIONS) == (pindex->nTx > 0)); // This is pruning-independent.
        // All parents having had data (at some point) is equivalent to all parents being VALID_TRANSACTIONS, which is equivalent to nChainTx being set.
        assert((pindexFirstNeverProcessed != NULL) == (pindex->nChainTx == 0)); // nChainTx != 0 is used to signal that all parent blocks have been processed (but may have been pruned).
        assert((pindexFirstNotTransactionsValid != NULL) == (pindex->nChainTx == 0));
        assert(pindex->nHeight == nHeight); // nHeight must be consistent.
        assert(pindex->pprev == NULL || pindex->nChainWork >= pindex->pprev->nChainWork); // For every block except the genesis block, the chainwork must be larger than the parent's.
        assert(nHeight < 2 || (pindex->pskip && (pindex->pskip->nHeight < nHeight))); // The pskip pointer must point back for all but the first 2 blocks.
        assert(pindexFirstNotTreeValid == NULL); // All mapBlockIndex entries must at least be TREE valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TREE) assert(pindexFirstNotTreeValid == NULL); // TREE valid implies all parents are TREE valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_CHAIN) assert(pindexFirstNotChainValid == NULL); // CHAIN valid implies all parents are CHAIN valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_SCRIPTS) assert(pindexFirstNotScriptsValid == NULL); // SCRIPTS valid implies all parents are SCRIPTS valid
        if (pindexFirstInvalid == NULL) {
            // Checks for not-invalid blocks.
            assert((pindex->nStatus & BLOCK_FAILED_MASK) == 0); // The failed mask cannot be set for blocks without invalid parents.
        }
        if (!CBlockIndexWorkComparator()(pindex, chainActive.Tip()) && pindexFirstNeverProcessed == NULL) {
            if (pindexFirstInvalid == NULL) {
                // If this block sorts at least as good as the current tip and
                // is valid and we have all data for its parents, it must be in
                // setBlockIndexCandidates.  chainActive.Tip() must also be there
                // even if some data has been pruned.
                if (pindexFirstMissing == NULL || pindex == chainActive.Tip()) {
                    assert(setBlockIndexCandidates.count(pindex));
                }
                // If some parent is missing, then it could be that this block was in
                // setBlockIndexCandidates but had to be removed because of the missing data.
                // In this case it must be in mapBlocksUnlinked -- see test below.
            }
        } else { // If this block sorts worse than the current tip or some ancestor's block has never been seen, it cannot be in setBlockIndexCandidates.
            assert(setBlockIndexCandidates.count(pindex) == 0);
        }
        // Check whether this block is in mapBlocksUnlinked.
        std::pair<std::multimap<CBlockIndex*,CBlockIndex*>::iterator,std::multimap<CBlockIndex*,CBlockIndex*>::iterator> rangeUnlinked = mapBlocksUnlinked.equal_range(pindex->pprev);
        bool foundInUnlinked = false;
        while (rangeUnlinked.first != rangeUnlinked.second) {
            assert(rangeUnlinked.first->first == pindex->pprev);
            if (rangeUnlinked.first->second == pindex) {
                foundInUnlinked = true;
                break;
            }
            rangeUnlinked.first++;
        }
        if (pindex->pprev && (pindex->nStatus & BLOCK_HAVE_DATA) && pindexFirstNeverProcessed != NULL && pindexFirstInvalid == NULL) {
            // If this block has block data available, some parent was never received, and has no invalid parents, it must be in mapBlocksUnlinked.
            assert(foundInUnlinked);
        }
        if (!(pindex->nStatus & BLOCK_HAVE_DATA)) assert(!foundInUnlinked); // Can't be in mapBlocksUnlinked if we don't HAVE_DATA
        if (pindexFirstMissing == NULL) assert(!foundInUnlinked); // We aren't missing data for any parent -- cannot be in mapBlocksUnlinked.
        if (pindex->pprev && (pindex->nStatus & BLOCK_HAVE_DATA) && pindexFirstNeverProcessed == NULL && pindexFirstMissing != NULL) {
            // We HAVE_DATA for this block, have received data for all parents at some point, but we're currently missing data for some parent.
            assert(fHavePruned); // We must have pruned.
            // This block may have entered mapBlocksUnlinked if:
            //  - it has a descendant that at some point had more work than the
            //    tip, and
            //  - we tried switching to that descendant but were missing
            //    data for some intermediate block between chainActive and the
            //    tip.
            // So if this block is itself better than chainActive.Tip() and it wasn't in
            // setBlockIndexCandidates, then it must be in mapBlocksUnlinked.
            if (!CBlockIndexWorkComparator()(pindex, chainActive.Tip()) && setBlockIndexCandidates.count(pindex) == 0) {
                if (pindexFirstInvalid == NULL) {
                    assert(foundInUnlinked);
                }
            }
        }
        // assert(pindex->GetBlockHash() == pindex->GetBlockHeader().GetHash()); // Perhaps too slow
        // End: actual consistency checks.

        // Try descending into the first subnode.
        std::pair<std::multimap<CBlockIndex*,CBlockIndex*>::iterator,std::multimap<CBlockIndex*,CBlockIndex*>::iterator> range = forward.equal_range(pindex);
        if (range.first != range.second) {
            // A subnode was found.
            pindex = range.first->second;
            nHeight++;
            continue;
        }
        // This is a leaf node.
        // Move upwards until we reach a node of which we have not yet visited the last child.
        while (pindex) {
            // We are going to either move to a parent or a sibling of pindex.
            // If pindex was the first with a certain property, unset the corresponding variable.
            if (pindex == pindexFirstInvalid) pindexFirstInvalid = NULL;
            if (pindex == pindexFirstMissing) pindexFirstMissing = NULL;
            if (pindex == pindexFirstNeverProcessed) pindexFirstNeverProcessed = NULL;
            if (pindex == pindexFirstNotTreeValid) pindexFirstNotTreeValid = NULL;
            if (pindex == pindexFirstNotTransactionsValid) pindexFirstNotTransactionsValid = NULL;
            if (pindex == pindexFirstNotChainValid) pindexFirstNotChainValid = NULL;
            if (pindex == pindexFirstNotScriptsValid) pindexFirstNotScriptsValid = NULL;
            // Find our parent.
            CBlockIndex* pindexPar = pindex->pprev;
            // Find which child we just visited.
            std::pair<std::multimap<CBlockIndex*,CBlockIndex*>::iterator,std::multimap<CBlockIndex*,CBlockIndex*>::iterator> rangePar = forward.equal_range(pindexPar);
            while (rangePar.first->second != pindex) {
                assert(rangePar.first != rangePar.second); // Our parent must have at least the node we're coming from as child.
                rangePar.first++;
            }
            // Proceed to the next one.
            rangePar.first++;
            if (rangePar.first != rangePar.second) {
                // Move to the sibling.
                pindex = rangePar.first->second;
                break;
            } else {
                // Move up further.
                pindex = pindexPar;
                nHeight--;
                continue;
            }
        }
    }

    // Check that we actually traversed the entire map.
    assert(nNodes == forward.size());
}

std::string GetWarnings(const std::string& strFor)
{
    string strStatusBar;
    string strRPC;
    string strGUI;
    const string uiAlertSeperator = "<hr />";

    if (!CLIENT_VERSION_IS_RELEASE) {
        strStatusBar = "This is a pre-release test build - use at your own risk - do not use for mining or merchant applications";
        strGUI = _("This is a pre-release test build - use at your own risk - do not use for mining or merchant applications");
    }

    if (GetBoolArg("-testsafemode", DEFAULT_TESTSAFEMODE))
        strStatusBar = strRPC = strGUI = "testsafemode enabled";

    // Misc warnings like out of disk space and clock is wrong
    if (strMiscWarning != "")
    {
        strStatusBar = strMiscWarning;
        strGUI += (strGUI.empty() ? "" : uiAlertSeperator) + strMiscWarning;
    }

    if (fLargeWorkForkFound)
    {
        strStatusBar = strRPC = "Warning: The network does not appear to fully agree! Some miners appear to be experiencing issues.";
        strGUI += (strGUI.empty() ? "" : uiAlertSeperator) + _("Warning: The network does not appear to fully agree! Some miners appear to be experiencing issues.");
    }
    else if (fLargeWorkInvalidChainFound)
    {
        strStatusBar = strRPC = "Warning: We do not appear to fully agree with our peers! You may need to upgrade, or other nodes may need to upgrade.";
        strGUI += (strGUI.empty() ? "" : uiAlertSeperator) + _("Warning: We do not appear to fully agree with our peers! You may need to upgrade, or other nodes may need to upgrade.");
    }

    if (strFor == "gui")
        return strGUI;
    else if (strFor == "statusbar")
        return strStatusBar;
    else if (strFor == "rpc")
        return strRPC;
    assert(!"GetWarnings(): invalid parameter");
    return "error";
}








//////////////////////////////////////////////////////////////////////////////
// Messages
//


bool static AlreadyHave(const CInv& inv) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    switch (inv.type)
    {
    case MSG_TX:
    case MSG_WITNESS_TX:
        {
            assert(recentRejects);
            if (chainActive.Tip()->GetBlockHash() != hashRecentRejectsChainTip)
            {
                // If the chain tip has changed previously rejected transactions
                // might be now valid, e.g. due to a nLockTime'd tx becoming valid,
                // or a double-spend. Reset the rejects filter and give those
                // txs a second chance.
                hashRecentRejectsChainTip = chainActive.Tip()->GetBlockHash();
                recentRejects->reset();
            }

            // Use pcoinsTip->HaveCoinsInCache as a quick approximation to exclude
            // requesting or processing some txs which have already been included in a block
            return recentRejects->contains(inv.hash) ||
                   mempool.exists(inv.hash) ||
                   mapOrphanTransactions.count(inv.hash) ||
                   pcoinsTip->HaveCoinsInCache(inv.hash);
        }
    case MSG_BLOCK:
    case MSG_WITNESS_BLOCK:
        return mapBlockIndex.count(inv.hash);
    }
    // Don't know what it is, just say we already got one
    return true;
}

void static ProcessGetData(CNode* pfrom, const Consensus::Params& consensusParams)
{
    std::deque<CInv>::iterator it = pfrom->vRecvGetData.begin();

    vector<CInv> vNotFound;

    LOCK(cs_main);

    while (it != pfrom->vRecvGetData.end()) {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        const CInv &inv = *it;
        {
            boost::this_thread::interruption_point();
            it++;

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK || inv.type == MSG_CMPCT_BLOCK || inv.type == MSG_WITNESS_BLOCK)
            {
                bool send = false;
                BlockMap::iterator mi = mapBlockIndex.find(inv.hash);
                if (mi != mapBlockIndex.end())
                {
                    if (chainActive.Contains(mi->second)) {
                        send = true;
                    } else {
                        static const int nOneMonth = 30 * 24 * 60 * 60;
                        // To prevent fingerprinting attacks, only send blocks outside of the active
                        // chain if they are valid, and no more than a month older (both in time, and in
                        // best equivalent proof of work) than the best header chain we know about.
                        send = mi->second->IsValid(BLOCK_VALID_SCRIPTS) && (pindexBestHeader != NULL) &&
                            (pindexBestHeader->GetBlockTime() - mi->second->GetBlockTime() < nOneMonth) &&
                            (GetBlockProofEquivalentTime(*pindexBestHeader, *mi->second, *pindexBestHeader, consensusParams) < nOneMonth);
                        if (!send) {
                            LogPrintf("%s: ignoring request from peer=%i for old block that isn't in the main chain\n", __func__, pfrom->GetId());
                        }
                    }
                }
                // disconnect node in case we have reached the outbound limit for serving historical blocks
                // never disconnect whitelisted nodes
                static const int nOneWeek = 7 * 24 * 60 * 60; // assume > 1 week = historical
                if (send && CNode::OutboundTargetReached(true) && ( ((pindexBestHeader != NULL) && (pindexBestHeader->GetBlockTime() - mi->second->GetBlockTime() > nOneWeek)) || inv.type == MSG_FILTERED_BLOCK) && !pfrom->fWhitelisted)
                {
                    LogPrint("net", "historical block serving limit reached, disconnect peer=%d\n", pfrom->GetId());

                    //disconnect node
                    pfrom->fDisconnect = true;
                    send = false;
                }
                // Pruned nodes may have deleted the block, so check whether
                // it's available before trying to send.
                if (send && (mi->second->nStatus & BLOCK_HAVE_DATA))
                {
                    // Send block from disk
                    CBlock block;
                    if (!ReadBlockFromDisk(block, (*mi).second, consensusParams))
                        assert(!"cannot load block from disk");
                    if (inv.type == MSG_BLOCK)
                        pfrom->PushMessageWithFlag(SERIALIZE_TRANSACTION_NO_WITNESS, NetMsgType::BLOCK, block);
                    else if (inv.type == MSG_WITNESS_BLOCK)
                        pfrom->PushMessage(NetMsgType::BLOCK, block);
                    else if (inv.type == MSG_FILTERED_BLOCK)
                    {
                        bool send = false;
                        CMerkleBlock merkleBlock;
                        {
                            LOCK(pfrom->cs_filter);
                            if (pfrom->pfilter) {
                                send = true;
                                merkleBlock = CMerkleBlock(block, *pfrom->pfilter);
                            }
                        }
                        if (send) {
                            pfrom->PushMessage(NetMsgType::MERKLEBLOCK, merkleBlock);
                            // CMerkleBlock just contains hashes, so also push any transactions in the block the client did not see
                            // This avoids hurting performance by pointlessly requiring a round-trip
                            // Note that there is currently no way for a node to request any single transactions we didn't send here -
                            // they must either disconnect and retry or request the full block.
                            // Thus, the protocol spec specified allows for us to provide duplicate txn here,
                            // however we MUST always provide at least what the remote peer needs
                            typedef std::pair<unsigned int, uint256> PairType;
                            BOOST_FOREACH(PairType& pair, merkleBlock.vMatchedTxn)
                                pfrom->PushMessageWithFlag(SERIALIZE_TRANSACTION_NO_WITNESS, NetMsgType::TX, block.vtx[pair.first]);
                        }
                        // else
                            // no response
                    }
                    else if (inv.type == MSG_CMPCT_BLOCK)
                    {
                        // If a peer is asking for old blocks, we're almost guaranteed
                        // they wont have a useful mempool to match against a compact block,
                        // and we don't feel like constructing the object for them, so
                        // instead we respond with the full, non-compact block.
                        bool fPeerWantsWitness = State(pfrom->GetId())->fWantsCmpctWitness;
                        if (CanDirectFetch(consensusParams) && mi->second->nHeight >= chainActive.Height() - MAX_CMPCTBLOCK_DEPTH) {
                            CBlockHeaderAndShortTxIDs cmpctblock(block, fPeerWantsWitness);
                            pfrom->PushMessageWithFlag(fPeerWantsWitness ? 0 : SERIALIZE_TRANSACTION_NO_WITNESS, NetMsgType::CMPCTBLOCK, cmpctblock);
                        } else
                            pfrom->PushMessageWithFlag(fPeerWantsWitness ? 0 : SERIALIZE_TRANSACTION_NO_WITNESS, NetMsgType::BLOCK, block);
                    }

                    // Trigger the peer node to send a getblocks request for the next batch of inventory
                    if (inv.hash == pfrom->hashContinue)
                    {
                        // Bypass PushInventory, this must send even if redundant,
                        // and we want it right after the last block so they don't
                        // wait for other stuff first.
                        vector<CInv> vInv;
                        vInv.push_back(CInv(MSG_BLOCK, chainActive.Tip()->GetBlockHash()));
                        pfrom->PushMessage(NetMsgType::INV, vInv);
                        pfrom->hashContinue.SetNull();
                    }
                }
            }
            else if (inv.type == MSG_TX || inv.type == MSG_WITNESS_TX)
            {
                // Send stream from relay memory
                bool push = false;
                auto mi = mapRelay.find(inv.hash);
                if (mi != mapRelay.end()) {
                    pfrom->PushMessageWithFlag(inv.type == MSG_TX ? SERIALIZE_TRANSACTION_NO_WITNESS : 0, NetMsgType::TX, *mi->second);
                    push = true;
                } else if (pfrom->timeLastMempoolReq) {
                    auto txinfo = mempool.info(inv.hash);
                    // To protect privacy, do not answer getdata using the mempool when
                    // that TX couldn't have been INVed in reply to a MEMPOOL request.
                    if (txinfo.tx && txinfo.nTime <= pfrom->timeLastMempoolReq) {
                        pfrom->PushMessageWithFlag(inv.type == MSG_TX ? SERIALIZE_TRANSACTION_NO_WITNESS : 0, NetMsgType::TX, *txinfo.tx);
                        push = true;
                    }
                }
                if (!push) {
                    vNotFound.push_back(inv);
                }
            }

            // Track requests for our stuff.
            GetMainSignals().Inventory(inv.hash);

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK || inv.type == MSG_CMPCT_BLOCK || inv.type == MSG_WITNESS_BLOCK)
                break;
        }
    }

    pfrom->vRecvGetData.erase(pfrom->vRecvGetData.begin(), it);

    if (!vNotFound.empty()) {
        // Let the peer know that we didn't find what it asked for, so it doesn't
        // have to wait around forever. Currently only SPV clients actually care
        // about this message: it's needed when they are recursively walking the
        // dependencies of relevant unconfirmed transactions. SPV clients want to
        // do that because they want to know about (and store and rebroadcast and
        // risk analyze) the dependencies of transactions relevant to them, without
        // having to download the entire memory pool.
        pfrom->PushMessage(NetMsgType::NOTFOUND, vNotFound);
    }
}

uint32_t GetFetchFlags(CNode* pfrom, CBlockIndex* pprev, const Consensus::Params& chainparams) {
    uint32_t nFetchFlags = 0;
    if ((nLocalServices & NODE_WITNESS) && State(pfrom->GetId())->fHaveWitness) {
        nFetchFlags |= MSG_WITNESS_FLAG;
    }
    return nFetchFlags;
}

bool static ProcessMessage(CNode* pfrom, string strCommand, CDataStream& vRecv, int64_t nTimeReceived, const CChainParams& chainparams)
{
    LogPrint("net", "received: %s (%u bytes) peer=%d\n", SanitizeString(strCommand), vRecv.size(), pfrom->id);
    if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0)
    {
        LogPrintf("dropmessagestest DROPPING RECV MESSAGE\n");
        return true;
    }


    if (!(nLocalServices & NODE_BLOOM) &&
              (strCommand == NetMsgType::FILTERLOAD ||
               strCommand == NetMsgType::FILTERADD ||
               strCommand == NetMsgType::FILTERCLEAR))
    {
        if (pfrom->nVersion >= NO_BLOOM_VERSION) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 100);
            return false;
        } else {
            pfrom->fDisconnect = true;
            return false;
        }
    }


    if (strCommand == NetMsgType::VERSION)
    {
        // Feeler connections exist only to verify if address is online.
        if (pfrom->fFeeler) {
            assert(pfrom->fInbound == false);
            pfrom->fDisconnect = true;
        }

        // Each connection can only send one version message
        if (pfrom->nVersion != 0)
        {
            pfrom->PushMessage(NetMsgType::REJECT, strCommand, REJECT_DUPLICATE, string("Duplicate version message"));
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 1);
            return false;
        }

        int64_t nTime;
        CAddress addrMe;
        CAddress addrFrom;
        uint64_t nNonce = 1;
        uint64_t nServiceInt;
        vRecv >> pfrom->nVersion >> nServiceInt >> nTime >> addrMe;
        pfrom->nServices = ServiceFlags(nServiceInt);
        if (!pfrom->fInbound)
        {
            addrman.SetServices(pfrom->addr, pfrom->nServices);
        }
        if (pfrom->nServicesExpected & ~pfrom->nServices)
        {
            LogPrint("net", "peer=%d does not offer the expected services (%08x offered, %08x expected); disconnecting\n", pfrom->id, pfrom->nServices, pfrom->nServicesExpected);
            pfrom->PushMessage(NetMsgType::REJECT, strCommand, REJECT_NONSTANDARD,
                               strprintf("Expected to offer services %08x", pfrom->nServicesExpected));
            pfrom->fDisconnect = true;
            return false;
        }

        if (pfrom->nVersion < MIN_PEER_PROTO_VERSION)
        {
            // disconnect from peers older than this proto version
            LogPrintf("peer=%d using obsolete version %i; disconnecting\n", pfrom->id, pfrom->nVersion);
            pfrom->PushMessage(NetMsgType::REJECT, strCommand, REJECT_OBSOLETE,
                               strprintf("Version must be %d or greater", MIN_PEER_PROTO_VERSION));
            pfrom->fDisconnect = true;
            return false;
        }

        if (pfrom->nVersion == 10300)
            pfrom->nVersion = 300;
        if (!vRecv.empty())
            vRecv >> addrFrom >> nNonce;
        if (!vRecv.empty()) {
            vRecv >> LIMITED_STRING(pfrom->strSubVer, MAX_SUBVERSION_LENGTH);
            pfrom->cleanSubVer = SanitizeString(pfrom->strSubVer);
        }
        if (!vRecv.empty()) {
            vRecv >> pfrom->nStartingHeight;
        }
        {
            LOCK(pfrom->cs_filter);
            if (!vRecv.empty())
                vRecv >> pfrom->fRelayTxes; // set to true after we get the first filter* message
            else
                pfrom->fRelayTxes = true;
        }

        // Disconnect if we connected to ourself
        if (nNonce == nLocalHostNonce && nNonce > 1)
        {
            LogPrintf("connected to self at %s, disconnecting\n", pfrom->addr.ToString());
            pfrom->fDisconnect = true;
            return true;
        }

        pfrom->addrLocal = addrMe;
        if (pfrom->fInbound && addrMe.IsRoutable())
        {
            SeenLocal(addrMe);
        }

        // Be shy and don't send version until we hear
        if (pfrom->fInbound)
            pfrom->PushVersion();

        pfrom->fClient = !(pfrom->nServices & NODE_NETWORK);

        if((pfrom->nServices & NODE_WITNESS))
        {
            LOCK(cs_main);
            State(pfrom->GetId())->fHaveWitness = true;
        }

        // Potentially mark this peer as a preferred download peer.
        {
        LOCK(cs_main);
        UpdatePreferredDownload(pfrom, State(pfrom->GetId()));
        }

        // Change version
        pfrom->PushMessage(NetMsgType::VERACK);
        pfrom->ssSend.SetVersion(min(pfrom->nVersion, PROTOCOL_VERSION));

        if (!pfrom->fInbound)
        {
            // Advertise our address
            if (fListen && !IsInitialBlockDownload())
            {
                CAddress addr = GetLocalAddress(&pfrom->addr);
                if (addr.IsRoutable())
                {
                    LogPrintf("ProcessMessages: advertising address %s\n", addr.ToString());
                    pfrom->PushAddress(addr);
                } else if (IsPeerAddrLocalGood(pfrom)) {
                    addr.SetIP(pfrom->addrLocal);
                    LogPrintf("ProcessMessages: advertising address %s\n", addr.ToString());
                    pfrom->PushAddress(addr);
                }
            }

            // Get recent addresses
            if (pfrom->fOneShot || pfrom->nVersion >= CADDR_TIME_VERSION || addrman.size() < 1000)
            {
                pfrom->PushMessage(NetMsgType::GETADDR);
                pfrom->fGetAddr = true;
            }
            addrman.Good(pfrom->addr);
        }

        pfrom->fSuccessfullyConnected = true;

        string remoteAddr;
        if (fLogIPs)
            remoteAddr = ", peeraddr=" + pfrom->addr.ToString();

        LogPrintf("receive version message: %s: version %d, blocks=%d, us=%s, peer=%d%s\n",
                  pfrom->cleanSubVer, pfrom->nVersion,
                  pfrom->nStartingHeight, addrMe.ToString(), pfrom->id,
                  remoteAddr);

        int64_t nTimeOffset = nTime - GetTime();
        pfrom->nTimeOffset = nTimeOffset;
        AddTimeData(pfrom->addr, nTimeOffset);
    }


    else if (pfrom->nVersion == 0)
    {
        // Must have a version message before anything else
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), 1);
        return false;
    }


    else if (strCommand == NetMsgType::VERACK)
    {
        pfrom->SetRecvVersion(min(pfrom->nVersion, PROTOCOL_VERSION));

        // Mark this node as currently connected, so we update its timestamp later.
        if (pfrom->fNetworkNode) {
            LOCK(cs_main);
            State(pfrom->GetId())->fCurrentlyConnected = true;
        }

        if (pfrom->nVersion >= SENDHEADERS_VERSION) {
            // Tell our peer we prefer to receive headers rather than inv's
            // We send this to non-NODE NETWORK peers as well, because even
            // non-NODE NETWORK peers can announce blocks (such as pruning
            // nodes)
            pfrom->PushMessage(NetMsgType::SENDHEADERS);
        }
        if (pfrom->nVersion >= SHORT_IDS_BLOCKS_VERSION) {
            // Tell our peer we are willing to provide version 1 or 2 cmpctblocks
            // However, we do not request new block announcements using
            // cmpctblock messages.
            // We send this to non-NODE NETWORK peers as well, because
            // they may wish to request compact blocks from us
            bool fAnnounceUsingCMPCTBLOCK = false;
            uint64_t nCMPCTBLOCKVersion = 2;
            if (nLocalServices & NODE_WITNESS)
                pfrom->PushMessage(NetMsgType::SENDCMPCT, fAnnounceUsingCMPCTBLOCK, nCMPCTBLOCKVersion);
            nCMPCTBLOCKVersion = 1;
            pfrom->PushMessage(NetMsgType::SENDCMPCT, fAnnounceUsingCMPCTBLOCK, nCMPCTBLOCKVersion);
        }
    }


    else if (strCommand == NetMsgType::ADDR)
    {
        vector<CAddress> vAddr;
        vRecv >> vAddr;

        // Don't want addr from older versions unless seeding
        if (pfrom->nVersion < CADDR_TIME_VERSION && addrman.size() > 1000)
            return true;
        if (vAddr.size() > 1000)
        {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 20);
            return error("message addr size() = %u", vAddr.size());
        }

        // Store the new addresses
        vector<CAddress> vAddrOk;
        int64_t nNow = GetAdjustedTime();
        int64_t nSince = nNow - 10 * 60;
        BOOST_FOREACH(CAddress& addr, vAddr)
        {
            boost::this_thread::interruption_point();

            if ((addr.nServices & REQUIRED_SERVICES) != REQUIRED_SERVICES)
                continue;

            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
                addr.nTime = nNow - 5 * 24 * 60 * 60;
            pfrom->AddAddressKnown(addr);
            bool fReachable = IsReachable(addr);
            if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable())
            {
                // Relay to a limited number of other nodes
                {
                    LOCK(cs_vNodes);
                    // Use deterministic randomness to send to the same nodes for 24 hours
                    // at a time so the addrKnowns of the chosen nodes prevent repeats
                    static const uint64_t salt0 = GetRand(std::numeric_limits<uint64_t>::max());
                    static const uint64_t salt1 = GetRand(std::numeric_limits<uint64_t>::max());
                    uint64_t hashAddr = addr.GetHash();
                    multimap<uint64_t, CNode*> mapMix;
                    const CSipHasher hasher = CSipHasher(salt0, salt1).Write(hashAddr << 32).Write((GetTime() + hashAddr) / (24*60*60));
                    BOOST_FOREACH(CNode* pnode, vNodes)
                    {
                        if (pnode->nVersion < CADDR_TIME_VERSION)
                            continue;
                        uint64_t hashKey = CSipHasher(hasher).Write(pnode->id).Finalize();
                        mapMix.insert(make_pair(hashKey, pnode));
                    }
                    int nRelayNodes = fReachable ? 2 : 1; // limited relaying of addresses outside our network(s)
                    for (multimap<uint64_t, CNode*>::iterator mi = mapMix.begin(); mi != mapMix.end() && nRelayNodes-- > 0; ++mi)
                        ((*mi).second)->PushAddress(addr);
                }
            }
            // Do not store addresses outside our network
            if (fReachable)
                vAddrOk.push_back(addr);
        }
        addrman.Add(vAddrOk, pfrom->addr, 2 * 60 * 60);
        if (vAddr.size() < 1000)
            pfrom->fGetAddr = false;
        if (pfrom->fOneShot)
            pfrom->fDisconnect = true;
    }

    else if (strCommand == NetMsgType::SENDHEADERS)
    {
        LOCK(cs_main);
        State(pfrom->GetId())->fPreferHeaders = true;
    }

    else if (strCommand == NetMsgType::SENDCMPCT)
    {
        bool fAnnounceUsingCMPCTBLOCK = false;
        uint64_t nCMPCTBLOCKVersion = 0;
        vRecv >> fAnnounceUsingCMPCTBLOCK >> nCMPCTBLOCKVersion;
        if (nCMPCTBLOCKVersion == 1 || ((nLocalServices & NODE_WITNESS) && nCMPCTBLOCKVersion == 2)) {
            LOCK(cs_main);
            // fProvidesHeaderAndIDs is used to "lock in" version of compact blocks we send (fWantsCmpctWitness)
            if (!State(pfrom->GetId())->fProvidesHeaderAndIDs) {
                State(pfrom->GetId())->fProvidesHeaderAndIDs = true;
                State(pfrom->GetId())->fWantsCmpctWitness = nCMPCTBLOCKVersion == 2;
            }
            if (State(pfrom->GetId())->fWantsCmpctWitness == (nCMPCTBLOCKVersion == 2)) // ignore later version announces
                State(pfrom->GetId())->fPreferHeaderAndIDs = fAnnounceUsingCMPCTBLOCK;
            if (!State(pfrom->GetId())->fSupportsDesiredCmpctVersion) {
                if (nLocalServices & NODE_WITNESS)
                    State(pfrom->GetId())->fSupportsDesiredCmpctVersion = (nCMPCTBLOCKVersion == 2);
                else
                    State(pfrom->GetId())->fSupportsDesiredCmpctVersion = (nCMPCTBLOCKVersion == 1);
            }
        }
    }


    else if (strCommand == NetMsgType::INV)
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 20);
            return error("message inv size() = %u", vInv.size());
        }

        bool fBlocksOnly = !fRelayTxes;

        // Allow whitelisted peers to send data other than blocks in blocks only mode if whitelistrelay is true
        if (pfrom->fWhitelisted && GetBoolArg("-whitelistrelay", DEFAULT_WHITELISTRELAY))
            fBlocksOnly = false;

        LOCK(cs_main);

        uint32_t nFetchFlags = GetFetchFlags(pfrom, chainActive.Tip(), chainparams.GetConsensus());

        std::vector<CInv> vToFetch;

        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++)
        {
            CInv &inv = vInv[nInv];

            boost::this_thread::interruption_point();

            bool fAlreadyHave = AlreadyHave(inv);
            LogPrint("net", "got inv: %s  %s peer=%d\n", inv.ToString(), fAlreadyHave ? "have" : "new", pfrom->id);

            if (inv.type == MSG_TX) {
                inv.type |= nFetchFlags;
            }

            if (inv.type == MSG_BLOCK) {
                UpdateBlockAvailability(pfrom->GetId(), inv.hash);
                if (!fAlreadyHave && !fImporting && !fReindex && !mapBlocksInFlight.count(inv.hash)) {
                    // First request the headers preceding the announced block. In the normal fully-synced
                    // case where a new block is announced that succeeds the current tip (no reorganization),
                    // there are no such headers.
                    // Secondly, and only when we are close to being synced, we request the announced block directly,
                    // to avoid an extra round-trip. Note that we must *first* ask for the headers, so by the
                    // time the block arrives, the header chain leading up to it is already validated. Not
                    // doing this will result in the received block being rejected as an orphan in case it is
                    // not a direct successor.
                    pfrom->PushMessage(NetMsgType::GETHEADERS, chainActive.GetLocator(pindexBestHeader), inv.hash);
                    CNodeState *nodestate = State(pfrom->GetId());
                    if (CanDirectFetch(chainparams.GetConsensus()) &&
                        nodestate->nBlocksInFlight < MAX_BLOCKS_IN_TRANSIT_PER_PEER &&
                        (!IsWitnessEnabled(chainActive.Tip(), chainparams.GetConsensus()) || State(pfrom->GetId())->fHaveWitness)) {
                        inv.type |= nFetchFlags;
                        if (nodestate->fSupportsDesiredCmpctVersion)
                            vToFetch.push_back(CInv(MSG_CMPCT_BLOCK, inv.hash));
                        else
                            vToFetch.push_back(inv);
                        // Mark block as in flight already, even though the actual "getdata" message only goes out
                        // later (within the same cs_main lock, though).
                        MarkBlockAsInFlight(pfrom->GetId(), inv.hash, chainparams.GetConsensus());
                    }
                    LogPrint("net", "getheaders (%d) %s to peer=%d\n", pindexBestHeader->nHeight, inv.hash.ToString(), pfrom->id);
                }
            }
            else
            {
                pfrom->AddInventoryKnown(inv);
                if (fBlocksOnly)
                    LogPrint("net", "transaction (%s) inv sent in violation of protocol peer=%d\n", inv.hash.ToString(), pfrom->id);
                else if (!fAlreadyHave && !fImporting && !fReindex && !IsInitialBlockDownload())
                    pfrom->AskFor(inv);
            }

            // Track requests for our stuff
            GetMainSignals().Inventory(inv.hash);

            if (pfrom->nSendSize > (SendBufferSize() * 2)) {
                Misbehaving(pfrom->GetId(), 50);
                return error("send buffer size() = %u", pfrom->nSendSize);
            }
        }

        if (!vToFetch.empty())
            pfrom->PushMessage(NetMsgType::GETDATA, vToFetch);
    }


    else if (strCommand == NetMsgType::GETDATA)
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 20);
            return error("message getdata size() = %u", vInv.size());
        }

        if (fDebug || (vInv.size() != 1))
            LogPrint("net", "received getdata (%u invsz) peer=%d\n", vInv.size(), pfrom->id);

        if ((fDebug && vInv.size() > 0) || (vInv.size() == 1))
            LogPrint("net", "received getdata for: %s peer=%d\n", vInv[0].ToString(), pfrom->id);

        pfrom->vRecvGetData.insert(pfrom->vRecvGetData.end(), vInv.begin(), vInv.end());
        ProcessGetData(pfrom, chainparams.GetConsensus());
    }


    else if (strCommand == NetMsgType::GETBLOCKS)
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        LOCK(cs_main);

        // Find the last block the caller has in the main chain
        CBlockIndex* pindex = FindForkInGlobalIndex(chainActive, locator);

        // Send the rest of the chain
        if (pindex)
            pindex = chainActive.Next(pindex);
        int nLimit = 500;
        LogPrint("net", "getblocks %d to %s limit %d from peer=%d\n", (pindex ? pindex->nHeight : -1), hashStop.IsNull() ? "end" : hashStop.ToString(), nLimit, pfrom->id);
        for (; pindex; pindex = chainActive.Next(pindex))
        {
            if (pindex->GetBlockHash() == hashStop)
            {
                LogPrint("net", "  getblocks stopping at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                break;
            }
            // If pruning, don't inv blocks unless we have on disk and are likely to still have
            // for some reasonable time window (1 hour) that block relay might require.
            const int nPrunedBlocksLikelyToHave = MIN_BLOCKS_TO_KEEP - 3600 / chainparams.GetConsensus().nPowTargetSpacing;
            if (fPruneMode && (!(pindex->nStatus & BLOCK_HAVE_DATA) || pindex->nHeight <= chainActive.Tip()->nHeight - nPrunedBlocksLikelyToHave))
            {
                LogPrint("net", " getblocks stopping, pruned or too old block at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                break;
            }
            pfrom->PushInventory(CInv(MSG_BLOCK, pindex->GetBlockHash()));
            if (--nLimit <= 0)
            {
                // When this block is requested, we'll send an inv that'll
                // trigger the peer to getblocks the next batch of inventory.
                LogPrint("net", "  getblocks stopping at limit %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                pfrom->hashContinue = pindex->GetBlockHash();
                break;
            }
        }
    }


    else if (strCommand == NetMsgType::GETBLOCKTXN)
    {
        BlockTransactionsRequest req;
        vRecv >> req;

        LOCK(cs_main);

        BlockMap::iterator it = mapBlockIndex.find(req.blockhash);
        if (it == mapBlockIndex.end() || !(it->second->nStatus & BLOCK_HAVE_DATA)) {
            LogPrintf("Peer %d sent us a getblocktxn for a block we don't have", pfrom->id);
            return true;
        }

        if (it->second->nHeight < chainActive.Height() - MAX_BLOCKTXN_DEPTH) {
            // If an older block is requested (should never happen in practice,
            // but can happen in tests) send a block response instead of a
            // blocktxn response. Sending a full block response instead of a
            // small blocktxn response is preferable in the case where a peer
            // might maliciously send lots of getblocktxn requests to trigger
            // expensive disk reads, because it will require the peer to
            // actually receive all the data read from disk over the network.
            LogPrint("net", "Peer %d sent us a getblocktxn for a block > %i deep", pfrom->id, MAX_BLOCKTXN_DEPTH);
            CInv inv;
            inv.type = State(pfrom->GetId())->fWantsCmpctWitness ? MSG_WITNESS_BLOCK : MSG_BLOCK;
            inv.hash = req.blockhash;
            pfrom->vRecvGetData.push_back(inv);
            ProcessGetData(pfrom, chainparams.GetConsensus());
            return true;
        }

        CBlock block;
        assert(ReadBlockFromDisk(block, it->second, chainparams.GetConsensus()));

        BlockTransactions resp(req);
        for (size_t i = 0; i < req.indexes.size(); i++) {
            if (req.indexes[i] >= block.vtx.size()) {
                Misbehaving(pfrom->GetId(), 100);
                LogPrintf("Peer %d sent us a getblocktxn with out-of-bounds tx indices", pfrom->id);
                return true;
            }
            resp.txn[i] = block.vtx[req.indexes[i]];
        }
        pfrom->PushMessageWithFlag(State(pfrom->GetId())->fWantsCmpctWitness ? 0 : SERIALIZE_TRANSACTION_NO_WITNESS, NetMsgType::BLOCKTXN, resp);
    }


    else if (strCommand == NetMsgType::GETHEADERS)
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        LOCK(cs_main);
        if (IsInitialBlockDownload() && !pfrom->fWhitelisted) {
            LogPrint("net", "Ignoring getheaders from peer=%d because node is in initial block download\n", pfrom->id);
            return true;
        }

        CNodeState *nodestate = State(pfrom->GetId());
        CBlockIndex* pindex = NULL;
        if (locator.IsNull())
        {
            // If locator is null, return the hashStop block
            BlockMap::iterator mi = mapBlockIndex.find(hashStop);
            if (mi == mapBlockIndex.end())
                return true;
            pindex = (*mi).second;
        }
        else
        {
            // Find the last block the caller has in the main chain
            pindex = FindForkInGlobalIndex(chainActive, locator);
            if (pindex)
                pindex = chainActive.Next(pindex);
        }

        // we must use CBlocks, as CBlockHeaders won't include the 0x00 nTx count at the end
        vector<CBlock> vHeaders;
        int nLimit = MAX_HEADERS_RESULTS;
        LogPrint("net", "getheaders %d to %s from peer=%d\n", (pindex ? pindex->nHeight : -1), hashStop.ToString(), pfrom->id);
        for (; pindex; pindex = chainActive.Next(pindex))
        {
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
        }
        // pindex can be NULL either if we sent chainActive.Tip() OR
        // if our peer has chainActive.Tip() (and thus we are sending an empty
        // headers message). In both cases it's safe to update
        // pindexBestHeaderSent to be our tip.
        nodestate->pindexBestHeaderSent = pindex ? pindex : chainActive.Tip();
        pfrom->PushMessage(NetMsgType::HEADERS, vHeaders);
    }


    else if (strCommand == NetMsgType::TX)
    {
        // Stop processing the transaction early if
        // We are in blocks only mode and peer is either not whitelisted or whitelistrelay is off
        if (!fRelayTxes && (!pfrom->fWhitelisted || !GetBoolArg("-whitelistrelay", DEFAULT_WHITELISTRELAY)))
        {
            LogPrint("net", "transaction sent in violation of protocol peer=%d\n", pfrom->id);
            return true;
        }

        deque<COutPoint> vWorkQueue;
        vector<uint256> vEraseQueue;
        CTransaction tx;
        vRecv >> tx;

        CInv inv(MSG_TX, tx.GetHash());
        pfrom->AddInventoryKnown(inv);

        LOCK(cs_main);

        bool fMissingInputs = false;
        CValidationState state;

        pfrom->setAskFor.erase(inv.hash);
        mapAlreadyAskedFor.erase(inv.hash);

        if (!AlreadyHave(inv) && AcceptToMemoryPool(mempool, state, tx, true, &fMissingInputs)) {
            mempool.check(pcoinsTip);
            RelayTransaction(tx);
            for (unsigned int i = 0; i < tx.vout.size(); i++) {
                vWorkQueue.emplace_back(inv.hash, i);
            }

            pfrom->nLastTXTime = GetTime();

            LogPrint("mempool", "AcceptToMemoryPool: peer=%d: accepted %s (poolsz %u txn, %u kB)\n",
                pfrom->id,
                tx.GetHash().ToString(),
                mempool.size(), mempool.DynamicMemoryUsage() / 1000);

            // Recursively process any orphan transactions that depended on this one
            set<NodeId> setMisbehaving;
            while (!vWorkQueue.empty()) {
                auto itByPrev = mapOrphanTransactionsByPrev.find(vWorkQueue.front());
                vWorkQueue.pop_front();
                if (itByPrev == mapOrphanTransactionsByPrev.end())
                    continue;
                for (auto mi = itByPrev->second.begin();
                     mi != itByPrev->second.end();
                     ++mi)
                {
                    const CTransaction& orphanTx = (*mi)->second.tx;
                    const uint256& orphanHash = orphanTx.GetHash();
                    NodeId fromPeer = (*mi)->second.fromPeer;
                    bool fMissingInputs2 = false;
                    // Use a dummy CValidationState so someone can't setup nodes to counter-DoS based on orphan
                    // resolution (that is, feeding people an invalid transaction based on LegitTxX in order to get
                    // anyone relaying LegitTxX banned)
                    CValidationState stateDummy;


                    if (setMisbehaving.count(fromPeer))
                        continue;
                    if (AcceptToMemoryPool(mempool, stateDummy, orphanTx, true, &fMissingInputs2)) {
                        LogPrint("mempool", "   accepted orphan tx %s\n", orphanHash.ToString());
                        RelayTransaction(orphanTx);
                        for (unsigned int i = 0; i < orphanTx.vout.size(); i++) {
                            vWorkQueue.emplace_back(orphanHash, i);
                        }
                        vEraseQueue.push_back(orphanHash);
                    }
                    else if (!fMissingInputs2)
                    {
                        int nDos = 0;
                        if (stateDummy.IsInvalid(nDos) && nDos > 0)
                        {
                            // Punish peer that gave us an invalid orphan tx
                            Misbehaving(fromPeer, nDos);
                            setMisbehaving.insert(fromPeer);
                            LogPrint("mempool", "   invalid orphan tx %s\n", orphanHash.ToString());
                        }
                        // Has inputs but not accepted to mempool
                        // Probably non-standard or insufficient fee/priority
                        LogPrint("mempool", "   removed orphan tx %s\n", orphanHash.ToString());
                        vEraseQueue.push_back(orphanHash);
                        if (orphanTx.wit.IsNull() && !stateDummy.CorruptionPossible()) {
                            // Do not use rejection cache for witness transactions or
                            // witness-stripped transactions, as they can have been malleated.
                            // See https://github.com/bitcoin/bitcoin/issues/8279 for details.
                            assert(recentRejects);
                            recentRejects->insert(orphanHash);
                        }
                    }
                    mempool.check(pcoinsTip);
                }
            }

            BOOST_FOREACH(uint256 hash, vEraseQueue)
                EraseOrphanTx(hash);
        }
        else if (fMissingInputs)
        {
            bool fRejectedParents = false; // It may be the case that the orphans parents have all been rejected
            BOOST_FOREACH(const CTxIn& txin, tx.vin) {
                if (recentRejects->contains(txin.prevout.hash)) {
                    fRejectedParents = true;
                    break;
                }
            }
            if (!fRejectedParents) {
                uint32_t nFetchFlags = GetFetchFlags(pfrom, chainActive.Tip(), chainparams.GetConsensus());
                BOOST_FOREACH(const CTxIn& txin, tx.vin) {
                    CInv _inv(MSG_TX | nFetchFlags, txin.prevout.hash);
                    pfrom->AddInventoryKnown(_inv);
                    if (!AlreadyHave(_inv)) pfrom->AskFor(_inv);
                }
                AddOrphanTx(tx, pfrom->GetId());

                // DoS prevention: do not allow mapOrphanTransactions to grow unbounded
                unsigned int nMaxOrphanTx = (unsigned int)std::max((int64_t)0, GetArg("-maxorphantx", DEFAULT_MAX_ORPHAN_TRANSACTIONS));
                unsigned int nEvicted = LimitOrphanTxSize(nMaxOrphanTx);
                if (nEvicted > 0)
                    LogPrint("mempool", "mapOrphan overflow, removed %u tx\n", nEvicted);
            } else {
                LogPrint("mempool", "not keeping orphan with rejected parents %s\n",tx.GetHash().ToString());
            }
        } else {
            if (tx.wit.IsNull() && !state.CorruptionPossible()) {
                // Do not use rejection cache for witness transactions or
                // witness-stripped transactions, as they can have been malleated.
                // See https://github.com/bitcoin/bitcoin/issues/8279 for details.
                assert(recentRejects);
                recentRejects->insert(tx.GetHash());
            }

            if (pfrom->fWhitelisted && GetBoolArg("-whitelistforcerelay", DEFAULT_WHITELISTFORCERELAY)) {
                // Always relay transactions received from whitelisted peers, even
                // if they were already in the mempool or rejected from it due
                // to policy, allowing the node to function as a gateway for
                // nodes hidden behind it.
                //
                // Never relay transactions that we would assign a non-zero DoS
                // score for, as we expect peers to do the same with us in that
                // case.
                int nDoS = 0;
                if (!state.IsInvalid(nDoS) || nDoS == 0) {
                    LogPrintf("Force relaying tx %s from whitelisted peer=%d\n", tx.GetHash().ToString(), pfrom->id);
                    RelayTransaction(tx);
                } else {
                    LogPrintf("Not relaying invalid transaction %s from whitelisted peer=%d (%s)\n", tx.GetHash().ToString(), pfrom->id, FormatStateMessage(state));
                }
            }
        }
        int nDoS = 0;
        if (state.IsInvalid(nDoS))
        {
            LogPrint("mempoolrej", "%s from peer=%d was not accepted: %s\n", tx.GetHash().ToString(),
                pfrom->id,
                FormatStateMessage(state));
            if (state.GetRejectCode() < REJECT_INTERNAL) // Never send AcceptToMemoryPool's internal codes over P2P
                pfrom->PushMessage(NetMsgType::REJECT, strCommand, (unsigned char)state.GetRejectCode(),
                                   state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), inv.hash);
            if (nDoS > 0) {
                Misbehaving(pfrom->GetId(), nDoS);
            }
        }
        FlushStateToDisk(state, FLUSH_STATE_PERIODIC);
    }


    else if (strCommand == NetMsgType::CMPCTBLOCK && !fImporting && !fReindex) // Ignore blocks received while importing
    {
        CBlockHeaderAndShortTxIDs cmpctblock;
        vRecv >> cmpctblock;

        // Keep a CBlock for "optimistic" compactblock reconstructions (see
        // below)
        CBlock block;
        bool fBlockReconstructed = false;

        LOCK(cs_main);

        if (mapBlockIndex.find(cmpctblock.header.hashPrevBlock) == mapBlockIndex.end()) {
            // Doesn't connect (or is genesis), instead of DoSing in AcceptBlockHeader, request deeper headers
            if (!IsInitialBlockDownload())
                pfrom->PushMessage(NetMsgType::GETHEADERS, chainActive.GetLocator(pindexBestHeader), uint256());
            return true;
        }

        CBlockIndex *pindex = NULL;
        CValidationState state;
        if (!AcceptBlockHeader(cmpctblock.header, state, chainparams, &pindex)) {
            int nDoS;
            if (state.IsInvalid(nDoS)) {
                if (nDoS > 0)
                    Misbehaving(pfrom->GetId(), nDoS);
                LogPrintf("Peer %d sent us invalid header via cmpctblock\n", pfrom->id);
                return true;
            }
        }

        // If AcceptBlockHeader returned true, it set pindex
        assert(pindex);
        UpdateBlockAvailability(pfrom->GetId(), pindex->GetBlockHash());

        std::map<uint256, pair<NodeId, list<QueuedBlock>::iterator> >::iterator blockInFlightIt = mapBlocksInFlight.find(pindex->GetBlockHash());
        bool fAlreadyInFlight = blockInFlightIt != mapBlocksInFlight.end();

        if (pindex->nStatus & BLOCK_HAVE_DATA) // Nothing to do here
            return true;

        if (pindex->nChainWork <= chainActive.Tip()->nChainWork || // We know something better
                pindex->nTx != 0) { // We had this block at some point, but pruned it
            if (fAlreadyInFlight) {
                // We requested this block for some reason, but our mempool will probably be useless
                // so we just grab the block via normal getdata
                std::vector<CInv> vInv(1);
                vInv[0] = CInv(MSG_BLOCK | GetFetchFlags(pfrom, pindex->pprev, chainparams.GetConsensus()), cmpctblock.header.GetHash());
                pfrom->PushMessage(NetMsgType::GETDATA, vInv);
            }
            return true;
        }

        // If we're not close to tip yet, give up and let parallel block fetch work its magic
        if (!fAlreadyInFlight && !CanDirectFetch(chainparams.GetConsensus()))
            return true;

        CNodeState *nodestate = State(pfrom->GetId());

        if (IsWitnessEnabled(pindex->pprev, chainparams.GetConsensus()) && !nodestate->fSupportsDesiredCmpctVersion) {
            // Don't bother trying to process compact blocks from v1 peers
            // after segwit activates.
            return true;
        }

        // We want to be a bit conservative just to be extra careful about DoS
        // possibilities in compact block processing...
        if (pindex->nHeight <= chainActive.Height() + 2) {
            if ((!fAlreadyInFlight && nodestate->nBlocksInFlight < MAX_BLOCKS_IN_TRANSIT_PER_PEER) ||
                 (fAlreadyInFlight && blockInFlightIt->second.first == pfrom->GetId())) {
                list<QueuedBlock>::iterator *queuedBlockIt = NULL;
                if (!MarkBlockAsInFlight(pfrom->GetId(), pindex->GetBlockHash(), chainparams.GetConsensus(), pindex, &queuedBlockIt)) {
                    if (!(*queuedBlockIt)->partialBlock)
                        (*queuedBlockIt)->partialBlock.reset(new PartiallyDownloadedBlock(&mempool));
                    else {
                        // The block was already in flight using compact blocks from the same peer
                        LogPrint("net", "Peer sent us compact block we were already syncing!\n");
                        return true;
                    }
                }

                PartiallyDownloadedBlock& partialBlock = *(*queuedBlockIt)->partialBlock;
                ReadStatus status = partialBlock.InitData(cmpctblock);
                if (status == READ_STATUS_INVALID) {
                    MarkBlockAsReceived(pindex->GetBlockHash()); // Reset in-flight state in case of whitelist
                    Misbehaving(pfrom->GetId(), 100);
                    LogPrintf("Peer %d sent us invalid compact block\n", pfrom->id);
                    return true;
                } else if (status == READ_STATUS_FAILED) {
                    // Duplicate txindexes, the block is now in-flight, so just request it
                    std::vector<CInv> vInv(1);
                    vInv[0] = CInv(MSG_BLOCK | GetFetchFlags(pfrom, pindex->pprev, chainparams.GetConsensus()), cmpctblock.header.GetHash());
                    pfrom->PushMessage(NetMsgType::GETDATA, vInv);
                    return true;
                }

                if (!fAlreadyInFlight && mapBlocksInFlight.size() == 1 && pindex->pprev->IsValid(BLOCK_VALID_CHAIN)) {
                    // We seem to be rather well-synced, so it appears pfrom was the first to provide us
                    // with this block! Let's get them to announce using compact blocks in the future.
                    MaybeSetPeerAsAnnouncingHeaderAndIDs(nodestate, pfrom);
                }

                BlockTransactionsRequest req;
                for (size_t i = 0; i < cmpctblock.BlockTxCount(); i++) {
                    if (!partialBlock.IsTxAvailable(i))
                        req.indexes.push_back(i);
                }
                if (req.indexes.empty()) {
                    // Dirty hack to jump to BLOCKTXN code (TODO: move message handling into their own functions)
                    BlockTransactions txn;
                    txn.blockhash = cmpctblock.header.GetHash();
                    CDataStream blockTxnMsg(SER_NETWORK, PROTOCOL_VERSION);
                    blockTxnMsg << txn;
                    return ProcessMessage(pfrom, NetMsgType::BLOCKTXN, blockTxnMsg, nTimeReceived, chainparams);
                } else {
                    req.blockhash = pindex->GetBlockHash();
                    pfrom->PushMessage(NetMsgType::GETBLOCKTXN, req);
                }
            } else {
                // This block is either already in flight from a different
                // peer, or this peer has too many blocks outstanding to
                // download from.
                // Optimistically try to reconstruct anyway since we might be
                // able to without any round trips.
                PartiallyDownloadedBlock tempBlock(&mempool);
                ReadStatus status = tempBlock.InitData(cmpctblock);
                if (status != READ_STATUS_OK) {
                    // TODO: don't ignore failures
                    return true;
                }
                std::vector<CTransaction> dummy;
                status = tempBlock.FillBlock(block, dummy);
                if (status == READ_STATUS_OK) {
                    fBlockReconstructed = true;
                }
            }
        } else {
            if (fAlreadyInFlight) {
                // We requested this block, but its far into the future, so our
                // mempool will probably be useless - request the block normally
                std::vector<CInv> vInv(1);
                vInv[0] = CInv(MSG_BLOCK | GetFetchFlags(pfrom, pindex->pprev, chainparams.GetConsensus()), cmpctblock.header.GetHash());
                pfrom->PushMessage(NetMsgType::GETDATA, vInv);
                return true;
            } else {
                // If this was an announce-cmpctblock, we want the same treatment as a header message
                // Dirty hack to process as if it were just a headers message (TODO: move message handling into their own functions)
                std::vector<CBlock> headers;
                headers.push_back(cmpctblock.header);
                CDataStream vHeadersMsg(SER_NETWORK, PROTOCOL_VERSION);
                vHeadersMsg << headers;
                return ProcessMessage(pfrom, NetMsgType::HEADERS, vHeadersMsg, nTimeReceived, chainparams);
            }
        }

        if (fBlockReconstructed) {
            // If we got here, we were able to optimistically reconstruct a
            // block that is in flight from some other peer.  However, this
            // cmpctblock may be invalid.  In particular, while we've checked
            // that the block merkle root commits to the transaction ids, we
            // haven't yet checked that tx witnesses are properly committed to
            // in the coinbase witness commitment.
            //
            // ProcessNewBlock will call MarkBlockAsReceived(), which will
            // clear any in-flight compact block state that might be present
            // from some other peer.  We don't want a malleated compact block
            // request to interfere with block relay, so we don't want to call
            // ProcessNewBlock until we've already checked that the witness
            // commitment is correct.
            {
                LOCK(cs_main);
                CValidationState dummy;
                if (!ContextualCheckBlock(block, dummy, pindex->pprev)) {
                    // TODO: could send reject message to peer?
                    return true;
                }
            }
            CValidationState state;
            ProcessNewBlock(state, chainparams, pfrom, &block, true, NULL, false);
            // TODO: could send reject message if block is invalid?
        }

        CheckBlockIndex(chainparams.GetConsensus());
    }

    else if (strCommand == NetMsgType::BLOCKTXN && !fImporting && !fReindex) // Ignore blocks received while importing
    {
        BlockTransactions resp;
        vRecv >> resp;

        LOCK(cs_main);

        map<uint256, pair<NodeId, list<QueuedBlock>::iterator> >::iterator it = mapBlocksInFlight.find(resp.blockhash);
        if (it == mapBlocksInFlight.end() || !it->second.second->partialBlock ||
                it->second.first != pfrom->GetId()) {
            LogPrint("net", "Peer %d sent us block transactions for block we weren't expecting\n", pfrom->id);
            return true;
        }

        PartiallyDownloadedBlock& partialBlock = *it->second.second->partialBlock;
        CBlock block;
        ReadStatus status = partialBlock.FillBlock(block, resp.txn);
        if (status == READ_STATUS_INVALID) {
            MarkBlockAsReceived(resp.blockhash); // Reset in-flight state in case of whitelist
            Misbehaving(pfrom->GetId(), 100);
            LogPrintf("Peer %d sent us invalid compact block/non-matching block transactions\n", pfrom->id);
            return true;
        } else if (status == READ_STATUS_FAILED) {
            // Might have collided, fall back to getdata now :(
            std::vector<CInv> invs;
            invs.push_back(CInv(MSG_BLOCK | GetFetchFlags(pfrom, chainActive.Tip(), chainparams.GetConsensus()), resp.blockhash));
            pfrom->PushMessage(NetMsgType::GETDATA, invs);
        } else {
            // Block is either okay, or possibly we received
            // READ_STATUS_CHECKBLOCK_FAILED.
            // Note that CheckBlock can only fail for one of a few reasons:
            // 1. bad-proof-of-work (impossible here, because we've already
            //    accepted the header)
            // 2. merkleroot doesn't match the transactions given (already
            //    caught in FillBlock with READ_STATUS_FAILED, so
            //    impossible here)
            // 3. the block is otherwise invalid (eg invalid coinbase,
            //    block is too big, too many legacy sigops, etc).
            // So if CheckBlock failed, #3 is the only possibility.
            // Under BIP 152, we don't DoS-ban unless proof of work is
            // invalid (we don't require all the stateless checks to have
            // been run).  This is handled below, so just treat this as
            // though the block was successfully read, and rely on the
            // handling in ProcessNewBlock to ensure the block index is
            // updated, reject messages go out, etc.
            CValidationState state;
            // BIP 152 permits peers to relay compact blocks after validating
            // the header only; we should not punish peers if the block turns
            // out to be invalid.
            ProcessNewBlock(state, chainparams, pfrom, &block, false, NULL, false);
            int nDoS;
            if (state.IsInvalid(nDoS)) {
                assert (state.GetRejectCode() < REJECT_INTERNAL); // Blocks are never rejected with internal reject codes
                pfrom->PushMessage(NetMsgType::REJECT, strCommand, (unsigned char)state.GetRejectCode(),
                                   state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), block.GetHash());
            }
        }
    }


    else if (strCommand == NetMsgType::HEADERS && !fImporting && !fReindex) // Ignore headers received while importing
    {
        std::vector<CBlockHeader> headers;

        // Bypass the normal CBlock deserialization, as we don't want to risk deserializing 2000 full blocks.
        unsigned int nCount = ReadCompactSize(vRecv);
        if (nCount > MAX_HEADERS_RESULTS) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 20);
            return error("headers message size = %u", nCount);
        }
        headers.resize(nCount);
        for (unsigned int n = 0; n < nCount; n++) {
            vRecv >> headers[n];
            ReadCompactSize(vRecv); // ignore tx count; assume it is 0.
        }

        {
        LOCK(cs_main);

        if (nCount == 0) {
            // Nothing interesting. Stop asking this peers for more headers.
            return true;
        }

        CNodeState *nodestate = State(pfrom->GetId());

        // If this looks like it could be a block announcement (nCount <
        // MAX_BLOCKS_TO_ANNOUNCE), use special logic for handling headers that
        // don't connect:
        // - Send a getheaders message in response to try to connect the chain.
        // - The peer can send up to MAX_UNCONNECTING_HEADERS in a row that
        //   don't connect before giving DoS points
        // - Once a headers message is received that is valid and does connect,
        //   nUnconnectingHeaders gets reset back to 0.
        if (mapBlockIndex.find(headers[0].hashPrevBlock) == mapBlockIndex.end() && nCount < MAX_BLOCKS_TO_ANNOUNCE) {
            nodestate->nUnconnectingHeaders++;
            pfrom->PushMessage(NetMsgType::GETHEADERS, chainActive.GetLocator(pindexBestHeader), uint256());
            LogPrint("net", "received header %s: missing prev block %s, sending getheaders (%d) to end (peer=%d, nUnconnectingHeaders=%d)\n",
                    headers[0].GetHash().ToString(),
                    headers[0].hashPrevBlock.ToString(),
                    pindexBestHeader->nHeight,
                    pfrom->id, nodestate->nUnconnectingHeaders);
            // Set hashLastUnknownBlock for this peer, so that if we
            // eventually get the headers - even from a different peer -
            // we can use this peer to download.
            UpdateBlockAvailability(pfrom->GetId(), headers.back().GetHash());

            if (nodestate->nUnconnectingHeaders % MAX_UNCONNECTING_HEADERS == 0) {
                Misbehaving(pfrom->GetId(), 20);
            }
            return true;
        }

        CBlockIndex *pindexLast = NULL;
        BOOST_FOREACH(const CBlockHeader& header, headers) {
            CValidationState state;
            if (pindexLast != NULL && header.hashPrevBlock != pindexLast->GetBlockHash()) {
                Misbehaving(pfrom->GetId(), 20);
                return error("non-continuous headers sequence");
            }
            if (!AcceptBlockHeader(header, state, chainparams, &pindexLast)) {
                int nDoS;
                if (state.IsInvalid(nDoS)) {
                    if (nDoS > 0)
                        Misbehaving(pfrom->GetId(), nDoS);
                    return error("invalid header received");
                }
            }
        }

        if (nodestate->nUnconnectingHeaders > 0) {
            LogPrint("net", "peer=%d: resetting nUnconnectingHeaders (%d -> 0)\n", pfrom->id, nodestate->nUnconnectingHeaders);
        }
        nodestate->nUnconnectingHeaders = 0;

        assert(pindexLast);
        UpdateBlockAvailability(pfrom->GetId(), pindexLast->GetBlockHash());

        if (nCount == MAX_HEADERS_RESULTS) {
            // Headers message had its maximum size; the peer may have more headers.
            // TODO: optimize: if pindexLast is an ancestor of chainActive.Tip or pindexBestHeader, continue
            // from there instead.
            LogPrint("net", "more getheaders (%d) to end to peer=%d (startheight:%d)\n", pindexLast->nHeight, pfrom->id, pfrom->nStartingHeight);
            pfrom->PushMessage(NetMsgType::GETHEADERS, chainActive.GetLocator(pindexLast), uint256());
        }

        bool fCanDirectFetch = CanDirectFetch(chainparams.GetConsensus());
        // If this set of headers is valid and ends in a block with at least as
        // much work as our tip, download as much as possible.
        if (fCanDirectFetch && pindexLast->IsValid(BLOCK_VALID_TREE) && chainActive.Tip()->nChainWork <= pindexLast->nChainWork) {
            vector<CBlockIndex *> vToFetch;
            CBlockIndex *pindexWalk = pindexLast;
            // Calculate all the blocks we'd need to switch to pindexLast, up to a limit.
            while (pindexWalk && !chainActive.Contains(pindexWalk) && vToFetch.size() <= MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
                if (!(pindexWalk->nStatus & BLOCK_HAVE_DATA) &&
                        !mapBlocksInFlight.count(pindexWalk->GetBlockHash()) &&
                        (!IsWitnessEnabled(pindexWalk->pprev, chainparams.GetConsensus()) || State(pfrom->GetId())->fHaveWitness)) {
                    // We don't have this block, and it's not yet in flight.
                    vToFetch.push_back(pindexWalk);
                }
                pindexWalk = pindexWalk->pprev;
            }
            // If pindexWalk still isn't on our main chain, we're looking at a
            // very large reorg at a time we think we're close to caught up to
            // the main chain -- this shouldn't really happen.  Bail out on the
            // direct fetch and rely on parallel download instead.
            if (!chainActive.Contains(pindexWalk)) {
                LogPrint("net", "Large reorg, won't direct fetch to %s (%d)\n",
                        pindexLast->GetBlockHash().ToString(),
                        pindexLast->nHeight);
            } else {
                vector<CInv> vGetData;
                // Download as much as possible, from earliest to latest.
                BOOST_REVERSE_FOREACH(CBlockIndex *pindex, vToFetch) {
                    if (nodestate->nBlocksInFlight >= MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
                        // Can't download any more from this peer
                        break;
                    }
                    uint32_t nFetchFlags = GetFetchFlags(pfrom, pindex->pprev, chainparams.GetConsensus());
                    vGetData.push_back(CInv(MSG_BLOCK | nFetchFlags, pindex->GetBlockHash()));
                    MarkBlockAsInFlight(pfrom->GetId(), pindex->GetBlockHash(), chainparams.GetConsensus(), pindex);
                    LogPrint("net", "Requesting block %s from  peer=%d\n",
                            pindex->GetBlockHash().ToString(), pfrom->id);
                }
                if (vGetData.size() > 1) {
                    LogPrint("net", "Downloading blocks toward %s (%d) via headers direct fetch\n",
                            pindexLast->GetBlockHash().ToString(), pindexLast->nHeight);
                }
                if (vGetData.size() > 0) {
                    if (nodestate->fSupportsDesiredCmpctVersion && vGetData.size() == 1 && mapBlocksInFlight.size() == 1 && pindexLast->pprev->IsValid(BLOCK_VALID_CHAIN)) {
                        // We seem to be rather well-synced, so it appears pfrom was the first to provide us
                        // with this block! Let's get them to announce using compact blocks in the future.
                        MaybeSetPeerAsAnnouncingHeaderAndIDs(nodestate, pfrom);
                        // In any case, we want to download using a compact block, not a regular one
                        vGetData[0] = CInv(MSG_CMPCT_BLOCK, vGetData[0].hash);
                    }
                    pfrom->PushMessage(NetMsgType::GETDATA, vGetData);
                }
            }
        }

        CheckBlockIndex(chainparams.GetConsensus());
        }

        NotifyHeaderTip();
    }

    else if (strCommand == NetMsgType::BLOCK && !fImporting && !fReindex) // Ignore blocks received while importing
    {
        CBlock block;
        vRecv >> block;

        LogPrint("net", "received block %s peer=%d\n", block.GetHash().ToString(), pfrom->id);

        CValidationState state;
        // Process all blocks from whitelisted peers, even if not requested,
        // unless we're still syncing with the network.
        // Such an unrequested block may still be processed, subject to the
        // conditions in AcceptBlock().
        bool forceProcessing = pfrom->fWhitelisted && !IsInitialBlockDownload();
        ProcessNewBlock(state, chainparams, pfrom, &block, forceProcessing, NULL, true);
        int nDoS;
        if (state.IsInvalid(nDoS)) {
            assert (state.GetRejectCode() < REJECT_INTERNAL); // Blocks are never rejected with internal reject codes
            pfrom->PushMessage(NetMsgType::REJECT, strCommand, (unsigned char)state.GetRejectCode(),
                               state.GetRejectReason().substr(0, MAX_REJECT_MESSAGE_LENGTH), block.GetHash());
            if (nDoS > 0) {
                LOCK(cs_main);
                Misbehaving(pfrom->GetId(), nDoS);
            }
        }

    }


    else if (strCommand == NetMsgType::GETADDR)
    {
        // This asymmetric behavior for inbound and outbound connections was introduced
        // to prevent a fingerprinting attack: an attacker can send specific fake addresses
        // to users' AddrMan and later request them by sending getaddr messages.
        // Making nodes which are behind NAT and can only make outgoing connections ignore
        // the getaddr message mitigates the attack.
        if (!pfrom->fInbound) {
            LogPrint("net", "Ignoring \"getaddr\" from outbound connection. peer=%d\n", pfrom->id);
            return true;
        }

        // Only send one GetAddr response per connection to reduce resource waste
        //  and discourage addr stamping of INV announcements.
        if (pfrom->fSentAddr) {
            LogPrint("net", "Ignoring repeated \"getaddr\". peer=%d\n", pfrom->id);
            return true;
        }
        pfrom->fSentAddr = true;

        pfrom->vAddrToSend.clear();
        vector<CAddress> vAddr = addrman.GetAddr();
        BOOST_FOREACH(const CAddress &addr, vAddr)
            pfrom->PushAddress(addr);
    }


    else if (strCommand == NetMsgType::MEMPOOL)
    {
        if (!(nLocalServices & NODE_BLOOM) && !pfrom->fWhitelisted)
        {
            LogPrint("net", "mempool request with bloom filters disabled, disconnect peer=%d\n", pfrom->GetId());
            pfrom->fDisconnect = true;
            return true;
        }

        if (CNode::OutboundTargetReached(false) && !pfrom->fWhitelisted)
        {
            LogPrint("net", "mempool request with bandwidth limit reached, disconnect peer=%d\n", pfrom->GetId());
            pfrom->fDisconnect = true;
            return true;
        }

        LOCK(pfrom->cs_inventory);
        pfrom->fSendMempool = true;
    }


    else if (strCommand == NetMsgType::PING)
    {
        if (pfrom->nVersion > BIP0031_VERSION)
        {
            uint64_t nonce = 0;
            vRecv >> nonce;
            // Echo the message back with the nonce. This allows for two useful features:
            //
            // 1) A remote node can quickly check if the connection is operational
            // 2) Remote nodes can measure the latency of the network thread. If this node
            //    is overloaded it won't respond to pings quickly and the remote node can
            //    avoid sending us more work, like chain download requests.
            //
            // The nonce stops the remote getting confused between different pings: without
            // it, if the remote node sends a ping once per second and this node takes 5
            // seconds to respond to each, the 5th ping the remote sends would appear to
            // return very quickly.
            pfrom->PushMessage(NetMsgType::PONG, nonce);
        }
    }


    else if (strCommand == NetMsgType::PONG)
    {
        int64_t pingUsecEnd = nTimeReceived;
        uint64_t nonce = 0;
        size_t nAvail = vRecv.in_avail();
        bool bPingFinished = false;
        std::string sProblem;

        if (nAvail >= sizeof(nonce)) {
            vRecv >> nonce;

            // Only process pong message if there is an outstanding ping (old ping without nonce should never pong)
            if (pfrom->nPingNonceSent != 0) {
                if (nonce == pfrom->nPingNonceSent) {
                    // Matching pong received, this ping is no longer outstanding
                    bPingFinished = true;
                    int64_t pingUsecTime = pingUsecEnd - pfrom->nPingUsecStart;
                    if (pingUsecTime > 0) {
                        // Successful ping time measurement, replace previous
                        pfrom->nPingUsecTime = pingUsecTime;
                        pfrom->nMinPingUsecTime = std::min(pfrom->nMinPingUsecTime, pingUsecTime);
                    } else {
                        // This should never happen
                        sProblem = "Timing mishap";
                    }
                } else {
                    // Nonce mismatches are normal when pings are overlapping
                    sProblem = "Nonce mismatch";
                    if (nonce == 0) {
                        // This is most likely a bug in another implementation somewhere; cancel this ping
                        bPingFinished = true;
                        sProblem = "Nonce zero";
                    }
                }
            } else {
                sProblem = "Unsolicited pong without ping";
            }
        } else {
            // This is most likely a bug in another implementation somewhere; cancel this ping
            bPingFinished = true;
            sProblem = "Short payload";
        }

        if (!(sProblem.empty())) {
            LogPrint("net", "pong peer=%d: %s, %x expected, %x received, %u bytes\n",
                pfrom->id,
                sProblem,
                pfrom->nPingNonceSent,
                nonce,
                nAvail);
        }
        if (bPingFinished) {
            pfrom->nPingNonceSent = 0;
        }
    }


    else if (strCommand == NetMsgType::FILTERLOAD)
    {
        CBloomFilter filter;
        vRecv >> filter;

        if (!filter.IsWithinSizeConstraints())
        {
            // There is no excuse for sending a too-large filter
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 100);
        }
        else
        {
            LOCK(pfrom->cs_filter);
            delete pfrom->pfilter;
            pfrom->pfilter = new CBloomFilter(filter);
            pfrom->pfilter->UpdateEmptyFull();
            pfrom->fRelayTxes = true;
        }
    }


    else if (strCommand == NetMsgType::FILTERADD)
    {
        vector<unsigned char> vData;
        vRecv >> vData;

        // Nodes must NEVER send a data item > 520 bytes (the max size for a script data object,
        // and thus, the maximum size any matched object can have) in a filteradd message
        bool bad = false;
        if (vData.size() > MAX_SCRIPT_ELEMENT_SIZE) {
            bad = true;
        } else {
            LOCK(pfrom->cs_filter);
            if (pfrom->pfilter) {
                pfrom->pfilter->insert(vData);
            } else {
                bad = true;
            }
        }
        if (bad) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 100);
        }
    }


    else if (strCommand == NetMsgType::FILTERCLEAR)
    {
        LOCK(pfrom->cs_filter);
        delete pfrom->pfilter;
        pfrom->pfilter = new CBloomFilter();
        pfrom->fRelayTxes = true;
    }


    else if (strCommand == NetMsgType::REJECT)
    {
        if (fDebug) {
            try {
                string strMsg; unsigned char ccode; string strReason;
                vRecv >> LIMITED_STRING(strMsg, CMessageHeader::COMMAND_SIZE) >> ccode >> LIMITED_STRING(strReason, MAX_REJECT_MESSAGE_LENGTH);

                ostringstream ss;
                ss << strMsg << " code " << itostr(ccode) << ": " << strReason;

                if (strMsg == NetMsgType::BLOCK || strMsg == NetMsgType::TX)
                {
                    uint256 hash;
                    vRecv >> hash;
                    ss << ": hash " << hash.ToString();
                }
                LogPrint("net", "Reject %s\n", SanitizeString(ss.str()));
            } catch (const std::ios_base::failure&) {
                // Avoid feedback loops by preventing reject messages from triggering a new reject message.
                LogPrint("net", "Unparseable reject message received\n");
            }
        }
    }

    else if (strCommand == NetMsgType::FEEFILTER) {
        CAmount newFeeFilter = 0;
        vRecv >> newFeeFilter;
        if (MoneyRange(newFeeFilter)) {
            {
                LOCK(pfrom->cs_feeFilter);
                pfrom->minFeeFilter = newFeeFilter;
            }
            LogPrint("net", "received: feefilter of %s from peer=%d\n", CFeeRate(newFeeFilter).ToString(), pfrom->id);
        }
    }

    else if (strCommand == NetMsgType::NOTFOUND) {
        // We do not care about the NOTFOUND message, but logging an Unknown Command
        // message would be undesirable as we transmit it ourselves.
    }

    else {
        // Ignore unknown commands for extensibility
        LogPrint("net", "Unknown command \"%s\" from peer=%d\n", SanitizeString(strCommand), pfrom->id);
    }



    return true;
}

// requires LOCK(cs_vRecvMsg)
bool ProcessMessages(CNode* pfrom)
{
    const CChainParams& chainparams = Params();
    //if (fDebug)
    //    LogPrintf("%s(%u messages)\n", __func__, pfrom->vRecvMsg.size());

    //
    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (4) checksum
    //  (x) data
    //
    bool fOk = true;

    if (!pfrom->vRecvGetData.empty())
        ProcessGetData(pfrom, chainparams.GetConsensus());

    // this maintains the order of responses
    if (!pfrom->vRecvGetData.empty()) return fOk;

    std::deque<CNetMessage>::iterator it = pfrom->vRecvMsg.begin();
    while (!pfrom->fDisconnect && it != pfrom->vRecvMsg.end()) {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        // get next message
        CNetMessage& msg = *it;

        //if (fDebug)
        //    LogPrintf("%s(message %u msgsz, %u bytes, complete:%s)\n", __func__,
        //            msg.hdr.nMessageSize, msg.vRecv.size(),
        //            msg.complete() ? "Y" : "N");

        // end, if an incomplete message is found
        if (!msg.complete())
            break;

        // at this point, any failure means we can delete the current message
        it++;

        // Scan for message start
        if (memcmp(msg.hdr.pchMessageStart, chainparams.MessageStart(), MESSAGE_START_SIZE) != 0) {
            LogPrintf("PROCESSMESSAGE: INVALID MESSAGESTART %s peer=%d\n", SanitizeString(msg.hdr.GetCommand()), pfrom->id);
            fOk = false;
            break;
        }

        // Read header
        CMessageHeader& hdr = msg.hdr;
        if (!hdr.IsValid(chainparams.MessageStart()))
        {
            LogPrintf("PROCESSMESSAGE: ERRORS IN HEADER %s peer=%d\n", SanitizeString(hdr.GetCommand()), pfrom->id);
            continue;
        }
        string strCommand = hdr.GetCommand();

        // Message size
        unsigned int nMessageSize = hdr.nMessageSize;

        // Checksum
        CDataStream& vRecv = msg.vRecv;
        uint256 hash = Hash(vRecv.begin(), vRecv.begin() + nMessageSize);
        unsigned int nChecksum = ReadLE32((unsigned char*)&hash);
        if (nChecksum != hdr.nChecksum)
        {
            LogPrintf("%s(%s, %u bytes): CHECKSUM ERROR nChecksum=%08x hdr.nChecksum=%08x\n", __func__,
               SanitizeString(strCommand), nMessageSize, nChecksum, hdr.nChecksum);
            continue;
        }

        // Process message
        bool fRet = false;
        try
        {
            fRet = ProcessMessage(pfrom, strCommand, vRecv, msg.nTime, chainparams);
            boost::this_thread::interruption_point();
        }
        catch (const std::ios_base::failure& e)
        {
            pfrom->PushMessage(NetMsgType::REJECT, strCommand, REJECT_MALFORMED, string("error parsing message"));
            if (strstr(e.what(), "end of data"))
            {
                // Allow exceptions from under-length message on vRecv
                LogPrintf("%s(%s, %u bytes): Exception '%s' caught, normally caused by a message being shorter than its stated length\n", __func__, SanitizeString(strCommand), nMessageSize, e.what());
            }
            else if (strstr(e.what(), "size too large"))
            {
                // Allow exceptions from over-long size
                LogPrintf("%s(%s, %u bytes): Exception '%s' caught\n", __func__, SanitizeString(strCommand), nMessageSize, e.what());
            }
            else if (strstr(e.what(), "non-canonical ReadCompactSize()"))
            {
                // Allow exceptions from non-canonical encoding
                LogPrintf("%s(%s, %u bytes): Exception '%s' caught\n", __func__, SanitizeString(strCommand), nMessageSize, e.what());
            }
            else
            {
                PrintExceptionContinue(&e, "ProcessMessages()");
            }
        }
        catch (const boost::thread_interrupted&) {
            throw;
        }
        catch (const std::exception& e) {
            PrintExceptionContinue(&e, "ProcessMessages()");
        } catch (...) {
            PrintExceptionContinue(NULL, "ProcessMessages()");
        }

        if (!fRet)
            LogPrintf("%s(%s, %u bytes) FAILED peer=%d\n", __func__, SanitizeString(strCommand), nMessageSize, pfrom->id);

        break;
    }

    // In case the connection got shut down, its receive buffer was wiped
    if (!pfrom->fDisconnect)
        pfrom->vRecvMsg.erase(pfrom->vRecvMsg.begin(), it);

    return fOk;
}

class CompareInvMempoolOrder
{
    CTxMemPool *mp;
public:
    CompareInvMempoolOrder(CTxMemPool *mempool)
    {
        mp = mempool;
    }

    bool operator()(std::set<uint256>::iterator a, std::set<uint256>::iterator b)
    {
        /* As std::make_heap produces a max-heap, we want the entries with the
         * fewest ancestors/highest fee to sort later. */
        return mp->CompareDepthAndScore(*b, *a);
    }
};

bool SendMessages(CNode* pto)
{
    const Consensus::Params& consensusParams = Params().GetConsensus();
    {
        // Don't send anything until we get its version message
        if (pto->nVersion == 0)
            return true;

        //
        // Message: ping
        //
        bool pingSend = false;
        if (pto->fPingQueued) {
            // RPC ping request by user
            pingSend = true;
        }
        if (pto->nPingNonceSent == 0 && pto->nPingUsecStart + PING_INTERVAL * 1000000 < GetTimeMicros()) {
            // Ping automatically sent as a latency probe & keepalive.
            pingSend = true;
        }
        if (pingSend && !pto->fDisconnect) {
            uint64_t nonce = 0;
            while (nonce == 0) {
                GetRandBytes((unsigned char*)&nonce, sizeof(nonce));
            }
            pto->fPingQueued = false;
            pto->nPingUsecStart = GetTimeMicros();
            if (pto->nVersion > BIP0031_VERSION) {
                pto->nPingNonceSent = nonce;
                pto->PushMessage(NetMsgType::PING, nonce);
            } else {
                // Peer is too old to support ping command with nonce, pong will never arrive.
                pto->nPingNonceSent = 0;
                pto->PushMessage(NetMsgType::PING);
            }
        }

        TRY_LOCK(cs_main, lockMain); // Acquire cs_main for IsInitialBlockDownload() and CNodeState()
        if (!lockMain)
            return true;

        // Address refresh broadcast
        int64_t nNow = GetTimeMicros();
        if (!IsInitialBlockDownload() && pto->nNextLocalAddrSend < nNow) {
            AdvertiseLocal(pto);
            pto->nNextLocalAddrSend = PoissonNextSend(nNow, AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL);
        }

        //
        // Message: addr
        //
        if (pto->nNextAddrSend < nNow) {
            pto->nNextAddrSend = PoissonNextSend(nNow, AVG_ADDRESS_BROADCAST_INTERVAL);
            vector<CAddress> vAddr;
            vAddr.reserve(pto->vAddrToSend.size());
            BOOST_FOREACH(const CAddress& addr, pto->vAddrToSend)
            {
                if (!pto->addrKnown.contains(addr.GetKey()))
                {
                    pto->addrKnown.insert(addr.GetKey());
                    vAddr.push_back(addr);
                    // receiver rejects addr messages larger than 1000
                    if (vAddr.size() >= 1000)
                    {
                        pto->PushMessage(NetMsgType::ADDR, vAddr);
                        vAddr.clear();
                    }
                }
            }
            pto->vAddrToSend.clear();
            if (!vAddr.empty())
                pto->PushMessage(NetMsgType::ADDR, vAddr);
            // we only send the big addr message once
            if (pto->vAddrToSend.capacity() > 40)
                pto->vAddrToSend.shrink_to_fit();
        }

        CNodeState &state = *State(pto->GetId());
        if (state.fShouldBan) {
            if (pto->fWhitelisted)
                LogPrintf("Warning: not punishing whitelisted peer %s!\n", pto->addr.ToString());
            else {
                pto->fDisconnect = true;
                if (pto->addr.IsLocal())
                    LogPrintf("Warning: not banning local peer %s!\n", pto->addr.ToString());
                else
                {
                    CNode::Ban(pto->addr, BanReasonNodeMisbehaving);
                }
            }
            state.fShouldBan = false;
        }

        BOOST_FOREACH(const CBlockReject& reject, state.rejects)
            pto->PushMessage(NetMsgType::REJECT, (string)NetMsgType::BLOCK, reject.chRejectCode, reject.strRejectReason, reject.hashBlock);
        state.rejects.clear();

        // Start block sync
        if (pindexBestHeader == NULL)
            pindexBestHeader = chainActive.Tip();
        bool fFetch = state.fPreferredDownload || (nPreferredDownload == 0 && !pto->fClient && !pto->fOneShot); // Download if this is a nice peer, or we have no nice peers and this one might do.
        if (!state.fSyncStarted && !pto->fClient && !pto->fDisconnect && !fImporting && !fReindex) {
            // Only actively request headers from a single peer, unless we're close to today.
            if ((nSyncStarted == 0 && fFetch) || pindexBestHeader->GetBlockTime() > GetAdjustedTime() - nMaxTipAge) {
                state.fSyncStarted = true;
                nSyncStarted++;
                const CBlockIndex *pindexStart = pindexBestHeader;
                /* If possible, start at the block preceding the currently
                   best known header.  This ensures that we always get a
                   non-empty list of headers back as long as the peer
                   is up-to-date.  With a non-empty response, we can initialise
                   the peer's known best block.  This wouldn't be possible
                   if we requested starting at pindexBestHeader and
                   got back an empty response.  */
                if (pindexStart->pprev)
                    pindexStart = pindexStart->pprev;
                LogPrint("net", "initial getheaders (%d) to peer=%d (startheight:%d)\n", pindexStart->nHeight, pto->id, pto->nStartingHeight);
                pto->PushMessage(NetMsgType::GETHEADERS, chainActive.GetLocator(pindexStart), uint256());
            }
        }

        // Resend wallet transactions that haven't gotten in a block yet
        // Except during reindex, importing and IBD, when old wallet
        // transactions become unconfirmed and spams other nodes.
        if (!fReindex && !fImporting && !IsInitialBlockDownload())
        {
            GetMainSignals().Broadcast(nTimeBestReceived);
        }

        //
        // Try sending block announcements via headers
        //
        {
            // If we have less than MAX_BLOCKS_TO_ANNOUNCE in our
            // list of block hashes we're relaying, and our peer wants
            // headers announcements, then find the first header
            // not yet known to our peer but would connect, and send.
            // If no header would connect, or if we have too many
            // blocks, or if the peer doesn't want headers, just
            // add all to the inv queue.
            LOCK(pto->cs_inventory);
            vector<CBlock> vHeaders;
            bool fRevertToInv = ((!state.fPreferHeaders &&
                                 (!state.fPreferHeaderAndIDs || pto->vBlockHashesToAnnounce.size() > 1)) ||
                                pto->vBlockHashesToAnnounce.size() > MAX_BLOCKS_TO_ANNOUNCE);
            CBlockIndex *pBestIndex = NULL; // last header queued for delivery
            ProcessBlockAvailability(pto->id); // ensure pindexBestKnownBlock is up-to-date

            if (!fRevertToInv) {
                bool fFoundStartingHeader = false;
                // Try to find first header that our peer doesn't have, and
                // then send all headers past that one.  If we come across any
                // headers that aren't on chainActive, give up.
                BOOST_FOREACH(const uint256 &hash, pto->vBlockHashesToAnnounce) {
                    BlockMap::iterator mi = mapBlockIndex.find(hash);
                    assert(mi != mapBlockIndex.end());
                    CBlockIndex *pindex = mi->second;
                    if (chainActive[pindex->nHeight] != pindex) {
                        // Bail out if we reorged away from this block
                        fRevertToInv = true;
                        break;
                    }
                    if (pBestIndex != NULL && pindex->pprev != pBestIndex) {
                        // This means that the list of blocks to announce don't
                        // connect to each other.
                        // This shouldn't really be possible to hit during
                        // regular operation (because reorgs should take us to
                        // a chain that has some block not on the prior chain,
                        // which should be caught by the prior check), but one
                        // way this could happen is by using invalidateblock /
                        // reconsiderblock repeatedly on the tip, causing it to
                        // be added multiple times to vBlockHashesToAnnounce.
                        // Robustly deal with this rare situation by reverting
                        // to an inv.
                        fRevertToInv = true;
                        break;
                    }
                    pBestIndex = pindex;
                    if (fFoundStartingHeader) {
                        // add this to the headers message
                        vHeaders.push_back(pindex->GetBlockHeader());
                    } else if (PeerHasHeader(&state, pindex)) {
                        continue; // keep looking for the first new block
                    } else if (pindex->pprev == NULL || PeerHasHeader(&state, pindex->pprev)) {
                        // Peer doesn't have this header but they do have the prior one.
                        // Start sending headers.
                        fFoundStartingHeader = true;
                        vHeaders.push_back(pindex->GetBlockHeader());
                    } else {
                        // Peer doesn't have this header or the prior one -- nothing will
                        // connect, so bail out.
                        fRevertToInv = true;
                        break;
                    }
                }
            }
            if (!fRevertToInv && !vHeaders.empty()) {
                if (vHeaders.size() == 1 && state.fPreferHeaderAndIDs) {
                    // We only send up to 1 block as header-and-ids, as otherwise
                    // probably means we're doing an initial-ish-sync or they're slow
                    LogPrint("net", "%s sending header-and-ids %s to peer %d\n", __func__,
                            vHeaders.front().GetHash().ToString(), pto->id);
                    //TODO: Shouldn't need to reload block from disk, but requires refactor
                    CBlock block;
                    assert(ReadBlockFromDisk(block, pBestIndex, consensusParams));
                    CBlockHeaderAndShortTxIDs cmpctblock(block, state.fWantsCmpctWitness);
                    pto->PushMessageWithFlag(state.fWantsCmpctWitness ? 0 : SERIALIZE_TRANSACTION_NO_WITNESS, NetMsgType::CMPCTBLOCK, cmpctblock);
                    state.pindexBestHeaderSent = pBestIndex;
                } else if (state.fPreferHeaders) {
                    if (vHeaders.size() > 1) {
                        LogPrint("net", "%s: %u headers, range (%s, %s), to peer=%d\n", __func__,
                                vHeaders.size(),
                                vHeaders.front().GetHash().ToString(),
                                vHeaders.back().GetHash().ToString(), pto->id);
                    } else {
                        LogPrint("net", "%s: sending header %s to peer=%d\n", __func__,
                                vHeaders.front().GetHash().ToString(), pto->id);
                    }
                    pto->PushMessage(NetMsgType::HEADERS, vHeaders);
                    state.pindexBestHeaderSent = pBestIndex;
                } else
                    fRevertToInv = true;
            }
            if (fRevertToInv) {
                // If falling back to using an inv, just try to inv the tip.
                // The last entry in vBlockHashesToAnnounce was our tip at some point
                // in the past.
                if (!pto->vBlockHashesToAnnounce.empty()) {
                    const uint256 &hashToAnnounce = pto->vBlockHashesToAnnounce.back();
                    BlockMap::iterator mi = mapBlockIndex.find(hashToAnnounce);
                    assert(mi != mapBlockIndex.end());
                    CBlockIndex *pindex = mi->second;

                    // Warn if we're announcing a block that is not on the main chain.
                    // This should be very rare and could be optimized out.
                    // Just log for now.
                    if (chainActive[pindex->nHeight] != pindex) {
                        LogPrint("net", "Announcing block %s not on main chain (tip=%s)\n",
                            hashToAnnounce.ToString(), chainActive.Tip()->GetBlockHash().ToString());
                    }

                    // If the peer's chain has this block, don't inv it back.
                    if (!PeerHasHeader(&state, pindex)) {
                        pto->PushInventory(CInv(MSG_BLOCK, hashToAnnounce));
                        LogPrint("net", "%s: sending inv peer=%d hash=%s\n", __func__,
                            pto->id, hashToAnnounce.ToString());
                    }
                }
            }
            pto->vBlockHashesToAnnounce.clear();
        }

        //
        // Message: inventory
        //
        vector<CInv> vInv;
        {
            LOCK(pto->cs_inventory);
            vInv.reserve(std::max<size_t>(pto->vInventoryBlockToSend.size(), INVENTORY_BROADCAST_MAX));

            // Add blocks
            BOOST_FOREACH(const uint256& hash, pto->vInventoryBlockToSend) {
                vInv.push_back(CInv(MSG_BLOCK, hash));
                if (vInv.size() == MAX_INV_SZ) {
                    pto->PushMessage(NetMsgType::INV, vInv);
                    vInv.clear();
                }
            }
            pto->vInventoryBlockToSend.clear();

            // Check whether periodic sends should happen
            bool fSendTrickle = pto->fWhitelisted;
            if (pto->nNextInvSend < nNow) {
                fSendTrickle = true;
                // Use half the delay for outbound peers, as there is less privacy concern for them.
                pto->nNextInvSend = PoissonNextSend(nNow, INVENTORY_BROADCAST_INTERVAL >> !pto->fInbound);
            }

            // Time to send but the peer has requested we not relay transactions.
            if (fSendTrickle) {
                LOCK(pto->cs_filter);
                if (!pto->fRelayTxes) pto->setInventoryTxToSend.clear();
            }

            // Respond to BIP35 mempool requests
            if (fSendTrickle && pto->fSendMempool) {
                auto vtxinfo = mempool.infoAll();
                pto->fSendMempool = false;
                CAmount filterrate = 0;
                {
                    LOCK(pto->cs_feeFilter);
                    filterrate = pto->minFeeFilter;
                }

                LOCK(pto->cs_filter);

                for (const auto& txinfo : vtxinfo) {
                    const uint256& hash = txinfo.tx->GetHash();
                    CInv inv(MSG_TX, hash);
                    pto->setInventoryTxToSend.erase(hash);
                    if (filterrate) {
                        if (txinfo.feeRate.GetFeePerK() < filterrate)
                            continue;
                    }
                    if (pto->pfilter) {
                        if (!pto->pfilter->IsRelevantAndUpdate(*txinfo.tx)) continue;
                    }
                    pto->filterInventoryKnown.insert(hash);
                    vInv.push_back(inv);
                    if (vInv.size() == MAX_INV_SZ) {
                        pto->PushMessage(NetMsgType::INV, vInv);
                        vInv.clear();
                    }
                }
                pto->timeLastMempoolReq = GetTime();
            }

            // Determine transactions to relay
            if (fSendTrickle) {
                // Produce a vector with all candidates for sending
                vector<std::set<uint256>::iterator> vInvTx;
                vInvTx.reserve(pto->setInventoryTxToSend.size());
                for (std::set<uint256>::iterator it = pto->setInventoryTxToSend.begin(); it != pto->setInventoryTxToSend.end(); it++) {
                    vInvTx.push_back(it);
                }
                CAmount filterrate = 0;
                {
                    LOCK(pto->cs_feeFilter);
                    filterrate = pto->minFeeFilter;
                }
                // Topologically and fee-rate sort the inventory we send for privacy and priority reasons.
                // A heap is used so that not all items need sorting if only a few are being sent.
                CompareInvMempoolOrder compareInvMempoolOrder(&mempool);
                std::make_heap(vInvTx.begin(), vInvTx.end(), compareInvMempoolOrder);
                // No reason to drain out at many times the network's capacity,
                // especially since we have many peers and some will draw much shorter delays.
                unsigned int nRelayedTransactions = 0;
                LOCK(pto->cs_filter);
                while (!vInvTx.empty() && nRelayedTransactions < INVENTORY_BROADCAST_MAX) {
                    // Fetch the top element from the heap
                    std::pop_heap(vInvTx.begin(), vInvTx.end(), compareInvMempoolOrder);
                    std::set<uint256>::iterator it = vInvTx.back();
                    vInvTx.pop_back();
                    uint256 hash = *it;
                    // Remove it from the to-be-sent set
                    pto->setInventoryTxToSend.erase(it);
                    // Check if not in the filter already
                    if (pto->filterInventoryKnown.contains(hash)) {
                        continue;
                    }
                    // Not in the mempool anymore? don't bother sending it.
                    auto txinfo = mempool.info(hash);
                    if (!txinfo.tx) {
                        continue;
                    }
                    if (filterrate && txinfo.feeRate.GetFeePerK() < filterrate) {
                        continue;
                    }
                    if (pto->pfilter && !pto->pfilter->IsRelevantAndUpdate(*txinfo.tx)) continue;
                    // Send
                    vInv.push_back(CInv(MSG_TX, hash));
                    nRelayedTransactions++;
                    {
                        // Expire old relay messages
                        while (!vRelayExpiration.empty() && vRelayExpiration.front().first < nNow)
                        {
                            mapRelay.erase(vRelayExpiration.front().second);
                            vRelayExpiration.pop_front();
                        }

                        auto ret = mapRelay.insert(std::make_pair(hash, std::move(txinfo.tx)));
                        if (ret.second) {
                            vRelayExpiration.push_back(std::make_pair(nNow + 15 * 60 * 1000000, ret.first));
                        }
                    }
                    if (vInv.size() == MAX_INV_SZ) {
                        pto->PushMessage(NetMsgType::INV, vInv);
                        vInv.clear();
                    }
                    pto->filterInventoryKnown.insert(hash);
                }
            }
        }
        if (!vInv.empty())
            pto->PushMessage(NetMsgType::INV, vInv);

        // Detect whether we're stalling
        nNow = GetTimeMicros();
        if (!pto->fDisconnect && state.nStallingSince && state.nStallingSince < nNow - 1000000 * BLOCK_STALLING_TIMEOUT) {
            // Stalling only triggers when the block download window cannot move. During normal steady state,
            // the download window should be much larger than the to-be-downloaded set of blocks, so disconnection
            // should only happen during initial block download.
            LogPrintf("Peer=%d is stalling block download, disconnecting\n", pto->id);
            pto->fDisconnect = true;
        }
        // In case there is a block that has been in flight from this peer for 2 + 0.5 * N times the block interval
        // (with N the number of peers from which we're downloading validated blocks), disconnect due to timeout.
        // We compensate for other peers to prevent killing off peers due to our own downstream link
        // being saturated. We only count validated in-flight blocks so peers can't advertise non-existing block hashes
        // to unreasonably increase our timeout.
        if (!pto->fDisconnect && state.vBlocksInFlight.size() > 0) {
            QueuedBlock &queuedBlock = state.vBlocksInFlight.front();
            int nOtherPeersWithValidatedDownloads = nPeersWithValidatedDownloads - (state.nBlocksInFlightValidHeaders > 0);
            if (nNow > state.nDownloadingSince + consensusParams.nPowTargetSpacing * (BLOCK_DOWNLOAD_TIMEOUT_BASE + BLOCK_DOWNLOAD_TIMEOUT_PER_PEER * nOtherPeersWithValidatedDownloads)) {
                LogPrintf("Timeout downloading block %s from peer=%d, disconnecting\n", queuedBlock.hash.ToString(), pto->id);
                pto->fDisconnect = true;
            }
        }

        //
        // Message: getdata (blocks)
        //
        vector<CInv> vGetData;
        if (!pto->fDisconnect && !pto->fClient && (fFetch || !IsInitialBlockDownload()) && state.nBlocksInFlight < MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
            vector<CBlockIndex*> vToDownload;
            NodeId staller = -1;
            FindNextBlocksToDownload(pto->GetId(), MAX_BLOCKS_IN_TRANSIT_PER_PEER - state.nBlocksInFlight, vToDownload, staller, consensusParams);
            BOOST_FOREACH(CBlockIndex *pindex, vToDownload) {
                uint32_t nFetchFlags = GetFetchFlags(pto, pindex->pprev, consensusParams);
                vGetData.push_back(CInv(MSG_BLOCK | nFetchFlags, pindex->GetBlockHash()));
                MarkBlockAsInFlight(pto->GetId(), pindex->GetBlockHash(), consensusParams, pindex);
                LogPrint("net", "Requesting block %s (%d) peer=%d\n", pindex->GetBlockHash().ToString(),
                    pindex->nHeight, pto->id);
            }
            if (state.nBlocksInFlight == 0 && staller != -1) {
                if (State(staller)->nStallingSince == 0) {
                    State(staller)->nStallingSince = nNow;
                    LogPrint("net", "Stall started peer=%d\n", staller);
                }
            }
        }

        //
        // Message: getdata (non-blocks)
        //
        while (!pto->fDisconnect && !pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow)
        {
            const CInv& inv = (*pto->mapAskFor.begin()).second;
            if (!AlreadyHave(inv))
            {
                if (fDebug)
                    LogPrint("net", "Requesting %s peer=%d\n", inv.ToString(), pto->id);
                vGetData.push_back(inv);
                if (vGetData.size() >= 1000)
                {
                    pto->PushMessage(NetMsgType::GETDATA, vGetData);
                    vGetData.clear();
                }
            } else {
                //If we're not going to ask, don't expect a response.
                pto->setAskFor.erase(inv.hash);
            }
            pto->mapAskFor.erase(pto->mapAskFor.begin());
        }
        if (!vGetData.empty())
            pto->PushMessage(NetMsgType::GETDATA, vGetData);

        //
        // Message: feefilter
        //
        // We don't want white listed peers to filter txs to us if we have -whitelistforcerelay
        if (!pto->fDisconnect && pto->nVersion >= FEEFILTER_VERSION && GetBoolArg("-feefilter", DEFAULT_FEEFILTER) &&
            !(pto->fWhitelisted && GetBoolArg("-whitelistforcerelay", DEFAULT_WHITELISTFORCERELAY))) {
            CAmount currentFilter = mempool.GetMinFee(GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000).GetFeePerK();
            int64_t timeNow = GetTimeMicros();
            if (timeNow > pto->nextSendTimeFeeFilter) {
                CAmount filterToSend = filterRounder.round(currentFilter);
                if (filterToSend != pto->lastSentFeeFilter) {
                    pto->PushMessage(NetMsgType::FEEFILTER, filterToSend);
                    pto->lastSentFeeFilter = filterToSend;
                }
                pto->nextSendTimeFeeFilter = PoissonNextSend(timeNow, AVG_FEEFILTER_BROADCAST_INTERVAL);
            }
            // If the fee filter has changed substantially and it's still more than MAX_FEEFILTER_CHANGE_DELAY
            // until scheduled broadcast, then move the broadcast to within MAX_FEEFILTER_CHANGE_DELAY.
            else if (timeNow + MAX_FEEFILTER_CHANGE_DELAY * 1000000 < pto->nextSendTimeFeeFilter &&
                     (currentFilter < 3 * pto->lastSentFeeFilter / 4 || currentFilter > 4 * pto->lastSentFeeFilter / 3)) {
                pto->nextSendTimeFeeFilter = timeNow + (insecure_rand() % MAX_FEEFILTER_CHANGE_DELAY) * 1000000;
            }
        }
    }
    return true;
}

 std::string CBlockFileInfo::ToString() const {
     return strprintf("CBlockFileInfo(blocks=%u, size=%u, heights=%u...%u, time=%s...%s)", nBlocks, nSize, nHeightFirst, nHeightLast, DateTimeStrFormat("%Y-%m-%d", nTimeFirst), DateTimeStrFormat("%Y-%m-%d", nTimeLast));
 }

ThresholdState VersionBitsTipState(const Consensus::Params& params, Consensus::DeploymentPos pos)
{
    LOCK(cs_main);
    return VersionBitsState(chainActive.Tip(), params, pos, versionbitscache);
}

class CMainCleanup
{
public:
    CMainCleanup() {}
    ~CMainCleanup() {
        // block headers
        BlockMap::iterator it1 = mapBlockIndex.begin();
        for (; it1 != mapBlockIndex.end(); it1++)
            delete (*it1).second;
        mapBlockIndex.clear();

        // orphan transactions
        mapOrphanTransactions.clear();
        mapOrphanTransactionsByPrev.clear();
    }
} instance_of_cmaincleanup;
