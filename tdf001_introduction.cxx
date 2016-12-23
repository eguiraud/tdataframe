/// \file
/// \ingroup tutorial_functionalchains
/// \notebook -nodraw
/// This tutorial illustrates the basic features of the TDataFrame class, 
/// a utility which allows to interact with data stored in TTrees following
/// a functional-chain like approach.
///
/// \macro_code
///
/// \author Enrico Guiraud
/// \date December 2016

// ## Preparation
// This notebook can be compiled with this invocation
// `g++ -o tdf001_introduction tdf001_introduction.C `root-config --cflags --libs` -lTreePlayer`

#include "TFile.h"
#include "TH1F.h"
#include "TTree.h"
#include "ROOT/TSeq.hxx"

#include "TDataFrame_old.hxx"

// A simple helper function to fill a test tree: this makes the example 
// stand-alone.
void fill_tree(const char* filename, const char* treeName) {
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

int tdf001_introduction() {

   // We prepare an input tree to run on
   auto fileName = "myfile.root";
   auto treeName = "myTree";
   fill_tree(fileName,treeName);


   // We read the tree from the file and create a TDataFrame, a class that 
   // allows us to interact with the data contained in the tree.
   TFile f(fileName);
   TTree* t;
   f.GetObject(treeName,t);
   TDataFrame d(*t);

   // ## Operations on the dataframe 
   // We now review some "actions" which can be performed on the data frame
   // First of all we define now our cut-flow with two lambda functions. We 
   // can use functions too.
   auto cutb1 = [](double b1) { return b1 < 5.; };
   auto cutb1b2 = [](int b2, double b1) { return b2 % 2 && b1 < 4.; };

   // ### `collect_entries` action
   // The `collect_entries` alloews to retrieve the number of the entries 
   // that passed the filters
   auto entries = d.filter(cutb1, {"b1"})
                   .filter(cutb1b2, {"b2","b1"})
                   .collect_entries();

   for(auto x: entries)
      std::cout << "entry " << x << " passed all filters" << std::endl;

   // ### `get` action
   // The `get` action allows to retrieve all values of the variable stored in a 
   // particular branch that passed filters we specified. The values are stored 
   // in a list.
   auto b1_cut = d.filter(cutb1, {"b1"}).get<double>("b1");
   std::cout << "\nselected b1 entries" << std::endl;
   for(auto b1_entry: b1_cut)
      std::cout << b1_entry << " ";
   std::cout << std::endl;

   // ### `fillhist` action
   // The `fillhist` action allows to fill an histogram. It returns a TH1F filled 
   // with values of the branch that passed the filters.
   auto hist = d.filter(cutb1, {"b1"}).fillhist<double>("b1");
   std::cout << "\nfilled h " << hist.GetEntries() << " times, mean: " << hist.GetMean() << std::endl;

   TH1D hist2("hist2", "hist2", 200, 0, 20);
   d.filter(cutb1, {"b1"}).fillhist<double>("b1", hist2);
   std::cout << "\nfilled h2 " << hist2.GetEntries() << " times, mean: " << hist2.GetMean() << std::endl;

   // ### `foreach` action
   // The most generic action of all: an operation is applied to all entries. 
   // In this case we fill a histogram. In some sense this is a violation of a 
   // purely functional paradigm - C++ allows to do that.
   TH1F h("h", "h", 12, -1, 11);
   d.filter([](int b2) { return b2 % 2 == 0; }, {"b2"})
    .foreach([&h](double b1) { h.Fill(b1); }, {"b1"});

   std::cout << "\nh filled with " << h.GetEntries() << " entries" << std::endl;

   // It is also possible to select the branches which will be used by default
   // upfront. In this case there is no need to specify the name of the input
   // branch of cutb1 (the first cut).
   TDataFrame d2(*t, {"b1"});
   auto entries_bis = d2.filter(cutb1).filter(cutb1b2, {"b2", "b1"}).collect_entries();
   std::cout << "\ndefault branches: "
             << (entries == entries_bis ? "ok" : "ko")
             << std::endl;

   return 0;
}

int main(){
   return tdf001_introduction();
}

