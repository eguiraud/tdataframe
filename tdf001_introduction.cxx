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

#include "TDataFrame.hxx"

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
   TDataFrame d(treeName, &f);

   // ## Operations on the dataframe 
   // We now review some "actions" which can be performed on the data frame.
   // All actions but ForEach return a TActionResultPtr<T>. The series of operations
   // on the data frame is not executed until one of those pointers is accessed or 
   // the TDataFrame::Run method is invoked.
   // But first of all, let us we define now our cut-flow with two lambda functions. 
   // We can use functions too.
   auto cutb1 = [](double b1) { return b1 < 5.; };
   auto cutb1b2 = [](int b2, double b1) { return b2 % 2 && b1 < 4.; };

   // ### `Count` action
   // The `Count` allows to retrieve the number of the entries 
   // that passed the filters
   auto entries = d.Filter(cutb1, {"b1"})
                   .Filter(cutb1b2, {"b2","b1"})
                   .Count();

   std::cout << *entries << " entries passed all filters" << std::endl;

   // ### `Min`, `Max` and `Mean` actions
   // These actions allow to retrieve statistical information about the entries
   // passing the cuts, if any.
   auto& b1b2_cut = d.Filter(cutb1b2, {"b2","b1"});
   auto minVal = b1b2_cut.Min("b1");
   auto maxVal = b1b2_cut.Max("b1");
   auto meanVal = b1b2_cut.Mean("b1");
   std::cout << "The mean is always included between the min and the max: " 
             << *minVal << " <= " << *meanVal << " <= " << *maxVal << std::endl;

   // ### `Get` action
   // The `Get` action allows to retrieve all values of the variable stored in a 
   // particular branch that passed filters we specified. The values are stored 
   // in a list by default, but other collections can be chosen.
   auto& b1_cut = d.Filter(cutb1, {"b1"});
   auto b1List = b1_cut.Get<double>("b1");
   auto b1Vec = b1_cut.Get<double, std::vector<double>>("b1");

   std::cout << "\nselected b1 entries" << std::endl;
   for(auto b1_entry : *b1List)
      std::cout << b1_entry << " ";
   std::cout << std::endl;
   auto cl = TClass::GetClass(typeid(*b1Vec));
   std::cout << "The type of b1Vec is" << cl->GetName() << std::endl;

   // ### `Histo` action
   // The `Histo` action allows to fill an histogram. It returns a TH1F filled 
   // with values of the branch that passed the filters. For the most common types, the type of 
   // the branch is automatically guessed.
   auto hist = d.Filter(cutb1, {"b1"}).Histo("b1");
   std::cout << "\nfilled h " << hist->GetEntries() << " times, mean: " << hist->GetMean() << std::endl;

   // ### `Foreach` action
   // The most generic action of all: an operation is applied to all entries. 
   // In this case we fill a histogram. In some sense this is a violation of a 
   // purely functional paradigm - C++ allows to do that.
   TH1F h("h", "h", 12, -1, 11);
   d.Filter([](int b2) { return b2 % 2 == 0; }, {"b2"})
    .Foreach([&h](double b1) { h.Fill(b1); }, {"b1"});
   d.Run();

   std::cout << "\nh filled with " << h.GetEntries() << " entries" << std::endl;

   // It is also possible to select the branches which will be used by default
   // upfront. In this case there is no need to specify the name of the input
   // branch of cutb1 (the first cut).
   TDataFrame d2(treeName, &f, {"b1"});
   auto entries_bis = d2.Filter(cutb1).Filter(cutb1b2, {"b2", "b1"}).Count();
   std::cout << "After the cuts, we are left with " << *entries_bis << " entries\n";


   return 0;
}

int main(){
   return tdf001_introduction();
}

