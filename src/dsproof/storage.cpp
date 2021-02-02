// Copyright (C) 2019-2020 Tom Zander <tomz@freedommail.ch>
// Copyright (C) 2020 Calin Culianu <calin.culianu@gmail.com>
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/siphash.h>
#include <dsproof/storage.h>
#include <logging.h>
#include <primitives/transaction.h>
#include <random.h>
#include <util/time.h>

#include <cstdint>
#include <limits>
#include <stdexcept>


DoubleSpendProofStorage::DoubleSpendProofStorage()
    : m_recentRejects(120000, 0.000001)
{
}

//! Helper struct to catch index modify failures (indicates programming error)
struct DoubleSpendProofStorage::ModFastFail {
    void operator()(Entry &e) const {
        LogPrintf("DSProof: Failed to modify m_proofs for entry: %s\n", e.proof.GetId().ToString());
        assert(!"Internal Error: Failed to modify an m_proofs entry");
    }
};

bool DoubleSpendProofStorage::add(const DoubleSpendProof &proof)
{
    if (proof.isEmpty()) {
        // this should never happen and indicates a programming error
        throw std::invalid_argument(strprintf("%s: DSProof is empty", __func__));
    }

    LOCK(m_lock);

    const auto &hash = proof.GetId();
    {
        auto it = m_proofs.find(hash);
        if (it != m_proofs.end()) {
            if (it->orphan) {
                // mark it as not an orphan now due to explicit add
                decrementOrphans(1);
                m_proofs.modify(it, [](Entry &e) { e.orphan = false; }, ModFastFail());
            }
            return false;
        }
    }

    Entry e;
    e.proof = proof;
    m_proofs.emplace(std::move(e));
    return true;
}

void DoubleSpendProofStorage::addOrphan(const DoubleSpendProof &proof, NodeId nodeId)
{
    LOCK(m_lock);
    add(proof);
    const DspId &hash = proof.GetId();
    auto it = m_proofs.find(hash);
    assert(it != m_proofs.end()); // cannot happen since above add() call guarantees it now exists

    m_proofs.modify(it, [nodeId, this, &hash](Entry &e) EXCLUSIVE_LOCKS_REQUIRED(m_lock) {
        if (e.nodeId < 0 && nodeId > -1)
            e.nodeId = nodeId;
        if (e.timeStamp < 0)
            e.timeStamp = GetTime();
        incrementOrphans(!e.orphan, hash); // actually increments only if orphan false -- may reap older orphans as a side-effect
        e.orphan = true; // called after to ensure this one makes it in as an orphan even if above reaper reaped
    }, ModFastFail());
}

std::list<std::pair<DspId, NodeId>> DoubleSpendProofStorage::findOrphans(const COutPoint &prevOut) const
{
    std::list<std::pair<DspId, NodeId>> answer;
    LOCK(m_lock);
    const auto iters = m_proofs.get<tag_COutPoint>().equal_range(prevOut);
    for (auto it = iters.first; it != iters.second; ++it) {
        if (it->orphan)
            answer.emplace_back(it->proof.GetId(), it->nodeId);
    }
    return answer;
}

/// Returns all the orphans known to this storage instance.
std::vector<std::pair<DoubleSpendProof, bool>> DoubleSpendProofStorage::getAll(bool includeOrphans) const {
    std::vector<std::pair<DoubleSpendProof, bool>> ret;
    LOCK(m_lock);
    for (const auto & entry: m_proofs) {
        if (entry.orphan && !includeOrphans)
            continue;
        ret.emplace_back(entry.proof, entry.orphan);
    }
    return ret;
}

void DoubleSpendProofStorage::claimOrphan(const DspId &hash)
{
    LOCK(m_lock);
    auto it = m_proofs.find(hash);
    if (it != m_proofs.end() && it->orphan) {
        decrementOrphans(1);
        m_proofs.modify(it, [](Entry &e){ e.orphan = false; }, ModFastFail());
    }
}

bool DoubleSpendProofStorage::remove(const DspId &hash)
{
    LOCK(m_lock);
    auto it = m_proofs.find(hash);
    if (it != m_proofs.end()) {
        decrementOrphans(it->orphan); // actually decrements only if orphan == true
        m_proofs.erase(it);
        return true;
    }
    return false;
}

DoubleSpendProof DoubleSpendProofStorage::lookup(const DspId &hash) const
{
    DoubleSpendProof ret;
    LOCK(m_lock);
    auto it = m_proofs.find(hash);
    if (it != m_proofs.end())
        ret = it->proof;
    return ret;
}

bool DoubleSpendProofStorage::exists(const DspId &hash) const
{
    LOCK(m_lock);
    return m_proofs.find(hash) != m_proofs.end();
}

bool DoubleSpendProofStorage::isRecentlyRejectedProof(const DspId &hash) const
{
    LOCK(m_lock);
    return m_recentRejects.contains(hash);
}

void DoubleSpendProofStorage::markProofRejected(const DspId &hash)
{
    LOCK(m_lock);
    m_recentRejects.insert(hash);
}

void DoubleSpendProofStorage::newBlockFound()
{
    LOCK(m_lock);
    m_recentRejects.reset();
}

size_t DoubleSpendProofStorage::size() const {
    LOCK(m_lock);
    return m_proofs.size();
}

void DoubleSpendProofStorage::clear() {
    LOCK(m_lock);
    m_proofs.clear();
    m_recentRejects.reset();
    m_numOrphans = 0;
}

DoubleSpendProofStorage::SaltedHasher::SaltedHasher()
    : k0(GetRand(std::numeric_limits<uint64_t>::max())),
      k1(GetRand(std::numeric_limits<uint64_t>::max()))
{}

size_t DoubleSpendProofStorage::SaltedHasher::operator()(const uint256 &hash) const
{
    return SipHashUint256(k0, k1, hash);
}

size_t DoubleSpendProofStorage::SaltedHasher::operator()(const COutPoint &outPoint) const
{
    return SipHashUint256Extra(k0, k1, outPoint.GetTxId(), outPoint.GetN());
}


// --- Orphan upkeep (see also storage_cleanup.cpp)

int DoubleSpendProofStorage::secondsToKeepOrphans() const {
    LOCK(m_lock);
    return m_secondsToKeepOrphans;
}

void DoubleSpendProofStorage::setSecondsToKeepOrphans(int secs) {
    if (secs >= 0) {
        LOCK(m_lock);
        m_secondsToKeepOrphans = secs;
    }
}

size_t DoubleSpendProofStorage::maxOrphans() const {
    LOCK(m_lock);
    return m_maxOrphans;
}
void DoubleSpendProofStorage::setMaxOrphans(size_t max) {
    LOCK(m_lock);
    m_maxOrphans = max;
}

size_t DoubleSpendProofStorage::numOrphans() const {
    LOCK(m_lock);
    return m_numOrphans;
}

void DoubleSpendProofStorage::decrementOrphans(size_t n)
{
    if (n) {
        if (m_numOrphans < n)
            throw std::runtime_error(strprintf("Internal error in DSProof %s: Orphan counter not as expected.", __func__));
        m_numOrphans -= n;
    }
}

void DoubleSpendProofStorage::incrementOrphans(size_t n, const DspId &dontDeleteHash)
{
    if (n) {
        m_numOrphans += n;
        checkOrphanLimit(dontDeleteHash);
    }
}

void DoubleSpendProofStorage::checkOrphanLimit(const DspId &dontDeleteHash)
{
    // allow up to 25% more than maxOrphans() as a performance tweak, to avoid this being called for every ophan add.
    const size_t highWaterMark = size_t(m_maxOrphans * 1.25);
    const size_t lowWaterMark = m_maxOrphans;
    if (m_numOrphans > highWaterMark) {
        // remove oldest first
        size_t ctr = 0;
        auto &index = m_proofs.get<tag_TimeStamp>(); // ordered by timestamp
        for (auto it = index.begin(); it != index.end() && m_numOrphans > lowWaterMark; ) {
            if (it->orphan && dontDeleteHash != it->proof.GetId()) {
                it = index.erase(it);
                decrementOrphans(1);
                ++ctr;
            } else
                ++it;
        }
        LogPrint(BCLog::DSPROOF, "DSProof %s: reaped %d orphans, orphan count now %d (thresh-low: %d, thresh-high: %d",
                 __func__, ctr, m_numOrphans, lowWaterMark, highWaterMark);
    }
}
