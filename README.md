# Functional chains for ROOT data analyses
<a href="https://scan.coverity.com/projects/bluehood-tdataframe">
  <img alt="Coverity Scan Build Status"
       src="https://scan.coverity.com/projects/11179/badge.svg"/>
</a>

The main aim of this project is to allow ROOT users to easily operate on branches of a TTree by using functional chains.
This makes it possible to process trees this way:
```c++
TDataFrame d(treeName, filePtr);
auto isNeg = [](int x) { return x < 0; };
TH1F h;
auto fill = [&h](double x, double y) { h->Fill(sqrt(x*x + y*y)); };
d.Filter(isNeg, {"theta"}).foreach(fill, {"pt_x", "pt_y"});
```
No need for TSelectors or TTreeReader loops anymore. As a huge plus, parallelisation, caching and other optimisations are performed transparently or requiring only minimal action on the user's part.

Try this interactively in your browser now:
<a href="https://cern.ch/swanserver/cgi-bin/go/?projurl=https://github.com/bluehood/tdataframe.git">
<img src="https://swanserver.web.cern.ch/swanserver/images/badge_swan_white_150.png" /></a>

## Contents
* [Quick rundown](#quick-rundown)
* [Run example on lxplus7 in 5 commands](#run-example-on-lxplus7-in-5-commands)
* [Perks](#perks)
* [Example code](#example-code)
* [Project files description](#project-files-description)

## Quick rundown
1. **Build the dataframe**
`TDataFrame` represents the entry point to your dataset. 
2. **Apply some transformations**
   * **cuts**: one or more `Filter` calls can be chained together to apply a series of cuts (i.e. filters) to the tree entries
   * **create temporary branches/columns**: new quantities can be calculated using branch values and can subsequently be referred to as if they were real branches
3. **Execute actions**
A functional chain ends with an *action* call, indicating an operation that will be performed on each entry that passes all filters.
Non-exhaustive list of actions (easy to implement more):
    * `Count`: return the number of entries (that passed all filters)
    * `Min,Max,Mean`: 
    * `Get<T>("branch")`: return a `list<T>` of values of "branch"
    * `Histo("branch")`: return a TH1 filled with the values of "branch"
    * `Foreach(<function>, {"branch1", "branch2"})`: apply `function` to "branch1" and "branch2", once per entry

See [below](#example-code) for an example, or...

## Run example on lxplus7 in 5 commands
```bash
ssh lxplus7
. /cvmfs/sft.cern.ch/lcg/app/releases/ROOT/6.08.02/x86_64-centos7-gcc48-opt/root/bin/thisroot.sh
git clone https://github.com/bluehood/tdataframe
cd tdataframe
root -b tests/test2.cxx
```

## Perks
* lazy evaluation: filters are only applied when an action is performed. This allows for parallelisation and other optimisations to be performed
* support for functional *graphs* as opposed to chains: the state of the chain can be stored and reused at any point. Smart optimisations are performed in order to only loop and execute actions in bulk, as lazily as possible
* a default set of branches can be specified when constructing a `TDataFrame`. It will be used as fallback when a set specific to the filter/action is not specified
* transparent parallelisation of loops over trees becomes possible
* does not require any change to ROOT
* no jitting is performed, the framework only relies on c++11 features and c++ templates
* runtime errors in case of branch-variable type mismatch:
```c++
    Error: the branch contains data of type double.
    It cannot be accessed by a TTreeReaderValue<int>.
```
* static sanity checks where possible (e.g. filter lambdas must return a bool)
* exception-throwing runtime sanity checks (e.g. number of branches == number of lambda arguments)
* `TDataFrame` can do everything a TSelector or a raw loop over a TTreeReader can

## Example code
The following snippet of code applies a strict cut to all events, creates a new temporary branch from the remaining ones, and then "forks" the stream of actions to apply two different cuts and fill histograms with the values contained in the temporary branch.
```c++
TFile file(fileName);
// build dataframe like you would build a TTreeReader
// the branch list is used as fallback whenever none is specified
TDataFrame d(treeName, &file, {"b1","b2"});
// apply a cut and save the state of the chain
auto filtered = d.Filter(myBigCut);
// plot branch "b1" at this point of the chain. no need to specify the type if it's a common one, e.g. double
auto h1 = filtered.Histo("b1");
// create a new branch "v" with a vector extracted from a complex object (only for filtered entries)
// from now on we can refer to "v" as if it was a real branch
auto newBranch = filtered.AddBranch("v", [](const Obj& o) { return o.getVector(); }, {"obj"});
// apply a cut and fill a histogram with "v"
// since "v" is a vector type we loop over it and fill the histogram with its elements
// since "v" is a vector of a common type, say double, we do not need to specify the type
auto h2 = newBranch.Filter(cut1).Histo("v");
// apply a different cut and fill a new histogram
auto h3 = newBranch.Filter(cut2).Histo("v");
// as soon as we access the result of one of the actions, the whole (forked) functional chain is run
// objects read from each branch are built once and never copied around
h2->Draw();
// h1 and h3 have also been filled during the run, no need to loop again when we access their content
h3->Draw("SAME");      
```

## Project files description
* `TDataFrame.hxx`: functional chain implementation
* `tests/*.cxx`: example usage/tutorial/unit testing
* `notebooks/*.ipynb`: ipython notebook with same content as %.C
* `benchmarks/*.cxx`: snippets useful to evaluate `TDataFrame`'s performance
* `test_checker/check.sh`: minimal test checker
* `README.md`: this document
