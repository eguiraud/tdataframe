// TODO
// - extracting filter and foreach from TDataFrame and TDataFrameFilter (they are identical)
#ifndef TDATAFRAME
#define TDATAFRAME

#include <list>
#include <vector>
#include <string>
#include <memory>
#include <tuple>
#include "TTreeReaderValue.h"
#include "TTreeReader.h"

/******* meta-utils **********/
// extract parameter types from a callable object
// credits to kennytm 
template<typename T>
struct f_traits : public f_traits<decltype(&T::operator())> {};

// lambdas and std::function
template<typename R, typename T, typename... Args>
struct f_traits<R(T::*)(Args...) const> {
   using arg_types_tuple = typename std::tuple<Args...>;
   using ret_t = R;
};

// mutable lambdas and functor classes
template<typename R, typename T, typename... Args>
struct f_traits<R(T::*)(Args...)> {
   using arg_types_tuple = typename std::tuple<Args...>;
   using ret_t = R;
};

// free functions
template<typename R, typename... Args>
struct f_traits<R(*)(Args...)> {
   using arg_types_tuple = typename std::tuple<Args...>;
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
class TDataFrameActionBase;
using ABVec = std::vector<std::shared_ptr<TDataFrameActionBase>>;


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


template<typename F, typename PrevDataFrame>
class TDataFrameAction : public TDataFrameActionBase {
   public:
   TDataFrameAction(F f, const BranchList& bl, PrevDataFrame pd)
      : fAction(f), fBranchList(bl), fPrevData(pd) { }

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
   TVBVec fReaderValues;
   using f_arg_types = typename f_traits<F>::arg_types_tuple;
   using f_arg_ind = typename gens<std::tuple_size<f_arg_types>::value>::type;
};


template<typename FilterF, typename PrevDataFrame>
class TDataFrameFilter {
   template<typename A, typename B> friend class TDataFrameAction;
   template<typename A, typename B> friend class TDataFrameFilter;

   public:
   TDataFrameFilter(FilterF f, const BranchList& bl, PrevDataFrame pd)
      : fFilter(f), fBranchList(bl), fPrevData(pd), fLastCheckedEntry(-1),
        fLastResult(true) { }

   template<typename F>
   auto Filter(F f, const BranchList& bl) -> TDataFrameFilter<F, decltype(*this)> {
      return TDataFrameFilter<F, decltype(*this)>(f, bl, *this);
   }

   template<typename F>
   void Foreach(F f, const BranchList& bl) {
      Book(std::make_shared<TDataFrameAction<F, decltype(*this)>>(f, bl, *this));
   }

   private:
   bool CheckFilters(int entry) {
      if(entry == fLastCheckedEntry)
         return fLastResult;
      if(!fPrevData.CheckFilters(entry))
         return false;
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

   void Book(std::shared_ptr<TDataFrameActionBase> ptr) {
      fPrevData.Book(ptr);
   }

   FilterF fFilter;
   const BranchList fBranchList;
   PrevDataFrame& fPrevData;
   TVBVec fReaderValues;
   int fLastCheckedEntry;
   bool fLastResult;
   using f_arg_types = typename f_traits<FilterF>::arg_types_tuple;
   using f_arg_ind = typename gens<std::tuple_size<f_arg_types>::value>::type;
};


class TDataFrame {
   template<typename A, typename B> friend class TDataFrameAction;
   template<typename A, typename B> friend class TDataFrameFilter;

   public:
   TDataFrame(const std::string& treeName, TDirectory* dirPtr)
      : fTreeName(treeName), fDirPtr(dirPtr) {};
   template<typename F>
   auto Filter(F f, const BranchList& bl) -> TDataFrameFilter<F, decltype(*this)> {
      return TDataFrameFilter<F, decltype(*this)>(f, bl, *this);
   }

   template<typename F>
   void Foreach(F f, const BranchList& bl) {
      Book(std::make_shared<TDataFrameAction<F, decltype(*this)>>(f, bl, *this));
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
   }

   private:
   ABVec fBookedActions;
   std::string fTreeName;
   TDirectory* fDirPtr;

   void Book(std::shared_ptr<TDataFrameActionBase> actionPtr) {
      fBookedActions.push_back(actionPtr);
   }

   // dummy call, end of recursive chain
   bool CheckFilters(int entry) {
      return true;
   }

   // dummy call, end of recursive chain
   void BuildReaderValues(TTreeReader& r) {
      return;
   }
};

#endif //TDATAFRAME
