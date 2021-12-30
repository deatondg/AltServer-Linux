CFLAGS := -DHAVE_CONFIG_H -DDEBUG -O0 -g
CXXFLAGS = $(CFLAGS) -std=c++17
INC_CFLAGS := -Ilibraries -Ilibraries/AltSign
LDFLAGS = libraries/AltSign/AltSign.a -lssl -lcrypto -lpthread -lcorecrypto_static -lzip -lm -lz -lcpprest -lboost_system -lboost_filesystem -lstdc++ -lssl -lcrypto -luuid -ldl -lplist -lusbmuxd -limobiledevice -lminiupnpc

c_srcs := $(wildcard src/*.c)
c_objs := $(addsuffix .o, $(c_srcs))
cpp_srcs := $(filter-out src/AltServerMain.cpp, $(wildcard src/*.cpp))
cpp_objs := $(addsuffix .o, $(cpp_srcs))
most_srcs := $(c_srcs) $(cpp_srcs)
most_objs := $(c_objs) $(cpp_objs)

$(c_objs) : $(@:.o=)
	$(CC) $(CFLAGS) $(INC_CFLAGS) -o $@ -c $(@:.o=)
$(cpp_objs) : $(@:.o=)
	$(CXX) $(CXXFLAGS) $(INC_CFLAGS) -o $@ -c $(@:.o=)
src/AltServerMain.cpp.o: src/AltServerMain.cpp
	$(CXX) $(CXXFLAGS) -DNO_USBMUXD_STUB $(INC_CFLAGS) -o $@ -c $^
src/AltServerUPnPMain.cpp.o: src/AltServerMain.cpp
	$(CXX) $(CXXFLAGS) -DNO_UPNP_STUB $(INC_CFLAGS) -o $@ -c $^
src/AltServerNetMain.cpp.o: src/AltServerMain.cpp
	$(CXX) $(CXXFLAGS) $(INC_CFLAGS) -o $@ -c $^

lib_AltSign:
	$(MAKE) -C libraries/AltSign

AltServer :: lib_AltSign
AltServerUPnP :: lib_AltSign
AltServerNet :: lib_AltSign

AltServer :: $(most_objs) src/AltServerMain.cpp.o
	$(CC) -o $@ $^ $(LDFLAGS)
AltServerUPnP :: $(most_objs) src/AltServerUPnPMain.cpp.o
	$(CC) -o $@ $^ $(LDFLAGS)
AltServerNet :: $(most_objs) src/AltServerNetMain.cpp.o
	$(CC) -o $@ $^ $(LDFLAGS)

.PHONY: clean all lib_AltSign
clean:
	rm -f $(most_objs) src/AltServerMain.cpp.o src/AltServerUPnPMain.cpp.o src/AltServerNetMain.cpp.o libraries/*.a AltServer AltServerUPnP AltServerNet
	$(MAKE) -C libraries/AltSign clean

all: AltServer AltServerUPnP AltServerNet
.DEFAULT_GOAL := all
