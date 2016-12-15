#include "TDataFrame2.hpp"
#include "TFile.h"
#include "TTree.h"
#include "ROOT/TSeq.hxx"
#include <cassert>
#include <iostream>

// A simple helper function to fill a test tree and save it to file
// This makes the example stand-alone
void FillTree(const char* filename, const char* treeName) {
   TFile f(filename,"RECREATE");
   TTree t(treeName,treeName);
   double b1;
   int b2;
   t.Branch("b1", &b1);
   t.Branch("b2", &b2);
   for(auto i : ROOT::TSeqI(10)) {
      b1 = i;
      b2 = i*i;
      t.Fill();
   }
   t.Write();
   f.Close();
   return;
}

int main() {
   // Prepare an input tree to run on
   auto fileName = "myfile.root";
   auto treeName = "myTree";
   FillTree(fileName,treeName);

   TFile f(fileName);
   // Define data-frame
   TDataFrame d(treeName, &f);
   // ...and two dummy filters
   auto ok = []() { return true; };
   auto ko = []() { return false; };
   d.Filter(ok, {}).Foreach([](double x) { std::cout << x << std::endl; }, {"b1"});
   d.Run();
   // Fork stream: always apply first filter before doing two different Foreach
   auto dd = d.Filter(ok, {});
   dd.Foreach([](double x) { std::cout << x << " "; }, {"b1"});
   dd.Foreach([](int y) { std::cout << y << std::endl; }, {"b2"});
   // Fork again: now apply two filters before performing action
   auto ddd = dd.Filter(ko, {});
   ddd.Foreach([]() { std::cout << "ERROR" << std::endl; }, {});

   d.Run();
   return 0;
}
