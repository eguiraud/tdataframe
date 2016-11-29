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
   public:
   A(TTree& _t, std::vector<std::string> _s, F _f) : t(&_t), s(_s), f(_f) {
      build_tvb(typename gens<std::tuple_size<typename arg_types<F>::types>::value>::type());
   }
//   void apply() {
//      while(t.Next())
//         if(applyfilter(t.GetCurrentEntry()))
//            std::cout << t.GetCurrentEntry() << " OK" << std::endl;
//   }
   private:
   template<int...S>
   void build_tvb(seq<S...>) {
      tvb = { new TTreeReaderValue< std::tuple_element<S, typename arg_types<F>::types> >(t, s[S].c_str())... };
   }
   TTreeReader t;
   std::vector<std::string> s;
   F f;
   std::vector<TTreeReaderValueBase*> tvb;
};

void fill_tree(TTree& t);

int main() {
   TTree t("t", "t");
   fill_tree(t);
   std::function<bool(int, double)> f = [](int b2, double b1) { return b2 % 2 && b1 < 4.; };
   A<decltype(f)> a(t, {"b1", "b2"}, f);
//   a.apply();
   return 0;
}

void fill_tree(TTree& t) {
   double b1;
   int b2;
   t.Branch("b1", &b1);
   t.Branch("b2", &b2);
   for(unsigned i = 0; i < 10; ++i) {
      b1 = i;
      b2 = i*i;
      t.Fill();
   }
   return;
}
