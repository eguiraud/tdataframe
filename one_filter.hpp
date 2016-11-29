#ifndef ONEFILTER
#define ONEFILTER

#include <tuple>
#include <functional>
#include <vector>
#include <string>
#include <utility>
#include "TTreeReaderValue.h"
#include "TTreeReader.h"
using namespace ROOT::Internal;

template<class T>
struct arg_types {
   static_assert(sizeof(T) == -1, "this line should never be compiled");
};

template<class T, class... Args>
struct arg_types<std::function<T(Args...)>> {
   using types = typename std::tuple<Args...>;
};

template<int ...>
struct seq {};

template<int N, int ...S>
struct gens : gens<N-1, N-1, S...> {};

template<int ...S>
struct gens<0, S...>{
   typedef seq<S...> type;
};

template<class F>
class A {
   using arg_types = typename arg_types<F>::types;
   using arg_indexes = typename gens<std::tuple_size<arg_types>::value>::type;
   public:
   A(TTree& _t, std::vector<std::string> _s, F _f) : t(&_t), s(_s), f(_f) {
      build_tvb(arg_indexes());
   }
   void filter() {
      while(t.Next())
         if(applyfilter(arg_indexes()))
            std::cout << t.GetCurrentEntry() << " OK" << std::endl;
   }
   private:
   template<int...S>
   void build_tvb(seq<S...>) {
      tvb = { new TTreeReaderValue< typename std::tuple_element<S, arg_types>::type >(t, s[S].c_str())... };
   }
   template<int...S>
   bool applyfilter(seq<S...>) {
      return f(*(static_cast<TTreeReaderValue<typename std::tuple_element<S, arg_types>::type>*>(tvb[S]))->Get() ...);
   }
   TTreeReader t;
   std::vector<std::string> s;
   F f;
   std::vector<TTreeReaderValueBase*> tvb;
};

#endif // ONEFILTER
