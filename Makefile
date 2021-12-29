ifdef NO_USBMUXD_STUB
	PROGRAM := AltServer
else
	ifdef NO_UPNP_STUB
		PROGRAM := AltServerNet
	else
		PROGRAM := AltServerUPnP
	endif
endif


%.c.o : %.c
	$(CC) $(CFLAGS) $(EXTRA_FLAGS) -o $@ -c $<

%.cpp.o : %.cpp
	$(CXX) $(CXXFLAGS) $(EXTRA_FLAGS) -o $@ -c $<

CFLAGS := -DHAVE_CONFIG_H -DDEBUG -O0 -g
CXXFLAGS = $(CFLAGS) -std=c++17

main_src := $(wildcard src/*.c) $(wildcard src/*.cpp)

ifdef NO_USBMUXD_STUB
	CFLAGS += -DNO_USBMUXD_STUB
else
	main_src += src/phone/libusbmuxd-stub.c
	ifdef NO_UPNP_STUB
		CFLAGS += -DNO_UPNP_STUB
	else
		main_src += src/phone/idevice-stub.c
	endif
endif

miniupnpc_src = minissdpc.c miniwget.c minixml.c igd_desc_parse.c minisoap.c \
		  miniupnpc.c upnpreplyparse.c upnpcommands.c upnperrors.c \
		  connecthostport.c portlistingparse.c receivedata.c upnpdev.c \
		  addr_is_reserved.c
miniupnpc_src := $(miniupnpc_src:%.c=libraries/miniupnpc/%.c)
miniupnpc_include := -Ilibraries/miniupnpc

INC_CFLAGS := -Ilibraries
INC_CFLAGS += $(miniupnpc_include)
INC_CFLAGS += -Ilibraries/AltSign

allsrc := $(main_src) 
allsrc += $(miniupnpc_src)

allobj = $(addsuffix .o, $(allsrc))

# $(miniupnpc_src:.c=.o) : $(miniupnpc_src)
# 	$(CC) $(CFLAGS) -Ilibraries -c $<
$(addsuffix .o, $(miniupnpc_src)) : EXTRA_FLAGS := -Ilibraries $(miniupnpc_include)
libraries/miniupnp.a : $(addsuffix .o, $(miniupnpc_src))
	ar rcs $@ $^

$(addsuffix .o, $(main_src)) : EXTRA_FLAGS := -Ilibraries $(INC_CFLAGS)

#%.o : %.c
#	$(CC) $(CFLAGS) $(INC_CFLAGS) -c $< -o $@

lib_AltSign:
	$(MAKE) -C libraries/AltSign

LDFLAGS = libraries/AltSign/AltSign.a -lssl -lcrypto -lpthread -lcorecrypto_static -lzip -lm -lz -lcpprest -lboost_system -lboost_filesystem -lstdc++ -lssl -lcrypto -luuid -ldl -lplist -lusbmuxd -limobiledevice
$(PROGRAM):: lib_AltSign

$(PROGRAM):: $(addsuffix .o, $(main_src)) libraries/miniupnp.a
	$(CC) -o $@ $^ $(LDFLAGS)

.PHONY: clean all lib_AltSign
clean:
	rm -f $(allobj) libraries/*.a $(PROGRAM)

all: $(PROGRAM)
.DEFAULT_GOAL := all
