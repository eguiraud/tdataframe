# Functional chains investigation
This repository contains ideas and snippets useful for the development of
functional chains useful for ROOT analyses. A short description of each file
follows.

## Data.h, functional\_chain.cpp
Working example of how to store lambdas inside objects. The main idea is that
each `filter` call creates a new (templated) Data object that contains the
new lambda and encapsulates the previous Data object.

The call to `reduce` triggers the execution of all filters (by recursive call
to the filter of each encapsulated Data object), applies the reduce lambda
and returns the final result.

## test\_tree.root
Simple TTree to be used for testing. It has two branches ("b1/D" and "b2/I")
with four entries each.

## extract\_arg\_type.cpp
Snippet that shows how to extract the types of the arguments of an std::function
(at compile time, obviously).

## TData.h, root\_chain.cpp
Exploration of a possible implementation of functional chains for a ROOT
analysis. The goal, for now, is having something like this compile and execute:
```
TData d(mytree);
auto good_entries = d.filter(f1).filter(f2).collect_entries();
```
where `f1` and `f2` are callables that take branches as arguments and return
a boolean, e.g.
```
auto f1 = [](double b1, int b2) { return b1 > 0. && b2 < 4 };
```

My idea was letting the compiler associate branch name and lambda argument in
this way:
```
d.filter({"b1, "b2"}, f1);
```
where `f1` is defined as above, so the compiler can associate the first argument
of the lambda `f1` with the branch name "b1".
The blocking issue I encountered following this path is that it is very tricky
(not even sure whether it's possible) to

1. allocate exactly one variable of the correct type for each branch
2. recover that variable at runtime, when `TTree::GetEntries` is called

So maybe we need to take a different direction.
