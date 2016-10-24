#include <dmlc/omp.h>
#include <vector>
#include "./sync.h"
#include "./hist_util.h"
#include "./quantile.h"

namespace xgboost {
namespace common {

void HistCutMatrix::Init(DMatrix *p_fmat, size_t max_num_bins) {
  typedef common::WXQuantileSketch<bst_float, bst_float> WXQSketch;
  const MetaInfo& info = p_fmat->info();
  // safe factor for better accuracy
  const int kFactor = 8;
  std::vector<WXQSketch> sketchs;

  int nthread;
  //#pragma omp parallel
  {
    nthread = omp_get_num_threads();
  }
  nthread = std::max(nthread / 2, 1);

  unsigned nstep = (info.num_col + nthread - 1) / nthread;
  unsigned ncol = static_cast<unsigned>(info.num_col);
  sketchs.resize(info.num_col);
  for (auto& s : sketchs) {
    s.Init(info.num_row, 1.0 / (max_num_bins * kFactor));
  }

  dmlc::DataIter<RowBatch> *iter = p_fmat->RowIterator();
  iter->BeforeFirst();
  while (iter->Next()) {
    const RowBatch& batch = iter->Value();
    //#pragma omp parallel num_threads(nthread)
    {
      CHECK_EQ(nthread, omp_get_num_threads());
      unsigned tid = static_cast<unsigned>(omp_get_thread_num());
      unsigned begin = std::min(nstep * tid, ncol);
      unsigned end = std::min(nstep * (tid + 1), ncol);
      for (size_t i = 0; i < batch.size; ++i) { // NOLINT(*)
        bst_uint ridx = static_cast<bst_uint>(batch.base_rowid + i);
        RowBatch::Inst inst = batch[i];
        for (bst_uint j = 0; j < inst.length; ++j) {
          if (inst[j].index >= begin && inst[j].index < end) {
            sketchs[inst[j].index].Push(inst[j].fvalue, info.GetWeight(ridx));
          }
        }
      }
    }
  }

  // gather the histogram data
  rabit::SerializeReducer<WXQSketch::SummaryContainer> sreducer;
  std::vector<WXQSketch::SummaryContainer> summary_array;
  summary_array.resize(sketchs.size());
  for (size_t i = 0; i < sketchs.size(); ++i) {
    WXQSketch::SummaryContainer out;
    sketchs[i].GetSummary(&out);
    summary_array[i].Reserve(max_num_bins * kFactor);
    summary_array[i].SetPrune(out, max_num_bins * kFactor);
  }
  size_t nbytes = WXQSketch::SummaryContainer::CalcMemCost(max_num_bins * kFactor);
  sreducer.Allreduce(dmlc::BeginPtr(summary_array), nbytes, summary_array.size());

  for (size_t fid = 0; fid < summary_array.size(); ++fid) {
    WXQSketch::SummaryContainer a;
    a.Reserve(max_num_bins);
    a.SetPrune(summary_array[fid], max_num_bins);
    for (size_t i = 2; i < a.size; ++i) {
      bst_float cpt = a.data[i - 1].value - rt_eps;
      if (i == 2 || cpt > cut.back()) {
        cut.push_back(cpt);
      }
      // push a value that is greater than anything
      if (a.size != 0) {
        bst_float cpt = a.data[a.size - 1].value;
        // this must be bigger than last value in a scale
        bst_float last = cpt + fabs(cpt) + rt_eps;
        cut.push_back(last);
      }
      row_ptr.push_back(cut.size());
    }
  }
}


void GHistIndexMatrix::Init(DMatrix* p_fmat) {
  CHECK(cut != nullptr);
  dmlc::DataIter<RowBatch> *iter = p_fmat->RowIterator();
  hit_count.resize(cut->row_ptr.back(), 0);

  iter->BeforeFirst();
  while (iter->Next()) {
    const RowBatch& batch = iter->Value();
    size_t rbegin = row_ptr.size() - 1;
    for (size_t i = 0; i < batch.size; ++i) {
      row_ptr.push_back(batch[i].length + row_ptr.back());
    }
    index.resize(row_ptr.back());
    omp_ulong bsize = static_cast<omp_ulong>(batch.size);
    #pragma omp parallel for
    for (omp_ulong i = 0; i < bsize; ++i) { // NOLINT(*)
      size_t ibegin = row_ptr[rbegin + i];
      size_t iend = row_ptr[rbegin + i + 1];
      RowBatch::Inst inst = batch[i];
      CHECK_EQ(ibegin + inst.length, iend);
      for (bst_uint j = 0; j < inst.length; ++j) {
        unsigned fid = inst[j].index;
        auto cbegin = cut->cut.begin() + cut->row_ptr[fid];
        auto cend = cut->cut.begin() + cut->row_ptr[fid + 1];
        auto it = std::upper_bound(cbegin, cend, inst[j].fvalue);
        if (it == cend) it = cend - 1;
        unsigned idx = static_cast<unsigned>(it - cut->cut.begin());
        index[ibegin + j] = idx;
      }
      std::sort(index.begin() + ibegin, index.begin() + iend);
    }
  }
}


}  // namespace common
}  // namespace xgboost