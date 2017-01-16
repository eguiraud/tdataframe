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
#include <list>
#include <string>
#include <thread>
#include <type_traits> // std::decay
#include <typeinfo>
#include <utility> // std::move
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

// returned wrapper around f that adds the unsigned int slot parameter
template <typename R, typename F, typename... Args>
std::function<R(unsigned int, Args...)> AddSlotParameter(F f, TTypeList<Args...>)
{
   return [f](unsigned int, Args... a) -> R { return f(a...); };
}

// compile-time integer sequence generator
// e.g. calling TGenS<3>::type() instantiates a TSeq<0,1,2>
template <int...>
struct TSeq { };

template <int N, int... S>
struct TGenS : TGenS<N - 1, N - 1, S...> { };

template <int... S>
struct TGenS<0, S...> {
   using Type_t = TSeq<S...>;
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

// Result pointer: can move below?
namespace ROOT {

using BranchList = std::vector<std::string>;

// Fwd declarations
namespace Details {
class TDataFrameImpl;
}

// Smart pointer for the return type of actions
class TActionResultPtrBase {
   friend class Details::TDataFrameImpl;
   std::weak_ptr<Details::TDataFrameImpl> fFirstData;
   std::shared_ptr<bool> fReadyPtr = std::make_shared<bool>(false);
   void SetReady() { *fReadyPtr = true; }
protected:
   bool IsReady() { return *fReadyPtr; }
   void TriggerRun();

public:
   TActionResultPtrBase(std::weak_ptr<Details::TDataFrameImpl> firstData);
   virtual ~TActionResultPtrBase() {}
};

template <typename T>
class TActionResultPtr : public TActionResultPtrBase {
   std::shared_ptr<T> fObjPtr;

public:
   TActionResultPtr(std::shared_ptr<T> objPtr, std::weak_ptr<Details::TDataFrameImpl> firstData)
      : TActionResultPtrBase(firstData), fObjPtr(objPtr) { }
   T *Get()
   {
      if (!IsReady()) TriggerRun();
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
TVBVec_t BuildReaderValues(TTreeReader &r, const BranchList &bl, const BranchList &tmpbl,
                           TDFTraitsUtils::TTypeList<BranchTypes...>,
                           TDFTraitsUtils::TSeq<S...>)
{
   // isTmpBranch has length bl.size(). Elements are true if the corresponding
   // branch is a "fake" branch created with AddBranch, false if they are
   // actual branches present in the TTree.
   std::array<bool, sizeof...(S)> isTmpBranch;
   for (unsigned int i   = 0; i < isTmpBranch.size(); ++i)
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

template <typename F>
const BranchList &PickBranchList(F f, const BranchList &bl, const BranchList &defBl)
{
   // return local BranchList or default BranchList according to which one
   // should be used
   auto nArgs = TDFTraitsUtils::TFunctionTraits<F>::ArgTypes_t::fgSize;
   bool useDefBl = false;
   if (nArgs != bl.size()) {
      if (bl.size() == 0 && nArgs == defBl.size()) {
         useDefBl = true;
      } else {
         auto msg = "mismatch between number of filter arguments (" + std::to_string(nArgs) +
                    ") and number of branches (" + std::to_string(bl.size() ?: defBl.size()) + ")";
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
class TDataFrameAction : public TDataFrameActionBase {
   using BranchTypes_t = typename TDFTraitsUtils::TRemoveFirst<typename TDFTraitsUtils::TFunctionTraits<F>::ArgTypes_t>::Types_t;
   using TypeInd_t = typename TDFTraitsUtils::TGenS<BranchTypes_t::fgSize>::Type_t;

public:
   TDataFrameAction(F f, const BranchList &bl, std::weak_ptr<PrevDataFrame> pd)
      : fAction(f), fBranches(bl), fTmpBranches(pd.lock()->fTmpBranches), fPrevData(pd.lock().get()),
        fFirstData(pd.lock()->fFirstData) { }

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
                            TDFTraitsUtils::TSeq<S...>,
                            TDFTraitsUtils::TTypeList<BranchTypes...>)
   {
      // Take each pointer in tvb, cast it to a pointer to the
      // correct specialization of TTreeReaderValue, and get its content.
      // S expands to a sequence of integers 0 to sizeof...(types)-1
      // S and types are expanded simultaneously by "..."
      fAction(slot, GetBranchValue<S, BranchTypes>(fReaderValues[slot][S], slot, entry, fBranches[S], fFirstData)...);
   }

   F fAction;
   const BranchList fBranches;
   const BranchList fTmpBranches;
   PrevDataFrame *fPrevData;
   std::weak_ptr<Details::TDataFrameImpl> fFirstData;
   std::vector<TVBVec_t> fReaderValues;
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
   static constexpr unsigned int fgTotalBufSize = 2097152;
   using BufEl_t = double;
   using Buf_t = std::vector<BufEl_t>;

   std::vector<Buf_t> fBuffers;
   unsigned int fBufSize;
   std::shared_ptr<TH1F> fResultHist;
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
      *fResultMean = sumOfSums / sumOfCounts;
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
public:
   TDataFrameInterface() = delete;
   TDataFrameInterface(std::shared_ptr<Proxied> proxied) : fProxiedPtr(proxied) {}
   TDataFrameInterface(const std::string &treeName, TDirectory *dirPtr, const BranchList &defaultBranches = {});
   TDataFrameInterface(TTree &tree, const BranchList &defaultBranches = {});

   template <typename F>
   TDataFrameInterface<Details::TDataFrameFilter<F, Proxied>> Filter(F f, const BranchList &bl = {})
   {
      ROOT::Internal::CheckFilter(f);
      const BranchList &defBl = fProxiedPtr->GetDataFrame().lock()->GetDefaultBranches();
      const BranchList &actualBl = Internal::PickBranchList(f, bl, defBl);
      using DFF_t = Details::TDataFrameFilter<F, Proxied>;
      auto FilterPtr = std::make_shared<DFF_t> (f, actualBl, fProxiedPtr);
      TDataFrameInterface<DFF_t> tdf_f(FilterPtr);
      Book(FilterPtr);
      return tdf_f;
   }

   template <typename F>
   TDataFrameInterface<Details::TDataFrameBranch<F, Proxied>> AddBranch(const std::string &name, F expression,
                                                                        const BranchList &bl = {})
   {
      const BranchList &defBl = fProxiedPtr->GetDataFrame().lock()->GetDefaultBranches();
      const BranchList &actualBl = Internal::PickBranchList(expression, bl, defBl);
      using DFB_t = Details::TDataFrameBranch<F, Proxied>;
      auto BranchPtr = std::make_shared<DFB_t>(name, expression, actualBl, fProxiedPtr);
      TDataFrameInterface<DFB_t> tdf_b(BranchPtr);
      Book(BranchPtr);
      return tdf_b;
   }

   template <typename F>
   void Foreach(F f, const BranchList &bl = {})
   {
      GetDataFrameChecked();
      const BranchList &defBl= fProxiedPtr->GetDataFrame().lock()->GetDefaultBranches();
      const BranchList &actualBl = Internal::PickBranchList(f, bl, defBl);
      namespace IU = Internal::TDFTraitsUtils;
      using ArgTypes_t = typename IU::TFunctionTraits<decltype(f)>::ArgTypesNoDecay_t;
      using RetType_t = typename IU::TFunctionTraits<decltype(f)>::RetType_t;
      auto fWithSlot = IU::AddSlotParameter<RetType_t>(f, ArgTypes_t());
      using DFA_t  = Internal::TDataFrameAction<decltype(fWithSlot), Proxied>;
      Book(std::make_shared<DFA_t>(fWithSlot, actualBl, fProxiedPtr));
      fProxiedPtr->GetDataFrame().lock()->Run();
   }

   TActionResultPtr<unsigned int> Count()
   {
      auto df = GetDataFrameChecked();
      unsigned int nSlots = df.lock()->GetNSlots();
      TActionResultPtr<unsigned int> c (std::make_shared<unsigned int>(0), df);
      auto cPtr = c.GetUnchecked();
      auto cOp = std::make_shared<Internal::Operations::CountOperation>(cPtr, nSlots);
      auto countAction = [cOp](unsigned int slot) mutable { cOp->Exec(slot); };
      BranchList bl = {};
      using DFA_t = Internal::TDataFrameAction<decltype(countAction), Proxied>;
      Book(std::shared_ptr<DFA_t>(new DFA_t(countAction, bl, fProxiedPtr)));
      return c;
   }

   template <typename T, typename COLL = std::list<T>>
   TActionResultPtr<COLL> Get(const std::string &branchName = "")
   {
      auto df = GetDataFrameChecked();
      unsigned int nSlots = df.lock()->GetNSlots();
      auto theBranchName(branchName);
      GetDefaultBranchName(theBranchName, "get the values of the branch");
      auto valuesPtr = std::make_shared<COLL>();
      TActionResultPtr<COLL> values(valuesPtr, df);
      auto getOp = std::make_shared<Internal::Operations::GetOperation<T,COLL>>(valuesPtr, nSlots);
      auto getAction = [getOp] (unsigned int slot , const T &v) mutable { getOp->Exec(v, slot); };
      BranchList bl = {theBranchName};
      using DFA_t = Internal::TDataFrameAction<decltype(getAction), Proxied>;
      Book(std::shared_ptr<DFA_t>(new DFA_t(getAction, bl, fProxiedPtr)));
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
   /// Get the TDataFrameImpl if reachable. If not, throw.
   std::weak_ptr<Details::TDataFrameImpl> GetDataFrameChecked()
   {
      auto df = fProxiedPtr->GetDataFrame();
      if (df.expired()) {
         throw std::runtime_error("The main TDataFrame is not reachable: did it go out of scope?");
      }
      return df;
   }

   void GetDefaultBranchName(std::string &theBranchName, const std::string &actionNameForErr)
   {
      if (theBranchName.empty()) {
         // Try the default branch if possible
         const BranchList &defBl = fProxiedPtr->GetDataFrame().lock()->GetDefaultBranches();
         if (defBl.size() == 1) {
            theBranchName = defBl[0];
         } else {
            std::string msg("No branch in input to ");
            msg += actionNameForErr;
            msg += " and more than one default branch.";
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
         // Here we pass the shared_ptr and not the pointer
         auto fillOp = std::make_shared<Internal::Operations::FillOperation>(h, nSlots);
         auto fillLambda = [fillOp](unsigned int slot, const BranchType &v) mutable { fillOp->Exec(v, slot); };
         BranchList bl = {theBranchName};
         using DFA_t = Internal::TDataFrameAction<decltype(fillLambda), Proxied>;
         thisFrame->Book(std::make_shared<DFA_t>(fillLambda, bl, thisFrame->fProxiedPtr));
         return TActionResultPtr<TH1F>(h, thisFrame->GetDataFrameChecked());
      }
   };

   template <typename BranchType, typename ThisType, typename ActionResultType>
   struct SimpleAction<BranchType, ActionResultType, Internal::EActionType::kMin, ThisType> {
      static TActionResultPtr<ActionResultType> BuildAndBook(ThisType thisFrame, const std::string &theBranchName,
                                                             std::shared_ptr<ActionResultType> minV, unsigned int nSlots)
      {
         auto minOp = std::make_shared<Internal::Operations::MinOperation>(minV.get(), nSlots);
         auto minOpLambda = [minOp](unsigned int slot, const BranchType &v) mutable { minOp->Exec(v, slot); };
         BranchList bl = {theBranchName};
         using DFA_t = Internal::TDataFrameAction<decltype(minOpLambda), Proxied>;
         thisFrame->Book(std::make_shared<DFA_t>(minOpLambda, bl, thisFrame->fProxiedPtr));
         return TActionResultPtr<ActionResultType>(minV, thisFrame->GetDataFrameChecked());
      }
   };

   template <typename BranchType, typename ThisType, typename ActionResultType>
   struct SimpleAction<BranchType, ActionResultType, Internal::EActionType::kMax, ThisType> {
      static TActionResultPtr<ActionResultType> BuildAndBook(ThisType thisFrame, const std::string &theBranchName,
                                                             std::shared_ptr<ActionResultType> maxV, unsigned int nSlots)
      {
         auto maxOp = std::make_shared<Internal::Operations::MaxOperation>(maxV.get(), nSlots);
         auto maxOpLambda = [maxOp](unsigned int slot, const BranchType &v) mutable { maxOp->Exec(v, slot); };
         BranchList bl = {theBranchName};
         using DFA_t = Internal::TDataFrameAction<decltype(maxOpLambda), Proxied>;
         thisFrame->Book(std::make_shared<DFA_t>(maxOpLambda, bl, thisFrame->fProxiedPtr));
         return TActionResultPtr<ActionResultType>(maxV, thisFrame->GetDataFrameChecked());
      }
   };

   template <typename BranchType, typename ThisType, typename ActionResultType>
   struct SimpleAction<BranchType, ActionResultType, Internal::EActionType::kMean, ThisType> {
      static TActionResultPtr<ActionResultType> BuildAndBook(ThisType thisFrame, const std::string &theBranchName,
                                                             std::shared_ptr<ActionResultType> meanV, unsigned int nSlots)
      {
         auto meanOp = std::make_shared<Internal::Operations::MeanOperation>(meanV.get(), nSlots);
         auto meanOpLambda = [meanOp](unsigned int slot, const BranchType &v) mutable { meanOp->Exec(v, slot); };
         BranchList bl = {theBranchName};
         using DFA_t = Internal::TDataFrameAction<decltype(meanOpLambda), Proxied>;
         thisFrame->Book(std::make_shared<DFA_t>(meanOpLambda, bl, thisFrame->fProxiedPtr));
         return TActionResultPtr<ActionResultType>(meanV, thisFrame->GetDataFrameChecked());
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
      auto tree = (TTree *)df.lock()->GetDirectory()->Get(df.lock()->GetTreeName().c_str());
      auto branch = tree->GetBranch(theBranchName.c_str());
      unsigned int nSlots = df.lock()->GetNSlots();
      if (!branch) {
         // temporary branch
         const auto &type_id = df.lock()->GetBookedBranch(theBranchName).GetTypeId();
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
   template <typename T>
   void Book(std::shared_ptr<T> ptr)
   {
      fProxiedPtr->Book(ptr);
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
class TDataFrameBranch : public TDataFrameBranchBase {
   using BranchTypes_t = typename Internal
   ::TDFTraitsUtils::TFunctionTraits<F>::ArgTypes_t;
   using TypeInd_t = typename Internal::TDFTraitsUtils::TGenS<BranchTypes_t::fgSize>::Type_t;
   using RetType_t = typename Internal::TDFTraitsUtils::TFunctionTraits<F>::RetType_t;

public:
   TDataFrameBranch(const std::string &name, F expression, const BranchList &bl, std::weak_ptr<PrevData> pd)
      : fName(name), fExpression(expression), fBranches(bl), fTmpBranches(pd.lock()->fTmpBranches),
        fFirstData(pd.lock()->fFirstData), fPrevData(pd.lock().get())
   {
      fTmpBranches.emplace_back(name);
   }

   TDataFrameBranch(const TDataFrameBranch &) = delete;

   std::weak_ptr<TDataFrameImpl> GetDataFrame() const { return fFirstData; }

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

   template <typename T>
   void Book(std::shared_ptr<T> ptr);

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
                                             Internal::TDFTraitsUtils::TSeq<S...>,
                                             unsigned int slot, int entry)
   {
      auto valuePtr = std::make_shared<RetType_t>(fExpression(
         Internal::GetBranchValue<S, BranchTypes>(fReaderValues[slot][S], slot, entry, fBranches[S], fFirstData)...));
      return valuePtr;
   }

   const std::string fName;
   F fExpression;
   const BranchList fBranches;
   BranchList fTmpBranches;
   std::vector<ROOT::Internal::TVBVec_t> fReaderValues;
   std::vector<std::shared_ptr<RetType_t>> fLastResultPtr;
   std::weak_ptr<TDataFrameImpl> fFirstData;
   PrevData *fPrevData;
   std::vector<int> fLastCheckedEntry = {-1};
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
class TDataFrameFilter : public TDataFrameFilterBase {
   using BranchTypes_t = typename Internal::TDFTraitsUtils::TFunctionTraits<FilterF>::ArgTypes_t;
   using TypeInd_t = typename Internal::TDFTraitsUtils::TGenS<BranchTypes_t::fgSize>::Type_t;

public:
   TDataFrameFilter(FilterF f, const BranchList &bl, std::weak_ptr<PrevDataFrame> pd)
      : fFilter(f), fBranches(bl), fTmpBranches(pd.lock()->fTmpBranches), fPrevData(pd.lock().get()),
        fFirstData(pd.lock()->fFirstData) { }

   std::weak_ptr<TDataFrameImpl> GetDataFrame() const { return fFirstData; }

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
                          Internal::TDFTraitsUtils::TSeq<S...>,
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

   template <typename T>
   void Book(std::shared_ptr<T> ptr);

   void CreateSlots(unsigned int nSlots)
   {
      fReaderValues.resize(nSlots);
      fLastCheckedEntry.resize(nSlots);
      fLastResult.resize(nSlots);
   }

   FilterF fFilter;
   const BranchList fBranches;
   const BranchList fTmpBranches;
   PrevDataFrame *fPrevData;
   std::weak_ptr<TDataFrameImpl> fFirstData;
   std::vector<Internal::TVBVec_t> fReaderValues = {};
   std::vector<int> fLastCheckedEntry = {-1};
   std::vector<int> fLastResult = {true}; // std::vector<bool> cannot be used in a MT context safely
};

class TDataFrameImpl : public std::enable_shared_from_this<TDataFrameImpl> {
public:
   TDataFrameImpl(const std::string &treeName, TDirectory *dirPtr, const BranchList &defaultBranches = {})
      : fTreeName(treeName), fDirPtr(dirPtr), fDefaultBranches(defaultBranches), fNSlots(ROOT::Internal::GetNSlots()) { }

   TDataFrameImpl(TTree &tree, const BranchList &defaultBranches = {}) : fTree(&tree), fDefaultBranches(defaultBranches), fNSlots(ROOT::Internal::GetNSlots())
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

            BuildAllReaderValues(r, slot); // TODO: if TTreeProcessor reuses the same TTreeReader object there is no
                                           // need to re-build the TTreeReaderValues

            // recursive call to check filters and conditionally execute actions
            while (r.Next())
               for (auto &actionPtr : fBookedActions) actionPtr->Run(slot, r.GetCurrentEntry());
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
            for (auto &actionPtr : fBookedActions) actionPtr->Run(0, r.GetCurrentEntry());
#ifdef R__USE_IMT
      }
#endif // R__USE_IMT

      // forget actions and "detach" the action result pointers marking them ready and forget them too
      fBookedActions.clear();
      for (auto aptr : fActionResultsPtrs) {
         aptr->SetReady();
      }
      fActionResultsPtrs.clear();
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

   const BranchList &GetDefaultBranches() const { return fDefaultBranches; }

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

   void Book(Internal::ActionBasePtr_t actionPtr) { fBookedActions.emplace_back(std::move(actionPtr)); }

   void Book(Details::FilterBasePtr_t filterPtr) { fBookedFilters.emplace_back(std::move(filterPtr)); }

   void Book(TmpBranchBasePtr_t branchPtr) { fBookedBranches[branchPtr->GetName()] = std::move(branchPtr); }

   // dummy call, end of recursive chain of calls
   bool CheckFilters(int, unsigned int) { return true; }

   // register a TActionResultPtr
   void RegisterActionResult(TActionResultPtrBase *ptr) { fActionResultsPtrs.emplace_back(ptr); }

   unsigned int GetNSlots() {return fNSlots;}

   Internal::ActionBaseVec_t fBookedActions;
   Details::FilterBaseVec_t fBookedFilters;
   std::map<std::string, TmpBranchBasePtr_t> fBookedBranches;
   std::vector<TActionResultPtrBase *> fActionResultsPtrs;
   std::string fTreeName;
   TDirectory *fDirPtr = nullptr;
   TTree *fTree = nullptr;
   const BranchList fDefaultBranches;
   // always empty: each object in the chain copies this list from the previous
   // and they must copy an empty list from the base TDataFrameImpl
   const BranchList fTmpBranches;
   std::weak_ptr<TDataFrameImpl> fFirstData;
   unsigned int fNSlots;
};

} // end NS Details

} // end NS ROOT

// Functions and method implementations
namespace ROOT {
template <typename T>
TDataFrameInterface<T>::TDataFrameInterface(const std::string &treeName, TDirectory *dirPtr,
                                            const BranchList &defaultBranches)
   : fProxiedPtr(std::make_shared<Details::TDataFrameImpl>(treeName, dirPtr, defaultBranches))
{
   fProxiedPtr->fFirstData = fProxiedPtr->shared_from_this();
}

template <typename T>
TDataFrameInterface<T>::TDataFrameInterface(TTree &tree, const BranchList &defaultBranches)
   : fProxiedPtr(std::make_shared<Details::TDataFrameImpl>(tree, defaultBranches))
{
   fProxiedPtr->fFirstData = fProxiedPtr->shared_from_this();
}

void TActionResultPtrBase::TriggerRun()
{
   fFirstData.lock()->Run();
}

TActionResultPtrBase::TActionResultPtrBase(std::weak_ptr<Details::TDataFrameImpl> firstData) : fFirstData(firstData)
{
   firstData.lock()->RegisterActionResult(this);
}

namespace Details {
// N.B. these methods could be unified and put in TDataFrameInterface, if I
// could find a way to make it compile
template <typename F, typename PrevData>
template <typename T>
void TDataFrameFilter<F, PrevData>::Book(std::shared_ptr<T> ptr)
{
   fFirstData.lock()->Book(ptr);
}

template <typename F, typename PrevData>
template <typename T>
void TDataFrameBranch<F, PrevData>::Book(std::shared_ptr<T> ptr)
{
   fFirstData.lock()->Book(ptr);
}

} // end NS Details

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
