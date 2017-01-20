BENCHS:=benchmark

all: $(BENCHS)

%: %.cxx ../TDataFrame.hxx; \
   g++ -std=c++11 -g -O2 -o $@ $< `root-config --libs --cflags` -lTreePlayer -I ../

.PHONY: clean
clean: ;\
   rm -rf $(BENCHS) # *.root
