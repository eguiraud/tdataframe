#include "TFile.h"
#include "TTree.h"
#include "TROOT.h"

#include "TDataFrame.hxx"

#include <iostream>

int main() {
   {
      TFile wf("emptyTree.root", "RECREATE");
      TTree t("emptyTree", "emptyTree");
      int a;
      t.Branch("a", &a);
      t.Write();
   }

   TFile f("emptyTree.root");
   ROOT::TDataFrame d("emptyTree", &f, {"a"});
   d.AddBranch("b", []() { return 8; });
   try {
      auto c = d.AddBranch("a", []() { return 42; });
   } catch (const std::runtime_error& e) {
      std::cout << "Exception catched: " << e.what() << std::endl;
   }
   
   return 0;
}
