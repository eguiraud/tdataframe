#ifndef ROOT_TDATAFRAME
#define ROOT_TDATAFRAME

#include "TBranchElement.h"
#include "TDirectory.h"
#include "TH1F.h" // For Histo actions
#include "TROOT.h" // IsImplicitMTEnabled, GetImplicitMTPoolSize
#include "ROOT/TSpinMutex.hxx"
#include "ROOT/TTreeProcessor.hxx"
#include "TTreeReader.h"
#include "TTreeReaderValue.h"

#include <algorithm> // std::find
#include <array>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <type_traits> // std::decay
#include <typeinfo>
#include <vector>

// Meta programming utilities, perhaps to be moved in core/foundation
namespace ROOT {
namespace Internal {
namespace TDFTraitsUtils {
template <typename... Types>
struct TTypeList {
   static constexpr std::size_t fgSize = sizeof...(Types);
};

// extract parameter types from a callable object
template <typename T>
struct TFunctionTraits {
   using ArgTypes_t = typename TFunctionTraits<decltype(&T::operator())>::ArgTypes_t;
   using ArgTypesNoDecay_t = typename TFunctionTraits<decltype(&T::operator())>::ArgTypesNoDecay_t;
   using RetType_t = typename TFunctionTraits<decltype(&T::operator())>::RetType_t;
};

// lambdas and std::function
template <typename R, typename T, typename... Args>
struct TFunctionTraits<R (T::*)(Args...) const> {
   using ArgTypes_t = TTypeList<typename std::decay<Args>::type...>;
   using ArgTypesNoDecay_t = TTypeList<Args...>;
   using RetType_t = R;
};

// mutable lambdas and functor classes
template <typename R, typename T, typename... Args>
struct TFunctionTraits<R (T::*)(Args...)> {
   using ArgTypes_t = TTypeList<typename std::decay<Args>::type...>;
   using ArgTypesNoDecay_t = TTypeList<Args...>;
   using RetType_t = R;
};

// function pointers
template <typename R, typename... Args>
struct TFunctionTraits<R (*)(Args...)> {
   using ArgTypes_t = TTypeList<typename std::decay<Args>::type...>;
   using ArgTypesNoDecay_t = TTypeList<Args...>;
   using RetType_t = R;
};

// free functions
template <typename R, typename... Args>
struct TFunctionTraits<R (Args...)> {
   using ArgTypes_t = TTypeList<typename std::decay<Args>::type...>;
   using ArgTypesNoDecay_t = TTypeList<Args...>;
   using RetType_t = R;
};

// remove first type from TypeList
template <typename>
struct TRemoveFirst { };

template <typename T, typename... Args>
struct TRemoveFirst<TTypeList<T, Args...>> {
   using Types_t = TTypeList<Args...>;
};

// return wrapper around f that prepends an `unsigned int slot` parameter
template <typename R, typename F, typename... Args>
std::function<R(unsigned int, Args...)> AddSlotParameter(F f, TTypeList<Args...>)
{
   return [f](unsigned int, Args... a) -> R { return f(a...); };
}

// compile-time integer sequence generator
// e.g. calling TGenStaticSeq<3>::type() instantiates a TStaticSeq<0,1,2>
template <int...>
struct TStaticSeq { };

template <int N, int... S>
struct TGenStaticSeq : TGenStaticSeq<N - 1, N - 1, S...> { };

template <int... S>
struct TGenStaticSeq<0, S...> {
   using Type_t = TStaticSeq<S...>;
};

template <typename T>
struct TIsContainer {
   using Test_t = typename std::decay<T>::type;

   template <typename A>
   static constexpr bool Test(A *pt, A const *cpt = nullptr, decltype(pt->begin()) * = nullptr,
                              decltype(pt->end()) * = nullptr, decltype(cpt->begin()) * = nullptr,
                              decltype(cpt->end()) * = nullptr, typename A::iterator *pi = nullptr,
                              typename A::const_iterator *pci = nullptr, typename A::value_type *pv = nullptr)
   {
      using It_t = typename A::iterator;
      using CIt_t = typename A::const_iterator;
      using V_t = typename A::value_type;
      return std::is_same<Test_t, std::vector<bool>>::value ||
             (std::is_same<decltype(pt->begin()), It_t>::value &&
              std::is_same<decltype(pt->end()), It_t>::value &&
              std::is_same<decltype(cpt->begin()), CIt_t>::value &&
              std::is_same<decltype(cpt->end()), CIt_t>::value &&
              std::is_same<decltype(**pi), V_t &>::value &&
              std::is_same<decltype(**pci), V_t const &>::value);
   }

   template <typename A>
   static constexpr bool Test(...)
   {
      return false;
   }

   static const bool fgValue = Test<Test_t>(nullptr);
};

} // end NS TDFTraitsUtils

} // end NS Internal

} // end NS ROOT

namespace ROOT {

using BranchVec = std::vector<std::string>;

// Fwd declarations
namespace Details {
class TDataFrameImpl;
}

/// Smart pointer for the return type of actions
template <typename T>
class TActionResultPtr {
   using SPT_t = std::shared_ptr<T> ;
   using SPTDFI_t = std::shared_ptr<Details::TDataFrameImpl>;
   using WPTDFI_t = std::weak_ptr<Details::TDataFrameImpl>;
   using ShrdPtrBool_t = std::shared_ptr<bool>;
   friend class Details::TDataFrameImpl;

   ShrdPtrBool_t fReadiness = std::make_shared<bool>(false);
   WPTDFI_t fFirstData;
   SPT_t fObjPtr;
   void TriggerRun();
   TActionResultPtr(SPT_t objPtr, ShrdPtrBool_t readiness, SPTDFI_t firstData)
      : fFirstData(firstData), fObjPtr(objPtr), fReadiness(readiness) { }
   static TActionResultPtr<T> MakeActionResultPtr(SPT_t objPtr, ShrdPtrBool_t readiness, SPTDFI_t firstData)
   {
      return TActionResultPtr(objPtr, readiness, firstData);
   }
public:
   TActionResultPtr() = delete;
   T *Get()
   {
      if (!*fReadiness) TriggerRun();
      return fObjPtr.get();
   }
   T &operator*() { return *Get(); }
   T *operator->() { return Get(); }
   T *GetUnchecked() { return fObjPtr.get(); }
};

} // end NS ROOT

// Internal classes
namespace ROOT {

namespace Details {
class TDataFrameImpl;
}

namespace Internal {

unsigned int GetNSlots() {
   unsigned int nSlots = 1;
#ifdef R__USE_IMT
   if (ROOT::IsImplicitMTEnabled()) nSlots = ROOT::GetImplicitMTPoolSize();
#endif // R__USE_IMT
   return nSlots;
}

using TVBPtr_t = std::shared_ptr<TTreeReaderValueBase>;
using TVBVec_t = std::vector<TVBPtr_t>;

template <int... S, typename... BranchTypes>
TVBVec_t BuildReaderValues(TTreeReader &r, const BranchVec &bl, const BranchVec &tmpbl,
                           TDFTraitsUtils::TTypeList<BranchTypes...>,
                           TDFTraitsUtils::TStaticSeq<S...>)
{
   // isTmpBranch has length bl.size(). Elements are true if the corresponding
   // branch is a "fake" branch created with AddBranch, false if they are
   // actual branches present in the TTree.
   std::array<bool, sizeof...(S)> isTmpBranch;
   for (unsigned int i = 0; i < isTmpBranch.size(); ++i)
      isTmpBranch[i] = std::find(tmpbl.begin(), tmpbl.end(), bl.at(i)) != tmpbl.end();

   // Build vector of pointers to TTreeReaderValueBase.
   // tvb[i] points to a TTreeReaderValue specialized for the i-th BranchType,
   // corresponding to the i-th branch in bl
   // For temporary branches (declared with AddBranch) a nullptr is created instead
   // S is expected to be a sequence of sizeof...(BranchTypes) integers
   TVBVec_t tvb{isTmpBranch[S] ? nullptr : std::make_shared<TTreeReaderValue<BranchTypes>>(
                                            r, bl.at(S).c_str())...}; // "..." expands BranchTypes and S simultaneously

   return tvb;
}

template <typename Filter>
void CheckFilter(Filter f)
{
   using FilterRet_t = typename TDFTraitsUtils::TFunctionTraits<Filter>::RetType_t;
   static_assert(std::is_same<FilterRet_t, bool>::value, "filter functions must return a bool");
}

void CheckTmpBranch(const std::string& branchName, TTree *treePtr)
{
   auto branch = treePtr->GetBranch(branchName.c_str());
   if (branch != nullptr) {
      auto msg = "branch \"" + branchName + "\" already present in TTree";
      throw std::runtime_error(msg);
   }
}

const BranchVec &PickBranchVec(unsigned int nArgs, const BranchVec &bl, const BranchVec &defBl)
{
   // return local BranchVec or default BranchVec according to which one
   // should be used
   bool useDefBl = false;
   if (nArgs != bl.size()) {
      if (bl.size() == 0 && nArgs == defBl.size()) {
         useDefBl = true;
      } else {
         auto msg = "mismatch between number of filter arguments (" + std::to_string(nArgs) +
                    ") and number of branches (" + std::to_string(bl.size() ? bl.size() : defBl.size()) + ")";
         throw std::runtime_error(msg);
      }
   }

   return useDefBl ? defBl : bl;
}

class TDataFrameActionBase {
public:
   virtual ~TDataFrameActionBase() {}
   virtual void Run(unsigned int slot, int entry) = 0;
   virtual void BuildReaderValues(TTreeReader &r, unsigned int slot) = 0;
   virtual void CreateSlots(unsigned int nSlots) = 0;
};

using ActionBasePtr_t = std::shared_ptr<TDataFrameActionBase>;
using ActionBaseVec_t = std::vector<ActionBasePtr_t>;

// Forward declarations
template <int S, typename T>
T &GetBranchValue(TVBPtr_t &readerValues, unsigned int slot, int entry, const std::string &branch,
                  std::weak_ptr<Details::TDataFrameImpl> df);

template <typename F, typename PrevDataFrame>
class TDataFrameAction final : public TDataFrameActionBase {
   using BranchTypes_t = typename TDFTraitsUtils::TRemoveFirst<typename TDFTraitsUtils::TFunctionTraits<F>::ArgTypes_t>::Types_t;
   using TypeInd_t = typename TDFTraitsUtils::TGenStaticSeq<BranchTypes_t::fgSize>::Type_t;

   F fAction;
   const BranchVec fBranches;
   const BranchVec fTmpBranches;
   PrevDataFrame *fPrevData;
   std::weak_ptr<Details::TDataFrameImpl> fFirstData;
   std::vector<TVBVec_t> fReaderValues;

public:
   TDataFrameAction(F f, const BranchVec &bl, std::weak_ptr<PrevDataFrame> pd)
      : fAction(f), fBranches(bl), fTmpBranches(pd.lock()->GetTmpBranches()), fPrevData(pd.lock().get()),
        fFirstData(pd.lock()->GetDataFrame()) { }

   TDataFrameAction(const TDataFrameAction &) = delete;

   void Run(unsigned int slot, int entry)
   {
      // check if entry passes all filters
      if (CheckFilters(slot, entry)) ExecuteAction(slot, entry);
   }

   bool CheckFilters(unsigned int slot, int entry)
   {
      // start the recursive chain of CheckFilters calls
      return fPrevData->CheckFilters(slot, entry);
   }

   void ExecuteAction(unsigned int slot, int entry) { ExecuteActionHelper(slot, entry, TypeInd_t(), BranchTypes_t()); }

   void CreateSlots(unsigned int nSlots) { fReaderValues.resize(nSlots); }

   void BuildReaderValues(TTreeReader &r, unsigned int slot)
   {
      fReaderValues[slot] = ROOT::Internal::BuildReaderValues(r, fBranches, fTmpBranches, BranchTypes_t(), TypeInd_t());
   }

   template <int... S, typename... BranchTypes>
   void ExecuteActionHelper(unsigned int slot, int entry,
                            TDFTraitsUtils::TStaticSeq<S...>,
                            TDFTraitsUtils::TTypeList<BranchTypes...>)
   {
      // Take each pointer in tvb, cast it to a pointer to the
      // correct specialization of TTreeReaderValue, and get its content.
      // S expands to a sequence of integers 0 to sizeof...(types)-1
      // S and types are expanded simultaneously by "..."
      fAction(slot, GetBranchValue<S, BranchTypes>(fReaderValues[slot][S], slot, entry, fBranches[S], fFirstData)...);
   }
};

namespace Operations {
using namespace Internal::TDFTraitsUtils;
using Count_t = unsigned long;

class CountOperation {
   unsigned int *fResultCount;
   std::vector<Count_t> fCounts;

public:
   CountOperation(unsigned int *resultCount, unsigned int nSlots) : fResultCount(resultCount), fCounts(nSlots, 0) {}

   void Exec(unsigned int slot)
   {
      fCounts[slot]++;
   }

   ~CountOperation()
   {
      *fResultCount = 0;
      for (auto &c : fCounts) {
         *fResultCount += c;
      }
   }
};

class FillOperation {
   // this sets a total initial size of 16 MB for the buffers (can increase)
   static constexpr unsigned int fgTotalBufSize = 2097152;
   using BufEl_t = double;
   using Buf_t = std::vector<BufEl_t>;

   std::vector<Buf_t> fBuffers;
   std::shared_ptr<TH1F> fResultHist;
   unsigned int fBufSize;
   Buf_t fMin;
   Buf_t fMax;

   template <typename T>
   void UpdateMinMax(unsigned int slot, T v) {
      auto& thisMin = fMin[slot];
      auto& thisMax = fMax[slot];
      thisMin = std::min(thisMin, (BufEl_t)v);
      thisMax = std::max(thisMax, (BufEl_t)v);
   }

public:
   FillOperation(std::shared_ptr<TH1F> h, unsigned int nSlots) : fResultHist(h),
                                                                 fBufSize (fgTotalBufSize / nSlots),
                                                                 fMin(nSlots, std::numeric_limits<BufEl_t>::max()),
                                                                 fMax(nSlots, std::numeric_limits<BufEl_t>::min())
   {
      fBuffers.reserve(nSlots);
      for (unsigned int i=0; i<nSlots; ++i) {
         Buf_t v;
         v.reserve(fBufSize);
         fBuffers.emplace_back(v);
      }
   }

   template <typename T, typename std::enable_if<!TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(T v, unsigned int slot)
   {
      UpdateMinMax(slot, v);
      fBuffers[slot].emplace_back(v);
   }

   template <typename T, typename std::enable_if<TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(const T &vs, unsigned int slot)
   {
      auto& thisBuf = fBuffers[slot];
      for (auto& v : vs) {
         UpdateMinMax(slot, v);
         thisBuf.emplace_back(v); // TODO: Can be optimised in case T == BufEl_t
      }
   }

   ~FillOperation()
   {

      BufEl_t globalMin = *std::min_element(fMin.begin(), fMin.end());
      BufEl_t globalMax = *std::max_element(fMax.begin(), fMax.end());

      if (fResultHist->CanExtendAllAxes() &&
          globalMin != std::numeric_limits<BufEl_t>::max() &&
          globalMax != std::numeric_limits<BufEl_t>::min()) {
         auto xaxis = fResultHist->GetXaxis();
         fResultHist->ExtendAxis(globalMin, xaxis);
         fResultHist->ExtendAxis(globalMax, xaxis);
      }

      for (auto& buf : fBuffers) {
         Buf_t w(buf.size(),1); // A bug in FillN?
         fResultHist->FillN(buf.size(), buf.data(),  w.data());
      }
   }
};


class FillTOOperation {
   TThreadedObject<TH1F> fTo;

public:

   FillTOOperation(std::shared_ptr<TH1F> h, unsigned int nSlots) : fTo(*h)
   {
      fTo.SetAtSlot(0, h);
      // Initialise all other slots
      for (unsigned int i = 0 ; i < nSlots; ++i) {
         fTo.GetAtSlot(i);
      }
   }

   template <typename T, typename std::enable_if<!TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(T v, unsigned int slot)
   {
      fTo.GetAtSlotUnchecked(slot)->Fill(v);
   }

   template <typename T, typename std::enable_if<TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(const T &vs, unsigned int slot)
   {
      auto thisSlotH = fTo.GetAtSlotUnchecked(slot);
      for (auto& v : vs) {
         thisSlotH->Fill(v); // TODO: Can be optimised in case T == vector<double>
      }
   }

   ~FillTOOperation()
   {
      fTo.Merge();
   }

};

// note: changes to this class should probably be replicated in its partial
// specialization below
template<typename T, typename COLL>
class GetOperation {
   std::vector<std::shared_ptr<COLL>> fColls;
public:
   GetOperation(std::shared_ptr<COLL> resultColl, unsigned int nSlots)
   {
      fColls.emplace_back(resultColl);
      for (unsigned int i = 1; i < nSlots; ++i)
         fColls.emplace_back(std::make_shared<COLL>());
   }

   template <typename V, typename std::enable_if<!TIsContainer<V>::fgValue, int>::type = 0>
   void Exec(V v, unsigned int slot)
   {
      fColls[slot]->emplace_back(v);
   }

   template <typename V, typename std::enable_if<TIsContainer<V>::fgValue, int>::type = 0>
   void Exec(const V &vs, unsigned int slot)
   {
      auto thisColl = fColls[slot];
      thisColl.insert(std::begin(thisColl), std::begin(vs), std::begin(vs));
   }

   ~GetOperation()
   {
      auto rColl = fColls[0];
      for (unsigned int i = 1; i < fColls.size(); ++i) {
         auto& coll = fColls[i];
         for (T &v : *coll) {
            rColl->emplace_back(v);
         }
      }
   }
};

// note: changes to this class should probably be replicated in its unspecialized
// declaration above
template<typename T>
class GetOperation<T, std::vector<T>> {
   std::vector<std::shared_ptr<std::vector<T>>> fColls;
public:
   GetOperation(std::shared_ptr<std::vector<T>> resultColl, unsigned int nSlots)
   {
      fColls.emplace_back(resultColl);
      for (unsigned int i = 1; i < nSlots; ++i) {
         auto v = std::make_shared<std::vector<T>>();
         v->reserve(1024);
         fColls.emplace_back(v);
      }
   }

   template <typename V, typename std::enable_if<!TIsContainer<V>::fgValue, int>::type = 0>
   void Exec(V v, unsigned int slot)
   {
      fColls[slot]->emplace_back(v);
   }

   template <typename V, typename std::enable_if<TIsContainer<V>::fgValue, int>::type = 0>
   void Exec(const V &vs, unsigned int slot)
   {
      auto thisColl = fColls[slot];
      thisColl->insert(std::begin(thisColl), std::begin(vs), std::begin(vs));
   }

   ~GetOperation()
   {
      unsigned int totSize = 0;
      for (auto& coll : fColls) totSize += coll->size();
      auto rColl = fColls[0];
      rColl->reserve(totSize);
      for (unsigned int i = 1; i < fColls.size(); ++i) {
         auto& coll = fColls[i];
         rColl->insert(rColl->end(), coll->begin(), coll->end());
      }
   }
};

class MinOperation {
   Double_t *fResultMin;
   std::vector<double> fMins;

public:
   MinOperation(Double_t *minVPtr, unsigned int nSlots)
      : fResultMin(minVPtr), fMins(nSlots, std::numeric_limits<Double_t>::max()) { }
   template <typename T, typename std::enable_if<!TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(T v, unsigned int slot)
   {
      fMins[slot] = std::min((Double_t)v, fMins[slot]);
   }
   template <typename T, typename std::enable_if<TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(const T &vs, unsigned int slot)
   {
      for (auto &&v : vs) fMins[slot] = std::min((Double_t)v, fMins[slot]);
   }
   ~MinOperation()
   {
      *fResultMin = std::numeric_limits<Double_t>::max();
      for (auto &m : fMins) *fResultMin = std::min(m, *fResultMin);
   }
};

class MaxOperation {
   Double_t *fResultMax;
   std::vector<double> fMaxs;

public:
   MaxOperation(Double_t *maxVPtr, unsigned int nSlots)
      : fResultMax(maxVPtr), fMaxs(nSlots, std::numeric_limits<Double_t>::min()) { }
   template <typename T, typename std::enable_if<!TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(T v, unsigned int slot)
   {
      fMaxs[slot] = std::max((Double_t)v, fMaxs[slot]);
   }

   template <typename T, typename std::enable_if<TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(const T &vs, unsigned int slot)
   {
      for (auto &&v : vs) fMaxs[slot] = std::max((Double_t)v, fMaxs[slot]);
   }

   ~MaxOperation()
   {
      *fResultMax = std::numeric_limits<Double_t>::min();
      for (auto &m : fMaxs) {
         *fResultMax = std::max(m, *fResultMax);
      }
   }
};

class MeanOperation {
   Double_t *fResultMean;
   std::vector<Count_t> fCounts;
   std::vector<double> fSums;

public:
   MeanOperation(Double_t *meanVPtr, unsigned int nSlots) : fResultMean(meanVPtr), fCounts(nSlots, 0), fSums(nSlots, 0) {}
   template <typename T, typename std::enable_if<!TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(T v, unsigned int slot)
   {
      fSums[slot] += v;
      fCounts[slot] ++;
   }

   template <typename T, typename std::enable_if<TIsContainer<T>::fgValue, int>::type = 0>
   void Exec(const T &vs, unsigned int slot)
   {
      for (auto &&v : vs) {
         fSums[slot] += v;
         fCounts[slot]++;
      }
   }

   ~MeanOperation()
   {
      double sumOfSums = 0;
      for (auto &s : fSums) sumOfSums += s;
      Count_t sumOfCounts = 0;
      for (auto &c : fCounts) sumOfCounts += c;
      *fResultMean = sumOfSums / (sumOfCounts > 0 ? sumOfCounts : 1);
   }
};

} // end of NS Operations

enum class EActionType : short { kHisto1D, kMin, kMax, kMean };

} // end NS Internal

namespace Details {
// forward declarations for TDataFrameInterface
template <typename F, typename PrevData>
class TDataFrameFilter;
template <typename F, typename PrevData>
class TDataFrameBranch;
class TDataFrameImpl;
}

template <typename Proxied>
class TDataFrameInterface {
   template<typename T> friend class TDataFrameInterface;
public:
   TDataFrameInterface(const std::string &treeName, TDirectory *dirPtr, const BranchVec &defaultBranches = {});
   TDataFrameInterface(TTree &tree, const BranchVec &defaultBranches = {});

   template <typename F>
   TDataFrameInterface<Details::TDataFrameFilter<F, Proxied>> Filter(F f, const BranchVec &bl = {})
   {
      ROOT::Internal::CheckFilter(f);
      auto df = GetDataFrameChecked();
      const BranchVec &defBl = df->GetDefaultBranches();
      auto nArgs = Internal::TDFTraitsUtils::TFunctionTraits<F>::ArgTypes_t::fgSize;
      const BranchVec &actualBl = Internal::PickBranchVec(nArgs, bl, defBl);
      using DFF_t = Details::TDataFrameFilter<F, Proxied>;
      auto FilterPtr = std::make_shared<DFF_t> (f, actualBl, fProxiedPtr);
      TDataFrameInterface<DFF_t> tdf_f(FilterPtr);
      df->Book(FilterPtr);
      return tdf_f;
   }

   template <typename F>
   TDataFrameInterface<Details::TDataFrameBranch<F, Proxied>> AddBranch(const std::string &name, F expression,
                                                                        const BranchVec &bl = {})
   {
      auto df = GetDataFrameChecked();
      ROOT::Internal::CheckTmpBranch(name, df->GetTree());
      const BranchVec &defBl = df->GetDefaultBranches();
      auto nArgs = Internal::TDFTraitsUtils::TFunctionTraits<F>::ArgTypes_t::fgSize;
      const BranchVec &actualBl = Internal::PickBranchVec(nArgs, bl, defBl);
      using DFB_t = Details::TDataFrameBranch<F, Proxied>;
      auto BranchPtr = std::make_shared<DFB_t>(name, expression, actualBl, fProxiedPtr);
      TDataFrameInterface<DFB_t> tdf_b(BranchPtr);
      df->Book(BranchPtr);
      return tdf_b;
   }

   template <typename F>
   void Foreach(F f, const BranchVec &bl = {})
   {
      namespace IU = Internal::TDFTraitsUtils;
      using ArgTypes_t = typename IU::TFunctionTraits<decltype(f)>::ArgTypesNoDecay_t;
      using RetType_t = typename IU::TFunctionTraits<decltype(f)>::RetType_t;
      auto fWithSlot = IU::AddSlotParameter<RetType_t>(f, ArgTypes_t());
      ForeachSlot(fWithSlot, bl);
   }

   template<typename F>
   void ForeachSlot(F f, const BranchVec &bl = {}) {
      auto df = GetDataFrameChecked();
      const BranchVec &defBl= df->GetDefaultBranches();
      auto nArgs = Internal::TDFTraitsUtils::TFunctionTraits<F>::ArgTypes_t::fgSize;
      const BranchVec &actualBl = Internal::PickBranchVec(nArgs-1, bl, defBl);
      using DFA_t  = Internal::TDataFrameAction<decltype(f), Proxied>;
      df->Book(std::make_shared<DFA_t>(f, actualBl, fProxiedPtr));
      df->Run();
   }

   TActionResultPtr<unsigned int> Count()
   {
      auto df = GetDataFrameChecked();
      unsigned int nSlots = df->GetNSlots();
      auto c = df->MakeActionResultPtr(std::make_shared<unsigned int>(0));
      auto cPtr = c.GetUnchecked();
      auto cOp = std::make_shared<Internal::Operations::CountOperation>(cPtr, nSlots);
      auto countAction = [cOp](unsigned int slot) mutable { cOp->Exec(slot); };
      BranchVec bl = {};
      using DFA_t = Internal::TDataFrameAction<decltype(countAction), Proxied>;
      df->Book(std::shared_ptr<DFA_t>(new DFA_t(countAction, bl, fProxiedPtr)));
      return c;
   }

   template <typename T, typename COLL = std::vector<T>>
   TActionResultPtr<COLL> Get(const std::string &branchName = "")
   {
      auto df = GetDataFrameChecked();
      unsigned int nSlots = df->GetNSlots();
      auto theBranchName(branchName);
      GetDefaultBranchName(theBranchName, "get the values of the branch");
      auto valuesPtr = std::make_shared<COLL>();
      auto values = df->MakeActionResultPtr(valuesPtr);
      auto getOp = std::make_shared<Internal::Operations::GetOperation<T,COLL>>(valuesPtr, nSlots);
      auto getAction = [getOp] (unsigned int slot , const T &v) mutable { getOp->Exec(v, slot); };
      BranchVec bl = {theBranchName};
      using DFA_t = Internal::TDataFrameAction<decltype(getAction), Proxied>;
      df->Book(std::shared_ptr<DFA_t>(new DFA_t(getAction, bl, fProxiedPtr)));
      return values;
   }

   template <typename T = Double_t>
   TActionResultPtr<TH1F> Histo(const std::string &branchName, const TH1F &model)
   {
      auto theBranchName(branchName);
      GetDefaultBranchName(theBranchName, "fill the histogram");
      auto h = std::make_shared<TH1F>(model);
      return CreateAction<T, Internal::EActionType::kHisto1D>(theBranchName, h);
   }

   template <typename T = Double_t>
   TActionResultPtr<TH1F> Histo(const std::string &branchName = "", int nBins = 128, Double_t minVal = 0.,
                                Double_t maxVal = 0.)
   {
      auto theBranchName(branchName);
      GetDefaultBranchName(theBranchName, "fill the histogram");
      auto h = std::make_shared<TH1F>("", "", nBins, minVal, maxVal);
      if (minVal == maxVal) {
         h->SetCanExtend(TH1::kAllAxes);
      }
      return CreateAction<T, Internal::EActionType::kHisto1D>(theBranchName, h);
   }

   template <typename T = Double_t>
   TActionResultPtr<Double_t> Min(const std::string &branchName = "")
   {
      auto theBranchName(branchName);
      GetDefaultBranchName(theBranchName, "calculate the minumum");
      auto minV = std::make_shared<T>(std::numeric_limits<T>::max());
      return CreateAction<T, Internal::EActionType::kMin>(theBranchName, minV);
   }

   template <typename T = Double_t>
   TActionResultPtr<Double_t> Max(const std::string &branchName = "")
   {
      auto theBranchName(branchName);
      GetDefaultBranchName(theBranchName, "calculate the maximum");
      auto maxV = std::make_shared<T>(std::numeric_limits<T>::min());
      return CreateAction<T, Internal::EActionType::kMax>(theBranchName, maxV);
   }

   template <typename T = Double_t>
   TActionResultPtr<Double_t> Mean(const std::string &branchName = "")
   {
      auto theBranchName(branchName);
      GetDefaultBranchName(theBranchName, "calculate the mean");
      auto meanV = std::make_shared<T>(0);
      return CreateAction<T, Internal::EActionType::kMean>(theBranchName, meanV);
   }

private:
   TDataFrameInterface(std::shared_ptr<Proxied> proxied) : fProxiedPtr(proxied) {}

   /// Get the TDataFrameImpl if reachable. If not, throw.
   std::shared_ptr<Details::TDataFrameImpl> GetDataFrameChecked()
   {
      auto df = fProxiedPtr->GetDataFrame().lock();
      if (!df) {
         throw std::runtime_error("The main TDataFrame is not reachable: did it go out of scope?");
      }
      return df;
   }

   void GetDefaultBranchName(std::string &theBranchName, const std::string &actionNameForErr)
   {
      if (theBranchName.empty()) {
         // Try the default branch if possible
         auto df = GetDataFrameChecked();
         const BranchVec &defBl = df->GetDefaultBranches();
         if (defBl.size() == 1) {
            theBranchName = defBl[0];
         } else {
            std::string msg("No branch in input to ");
            msg += actionNameForErr;
            msg += " and default branch list has size ";
            msg += std::to_string(defBl.size());
            msg += ", need 1";
            throw std::runtime_error(msg);
         }
      }
   }

   template <typename BranchType, typename ActionResultType, enum Internal::EActionType, typename ThisType>
   struct SimpleAction {};

   template <typename BranchType, typename ThisType>
   struct SimpleAction<BranchType, TH1F, Internal::EActionType::kHisto1D, ThisType> {
      static TActionResultPtr<TH1F> BuildAndBook(ThisType thisFrame, const std::string &theBranchName,
                                                 std::shared_ptr<TH1F> h, unsigned int nSlots)
      {
         // we use a shared_ptr so that the operation has the same scope of the lambda
         // and therefore of the TDataFrameAction that contains it: merging of results
         // from different threads is performed in the operation's destructor, at the
         // moment when the TDataFrameAction is deleted by TDataFrameImpl
         BranchVec bl = {theBranchName};
         auto df = thisFrame->GetDataFrameChecked();
         auto xaxis = h->GetXaxis();
         auto hasAxisLimits = !(xaxis->GetXmin() == 0. && xaxis->GetXmax() == 0.);

         if (hasAxisLimits) {
            auto fillTOOp = std::make_shared<Internal::Operations::FillTOOperation>(h, nSlots);
            auto fillLambda = [fillTOOp](unsigned int slot, const BranchType &v) mutable { fillTOOp->Exec(v, slot); };
            using DFA_t = Internal::TDataFrameAction<decltype(fillLambda), Proxied>;
            df->Book(std::make_shared<DFA_t>(fillLambda, bl, thisFrame->fProxiedPtr));
         } else {
            auto fillOp = std::make_shared<Internal::Operations::FillOperation>(h, nSlots);
            auto fillLambda = [fillOp](unsigned int slot, const BranchType &v) mutable { fillOp->Exec(v, slot); };
            using DFA_t = Internal::TDataFrameAction<decltype(fillLambda), Proxied>;
            df->Book(std::make_shared<DFA_t>(fillLambda, bl, thisFrame->fProxiedPtr));
         }
         return df->MakeActionResultPtr(h);
      }
   };

   template <typename BranchType, typename ThisType, typename ActionResultType>
   struct SimpleAction<BranchType, ActionResultType, Internal::EActionType::kMin, ThisType> {
      static TActionResultPtr<ActionResultType> BuildAndBook(ThisType thisFrame, const std::string &theBranchName,
                                                             std::shared_ptr<ActionResultType> minV, unsigned int nSlots)
      {
         // see "TActionResultPtr<TH1F> BuildAndBook" for why this is a shared_ptr
         auto minOp = std::make_shared<Internal::Operations::MinOperation>(minV.get(), nSlots);
         auto minOpLambda = [minOp](unsigned int slot, const BranchType &v) mutable { minOp->Exec(v, slot); };
         BranchVec bl = {theBranchName};
         using DFA_t = Internal::TDataFrameAction<decltype(minOpLambda), Proxied>;
         auto df = thisFrame->GetDataFrameChecked();
         df->Book(std::make_shared<DFA_t>(minOpLambda, bl, thisFrame->fProxiedPtr));
         return df->MakeActionResultPtr(minV);
      }
   };

   template <typename BranchType, typename ThisType, typename ActionResultType>
   struct SimpleAction<BranchType, ActionResultType, Internal::EActionType::kMax, ThisType> {
      static TActionResultPtr<ActionResultType> BuildAndBook(ThisType thisFrame, const std::string &theBranchName,
                                                             std::shared_ptr<ActionResultType> maxV, unsigned int nSlots)
      {
         // see "TActionResultPtr<TH1F> BuildAndBook" for why this is a shared_ptr
         auto maxOp = std::make_shared<Internal::Operations::MaxOperation>(maxV.get(), nSlots);
         auto maxOpLambda = [maxOp](unsigned int slot, const BranchType &v) mutable { maxOp->Exec(v, slot); };
         BranchVec bl = {theBranchName};
         using DFA_t = Internal::TDataFrameAction<decltype(maxOpLambda), Proxied>;
         auto df = thisFrame->GetDataFrameChecked();
         df->Book(std::make_shared<DFA_t>(maxOpLambda, bl, thisFrame->fProxiedPtr));
         return df->MakeActionResultPtr(maxV);
      }
   };

   template <typename BranchType, typename ThisType, typename ActionResultType>
   struct SimpleAction<BranchType, ActionResultType, Internal::EActionType::kMean, ThisType> {
      static TActionResultPtr<ActionResultType> BuildAndBook(ThisType thisFrame, const std::string &theBranchName,
                                                             std::shared_ptr<ActionResultType> meanV, unsigned int nSlots)
      {
         // see "TActionResultPtr<TH1F> BuildAndBook" for why this is a shared_ptr
         auto meanOp = std::make_shared<Internal::Operations::MeanOperation>(meanV.get(), nSlots);
         auto meanOpLambda = [meanOp](unsigned int slot, const BranchType &v) mutable { meanOp->Exec(v, slot); };
         BranchVec bl = {theBranchName};
         using DFA_t = Internal::TDataFrameAction<decltype(meanOpLambda), Proxied>;
         auto df = thisFrame->GetDataFrameChecked();
         df->Book(std::make_shared<DFA_t>(meanOpLambda, bl, thisFrame->fProxiedPtr));
         return df->MakeActionResultPtr(meanV);
      }
   };

   template <typename BranchType, Internal::EActionType ActionType, typename ActionResultType>
   TActionResultPtr<ActionResultType> CreateAction(const std::string & theBranchName,
                                                   std::shared_ptr<ActionResultType> r)
   {
      // More types can be added at will at the cost of some compilation time and size of binaries.
      using ART_t = ActionResultType;
      using TT_t = decltype(this);
      const auto at = ActionType;
      auto df = GetDataFrameChecked();
      auto tree = static_cast<TTree*>(df->GetDirectory()->Get(df->GetTreeName().c_str()));
      auto branch = tree->GetBranch(theBranchName.c_str());
      unsigned int nSlots = df->GetNSlots();
      if (!branch) {
         // temporary branch
         const auto &type_id = df->GetBookedBranch(theBranchName).GetTypeId();
         if (type_id == typeid(Char_t)) {
            return SimpleAction<Char_t, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         } else if (type_id == typeid(int)) {
            return SimpleAction<int, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         } else if (type_id == typeid(Double_t)) {
            return SimpleAction<Double_t, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         } else if (type_id == typeid(Double_t)) {
            return SimpleAction<Double_t, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         } else if (type_id == typeid(std::vector<Double_t>)) {
            return SimpleAction<std::vector<Double_t>, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         } else if (type_id == typeid(std::vector<Float_t>)) {
            return SimpleAction<std::vector<Float_t>, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         }
      }
      // real branch
      auto branchEl = dynamic_cast<TBranchElement *>(branch);
      if (!branchEl) { // This is a fundamental type
         auto title    = branch->GetTitle();
         auto typeCode = title[strlen(title) - 1];
         if (typeCode == 'B') {
            return SimpleAction<Char_t, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         }
         // else if (typeCode == 'b') { return SimpleAction<UChar_t, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots); }
         // else if (typeCode == 'S') { return SimpleAction<Short_t, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots); }
         // else if (typeCode == 's') { return SimpleAction<UShort_t, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots); }
         else if (typeCode == 'I') {
            return SimpleAction<int, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         }
         // else if (typeCode == 'i') { return SimpleAction<unsigned int , ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots); }
         // else if (typeCode == 'F') { return SimpleAction<Float_t, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots); }
         else if (typeCode == 'D') {
            return SimpleAction<Double_t, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         }
         // else if (typeCode == 'L') { return SimpleAction<Long64_t, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots); }
         // else if (typeCode == 'l') { return SimpleAction<ULong64_t, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots); }
         else if (typeCode == 'O') {
            return SimpleAction<bool, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         }
      } else {
         std::string typeName = branchEl->GetTypeName();
         if (typeName == "vector<double>") {
            return SimpleAction<std::vector<Double_t>, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         } else if (typeName == "vector<float>") {
            return SimpleAction<std::vector<Float_t>, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
         }
      }
      return SimpleAction<BranchType, ART_t, at, TT_t>::BuildAndBook(this, theBranchName, r, nSlots);
   }

   std::shared_ptr<Proxied> fProxiedPtr;
};

using TDataFrame = TDataFrameInterface<ROOT::Details::TDataFrameImpl>;

namespace Details {

class TDataFrameBranchBase {
public:
   virtual ~TDataFrameBranchBase() {}
   virtual void BuildReaderValues(TTreeReader &r, unsigned int slot) = 0;
   virtual void CreateSlots(unsigned int nSlots) = 0;
   virtual std::string GetName() const       = 0;
   virtual void *GetValue(unsigned int slot, int entry) = 0;
   virtual const std::type_info &GetTypeId() const = 0;
};
using TmpBranchBasePtr_t = std::shared_ptr<TDataFrameBranchBase>;

template <typename F, typename PrevData>
class TDataFrameBranch final : public TDataFrameBranchBase {
   using BranchTypes_t = typename Internal
   ::TDFTraitsUtils::TFunctionTraits<F>::ArgTypes_t;
   using TypeInd_t = typename Internal::TDFTraitsUtils::TGenStaticSeq<BranchTypes_t::fgSize>::Type_t;
   using RetType_t = typename Internal::TDFTraitsUtils::TFunctionTraits<F>::RetType_t;

   const std::string fName;
   F fExpression;
   const BranchVec fBranches;
   BranchVec fTmpBranches;
   std::vector<ROOT::Internal::TVBVec_t> fReaderValues;
   std::vector<std::shared_ptr<RetType_t>> fLastResultPtr;
   std::weak_ptr<TDataFrameImpl> fFirstData;
   PrevData *fPrevData;
   std::vector<int> fLastCheckedEntry = {-1};

public:
   TDataFrameBranch(const std::string &name, F expression, const BranchVec &bl, std::shared_ptr<PrevData> pd)
      : fName(name), fExpression(expression), fBranches(bl), fTmpBranches(pd->GetTmpBranches()),
        fFirstData(pd->GetDataFrame()), fPrevData(pd.get())
   {
      fTmpBranches.emplace_back(name);
   }

   TDataFrameBranch(const TDataFrameBranch &) = delete;

   std::weak_ptr<TDataFrameImpl> GetDataFrame() const { return fFirstData; }

   BranchVec GetTmpBranches() const { return fTmpBranches; }

   void BuildReaderValues(TTreeReader &r, unsigned int slot)
   {
      fReaderValues[slot] = Internal::BuildReaderValues(r, fBranches, fTmpBranches, BranchTypes_t(), TypeInd_t());
   }

   void *GetValue(unsigned int slot, int entry)
   {
      if (entry != fLastCheckedEntry[slot]) {
         // evaluate this filter, cache the result
         auto newValuePtr = GetValueHelper(BranchTypes_t(), TypeInd_t(), slot, entry);
         fLastResultPtr[slot] = newValuePtr;
         fLastCheckedEntry[slot] = entry;
      }
      return static_cast<void *>(fLastResultPtr[slot].get());
   }

   const std::type_info &GetTypeId() const { return typeid(RetType_t); }

   void CreateSlots(unsigned int nSlots)
   {
      fReaderValues.resize(nSlots);
      fLastCheckedEntry.resize(nSlots);
      fLastResultPtr.resize(nSlots);
   }

   bool CheckFilters(unsigned int slot, int entry)
   {
      // dummy call: it just forwards to the previous object in the chain
      return fPrevData->CheckFilters(slot, entry);
   }

   std::string GetName() const { return fName; }

   template <int... S, typename... BranchTypes>
   std::shared_ptr<RetType_t> GetValueHelper(Internal::TDFTraitsUtils::TTypeList<BranchTypes...>,
                                             Internal::TDFTraitsUtils::TStaticSeq<S...>,
                                             unsigned int slot, int entry)
   {
      auto valuePtr = std::make_shared<RetType_t>(fExpression(
         Internal::GetBranchValue<S, BranchTypes>(fReaderValues[slot][S], slot, entry, fBranches[S], fFirstData)...));
      return valuePtr;
   }
};

class TDataFrameFilterBase {
public:
   virtual ~TDataFrameFilterBase() {}
   virtual void BuildReaderValues(TTreeReader &r, unsigned int slot) = 0;
   virtual void CreateSlots(unsigned int nSlots) = 0;
};
using FilterBasePtr_t = std::shared_ptr<TDataFrameFilterBase>;
using FilterBaseVec_t = std::vector<FilterBasePtr_t>;

template <typename FilterF, typename PrevDataFrame>
class TDataFrameFilter final : public TDataFrameFilterBase {
   using BranchTypes_t = typename Internal::TDFTraitsUtils::TFunctionTraits<FilterF>::ArgTypes_t;
   using TypeInd_t = typename Internal::TDFTraitsUtils::TGenStaticSeq<BranchTypes_t::fgSize>::Type_t;

   FilterF fFilter;
   const BranchVec fBranches;
   const BranchVec fTmpBranches;
   PrevDataFrame *fPrevData;
   std::weak_ptr<TDataFrameImpl> fFirstData;
   std::vector<Internal::TVBVec_t> fReaderValues = {};
   std::vector<int> fLastCheckedEntry = {-1};
   std::vector<int> fLastResult = {true}; // std::vector<bool> cannot be used in a MT context safely

public:
   TDataFrameFilter(FilterF f, const BranchVec &bl, std::shared_ptr<PrevDataFrame> pd)
      : fFilter(f), fBranches(bl), fTmpBranches(pd->GetTmpBranches()), fPrevData(pd.get()),
        fFirstData(pd->GetDataFrame()) { }

   std::weak_ptr<TDataFrameImpl> GetDataFrame() const { return fFirstData; }

   BranchVec GetTmpBranches() const { return fTmpBranches; }

   TDataFrameFilter(const TDataFrameFilter &) = delete;

   bool CheckFilters(unsigned int slot, int entry)
   {
      if (entry != fLastCheckedEntry[slot]) {
         if (!fPrevData->CheckFilters(slot, entry)) {
            // a filter upstream returned false, cache the result
            fLastResult[slot] = false;
         } else {
            // evaluate this filter, cache the result
            fLastResult[slot] = CheckFilterHelper(BranchTypes_t(), TypeInd_t(), slot, entry);
         }
         fLastCheckedEntry[slot] = entry;
      }
      return fLastResult[slot];
   }

   template <int... S, typename... BranchTypes>
   bool CheckFilterHelper(Internal::TDFTraitsUtils::TTypeList<BranchTypes...>,
                          Internal::TDFTraitsUtils::TStaticSeq<S...>,
                          unsigned int slot, int entry)
   {
      // Take each pointer in tvb, cast it to a pointer to the
      // correct specialization of TTreeReaderValue, and get its content.
      // S expands to a sequence of integers 0 to sizeof...(types)-1
      // S and types are expanded simultaneously by "..."
      return fFilter(
         Internal::GetBranchValue<S, BranchTypes>(fReaderValues[slot][S], slot, entry, fBranches[S], fFirstData)...);
   }

   void BuildReaderValues(TTreeReader &r, unsigned int slot)
   {
      fReaderValues[slot] = Internal::BuildReaderValues(r, fBranches, fTmpBranches, BranchTypes_t(), TypeInd_t());
   }

   void CreateSlots(unsigned int nSlots)
   {
      fReaderValues.resize(nSlots);
      fLastCheckedEntry.resize(nSlots);
      fLastResult.resize(nSlots);
   }
};

class TDataFrameImpl {

   Internal::ActionBaseVec_t fBookedActions;
   Details::FilterBaseVec_t fBookedFilters;
   std::map<std::string, TmpBranchBasePtr_t> fBookedBranches;
   std::vector<std::shared_ptr<bool>> fResPtrsReadiness;
   std::string fTreeName;
   TDirectory *fDirPtr = nullptr;
   TTree *fTree = nullptr;
   const BranchVec fDefaultBranches;
   // always empty: each object in the chain copies this list from the previous
   // and they must copy an empty list from the base TDataFrameImpl
   const BranchVec fTmpBranches;
   unsigned int fNSlots;
   // TDataFrameInterface<TDataFrameImpl> calls SetFirstData to set this to a
   // weak pointer to the TDataFrameImpl object itself
   // so subsequent objects in the chain can call GetDataFrame on TDataFrameImpl
   std::weak_ptr<TDataFrameImpl> fFirstData;

public:
   TDataFrameImpl(const std::string &treeName, TDirectory *dirPtr, const BranchVec &defaultBranches = {})
      : fTreeName(treeName), fDirPtr(dirPtr), fDefaultBranches(defaultBranches), fNSlots(ROOT::Internal::GetNSlots()) { }

   TDataFrameImpl(TTree &tree, const BranchVec &defaultBranches = {}) : fTree(&tree), fDefaultBranches(defaultBranches), fNSlots(ROOT::Internal::GetNSlots())
   { }

   TDataFrameImpl(const TDataFrameImpl &) = delete;

   void Run()
   {
#ifdef R__USE_IMT
      if (ROOT::IsImplicitMTEnabled()) {
         const auto fileName = fTree ? static_cast<TFile *>(fTree->GetCurrentFile())->GetName() : fDirPtr->GetName();
         const std::string    treeName = fTree ? fTree->GetName() : fTreeName;
         ROOT::TTreeProcessor tp(fileName, treeName);
         ROOT::TSpinMutex     slotMutex;
         std::map<std::thread::id, unsigned int> slotMap;
         unsigned int globalSlotIndex = 0;
         CreateSlots(fNSlots);
         tp.Process([this, &slotMutex, &globalSlotIndex, &slotMap](TTreeReader &r) -> void {
            const auto thisThreadID = std::this_thread::get_id();
            unsigned int slot;
            {
               std::lock_guard<ROOT::TSpinMutex> l(slotMutex);
               auto thisSlotIt = slotMap.find(thisThreadID);
               if (thisSlotIt != slotMap.end()) {
                  slot = thisSlotIt->second;
               } else {
                  slot = globalSlotIndex;
                  slotMap[thisThreadID] = slot;
                  ++globalSlotIndex;
               }
            }

            BuildAllReaderValues(r, slot);

            // recursive call to check filters and conditionally execute actions
            while (r.Next())
               for (auto &actionPtr : fBookedActions)
                  actionPtr->Run(slot, r.GetCurrentEntry());
         });
      } else {
#endif // R__USE_IMT
         TTreeReader r;
         if (fTree) {
            r.SetTree(fTree);
         } else {
            r.SetTree(fTreeName.c_str(), fDirPtr);
         }

         CreateSlots(1);
         BuildAllReaderValues(r, 0);

         // recursive call to check filters and conditionally execute actions
         while (r.Next())
            for (auto &actionPtr : fBookedActions)
               actionPtr->Run(0, r.GetCurrentEntry());
#ifdef R__USE_IMT
      }
#endif // R__USE_IMT

      // forget actions and "detach" the action result pointers marking them ready and forget them too
      fBookedActions.clear();
      for (auto readiness : fResPtrsReadiness) {
         *readiness.get() = true;
      }
      fResPtrsReadiness.clear();
   }

   // build reader values for all actions, filters and branches
   void BuildAllReaderValues(TTreeReader &r, unsigned int slot)
   {
      for (auto &ptr : fBookedActions) ptr->BuildReaderValues(r, slot);
      for (auto &ptr : fBookedFilters) ptr->BuildReaderValues(r, slot);
      for (auto &bookedBranch : fBookedBranches) bookedBranch.second->BuildReaderValues(r, slot);
   }

   // inform all actions filters and branches of the required number of slots
   void CreateSlots(unsigned int nSlots)
   {
      for (auto &ptr : fBookedActions) ptr->CreateSlots(nSlots);
      for (auto &ptr : fBookedFilters) ptr->CreateSlots(nSlots);
      for (auto &bookedBranch : fBookedBranches) bookedBranch.second->CreateSlots(nSlots);
   }

   std::weak_ptr<Details::TDataFrameImpl> GetDataFrame() const { return fFirstData; }

   const BranchVec &GetDefaultBranches() const { return fDefaultBranches; }

   const BranchVec GetTmpBranches() const { return fTmpBranches; }

   TTree* GetTree() const {
      if (fTree) {
         return fTree;
      } else {
         auto treePtr = static_cast<TTree*>(fDirPtr->Get(fTreeName.c_str()));
         return treePtr;
      }
   }

   const TDataFrameBranchBase &GetBookedBranch(const std::string &name) const
   {
      return *fBookedBranches.find(name)->second.get();
   }

   void *GetTmpBranchValue(const std::string &branch, unsigned int slot, int entry)
   {
      return fBookedBranches.at(branch)->GetValue(slot, entry);
   }

   TDirectory *GetDirectory() const { return fDirPtr; }

   std::string GetTreeName() const { return fTreeName; }

   void SetFirstData(const std::shared_ptr<TDataFrameImpl>& sp) { fFirstData = sp; }

   void Book(Internal::ActionBasePtr_t actionPtr) { fBookedActions.emplace_back(actionPtr); }

   void Book(Details::FilterBasePtr_t filterPtr) { fBookedFilters.emplace_back(filterPtr); }

   void Book(TmpBranchBasePtr_t branchPtr) { fBookedBranches[branchPtr->GetName()] = branchPtr; }

   // dummy call, end of recursive chain of calls
   bool CheckFilters(int, unsigned int) { return true; }

   unsigned int GetNSlots() {return fNSlots;}

   template<typename T>
   TActionResultPtr<T> MakeActionResultPtr(std::shared_ptr<T> r)
   {
      auto readiness = std::make_shared<bool>(false);
      // since fFirstData is a weak_ptr to `this`, we are sure the lock succeeds
      auto df = fFirstData.lock();
      auto resPtr = TActionResultPtr<T>::MakeActionResultPtr(r, readiness, df);
      fResPtrsReadiness.emplace_back(readiness);
      return resPtr;
   }
};

} // end NS Details

} // end NS ROOT

// Functions and method implementations
namespace ROOT {
template <typename T>
TDataFrameInterface<T>::TDataFrameInterface(const std::string &treeName, TDirectory *dirPtr,
                                            const BranchVec &defaultBranches)
   : fProxiedPtr(std::make_shared<Details::TDataFrameImpl>(treeName, dirPtr, defaultBranches))
{
   fProxiedPtr->SetFirstData(fProxiedPtr);
}

template <typename T>
TDataFrameInterface<T>::TDataFrameInterface(TTree &tree, const BranchVec &defaultBranches)
   : fProxiedPtr(std::make_shared<Details::TDataFrameImpl>(tree, defaultBranches))
{
   fProxiedPtr->SetFirstData(fProxiedPtr);
}

template<typename T>
void TActionResultPtr<T>::TriggerRun()
{
   auto df = fFirstData.lock();
   if (!df) {
      throw std::runtime_error("The main TDataFrame is not reachable: did it go out of scope?");
   }
   df->Run();
}

namespace Internal {
template <int S, typename T>
T &GetBranchValue(TVBPtr_t &readerValue, unsigned int slot, int entry, const std::string &branch,
                  std::weak_ptr<Details::TDataFrameImpl> df)
{
   if (readerValue == nullptr) {
      // temporary branch
      void *tmpBranchVal = df.lock()->GetTmpBranchValue(branch, slot, entry);
      return *static_cast<T *>(tmpBranchVal);
   } else {
      // real branch
      return **std::static_pointer_cast<TTreeReaderValue<T>>(readerValue);
   }
}

} // end NS Internal

} // end NS ROOT

// FIXME: need to rethink the printfunction

#endif // ROOT_TDATAFRAME
