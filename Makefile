INSTALLDIR=/home/chenkan/work/gcc-local
PLUGINDIR=$(shell $(INSTALLDIR)/bin/g++ -print-file-name=plugin)

all:
	g++ plugin.cpp gcj.cpp -I $(PLUGINDIR)/include -fPIC -g -shared -o gcj.so -Wall
	g++ bin.cpp gcj.cpp -g -o gcj -Wall
