CXXFLAGS = $(shell pkg-config --cflags glib-2.0)

.PHONY: all clean

all: x10mms

x10mms: x10mms.o
	g++ -lxmms -o $@ $^

x10mms.o: x10mms.cpp

clean:
	rm -f *.o *~ x10mms 
