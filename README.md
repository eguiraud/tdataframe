# Functional chains for ROOT data analyses
The main aim of this project is to allow ROOT users to easily operate on branches of a TTree by using functional chains.
This makes it possible to process trees this way:
```c++
TDataFrame d(tree_name, file_ptr);
auto neg = [](int x) { return x < 0; };
TH1F h;
auto fill = [&h](double x, double y) { h->Fill(sqrt(x*x + y*y)); };
d.filter(neg, {"theta"}).foreach(fill, {"pt_x", "pt_y"});
```
No need for TSelectors or TTreeReader loops anymore. As a huge plus, parallelisation, caching and other optimisations are performed transparently or requiring only minimal action on the user's part.

## Contents
* [Quick rundown](#quick-rundown)
* [Run example on lxplus7 in 5 commands](#run-example-on-lxplus7-in-5-commands)
* [Perks](#perks)
* [Example code](#example-code)
* [Project files description](#project-files-description)

## Quick rundown
1. **Build the dataframe**
`TDataFrame` represents the entry point to your dataset. 
2. **Apply some cuts**
Starting from a `TDataFrame`, one or more `filter` calls can be chained together to apply a series of cuts (i.e. filters) to the tree entries.
3. **Operate on entries that pass all cuts**
After filtering, an *action* must be invoked, and it will be performed on each entry that passes all filters.
Non-exhaustive list of actions (easy to implement more):
    * `count()`: return the number of entries (passing all filters)
    * `get<T>("branch")`: return a `list<T>` of values of "branch"
    * `fillhist("branch")`: return a TH1F filled with the values of "branch"
    * `foreach(<function>, {"branch1", "branch2"})`: apply `function` to "branch1" and "branch2" 

See [below](#example-code) for some examples.

## Run example on lxplus7 in 5 commands
```bash
ssh lxplus7
. /cvmfs/sft.cern.ch/lcg/app/releases/ROOT/6.08.02/x86_64-centos7-gcc48-opt/root/bin/thisroot.sh
git clone https://github.com/bluehood/tdataframe
cd tdataframe
root -b example.cpp
```

## Perks
* lazy evaluation: filters are only applied when an action is performed. This allows for parallelisation and other optimisations to be performed
* a default set of branches can be specified when constructing a `TDataFrame`. It will be used as fallback when a set specific to the filter/action is not specified
* parallel `foreach` actions are supported:
```c++
   ROOT::EnableImplicitMT();
   ROOT::TThreadedObject<TH1F> h("h", "h", 100, 0, 1); // thread-safe TH1F
   TDataFrame d(tree_name, file_ptr, {"default_branch"});
   d.filter([]() { return true; }, {})
    .foreach([&h](double def_b) { th_h->Fill(def_b); }); // parallel over tree entries
   th_h.Merge();
```
* does not require any change to ROOT
* no jitting is performed, the framework only relies on c++11 features and c++ templates
* runtime errors in case of branch-variable type mismatch:
```c++
    Error: the branch contains data of type double.
    It cannot be accessed by a TTreeReaderValue<int>.
```
* trivial filters are allowed: `c++ d.filter([]() { return true; }, {});` (might be useful if the lambda captures something)
* static sanity checks where possible (e.g. filter lambdas must return a bool)
* exception-throwing runtime sanity checks (e.g. number of branches == number of lambda arguments)
* `TDataFrame` can do everything a TSelector or a raw loop over a TTreeReader can

## Example code
This should compile against ROOT's master (or ROOT v6.08.2).
It will not run because you don't have my test tree, I'm just putting this here as an example snippet.
Parallel processing requires ROOT to be compiled with `-Dimt=ON` (support for implicit multi-threading enabled).

```c++
#include "TFile.h"
#include "TDataFrame.hpp"                                                       
#include "ROOT/TThreadedObject.hxx"                                             
#include "TH1F.h"

int main() {                                                                    
   TFile file("mytree.root");                                                   
   TDataFrame d("mytree", &file);                                               
   
   // define filters                                                            
   auto cutx = [](double x) { return x < 5.; };                              
   auto cutxy = [](int x, double y) { return x % 2 && y < 4.; };          
                                                                                 
   // `get` action         
   std::list<double> x_cut = d.filter(cutx, {"x"}).get<double>("x");
                                                                                
   // `fillhist` action                                                
   TH1F hist = d.filter(cutx, {"x"}).fillhist("x");    
                                                                                
   // on-the-fly filter and `foreach` action      
   TH1F h("h", "h", 10, 0, 1);                                                
   d.filter([](int y) { return y > 0; }, {"y"})                         
    .foreach([&h](double y) { h.Fill(y); }, {"y"});                          
                                                                 
   // parallel `foreach`: same thing but in multi-threading                     
   ROOT::EnableImplicitMT();                                                   
   ROOT::TThreadedObject<TH1F> th_h("h", "h", 10, 0, 1);   // thread-safe TH1F                 
   d.filter([](int x) { return x % 2 == 0; }, {"x"})       // parallel loop over entries     
    .foreach({"x"}, [&th_h](double x) { th_h->Fill(x); }); // multi-thread filling is performed             
   th_h.Merge();                                      
                                                                                
   // default branch specified in TDataFrame ctor, so no need to pass it to `filter`
   // unless we specify a different one
   TDataFrame d2("mytree", &file, {"x"});                                      
   std::list<unsigned> entries2 = d2.filter(cutx).filter(cutxy, {"x", "y"}).collect_entries();
   
   return 0;                                                                    
}               

```

## Project files description
* `TDataFrame.hpp`: functional chain implementation
* `example.cpp`: example usage/functionality testing
