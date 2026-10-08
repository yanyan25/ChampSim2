/*
 * DD-TAGE -- the predictor from "A Deep Dive Into TAGE-SC-L", Alberto Ros
 * (University of Murcia), CBP2025 (CBP6) submission -- hosted in ChampSim.
 *
 * Placed 5th of the nine ranked submissions on overall BrMisPKI (CBP2025
 * closing remarks [1]), in a storage budget of 191.90 KB -- verified: the
 * vendored copy's own predictorsize() reports 191.900269 KB. Ros reports 3.4120
 * MPKI and 145.4 CycWpPKI against his own 192KB TAGE-SC-L baseline's 3.4405 and
 * 146.1. Note the championship's reference point is a 192KB-scaled TAGE-SC-L,
 * NOT the 64KB CBP2016 predictor, so reductions quoted there are against a much
 * stronger baseline than cbp6_tagescl64.
 *
 * [1] CBP2025 Closing Remarks, Results, and Awards, 6th Championship Branch
 *     Prediction, ISCA 2025, Tokyo, 21 June 2025.
 *
 * Unlike RUNLTS, this predictor uses only PC and branch history -- it never
 * touches the CBP2025 register-value channel. It therefore transfers into
 * ChampSim at FULL STRENGTH: there is no analogue of cbp6_runlts_norv's missing
 * component, and a result from this module is the predictor as submitted.
 *
 * The submission's method names differ from the ones champsim::cbp6::host calls,
 * so the tenant is a thin adapter (see the .cc). The differences are naming and
 * argument order only; no predictor logic is changed.
 *
 * SINGLE CORE, SINGLE CONTEXT ONLY, as with the other CBP6 tenants: predictor
 * state lives in namespace-scope globals shared by every instance.
 */

#ifndef BRANCH_CBP6_DDTAGE_H
#define BRANCH_CBP6_DDTAGE_H

#include <cstdint>

#include "address.h"
#include "modules.h"

class cbp6_ddtage : champsim::modules::branch_predictor
{
public:
  using branch_predictor::branch_predictor;

  void initialize_branch_predictor();
  bool predict_branch(champsim::address ip, champsim::address predicted_target, bool always_taken, uint8_t branch_type, champsim::address_space_id asid);
  void last_branch_result(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type, champsim::address_space_id asid);
  void branch_predictor_final_stats();
  void branch_execute_resolve(uint64_t instr_id, champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type,
                              champsim::address_space_id asid);
};

#endif
