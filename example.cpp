// Compile with  g++ -o root_chain root_chain.cpp `root-config --cflags --libs` -lTreePlayer
#include "TTree.h"
#include "TFile.h"
#include "TDataFrame.hpp"
#include "TH1F.h"

// A simple function to fill a test tree
void fill_tree(const char*, const char*);

int root_chain() {

   // We prepare an input tree for this example --------------------------------
   auto fileName = "myfile.root";
   auto treeName = "myTree";
   fill_tree(fileName,treeName);
   // End of the preparation ---------------------------------------------------

   // Here starts the fun

   // We read the tree from the file
   TFile f(fileName);
   TTree* t;
   f.GetObject(treeName,t);

   // TDataFrame is the class which allows us to interact with the data contained
   // in the tree
   TDataFrame d(*t);

   // We define our cut-flow
   auto cutb1 = [](double b1) { return b1 < 5.; };
   auto cutb1b2 = [](int b2, double b1) { return b2 % 2 && b1 < 4.; };

   // We now review some "actions" which can be performed on the data frame

   // 1) `collect_entries` action: retrieve all entries that passed the filters
   auto entries = d.filter(cutb1, {"b1"})
                   .filter(cutb1b2, {"b2","b1"})
                   .collect_entries();

   for(auto x: entries)
      std::cout << "entry " << x << " passed all filters" << std::endl;

   // 2) `get` action: retrieve all values of the branch that passed filters
   auto b1_cut = d.filter(cutb1, {"b1"}).get<double>("b1");

   std::cout << "\nselected b1 entries" << std::endl;
   for(auto b1_entry: b1_cut)
      std::cout << b1_entry << " ";
   std::cout << std::endl;

   // 3) `fillhist` action: return a TH1F filled with values of the branch that
   // passed the filters
   auto hist = d.filter(cutb1, {"b1"}).fillhist<double>("b1");
   std::cout << "\nfilled h " << hist.GetEntries() << " times" << std::endl;

   // 4) `foreach` action: the most generic of all. In this case we fill a
   // histogram
   TH1F h("h", "h", 12, -1, 11);
   d.filter([](int b2) { return b2 % 2 == 0; }, {"b2"})
    .foreach([&h](double b1) { h.Fill(b1); }, {"b1"});

   std::cout << "\nh filled with " << h.GetEntries() << " entries" << std::endl;

   // It is also possible to select the branches which will be used by default
   // upfront. In this case there is no need to specify the name of the input
   // branch of cutb1 (the first cut)
   TDataFrame d2(*t, {"b1"});
   auto entries_bis = d2.filter(cutb1).filter(cutb1b2, {"b2", "b1"}).collect_entries();
   std::cout << "\ndefault branches: "
             << (entries == entries_bis ? "ok" : "ko")
             << std::endl;

   return 0;
}

void fill_tree(const char* filename, const char* treeName) {
   TFile f(filename,"RECREATE");
   TTree t(treeName,treeName);
   double b1;
   int b2;
   t.Branch("b1", &b1);
   t.Branch("b2", &b2);
   for(unsigned i = 0; i < 10; ++i) {
      b1 = i;
      b2 = i*i;
      t.Fill();
   }
   t.Write();
   f.Close();
   return;
}

int main(){
   return root_chain();
}

