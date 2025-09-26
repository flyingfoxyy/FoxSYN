/*============================================================================\
|                                                                             |
| file:      node.cpp                                                         |
| author:    Longfei                                                          |
| purpose:   implementation of node                                           |
| version:   0.1                                                              |
| date:      2025-3-23                                                        |
\============================================================================*/

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <istream>

#include "base/abc/abc.h"

#include "mapping_graph.hpp"

namespace fox::supper {

// template class graph_t<2>;
// template class graph_t<6>;
// template class graph_t<7>;
// template class graph_t<8>;
// template class graph_t<9>;
// template class graph_t<10>;
// template class graph_t<11>;
// template class graph_t<12>;
// template class graph_t<13>;
// template class graph_t<14>;
// template class graph_t<15>;
// template class graph_t<16>;

// graph_t<2> *create_from_abc(Abc_Ntk_t *ntk) {
//     graph_t<2> *g = new graph_t<2>(Abc_NtkNodeNum(ntk), Abc_NtkPiNum(ntk), Abc_NtkPoNum(ntk));
//     int i;
//     Abc_Obj_t *node;
//     for (int n = 0; n != ntk->vObjs->nSize; ++i)
//     {
//         Abc_Obj_t *node = Abc_NtkObj(ntk, i);
//         if (node) {
//             switch (Abc_ObjType(node)) {
//                 case ABC_OBJ_NODE:
//                     g->add_node(graph_t<2>::node_type::LOGIC, Abc_ObjFaninId0(node), Abc_ObjFaninC0(node), Abc_ObjFaninId1(node), Abc_ObjFaninC1(node));
//                     break;
//             }
//         } else {
//             g->add_node();
//         }
//     }
//     Abc_NtkForEachObj(pNtk, pObj, i)

// }

}
