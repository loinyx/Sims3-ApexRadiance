// Gather batches of a region-listed layer (real-time GI work package R-G1). The GI worker (patch 0023) sizes them with
// this; tools/gi_server_test/list_batch_check.cpp runs it against a cost model of the measured lots.
// A listed gather has a few columns of 64 texels, so it leaves most of the GPU idle and its time hardly falls with the
// list length. The batch therefore counts whole gathers: kept as is for a shorter list, cut in proportion for a longer one.
#pragma once
#include <algorithm>
#include <cstdint>

struct GiListBatch {
  float gathers = 2.0f;           // gathers that fit the budget, measured at `columns`
  uint32_t columns = 1u << 16;    // no measurement yet: the first batch is two gathers at any length

  // Gathers of a list of groupsX columns that fit the budget.
  float fit(uint32_t groupsX) const {
    return gathers * std::min(1.0f, float(columns) / float(std::max<uint32_t>(groupsX, 1)));
  }

  // Gathers of the next submission. Below 2 the caller runs one gather in slices.
  uint32_t next(uint32_t groupsX, uint32_t remaining) const {
    return std::min(uint32_t(fit(groupsX)), remaining);
  }

  // Records a submission of `done` gathers that took `us` of GPU time. Lamps differ in cost, so the size drops at once
  // to what fits and grows back by a quarter a submission.
  void learn(uint32_t groupsX, uint32_t done, uint32_t us, uint32_t budgetUs) {
    const float known = fit(groupsX);
    const float measured = float(done) * float(budgetUs) / float(std::max<uint32_t>(us, 1));
    gathers = std::max(1.0f, std::min({ measured, known * 1.25f, 65536.0f }));
    columns = std::max<uint32_t>(groupsX, 1);
  }
};
