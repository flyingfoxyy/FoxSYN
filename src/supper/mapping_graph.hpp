// #pragma once

// #include <vector>

// #include "basic.hpp"

// #define USE_LIT_FANIN 1

// namespace fox::supper {
// ////////////////////////////////////////////////////////////////////////////////////////////////
// ///// A compact implementation for N-bounded graph
// ////////////////////////////////////////////////////////////////////////////////////////////////
// template <std::size_t N>
// class graph_t {
// public:
//     enum class node_type {
//         ONE   ,
//         PI    ,
//         PO    ,
//         LOGIC ,
//         NONE
//     };
//     class node_t
//     {
//         Lit        _fanins[N] {0};
//         node_type  _type :  3 {0};
//         uint       _size :  5 {0};
//         uint       _id   : 24 {0};
//     public:
//         node_t() = default;
//        ~node_t() = default;
//         node_t(node_type type, uint size) : _type(type), _size(size) {}

//         void set_fanin(uint fanin, uint idx, uint c) {
//             _fanins[idx] = compl_cond(fanin, c);
//         }

//         node_t *get_fanin(uint idx) const {
//             assert(_id < _fanins[idx]);
//             return this - _fanins[idx];
//         }

//         uint input_size() const { return _size; }
//     };

//     using elem_type = node_t;

// protected:
//     std::vector<node_t>   _nodes;
//     std::vector<node_t *> _pi;
//     std::vector<node_t *> _po;

// public:
//     graph_t(uint max_node_num, uint num_pi = 0, uint num_po = 0) {
//         _nodes.reserve(max_node_num);
//         if (num_pi)
//             _pi.reserve(num_pi);
//         if (num_po)
//             _po.reserve(num_po);
//     }

//     // create 
//     template <typename... Args>
//     void add_node(Args&&... args) {
//         node_t &node = _nodes.emplace_back(std::forward<Args>(args)...);
//         if (node.is_pi())
//             _pi.push_back(&node);
//         else if (node.is_po())
//             _po.push_back(&node);
//         node->_id = _nodes.size() - 1;
//     }

//     node_t       *operator[](uint   id)     { return &_nodes[id]; }
//     node_t       *operator[](node_t *n)     { return n;           }
//     const node_t *operator[](uint id) const { return &_nodes[id]; }
//     node_t       *pi        (uint id) const { return _pi[id];     }
//     node_t       *po        (uint id) const { return _po[id];     }

//     uint num_pi() const { return _pi.size(); }
//     uint num_po() const { return _po.size(); }
// };

// extern template class graph_t <2>;
// extern template class graph_t <6>;
// extern template class graph_t <7>;
// extern template class graph_t <8>;
// extern template class graph_t <9>;
// extern template class graph_t<10>;
// extern template class graph_t<11>;
// extern template class graph_t<12>;
// extern template class graph_t<13>;
// extern template class graph_t<14>;
// extern template class graph_t<15>;
// extern template class graph_t<16>;

// }
