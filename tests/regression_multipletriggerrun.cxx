#include "ROOT/TSeq.hxx"
#include "TDataFrame.hxx"
#include "TFile.h"
#include "TTree.h"

void fill_tree(const char* filename, const char* treeName) {
   TFile f(filename,"RECREATE");
   TTree t(treeName,treeName);
   double b1;
   t.Branch("b1", &b1);
   b1 = 1;
   t.Fill();
   t.Write();
   f.Close();
   return;
}

int main() {
   // We prepare an input tree to run on
   auto fileName = "myfile.root";
   auto treeName = "myTree";
   fill_tree(fileName,treeName);

   TFile file(fileName);
   ROOT::TDataFrame d(treeName, &file, {"b1"});
   auto sentinel = []() { std::cout << "filter called" << std::endl; return true; };
   auto f1 = d.Filter(sentinel, {});
   auto m1 = f1.Min();
   auto trigger1 = *m1;
   std::cout << "end first run" << std::endl;
   auto f2 = d.Filter(sentinel, {});
   auto dummy = f2.Max();
   trigger1 = *m1; // this should NOT cause a second printing of "filter called"

   return 0;
}
