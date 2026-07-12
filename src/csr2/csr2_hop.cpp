#include "csr2_internal.hpp"

#include <algorithm>
#include <functional>
#include <queue>
#include <tuple>

namespace fox::csr2::detail {

namespace {

bool IsPartStatVertex(Abc_Obj_t *pObj)
{
    return pObj
        && (Abc_ObjIsPi(pObj)
         || Abc_ObjIsNode(pObj)
         || Abc_ObjType(pObj) == ABC_OBJ_CONST1);
}

int RecomputeArrival(Abc_Obj_t *pObj, const std::vector<int> &arrival)
{
    if (!IsPartStatVertex(pObj)
        || Abc_ObjGetPartId(pObj) == ABC_PART_ID_NONE)
        return 0;

    int best = 0;
    Abc_Obj_t *pFanin;
    int i;
    Abc_ObjForEachFanin(pObj, pFanin, i)
    {
        if (!IsPartStatVertex(pFanin)
            || Abc_ObjGetPartId(pFanin) == ABC_PART_ID_NONE)
            continue;
        const int candidate = arrival[pFanin->Id]
            + (Abc_ObjGetPartId(pFanin) != Abc_ObjGetPartId(pObj));
        best = std::max(best, candidate);
    }
    return best;
}

bool ComputeFullArrival(Abc_Ntk_t *pNtk, std::vector<int> &arrival,
                        std::vector<int> *topo_rank)
{
    if (!pNtk)
        return false;

    arrival.assign(Abc_NtkObjNumMax(pNtk), 0);
    if (topo_rank)
        topo_rank->assign(Abc_NtkObjNumMax(pNtk), -1);

    Vec_Ptr_t *vNodes = Abc_NtkDfs(pNtk, 1);
    if (!vNodes)
        return false;
    Abc_Obj_t *pObj;
    int i;
    Vec_PtrForEachEntry(Abc_Obj_t *, vNodes, pObj, i)
    {
        if (pObj->Id >= static_cast<int>(arrival.size()))
        {
            Vec_PtrFree(vNodes);
            return false;
        }
        if (topo_rank)
            (*topo_rank)[pObj->Id] = i;
        if (IsPartStatVertex(pObj))
            arrival[pObj->Id] = RecomputeArrival(pObj, arrival);
    }
    Vec_PtrFree(vNodes);
    return true;
}

} // namespace

bool HopState::Initialize(Abc_Ntk_t *pNtk)
{
    return ComputeFullArrival(pNtk, arrival_, &topo_rank_);
}

HopState::Transaction HopState::BeginTransaction() const
{
    Transaction txn;
    txn.logged.assign(arrival_.size(), 0);
    return txn;
}

bool HopState::PropagateFrom(Abc_Ntk_t *pNtk,
                             const std::vector<int> &start_ids,
                             int hop_limit, Transaction &txn)
{
    if (!pNtk || arrival_.size() != static_cast<size_t>(Abc_NtkObjNumMax(pNtk))
        || topo_rank_.size() != arrival_.size())
        return false;
    if (txn.logged.size() != arrival_.size())
        txn.logged.assign(arrival_.size(), 0);

    using QueueEntry = std::pair<int, int>;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>,
                        std::greater<QueueEntry>> queue;
    std::vector<char> queued(arrival_.size(), 0);
    for (int obj_id : start_ids)
    {
        if (obj_id < 0 || static_cast<size_t>(obj_id) >= arrival_.size()
            || topo_rank_[obj_id] < 0)
            return false;
        if (!queued[obj_id])
        {
            queue.emplace(topo_rank_[obj_id], obj_id);
            queued[obj_id] = 1;
        }
    }

    while (!queue.empty())
    {
        const int obj_id = queue.top().second;
        queue.pop();
        queued[obj_id] = 0;
        Abc_Obj_t *pObj = Abc_NtkObj(pNtk, obj_id);
        if (!pObj || !IsPartStatVertex(pObj))
            continue;

        const int updated = RecomputeArrival(pObj, arrival_);
        if (updated == arrival_[obj_id])
            continue;
        if (!txn.logged[obj_id])
        {
            txn.changes.push_back({obj_id, arrival_[obj_id]});
            txn.logged[obj_id] = 1;
        }
        arrival_[obj_id] = updated;
        if (updated > hop_limit)
            return false;

        Abc_Obj_t *pFanout;
        int i;
        Abc_ObjForEachFanout(pObj, pFanout, i)
        {
            if (!IsPartStatVertex(pFanout))
                continue;
            const int fanout_id = pFanout->Id;
            if (fanout_id < 0 || static_cast<size_t>(fanout_id) >= arrival_.size()
                || topo_rank_[fanout_id] < 0 || queued[fanout_id])
                continue;
            queue.emplace(topo_rank_[fanout_id], fanout_id);
            queued[fanout_id] = 1;
        }
    }
    return true;
}

void HopState::Rollback(Transaction &txn)
{
    for (auto it = txn.changes.rbegin(); it != txn.changes.rend(); ++it)
        arrival_[it->obj_id] = it->old_arrival;
    txn.changes.clear();
    std::fill(txn.logged.begin(), txn.logged.end(), 0);
}

bool HopState::VerifyAgainstFull(Abc_Ntk_t *pNtk) const
{
    std::vector<int> full;
    return ComputeFullArrival(pNtk, full, nullptr) && full == arrival_;
}

int HopState::arrival(int obj_id) const
{
    return obj_id >= 0 && static_cast<size_t>(obj_id) < arrival_.size()
        ? arrival_[obj_id] : -1;
}

int HopState::topo_rank(int obj_id) const
{
    return obj_id >= 0 && static_cast<size_t>(obj_id) < topo_rank_.size()
        ? topo_rank_[obj_id] : -1;
}

} // namespace fox::csr2::detail
