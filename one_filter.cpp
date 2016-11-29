#include "one_filter.hpp"

void fill_tree(TTree& t);

int main() {
   TTree t("t", "t");
   fill_tree(t);
   std::function<bool(int, double)> f = [](int b2, double b1) { return b2 % 2 && b1 < 4.; };
   A<decltype(f)> a(t, {"b2", "b1"}, f);
   a.filter();
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
