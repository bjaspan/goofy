SRCS	= goofy.cc url.cc
OBJS	= goofy.o url.o
CXXFLAGS	= -g

goofy: $(OBJS)
	$(CXX) -o goofy $(OBJS)

clean:
	rm -f goofy $(OBJS)
