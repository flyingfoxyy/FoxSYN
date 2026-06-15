/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set expandtab tabstop=2 softtabstop=2 shiftwidth=2: */
//============================================================================
// Copyright (c) 2014, All Right Reserved, FMSH
//
// file:      map.cc
// author:    wuchang
// purpose:   technology mapping
// revision history:
// 2014/11/03 initial version
//============================================================================

#include "utility/define_names.h"
#include "syn/map.h"
#include "syn/optimize.h"
#include "syn/abc.h"
//#include "syn/restruct/restruct.h"
#include "timing2/timing.h"
#include "syn/syn_timing.h"
#include "messages/syn_msg_ids.h"

using namespace nimbus;

namespace SYN {

  void Opt::preserveMux() {
    std::vector<DataModel::Model*> mod_vec;
    top_->bottomupModel(mod_vec);
    for (size_t i = 0; i < mod_vec.size(); ++i)
      if (isWorkLibModel(mod_vec[i]) && mod_vec[i]->num_insts() > 1)
        preserveMux(mod_vec[i]);
  }

  void Opt::preserveMux(DataModel::Model * model) {
    if (model == NULL) return;
    std::vector<DataModel::Pin*> opin_vec;
    model->topoOrder(opin_vec);
    for (int i = (int)opin_vec.size() - 1; i >= 0; --i) {
      DataModel::Pin* opin = opin_vec[i];
      if (!opin->owner() || opin->owner()->preserve()) continue;
      if (opin->is_gate_pin()) {
        DataModel::Gate* gate = opin->gate();
        std::vector<DataModel::Gate *> muxs;
        getRelatedMux2(gate, muxs);
        if (muxs.size() > 5) {
          gate->set_preserve(true);
          for (size_t i = 0; i < muxs.size(); i++) {
            muxs[i]->set_preserve(true);
          }
        }
      }
    }
  }

  void Opt::getRelatedMux2(DataModel::Gate *gate, std::vector<DataModel::Gate *> &muxs) {
    if (gate->preserve()) return;
    size_t selIdx = -1;
    size_t trueIdx = -1;
    size_t falseIdx = -1;
    if (gate->isMux2(selIdx, trueIdx, falseIdx)) {
      DataModel::Pin * pin0 = gate->getInpin(falseIdx);
      DataModel::Pin * pin1 = gate->getInpin(trueIdx);
      pin0 = (pin0 && pin0->net()) ? pin0->net()->source() : nullptr; 
      pin1 = (pin1 && pin1->net()) ? pin1->net()->source() : nullptr; 
      if (pin0 && pin1 && pin0->is_gate_pin() && pin1->is_gate_pin()) {
        DataModel::Gate * gate0 = pin0->gate();
        DataModel::Gate * gate1 = pin1->gate();
        size_t selIdx = -1;
        size_t trueIdx = -1;
        size_t falseIdx = -1;
        if (gate0->isMux2(selIdx, trueIdx, falseIdx) && gate1->isMux2(selIdx, trueIdx, falseIdx)) {
          getRelatedMux2(gate0, muxs);
          getRelatedMux2(gate1, muxs);
          muxs.push_back(gate0);
          muxs.push_back(gate1);
        }
      }
    }
  }

  void Opt::unpreserveMux() {
    std::vector<DataModel::Model*> mod_vec;
    top_->bottomupModel(mod_vec);
    for (size_t i = 0; i < mod_vec.size(); ++i)
      if (isWorkLibModel(mod_vec[i]) && mod_vec[i]->num_insts() > 1)
        unpreserveMux(mod_vec[i]);
  }

  void Opt::unpreserveMux(DataModel::Model * model) {
    if (model == NULL) return;
    for (DataModel::ModelElemIter elemIter = model->element_begin();
         elemIter != model->element_end(); ++elemIter) {
      DataModel::Element* elem = *elemIter;
      if (!elem->isGate()) continue;
      DataModel::Gate* gate = static_cast<DataModel::Gate*>(elem);
      if (gate->preserve()) {
        gate->set_preserve(false);
      }
    }
  }

  void Opt::map(int k, bool with_muxf, bool resyn) {
    std::vector<nimbus::DataModel::Model*> mod_vec;
    top_->bottomupModel(mod_vec);
    for (size_t i = 0; i < mod_vec.size(); ++i) {
      if (isWorkLibModel(mod_vec[i]) && mod_vec[i]->num_insts() > 0) {
        Map mapper(mod_vec[i], k, with_muxf);
        mapper.map(resyn);
      }
    }
  }
  
  void CutCost::insert(const CutSolution& sol) {
    ITER itr_next;
    for (ITER itr = begin(); itr != end(); itr = itr_next) {
      itr_next = itr;
      ++itr_next;
      CutSolution& tmp = *itr;
      CutSolution::REL rel = sol.compare(tmp);
      switch (rel) {
      case CutSolution::PRE:
        _curve.insert(itr, sol);
        return;
      case CutSolution::SUCC:
        //continue to compare
        break;
      case CutSolution::DOM_L:
        _curve.erase(itr);
        break;
      case CutSolution::DOM_R:
        return;
      case CutSolution::SAME:
        return;
      }
    }
    _curve.push_back(sol);
  }

  CutCost::REL CutCost::compare(CutCost& rhs) {
    CutSolution& sol_area = minAreaSol();
    CutSolution& sol_dly = minDelaySol();
    CutSolution& rhs_area = rhs.minAreaSol();
    CutSolution& rhs_dly = rhs.minDelaySol();
    CutSolution& sol_mid = *middle();
    CutSolution& rhs_mid = *rhs.middle();

    CutSolution::REL rel_area = sol_area.compare(rhs_area);
    CutSolution::REL rel_dly = sol_dly.compare(rhs_dly);
    CutSolution::REL rel_mid = sol_mid.compare(rhs_mid);

    int flag = 0;
    REL rel = PRE;
    //10: this is dominated by rhs
    //01: rhs is dominated by this
    //00: no dominance relationship
    //11: qor is the same
    if ((rel_area == CutSolution::DOM_L || rel_area == CutSolution::SAME) &&
        (rel_dly == CutSolution::DOM_L || rel_dly == CutSolution::SAME) &&
        (rel_mid == CutSolution::DOM_L || rel_mid == CutSolution::SAME)) {
      flag |= 2;
    }
    if ((rel_area == CutSolution::DOM_R || rel_area == CutSolution::SAME) &&
        (rel_dly == CutSolution::DOM_R || rel_dly == CutSolution::SAME) &&
        (rel_mid == CutSolution::DOM_R || rel_mid == CutSolution::SAME)) {
      flag |= 1;
    }
    switch (flag) {
    case 2: rel = DOM_L; break;
    case 1: rel = DOM_R; break;
    case 3: rel = SAME; break;
    default:
      if (sol_dly.getDelay() <= rhs_dly.getDelay()) rel = PRE;
      else rel = SUCC;
      break;
    }
    return rel;
  }

  void CutCost::prune(int nsol) {
    int step = static_cast<int>(getNumSol()) / nsol + 1;
    if (step < 2) return;
    int i = 0;
    ITER itr_next;
    for (ITER itr = begin(); itr != end(); itr = itr_next, ++i) {
      itr_next = itr;
      ++ itr_next;
      if (i > 0 && i < static_cast<int>(getNumSol()-1) && (i % step) != 0) {
        _curve.erase(itr);
      }
    }
  }

  CutSolution& CutCost::getSolForDelay(Delay target) {
    for (ITER itr = begin(); itr != end(); ++itr) {
      if (itr->getDelay() <= target) return *itr;
    }
    return minDelaySol();
  }

  void Cut::combineSol(Map* mapper, nimbus::DataModel::Pin* root, Cut* subcut, int fanout, nimbus::DataModel::Pin* input_pin) {
    // combine normal solution. For gate viewed as normal, any cut could combine any type of solution
    if (getNumSol() == 0) {
      CutSolution sol(mapper->getCutArea(this, input_pin), 
                      mapper->getCutDelay(this, input_pin), 1);
      _cost.insert(sol);
    }
    CutCost newcost;
    for (CutCost::ITER itr = _cost.begin(); itr != _cost.end(); ++itr) {
      for (CutCost::ITER sub_itr = subcut->_cost.begin(); sub_itr != subcut->_cost.end(); ++sub_itr) {
        CutSolution newsol = *itr;
        newsol.incArea((sub_itr->getArea() - mapper->getCutArea(subcut, input_pin)) / (Area)fanout);
        newsol.updateDelay(sub_itr->getDelay() - mapper->getCutDelay(subcut, input_pin) + mapper->getCutDelay(this, input_pin));
        newsol.updateLevel(sub_itr->getLevel());
        newcost.insert(newsol);
      }
    }
    newcost.prune(2*getSolBound());
    _cost = newcost;

    if (!mapper->with_muxf()) {
     return; 
    }

    bool isGateMuxf = false;
    DataModel::Gate* gate;
    size_t selIdx = -1;
    size_t trueIdx = -1;
    size_t falseIdx = -1;
    if (!root->isInpin() && root->gate()) {
      gate = root->gate();
      if (gate->isMuxf(selIdx, trueIdx, falseIdx)) {
        isGateMuxf = true;
      }
    }

    // only solutions of base cut could be combined when viewed as muxf7/muxf8
    if (isGateMuxf && isBaseCut()) {
      DataModel::Pin* trueIdxPin = gate->getInpin(trueIdx);
      DataModel::Pin* falseIdxPin = gate->getInpin(falseIdx);
      DataModel::Pin* selIdxPin = gate->getInpin(selIdx);
      DataModel::Pin* trueIdxDrv = trueIdxPin->net()->source();
      DataModel::Pin* falseIdxDrv = falseIdxPin->net()->source();
      DataModel::Pin* selIdxDrv = selIdxPin->net()->source();
      Cut* trueIdxCut = 0;
      Cut* falseIdxCut = 0;
      Cut* selIdxCut = 0;
      for (Map::CUT_ITER itr = mapper->cutBegin(trueIdxDrv); itr != mapper->cutEnd(trueIdxDrv); ++itr) {
        if ((*itr)->isTrivial()) {
          trueIdxCut = *itr;
        }
      }
      for (Map::CUT_ITER itr = mapper->cutBegin(falseIdxDrv); itr != mapper->cutEnd(falseIdxDrv); ++itr) {
        if ((*itr)->isTrivial()) {
          falseIdxCut = *itr;
        }
      }
      for (Map::CUT_ITER itr = mapper->cutBegin(selIdxDrv); itr != mapper->cutEnd(selIdxDrv); ++itr) {
        if ((*itr)->isTrivial()) {
          selIdxCut = *itr;
        }
      }
      if (!subcut->isTrivial()) {
        nimbus_error(IDS_SYN_0031);
        throw std::runtime_error("Run-time error.\n");
      }

      Delay muxf7LvlDly = mapper->getMuxf7Delay(); // reduce an edgeDelay since delay(LUT->muxf7) = 0
      Delay muxf8LvlDly = mapper->getMuxf8Delay(); // reduce an edgeDelay since delay(muxf7->muxf8) = 0
      // combine muxf7 solution. muxf7 cut could only combine normal solutions if both data port drive pins have normal solutions
      if (trueIdxCut->getNumSol() > 0 && falseIdxCut->getNumSol() > 0) {
        if (getNumMuxf7Sol() == 0) {
          CutSolution sol(mapper->getCutArea(this, input_pin), 
                          mapper->getCutDelay(this, input_pin), 1);
          _muxf7_cost.insert(sol);
        }
        // combine solution with data port drive 
        if ((*(subcut->begin()) == *(trueIdxCut->begin())) || (*(subcut->begin()) == *(falseIdxCut->begin()))) {
          CutCost new_muxf7_cost;
          for (CutCost::ITER itr = _muxf7_cost.begin(); itr != _muxf7_cost.end(); ++itr) {
            for (CutCost::ITER sub_itr = subcut->_cost.begin(); sub_itr != subcut->_cost.end(); ++sub_itr) {
              CutSolution newsol = *itr;
              // TODO fine tune area increment compensation and delay cost for muxf7
              //newsol.incArea((sub_itr->getArea() - mapper->getCutArea(subcut, input_pin)) / (Area)fanout);
              newsol.incArea(sub_itr->getArea() - mapper->getCutArea(subcut, input_pin));
              //newsol.updateDelay(sub_itr->getDelay() - mapper->getCutDelay(subcut, input_pin) + mapper->getCutDelay(this, input_pin));
              newsol.updateDelay(sub_itr->getDelay() - mapper->getCutDelay(subcut, input_pin) - mapper->getEdgeDelay() + muxf7LvlDly);
              newsol.updateLevel(sub_itr->getLevel());
              new_muxf7_cost.insert(newsol);
            }
          }
          new_muxf7_cost.prune(2*getSolBound());
          _muxf7_cost = new_muxf7_cost;
        // combine solution with selection port drive
        } else if (*(subcut->begin()) == *(selIdxCut->begin())) {
          CutCost new_muxf7_cost;
          for (CutCost::ITER itr = _muxf7_cost.begin(); itr != _muxf7_cost.end(); ++itr) {
            for (CutCost::ITER sub_itr = subcut->_cost.begin(); sub_itr != subcut->_cost.end(); ++sub_itr) {
              CutSolution newsol = *itr;
              // TODO fine tune area increment compensation and delay cost for muxf7
              //newsol.incArea((sub_itr->getArea() - mapper->getCutArea(subcut, input_pin)) / (Area)fanout);
              newsol.incArea(sub_itr->getArea() - mapper->getCutArea(subcut, input_pin));
              //newsol.updateDelay(sub_itr->getDelay() - mapper->getCutDelay(subcut, input_pin) + mapper->getCutDelay(this, input_pin));
              newsol.updateDelay(sub_itr->getDelay() - mapper->getCutDelay(subcut, input_pin) + muxf7LvlDly);
              newsol.updateLevel(sub_itr->getLevel());
              new_muxf7_cost.insert(newsol);
            }
            for (CutCost::ITER sub_itr = subcut->_muxf7_cost.begin(); sub_itr != subcut->_muxf7_cost.end(); ++sub_itr) {
              CutSolution newsol = *itr;
              // TODO fine tune area increment compensation and delay cost for muxf7
              //newsol.incArea((sub_itr->getArea() - mapper->getCutArea(subcut, input_pin)) / (Area)fanout);
              newsol.incArea(sub_itr->getArea() - mapper->getCutArea(subcut, input_pin));
              //newsol.updateDelay(sub_itr->getDelay() - mapper->getCutDelay(subcut, input_pin) + mapper->getCutDelay(this, input_pin));
              newsol.updateDelay(sub_itr->getDelay() - mapper->getCutDelay(subcut, input_pin) + muxf7LvlDly);
              newsol.updateLevel(sub_itr->getLevel());
              new_muxf7_cost.insert(newsol);
            }
            for (CutCost::ITER sub_itr = subcut->_muxf8_cost.begin(); sub_itr != subcut->_muxf8_cost.end(); ++sub_itr) {
              CutSolution newsol = *itr;
              // TODO fine tune area increment compensation and delay cost for muxf7
              //newsol.incArea((sub_itr->getArea() - mapper->getCutArea(subcut, input_pin)) / (Area)fanout);
              newsol.incArea(sub_itr->getArea() - mapper->getCutArea(subcut, input_pin));
              //newsol.updateDelay(sub_itr->getDelay() - mapper->getCutDelay(subcut, input_pin) + mapper->getCutDelay(this, input_pin));
              newsol.updateDelay(sub_itr->getDelay() - mapper->getCutDelay(subcut, input_pin) + muxf7LvlDly);
              newsol.updateLevel(sub_itr->getLevel());
              new_muxf7_cost.insert(newsol);
            }
          }
          new_muxf7_cost.prune(2*getSolBound());
          _muxf7_cost = new_muxf7_cost;
        } else {
          nimbus_error(IDS_SYN_0031);
          throw std::runtime_error("Run-time error.\n");
        }
      }

      // combine muxf8 solution. muxf8 cut could only combine muxf7 solutions if both data port drive pins have muxf7 solutions
      if (trueIdxCut->getNumMuxf7Sol() > 0 && falseIdxCut->getNumMuxf7Sol() > 0) {
        if (getNumMuxf8Sol() == 0) {
          CutSolution sol(mapper->getCutArea(this, input_pin), 
                          mapper->getCutDelay(this, input_pin), 1);
          _muxf8_cost.insert(sol);
        }
        // combine solution with data port drive
        if ((*(subcut->begin()) == *(trueIdxCut->begin())) || (*(subcut->begin()) == *(falseIdxCut->begin()))) {
          CutCost new_muxf8_cost;
          for (CutCost::ITER itr = _muxf8_cost.begin(); itr != _muxf8_cost.end(); ++itr) {
            for (CutCost::ITER sub_itr = subcut->_muxf7_cost.begin(); sub_itr != subcut->_muxf7_cost.end(); ++sub_itr) {
              CutSolution newsol = *itr;
              // TODO fine tune area increment compensation and delay cost for muxf8
              //newsol.incArea((sub_itr->getArea() - mapper->getCutArea(subcut, input_pin)) / (Area)fanout);
              newsol.incArea(sub_itr->getArea() - mapper->getCutArea(subcut, input_pin));
              //newsol.updateDelay(sub_itr->getDelay() - mapper->getCutDelay(subcut, input_pin) + mapper->getCutDelay(this, input_pin));
              newsol.updateDelay(sub_itr->getDelay() - mapper->getCutDelay(subcut, input_pin) - mapper->getEdgeDelay() + muxf8LvlDly);
              newsol.updateLevel(sub_itr->getLevel());
              new_muxf8_cost.insert(newsol);
            }
          }
          new_muxf8_cost.prune(2*getSolBound());
          _muxf8_cost = new_muxf8_cost;
        // combine solution with selection port drive
        } else if (*(subcut->begin()) == *(selIdxCut->begin())) {
          CutCost new_muxf8_cost;
          for (CutCost::ITER itr = _muxf8_cost.begin(); itr != _muxf8_cost.end(); ++itr) {
            for (CutCost::ITER sub_itr = subcut->_cost.begin(); sub_itr != subcut->_cost.end(); ++sub_itr) {
              CutSolution newsol = *itr;
              // TODO fine tune area increment compensation and delay cost for muxf8
              //newsol.incArea((sub_itr->getArea() - mapper->getCutArea(subcut, input_pin)) / (Area)fanout);
              newsol.incArea(sub_itr->getArea() - mapper->getCutArea(subcut, input_pin));
              //newsol.updateDelay(sub_itr->getDelay() - mapper->getCutDelay(subcut, input_pin) + mapper->getCutDelay(this, input_pin));
              newsol.updateDelay(sub_itr->getDelay() - mapper->getCutDelay(subcut, input_pin) + muxf8LvlDly);
              newsol.updateLevel(sub_itr->getLevel());
              new_muxf8_cost.insert(newsol);
            }
            for (CutCost::ITER sub_itr = subcut->_muxf7_cost.begin(); sub_itr != subcut->_muxf7_cost.end(); ++sub_itr) {
              CutSolution newsol = *itr;
              // TODO fine tune area increment compensation and delay cost for muxf8
              //newsol.incArea((sub_itr->getArea() - mapper->getCutArea(subcut, input_pin)) / (Area)fanout);
              newsol.incArea(sub_itr->getArea() - mapper->getCutArea(subcut, input_pin));
              //newsol.updateDelay(sub_itr->getDelay() - mapper->getCutDelay(subcut, input_pin) + mapper->getCutDelay(this, input_pin));
              newsol.updateDelay(sub_itr->getDelay() - mapper->getCutDelay(subcut, input_pin) + muxf8LvlDly);
              newsol.updateLevel(sub_itr->getLevel());
              new_muxf8_cost.insert(newsol);
            }
            for (CutCost::ITER sub_itr = subcut->_muxf8_cost.begin(); sub_itr != subcut->_muxf8_cost.end(); ++sub_itr) {
              CutSolution newsol = *itr;
              // TODO fine tune area increment compensation and delay cost for muxf8
              //newsol.incArea((sub_itr->getArea() - mapper->getCutArea(subcut, input_pin)) / (Area)fanout);
              newsol.incArea(sub_itr->getArea() - mapper->getCutArea(subcut, input_pin));
              //newsol.updateDelay(sub_itr->getDelay() - mapper->getCutDelay(subcut, input_pin) + mapper->getCutDelay(this, input_pin));
              newsol.updateDelay(sub_itr->getDelay() - mapper->getCutDelay(subcut, input_pin) + muxf8LvlDly);
              newsol.updateLevel(sub_itr->getLevel());
              new_muxf8_cost.insert(newsol);
            }
          }
          new_muxf8_cost.prune(2*getSolBound());
          _muxf8_cost = new_muxf8_cost;
        } else {
          nimbus_error(IDS_SYN_0031);
          throw std::runtime_error("Run-time error.\n");
        }
      }
    }
  }

  void Cut::solAdjust(Area area, Delay delay, int lvl) {
    for (CutCost::ITER itr = solBegin(); itr != solEnd(); ++itr) {
      itr->setArea(itr->getArea() + area);
      itr->setDelay(itr->getDelay() + delay);
      itr->setLevel(itr->getLevel() + lvl);
    }
  }

  bool Map::nextCut(std::vector<DataModel::Pin*>& input_vec, std::vector<bool>& input_across,
                         std::vector<CUT_ITER>& iter_vec) {
    bool find_one = false;
    int index = 0;
    do {
      if (input_across[index]) {
        ++ iter_vec[index];
        if (iter_vec[index] == cutEnd(input_vec[index])) {
          iter_vec[index] = cutBegin(input_vec[index]);
          ++ index;
        } else find_one = true;
      } else {
        ++ index;
      }
    } while (!find_one && index < static_cast<int>(input_vec.size()));
    return find_one;
  }

  bool Map::insertCut(DataModel::Pin* pin, Cut* cut) {
    CUT_ITER itr_next;
    bool near_full = (getNumCut(pin) > (2*getCutLimit()/3));
    for (CUT_ITER itr = cutBegin(pin); itr != cutEnd(pin); itr = itr_next) {
      itr_next = itr;
      ++itr_next;
      Cut* ctmp = *itr;
      if (cut->isRedundant(ctmp)) {
        return false;
      } else if (ctmp->isRedundant(cut)) {
        eraseCut(pin, itr);
      } else {
        CutCost::REL rel = cut->compare(ctmp);
        switch (rel) {
        case CutCost::SAME:
          if (cut->getCutSize() < ctmp->getCutSize()) {
            eraseCut(pin, itr);
          } else {
            return false;
          }
          break;
        case CutCost::DOM_L:
          if (cut->getCutSize() <= ctmp->getCutSize() || near_full) {
            eraseCut(pin, itr);
          }
          break;
        case CutCost::DOM_R:
          if (cut->getCutSize() >= ctmp->getCutSize() || near_full) {
            return false;
          }
          break;
        default:
          break;
        }
      }
    }
    bool inserted = false;
    if (getNumCut(pin) < getCutLimit()) {
      insertCut(pin, cutEnd(pin), cut);
      inserted = true;
    }
    return inserted;
  }

  void Map::printCut(nimbus::DataModel::Pin* pin) {
    nimbus::Console::printInfo("  show cuts for pin: %s", pin->name());
    if (pin->owner()) {
      nimbus::Console::printInfo(" owner: %s", pin->owner()->name());
    }   
    nimbus::Console::printInfo("\n");
    for (CUT_ITER itr = cutBegin(pin); itr != cutEnd(pin); ++itr) {
      Cut* cut = *itr;
      printCut(cut);
    }   
  }

  void Map::printCut(Cut* cut) {
    nimbus::Console::printInfo("    ");
    for (Cut::ITER citr = cut->begin(); citr != cut->end(); ++citr) {
      nimbus::DataModel::Pin* drv = *citr;
      nimbus::Console::printInfo(" <%s>", drv->name());
      if (drv->owner()) {
        nimbus::Console::printInfo("(%s)", drv->owner()->name());
      }
    }   
    nimbus::Console::printInfo("\n");
    int n_sol = 0;
    msg(0, "root sol\n");
    for (CutCost::ITER itr = cut->solBegin(); itr != cut->solEnd(); ++itr) {
      msg(0, "        sol %d, delay %d level %d area %f\n", n_sol, itr->getDelay(), itr->getLevel(), itr->getArea());
      n_sol++;
    }
    msg(0, "muxf7 sol\n");
    n_sol = 0;
    for (CutCost::ITER itr = cut->muxf7SolBegin(); itr != cut->muxf7SolEnd(); ++itr) {
      msg(0, "        sol %d, delay %d level %d area %f\n", n_sol, itr->getDelay(), itr->getLevel(), itr->getArea());
      n_sol++;
    }
    msg(0, "muxf8 sol\n");
    n_sol = 0;
    for (CutCost::ITER itr = cut->muxf8SolBegin(); itr != cut->muxf8SolEnd(); ++itr) {
      msg(0, "        sol %d, delay %d level %d area %f\n", n_sol, itr->getDelay(), itr->getLevel(), itr->getArea());
      n_sol++;
    }
  }

  void Map::cutEnu(DataModel::Pin* root) {
    std::vector<CUT_ITER> iter_vec;
    std::vector<int> fanout_vec;
    std::vector<DataModel::Pin*> input_vec;
    std::vector<bool> input_across;
    Cut* merged_cut;
    bool to_preserve = false;
    size_t in_bound = getInBound();

    if (!root->isInpin() && root->owner()) {
      DataModel::Element* owner = root->owner();
      to_preserve = (owner->isInst() || owner->preserve());
      size_t in_size = getNumIn(owner);
      in_bound = to_preserve ? in_size : getInBound();
      input_vec.reserve(in_size);
      input_across.reserve(in_size);
      fanout_vec.reserve(in_size);
      iter_vec.reserve(in_size);
      for (DataModel::Element::PinIter pitr = owner->begin(); pitr != owner->end(); ++pitr) {
        DataModel::Pin* pin = *pitr;
        if (pin->isInpin() && pin->net() && pin->net()->valid() &&
            !pin->isClk() && !pin->is_ppo() && owner->hasArc(pin, root)) {
          DataModel::Pin* drv = pin->net()->source();
          input_vec.push_back(drv);
          fanout_vec.push_back(drv->net()->numSink());
          iter_vec.push_back(cutBegin(drv));
          DataModel::Gate* input = drv->gate();
          input_across.push_back(input && !input->preserve() && drv->net() && !drv->net()->isRTLKeep());
        }
      }
    } else if (root->net() && root->net()->valid() && root->net()->source() != root) {
      to_preserve = true;
      DataModel::Pin* drv = root->net()->source();
      input_vec.push_back(drv);
      input_across.push_back(false);
      fanout_vec.push_back(drv->net()->numSink());
      iter_vec.push_back(cutBegin(drv));
    }

    if (input_vec.size() > 0) {
      do {
        merged_cut = new Cut(root);
        for (size_t i = 0; i < input_vec.size(); ++i) {
          if (getNumCut(input_vec[i]) == 0) continue;  // if getNumCut(input_vec[i]) == 0, Cut * cut = *iter_vec[i]; add by zhiyong. 
          if (Cut* cut = *iter_vec[i]) {
            merged_cut->combine(cut);
            if (merged_cut->getCutSize() > in_bound) {
              break;
            }
          }
        }
        if (merged_cut->getCutSize() > 0 &&
            merged_cut->getCutSize() <= in_bound && 
            !isRedundant(root, merged_cut)) {
          for (size_t i = 0; i < input_vec.size(); ++i) {
            if (getNumCut(input_vec[i]) == 0) continue; // if getNumCut(input_vec[i]) == 0, Cut * cut = *iter_vec[i]; add by zhiyong.
            Cut* cut = *iter_vec[i];
            merged_cut->combineSol(this, root, cut, fanout_vec[i], input_vec[i]);
          }
          merged_cut->pruneSol();
          if (with_muxf()) {
            merged_cut->pruneMuxf7Sol();
            merged_cut->pruneMuxf8Sol();
          }
          if (!insertCut(root, merged_cut)) {
            delete merged_cut;
          }
        } else {
          delete merged_cut;
        }
      } while (!to_preserve && nextCut(input_vec, input_across, iter_vec));
    }

    if ((root->isPo() || root->isPpo()) && getNumCut(root) != 0) { // add "getNumCut(root) != 0" for CR_14725.
      Cut* cut = *cutBegin(root);
      cut->setTrivial();
    } else {
      //generate trivial cut
      merged_cut = new Cut(root);
      merged_cut->setTrivial();
      merged_cut->addInput(root);
      Delay lvlDly = getCutDelay(merged_cut, root) + getEdgeDelay();
      //Delay muxf7LvlDly = getMuxf7Delay() + getEdgeDelay();
      //Delay muxf8LvlDly = getMuxf8Delay(); // reduce an edgeDelay since delay(muxf7->muxf8) = 0
      if (getNumCut(root) == 0) {//pi
        CutSolution sol(getLutArea(), getArrival(root) + lvlDly, getLevel(root) + 1);
        merged_cut->insertSol(sol);
      } else {
        for (CUT_ITER itr = cutBegin(root); itr != cutEnd(root); ++itr) {
          Cut* cut = *itr;
          for (CutCost::ITER sitr = cut->solBegin(); sitr != cut->solEnd(); ++sitr) {
            CutSolution sol = *sitr;
            sol.incArea(getCutArea(merged_cut, root));
            sol.incDelay(lvlDly);
            sol.incLevel();
            merged_cut->insertSol(sol);
          }
          // FIXME : hardcode here. I think it not necessary for inst pin to calculate cut
          if (root->inst() && 
              (root->inst()->down_entity()->type() == DataModel::kEntityCARRY8P ||
               root->inst()->down_entity()->type() == DataModel::kEntityCARRY4))
            for (CutCost::ITER sitr = merged_cut->solBegin(); sitr != merged_cut->solEnd(); ++sitr)
              sitr->setArea(static_cast<Area>(2));

          if (with_muxf()) {
            for (CutCost::ITER sitr = cut->muxf7SolBegin(); sitr != cut->muxf7SolEnd(); ++sitr) {
              CutSolution sol = *sitr;
              sol.incArea(getCutArea(merged_cut, root));
              sol.incDelay(lvlDly);
              sol.incLevel();
              merged_cut->insertMuxf7Sol(sol);
              //merged_cut->insertSol(sol);
            }
            for (CutCost::ITER sitr = cut->muxf8SolBegin(); sitr != cut->muxf8SolEnd(); ++sitr) {
              CutSolution sol = *sitr;
              sol.incArea(getCutArea(merged_cut, root));
              sol.incDelay(lvlDly);
              sol.incLevel();
              merged_cut->insertMuxf8Sol(sol);
              //merged_cut->insertSol(sol);
            }
          }
        }
        merged_cut->pruneSol();
        if (with_muxf()) {
          merged_cut->pruneMuxf7Sol();
          merged_cut->pruneMuxf8Sol();
        }
      }
      pushFrontCut(root, merged_cut);
    }
  }

  Cut* Map::cutSel(DataModel::Pin* root, InstType rootType) {
    Domain domain = getDefaultDomain(top_);
    Delay required = getRequired(root, domain);
    Cut* best = 0;
    CutSolution best_sol;
    if (rootType == UNKNOWN) {
      if (with_muxf()) {
        size_t selIdx = -1;
        size_t trueIdx = -1;
        size_t falseIdx = -1;
        bool muxf7_prohibit = false;
        bool muxf8_prohibit = false;
        // restrictions for muxf7/muxf8 selection due to device structure
        if (!root->isInpin() && root->gate()) {
          DataModel::Gate* gate = root->gate();
          if (gate->isMuxf(selIdx, trueIdx, falseIdx)) {
            DataModel::Pin* trueIdxPin = gate->getInpin(trueIdx);
            DataModel::Pin* falseIdxPin = gate->getInpin(falseIdx);
            DataModel::Pin* trueIdxDrv = trueIdxPin->net()->source();
            DataModel::Pin* falseIdxDrv = falseIdxPin->net()->source();
            if (isLutRoot(trueIdxDrv) || isLutRoot(falseIdxDrv) || isMuxf7Root(trueIdxDrv) || isMuxf7Root(falseIdxDrv)) {
              muxf7_prohibit = true;
              muxf8_prohibit = true;
            } else {
              DataModel::Gate* trueDrvGate = trueIdxDrv->gate();
              DataModel::Gate* falseDrvGate = falseIdxDrv->gate();
              size_t trueDrv_selIdx = -1;
              size_t trueDrv_trueIdx = -1;
              size_t trueDrv_falseIdx = -1;
              if (trueDrvGate->isMuxf(trueDrv_selIdx, trueDrv_trueIdx, trueDrv_falseIdx)) {
                DataModel::Pin* trueDrv_trueIdxPin = trueDrvGate->getInpin(trueDrv_trueIdx);
                DataModel::Pin* trueDrv_falseIdxPin = trueDrvGate->getInpin(trueDrv_falseIdx);
                DataModel::Pin* trueDrv_trueIdxDrv = trueDrv_trueIdxPin->net()->source();
                DataModel::Pin* trueDrv_falseIdxDrv = trueDrv_falseIdxPin->net()->source();
                if (isLutRoot(trueDrv_trueIdxDrv) || isLutRoot(trueDrv_falseIdxDrv)) {
                  muxf8_prohibit = true;
                }
              }
              size_t falseDrv_selIdx = -1;
              size_t falseDrv_trueIdx = -1;
              size_t falseDrv_falseIdx = -1;
              if (falseDrvGate->isMuxf(falseDrv_selIdx, falseDrv_trueIdx, falseDrv_falseIdx)) {
                DataModel::Pin* falseDrv_trueIdxPin = falseDrvGate->getInpin(falseDrv_trueIdx);
                DataModel::Pin* falseDrv_falseIdxPin = falseDrvGate->getInpin(falseDrv_falseIdx);
                DataModel::Pin* falseDrv_trueIdxDrv = falseDrv_trueIdxPin->net()->source();
                DataModel::Pin* falseDrv_falseIdxDrv = falseDrv_falseIdxPin->net()->source();
                if (isLutRoot(falseDrv_trueIdxDrv) || isLutRoot(falseDrv_falseIdxDrv)) {
                  muxf8_prohibit = true;
                }
              }
            }
          }
        }
        if (!muxf8_prohibit) {
          // FIXME area is of higher priority for now for muxf7/muxf8 solution
          for (CUT_ITER itr = cutBegin(root); itr != cutEnd(root); ++itr) {
            Cut* cut = *itr;
            if (cut->isTrivial() == false) {
              if (cut->getNumMuxf8Sol() == 0) {
                continue;
              }
              CutSolution& sol = cut->getMuxf8SolForDelay(required);
              if (sol.getDelay() > required) continue;
              if (best == 0 ||
                  sol.getArea() < best_sol.getArea() ||
                  (sol.getArea() == best_sol.getArea() && sol.getDelay() < best_sol.getDelay())) {
                best = cut;
                best_sol = sol;
                root->gate()->setInstType(DataModel::Gate::MUXF8);
              }
            }
          }
        }
        if (!muxf7_prohibit) {
          // FIXME area is of higher priority for now for muxf7/muxf8 solution
          for (CUT_ITER itr = cutBegin(root); itr != cutEnd(root); ++itr) {
            Cut* cut = *itr;
            if (cut->isTrivial() == false) {
              if (cut->getNumMuxf7Sol() == 0) {
                continue;
              }
              CutSolution& sol = cut->getMuxf7SolForDelay(required);
              if (sol.getDelay() > required) continue;
              if (root->gate()->isInstMuxf8()) {
                if (sol.getArea() < best_sol.getArea()) {
                  best = cut;
                  best_sol = sol;
                  root->gate()->setInstType(DataModel::Gate::MUXF7);
                }
              } else {
                if (best == 0 ||
                    sol.getArea() < best_sol.getArea() ||
                    (sol.getArea() == best_sol.getArea() && sol.getDelay() < best_sol.getDelay())) {
                  best = cut;
                  best_sol = sol;
                  root->gate()->setInstType(DataModel::Gate::MUXF7);
                }
              }
            }
          }
        }
      }
      for (CUT_ITER itr = cutBegin(root); itr != cutEnd(root); ++itr) {
        Cut* cut = *itr;
        if (cut->isTrivial() == false) {
          CutSolution& sol = cut->getSolForDelay(required);
          if (sol.getDelay() > required) continue;
          if (root->gate()->isInstMuxf8() || root->gate()->isInstMuxf7()) {
            if (sol.getArea() < best_sol.getArea()) {
              best = cut;
              best_sol = sol;
              root->gate()->setInstType(DataModel::Gate::LUT);
            }
          } else if (best == 0 || 
              sol.getArea() < best_sol.getArea() || 
              (sol.getArea() == best_sol.getArea() && sol.getDelay() < best_sol.getDelay())) { 
            best = cut;
            best_sol = sol;
            root->gate()->setInstType(DataModel::Gate::LUT);
          }
        }
      }
    } else if (rootType == LUT) {
      for (CUT_ITER itr = cutBegin(root); itr != cutEnd(root); ++itr) {
        Cut* cut = *itr;
        if (cut->isTrivial() == false) {
          CutSolution& sol = cut->getSolForDelay(required);
          if (sol.getDelay() > required) continue;
          if (best == 0 || 
              sol.getArea() < best_sol.getArea() || 
              (sol.getArea() == best_sol.getArea() && sol.getDelay() < best_sol.getDelay())) { 
            best = cut;
            best_sol = sol;
            root->gate()->setInstType(DataModel::Gate::LUT);
          }
        }
      }
    } else if (rootType == MUXF7) {
      for (CUT_ITER itr = cutBegin(root); itr != cutEnd(root); ++itr) {
        Cut* cut = *itr;
        if (cut->isTrivial() == false) {
          if (cut->getNumMuxf7Sol() == 0) {
            continue;
          }
          CutSolution& sol = cut->getMuxf7SolForDelay(required);
          if (sol.getDelay() > required) continue;
          if (best == 0 || 
              sol.getArea() < best_sol.getArea() ||
              (sol.getArea() == best_sol.getArea() && sol.getDelay() < best_sol.getDelay())) {
            best = cut;
            best_sol = sol;
            root->gate()->setInstType(DataModel::Gate::MUXF7);
          }
        }
      }
    }
    if (!best) {
      nimbus_error(IDS_SYN_0031);
      throw std::runtime_error("Run-time error.\n");
    }
    selCut(root, best);
    return best;
  }

#if 1 //bottom-up collapsing
  void Map::lutGen(DataModel::Pin* root) {
    Cut* cut = getCut(root);
    std::vector<DataModel::Gate*> to_collapse;
    std::vector<DataModel::Pin*> cone, cutset;
    std::unordered_map<DataModel::Pin*, DataModel::Pin*> pin_map;
    DataModel::Pin* pin, *pin1;
    DataModel::Gate* gate;
    cutset.reserve(cut->getCutSize());
    for (Cut::ITER itr = cut->begin(); itr != cut->end(); ++itr) {
      DataModel::Pin * drv = (*itr);
      cutset.push_back(drv);
    }
    top_->topoOrder(root, cone, cutset);
    for (size_t i = 0; i < cone.size(); ++i) {
      pin = cone[i];
      if (pin->is_bidir()) continue; // inout pin may be put into the cone in topoOrder. 
      if (!pin->isGatePin()) {
        nimbus_error(IDS_SYN_0031);
        throw std::runtime_error("Run-time error.\n");
      }
      gate = pin->gate();
      if (pin != root) {
        gate = pin->gate()->duplicate();
        pin1 = gate->getOpin();
        pin_map[pin] = pin1;
      }
      for (size_t j = 0; j < gate->getNumIn(); ++j) {
        DataModel::Pin* ipin = gate->getInpin(j);
        if (DataModel::Pin* drv = ipin->net()->source()) {
          if (DataModel::Pin* drv1 = pin_map[drv]) {
            ipin->disconnect();
            ipin->connect(drv1);
          }
        }
      }
      to_collapse.push_back(gate);
    }
    DataModel::Gate* to_col_vec[10];
    int nin;
    for (size_t i = 0; i < to_collapse.size(); ++i) {
      gate = to_collapse[i];
      nin = 0;
      for (size_t j = 0; j < gate->getNumIn(); ++j) {
        DataModel::Pin* ipin = gate->getInpin(j);
        DataModel::Pin* drv = ipin->net()->source();
        if (drv && !cut->isCutInput(drv)) {
          DataModel::Gate* input = drv->gate();
          to_col_vec[nin++] = input;
        }
      }
      gate->mergeRtlInfo(to_col_vec, nin);
      for (int j = 0; j < nin; ++j) {
        gate->collapse(to_col_vec[j]);
      }
    }
    for (size_t i = 0; i < to_collapse.size() - 1; ++i) {
      gate = to_collapse[i];
      DataModel::Net* o_net = gate->getOpin()->net();
      gate->destroy();
      if (o_net && !o_net->valid()) {
        o_net->destroy();
      }
    }
    if (root->gate()->getNumIn() > getInBound()) {
      if (root->gate()->getNumIn() != (getInBound()+1)) {
        nimbus_error(IDS_SYN_0031);
        throw std::runtime_error("Run-time error.\n");
      }
      shannonDecomp(root->gate());
    }
  }
#else //top-down collapsing
  void Map::lutGen(DataModel::Pin* root) {
    Cut* cut = getCut(root);
    DataModel::Gate* gate = root->gate();
    std::vector<DataModel::Gate*> to_collapse;
    do {
      to_collapse.clear();
      for (size_t i = 0; i < gate->getNumIn(); ++i) {
        DataModel::Pin* ipin = gate->getInpin(i);
        DataModel::Pin* drv = ipin->net()->source();
        if (!cut->isCutInput(drv)) {
          DataModel::Gate* input = drv->gate();
          if (!input) {
            nimbus_error(IDS_SYN_0031);
            throw std::runtime_error("Run-time error.\n");
          }
          to_collapse.push_back(input);
        }
      }
      for (size_t i = 0; i < to_collapse.size(); ++i) {
        DataModel::Gate* input = to_collapse[i];
        gate->collapse(input);
      }
    } while (to_collapse.size() > 0);
    if (gate->getNumIn() > getInBound()) {
      if (gate->getNumIn() != (getInBound()+1)) {
        nimbus_error(IDS_SYN_0031);
        throw std::runtime_error("Run-time error.\n");
      }
      shannonDecomp(gate);
    }
  }
#endif

  // Shannon Decomposition: F(X1,X2,...,Xn) = X1 * F(1,X2,...,Xn) + X1' * F(0,X2,...,Xn)
  void Map::shannonDecomp(DataModel::Gate* gate) {
    int s_idx = -1;
    Delay s_dly = min_delay;
    DataModel::Pin* s_drv = NULL;

    for (size_t i = 0; i < gate->getNumIn(); ++i) {
      DataModel::Pin* ipin = gate->getInpin(i);
      if (DataModel::Pin* drv = ipin->net()->source()) {
        Cut* cut = *cutBegin(drv);
        Delay dly = cut->getMinDelay();
        //Gate::PHASE pp = gate->getFaninPhase(i);
        if (dly > s_dly) {
          s_dly = dly;
          s_idx = static_cast<int>(i);
          s_drv = drv;
        }
      }
    }
    DataModel::Gate* g1 = gate->duplicate();
    DataModel::Gate* g0 = gate->duplicate();
    std::string name = std::string(gate->name()) + "_1";
    g1->set_name(name);
    top_->add(g1);
    name = std::string(gate->name()) + "_0";
    g0->set_name(name);
    top_->add(g0);
    DataModel::Element* vcc = top_->getVcc();
    DataModel::Element* gnd = top_->getGnd();
    DataModel::Pin* vcc_opin = vcc->isInst()? vcc->findPin("O") : vcc->getPin(0);
    DataModel::Pin* gnd_opin = gnd->isInst()? gnd->findPin("O") : gnd->getPin(0);
    g1->getInpin(s_idx)->connect(vcc_opin);
    g0->getInpin(s_idx)->connect(gnd_opin);
    CoverTable* cvt = new CoverTable(3, 1, 2);
    Cube cube(cvt);
    cube.setVar(0, Cube::ONE);
    cube.setVar(1, Cube::DASH);
    cube.setVar(2, Cube::ONE);
    cvt->addCube(cube);
    cube.setVar(0, Cube::DASH);
    cube.setVar(1, Cube::ONE);
    cube.setVar(2, Cube::ZERO);
    cvt->addCube(cube);
    std::vector<DataModel::Pin*> inputs;
    inputs.push_back(g1->getOpin());
    inputs.push_back(g0->getOpin());
    inputs.push_back(s_drv);
    gate->reset(cvt, inputs);
    g1->removeConstFanin();
    g0->removeConstFanin();
  }

  void Opt::check_muxcy_spin(DataModel::Model * model) {
    std::vector<DataModel::Inst *> muxcys;
    for (DataModel::ModelInstIter itr = model->inst_begin(); itr != model->inst_end(); ++itr) {
      if (DataModel::Inst * inst = dynamic_cast<DataModel::Inst *>(*itr)) {
        if (strncmp(inst->down_entity()->name(), str_MUXCY, strlen(str_MUXCY)) == 0) muxcys.push_back(inst);
      }
    }
    for (size_t i = 0; i < muxcys.size(); ++i) {
      DataModel::Inst * inst = muxcys[i];
      DataModel::Net * ss_net = inst->findPin("S")->net();
      if (!ss_net) {
        nimbus_error(IDS_SYN_0031);
        throw std::runtime_error("Run-time error.\n");
      }
      std::vector<DataModel::Inst*> s_muxcys, s_xorcys;
      s_muxcys.clear();
      s_xorcys.clear();
      if (!ss_net->source() || !ss_net->source()->inst() || !ss_net->source()->inst()->isLUT()) s_muxcys.push_back(inst);
      for (DataModel::NetPinIter nitr = ss_net->begin(); nitr != ss_net->end(); ++nitr) {
        DataModel::Pin * pin = static_cast<DataModel::Pin *>(*nitr);
        if (!pin->isSink()) continue;
        DataModel::Inst * follower = pin->inst();
        if (!follower || follower == inst) continue; 
        char * down_entity_name = follower->down_entity()->name();
        if (strncmp(down_entity_name, str_MUXCY, strlen(str_MUXCY)) == 0 && strcmp(pin->name(), "S") == 0) {
          s_muxcys.push_back(follower);
        } else if (strncmp(down_entity_name, str_XORCY, strlen(str_XORCY)) == 0 && strcmp(pin->name(), "LI") == 0) {
          s_xorcys.push_back(follower);
        }
      }
      for (size_t j = 0; j < s_muxcys.size(); ++j) {
        DataModel::Inst * muxcy = s_muxcys[j];
        std::string muxcy_name = muxcy->name();
        DataModel::Inst * lut1 = DataModel::Inst::create((muxcy_name + "_lut1_named").c_str(), model, DataModel::findLib("hdl_lib")->findEntity("LUT1")->models()[0]);
        lut1->setProperty("INIT", "2");
        DataModel::Net * new_lut1_net = DataModel::Net::create((std::string(ss_net->name()) + "_onet").c_str(), model);
        lut1->findPin("O")->connect(new_lut1_net);
        lut1->findPin("I0")->connect(ss_net);
        muxcy->findPin("S")->disconnect();
        muxcy->findPin("S")->connect(new_lut1_net);
        DataModel::Inst * xorcy = NULL;
        DataModel::Net * ci_net = muxcy->findPin("CI")->net();
        for (size_t k = 0; k < s_xorcys.size(); ++k) {
          if (s_xorcys[k] && s_xorcys[k]->findPin("CI")->net() == ci_net) {
            xorcy = s_xorcys[k];
            s_xorcys[k] = NULL;
            break;
          }
        }
        if (xorcy) {
          xorcy->findPin("LI")->disconnect();
          xorcy->findPin("LI")->connect(new_lut1_net);
        }
      }
    }
  }

  void Map::map(bool resyn) {
    // full STA
    SynTiming syntm(top_);
    // Disable staBased mapping. (CR#19437)
    //if (Timing2::hasTimingConstraint()) {
    //  msg(1, "Run STA before map\n");
    //  syntm.runSTA(true);
    //  msg(1, "running STA based mapping...\n");
    //  setStaBased();
    //}

    std::vector<DataModel::Pin*> opin_vec;
    top_->topoOrder(opin_vec);
    int n_gate_pin = 0;
    for (size_t i = 0; i < opin_vec.size(); ++i) {
      if (opin_vec[i]->isGatePin()) ++ n_gate_pin;
    }
    if (n_gate_pin == 0) return;//no need to do mapping

    Delay max_arrival = min_delay;
    int min_level = 0;
    Domain domain = getDefaultDomain(top_);
    Area est_area = min_area;
    Area area = min_area;
    int n_lut = 0;
    int sum_cut = 0, max_cut = 0;


    //cut enumeration
    for (int i = 0; i < static_cast<int>(opin_vec.size()); ++i) {
      DataModel::Pin* opin = opin_vec[i];
      setIndex(opin, i);
      resetTiming(opin);
      if (opin->isPi() || opin->isPpi()) {
        if (isStaBased()) {
          float rat = 0.0f, at = 0.0f;
          (void)syntm.getWorstRatAndAt(opin, rat, at);
          setArrival(opin, static_cast<Delay>(at*1000.0), domain);
        } else
          setArrival(opin, 0, domain);
        setLevel(opin, 0);
      }

      // FIXME support various devices for muxf, remove hard code
      cutEnu(opin);
      {
        int tt = (int)getNumCut(opin);
        if (tt > max_cut) max_cut = tt;
        sum_cut += tt;
      }
      if (opin->isPo() || opin->isPpo()) {
        Cut* cut = *(cutBegin(opin));
        //Delay req = getRequired(opin, domain);
        //CutSolution& sol = cut->getSolForDelay(req);
        CutSolution& sol = cut->minDelaySol();
        if (sol.getDelay() > max_arrival) max_arrival = sol.getDelay();
        if (sol.getLevel() > min_level) min_level = sol.getLevel();
      }
    }
    //cut selection
    for (int i = static_cast<int>(opin_vec.size()-1); i >= 0; --i) {
      DataModel::Pin* opin = opin_vec[i];
      if (!opin->net() || opin->net()->num_pins() < 2) continue;
      bool isGateMuxf = false;
      DataModel::Gate* gate = 0;
      size_t selIdx = -1;
      size_t trueIdx = -1;
      size_t falseIdx = -1;
      if (!opin->isInpin() && opin->gate()) {
        gate = opin->gate();
        if (gate->isMuxf(selIdx, trueIdx, falseIdx)) {
          isGateMuxf = true;
        }
      }
      if (opin->isPo() || opin->isPpo()) {
        Cut* cut = *(cutBegin(opin));
        Delay req;
        if (isStaBased()) {
          CutSolution& sol = cut->minDelaySol();
          float rat = 0.0f, at = 0.0f;
          (void)syntm.getWorstRatAndAt(opin, rat, at);
          req = static_cast<Delay>(rat *1000.0);
          if (req < sol.getDelay()) req = sol.getDelay();
        } else
          req = max_arrival;
        setRequired(opin, req, domain);
        CutSolution& sol = cut->getSolForDelay(req);
        req -= (getCutDelay(cut, opin) + getEdgeDelay());
        est_area += (sol.getArea() - getLutArea());
        if (DataModel::Pin* drv = opin->net()->source()) {
          updateRequired(drv, req, domain);
          setRootType(drv, UNKNOWN);
        }
      } else if (isUnknownRoot(opin)) {
        //TODO compute area for report
        Delay req = getRequired(opin, domain);
        if (opin->is_gate_pin()) {
          if (!gate->isConst()) {
            Cut* cut = cutSel(opin, UNKNOWN);
            /*Console::printInfo("cut selected for gate: %s isLut: %d isMuxf7: %d isMuxf8: %d\n", gate->name(), gate->isInstLut(), gate->isInstMuxf7(), gate->isInstMuxf8());
            printCut(cut);*/
            if (gate->isInstLut()) {
              for (Cut::ITER itr = cut->begin(); itr != cut->end(); ++itr) {
                DataModel::Pin* drv = *itr;
                Delay req_drv = req - getLutDelay() - getEdgeDelay();
                updateRequired(drv, req_drv, domain);
                setRootType(drv, UNKNOWN);
              }
            } else if (gate->isInstMuxf7()) {
              if (!isGateMuxf) {
                nimbus_error(IDS_SYN_0031);
                throw std::runtime_error("Run-time error.\n");
              }
              DataModel::Pin* trueIdxPin = gate->getInpin(trueIdx);
              DataModel::Pin* trueIdxDrv = trueIdxPin->net()->source();
              if (!cut->included(trueIdxDrv)) {
                nimbus_error(IDS_SYN_0031);
                throw std::runtime_error("Run-time error.\n");
              }
              DataModel::Pin* falseIdxPin = gate->getInpin(falseIdx);
              DataModel::Pin* falseIdxDrv = falseIdxPin->net()->source();
              if (!cut->included(falseIdxDrv)) {
                nimbus_error(IDS_SYN_0031);
                throw std::runtime_error("Run-time error.\n");
              }
              DataModel::Pin* selIdxPin = gate->getInpin(selIdx);
              DataModel::Pin* selIdxDrv = selIdxPin->net()->source();
              if (!cut->included(selIdxDrv)) {
                nimbus_error(IDS_SYN_0031);
                throw std::runtime_error("Run-time error.\n");
              }
              // FIXME set various delay for I0/I1/S pin
              Delay req_drv = req - getMuxf7Delay();
              updateRequired(trueIdxDrv, req_drv, domain);
              setRootType(trueIdxDrv, LUT);
              updateRequired(falseIdxDrv, req_drv, domain);
              setRootType(falseIdxDrv, LUT);
              updateRequired(selIdxDrv, req_drv - getEdgeDelay(), domain);
              setRootType(selIdxDrv, UNKNOWN);
            } else if (gate->isInstMuxf8()) {
              if (!isGateMuxf) {
                nimbus_error(IDS_SYN_0031);
                throw std::runtime_error("Run-time error.\n");
              }
              DataModel::Pin* trueIdxPin = gate->getInpin(trueIdx);
              DataModel::Pin* trueIdxDrv = trueIdxPin->net()->source();
              if (!cut->included(trueIdxDrv)) {
                nimbus_error(IDS_SYN_0031);
                throw std::runtime_error("Run-time error.\n");
              }
              DataModel::Pin* falseIdxPin = gate->getInpin(falseIdx);
              DataModel::Pin* falseIdxDrv = falseIdxPin->net()->source();
              if (!cut->included(falseIdxDrv)) {
                nimbus_error(IDS_SYN_0031);
                throw std::runtime_error("Run-time error.\n");
              }
              DataModel::Pin* selIdxPin = gate->getInpin(selIdx);
              DataModel::Pin* selIdxDrv = selIdxPin->net()->source();
              if (!cut->included(selIdxDrv)) {
                nimbus_error(IDS_SYN_0031);
                throw std::runtime_error("Run-time error.\n");
              }
              // FIXME set various delay for I0/I1/S pin
              Delay req_drv = req - getMuxf8Delay(); // reduce an edgeDelay since delay(muxf7->muxf8) = 0
              updateRequired(trueIdxDrv, req_drv, domain);
              setRootType(trueIdxDrv, MUXF7);
              updateRequired(falseIdxDrv, req_drv, domain);
              setRootType(falseIdxDrv, MUXF7);
              updateRequired(selIdxDrv, req_drv - getEdgeDelay(), domain);
              setRootType(selIdxDrv, UNKNOWN);
            }
          }
        } else if (opin->owner()) {
          DataModel::Inst* owner = opin->getInst();
          area += owner->getArea();
          for (DataModel::Element::PinIter itr = owner->begin(); itr != owner->end(); ++itr) {
            DataModel::Pin* ipin = *itr;
            if (ipin->net() && ipin->isInpin() && !ipin->is_ppo() && owner->hasArc(ipin, opin)) {
              if (DataModel::Pin* drv = ipin->net()->source()) {
                Delay req_drv;
                if (isStaBased())
                  req_drv = req - owner->arcDelay(ipin, opin) - getEdgeDelay();
                else
                  req_drv = req - owner->arcDelay(ipin, opin) - ipin->net()->edgeDelay(ipin, drv);
                updateRequired(drv, req_drv, domain);
                setRootType(drv, UNKNOWN);
              }
            }
          }
        }
      } else if (isLutRoot(opin)) {
        if (!gate) {
          nimbus_error(IDS_SYN_0031);
          throw std::runtime_error("Run-time error.\n");
        }
        Delay req = getRequired(opin, domain);
        if (!gate->isConst()) {
          Cut* cut = cutSel(opin, LUT);
          /*Console::printInfo("cut selected for gate: %s isLut: %d isMuxf7: %d isMuxf8: %d\n", gate->name(), gate->isInstLut(), gate->isInstMuxf7(), gate->isInstMuxf8());
          printCut(cut);*/
          if (gate->isInstLut()) {
            for (Cut::ITER itr = cut->begin(); itr != cut->end(); ++itr) {
              DataModel::Pin* drv = *itr;
              Delay req_drv = req - getLutDelay() - getEdgeDelay();
              updateRequired(drv, req_drv, domain);
              setRootType(drv, UNKNOWN);
            }
          }
        }
      } else if (isMuxf7Root(opin)) {
        if (!isGateMuxf) {
          nimbus_error(IDS_SYN_0031);
          throw std::runtime_error("Run-time error.\n");
        }
        Delay req = getRequired(opin, domain);
        if (!gate->isConst()) {
          Cut* cut = cutSel(opin, MUXF7);
          /*Console::printInfo("cut selected for gate: %s isLut: %d isMuxf7: %d isMuxf8: %d\n", gate->name(), gate->isInstLut(), gate->isInstMuxf7(), gate->isInstMuxf8());
          printCut(cut);*/
          if (gate->isInstMuxf7()) {
            DataModel::Pin* trueIdxPin = gate->getInpin(trueIdx);
            DataModel::Pin* trueIdxDrv = trueIdxPin->net()->source();
            if (!cut->included(trueIdxDrv)) {
              nimbus_error(IDS_SYN_0031);
              throw std::runtime_error("Run-time error.\n");
            }
            DataModel::Pin* falseIdxPin = gate->getInpin(falseIdx);
            DataModel::Pin* falseIdxDrv = falseIdxPin->net()->source();
            if (!cut->included(falseIdxDrv)) {
              nimbus_error(IDS_SYN_0031);
              throw std::runtime_error("Run-time error.\n");
            }
            DataModel::Pin* selIdxPin = gate->getInpin(selIdx);
            DataModel::Pin* selIdxDrv = selIdxPin->net()->source();
            if (!cut->included(selIdxDrv)) {
              nimbus_error(IDS_SYN_0031);
              throw std::runtime_error("Run-time error.\n");
            }
            // FIXME set various delay for I0/I1/S pin
            Delay req_drv = req - getMuxf7Delay();
            updateRequired(trueIdxDrv, req_drv, domain);
            setRootType(trueIdxDrv, LUT);
            updateRequired(falseIdxDrv, req_drv, domain);
            setRootType(falseIdxDrv, LUT);
            updateRequired(selIdxDrv, req_drv - getEdgeDelay(), domain);
            setRootType(selIdxDrv, UNKNOWN);
          }
        }
      } else if (isMuxf8Root(opin)) {
        if (!isGateMuxf) {
          nimbus_error(IDS_SYN_0031);
          throw std::runtime_error("Run-time error.\n");
        }
        Delay req = getRequired(opin, domain);
        if (!gate->isConst()) {
          Cut* cut = cutSel(opin, MUXF8);
          /*Console::printInfo("cut selected for gate: %s isLut: %d isMuxf7: %d isMuxf8: %d\n", gate->name(), gate->isInstLut(), gate->isInstMuxf7(), gate->isInstMuxf8());
          printCut(cut);*/
          if (gate->isInstMuxf8()) {
            DataModel::Pin* trueIdxPin = gate->getInpin(trueIdx);
            DataModel::Pin* trueIdxDrv = trueIdxPin->net()->source();
            if (!cut->included(trueIdxDrv)) {
              nimbus_error(IDS_SYN_0031);
              throw std::runtime_error("Run-time error.\n");
            }
            DataModel::Pin* falseIdxPin = gate->getInpin(falseIdx);
            DataModel::Pin* falseIdxDrv = falseIdxPin->net()->source();
            if (!cut->included(falseIdxDrv)) {
              nimbus_error(IDS_SYN_0031);
              throw std::runtime_error("Run-time error.\n");
            }
            DataModel::Pin* selIdxPin = gate->getInpin(selIdx);
            DataModel::Pin* selIdxDrv = selIdxPin->net()->source();
            if (!cut->included(selIdxDrv)) {
              nimbus_error(IDS_SYN_0031);
              throw std::runtime_error("Run-time error.\n");
            }
            // FIXME set various delay for I0/I1/S pin
            Delay req_drv = req - getMuxf8Delay(); // reduce an edgeDelay since delay(muxf7->muxf8) = 0
            updateRequired(trueIdxDrv, req_drv, domain);
            setRootType(trueIdxDrv, MUXF7);
            updateRequired(falseIdxDrv, req_drv, domain);
            setRootType(falseIdxDrv, MUXF7);
            updateRequired(selIdxDrv, req_drv - getEdgeDelay(), domain);
            setRootType(selIdxDrv, UNKNOWN);
          }
        }
      }
    }
    msg(1, "estimated delay %d area %d level %d, computed area %d LUT# %d\n",
        max_arrival, (int)est_area, min_level, (int)area, n_lut);
    msg(1, "total cut# is %d max is %d average is %.1f\n",
        sum_cut, max_cut, (float)sum_cut/(float)opin_vec.size());
    //mapping generation
    int n_luts = 0;
    for (int i = static_cast<int>(opin_vec.size() - 1); i >= 0; --i) {
      DataModel::Pin* opin = opin_vec[i];
      if (opin->is_gate_pin()) {
        DataModel::Gate* gate = opin->gate();
        if (gate->isInstLut()) {
          n_luts++;
          lutGen(opin);
        }
      }
    }
    sweep(top_);
    if (resyn && n_luts > 2 && n_luts < 50000) {  //run resynthesis
#ifndef MEM_LEAK_WIN32
      Abc abc(top_);
      abc.resyn2rs(1, 1, 0);
#endif
    }
    top_->gateToInst();
  }

}
