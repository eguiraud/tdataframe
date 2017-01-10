#ifndef TDATAFRAME
#define TDATAFRAME

#include "TBranchElement.h"
#include "TH1F.h" // For Histo actions
#include "TTreeReader.h"
#include "TTreeReaderValue.h"

#include <algorithm> // std::find
#include <array>
#include <map>
#include <memory>
#include <list>
#include <string>
#include <type_traits> // std::decay
#include <typeinfo>
#include <utility> // std::move
#include <vector>

/******* meta-utils **********/
template<typename...Types>
struct TypeList {
   static constexpr std::size_t size = sizeof...(Types);
};

// extract parameter types from a callable object
template<typename T>
struct FunctionTraits : public FunctionTraits<decltype(&T::operator())> {};

// lambdas and std::function
template<typename R, typename T, typename... Args>
struct FunctionTraits<R(T::*)(Args...) const> {
   using ArgTypes = TypeList<typename std::decay<Args>::type...>;
   using RetType = R;
};

// mutable lambdas and functor classes
template<typename R, typename T, typename... Args>
struct FunctionTraits<R(T::*)(Args...)> {
   using ArgTypes = TypeList<typename std::decay<Args>::type...>;
   using RetType = R;
};

// free functions
template<typename R, typename... Args>
struct FunctionTraits<R(*)(Args...)> {
   using ArgTypes = TypeList<typename std::decay<Args>::type...>;
   using RetType = R;
};

// compile-time integer sequence generator
// e.g. calling gens<3>::type() instantiates a seq<0,1,2>
template<int ...>
struct seq {};

template<int N, int ...S>
struct gens : gens<N-1, N-1, S...> {};

template<int ...S>
struct gens<0, S...>{
   using type = seq<S...>;
};


// IsContainer check for type T
template<typename T>
struct IsContainer{
   using test_type = typename std::remove_const<T>::type;

   template<typename A>
   static constexpr bool test (
      A * pt,
      A const * cpt = nullptr,
      decltype(pt->begin()) * = nullptr,
      decltype(pt->end()) * = nullptr,
      decltype(cpt->begin()) * = nullptr,
      decltype(cpt->end()) * = nullptr,
      typename A::iterator * pi = nullptr,
      typename A::const_iterator * pci = nullptr,
      typename A::value_type * pv = nullptr)
   {
      using iterator =  typename A::iterator;
      using const_iterator = typename A::const_iterator;
      using value_type = typename A::value_type;
      return  std::is_same<test_type, std::vector<bool>>::value ||
              (std::is_same<decltype(pt->begin()),iterator>::value &&
               std::is_same<decltype(pt->end()),iterator>::value &&
               std::is_same<decltype(cpt->begin()),const_iterator>::value &&
               std::is_same<decltype(cpt->end()),const_iterator>::value &&
               std::is_same<decltype(**pi),value_type &>::value &&
               std::is_same<decltype(**pci),value_type const &>::value);
   }

   template<typename A>
   static constexpr bool test(...) {
      return false;
   }

   static const bool value = test<test_type>(nullptr);
};
/************* meta-utils end ***************/


using BranchList = std::vector<std::string>;
using TVBPtr = std::shared_ptr<ROOT::Internal::TTreeReaderValueBase>;
using TVBVec = std::vector<TVBPtr>;


template<int... S, typename... BranchTypes>
TVBVec BuildReaderValues(TTreeReader& r, const BranchList& bl,
                         const BranchList& tmpbl, TypeList<BranchTypes...>, seq<S...>)
{
   // isTmpBranch has length bl.size(). Elements are true if the corresponding
   // branch is a "fake" branch created with AddBranch, false if they are
   // actual branches present in the TTree.
   std::array<bool, sizeof...(S)> isTmpBranch;
   for(unsigned i = 0; i < isTmpBranch.size(); ++i)
      isTmpBranch[i] = std::find(tmpbl.begin(), tmpbl.end(), bl.at(i)) != tmpbl.end();

   // Build vector of pointers to TTreeReaderValueBase.
   // tvb[i] points to a TTreeReaderValue specialized for the i-th BranchType,
   // corresponding to the i-th branch in bl
   // For temporary branches (declared with AddBranch) a nullptr is created instead
   // S is expected to be a sequence of sizeof...(BranchTypes) integers
   TVBVec tvb{
      isTmpBranch[S] ?
      nullptr :
      std::make_shared<TTreeReaderValue<BranchTypes>>(r, bl.at(S).c_str())
      ... }; // "..." expands BranchTypes and S simultaneously

   return tvb;
}


template<typename Filter>
void CheckFilter(Filter f) {
   using FilterRetType = typename FunctionTraits<Filter>::RetType;
   static_assert(std::is_same<FilterRetType, bool>::value,
                 "filter functions must return a bool");
}


template<typename F>
const BranchList& PickBranchList(F f, const BranchList& bl, const BranchList& defBl) {
   // return local BranchList or default BranchList according to which one
   // should be used
   auto nArgs = FunctionTraits<F>::ArgTypes::size;
   bool useDefBl = false;
   if(nArgs != bl.size()) {
      if(bl.size() == 0 && nArgs == defBl.size()) {
         useDefBl = true;
      } else {
         auto msg = "mismatch between number of filter arguments ("
                     + std::to_string(nArgs) + ") and number of branches ("
                     + std::to_string(bl.size()?:defBl.size()) + ")";
         throw std::runtime_error(msg);
      }
   }

   return useDefBl ? defBl : bl;
}


class TDataFrameActionBase {
public:
   virtual ~TDataFrameActionBase() { }
   virtual void Run(int entry) = 0;
   virtual void BuildReaderValues(TTreeReader& r) = 0;
};
using ActionBasePtr = std::unique_ptr<TDataFrameActionBase>;
using ActionBaseVec = std::vector<ActionBasePtr>;


class TDataFrameFilterBase {
public:
   virtual ~TDataFrameFilterBase() { }
   virtual void BuildReaderValues(TTreeReader& r) = 0;
};
using FilterBasePtr = std::unique_ptr<TDataFrameFilterBase>;
using FilterBaseVec = std::vector<FilterBasePtr>;


class TDataFrameBranchBase {
public:
   virtual ~TDataFrameBranchBase() { }
   virtual void BuildReaderValues(TTreeReader& r) = 0;
   virtual std::string GetName() const = 0;
   virtual void* GetValue(int entry) = 0;
   virtual const std::type_info& GetTypeId() const = 0;
};
using TmpBranchBasePtr = std::unique_ptr<TDataFrameBranchBase>;
using TmpBranchBaseVec = std::vector<TmpBranchBasePtr>;


// Forward declaration
class TDataFrame;

// Smart pointer for the return type of actions
class TActionResultPtrBase {
   friend TDataFrame;
   TDataFrame& fFirstData;
   std::shared_ptr<bool> fReadyPtr = std::make_shared<bool>(false);
   void SetReady() {
      *fReadyPtr = true;
   }
protected:
   bool IsReady() {
      return *fReadyPtr;
   }
   void TriggerRun();
public:
   TActionResultPtrBase(TDataFrame& firstData);
};

using TActionResultPtrBaseVec = std::vector<TActionResultPtrBase*>;

template<typename T>
class TActionResultPtr : public TActionResultPtrBase{
   std::shared_ptr<T> fObjPtr;
public:
   TActionResultPtr(std::shared_ptr<T> objPtr, TDataFrame& firstData)
      : TActionResultPtrBase(firstData), fObjPtr(objPtr) {}
   T *get() {
      if (!IsReady()) TriggerRun();
      return fObjPtr.get();
   }
   T& operator*() { return *get(); }
   T *operator->() { return get(); }
   T *getUnchecked() { return fObjPtr.get(); }
};


// Forward declarations
template<int S, typename T>
T& GetBranchValue(TVBPtr& readerValues, int entry, const std::string& branch, TDataFrame& df);


template<typename F, typename PrevDataFrame>
class TDataFrameAction : public TDataFrameActionBase {
   using BranchTypes = typename FunctionTraits<F>::ArgTypes;
   using TypeInd = typename gens<BranchTypes::size>::type;

public:
   TDataFrameAction(F f, const BranchList& bl, PrevDataFrame& pd)
      : fAction(f), fBranches(bl), fTmpBranches(pd.fTmpBranches),
        fPrevData(pd), fFirstData(pd.fFirstData) { }

   TDataFrameAction(const TDataFrameAction&) = delete;

   void Run(int entry) {
      // check if entry passes all filters
      if(CheckFilters(entry))
         ExecuteAction(entry);
   }

   bool CheckFilters(int entry) {
      // start the recursive chain of CheckFilters calls
      return fPrevData.CheckFilters(entry);
   }

   void ExecuteAction(int entry) {
      ExecuteActionHelper(entry, TypeInd(), BranchTypes());
   }

   void BuildReaderValues(TTreeReader& r) {
      fReaderValues = ::BuildReaderValues(r, fBranches, fTmpBranches, BranchTypes(), TypeInd());
   }

private:
   template<int... S, typename... BranchTypes>
   void ExecuteActionHelper(int entry, seq<S...>, TypeList<BranchTypes...>) {
      // Take each pointer in tvb, cast it to a pointer to the
      // correct specialization of TTreeReaderValue, and get its content.
      // S expands to a sequence of integers 0 to sizeof...(types)-1
      // S and types are expanded simultaneously by "..."
      fAction(::GetBranchValue<S, BranchTypes>(fReaderValues[S], entry, fBranches[S], fFirstData)...);
   }

   F fAction;
   const BranchList fBranches;
   const BranchList fTmpBranches;
   PrevDataFrame& fPrevData;
   TDataFrame& fFirstData;
   TVBVec fReaderValues;
};


// forward declarations for TDataFrameInterface
template<typename F, typename PrevData>
class TDataFrameFilter;
template<typename F, typename PrevData>
class TDataFrameBranch;

namespace Operations {
   class FillOperation{
      TH1F* fHist;
   public:
      FillOperation(TH1F* h):fHist(h){};
      template<typename T, typename std::enable_if<!IsContainer<T>::value, int>::type = 0>
      void Exec(T v){fHist->Fill(v); }
      template<typename T, typename std::enable_if<IsContainer<T>::value, int>::type = 0>
      void Exec(const T&  vs){for (auto&& v : vs) {fHist->Fill(v);} }
   };

   class MinOperation{
      double* fMinVal;
   public:
      MinOperation(double* minVPtr):fMinVal(minVPtr){};
      template<typename T, typename std::enable_if<!IsContainer<T>::value, int>::type = 0>
      void Exec(T v){ *fMinVal = std::min((double)v, *fMinVal);}
      template<typename T, typename std::enable_if<IsContainer<T>::value, int>::type = 0>
      void Exec(const T&  vs){ for(auto&& v : vs) *fMinVal = std::min((double)v, *fMinVal);}
   };

   class MaxOperation{
      double* fMaxVal;
   public:
      MaxOperation(double* maxVPtr):fMaxVal(maxVPtr){};
      template<typename T, typename std::enable_if<!IsContainer<T>::value, int>::type = 0>
      void Exec(T v){ *fMaxVal = std::max((double)v,*fMaxVal);}
      template<typename T, typename std::enable_if<IsContainer<T>::value, int>::type = 0>
      void Exec(const T&  vs){ for(auto&& v : vs) *fMaxVal = std::max((double)v,*fMaxVal);}
   };

   class MeanOperation{
      unsigned long int n = 0;
      double* fMeanVal;
   public:
      MeanOperation(double* meanVPtr):fMeanVal(meanVPtr){};
      template<typename T, typename std::enable_if<!IsContainer<T>::value, int>::type = 0>
      void Cumulate(T v){ *fMeanVal += v; ++n;}
      template<typename T, typename std::enable_if<IsContainer<T>::value, int>::type = 0>
      void Cumulate(const T& vs){ for(auto&& v : vs) { *fMeanVal += v; ++n; } }
      ~MeanOperation(){if (n!=0) *fMeanVal/=n;}
   };
}


enum class EActionType : short {
   kHisto1D,
   kMin,
   kMax,
   kMean
};


// this class provides a common public interface to TDataFrame and TDataFrameFilter
// it contains the Filter call and all action calls
template<typename Derived>
class TDataFrameInterface {
public:
   TDataFrameInterface() : fDerivedPtr(static_cast<Derived*>(this)) { }
   virtual ~TDataFrameInterface() { }

   template<typename F>
   auto Filter(F f, const BranchList& bl = {}) -> TDataFrameFilter<F, Derived>& {
      ::CheckFilter(f);
      const BranchList& defBl = fDerivedPtr->GetDataFrame().GetDefaultBranches();
      const BranchList& actualBl = ::PickBranchList(f, bl, defBl);
      using DFF = TDataFrameFilter<F, Derived>;
      auto FilterPtr = std::unique_ptr<DFF>(new DFF(f, actualBl, *fDerivedPtr));
      auto& FilterRef = *FilterPtr;
      Book(std::move(FilterPtr));
      return FilterRef;
   }

   template<typename F>
   auto AddBranch(const std::string& name, F expression, const BranchList& bl = {})
   -> TDataFrameBranch<F, Derived>& {
      const BranchList& defBl = fDerivedPtr->GetDataFrame().GetDefaultBranches();
      const BranchList& actualBl = ::PickBranchList(expression, bl, defBl);
      using DFB = TDataFrameBranch<F, Derived>;
      auto BranchPtr = std::unique_ptr<DFB>(new DFB(name, expression, actualBl, *fDerivedPtr));
      auto& BranchRef = *BranchPtr;
      Book(std::move(BranchPtr));
      return BranchRef;
   }

   template<typename F>
   void Foreach(F f, const BranchList& bl = {}) {
      const BranchList& defBl = fDerivedPtr->GetDataFrame().GetDefaultBranches();
      const BranchList& actualBl = ::PickBranchList(f, bl, defBl);
      using DFA = TDataFrameAction<F, Derived>;
      Book(std::unique_ptr<DFA>(new DFA(f, actualBl, *fDerivedPtr)));
   }

   TActionResultPtr<unsigned> Count() {
      TActionResultPtr<unsigned> c (std::make_shared<unsigned>(0), fDerivedPtr->GetDataFrame());
      auto countAction = [&c]() -> void { (*c.getUnchecked())++; };
      BranchList bl = {};
      using DFA = TDataFrameAction<decltype(countAction), Derived>;
      Book(std::unique_ptr<DFA>(new DFA(countAction, bl, *fDerivedPtr)));
      return c;
   }


   template<typename T, typename COLL = std::list<T>>
   TActionResultPtr<COLL> Get(const std::string& branchName = "") {
      auto theBranchName (branchName);
      GetDefaultBranchName(theBranchName, "get the values of the branch");
      TActionResultPtr<COLL> values (std::make_shared<COLL>(), fDerivedPtr->GetDataFrame());
      auto valuesPtr = values.getUnchecked();
      auto getAction = [valuesPtr](const T& v) { valuesPtr->emplace_back(v); };
      BranchList bl = {theBranchName};
      using DFA = TDataFrameAction<decltype(getAction), Derived>;
      Book(std::unique_ptr<DFA>(new DFA(getAction, bl, *fDerivedPtr)));
      return values;
   }

   template<typename T = double>
   TActionResultPtr<TH1F> Histo(const std::string& branchName, const TH1F& model) {
      auto theBranchName (branchName);
      GetDefaultBranchName(theBranchName, "fill the histogram");
      auto h = std::make_shared<TH1F>(model);
      return CreateAction<T, EActionType::kHisto1D>(theBranchName,h);
   }

   template<typename T = double>
   TActionResultPtr<TH1F> Histo(const std::string& branchName = "", int nBins = 128, double minVal = 0., double maxVal = 0.) {
      auto theBranchName (branchName);
      GetDefaultBranchName(theBranchName, "fill the histogram");
      auto h = std::make_shared<TH1F>("","",nBins, minVal, maxVal);
      return CreateAction<T, EActionType::kHisto1D>(theBranchName,h);
   }

   template<typename T = double>
   TActionResultPtr<T> Min(const std::string& branchName = "") {
      auto theBranchName (branchName);
      GetDefaultBranchName(theBranchName, "calculate the minimum");
      auto minV = std::make_shared<T>(std::numeric_limits<T>::max());
      return CreateAction<T, EActionType::kMin>(theBranchName,minV);
   }

   template<typename T = double>
   TActionResultPtr<T> Max(const std::string& branchName = "") {
      auto theBranchName (branchName);
      GetDefaultBranchName(theBranchName, "calculate the maximum");
      auto maxV = std::make_shared<T>(std::numeric_limits<T>::min());
      return CreateAction<T, EActionType::kMax>(theBranchName,maxV);
   }

   template<typename T = double>
   TActionResultPtr<double> Mean(const std::string& branchName = "") {
      auto theBranchName (branchName);
      GetDefaultBranchName(theBranchName, "calculate the mean");
      auto meanV = std::make_shared<T>(0);
      return CreateAction<T, EActionType::kMean>(theBranchName,meanV);
   }

private:

   void GetDefaultBranchName(std::string& theBranchName, const std::string& actionNameForErr) {
      if (theBranchName.empty()) {
         // Try the default branch if possible
         const BranchList& defBl = fDerivedPtr->GetDataFrame().GetDefaultBranches();
         if (defBl.size() == 1) {
            theBranchName = defBl[0];
         } else {
            std::string msg ("No branch in input to ");
            msg += actionNameForErr;
            msg +=  " and more than one default branch.";
            throw std::runtime_error(msg);
         }
      }
   }

   template<typename BranchType, typename ActionResultType, enum EActionType, typename ThisType>
   struct SimpleAction{ };

   template<typename BranchType, typename ThisType>
   struct SimpleAction<BranchType, TH1F, EActionType::kHisto1D, ThisType> {
      static TActionResultPtr<TH1F> BuildAndBook(ThisType thisFrame, const std::string& theBranchName, std::shared_ptr<TH1F> h) {
         Operations::FillOperation fillOp(h.get());
         auto fillLambda = [fillOp](const BranchType& v) mutable { fillOp.Exec(v); };
         BranchList bl = {theBranchName};
         using DFA = TDataFrameAction<decltype(fillLambda), Derived>;
         thisFrame->Book(std::unique_ptr<DFA>(new DFA(fillLambda, bl, *thisFrame->fDerivedPtr)));
         return TActionResultPtr<TH1F>(h, thisFrame->GetDataFrame());
      }
   };

   template<typename BranchType, typename ThisType, typename ActionResultType>
   struct SimpleAction<BranchType, ActionResultType, EActionType::kMin, ThisType> {
      static TActionResultPtr<ActionResultType> BuildAndBook(ThisType thisFrame, const std::string& theBranchName, std::shared_ptr<ActionResultType> minV) {
         Operations::MinOperation minOp(minV.get());
         auto minOpLambda = [minOp](const BranchType& v) mutable { minOp.Exec(v); };
         BranchList bl = {theBranchName};
         using DFA = TDataFrameAction<decltype(minOpLambda), Derived>;
         thisFrame->Book(std::unique_ptr<DFA>(new DFA(minOpLambda, bl, *thisFrame->fDerivedPtr)));
         return TActionResultPtr<ActionResultType>(minV, thisFrame->GetDataFrame());
      }
   };

   template<typename BranchType, typename ThisType, typename ActionResultType>
   struct SimpleAction<BranchType, ActionResultType, EActionType::kMax, ThisType> {
      static TActionResultPtr<ActionResultType> BuildAndBook(ThisType thisFrame, const std::string& theBranchName, std::shared_ptr<ActionResultType> maxV) {
         Operations::MaxOperation maxOp(maxV.get());
         auto maxOpLambda = [maxOp](const BranchType& v) mutable { maxOp.Exec(v); };
         BranchList bl = {theBranchName};
         using DFA = TDataFrameAction<decltype(maxOpLambda), Derived>;
         thisFrame->Book(std::unique_ptr<DFA>(new DFA(maxOpLambda, bl, *thisFrame->fDerivedPtr)));
         return TActionResultPtr<ActionResultType>(maxV, thisFrame->GetDataFrame());
      }
   };

   template<typename BranchType, typename ThisType, typename ActionResultType>
   struct SimpleAction<BranchType, ActionResultType, EActionType::kMean, ThisType> {
      static TActionResultPtr<ActionResultType> BuildAndBook(ThisType thisFrame, const std::string& theBranchName, std::shared_ptr<ActionResultType> meanV) {
         Operations::MeanOperation meanOp(meanV.get());
         auto meanOpLambda = [meanOp](const BranchType& v) mutable { meanOp.Cumulate(v);};
         BranchList bl = {theBranchName};
         using DFA = TDataFrameAction<decltype(meanOpLambda), Derived>;
         thisFrame->Book(std::unique_ptr<DFA>(new DFA(meanOpLambda, bl, *thisFrame->fDerivedPtr)));
         return TActionResultPtr<ActionResultType>(meanV, thisFrame->GetDataFrame());
      }
   };

   template<typename BranchType, EActionType ActionType, typename ActionResultType>
   TActionResultPtr<ActionResultType> CreateAction(const std::string& theBranchName, std::shared_ptr<ActionResultType> r) {
      // More types can be added at will at the cost of some compilation time and size of binaries.
      using ART = ActionResultType;
      using TT = decltype(this);
      const auto AT = ActionType;
      auto& df = fDerivedPtr->GetDataFrame();
      auto tree = (TTree*) df.GetDirectory()->Get(df.GetTreeName().c_str());
      auto branch = tree->GetBranch(theBranchName.c_str());
      if(!branch) {
         const auto& type_id = df.GetBookedBranch(theBranchName).GetTypeId();
         if (type_id == typeid(char)) { return SimpleAction<char, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
         else if (type_id == typeid(int)) { return SimpleAction<int, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
         else if (type_id == typeid(double)) { return SimpleAction<double, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
         else if (type_id == typeid(bool)) { return SimpleAction<bool, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
         else if (type_id == typeid(std::vector<double>)) { return SimpleAction<std::vector<double>, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
         else if (type_id == typeid(std::vector<float>)) { return SimpleAction<std::vector<float>, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
      }
      auto branchEl = dynamic_cast<TBranchElement*>(branch);
      if (!branchEl) { // This is a fundamental type
         auto title = branch->GetTitle();
         auto typeCode = title[strlen(title)-1];
         if (typeCode == 'B') { return SimpleAction<char, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
         // else if (typeCode == 'b') { return SimpleAction<unsigned char, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
         // else if (typeCode == 'S') { return SimpleAction<short, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
         // else if (typeCode == 's') { return SimpleAction<unsigned short, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
         else if (typeCode == 'I') { return SimpleAction<int, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
         // else if (typeCode == 'i') { return SimpleAction<unsigned int, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
         // else if (typeCode == 'F') { return SimpleAction<float, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
         else if (typeCode == 'D') { return SimpleAction<double, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
         // else if (typeCode == 'L') { return SimpleAction<long int, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
         // else if (typeCode == 'l') { return SimpleAction<unsigned long int, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
         else if (typeCode == 'O') { return SimpleAction<bool, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
      } else {
         std::string typeName = branchEl->GetTypeName();
         if (typeName == "vector<double>") { return SimpleAction<std::vector<double>, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
         else if (typeName == "vector<float>") { return SimpleAction<std::vector<int>, ART, AT, TT>::BuildAndBook(this, theBranchName, r); }
      }
      return SimpleAction<BranchType, ART, AT, TT>::BuildAndBook(this, theBranchName, r);
   }

   template<typename T>
   void Book(std::unique_ptr<T> ptr) {
      fDerivedPtr->Book(std::move(ptr));
   }

   virtual TDataFrame& GetDataFrame() const = 0;

   Derived* fDerivedPtr;
};


template<typename F, typename PrevData>
class TDataFrameBranch : public TDataFrameInterface<TDataFrameBranch<F, PrevData>>,
                         public TDataFrameBranchBase {
   template<typename A, typename B> friend class TDataFrameAction;
   template<typename A, typename B> friend class TDataFrameFilter;
   template<typename A, typename B> friend class TDataFrameBranch;
   using BranchTypes = typename FunctionTraits<F>::ArgTypes;
   using TypeInd = typename gens<BranchTypes::size>::type;
   using RetType = typename FunctionTraits<F>::RetType;
   using TDFInterface = TDataFrameInterface<TDataFrameBranch<F, PrevData>>;

public:
   TDataFrameBranch(const std::string& name, F expression, const BranchList& bl, PrevData& pd)
      : fName(name), fExpression(expression), fBranches(bl), fTmpBranches(pd.fTmpBranches),
        fFirstData(pd.fFirstData), fPrevData(pd), fLastCheckedEntry(-1) {
      fTmpBranches.emplace_back(name);
   }

   TDataFrameBranch(const TDataFrameBranch&) = delete;

   TDataFrame& GetDataFrame() const {
      return fFirstData;
   }

   void BuildReaderValues(TTreeReader& r) {
      fReaderValues = ::BuildReaderValues(r, fBranches, fTmpBranches, BranchTypes(), TypeInd());
   }

   void* GetValue(int entry) {
      if(entry != fLastCheckedEntry) {
         // evaluate this filter, cache the result
         auto newValuePtr = GetValueHelper(BranchTypes(), TypeInd(), entry);
         fLastResultPtr.swap(newValuePtr);
         fLastCheckedEntry = entry;
      }
      return static_cast<void*>(fLastResultPtr.get());
   }

   const std::type_info& GetTypeId() const {
      return typeid(RetType);
   }

   template<typename T>
   void Book(std::unique_ptr<T> ptr);

   bool CheckFilters(int entry) {
      // dummy call: it just forwards to the previous object in the chain
      return fPrevData.CheckFilters(entry);
   }

   std::string GetName() const { return fName; }

private:
   template<int...S, typename...BranchTypes>
   std::unique_ptr<RetType> GetValueHelper(TypeList<BranchTypes...>, seq<S...>, int entry) {
      auto valuePtr = std::unique_ptr<RetType>(
         new RetType(
            fExpression(
               ::GetBranchValue<S, BranchTypes>(fReaderValues[S], entry, fBranches[S], fFirstData)...
            )
         )
      );
      return valuePtr;
   }

   const std::string fName;
   F fExpression;
   const BranchList fBranches;
   BranchList fTmpBranches;
   TVBVec fReaderValues;
   std::unique_ptr<RetType> fLastResultPtr;
   TDataFrame& fFirstData;
   PrevData& fPrevData;
   int fLastCheckedEntry;
};


template<typename FilterF, typename PrevDataFrame>
class TDataFrameFilter
   : public TDataFrameFilterBase,
     public TDataFrameInterface<TDataFrameFilter<FilterF, PrevDataFrame>>
{
   template<typename A, typename B> friend class TDataFrameAction;
   template<typename A, typename B> friend class TDataFrameFilter;
   template<typename A, typename B> friend class TDataFrameBranch;
   template<typename A> friend class TDataFrameInterface;
   using BranchTypes = typename FunctionTraits<FilterF>::ArgTypes;
   using TypeInd = typename gens<BranchTypes::size>::type;
   using TDFInterface = TDataFrameInterface<TDataFrameFilter<FilterF, PrevDataFrame>>;

public:
   TDataFrameFilter(FilterF f, const BranchList& bl, PrevDataFrame& pd)
      : fFilter(f), fBranches(bl), fTmpBranches(pd.fTmpBranches), fPrevData(pd),
        fFirstData(pd.fFirstData), fLastCheckedEntry(-1), fLastResult(true) { }

   TDataFrame& GetDataFrame() const {
      return fFirstData;
   }

   TDataFrameFilter(const TDataFrameFilter&) = delete;

private:
   bool CheckFilters(int entry) {
      if(entry != fLastCheckedEntry) {
         if(!fPrevData.CheckFilters(entry)) {
            // a filter upstream returned false, cache the result
            fLastResult = false;
         } else {
            // evaluate this filter, cache the result
            fLastResult = CheckFilterHelper(BranchTypes(), TypeInd(), entry);
         }
         fLastCheckedEntry = entry;
      }
      return fLastResult;
   }

   template<int... S, typename... BranchTypes>
   bool CheckFilterHelper(TypeList<BranchTypes...>, seq<S...>, int entry) {
      // Take each pointer in tvb, cast it to a pointer to the
      // correct specialization of TTreeReaderValue, and get its content.
      // S expands to a sequence of integers 0 to sizeof...(types)-1
      // S and types are expanded simultaneously by "..."
      return fFilter(::GetBranchValue<S, BranchTypes>(fReaderValues[S], entry, fBranches[S], fFirstData) ...);
   }

   void BuildReaderValues(TTreeReader& r) {
      fReaderValues = ::BuildReaderValues(r, fBranches, fTmpBranches, BranchTypes(), TypeInd());
   }

   template<typename T>
   void Book(std::unique_ptr<T> ptr);

   FilterF fFilter;
   const BranchList fBranches;
   const BranchList fTmpBranches;
   PrevDataFrame& fPrevData;
   TDataFrame& fFirstData;
   TVBVec fReaderValues;
   int fLastCheckedEntry;
   bool fLastResult;
};


class TDataFrame : public TDataFrameInterface<TDataFrame> {
   template<typename A, typename B> friend class TDataFrameAction;
   template<typename A, typename B> friend class TDataFrameFilter;
   template<typename A, typename B> friend class TDataFrameBranch;
   friend TActionResultPtrBase;
   friend class TDataFrameInterface<TDataFrame>;

public:
   TDataFrame(const std::string& treeName, TDirectory* dirPtr, const BranchList& defaultBranches = {})
      : fTreeName(treeName), fDirPtr(dirPtr),
        fDefaultBranches(defaultBranches), fFirstData(*this) { }

   TDataFrame(TTree& tree, const BranchList& defaultBranches = {})
      : fTree(&tree),
        fDefaultBranches(defaultBranches), fFirstData(*this) { }


   TDataFrame(const TDataFrame&) = delete;

   void Run() {
      TTreeReader r;
      if (fTree) r.SetTree(fTree);
      else r.SetTree(fTreeName.c_str(), fDirPtr);

      // build reader values for all actions, filters and branches
      for(auto& ptr : fBookedActions)
         ptr->BuildReaderValues(r);
      for(auto& ptr : fBookedFilters)
         ptr->BuildReaderValues(r);
      for(auto& bookedBranch : fBookedBranches)
         bookedBranch.second->BuildReaderValues(r);

      // recursive call to check filters and conditionally executing actions
      while(r.Next())
         for(auto& actionPtr : fBookedActions)
            actionPtr->Run(r.GetCurrentEntry());

      // forget everything
      fBookedActions.clear();
      fBookedFilters.clear();
      fBookedBranches.clear();
      for (auto aptr : fActionResultsPtrs) {
         aptr->SetReady();
      }
      fActionResultsPtrs.clear();
   }

   TDataFrame& GetDataFrame() const {
      return fFirstData;
   }

   const BranchList& GetDefaultBranches() const {
      return fDefaultBranches;
   }

   const TDataFrameBranchBase& GetBookedBranch(const std::string& name) const {
      return *fBookedBranches.find(name)->second.get();
   }

   void* GetTmpBranchValue(const std::string& branch, int entry) {
      return fBookedBranches.at(branch)->GetValue(entry);
   }

   TDirectory* GetDirectory() const {
      return fDirPtr;
   }

   std::string GetTreeName() const {
      return fTreeName;
   }

private:
   void Book(ActionBasePtr actionPtr) {
      fBookedActions.emplace_back(std::move(actionPtr));
   }

   void Book(FilterBasePtr filterPtr) {
      fBookedFilters.emplace_back(std::move(filterPtr));
   }

   void Book(TmpBranchBasePtr branchPtr) {
      fBookedBranches[branchPtr->GetName()] = std::move(branchPtr);
   }

   // dummy call, end of recursive chain of calls
   bool CheckFilters(int) {
      return true;
   }

   // register a TActionResultPtr
   void RegisterActionResult(TActionResultPtrBase* ptr) {
      fActionResultsPtrs.emplace_back(ptr);
   }

   ActionBaseVec fBookedActions;
   FilterBaseVec fBookedFilters;
   std::map<std::string, TmpBranchBasePtr> fBookedBranches;
   TActionResultPtrBaseVec fActionResultsPtrs;
   std::string fTreeName;
   TDirectory* fDirPtr = nullptr;
   TTree* fTree = nullptr;
   const BranchList fDefaultBranches;
   // always empty: each object in the chain copies this list from the previous
   // and they must copy an empty list from the base TDataFrame
   const BranchList fTmpBranches;
   TDataFrame& fFirstData;
};


//********* FUNCTION AND METHOD DEFINITIONS *********//
template<int S, typename T>
T& GetBranchValue(TVBPtr& readerValue, int entry, const std::string& branch, TDataFrame& df)
{
   if(readerValue == nullptr) {
      // temporary branch
      void* tmpBranchVal = df.GetTmpBranchValue(branch, entry);
      return *static_cast<T*>(tmpBranchVal);
   } else {
      // real branch
      return **std::static_pointer_cast<TTreeReaderValue<T>>(readerValue);
   }
}


void TActionResultPtrBase::TriggerRun()
{
   fFirstData.Run();
}


TActionResultPtrBase::TActionResultPtrBase(TDataFrame& firstData) : fFirstData(firstData)
{
   firstData.RegisterActionResult(this);
}


// N.B. these methods could be unified and put in TDataFrameInterface, if I
// could find a way to make it compile
template<typename F, typename PrevData>
template<typename T>
void TDataFrameFilter<F, PrevData>::Book(std::unique_ptr<T> ptr) {
   fFirstData.Book(std::move(ptr));
}

template<typename F, typename PrevData>
template<typename T>
void TDataFrameBranch<F, PrevData>::Book(std::unique_ptr<T> ptr) {
   fFirstData.Book(std::move(ptr));
}

// Print a TDataFrame at the Prompt
#include <sstream>

 namespace cling {
   std::string printValue(TDataFrame *df) {
      std::ostringstream ret;
      ret << "A data frame based on the \"" << df->GetTreeName() << "\" tree.";
      auto bl = df->GetDefaultBranches();
      if (bl.size() > 1) {
         ret << " Selected default branches are:" << std::endl;
         for (auto& b : bl) {
            ret << " - " << b << std::endl;
         }
      }
      if (bl.size() == 1) {
         ret << " The selected default branch is \"" << bl.front() << "\"";
      }
      return ret.str();
   }
}

#endif //TDATAFRAME
