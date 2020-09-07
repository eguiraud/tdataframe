# Functional chains for ROOT data analyses
[![DOI](https://zenodo.org/badge/76344887.svg)](https://zenodo.org/badge/latestdoi/76344887)
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
d.Filter(isNeg, {"theta"}).Foreach(fill, {"pt_x", "pt_y"});
```
No need for TSelectors or TTreeReader loops anymore. As a huge plus, parallelisation, caching and other optimisations are performed transparently or requiring only minimal action on the user's part.

**NB** This project is now part of [ROOT](https://github.com/root-mirror/root) and an up-to-date user guide can be found [here](https://root.cern/manual/data_frame/).
