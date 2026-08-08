#include "hive/hive_graph.hpp"

#include <algorithm>
#include <queue>

namespace fox::hive {

CombGraph::CombGraph(Abc_Ntk_t *pNtk)
    : m_ntk(pNtk)
{
    m_max_id = Abc_NtkObjNumMax(pNtk);
    m_is_vertex.assign(m_max_id, 0);
    m_ext_out.assign(m_max_id, 0);
    m_rank.assign(m_max_id, 0);
    m_preds.assign(m_max_id, {});
    m_succs.assign(m_max_id, {});
    m_ext_ins.assign(m_max_id, {});

    Abc_Obj_t *pObj;
    int i;
    Abc_NtkForEachNode(pNtk, pObj, i)
    {
        m_is_vertex[Abc_ObjId(pObj)] = 1;
        m_vertices.push_back((int)Abc_ObjId(pObj));
    }

    Abc_NtkForEachNode(pNtk, pObj, i)
    {
        const int id = (int)Abc_ObjId(pObj);
        Abc_Obj_t *pFanin;
        int k;
        Abc_ObjForEachFanin(pObj, pFanin, k)
        {
            const int fid = (int)Abc_ObjId(pFanin);
            if (m_is_vertex[fid])
                m_preds[id].push_back(fid);
            else
                m_ext_ins[id].push_back(fid);   // PI or BO
        }
        auto dedup = [](std::vector<int> &v)
        {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        };
        dedup(m_preds[id]);
        dedup(m_ext_ins[id]);

        Abc_Obj_t *pFanout;
        Abc_ObjForEachFanout(pObj, pFanout, k)
            if (!m_is_vertex[Abc_ObjId(pFanout)])
                m_ext_out[id] = 1;              // PO or BI (or any non-vertex)
    }

    // transpose for succs (keeps preds/succs mutually consistent)
    for (int v : m_vertices)
        for (int p : m_preds[v])
            m_succs[p].push_back(v);
    for (int v : m_vertices)
        std::sort(m_succs[v].begin(), m_succs[v].end());

    // rank: Kahn over vertex preds; rank(v) = 1 + max(rank(pred)), 0 if none
    std::vector<int> indeg(m_max_id, 0);
    for (int v : m_vertices)
        indeg[v] = (int)m_preds[v].size();
    std::queue<int> q;
    for (int v : m_vertices)
        if (indeg[v] == 0)
            q.push(v);
    int processed = 0;
    while (!q.empty())
    {
        const int v = q.front();
        q.pop();
        ++processed;
        for (int s : m_succs[v])
        {
            m_rank[s] = std::max(m_rank[s], m_rank[v] + 1);
            if (--indeg[s] == 0)
                q.push(s);
        }
    }
    m_acyclic = processed == (int)m_vertices.size();
}

} // namespace fox::hive
