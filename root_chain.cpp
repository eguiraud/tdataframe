#include "TTree.h"
#include "TDataFrame.hpp"
#include "TH1F.h"

void fill_tree(TTree& t);

int main() {
   TTree t("t", "t");
   fill_tree(t);
   TDataFrame d(t);
   // define filters
   auto cutb1 = [](double b1) { return b1 < 5.; };
   auto cutb1b2 = [](int b2, double b1) { return b2 % 2 && b1 < 4.; };

   // `collect_entries` action
   auto entries = d.filter({"b1"}, cutb1)
                   .filter({"b2","b1"}, cutb1b2)
                   .collect_entries();

   for(auto x: entries)
      std::cout << "entry " << x << " passed all filters" << std::endl;

   // `get` action
   auto b1_cut = d.filter({"b1"}, cutb1).get<double>("b1");

   std::cout << "\nselected b1 entries" << std::endl;
   for(auto b1_entry: b1_cut)
      std::cout << b1_entry << " ";
   std::cout << std::endl;

   // `foreach` action
   TH1F h("h", "h", 12, -1, 11);
   d.filter({"b2"}, [](int b2) { return b2 % 2; })
    .foreach({"b1"}, [&h](double b1) { h.Fill(b1); });

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
