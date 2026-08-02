#include "csr3/csr3.hpp"
#include "csr3/csr3_internal.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "base/abc/abc.h"

extern "C" {
#include "aig/aig/aig.h"
#include "sat/cnf/cnf.h"
#include "sat/bsat/satSolver.h"
Aig_Man_t * Abc_NtkToDar(Abc_Ntk_t *pNtk, int fExors, int fRegisters);
}

namespace fox::csr3 {

// Minterm cube over n vars for value v (var i = bit i of v, LSB first).
static std::string minterm_cube(uint64_t v, int n)
{
    std::string s(n, '0');
    for (int i = 0; i < n; ++i)
        if ((v >> i) & 1) s[i] = '1';
    return s;
}

std::string encoder_sop(const std::vector<uint64_t> &tuples, int k, int bit)
{
    // code assignment (Step 8) = tuple index; input is the k-bit line tuple.
    std::string sop;
    for (size_t c = 0; c < tuples.size(); ++c) {
        if ((c >> bit) & 1) {
            sop += minterm_cube(tuples[c], k);
            sop += " 1\n";
        }
    }
    return sop;
}

std::string decoder_sop(const std::vector<uint64_t> &tuples, int r, int j)
{
    // input is the r-bit code; onset = codes whose original tuple had line j set.
    std::string sop;
    for (size_t c = 0; c < tuples.size(); ++c) {
        if ((tuples[c] >> j) & 1) {
            sop += minterm_cube((uint64_t)c, r);
            sop += " 1\n";
        }
    }
    return sop;
}

// Step 10: prove  dec(enc(cone(x))) == cone(x)  for all x in support (miter includes
// the cone so don't-care codes on unreachable tuples are never exercised).
bool verify_group_codec(Abc_Ntk_t *pCone, const std::vector<uint64_t> &tuples, int k, int btlimit)
{
    int m = (int)tuples.size();
    int r = ceil_log2((long)m);

    Abc_Ntk_t *pMiter = Abc_NtkAlloc(ABC_NTK_LOGIC, ABC_FUNC_SOP, 1);
    pMiter->pName = Extra_UtilStrsav("csr3_codec_miter");
    Mem_Flex_t *pMan = (Mem_Flex_t *)pMiter->pManFunc;

    // duplicate the cone (support -> the k line values) into the miter
    Abc_NtkCleanCopy(pCone);
    Abc_Obj_t *pObj; int i;
    Abc_NtkForEachPi(pCone, pObj, i) pObj->pCopy = Abc_NtkCreatePi(pMiter);
    Abc_NtkForEachNode(pCone, pObj, i) {
        Abc_NtkDupObj(pMiter, pObj, 0);
        Abc_Obj_t *pFanin; int j;
        Abc_ObjForEachFanin(pObj, pFanin, j) Abc_ObjAddFanin(pObj->pCopy, pFanin->pCopy);
    }
    std::vector<Abc_Obj_t*> L(k);
    { int idx = 0; Abc_NtkForEachPo(pCone, pObj, idx) L[idx] = Abc_ObjFanin0(pObj)->pCopy; }

    // encoder: r nodes, each fed by all k line values
    std::vector<Abc_Obj_t*> C(r);
    for (int b = 0; b < r; ++b) {
        Abc_Obj_t *pEnc = Abc_NtkCreateNode(pMiter);
        for (int j = 0; j < k; ++j) Abc_ObjAddFanin(pEnc, L[j]);
        pEnc->pData = Abc_SopRegister(pMan, encoder_sop(tuples, k, b).c_str());
        C[b] = pEnc;
    }
    // decoder: k nodes; r==0 (m==1) means every line is a plain constant.
    // A per-line SOP with zero cubes (line j never set among reachable codes)
    // must become a 0-fanin constant node: ABC's SOP-to-AIG path (Abc_SopIsExorType)
    // asserts on a cube-less SOP that still has fanins wired to it.
    std::vector<Abc_Obj_t*> D(k);
    for (int j = 0; j < k; ++j) {
        Abc_Obj_t *pDec = Abc_NtkCreateNode(pMiter);
        if (r == 0) {
            pDec->pData = ((tuples[0] >> j) & 1) ? Abc_SopCreateConst1(pMan) : Abc_SopCreateConst0(pMan);
        } else {
            std::string sop = decoder_sop(tuples, r, j);
            if (sop.empty()) {
                pDec->pData = Abc_SopCreateConst0(pMan);
            } else {
                for (int b = 0; b < r; ++b) Abc_ObjAddFanin(pDec, C[b]);
                pDec->pData = Abc_SopRegister(pMan, sop.c_str());
            }
        }
        D[j] = pDec;
    }
    // XOR each line against its decoded value, OR the mismatches into one miter PO
    std::vector<Abc_Obj_t*> X(k);
    for (int j = 0; j < k; ++j) {
        Abc_Obj_t *pXor = Abc_NtkCreateNode(pMiter);
        Abc_ObjAddFanin(pXor, L[j]);
        Abc_ObjAddFanin(pXor, D[j]);
        pXor->pData = Abc_SopCreateXor(pMan, 2);
        X[j] = pXor;
    }
    Abc_Obj_t *pMiterOut = Abc_NtkCreateNode(pMiter);
    for (int j = 0; j < k; ++j) Abc_ObjAddFanin(pMiterOut, X[j]);
    pMiterOut->pData = Abc_SopCreateOr(pMan, k, NULL);
    Abc_Obj_t *pPo = Abc_NtkCreatePo(pMiter);
    Abc_ObjAddFanin(pPo, pMiterOut);

    if (!Abc_NtkCheck(pMiter)) { Abc_NtkDelete(pMiter); return false; }

    // prove the miter output can never be 1 (i.e. decoder(encoder(cone(x))) == cone(x))
    Abc_Ntk_t *pStrash = Abc_NtkStrash(pMiter, 0, 1, 0);
    Aig_Man_t *pAig = Abc_NtkToDar(pStrash, 0, 0);
    Cnf_Dat_t *pCnf = Cnf_Derive(pAig, Aig_ManCoNum(pAig));
    sat_solver *pSat = (sat_solver *)Cnf_DataWriteIntoSolver(pCnf, 1, 0);
    bool ok;
    if (pSat == NULL) {
        ok = true;   // CNF unconditionally UNSAT: PO can never be 1
    } else {
        Aig_Obj_t *pCo = Aig_ManCo(pAig, 0);
        Aig_Obj_t *pDrv = Aig_ObjFanin0(pCo);
        int fComplCo = Aig_ObjFaninC0(pCo);
        if (pDrv == NULL || Aig_ObjIsConst1(pDrv)) {
            ok = fComplCo ? true : false;   // PO is a structural constant
        } else {
            int var = pCnf->pVarNums[Aig_ObjId(pDrv)];
            int lit = Abc_Var2Lit(var, fComplCo);   // true iff driver == !fComplCo, i.e. PO == 1
            int status = sat_solver_solve(pSat, &lit, &lit + 1, (ABC_INT64_T)btlimit, 0, 0, 0);
            ok = (status == l_False);   // UNSAT under "PO=1": no counterexample exists
        }
        sat_solver_delete(pSat);
    }
    Cnf_DataFree(pCnf);
    Aig_ManStop(pAig);
    Abc_NtkDelete(pStrash);
    Abc_NtkDelete(pMiter);
    return ok;
}

// Step 11: insert encoder (srcPart, fed by the k lines) and decoder (dstPart, fed
// by the encoder) into pNtk, then repoint only the lines' dstPart fanouts to the
// decoder outputs. Local (srcPart) fanouts keep using the original line directly.
int apply_group_codec(Abc_Ntk_t *pNtk, const std::vector<Abc_Obj_t*> &lines,
                      const std::vector<uint64_t> &tuples, int srcPart, int dstPart)
{
    int k = (int)lines.size();
    int m = (int)tuples.size();
    int r = ceil_log2((long)m);
    Mem_Flex_t *pMan = (Mem_Flex_t *)pNtk->pManFunc;

    // Part-id assignment is deferred until after the acyclicity check below:
    // Abc_ObjSetPartId invalidates ALL Pdb stats (num_parts/cut_size/hop_num),
    // and that invalidation is not undone by this function's own rollback path,
    // so a rejected (cycle-introducing) attempt must never call it.
    std::vector<Abc_Obj_t*> C(r);
    for (int b = 0; b < r; ++b) {
        Abc_Obj_t *pEnc = Abc_NtkCreateNode(pNtk);
        for (int j = 0; j < k; ++j) Abc_ObjAddFanin(pEnc, lines[j]);
        pEnc->pData = Abc_SopRegister(pMan, encoder_sop(tuples, k, b).c_str());
        C[b] = pEnc;
    }

    // r==0 (m==1): every line is a plain constant, no encoder needed.
    // A per-line empty-onset SOP (line never set among reachable codes) must
    // become a 0-fanin constant node; see verify_group_codec for why.
    std::vector<Abc_Obj_t*> D(k);
    for (int j = 0; j < k; ++j) {
        Abc_Obj_t *pDec = Abc_NtkCreateNode(pNtk);
        if (r == 0) {
            pDec->pData = ((tuples[0] >> j) & 1) ? Abc_SopCreateConst1(pMan) : Abc_SopCreateConst0(pMan);
        } else {
            std::string sop = decoder_sop(tuples, r, j);
            if (sop.empty()) {
                pDec->pData = Abc_SopCreateConst0(pMan);
            } else {
                for (int b = 0; b < r; ++b) Abc_ObjAddFanin(pDec, C[b]);
                pDec->pData = Abc_SopRegister(pMan, sop.c_str());
            }
        }
        D[j] = pDec;
    }

    // snapshot each line's dstPart fanouts before patching (patching mutates fanout lists)
    std::vector<std::pair<Abc_Obj_t*,int>> patched;   // (fanout, line index j)
    for (int j = 0; j < k; ++j) {
        std::vector<Abc_Obj_t*> toPatch;
        Abc_Obj_t *pFanout; int i;
        Abc_ObjForEachFanout(lines[j], pFanout, i)
            if (Abc_ObjIsNode(pFanout) && Abc_ObjGetPartId(pFanout) == dstPart)
                toPatch.push_back(pFanout);
        for (Abc_Obj_t *pFanout : toPatch) {
            Abc_ObjPatchFanin(pFanout, lines[j], D[j]);
            patched.push_back({pFanout, j});
        }
    }

    // A joint decoder depends on ALL k lines; if any dstPart fanout we just
    // repointed is itself an ancestor of another line in the group (via some
    // other legal cross-partition path), the repoint closes a cycle that did
    // not exist in the original network. Detect and roll back rather than
    // install a network Abc_NtkCheck/CEC downstream can't even traverse.
    if (!Abc_NtkIsAcyclic(pNtk)) {
        for (auto &pr : patched) Abc_ObjPatchFanin(pr.first, D[pr.second], lines[pr.second]);
        for (Abc_Obj_t *pDec : D) Abc_NtkDeleteObj(pDec);
        for (Abc_Obj_t *pEnc : C) Abc_NtkDeleteObj(pEnc);
        return -1;
    }
    // accepted: only now tag the new nodes, since Abc_ObjSetPartId invalidates
    // Pdb stats and this attempt is no longer going to be rolled back
    for (Abc_Obj_t *pEnc : C) Abc_ObjSetPartId(pEnc, srcPart);
    for (Abc_Obj_t *pDec : D) Abc_ObjSetPartId(pDec, dstPart);
    return r;
}

} // namespace fox::csr3
