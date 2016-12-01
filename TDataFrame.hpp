#ifndef TDATAFRAME
#define TDATAFRAME

#include <vector>
#include <list>
#include <tuple>
#include <functional>
#include <string>
#include "TTree.h"
#include "TTreeReaderValue.h"
#include "TTreeReader.h"

/******* meta-utils **********/
// extract parameter types from a callable object
template<typename T>
struct arg_types : public arg_types<decltype(&T::operator())> {};

// lambdas and std::function
template<typename R, typename T, typename... Args>
struct arg_types<R(T::*)(Args...) const> {
   using types_tuple = typename std::tuple<Args...>;
};

// mutable lambdas and functor classes
template<typename R, typename T, typename... Args>
struct arg_types<R(T::*)(Args...)> {
   using types_tuple = typename std::tuple<Args...>;
};

// free functions
template<typename R, typename... Args>
struct arg_types<R(*)(Args...)> {
   using types_tuple = typename std::tuple<Args...>;
};


// compile-time integer sequence generator
// e.g. calling gens<3>::type() instantiates a seq<0,1,2>
// credits to Johannes Schaub (see http://stackoverflow.com/q/7858817/2195972)
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
using EntryList = std::list<unsigned>;
using TVB = ROOT::Internal::TTreeReaderValueBase;


// forward declaration (needed by TDataFrame)
template<class Filter, class PrevData>
class TTmpDataFrame;


class TDataFrame {
   template<class A, class B> friend class TTmpDataFrame;

   public:
   TDataFrame(TTree& _t) : t(&_t) {}
   template<class Filter>
   auto filter(const BranchList& bl, Filter f) -> TTmpDataFrame<Filter, decltype(*this)> {
      // Every time this TDataFrame is (re)used we want a fresh TTreeReader
      t.Restart();
      // Create a TTmpDataFrame that contains *this (and the new filter)
      return TTmpDataFrame<Filter, decltype(*this)>(t, bl, f, *this);
   }

   private:
   bool apply_filters() {
      // Dummy call (end of recursive chain of calls)
      return true;
   }
   TTreeReader t;
};


template<int... S, typename... arg_types>
std::vector<TVB*> build_tvb(TTreeReader& t, const BranchList& bl, std::tuple<arg_types...>, seq<S...>) {
   // Build vector of pointers to TTreeReaderValueBase.
   // tvb[i] is a TTreeReaderValue specialized for the i-th arg_type
   // S must be a sequence of sizeof...(arg_types) integers
   // arg_types and S are expanded simultaneously by "..."
   std::vector<TVB*> tvb{ new TTreeReaderValue <arg_types>(t, bl.at(S).c_str())... };
   return tvb;
}


template<class Filter, class PrevData>
class TTmpDataFrame {
   template<class A, class B> friend class TTmpDataFrame;
   template<class A, class B> friend class TTmpDataProcessor;
   using filter_types = typename arg_types<Filter>::types_tuple;
   using filter_ind = typename gens<std::tuple_size<filter_types>::value>::type;

   public:
   TTmpDataFrame(TTreeReader& _t, const BranchList& _bl, Filter _f,
                 PrevData& _pd) : t(_t), bl(_bl), f(_f), pd(_pd) {
      // Call helper function to build vector<TTreeReaderValueBase>
      tvb = build_tvb(t, bl, filter_types(), filter_ind());
   }

   ~TTmpDataFrame() {
      for(auto p: tvb)
         delete p;
   }

   template<class NewFilter>
   auto filter(const BranchList& bl, NewFilter f) -> TTmpDataFrame<NewFilter, decltype(*this)> {
      // Create a TTmpDataFrame that contains *this (and the new filter)
      return TTmpDataFrame<NewFilter, decltype(*this)>(t, bl, f, *this);
   }

   EntryList collect_entries() {
      EntryList l;
      while(t.Next())
         if(apply_filters())
            l.push_back(t.GetCurrentEntry());
      return l;
   }

   template<class T>
   std::list<T> get(std::string branch) {
      std::list<T> res;
      TTreeReaderValue<T> v(t, branch.c_str());
      while(t.Next())
         if(apply_filters())
            res.push_back(*v);

      return res;
   }

   private:
   bool apply_filters() {
      return apply_filters(filter_types(), filter_ind());
   }

   template<int... S, typename... types>
   bool apply_filters(std::tuple<types...>, seq<S...>) {
      // Recursive call to all previous filters
      if(!pd.apply_filters())
         return false;

      // Apply our filter
      // Take each pointer in tvb, cast it to a pointer to the
      // correct specialization of TTreeReaderValue, and get its content.
      // S expands to a sequence of integers 0 to sizeof...(types)-1
      // S and types are expanded simultaneously by "..."
      return f(*(static_cast<TTreeReaderValue<types>*>(tvb[S]))->Get() ...);
   }

   TTreeReader& t;
   std::vector<TVB*> tvb;
   const BranchList& bl;
   Filter f;
   PrevData pd;
};

#endif // TDATAFRAME

/*
Wish list:
- build TTreeReaderValues right before looping over TTree
- only build TTreeReaderValues once per branch
- static check that filters return a bool
*/
