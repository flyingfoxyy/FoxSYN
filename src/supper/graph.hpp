#pragma once

#include "basic.hpp"

namespace fox::supper {
// ========================================================================
// graph_t
// ========================================================================
class graph_t {
public:
    enum class node_type_t : uint8_t {
        ONE   ,
        PI    ,
        PO    ,
        LOGIC ,
        NONE
    };

    class node_t
    {
        uint        _size : 28;
        node_type_t _type :  4;
        Lit         _fanins[2];
    public:
        node_t()                                 : _size(0), _type(node_type_t::NONE), _fanins{} {}
        node_t(node_type_t type)                 : _size(0), _type(type), _fanins{}              {}
        node_t(node_type_t type, Lit f0)         : _size(1), _type(type), _fanins{f0, Lit(0)}    {}
        node_t(node_type_t type, Lit f0, Lit f1) : _size(2), _type(type), _fanins{f0, f1}        {}
       ~node_t() = default;

        Inline uint        size()     const { return _size;      }
        Inline node_type_t type()     const { return _type;      }
        Inline Lit operator[](uint i) const { return _fanins[i]; }

        Inline bool null    () const { return _type == node_type_t::NONE;  }
        Inline bool is_logic() const { return _type == node_type_t::LOGIC; }
        Inline bool is_pi   () const { return _type == node_type_t::PI;    }
        Inline bool is_po   () const { return _type == node_type_t::PO;    }

       friend class graph_t;
    };

protected:
    std::vector<node_t> _nodes;
    std::vector<uint>   _pi;
    std::vector<uint>   _po;

public:
    graph_t(uint max_node_num, uint num_pi = 0, uint num_po = 0) {
        _nodes.reserve(max_node_num);
        _pi   .reserve(num_pi);
        _po   .reserve(num_po);
    }

    ~graph_t() = default;

    Inline uint pwr()       const { return 0;             }
    Inline uint num_nodes() const { return _nodes.size(); }
    Inline uint num_po()    const { return _po.size();    }
    Inline uint num_pi()    const { return _pi.size();    }
    Inline uint num_logic() const { return num_nodes() - num_po() - num_pi() - 1; }

    Inline int begin()        const { return 1;                  }
    Inline int end()          const { return _nodes.size();      }
    Inline int rbegin()       const { return _nodes.size() - 1;  }
    Inline int rend()         const { return -1;                 }

    Inline int logic_begin()  const { return 1 + num_po() + num_pi(); }
    Inline int logic_end()    const { return end();                   }
    Inline int logic_rbegin() const { return _nodes.size() - 1;       }
    Inline int logic_rend()   const { return num_po() + num_pi();     }

    Inline int pi_begin()     const { return 1; }
    Inline int pi_end()       const { return 1 + num_pi(); }

    Inline const node_t &operator[](uint i) const { return _nodes[i];        }
    Inline const node_t &operator[](Lit  i) const { return _nodes[i.id()];   }
    Inline const node_t &get_pi(uint idx)   const { return _nodes[_pi[idx]]; }
    Inline const node_t &get_po(uint idx)   const { return _nodes[_po[idx]]; }

    Inline uint po_id(uint idx) const { return _po[idx]; }
    Inline uint pi_id(uint idx) const { return _pi[idx]; }

    void report(std::ostream &os);

    bool is_topologically_sorted() const;

    void *to_abc_ntk();

    /**
     * @brief Convert the graph structure to DOT format
     * 
     * @param path The path where the DOT file will be written
     * @return true if the file was successfully written, false otherwise
     */
    bool to_dot(const std::string &path) const;

    /**
     * @brief Convert the graph structure to Verilog netlist
     * 
     * @param path The path where the Verilog file will be written
     * @return true if the file was successfully written, false otherwise
     */
    bool write_to_verilog(const std::string &path) const;
};

#define ForEachGraphNode(mgr)                                              \
    for (int idx = (mgr).begin(); idx != (mgr).end(); ++idx)               \
        if ((mgr)[idx].null()) [[unlikely]] {} else

#define ForEachGraphLogicNode(mgr)                                         \
    for (int idx = (mgr).logic_begin(); idx != (mgr).logic_end(); ++idx)   \
        if (!(mgr)[idx].is_logic()) [[unlikely]] {} else

#define ForEachGraphNodeRev(mgr)                                           \
    for (int idx = (mgr).rbegin(); idx != (mgr).rend(); --idx)             \
        if ((mgr)[idx].null()) [[unlikely]] {} else

#define ForEachGraphLogicNodeRev(mgr)                                      \
    for (int idx = (mgr).logic_rbegin(); idx != (mgr).logic_rend(); --idx) \
        if (!(mgr)[idx].is_logic()) [[unlikely]] {} else

#define ForEachGraphPi(mgr) for (int idx = 0; idx != (mgr).num_pi(); ++idx)

#define ForEachGraphPo(mgr) for (int idx = 0; idx != (mgr).num_po(); ++idx)

#define ForEachGraphPoV(mgr)                                        \
    for (int idx = 0; idx != (mgr).num_po(); ++idx)                 \
        if (auto &n = (mgr).get_po(idx); n.size() && (mgr)[n[0]].is_logic())

}
