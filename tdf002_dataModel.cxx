/// \file
/// \ingroup tutorial_functionalchains
/// \notebook -nodraw
/// This tutorial shows the possibility to use data models which are more
/// complex than flat ntuples with TDataFrame
///
/// \macro_code
///
/// \author Danilo Piparo
/// \date December 2016

// ## Preparation
// This notebook can be compiled with this invocation
// `g++ -o tdf001_introduction tdf002_dataModel.C `root-config --cflags --libs` -lTreePlayer`

#include "Math/Vector3D.h"
#include "Math/Vector4D.h"
#include "TMath.h"
#include "TRandom3.h"
#include "TFile.h"
#include "TH1F.h"
#include "TTree.h"
#include "ROOT/TSeq.hxx"

#include "TDataFrame.hxx"

using FourVector = ROOT::Math::XYZTVector;
using CylFourVector = ROOT::Math::RhoEtaPhiVector;

// A simple helper function to fill a test tree: this makes the example 
// stand-alone.
void fill_tree(const char* filename, const char* treeName) {
   TFile f(filename,"RECREATE");
   TTree t(treeName,treeName);
   std::vector<FourVector>  tracks;
   t.Branch("tracks", &tracks);

   const double M = 0.13957;  // set pi+ mass
   TRandom3 R(1);

   for (auto i : ROOT::TSeqI(10)) {
      auto nPart = R.Poisson(5);
      tracks.clear();
      tracks.reserve(nPart);
      for (auto j : ROOT::TSeqI(nPart)) {
         double px = R.Gaus(0,10);
         double py = R.Gaus(0,10);
         double pt = sqrt(px*px +py*py);
         double eta = R.Uniform(-3,3);
         double phi = R.Uniform(0.0 , 2*TMath::Pi() );
         CylFourVector vcyl( pt, eta, phi);
         // set energy
         double E = sqrt( vcyl.R()*vcyl.R() + M*M);
         FourVector q( vcyl.X(), vcyl.Y(), vcyl.Z(), E);
         // fill track vector
         tracks.emplace_back(q);
      }
      t.Fill();
   }

   t.Write();
   f.Close();
   return;
}

int tdf002_dataModel() {

   // We prepare an input tree to run on
   auto fileName = "myfile_dataModel.root";
   auto treeName = "myTree";
   fill_tree(fileName,treeName);

   // We read the tree from the file and create a TDataFrame, a class that 
   // allows us to interact with the data contained in the tree.
   TFile f(fileName);
   TDataFrame d(treeName, &f);

   // ## Operating on branches which are collection of objects
   // Here we deal with the simplest of the cuts: we decide to accept the event
   // only if the number of tracks is greater than 5.
   auto n_cut = [](const std::vector<FourVector> & tracks) { return tracks.size() > 5; };
   auto nentries = d.Filter(n_cut, {"tracks"})
                   .Count();

   std::cout << *nentries.get() << " passed all filters" << std::endl;

   return 0;
}

int main(){
   return tdf002_dataModel();
}

