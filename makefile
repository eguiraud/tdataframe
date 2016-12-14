TARGETS:=tdf001_introduction

all: $(TARGETS)

tdf001_introduction: tdf001_introduction.C TDataFrame.hpp; \
   g++ -std=c++11 -o $@ $< `root-config --libs --cflags` -lTreePlayer

.PHONY: clean
clean: ;\
   rm -rf $(TARGETS)
