/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set expandtab tabstop=2 softtabstop=2 shiftwidth=2: */

/* head file for LUT mapping */

#ifndef _SYN_MAP_H_
#define _SYN_MAP_H_

#include "syn/syn_info.h"
#include "syn/optimize.h"

namespace SYN {

  class Map;

  class CutSolution {
  public:
    CutSolution() : _area(min_area), _delay(min_delay), _level(0) {}
    CutSolution(Area area, Delay dly, int lev) 
      : _area(area), _delay(dly), _level(lev) {}
    ~CutSolution() {}
    
    CutSolution& operator=(const CutSolution& rhs) {
      _area = rhs._area;
      _delay = rhs._delay;
      _level = rhs._level;
      return *this;
    }
    enum REL {PRE=0, SUCC, DOM_L, DOM_R, SAME};
    //PRE: left before right
    //SUCC: left after right
    //DOM_L: left dominate right
    //DOM_R: right dominate left
    //SAME: left same as right
    REL compare(const CutSolution& rhs, Area delta=0.0) const {
      //how to consider precision?
      int aa;
      Area cmp = _area - rhs._area;
      if (cmp < -delta) aa = -1;
      else if (cmp > delta) aa = 1;
      else aa = 0;

      int dd = (_delay - rhs._delay);
      if (aa < 0) {
        if (dd <= 0) return DOM_L;
        else return PRE;
      } else if (aa == 0) {
        if (dd == 0) return SAME;
        else if (dd < 0) return DOM_L;
        else return DOM_R;
      } else {
        if (dd < 0) return SUCC;
        else return DOM_R;
      }
    }

    Area getArea() const {return _area;}
    Delay getDelay() const {return _delay;}
    int getLevel() const {return _level;}
    void setArea(Area aa) {_area = aa;}
    void setDelay(Delay dd) {_delay = dd;}
    void setLevel(int ll) {_level = ll;}
    void incArea(Area aa) {_area += aa;}
    void incDelay(Delay dd) {_delay += dd;}
    void incLevel() {++ _level;}
    void updateDelay(Delay dd) {if (dd > _delay) _delay = dd;}
    void updateLevel(int ll) {if (ll > _level) _level = ll;}
  private:
    Area _area;
    Delay _delay;
    int _level;
  };
  
  //solution sort in area increasing order
  class CutCost {
  public:
    friend class Cut;
    CutCost() {}
    ~CutCost() {}

    void reset() {_curve.clear();}

    CutCost& operator=(const CutCost& rhs) {
      _curve = rhs._curve;
      return *this;
    }
    typedef std::list<CutSolution>::iterator ITER;

    enum REL {PRE=0, SUCC, DOM_L, DOM_R, SAME};

    REL compare(CutCost&);
    
    ITER begin() {return _curve.begin();}
    ITER end() {return _curve.end();}
    ITER middle() {
      ITER itr = begin();
      for (size_t i = 0; i < (getNumSol()>>1); ++i, ++itr) {}
      return itr;
    }

    CutSolution& minAreaSol() {return *_curve.begin();}
    CutSolution& minDelaySol() {return *_curve.rbegin();}
    CutSolution& getSolForDelay(Delay target);
    Area getAreaForDelay(Delay target) {return getSolForDelay(target).getArea();}

    void insert(const CutSolution&);

    size_t getNumSol() const {return _curve.size();}
    void prune(int n);
  private:
    std::list<CutSolution> _curve;
  };

  class Cut {
    friend class Map;
  public:
    Cut(nimbus::DataModel::Pin* pin) : _root_pin(pin), _trivial(false) {}
    ~Cut() {}
    
    void reset() {
      _cut_inputs.clear();
      _cost.reset();
      _muxf7_cost.reset();
      _muxf8_cost.reset();
      _trivial = false;
    }
    Cut& operator=(const Cut& rhs) {
      _cost = rhs._cost;
      _muxf7_cost = rhs._muxf7_cost;
      _muxf8_cost = rhs._muxf8_cost;
      _trivial = rhs._trivial;
      return *this;
    }
    typedef std::set<nimbus::DataModel::Pin*, indexCmp>::iterator ITER;
    ITER begin() {return _cut_inputs.begin();}
    ITER end() {return _cut_inputs.end();}
    size_t getCutSize() const {return _cut_inputs.size();}
    void combine(Cut* cut) {
      for (ITER itr = cut->begin(); itr != cut->end(); ++itr) {
        nimbus::DataModel::Pin* drv = *itr;
        _cut_inputs.insert(drv);
      }
    }
    void addInput(nimbus::DataModel::Pin* drv) {_cut_inputs.insert(drv);}
    bool isCutInput(nimbus::DataModel::Pin* drv) {return _cut_inputs.find(drv) != end();}

    bool isRedundant(Cut* rhs) {
      //check if this cut is redundant wrt rhs, ie, it includes rhs
      for (ITER itr = rhs->begin(); itr != rhs->end(); ++itr) {
        nimbus::DataModel::Pin* drv = *itr;
        if (!included(drv)) return false;
      }
      return true;
    }
    CutCost::REL compare(Cut* rhs) {
      return _cost.compare(rhs->_cost);
    }
    void combineSol(Map*, nimbus::DataModel::Pin*, Cut*, int, nimbus::DataModel::Pin*);
    void solAdjust(Area, Delay, int);
    size_t getNumSol() const {return _cost.getNumSol();}
    CutCost::ITER solBegin() {return _cost.begin();}
    CutCost::ITER solEnd() {return _cost.end();}
    CutSolution& minAreaSol() {return _cost.minAreaSol();}
    CutSolution& minDelaySol() {return _cost.minDelaySol();}
    CutSolution& getSolForDelay(Delay target) {return _cost.getSolForDelay(target);}
    void insertSol(const CutSolution& sol) {_cost.insert(sol);}
    void pruneSol() {_cost.prune(getSolBound());}

    bool isTrivial() const {return _trivial;}
    void setTrivial() {_trivial = true;}
    bool isBaseCut() {
      if (!_root_pin->isInpin() && _root_pin->gate()) {
        nimbus::DataModel::Gate* gate = _root_pin->gate();
        if (getCutSize() != gate->getNumIn())
          return false;
        for (size_t i = 0; i < gate->getNumIn(); ++i) {
          nimbus::DataModel::Pin* drv = gate->getInpin(i)->net()->source();
          if (!isCutInput(drv))
            return false;
        }
        return true;
      } else {
        return false;
      }
    }
    
    Delay getMinDelay() {
      CutSolution& sol = _cost.minDelaySol();
      return sol.getDelay();
    }

    CutCost::REL compareMuxf7Sol(Cut* rhs) {
      return _muxf7_cost.compare(rhs->_muxf7_cost);
    }
    size_t getNumMuxf7Sol() const {return _muxf7_cost.getNumSol();}
    CutCost::ITER muxf7SolBegin() {return _muxf7_cost.begin();}
    CutCost::ITER muxf7SolEnd() {return _muxf7_cost.end();}
    CutSolution& minAreaMuxf7Sol() {return _muxf7_cost.minAreaSol();}
    CutSolution& minDelayMuxf7Sol() {return _muxf7_cost.minDelaySol();}
    CutSolution& getMuxf7SolForDelay(Delay target) {return _muxf7_cost.getSolForDelay(target);}
    void insertMuxf7Sol(const CutSolution& sol) {_muxf7_cost.insert(sol);}
    void pruneMuxf7Sol() {_muxf7_cost.prune(getSolBound());}
    Delay getMuxf7SolMinDelay() {
      CutSolution& sol = _muxf7_cost.minDelaySol();
      return sol.getDelay();
    }

    CutCost::REL compareMuxf8Sol(Cut* rhs) {
      return _muxf8_cost.compare(rhs->_muxf8_cost);
    }
    size_t getNumMuxf8Sol() const {return _muxf8_cost.getNumSol();}
    CutCost::ITER muxf8SolBegin() {return _muxf8_cost.begin();}
    CutCost::ITER muxf8SolEnd() {return _muxf8_cost.end();}
    CutSolution& minAreaMuxf8Sol() {return _muxf8_cost.minAreaSol();}
    CutSolution& minDelayMuxf8Sol() {return _muxf8_cost.minDelaySol();}
    CutSolution& getMuxf8SolForDelay(Delay target) {return _muxf8_cost.getSolForDelay(target);}
    void insertMuxf8Sol(const CutSolution& sol) {_muxf8_cost.insert(sol);}
    void pruneMuxf8Sol() {_muxf8_cost.prune(getSolBound());}
    Delay getMuxf8SolMinDelay() {
      CutSolution& sol = _muxf8_cost.minDelaySol();
      return sol.getDelay();
    }

  private:
    nimbus::DataModel::Pin* _root_pin;
    std::set<nimbus::DataModel::Pin*, indexCmp> _cut_inputs;
    CutCost _cost;
    CutCost _muxf7_cost; // cost curve for gate viewed as muxf7
    CutCost _muxf8_cost; // cost curve for gate viewed as muxf8
    bool _trivial;
    int getSolBound() const {return 10;}
    
    bool included(nimbus::DataModel::Pin* drv) {return _cut_inputs.find(drv) != end();}
  };

  class Map : public Opt {
  public:
    friend class Cut;
    Map(nimbus::DataModel::Model* top, int k, bool muxf) : Opt(top), _in_bound(k) , _muxf(muxf), _sta(false) {}
    ~Map() {reset();}
    void reset() {
      for (std::unordered_map<nimbus::DataModel::Pin*, std::list<Cut*> >::iterator itr = _pin_cut_map.begin();
           itr != _pin_cut_map.end(); ++itr) {
        for (std::list<Cut*>::iterator citr = itr->second.begin(); citr != itr->second.end(); ++citr) {
          delete *citr;
        }
      }
      _pin_cut_map.clear();
      _pin_cut.clear();
      _pin_root_mark.clear();
    }
    void map(bool resyn);
    enum InstType {NONE, LUT, MUXF7, MUXF8, UNKNOWN};
    bool with_muxf() const {return _muxf;}
    
  private:
    size_t _in_bound;
    bool _muxf;
    bool _sta;

    std::unordered_map<nimbus::DataModel::Pin*, std::list<Cut*>> _pin_cut_map;
    std::unordered_map<nimbus::DataModel::Pin*, Cut*> _pin_cut;
    std::unordered_map<nimbus::DataModel::Pin*, InstType> _pin_root_mark;

    size_t getNumIn(nimbus::DataModel::Element* elem) const {return getNumPin(elem, nimbus::DataModel::kPortInput);}
    size_t getNumOut(nimbus::DataModel::Element* elem) const {return getNumPin(elem, nimbus::DataModel::kPortOutput);}
    size_t getNumInout(nimbus::DataModel::Element* elem) const {return getNumPin(elem, nimbus::DataModel::kPortInout);}
    size_t getNumPin(nimbus::DataModel::Element* elem, nimbus::DataModel::PortDir dir) const {
      size_t n = 0;
      for (nimbus::DataModel::Element::PinIter itr = elem->begin(); itr != elem->end(); ++itr) {
        if ((*itr)->dir() == dir) ++ n;
      }
      return n;
    }
    /* should set according hdl_lib */
    size_t getInBound() {return _in_bound;}
    size_t getCutLimit() {return 30;}
    void setStaBased() {_sta = true;}
    bool isStaBased() const {return _sta;}
    Delay getLutDelay() {
      if (_sta)
        return 43;
      else
        return 90;
    }
    Delay getEdgeDelay() {
      if (_sta)
        return 280;
      else
        return 500;
    }
    Area getLutArea() {return 1;}
    Delay getCutDelay(Cut* cut, nimbus::DataModel::Pin* input_pin) {
      nimbus::DataModel::Inst* inst = cut->_root_pin->inst();
      if (inst && cut->_root_pin->net() != input_pin->net()) {
        /* _root_pin == input_pin means the trivial cut of inst */
        nimbus::DataModel::Pin* ipin = 0;
        Delay max_arc_dly = 0;
        //nimbus_assert(cut->getCutSize() == getNumIn(inst));
        for (nimbus::DataModel::Element::PinIter itr = inst->begin(); itr != inst->end(); ++itr) {
          ipin = *itr;
          if (ipin->net() != input_pin->net()) continue;
          Delay arc_dly = inst->arcDelay(ipin, cut->_root_pin);
          if (max_arc_dly < arc_dly) max_arc_dly = arc_dly;
        }
        return max_arc_dly;
      } else {
        return cut->getCutSize() > _in_bound? static_cast<Delay>(getLutDelay() * 2.5) : getLutDelay();
      }
    }

    // TODO support MUXF6(V4)/MUXF8(V7)
    // edgeDelay(LUT6->MUXF7): 11
    // arcDelay(MUXF7/I,MUXF7/O): 95
    // arcDelay(MUXF7/S,MUXF7/O): 146
    Delay getMuxf7Delay() {
      if (_sta)
        return 146;
      else
        return 300;
    };

    // edgeDelay(MUXF7->MUXF8): 0
    // arcDelay(MUXF8/I,MUXF8/O): 43
    // arcDelay(MUXF8/S,MUXF8/O): 139
    Delay getMuxf8Delay() {
      if (_sta)
        return 139;
      else
        return 280;
    };

    Area getCutArea(Cut* cut, nimbus::DataModel::Pin* input_pin) {
      nimbus::DataModel::Inst* inst = cut->_root_pin->inst();
      if (inst && cut->_root_pin->net() != input_pin->net())
        /* _root_pin == input_pin means the trivial cut of inst */
        return inst->getArea();
      else
        return cut->getCutSize() <= _in_bound? getLutArea() : 
          getLutArea() * static_cast<Area>(2.5);
    }
    Area getMuxf7Area() {return 0;}
    Area getMuxf8Area() {return 0;}

    typedef std::list<Cut*>::iterator CUT_ITER;

    void cutEnu(nimbus::DataModel::Pin*);
    Cut* cutSel(nimbus::DataModel::Pin*, InstType);
    void lutGen(nimbus::DataModel::Pin*);
    bool isLutRoot(nimbus::DataModel::Pin* pin) {return _pin_root_mark[pin] == LUT;}
    bool isMuxf7Root(nimbus::DataModel::Pin* pin) {return _pin_root_mark[pin] == MUXF7;}
    bool isMuxf8Root(nimbus::DataModel::Pin* pin) {return _pin_root_mark[pin] == MUXF8;}
    bool isUnknownRoot(nimbus::DataModel::Pin* pin) {return _pin_root_mark[pin] == UNKNOWN;}
    void setRootType(nimbus::DataModel::Pin* pin, InstType type) {
      _pin_root_mark[pin] = type;
    }
    void selCut(nimbus::DataModel::Pin* pin, Cut* cut) {_pin_cut[pin] = cut;}
    Cut* getCut(nimbus::DataModel::Pin* pin) {return _pin_cut[pin];}

    //void map(Pin*);
    bool nextCut(std::vector<nimbus::DataModel::Pin*>&, std::vector<bool>&, std::vector<CUT_ITER>&);

    CUT_ITER cutBegin(nimbus::DataModel::Pin* pin) {return _pin_cut_map[pin].begin();}
    CUT_ITER cutEnd(nimbus::DataModel::Pin* pin) {return _pin_cut_map[pin].end();}
    size_t getNumCut(nimbus::DataModel::Pin* pin) {return _pin_cut_map[pin].size();}
    bool insertCut(nimbus::DataModel::Pin* pin, Cut* cut);//return true if inserted
    void insertCut(nimbus::DataModel::Pin* pin, CUT_ITER itr, Cut* cut) {
      _pin_cut_map[pin].insert(itr, cut);
    }
    void pushFrontCut(nimbus::DataModel::Pin* pin, Cut* cut) {
      _pin_cut_map[pin].insert(cutBegin(pin), cut);
    }
    void eraseCut(nimbus::DataModel::Pin* pin, CUT_ITER itr) {
      Cut* cut = *itr;
      _pin_cut_map[pin].erase(itr);
      delete cut;
    }
    void removeCut(nimbus::DataModel::Pin* pin, Cut* cut) {
      _pin_cut_map[pin].remove(cut);
      delete cut;
    }
    void printCut(nimbus::DataModel::Pin* pin);
    void printCut(Cut* cut);
    bool isRedundant(nimbus::DataModel::Pin* pin, Cut* cut) {
      for (CUT_ITER itr = cutBegin(pin); itr != cutEnd(pin); ++itr) {
        Cut* old_cut = *itr;
        if (cut->isRedundant(old_cut)) return true;
      }
      return false;
    }
    void shannonDecomp(nimbus::DataModel::Gate*);
  };
}
#endif //_SYN_MAP_H_
