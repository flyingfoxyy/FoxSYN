#pragma once

#include <string>
#include <utility>
#include <vector>

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
        node_type_t     _type {node_type_t::NONE};
        std::vector<Lit> _fanins;
        word            _truth {0};
        bool            _has_truth {false};
    public:
        node_t()                         = default;
        node_t(node_type_t type)         : _type(type) {}
        node_t(node_type_t type, Lit f0) : _type(type), _fanins{f0} {}
        node_t(node_type_t type, Lit f0, Lit f1) : _type(type), _fanins{f0, f1} {}
        node_t(node_type_t type, std::vector<Lit> fanins, word truth)
            : _type(type), _fanins(std::move(fanins)), _truth(truth), _has_truth(true) {}
       ~node_t() = default;

        Inline uint        size()     const { return _fanins.size(); }
        Inline node_type_t type()     const { return _type;      }
        Inline Lit operator[](uint i) const { return _fanins[i]; }
        Inline word        truth()    const { return _truth;     }
        Inline bool        has_truth() const { return _has_truth; }

        Inline bool null    () const { return _type == node_type_t::NONE;  }
        Inline bool is_logic() const { return _type == node_type_t::LOGIC; }
        Inline bool is_pi   () const { return _type == node_type_t::PI;    }
        Inline bool is_po   () const { return _type == node_type_t::PO;    }

        Inline void set_fanin(Lit fanin) {
            _fanins.clear();
            _fanins.push_back(fanin);
        }

       friend class graph_t;
    };

protected:
    std::vector<node_t> _nodes;
    std::vector<uint>   _pi;
    std::vector<uint>   _po;
    std::vector<std::string> _pi_names;
    std::vector<std::string> _po_names;

public:
    graph_t(uint max_node_num, uint num_pi = 0, uint num_po = 0) {
        _nodes   .reserve(max_node_num);
        _pi      .reserve(num_pi);
        _po      .reserve(num_po);
        _pi_names.reserve(num_pi);
        _po_names.reserve(num_po);
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
    Inline const std::string &pi_name(uint idx) const { return _pi_names[idx]; }
    Inline const std::string &po_name(uint idx) const { return _po_names[idx]; }

    uint add_const1();
    uint add_pi(std::string name = {});
    uint add_po(std::string name = {});
    uint add_lut(std::vector<Lit> fanins, word truth);
    void set_po_fanin(uint po_idx, Lit fanin);

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
