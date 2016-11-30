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
template<typename T>
struct arg_types : public arg_types<decltype(&T::operator())> {};

// convert types of arguments of std::function to corresponding std::tuple
template<typename R, typename T, typename... Args>
struct arg_types<R(T::*)(Args...) const> {
   using types = typename std::tuple<Args...>;
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


template<class Filter, class PrevData>
class TTmpDataFrame {
   template<class A, class B> friend class TTmpDataFrame;
   using arg_types = typename arg_types<Filter>::types;
   using arg_indexes = typename gens<std::tuple_size<arg_types>::value>::type;

   public:
   TTmpDataFrame(TTreeReader& _t, const BranchList& _bl, Filter _f,
                 PrevData& _pd) : t(_t), bl(_bl), f(_f), pd(_pd) {
      // Call helper function to build vector<TTreeReaderValueBase>
      build_tvb(arg_indexes());
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
   template<int...S>
   void build_tvb(seq<S...>) {
      // Build vector of pointers to TTreeReaderValueBase. Each element points
      // to a TTreeReaderValue that corresponds to a different argument of
      // the filter lambda
      tvb = { new TTreeReaderValue
                  < typename std::tuple_element<S, arg_types>::type >
                  (t, bl[S].c_str())
              ... // expand over the values of the parameter pack S
            };
   }

   template<int...S>
   bool apply_filters(seq<S...>) {
      // Recursive call to all previous filters
      if(!pd.apply_filters())
         return false;

      // Call our filter
      // The argument takes each pointer in tvb, casts it to a pointer to the
      // correct specialization of TTreeReaderValue, and gets its content.
      // N.B. S expands to a sequence of integers 0..N-1 where N is the number
      // of arguments taken by f.
      return f(* (static_cast< TTreeReaderValue< typename std::tuple_element< S, arg_types >::type > * >
                    (tvb[S])
                 )->Get()
               ... // expand over the values of the parameter pack S
              );
   }

   bool apply_filters() {
      return apply_filters(arg_indexes());
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
*/
