# Functional chains for ROOT data analyses
The main aim of this project is allowing ROOT users to easily operate on branches of a TTree by using functional chains.
This makes it possible to make calls such as these:

```c++
TH1F h;
TDataFrame d(tree_name, file_ptr);
auto neg = [](int x) { return x < 0; };
auto fill = [&h](double x, double y) { h->Fill(sqrt(x*x + y*y)); };
d.filter(neg, {"theta"}).foreach(fill, {"pt_x", "pt_y"});
```
Several `filter` calls can be chained before invoking an action (e.g. `foreach`, `get<T>`, `collect_entries`),
and are applied lazily, all at the same time.

The programming model is not purely functional since a `foreach` call has to act on external objects to be of any use.

## Perks
* runtime errors in case of branch-variable type mismatch:
```c++
    Error: the branch contains data of type double.
    It cannot be accessed by a TTreeReaderValue<int>.
```
* does not require any change to ROOT
* no jitting is performed, the framework only relies on c++11 features and c++ templates
* trivial filters are allowed: `c++ d.filter([]() { return true; }, {});` (might be useful if the lambda captures something)
* a default set of branches can be specified when constructing a `TDataFrame`. It will be used as fallback when a set specific to the filter/action is not specified
* parallel `foreach` actions are supported:
```c++
   ROOT::EnableImplicitMT();
   ROOT::TThreadedObject<TH1F> h("h", "h", 100, 0, 1); // thread-safe TH1F
   TDataFrame d(tree_name, file_ptr, {"default_branch"});
   d.filter([]() { return true; }, {})
    .foreach([&h](double def_b) { th_h->Fill(def_b); }); // action executed in parallel over tree entries
   th_h.Merge();
```
* static sanity checks where possible (e.g. filter lambdas must return be bool)
* runtime sanity checks on number of branches/lambda arguments (throws)

## Project files
* `TDataFrame.{h,c}pp`: functional chain implementation
* `root_chain.cpp`: example usage/functionality testing
