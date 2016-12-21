//  TODO
// - static checks n_args == branchlist.size (template classes over parameter N of std::array?)
// - implement other actions (the ones that return a TDataFrameFuture)
// - implement TDataFrameFuture, hide calls to TDataFrame::Run
#ifndef TDATAFRAME
#define TDATAFRAME

#include "TH1F.h" // For Histo actions

#include <iostream>
#include <list>
#include <vector>
#include <string>
#include <memory>
#include <tuple>
#include "TBranchElement.h"
#include "TTreeReaderValue.h"
#include "TTreeReader.h"

/******* meta-utils **********/
// Remove const and ref
template<typename T> struct removeConstAndRef{ using type = typename std::remove_const<typename std::remove_reference<T>::type>::type;};

template <typename... Args> struct removeCRFromTupleElements;

template <typename... Args> struct removeCRFromTupleElements<std::tuple<Args...>>{
    using type = typename std::tuple<typename removeConstAndRef<Args>::type...> ;
};

// extract parameter types from a callable object
template<typename T>
struct f_traits : public f_traits<decltype(&T::operator())> {};

// lambdas and std::function
template<typename R, typename T, typename... Args>
struct f_traits<R(T::*)(Args...) const> {
   using arg_types_tuple = typename removeCRFromTupleElements<std::tuple<Args...>>::type;
   using ret_t = R;
};

// mutable lambdas and functor classes
template<typename R, typename T, typename... Args>
struct f_traits<R(T::*)(Args...)> {
   using arg_types_tuple = typename removeCRFromTupleElements<std::tuple<Args...>>::type;
   using ret_t = R;
};

// free functions
template<typename R, typename... Args>
struct f_traits<R(*)(Args...)> {
   using arg_types_tuple = typename removeCRFromTupleElements<std::tuple<Args...>>::type;
   using ret_t = R;
};

// compile-time integer sequence generator
// e.g. calling gens<3>::type() instantiates a seq<0,1,2>
template<int ...>
struct seq {};

template<int N, int ...S>
struct gens : gens<N-1, N-1, S...> {};

template<int ...S>
struct gens<0, S...>{
   typedef seq<S...> type;
};
/****************************/


using BranchList = std::vector<std::string>;
using TVBVec = std::vector<std::shared_ptr<ROOT::Internal::TTreeReaderValueBase>>;


template<int... S, typename... arg_types>
TVBVec BuildReaderValues(TTreeReader& r, const BranchList& bl, std::tuple<arg_types...>, seq<S...>) {
   // Build vector of pointers to TTreeReaderValueBase.
   // tvb[i] is a TTreeReaderValue specialized for the i-th arg_type
   // S must be a sequence of sizeof...(arg_types) integers
   // arg_types and S are expanded simultaneously by "..."
   TVBVec tvb{ std::make_shared<TTreeReaderValue<arg_types>>(r, bl.at(S).c_str())... };
   return tvb;
}


template<typename Filter>
void CheckFilter(Filter f) {
   using FilterRetType = typename f_traits<Filter>::ret_t;
   static_assert(std::is_same<FilterRetType, bool>::value,
                 "filter functions must return a bool");
}


template<typename F>
const BranchList& ShouldUseDefaultBranches(F f, const BranchList& bl, const BranchList& defBl) {
   using ArgsTuple = typename f_traits<F>::arg_types_tuple;
   auto nArgs = std::tuple_size<ArgsTuple>::value;
   bool useDefBl = false;
   if(nArgs != bl.size()) {
      if(bl.size() == 0 && nArgs == defBl.size()) {
         useDefBl = true;
      } else {
         auto msg = "mismatch between number of filter arguments (" \
                     + std::to_string(nArgs) + ") and number of branches (" \
                     + std::to_string(bl.size()?:defBl.size()) + ")";
         throw std::runtime_error(msg);
      }
   }

   return useDefBl ? defBl : bl;
}


class TDataFrameActionBase {
   public:
   void Run(int entry) {
      // check if entry passes all filters
      if(CheckFilters(entry))
         ExecuteAction();
   }

   virtual bool CheckFilters(int entry) = 0;
   virtual void ExecuteAction() = 0;
   virtual void BuildReaderValues(TTreeReader& r) = 0;
};
using ActionBasePtr = std::shared_ptr<TDataFrameActionBase>;
using ActionBaseVec = std::vector<ActionBasePtr>;


class TDataFrameFilterBase {
   public:
   virtual bool CheckFilters(int entry) = 0;
   virtual void BuildReaderValues(TTreeReader& r) = 0;
   virtual void BookAction(ActionBasePtr ptr) = 0;
};
using FilterBasePtr = std::shared_ptr<TDataFrameFilterBase>;
using FilterBaseVec = std::vector<FilterBasePtr>;


// Forward declaration
class TDataFrame;

// Smart pointer for the return type of actions
class ActionResultPtrBase {
   friend TDataFrame;
   TDataFrame& fFirstData;
   bool fReady = false;
   void SetReady() {fReady = true;}
protected:
   bool IsReady() {return fReady;}
   void TriggerRun();
public:
   ActionResultPtrBase(TDataFrame& firstData);
};

using ActionResultPtrBaseVec = std::vector<ActionResultPtrBase*>;

template<typename T>
class ActionResultPtr : public ActionResultPtrBase{
   std::shared_ptr<T> fObjPtr;
public:
   ActionResultPtr(T* objPtr, TDataFrame& firstData): ActionResultPtrBase(firstData), fObjPtr(objPtr){}
   T *get() {
      if (!IsReady()) TriggerRun();
      return fObjPtr.get();
   }
   T *operator->() { return get(); }
   T *getUnchecked() { return fObjPtr.get(); }
};


template<typename F, typename PrevDataFrame>
class TDataFrameAction : public TDataFrameActionBase {
   using f_arg_types = typename f_traits<F>::arg_types_tuple;
   using f_arg_ind = typename gens<std::tuple_size<f_arg_types>::value>::type;

   public:
   TDataFrameAction(F f, const BranchList& bl, PrevDataFrame& pd)
      : fAction(f), fBranchList(bl), fPrevData(pd), fFirstData(pd.fFirstData) { }

   bool CheckFilters(int entry) {
      return fPrevData.CheckFilters(entry);
   }

   void ExecuteAction() {
      ExecuteActionHelper(f_arg_types(), f_arg_ind());
   }

   template<int... S, typename... types>
   void ExecuteActionHelper(std::tuple<types...>, seq<S...>) {
      // Take each pointer in tvb, cast it to a pointer to the
      // correct specialization of TTreeReaderValue, and get its content.
      // S expands to a sequence of integers 0 to sizeof...(types)-1
      // S and types are expanded simultaneously by "..."
      fAction(*(std::static_pointer_cast<TTreeReaderValue<types>>(fReaderValues[S]))->Get() ...);
   }

   void BuildReaderValues(TTreeReader& r) {
      fReaderValues = ::BuildReaderValues(r, fBranchList, f_arg_types(), f_arg_ind());
      fPrevData.BuildReaderValues(r);
   }

   private:
   F fAction;
   const BranchList fBranchList;
   PrevDataFrame& fPrevData;
   TDataFrame& fFirstData;
   TVBVec fReaderValues;
};

// forward declarations for TDataFrameInterface
template<typename FilterF, typename PrevDataFrame>
class TDataFrameFilter;

template<typename T>
struct IsContainer{
    typedef typename std::remove_const<T>::type test_type;

    template<typename A>
    static constexpr bool test(
        A * pt,
        A const * cpt = nullptr,
        decltype(pt->begin()) * = nullptr,
        decltype(pt->end()) * = nullptr,
        decltype(cpt->begin()) * = nullptr,
        decltype(cpt->end()) * = nullptr,
        typename A::iterator * pi = nullptr,
        typename A::const_iterator * pci = nullptr,
        typename A::value_type * pv = nullptr) {

        typedef typename A::iterator iterator;
        typedef typename A::const_iterator const_iterator;
        typedef typename A::value_type value_type;
        return  std::is_same<T, std::vector<bool>>::value ||
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


// this class provides a common public interface to TDataFrame and TDataFrameFilter
// it contains the Filter call and all action calls
template<typename Derived>
class TDataFrameInterface {
   public:
   template<typename F>
   auto Filter(F f, const BranchList& bl = {}) -> TDataFrameFilter<F, Derived> {
      ::CheckFilter(f);
      const BranchList& defBl = fDerivedPtr->GetDataFrame().GetDefaultBranches();
      const BranchList& actualBl = ::ShouldUseDefaultBranches(f, bl, defBl);
      auto DFFilterPtr = std::make_shared<TDataFrameFilter<F, Derived>>(f, actualBl, *fDerivedPtr);
      BookFilter(DFFilterPtr);
      return *DFFilterPtr;
   }

   template<typename F>
   void Foreach(F f, const BranchList& bl = {}) {
      const BranchList& defBl = fDerivedPtr->GetDataFrame().GetDefaultBranches();
      const BranchList& actualBl = ::ShouldUseDefaultBranches(f, bl, defBl);
      BookAction(std::make_shared<TDataFrameAction<F, Derived>>(f, actualBl, *fDerivedPtr));
   }

   ActionResultPtr<unsigned> Count() {
      ActionResultPtr<unsigned> c (new unsigned(0), fDerivedPtr->GetDataFrame());
      auto countAction = [&c]() -> void { (*c.getUnchecked())++; };
      BranchList bl = {};
      BookAction(std::make_shared<TDataFrameAction<decltype(countAction), Derived>>(countAction, bl, *fDerivedPtr));
      return c;
   }

   template<typename T = double>
   ActionResultPtr<TH1F> Histo(const std::string& branchName = "", int nBins = 128) {
      auto theBranchName (branchName);
      if (theBranchName.empty()) {
         // Try the default branch if possible
         const BranchList& defBl = fDerivedPtr->GetDataFrame().GetDefaultBranches();
         if (defBl.size() == 1) theBranchName = defBl[0];
         else {
            auto msg = "No branch in input to create a histogram and more than one default branch.";
         throw std::runtime_error(msg);
         }
      }
      auto& df = fDerivedPtr->GetDataFrame();
      ActionResultPtr<TH1F> h (new TH1F("","",nBins,0.,0.), df);
      // TODO: Here we need a proper switch to select at runtime the right type
      auto tree = (TTree*) df.fDirPtr->Get(df.fTreeName.c_str());
      auto branch = tree->GetBranch(theBranchName.c_str());
      auto branchEl = dynamic_cast<TBranchElement*>(branch);
      if (!branchEl) { // This is a fundamental type
         auto title = branch->GetTitle();
         auto typeCode = title[strlen(title)-1];
         if (typeCode == 'B') {BookHistoAction<char>(theBranchName, h); return h;}
         else if (typeCode == 'b') {BookHistoAction<unsigned char>(theBranchName, h); return h;}
         else if (typeCode == 'S') {BookHistoAction<short>(theBranchName, h); return h;}
         else if (typeCode == 's') {BookHistoAction<unsigned short>(theBranchName, h); return h;}
         else if (typeCode == 'I') {BookHistoAction<int>(theBranchName, h); return h;}
         else if (typeCode == 'i') {BookHistoAction<unsigned int>(theBranchName, h); return h;}
         else if (typeCode == 'F') {BookHistoAction<float>(theBranchName, h); return h;}
         else if (typeCode == 'D') {BookHistoAction<double>(theBranchName, h); return h;}
         else if (typeCode == 'L') {BookHistoAction<long int>(theBranchName, h); return h;}
         else if (typeCode == 'l') {BookHistoAction<unsigned long int>(theBranchName, h); return h;}
         else if (typeCode == 'O') {BookHistoAction<bool>(theBranchName, h); return h;}
      } else {
         std::string typeName = branchEl->GetTypeName();
         if (typeName == "vector<double>") {BookHistoAction<std::vector<double>>(theBranchName, h); return h;}
         else if (typeName == "vector<float>") {BookHistoAction<std::vector<double>>(theBranchName, h); return h;}
         else if (typeName == "vector<bool>") {BookHistoAction<std::vector<bool>>(theBranchName, h); return h;}
         else if (typeName == "vector<char>") {BookHistoAction<std::vector<char>>(theBranchName, h); return h;}
         else if (typeName == "vector<short>") {BookHistoAction<std::vector<short>>(theBranchName, h); return h;}
         else if (typeName == "vector<int>") {BookHistoAction<std::vector<int>>(theBranchName, h); return h;}
         else if (typeName == "vector<long int>") {BookHistoAction<std::vector<long int>>(theBranchName, h); return h;}
         else if (typeName == "vector<unsigned char>") {BookHistoAction<std::vector<unsigned char>>(theBranchName, h); return h;}
         else if (typeName == "vector<unsigned short>") {BookHistoAction<std::vector<unsigned short>>(theBranchName, h); return h;}
         else if (typeName == "vector<unsigned int>") {BookHistoAction<std::vector<unsigned int>>(theBranchName, h); return h;}
         else if (typeName == "vector<unsigned long int>") {BookHistoAction<std::vector<unsigned long int>>(theBranchName, h);
         return h; return h;}
      }
      BookHistoAction<T>(theBranchName, h);
      return h;
   }

   protected:
   Derived* fDerivedPtr;

   private:
   template<typename T, bool IsCont>
   class FillAction{
      TH1F* fHist;
   public:
      FillAction(TH1F* h):fHist(h){};
      void Fill(T v){fHist->Fill(v); }
   };

   template<typename T>
   class FillAction<T,true>{
      TH1F* fHist;
   public:
      FillAction(TH1F* h):fHist(h){};
      void Fill(T vs){for (auto&& v : vs) {fHist->Fill(v);} }
   };
   template<typename T, typename HistResult>
   void BookHistoAction(const std::string& theBranchName, HistResult& h){
      auto hv = h.getUnchecked();
      FillAction<T, IsContainer<T>::value> fa(hv);
      auto fillAction = [fa](T v) mutable { fa.Fill(v); };
      BranchList bl = {theBranchName};
      BookAction(std::make_shared<TDataFrameAction<decltype(fillAction), Derived>>(fillAction,
                                                                                   bl,
                                                                                   *fDerivedPtr));
   }

   virtual void BookAction(ActionBasePtr ptr) = 0;
   virtual void BookFilter(FilterBasePtr ptr) = 0;
   virtual TDataFrame& GetDataFrame() const = 0;
};


template<typename FilterF, typename PrevDataFrame>
class TDataFrameFilter
   : public TDataFrameFilterBase,
     public TDataFrameInterface<TDataFrameFilter<FilterF, PrevDataFrame>>
{
   template<typename A, typename B> friend class TDataFrameAction;
   template<typename A, typename B> friend class TDataFrameFilter;
   template<typename A> friend class TDataFrameInterface;
   using f_arg_types = typename f_traits<FilterF>::arg_types_tuple;
   using f_arg_ind = typename gens<std::tuple_size<f_arg_types>::value>::type;
   using TDFInterface = TDataFrameInterface<TDataFrameFilter<FilterF, PrevDataFrame>>;

   public:
   TDataFrameFilter(FilterF f, const BranchList& bl, PrevDataFrame& pd)
      : fFilter(f), fBranchList(bl), fPrevData(pd), fFirstData(pd.fFirstData), fLastCheckedEntry(-1),
        fLastResult(true) {
      TDFInterface::fDerivedPtr = this;
   }

   TDataFrame& GetDataFrame() const {
      return fFirstData;
   }

   private:
   bool CheckFilters(int entry) {
      if(entry == fLastCheckedEntry)
         // return cached result
         return fLastResult;
      if(!fPrevData.CheckFilters(entry))
         // a filter upstream returned false, cache the result
         fLastResult = false;
      else
         // evaluate this filter, cache the result
         fLastResult = CheckFilterHelper(f_arg_types(), f_arg_ind());
      fLastCheckedEntry = entry;
      return fLastResult;
   }

   template<int... S, typename... types>
   bool CheckFilterHelper(std::tuple<types...>, seq<S...>) {
      // Take each pointer in tvb, cast it to a pointer to the
      // correct specialization of TTreeReaderValue, and get its content.
      // S expands to a sequence of integers 0 to sizeof...(types)-1
      // S and types are expanded simultaneously by "..."
      return fFilter(*(std::static_pointer_cast<TTreeReaderValue<types>>(fReaderValues[S]))->Get() ...);
   }

   void BuildReaderValues(TTreeReader& r) {
      fReaderValues = ::BuildReaderValues(r, fBranchList, f_arg_types(), f_arg_ind());
      fPrevData.BuildReaderValues(r);
   }

   void BookAction(ActionBasePtr ptr) {
      fPrevData.BookAction(ptr);
   }

   void BookFilter(FilterBasePtr ptr) {
      fPrevData.BookFilter(ptr);
   }

   FilterF fFilter;
   const BranchList fBranchList;
   PrevDataFrame& fPrevData;
   TDataFrame& fFirstData;
   TVBVec fReaderValues;
   int fLastCheckedEntry;
   bool fLastResult;
};


class TDataFrame : public TDataFrameInterface<TDataFrame> {
   template<typename A, typename B> friend class TDataFrameAction;
   template<typename A, typename B> friend class TDataFrameFilter;
   friend ActionResultPtrBase;
   friend class TDataFrameInterface<TDataFrame>;

   public:
   TDataFrame(const std::string& treeName, TDirectory* dirPtr, const BranchList& defaultBranches = {})
      : fTreeName(treeName), fDirPtr(dirPtr), fDefaultBranches(defaultBranches),
        fFirstData(*this) {
      TDataFrameInterface<TDataFrame>::fDerivedPtr = this;
   }

   void Run() {
      TTreeReader r(fTreeName.c_str(), fDirPtr);

      // recursive call to all actions and filters
      for(auto actionPtr : fBookedActions)
         actionPtr->BuildReaderValues(r);

      // recursive call to check filters and conditionally executing actions
      while(r.Next())
         for(auto actionPtr : fBookedActions)
            actionPtr->Run(r.GetCurrentEntry());

      // forget everything
      fBookedActions.clear();
      fBookedFilters.clear();
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

   private:
   void BookAction(ActionBasePtr actionPtr) {
      fBookedActions.push_back(actionPtr);
   }

   void BookFilter(FilterBasePtr filterPtr) {
      fBookedFilters.push_back(filterPtr);
   }

   // dummy call, end of recursive chain
   bool CheckFilters(int) {
      return true;
   }

   // dummy call, end of recursive chain
   void BuildReaderValues(TTreeReader&) {
      return;
   }

   // register a ActionResultPtr
   void RegisterActionResult(ActionResultPtrBase* ptr) {
      fActionResultsPtrs.emplace_back(ptr);
   }

   ActionBaseVec fBookedActions;
   FilterBaseVec fBookedFilters;
   ActionResultPtrBaseVec fActionResultsPtrs;
   std::string fTreeName;
   TDirectory* fDirPtr;
   const BranchList fDefaultBranches;
   TDataFrame& fFirstData;
};

// Implementation of methods

void ActionResultPtrBase::TriggerRun() {fFirstData.Run();}
ActionResultPtrBase::ActionResultPtrBase(TDataFrame& firstData):fFirstData(firstData){
   firstData.RegisterActionResult(this);
}

#endif //TDATAFRAME
