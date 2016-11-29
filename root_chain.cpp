#include "TTree.h"
#include "TDataFrame.hpp"

void fill_tree(TTree& t);

int main() {
   TTree t("t", "t");
   fill_tree(t);
   TDataFrame d(t);
   FilterLambda<double> cutb1 = [](double b1) { return b1 < 5.; };
   FilterLambda<int, double> cutb1b2 = [](int b2, double b1) { return b2 % 2 && b1 < 4.; };
   auto entries = d.filter({"b1"}, cutb1)
                   .filter({"b2","b1"}, cutb1b2)
                   .collect_entries();
   
   for(auto x: entries)
      std::cout << "entry " << x << " passed all filters" << std::endl;

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
