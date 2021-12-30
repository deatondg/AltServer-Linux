ifdef NO_USBMUXD_STUB
	PROGRAM := AltServer
else
	ifdef NO_UPNP_STUB
		PROGRAM := AltServerNet
	else
		PROGRAM := AltServerUPnP
	endif
endif

CFLAGS := -DHAVE_CONFIG_H -DDEBUG -O0 -g
CXXFLAGS = $(CFLAGS) -std=c++17
INC_CFLAGS := -Ilibraries -Ilibraries/AltSign
LDFLAGS = libraries/AltSign/AltSign.a -lssl -lcrypto -lpthread -lcorecrypto_static -lzip -lm -lz -lcpprest -lboost_system -lboost_filesystem -lstdc++ -lssl -lcrypto -luuid -ldl -lplist -lusbmuxd -limobiledevice -lminiupnpc

%.c.o : %.c
	$(CC) $(CFLAGS) $(INC_CFLAGS) -o $@ -c $<

%.cpp.o : %.cpp
	$(CXX) $(CXXFLAGS) $(INC_CFLAGS) -o $@ -c $<


main_src := $(wildcard src/*.c) $(wildcard src/*.cpp)

ifdef NO_USBMUXD_STUB
	CFLAGS += -DNO_USBMUXD_STUB
else
	ifdef NO_UPNP_STUB
		CFLAGS += -DNO_UPNP_STUB
	endif
endif


allsrc := $(main_src) 

allobj = $(addsuffix .o, $(allsrc))

lib_AltSign:
	$(MAKE) -C libraries/AltSign

$(PROGRAM):: lib_AltSign

$(PROGRAM):: $(addsuffix .o, $(main_src))
	$(CC) -o $@ $^ $(LDFLAGS)

.PHONY: clean all lib_AltSign
clean:
	rm -f $(allobj) libraries/*.a $(PROGRAM)
	$(MAKE) -C libraries/AltSign clean

all: $(PROGRAM)
.DEFAULT_GOAL := all
