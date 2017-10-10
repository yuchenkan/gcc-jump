INSTALLDIR ?= /home/chenkan/work/gcc-local
PLUGINDIR = $(shell $(INSTALLDIR)/bin/g++ -print-file-name=plugin)

all:
	g++ plugin.cpp gcj.cpp -I $(PLUGINDIR)/include -fPIC -g -shared -o gcj.so -Wall
	g++ main.cpp gcj.cpp elf.cpp -g -o gcj -Wall
	#g++ elf.cpp -DTEST -g -o test -Wall
