#include "TDataFrame2.hpp"
#include <cassert>

int main() {
   auto dummy = []() { return; };
   TDataFrame d;
   d.filter(dummy, {}).foreach(dummy, {});
   assert(d.fBookedActions.size() == 1);
   d.foreach(dummy, {});
   return 0;
}
