#ifndef DATA
#define DATA

#include <list>
#include <type_traits> //std::result_of
#include <numeric> //std::accumulate
#include <algorithm> //std::remove_if

// this is NOT exposed to users
// data is copied by reference from the previous Data object in the chain
template<class T, class Filter=void, class PrevData=void>
class Data {
   template<class A, class B, class C> friend class Data;
   public:
   Data(std::list<T>& _d, Filter _f, PrevData _pd) : d(_d), f(_f), pd(_pd) {}
   Filter get_filter() const { return f; }
   PrevData get_prev() const { return pd; }
   template<class NewFilter> auto filter(NewFilter f) -> Data<T, NewFilter, decltype(*this)> {
      return Data<T, NewFilter, decltype(*this)>(d, f, *this);
   }
   template<class Reduce> auto reduce(Reduce r) -> typename std::result_of<Reduce(T,T)>::type {
      // apply all filters in turn
      apply_filter();

      // reduce
      return std::accumulate(++d.begin(), d.end(), d.front(), r);
   }

   private:
   void apply_filter() {
      // call all filters declared earlier
      pd.apply_filter();
      // apply filter
      d.remove_if(f);
   }
   std::list<T>& d;
   Filter f;
   PrevData pd;
};


// this is exposed to the users
// data is copied by value inside the class
template<class T>
class Data<T, void, void> {
   template<class A, class B, class C> friend class Data;
   public:
   Data(std::list<T> _d) : d(_d) {}
   template<class NewFilter> auto filter(NewFilter f) -> Data<T, NewFilter, decltype(*this)> {
      return Data<T, NewFilter, decltype(*this)>(d, f, *this);
   }
   template<class Reduce> auto reduce(Reduce r) -> typename std::result_of<Reduce(T,T)>::type {
      // reduce
      return std::accumulate(++d.begin(), d.end(), d.front(), r);
   }

   private:
   void apply_filter() { return; }
   std::list<T> d;
};

#endif // DATA
