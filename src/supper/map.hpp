#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>
#include <cassert>

#include "basic.hpp"

namespace fox::supper {

class Cut {
public:
    static constexpr uint BIT_NUM_SIZE = 7;
    static constexpr uint BIT_NUM_SIGN = std::numeric_limits<uint>::digits - BIT_NUM_SIZE;
    static constexpr uint MAX_CUT_SIZE = (1 << BIT_NUM_SIZE) - 1;
    static constexpr uint SIGN_MASK    = BIT_NUM_SIGN - 1;
    static constexpr uint R            = 2;

    uint size : BIT_NUM_SIZE;
    uint sign : BIT_NUM_SIGN;
    uint leaves[R];

    Cut(uint32_t *begin, uint32_t *end, Sign s) : size(end - begin), sign(s) {
        std::memcpy(leaves, begin, size);
    }

    Cut(uint i0, uint i1) : size(2), sign(0) {
        
    }

    Cut *next() const {
        if constexpr (R != 0) {
            const int extra_size = R >= size ? 0 : (size - R);
            assert(extra_size >= 0);
            return reinterpret_cast<Cut *>(
                reinterpret_cast<uintptr_t>(this) + sizeof(Cut) + sizeof(uint) * extra_size
            );
        } else {
            return reinterpret_cast<Cut *>(
                reinterpret_cast<uintptr_t>(this) + sizeof(Cut) + sizeof(uint) * size
            );
        }
    }
};

// #define create_bicut(lit0, lit1) allocate<Cut>(2, lit0 >> 1, lit1 >> 1)

// #define TOSIGN(id) (1u << (id))

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
        Lit         _fanins[2]{0};
    public:
        node_t()                                 : _size(0), _type(node_type_t::NONE)     {}
        node_t(node_type_t type)                 : _size(0), _type(type)                  {}
        node_t(node_type_t type, Lit f0)         : _size(1), _type(type), _fanins{f0, 0}  {}
        node_t(node_type_t type, Lit f0, Lit f1) : _size(2), _type(type), _fanins{f0, f1} {}
       ~node_t() = default;

        uint        size()         const { return _size;                }
        // Lit         lit(graph_t &g) const { return this - &g._nodes[0];  }
        Lit         fanin(uint i)   const { return _fanins[i];           }
        node_type_t type()  const { return _type;                }

        Lit         operator[](uint i) const { return _fanins[i]; }

        bool is_logic() const { return _type == node_type_t::LOGIC; }
        bool is_pi   () const { return _type == node_type_t::PI;    }
        bool is_po   () const { return _type == node_type_t::PO;    }

       friend class graph_t;
    };

protected:
    std::vector<node_t>   _nodes;
    std::vector<node_t *> _pi;
    std::vector<node_t *> _po;

public:
    graph_t(uint max_node_num, uint num_pi = 0, uint num_po = 0) {
        _nodes.reserve(max_node_num);
        if (num_pi)
            _pi.reserve(num_pi);
        if (num_po)
            _po.reserve(num_po);
    }

    graph_t(uint64_t num_pin) {

    }

    ~graph_t() = default;
    
    void add_node(node_type_t type, Lit f1 = 0, Lit f2 = 0) {
        node_t &node = _nodes.emplace_back(type, f1, f2);
        if (type == node_type_t::PI)
            _pi.push_back(&node);
        else if (type == node_type_t::PO)
            _po.push_back(&node);
    }

    uint num_nodes() const { return _nodes.size(); }
    uint num_po()    const { return _po.size();    }
    uint num_pi()    const { return _pi.size();    }

    int begin()  const { return 0;                  }
    int end()    const { return _nodes.size();      }
    int rbegin() const { return _nodes.size() - 1;  }
    int rend()   const { return -1;                 }

    const node_t &operator[](uint i) { return _nodes[i]; }
};

class enumerate_cut;

class mapping_config {
public:
    uint cut_size;
    uint lut_size;
    uint prune_mode;
    uint opt_target;
};

class mapper : public graph_t {
    std::vector<uint>  _int_ref;
    std::vector<uint>  _num_ref;
    std::vector<Area>  _area;  
    std::vector<Edge>  _edge;
    std::vector<Time>  _arrival;
    std::vector<Cut *> _best_cut;
    std::vector<std::vector<Cut *>> _cuts;
    
public:
    friend class enumerate_cut;

    mapper(uint max_node_num, uint num_pi = 0, uint num_po = 0)
    : graph_t(max_node_num, num_pi, num_po)
    {
        _int_ref .resize(max_node_num, 0);
        _num_ref .resize(max_node_num, 0);
        _area    .resize(max_node_num, 0);
        _edge    .resize(max_node_num, 0);
        _arrival .resize(max_node_num, 0);
        _cuts    .resize(max_node_num, {});
        _best_cut.resize(max_node_num, nullptr);
    }
    ~mapper() = default;

    std::vector<Cut *> &cut_set(Lit n) { return _cuts[n]; }

    Area &area(Lit n)    { return _area[n]; }
    Edge &edge(Lit n)    { return _edge[n]; }
    Time  arrival(Lit n) { return _arrival[n]; }
        

};

struct cut_cost_t {
    Area area;
    Edge edge;
    Time arr;
    uint idx;
};

// normal enumerate cut way
class enumerate_cut {
    mapper &_mgr;
public:
    enumerate_cut(mapper &mgr) : _mgr(mgr) {}
    ~enumerate_cut() = default;

    void operator()(uint id) {
        const auto &node = _mgr[id];
        if (!node.is_logic()) [[unlikely]]
            return;

        std::vector<Cut *> &cuts = _mgr.cut_set(id);
        Lit f0 = node[0];
        Lit f1 = node[1];

        Cut *bicut = allocate<Cut>(2, Lit2Var(f0), Lit2Var(f1));

        for (int i )



    }
};

class mapping_flow
{
public:
    mapping_flow() = default;
   ~mapping_flow() = default;

    std::string_view name() const { return ""; }

    virtual void premap()  {}
    virtual void postmap() {}

    virtual void forward(mapper &)  {}
    virtual void backward(mapper &) {}
};

// generic area-flow mapping algo
class area_flow : public mapping_flow
{
public:
    area_flow() = default;
   ~area_flow() = default;

    virtual std::string_view name() const { return ""; }

    virtual void premap() {}
    virtual void postmap() {}
    virtual void forward(mapper &mgr)  {
    }
    virtual void backward(mapper &) {}
};



}
