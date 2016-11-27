#include <list>
#include "Data.h"

bool prime(unsigned n);
unsigned sum(unsigned a, unsigned b);

int main() {
   std::list<int> args = {1,2,3,4,5,6,7,8,9};
   auto even = [](int n) { return n % 2 == 0; };
   Data<int> d(args);
   return d.filter(even).filter(prime).reduce(sum);
}


bool prime(unsigned n) {
   for(unsigned i=2; i < n; ++i)
      if(n % i == 0)
         return false;
   return true;
}


unsigned sum(unsigned a, unsigned b) {
   return a+b;
}
