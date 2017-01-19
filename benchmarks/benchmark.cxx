#include <iostream>
#include <vector>
#include "../TDataFrame.hxx"
#include "TFile.h"
#include "TMath.h"
#include "TTree.h"
#include "TRandom3.h"
#include "ROOT/TSeq.hxx"
#include "TSystem.h"
#include "Math/Vector3D.h"
#include "Math/Vector4D.h"
#include "TMath.h"


const char* fileName = "myBigfile.root";
const char* treeName = "myTree";
const unsigned int nevts = 100000;
const unsigned int poolSize = 4;

using FourVector = ROOT::Math::XYZTVector;
using FourVectors = std::vector<FourVector>;
using CylFourVector = ROOT::Math::RhoEtaPhiVector;

// A simple class to measure time.
class TimerRAII{
   using timePoint = std::chrono::time_point<std::chrono::system_clock>;
public:
   TimerRAII():fStart(std::chrono::high_resolution_clock::now()){};
   ~TimerRAII(){
      std::chrono::duration<double> deltaT=std::chrono::high_resolution_clock::now()-fStart;
      std::cout << "\nElapsed time: " << deltaT.count() << "s\n";
   }
private:
   timePoint fStart;
};



void getTracks(unsigned int mu, FourVectors& tracks) {
   static TRandom R(1);
   const double M = 0.13957;  // set pi+ mass
   auto nPart = R.Poisson(mu);
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
}

// A simple helper function to fill a test tree and save it to file
// This makes the example stand-alone
void FillTree(const char* filename, const char* treeName) {
   if (!gSystem->AccessPathName(filename)) return;
   TFile f(filename,"RECREATE");
   TTree t(treeName,treeName);
   double b1;
   int b2;
   std::vector<FourVector> tracks;
   std::vector<double> dv {-1,2,3,4};
   std::list<int> sl {1,2,3,4};
   t.Branch("b1", &b1);
   t.Branch("b2", &b2);
   t.Branch("tracks", &tracks);
   t.Branch("dv", &dv);
   t.Branch("sl", &sl);

   for(auto i : ROOT::TSeqI(nevts)) {
      if (i % 5000 == 0)
         std::cout << "Event " << i << " / " << nevts << std::endl;
      b1 = i;
      b2 = i*i;

      getTracks(1, tracks);

      dv.emplace_back(i);
      sl.emplace_back(i);
      t.Fill();
   }
   t.Write();
   f.Close();
   return;
}


void RunTDataFrame(TFile& f){

   auto getPt = [](const FourVectors& tracks) {
      std::vector<double> pts;
      pts.reserve(tracks.size());
      for (auto& t:tracks)
         pts.emplace_back(t.Pt());
      return pts;
      };

//   auto getPxPyPz = [](const FourVectors& tracks) {
//      std::vector<double> pxpypz;
//      pxpypz.reserve(tracks.size());
//      for (auto& t:tracks)
//         pxpypz.emplace_back(t.Px() + t.Py() + t.Pz());
//      return pxpypz;
//      };

   ROOT::TDataFrame d(treeName, &f, {"tracks"});
   auto ad = d.AddBranch("tracks_n", [](const FourVectors& tracks){return (int)tracks.size();})
               .Filter([](int tracks_n){return tracks_n > 2;}, {"tracks_n"})
               .AddBranch("tracks_pts", getPt);
//               .AddBranch("tracks_pxpypz", getPxPyPz);
   auto trPt = ad.Histo("tracks_pts");
//    auto trPts = ad.Histo("tracks_pts");
//    auto trPxPyPx = ad.Histo("tracks_pxpypz");
//    *trPxPyPx;
   *trPt;
}

void LoopRunTDataFrame(int n, TFile& f) {
   for (int i = 0 ; i< n; ++i) {
      RunTDataFrame(f);
   }
}

void RunTTreeDraw(TFile& f) {
   auto tree = (TTree*) f.Get(treeName);
   tree->Draw("tracks.Pt() >> tPt", "@tracks > 2");
//    tree->Draw("@tracks >> tN", "@tracks > 2");
//    tree->Draw("tracks.Px() + tracks.Py() + tracks.Pz() >> tPxPyPz", "@tracks > 2");
}

void LoopRunTTreeDraw(int n, TFile& f) {
   for (int i = 0 ; i< n; ++i) {
      RunTTreeDraw(f);
   }
}


int main(){

   FillTree(fileName, treeName);

   TFile f(fileName);

   auto getPt = [](const FourVectors& tracks) {
   std::vector<double> pts;
   pts.reserve(tracks.size());
   for (auto& t:tracks)
      pts.emplace_back(t.Pt());
   return pts;
   };

   int warmuploops = 10;
   int measurementloops = 1;

   // TTree Draw ---------------------------------------------------------------
   LoopRunTTreeDraw(warmuploops, f);
   {
      TimerRAII tr;
      LoopRunTTreeDraw(measurementloops, f);
      std::cout << "TTreeDraw measurement with " << measurementloops << " loops.";
   }

   // TDataFrame Draw ----------------------------------------------------------
   LoopRunTDataFrame(warmuploops, f);

   // measure
   {
      TimerRAII tr;
      LoopRunTDataFrame(measurementloops, f);
      std::cout << "TDataFrame measurement with " << measurementloops << " loops.";
   }

   // TDataFrame Draw // -------------------------------------------------------
   ROOT::EnableImplicitMT(poolSize);
   LoopRunTDataFrame(warmuploops, f);

   // measure
   {
      TimerRAII tr;
      LoopRunTDataFrame(measurementloops, f);
      std::cout << "TDataFrame measurement with a pool size of " << poolSize << " with " << measurementloops << " loops.";
   }


}
