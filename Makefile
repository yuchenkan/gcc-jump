INSTALLDIR=/home/chenkan/work/gcc-local
PLUGINDIR=$(shell $(INSTALLDIR)/bin/g++ -print-file-name=plugin)

all:
	g++ plugin.cpp gcj.cpp -I $(PLUGINDIR)/include -fPIC -g -shared -o plugin.so -Wall
