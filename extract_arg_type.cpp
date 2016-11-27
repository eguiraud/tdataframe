#include <tuple>
#include <functional>

template<int arg_n, class T>
struct arg_type {
   static_assert(sizeof(T) == -1, "this line should never be compiled");
};

template<int arg_n, class T, class... Args>
struct arg_type<arg_n, std::function<T(Args...)>> {
   static_assert(arg_n < sizeof...(Args), "argument number mismatch");
   using type = typename std::tuple_element<arg_n, std::tuple<Args...>>::type;
};


int main() {
   std::function<double(int, double)> f = [](int a, double b) { return a + b; };
   arg_type<0, decltype(f)>::type same_as_first = 0;
   arg_type<1, decltype(f)>::type same_as_second = 0.;
   return same_as_first;
}
