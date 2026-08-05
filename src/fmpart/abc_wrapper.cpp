#include "fmpart/abc_wrapper.hpp"

#include <algorithm>

namespace fox::fmpart {

namespace {
// 与 hpart.cpp 的 IsHyperNode 相同（那边的 IsCarrierNode 是它的重复体，此处合一）
bool IsHyperNode(Abc_Obj_t *pObj)
{
    return pObj != nullptr
        && (Abc_ObjIsPi(pObj)
         || Abc_ObjIsNode(pObj)
         || Abc_ObjIsLatch(pObj)
         || Abc_ObjType(pObj) == ABC_OBJ_CONST1);
}

bool ShouldTraverseInterconnect(Abc_Obj_t *pObj)
{
    return pObj != nullptr
        && (Abc_ObjIsNet(pObj) || Abc_ObjIsBi(pObj) || Abc_ObjIsBo(pObj));
}

void CollectSinks(Abc_Obj_t *pObj, const std::vector<int> &obj_to_vertex,
                  std::vector<int> &sinks, std::vector<char> &visited)
{
    Abc_Obj_t *pObjR = Abc_ObjRegular(pObj);
    Abc_Obj_t *pFanout;
    int i;

    if (pObjR == nullptr || pObjR->Id < 0 || pObjR->Id >= (int)visited.size())
        return;
    if (visited[pObjR->Id])
        return;
    visited[pObjR->Id] = 1;

    if (IsHyperNode(pObjR)) {
        const int vertex_id = obj_to_vertex[pObjR->Id];
        if (vertex_id >= 0)
            sinks.push_back(vertex_id);       // 0 基；hpart 为 hmetis 格式用 1 基
        return;
    }
    if (!ShouldTraverseInterconnect(pObjR))
        return;
    Abc_ObjForEachFanout(pObjR, pFanout, i)
        CollectSinks(pFanout, obj_to_vertex, sinks, visited);
}

} // namespace

AbcNtkWrapper::AbcNtkWrapper(Abc_Ntk_t *pNtk)
{
    Abc_Obj_t *pObj;
    int i;
    std::vector<int> obj_to_vertex(Abc_NtkObjNumMax(pNtk), -1);

    Abc_NtkForEachObj(pNtk, pObj, i) {
        if (IsHyperNode(pObj)) {
            obj_to_vertex[pObj->Id] = (int)m_vertices.size();
            m_vertices.push_back(pObj);
        }
    }

    Abc_NtkForEachObj(pNtk, pObj, i) {
        if (!IsHyperNode(pObj))
            continue;
        const int carrier_vertex = obj_to_vertex[pObj->Id];
        if (carrier_vertex < 0)
            continue;

        std::vector<int> pins;
        std::vector<char> visited(Abc_NtkObjNumMax(pNtk), 0);
        pins.push_back(carrier_vertex);

        Abc_Obj_t *pCarrier = pObj;
        if (Abc_ObjIsLatch(pObj)) {
            if (Abc_ObjFanoutNum(pObj) == 0)
                continue;
            pCarrier = Abc_ObjFanout0(pObj);
        }

        Abc_Obj_t *pFanout;
        int j;
        Abc_ObjForEachFanout(pCarrier, pFanout, j)
            CollectSinks(pFanout, obj_to_vertex, pins, visited);

        std::sort(pins.begin(), pins.end());
        pins.erase(std::unique(pins.begin(), pins.end()), pins.end());
        if (pins.size() >= 2)
            m_pins.push_back(std::move(pins));
    }
}

} // namespace fox::fmpart
