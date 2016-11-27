#ifndef TDATA
#define TDATA

#include <list>
#include <tuple>
#include <functional>
#include "TTree.h"

using BranchList = std::list<std::string>;
using EntryList = std::list<unsigned>;
//use as filterLambda<int,int>
template<class... T> using FilterLambda = std::function<bool(T...)>;


template<class FilterLambda, class PrevTData>
class TTmpData;


class TData {
   template<class A, class B> friend class TTmpData;
   public:
   TData(const TTree& _t) : t(_t) {}
   template<class FL>
   auto filter(const BranchList& bl, FL f) -> TTmpData<FL, decltype(*this)> {
      return TTmpData<FL, decltype(*this)>(t, bl, f, *this);
   }

   private:
   const TTree& t;
};


template<class FilterLambda, class PrevTData>
class TTmpData {
   template<class A, class B> friend class TTmpData;
   public:
   TTmpData(const TTree& _t, const BranchList& _bl, FilterLambda _f, PrevTData& _pd)
      : t(_t), bl(_bl), f(_f), pd(_pd) {}
   template<class FL>
   auto filter(const BranchList& bl, FL f) -> TTmpData<FL, decltype(*this)> {
      return TTmpData<FL, decltype(*this)>(t, bl, f, *this);
   }

   //EntryList collect_entries() const {
      // apply filters
      //make list of branches that we have to read out
      //create tuple with types corresponding to branches
      //for each branch:
      //   t.SetBranchAddress(t[i], tuple[i])
      //n_entries = t.GetEntries();
      //for(unsigned i=0; i<n_entries; ++i) {
      //   t.GetEntry(i);
      //   for each filter:
      //      if !filter(?):
      //         discarded.push_back(i);
      //         break;
      //}


      // create list without discarded entries
      //entries_list = []
      //discarded_index = 0
      //for i in xrange(0, n_entries):
      //   if discarded_index == len(discarded):
      //      entries_list.append(i)
      //   elif i == discarded[discarded_index]:
      //      discarded_index += 1
      //   else:
      //      entries_list.append(i)

   //}

   private:
   const TTree& t;
   const BranchList& bl;
   FilterLambda f;
   PrevTData pd;
};

#endif // TDATA
