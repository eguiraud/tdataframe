TESTS:=tdf001_introduction tdf002_dataModel test2 regression_multipletriggerrun

all: $(TESTS)

%: %.cxx ../TDataFrame.hxx; \
   g++ -std=c++11 -g -o $@ $< `root-config --libs --cflags` -lTreePlayer -I ../

.PHONY: clean
clean: ;\
   rm -rf $(TESTS) *.root
