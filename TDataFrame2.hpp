//  TODO
// - static checks: filter returns bool, n_args == branchlist.size
//   (the second one requires BranchList to become an std::array)
// - implement other actions (the ones that return a TDataFrameFuture)
// - implement TDataFrameFuture, hide calls to TDataFrame::Run
#ifndef TDATAFRAME
#define TDATAFRAME

#include <iostream>
#include <list>
#include <vector>
#include <string>
#include <memory>
#include <tuple>
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
template<typename T>
class ActionResultPtr {
   TDataFrame& fFirstData;
   std::shared_ptr<T> fObjPtr;
   ActionResultPtr(T* objPtr): fObjPtr(objPtr),
   {

   }
public:
   T *operator->() {
      return fObjPtr.get();
   }
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


// this class provides a common public interface to TDataFrame and TDataFrameFilter
// it contains the Filter call and all action calls
template<typename Derived>
class TDataFrameInterface {
   public:
   template<typename F>
   auto Filter(F f, const BranchList& bl) -> TDataFrameFilter<F, Derived> {
      auto DFFilterPtr = std::make_shared<TDataFrameFilter<F, Derived>>(f, bl, *fDerivedPtr);
      BookFilter(DFFilterPtr);
      return *DFFilterPtr;
   }

   template<typename F>
   void Foreach(F f, const BranchList& bl) {
      BookAction(std::make_shared<TDataFrameAction<F, Derived>>(f, bl, *fDerivedPtr));
   }

   unsigned* Count() {
      //TODO this needs to be a TDataFramePtr (smart pointer with special effects)
      unsigned* c = new unsigned(0);
      auto countAction = [c]() -> void { (*c)++; };
      BranchList bl = {};
      BookAction(std::make_shared<TDataFrameAction<decltype(countAction), Derived>>(countAction, bl, *fDerivedPtr));
      return c;
   }

   protected:
   Derived* fDerivedPtr;

   private:
   virtual void BookAction(ActionBasePtr ptr) = 0;
   virtual void BookFilter(FilterBasePtr ptr) = 0;
};


template<typename FilterF, typename PrevDataFrame>
class TDataFrameFilter
   : public TDataFrameFilterBase,
     public TDataFrameInterface<TDataFrameFilter<FilterF, PrevDataFrame>>
{
   template<typename A, typename B> friend class TDataFrameAction;
   template<typename A, typename B> friend class TDataFrameFilter;
   using f_arg_types = typename f_traits<FilterF>::arg_types_tuple;
   using f_arg_ind = typename gens<std::tuple_size<f_arg_types>::value>::type;
   using TDFInterface = TDataFrameInterface<TDataFrameFilter<FilterF, PrevDataFrame>>;

   public:
   TDataFrameFilter(FilterF f, const BranchList& bl, PrevDataFrame& pd)
      : fFilter(f), fBranchList(bl), fPrevData(pd), fFirstData(pd.fFirstData), fLastCheckedEntry(-1),
        fLastResult(true) {
      TDFInterface::fDerivedPtr = this;
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

   public:
   TDataFrame(const std::string& treeName, TDirectory* dirPtr)
      : fTreeName(treeName), fDirPtr(dirPtr), fFirstData(*this) {
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
   }

   private:
   ActionBaseVec fBookedActions;
   FilterBaseVec fBookedFilters;

   std::string fTreeName;
   TDataFrame& fFirstData;
   TDirectory* fDirPtr;

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
};

#endif //TDATAFRAME
