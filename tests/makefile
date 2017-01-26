TESTS:=tdf001_introduction tdf002_dataModel test_misc regression_multipletriggerrun \
test_par testIMT test_functiontraits regression_zeroentries test_branchoverwrite \
test_foreach regression_invalidref

all: $(TESTS)

%: %.cxx ../TDataFrame.hxx; \
   g++ -Wall -std=c++11 -g -o $@ $< `root-config --libs --cflags` -lTreePlayer -I ../

.PHONY: clean
clean: ;\
   rm -rf $(TESTS) *.root
