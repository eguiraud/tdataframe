# TDataFrame User Guide
`TDataFrame` allows to analyse data stored in TTrees with a high level interface, exploiting all the resources available on the machine in a transparent way for the user.

```c++
ROOT::EnableImplicitMT();
ROOT::TDataFrame d("myTree", file);
auto myHisto = d.Histo("Branch_A"); // This happens in parallel!
myHisto->Draw();
```

Calculations are expressed in terms of a type-safe *functional chain of actions and transformations*, `TDataFrame` takes care of their execution. The implementation automatically puts in place several low level optimisations such as multi-thread parallelisation and caching.

## Table of Contents
[Introduction](#introduction)<br>
[Crash course](#crash-course)<br>
[More features](#more-features)<br>
[Transformations](#transformations)<br>
[Actions](#actions)<br>
[Parallel execution](#parallel-execution)

## Introduction
A pipeline of operations is described to be performed on the data, the framework takes care
of the management of the loop over entries as well as low-level details such as I/O and parallelisation.
`TDataFrame` provides an interface to perform most common operations required by ROOT analyses;
at the same time, the users are not limited to those
common operations: building blocks to trigger custom calculations are available too.

`TDataFrame` is built with a *modular* and *flexible* workflow in mind, summarised as follows:

1.  **build a data-frame** object by specifying your data-set
2.  **apply a series of transformations** to your data
    1.  **filter** (e.g. apply some cuts) or
    2.  create a **new branch** (e.g. make available an alias or the result of a non trivial operation involving other branches)
3.  **apply actions** to the transformed data to produce results (e.g. fill a histogram)

<table>
<tr>
<td>
<b>TTreeReader</b>
</td>
<td>
<b>TDataFrame</b>
</tr>
<tr>
<td>
   <pre lang="c++">
TTreeReader reader("myTree", file);
TTreeReaderValue&lt;A_t&gt; a(reader, "A");
TTreeReaderValue&lt;B_t&gt; b(reader, "B");
TTreeReaderValue&lt;C_t&gt; c(reader, "C");
while(reader.Next()) {
   if(IsGoodEvent(a, b, c))
      DoStuff(a, b, c);
} 
   </pre>
</td>
<td>
  <pre lang="c++">
ROOT::TDataFrame d("myTree", file, {"A", "B", "C"});
d.Filter(IsGoodEvent).Foreach(DoStuff);
  </pre>
</td>
</tr>
<tr>
<td>
<b>TTree::Draw</b>
</td>
<td>
<b>TDataFrame</b>
</td>
</tr>
<tr>
<td>
<pre lang="c++">
TTree *t = static_cast&lt;TTree*&gt;(
   file->Get("myTree")
);
t->Draw("var", "var > 2");
</pre>
</td>
<td>
<pre lang="c++">
ROOT::TDataFrame d("myTree", file, "var");
d.Filter([](int v) { return v > 2; }).Histo();
</pre>
</td>
</tr>
</table>

Keep reading to follow a five-minute [crash course](#crash-course) to `TDataFrame`, or jump to an overview of useful [features](#more-features), or a more in-depth explanation of [transformations](#transformations), [actions](#actions) and [parallelism](#multi-thread-execution).

## Crash course
### Filling a histogram
Let's start with a very common task: filling a histogram
```c++
// Fill a TH1F with the "MET" branch
ROOT::TDataFrame d("myTree", filePtr); // build a TDataFrame like you would build a TTreeReader
auto h = d.Histo("MET");
h->Draw();
```
The first line creates a `TDataFrame` associated to the `TTree` "myTree". This tree has a branch named "MET".

`Histo` is an action; it returns a smart pointer (a `TActionResultPtr` to be precise) to a `TH1F` histogram filled with the `MET` of all events. The quantity stored in the branch must not necessarily be a single number per entry, it can also be a collection.

There are many other possible [actions](#overview), and all their results are wrapped in smart pointers; we'll see why in a minute.

### Applying a filter
Let's now pretend we want to cut over the value of branch "MET" and count how many events pass this cut:
```c++
// Select events with "MET" greater than 4., count events that passed the selection
auto metCut = [](double x) { return x > 4.; }; // a c++11 lambda function checking "x > 4"
ROOT::TDataFrame d("myTree", filePtr);
auto c = d.Filter(metCut, {"MET"}).Count();
std::cout << *c << std::endl;
```
`Filter` takes a function (a lambda in this example, but it can be any kind of function or even a functor class) and a list of branch names. The filter function is applied to the specified branches for each event; it is required to return a `bool` which signals whether the event passes the filter (`true`) or not (`false`). You can think of your data as "flowing" through the chain of calls, being transformed, filtered and finally used to perform actions. Multiple `Filter` calls can be chained one after another.

### Creating a temporary branch
Let's now consider the case in which "myTree" is a dataset which stores the three cartesian components of the momenta of some kind of particles. There is the need to study the transverse momentum of the particles, "pt". In order to be able to work on the "pt" variable, we can create a new column in the dataset with the `AddBranch` transformation:
```c++
auto ptCut = [](double x) { return x > 25.; };

auto sqrtSum = [](double x, double y) { return sqrt(x*x + y*y); };

auto producePtColl = [](std::vector<float> px_v, std::vector<float> py_v,) {
   auto len = px_v.size();
   std::vector<float> pts(len);
   for (int i : ROOT::TSeqI(len) {
      pts[i] = sqrtSum(px_v[i], py_v[i]);
   }
   return pts;
};

ROOT::TDataFrame d(treeName, filePtr);
auto ptMean = d.AddBranch("pt_v", producePtColl, {"px_v","py_v"})
               .Filter(ptCut, {"pt_v"})
               .Mean("pt_v");
std::cout << *ptMean << std::endl;
```
`AddBranch` creates the variable "pt_v" by applying `producePtColl` to "px_v" and "py_v". Later in the chain of calls we refer to variables created with `AddBranch` as if they were actual tree branches, but they are evaluated on the fly, once per event. As with filters, `AddBranch` calls can be chained with other transformations to create multiple temporary branches.

### Executing multiple actions
As a final example let us apply two different cuts on branch "MET" and fill two different histograms with the "pt_v" of the filtered events.
You should be able to easily understand what's happening:
```c++
// fill two histograms with the results of two opposite cuts
auto isBig = [](double x) { return x > 10.; };
ROOT::TDataFrame d(treeName, filePtr);
auto h1 = d.Filter(isBig, {"MET"}).Histo("pt_v");
auto h2 = d.Histo("pt_v");
h1->Draw();       // event loop is run once here
h2->Draw("SAME"); // no need to run the event loop again
```
`TDataFrame` executes all above actions by **running the event-loop only once**. The trick is that actions are not executed at the moment they are called, but they are **delayed** until the moment one of their results is accessed through the smart pointer. At that time, the even loop is triggered and *all* results are produced simultaneously.

It is therefore good practice to declare all your filters and actions *before* accessing their results, allowing `TDataFrame` to loop once and produce all results in one go.

### Going parallel
Let's say we would like to run the previous examples in parallel on several cores, dividing events fairly between cores. The only modification required to the snippets would be the addition of this line *before* constructing the main data-frame object:
```c++
ROOT::EnableImplicitMT();
```
Simple as that, enjoy your speed-up.

## More features
Here is a list of the most important features that have been omitted in the "Crash course" for brevity's sake.
You don't need to read all these to start using `TDataFrame`, but they are useful to save typing time and runtime.

### Default branch lists
When constructing a `TDataFrame` object, it is possible to specify a **default branch list** for your analysis, in the usual form of a list of strings representing branch names. The default branch list will be used as fallback whenever one specific to the transformation/action is not present.
```c++
// use "b1" and "b2" as default branches for `Filter`, `AddBranch` and actions
ROOT::TDataFrame d1(treeName, &file, {"b1","b2"});
// filter acts on default branch list, no need to specify it
auto h = d1.Filter([](int b1, int b2) { return b1 > b2; }).Histo("otherVar");

// just one default branch this time
ROOT::TDataFrame d2(treeName, &file, {"b1"});
// we can still specify non-default branch lists
// `Min` here can fall back to the default "b1"
auto min = d2.Filter([](double b2) { return b2 > 0; }, {"b2"}).Min();
```
<!-- Commented out until we have the std::array_view in ROOT
### Branches of collection types
We can rely on several features when dealing with branches of collection types (e.g. `vector<double>`, `int[3]`, or anything that you would put in a `TTreeReaderArray` rather than a `TTreeReaderValue`).

First of all, we **never need to spell out the exact type of the collection-type** branch in a transformation or an action. As it would be done when building a `TTreeReaderArray` for that branch, we just need to specify the type of the elements of the collection, in this way:
```c++
ROOT::TDataFrame d(treeName, &file, {"vecBranch"});
d.Filter(ROOT::ArrayView<double> vecBranch) { return vecBranch.size() > 0; }).Histo();
```

Moreover, actions detect whenever they are applied to a collection type and **adapt their behaviour to act on all elements of the collection**, for each entry. In the example above, `Histo()` (equivalent to `Histo("vecBranch")`) fills the histogram with the values of all elements of `vecBranch`, for each event.-->

### Branch type guessing and explicit declaration of branch types
C++ is a statically typed language: all types must be known at compile-time. This includes the types of the `TTree` branches we want to work on. For filters, temporary branches and some of the actions, **branch types are deduced from the signature** of the relevant filter function/temporary branch expression/action function:
```c++
// here b1 is deduced to be `int` and b2 to be `double`
dataFrame.Filter([](int x, double y) { return x > 0 && y < 0.; }, {"b1", "b2"});
```
If we specify an incorrect type for one of the branches, an exception with an informative message will be thrown at runtime, when the branch value is actually read from the `TTree`: the implementation of `TDataFrame` allows the detection of type mismatches. The same would happen if we swapped the order of "b1" and "b2" in the branch list passed to `Filter`.

Certain actions, on the other hand, do not take a function as argument (e.g. `Histo`), so we cannot deduce the type of the branch at compile-time. In this case **`TDataFrame` tries to guess the type of the branch**, trying out the most common ones and `std::vector` thereof. This is why we never needed to specify the branch types for all actions in the above snippets.

When the branch type is not a common one such as `int`, `double`, `char` or `float` it is therefore good practice to specify it as a template parameter to the action itself, like this:
```c++
dataFrame.Histo("b1"); // OK if b1 is a "common" type
dataFrame.Histo<Object_t>("myObject"); // OK, "myObject" is deduced to be of type `Object_t`
// dataFrame.Histo("myObject"); // THROWS an exception
```

### Generic actions
`TDataFrame` strives to offer a comprehensive set of standard actions that can be performed on each event. At the same time, it **allows users to execute arbitrary code (i.e. a generic action) inside the event loop** through the `Foreach` and `ForeachSlot` actions.

`Foreach(f, branchList)` takes a function `f` (lambda expression, free function, functor...) and a list of branches, and executes `f` on those branches for each event. The function passed must return nothing (i.e. `void`). It can be used to perform actions that are not already available in the interface. For example, the following snippet evaluates the root mean square of branch "b":
```c++
// Single-thread evaluation of RMS of branch "b" using Foreach
double sumSq = 0.;
unsigned int n = 0;
ROOT::TDataFrame d("bTree", bFilePtr);
d.Foreach([&sumSq, &n](double b) { ++n; sumSq += b*b; }, {"b"});
std::cout << "rms of b: " << std::sqrt(sumSq / n) << std::endl;
```
When executing on multiple threads, users are responsible for the thread-safety of the expression passed to `Foreach`.
The code above would need to employ some resource protection mechanism to ensure non-concurrent writing of `rms`; but this is probably too much head-scratch for such a simple operation.

`ForeachSlot` can help in this situation. It is an alternative version of `Foreach` for which the function takes an additional parameter besides the branches it should be applied to: an `unsigned int slot` parameter, where `slot` is a number indicating which thread (0, 1, 2 , ..., poolSize - 1) the function is being run in. We can take advantage of `ForeachSlot` to evaluate a thread-safe root mean square of branch "b":
```c++
// Thread-safe evaluation of RMS of branch "b" using ForeachSlot
ROOT::EnableImplicitMT();
unsigned int nSlots = ROOT::GetImplicitMTPoolSize();
std::vector<double> sumSqs(nSlots, 0.);
std::vector<unsigned int> ns(nSlots, 0);
ROOT::TDataFrame d("bTree", bFilePtr);
d.ForeachSlot([&sumSqs, &ns](unsigned int slot, double b) { sumSqs[slot] += b*b; ns[slot] += 1; }, {"b"});
double sumSq = std::accumulate(sumSqs.begin(), sumSqs.end(), 0.); // sum all squares
unsigned int n = std::accumulate(ns.begin(), ns.end(), 0); // sum all counts
std::cout << "rms of b: " << std::sqrt(sumSq / n) << std::endl;
```
You see how we created one `double` variable for each thread in the pool, and later merged their results via `std::accumulate`.

### Call graphs (storing and reusing sets of transformations)
**Sets of transformations can be stored as variables** and reused multiple times to create **call graphs** in which several paths of filtering/creation of branches are executed simultaneously; we often refer to this as "storing the state of the chain".

This feature can be used, for example, to create a temporary branch once and use it in several subsequent filters or actions, or to apply a strict filter to the data-set *before* executing several other transformations and actions, effectively reducing the amount of events processed.

Let's try to make this clearer with a commented example:
```c++
// build the data-frame and specify a default branch list
ROOT::TDataFrame d(treeName, filePtr, {"var1", "var2", "var3"});
// apply a cut and save the state of the chain
auto filtered = d.Filter(myBigCut);
// plot branch "var1" at this point of the chain
auto h1 = filtered.Histo("var1");
// create a new branch "vec" with a vector extracted from a complex object (only for filtered entries)
// and save the state of the chain
auto newBranchFiltered = filtered.AddBranch("vec", [](const Obj& o) { return o.getVector(); }, {"obj"});
// apply a cut and fill a histogram with "vec"
auto h2 = newBranchFiltered.Filter(cut1).Histo("vec");
// apply a different cut and fill a new histogram
auto h3 = newBranchFiltered.Filter(cut2).Histo("vec");
// Inspect results
std::cout << "Entries in h1: " << h1->GetEntries() << std::endl;
h2->Draw(); // first access to an action result: RUN ONCE AND FILL EVERYTHING
h3->Draw("SAME"); // event loop does not need to be run again now
```
`TDataFrame` detects when several actions use the same filter or the same temporary branch, and **only evaluates each filter or temporary branch once per event**, regardless of how many times that result is used down the call graph. Objects read from each branch are **built once and never copied**, for maximum efficiency.
When "upstream" filters are not passed, subsequent filters, temporary branch expressions and actions are not evaluated, so it might be advisable to put the strictest filters first in the chain.

## Transformations
### Filters
A filter is defined through a call to `Filter(f, branchList)`. `f` can be a function, a lambda expression, a functor class, or any other callable object. It must return a `bool` signalling whether the event has passed the selection (`true`) or not (`false`). `f` is re-entrant. It must perform "read-only" actions on the branches, and should not have side-effects to ensure correct results when implicit multi-threading is active.

`TDataFrame` only evaluates filters when necessary: if multiple filters are chained one after another, they are executed in order and the first one returning `false` causes the event to be discarded and triggers the processing of the next entry. If multiple actions or transformations depend on the same filter, that filter is not executed multiple times for each entry: after the first access it simply serves a cached result.

<!--#### Named filters To be uncommented when the support is added
An optional string parameter `filterName` can be specified to `Filter`, defining a **named filter**. Named filters work as usual, but also keep track of how many entries they accept and reject. Statistics are retrieved through a call to the `Report` method (coming soon).-->

### Temporary branches
Temporary branches are created by invoking `AddBranch(name, f, branchList)`. As usual, `f` can be any callable object (function, lambda expression, functor class...); it takes the values of the branches listed in `branchList` (a list of strings) as parameters, in the same order as they are listed in `branchList`. `f` must return the value that will be assigned to the temporary branch.

A new variable is created called `name`, accessible as if it was contained in the dataset from subsequent transformations/actions.

Use cases include:
-   caching the results of complex calculations for easy and efficient multiple access
-   extraction of quantities of interest from complex objects
-   branch aliasing, i.e. changing the name of a branch

<!-- To be uncommented when the support is added
Temporary branch values can be persistified by saving them to a new `TTree` using the `Snapshot` action.
An exception is thrown if the `name` of the new branch is already in use for another branch in the `TTree`.-->

## Actions
### Instant and delayed actions
Actions can be **instant** or **delayed**. Instant actions are executed as soon as they are called, while delayed actions are executed whenever the object they return is accessed for the first time. As a rule of thumb, all actions with a return value are delayed, the others are instant.
<!--One notable exception is `Snapshot` (see the table [below](#overview)).

Whenever an action is executed, all (delayed) actions with the same **range** (see later) are executed within the same event loop.

### Ranges (coming soon)
Ranges of entries can (or must) be specified for all actions. **Only the specified range of entries will be processed** during the event loop.
The default range is, of course, beginning to end.-->

### Overview
Here is a quick overview of what actions are present and what they do. Each one is described in more detail in the reference guide.

In the following, whenever we say an action "returns" something, we always mean it returns a smart pointer to it. Also note that all actions are only executed for events that pass all preceding filters.

 Delayed actions | Description | Notes
:--------------: | :---------: | :-----:
<!-- Accumulate | Execute a function with signature `R(R,T)` on each entry. T is a branch, R is an accumulator. Return the final value of the accumulator | coming soon -->
Count | Return the number of events processed |
Get | Build a collection of values of a branch |
Histo | Fill a histogram with the values of a branch that passed all filters |
Max | Return the maximum of processed branch values  |
Mean | Return the mean of processed branch values |
Min | Return the minimum of processed branch values |
<!-- Reduce | Execute a function with signature `T(T,T)` on each entry. Processed branch values are reduced (e.g. summed, merged) using this function. Return the final result of the reduction operation | coming soon -->
Sum | Return the sum of processed branch values | coming soon
**Instant actions** | **Description** | **Notes**
Foreach | Execute a user-defined function on each entry. Users are responsible for the thread-safety of this lambda when executing with implicit multi-threading enabled |
ForeachSlot | Same as `Foreach`, but the user-defined function must take an extra `unsigned int slot` as its first parameter. `slot` will take a different value, `0` to `nThreads - 1`, for each thread of execution. This is meant as a helper in writing thread-safe `Foreach` actions when using `TDataFrame` after `ROOT::EnableImplicitMT()`. `ForeachSlot` works just as well with single-thread execution: in that case `slot` will always be `0`. |
<!-- Head | Take a number `n`, run and pretty-print the first `n` events that passed all filters | coming soon -->
<!-- Snapshot | Save a set of branches and temporary branches to disk, return a new `TDataFrame` that works on the skimmed, augmented or otherwise processed data | coming soon -->
<!-- Tail  | Take a number `n`, run and pretty-print the last `n` events that passed all filters | coming soon -->

## Parallel execution
As pointed out before in this document, `TDataFrame` can transparently perform multi-threaded event loops to speed up the execution of its actions. Users only have to call `ROOT::EnableImplicitMT()` *before* constructing the `TDataFrame` object to indicate that it should take advantage of a pool of worker threads. **Each worker thread processes a distinct subset of entries**, and their partial results are merged before returning the final values to the user.

### Thread safety
`Filter` and `AddBranch` transformations should be inherently thread-safe: they have no side-effects and are not dependent on global state.
Most `Filter`/`AddBranch` functions will in fact be pure in the functional programming sense.
All actions are built to be thread-safe with the exception of `Foreach`, in which case users are responsible of thread-safety, see [here](#generic-actions).

<!--## Example snippets
Here you can find pre-made solutions to common problems. They should work out-of-the-box provided you have our "TDFTestTree.root" in the same directory where you execute the snippet.<br>
Please contact us if you think we are missing important, common use-cases.

```c++
// evaluation of several cuts
```-->
