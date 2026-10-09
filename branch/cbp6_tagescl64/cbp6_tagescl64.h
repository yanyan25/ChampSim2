/*
 * The CBP2016 TAGE-SC-L reference predictor (64KB budget), as shipped with the
 * CBP2025 framework, hosted in ChampSim.
 *
 * The predictor source under vendor/ is byte-identical to
 * cbp2025/cbp2016_tage_sc_l.h. All translation between ChampSim's module API
 * and the CBP6 interface lives in inc/cbp6/, so adding another CBP6 submission
 * means adding a directory, not porting code.
 *
 * SINGLE CORE, SINGLE CONTEXT ONLY. The vendored predictor keeps its tables in
 * namespace-scope globals rather than in the predictor object, so every instance
 * would share them. It declares no smt_capability, so it defaults to
 * champsim::modules::smt_capability::unsupported and the environment refuses a
 * second core or a second hardware context at configuration time rather than
 * silently producing wrong results.
 */

#ifndef BRANCH_CBP6_TAGESCL64_H
#define BRANCH_CBP6_TAGESCL64_H

#include <cstdint>

#include "address.h"
#include "modules.h"

class cbp6_tagescl64 : champsim::modules::branch_predictor
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
