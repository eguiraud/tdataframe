# Functional chains for ROOT data analyses
The main aim of this project is allowing ROOT users to easily operate on branches of a TTree by using functional chains.
This makes it possible to make calls such as these:

```
#!c++

TH1F h;
TDataFrame d(tree);
auto neg = [](int x) { return x < 0; };
auto fill = [&h](double x, double y) { h->Fill(sqrt(x*x + y*y)); };
d.filter({"theta"}, neg).foreach({"pt_x", "pt_y"}, fill);
```
Several `filter` calls can be chained before invoking an action (e.g. `foreach`, `get<T>`, `collect_entries`),
and are applied lazily, all at the same time.

At least for now, the programming model is not purely functional since a `foreach` call has to act on outside objects to be of any use.

## Perks
* simple loop over entries can be trivially parallelised:
```
#!c++

while(reader.Next())
   // recursive, ordered call to all filters                                              
   if(apply_filters())
      do_stuff();
```

* very reasonable compilation times
* runtime errors in case of branch-variable type mismatch:
```
#!c++

Error: the branch contains data of type double.
It cannot be accessed by a TTreeReaderValue<int>.
```

* does not require any change to ROOT
* no jitting is performed, the framework only relies on c++11 features and c++ templates


## Project files
* `TDataFrame.{h,c}pp`: functional chain implementation
* `root_chain.cpp`: example usage/functionality testing