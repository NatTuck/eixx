ERL_ROOT=/opt/erlang/R14A
BOOST_ROOT=/opt/boost
ERL_INTERFACE=$(ERL_ROOT)/lib/erlang/lib/erl_interface-3.7
CPPFLAGS = $(if $(tr1),-std=c++0x) -I./include -isystem $(BOOST_ROOT)/include \
			-isystem $(ERL_INTERFACE)/include -isystem $(ERL_INTERFACE)/src
LDFLAGS  = -L$(ERL_INTERFACE)/lib -L$(BOOST_ROOT)/lib -L./lib -lboost_system -lei

TARGETS  = test_perf test_eterm src/test_node lib/libeixx.so

all: $(TARGETS)

test_eterm_SOURCES = test/test_eterm.cpp test/test_eterm_encode.cpp \
					 test/test_eterm_format.cpp test/test_eterm_match.cpp \
					 test/test_eterm_pool.cpp test/test_eterm_refc.cpp \
					 test/test_mailbox.cpp

lib/libeixx.so: src/atom.cpp src/basic_otp_node_local.cpp
	g++ -g -O$(if $(optimize),3 -DBOOST_DISABLE_ASSERTS,0) \
		-o $@ $^ $(CPPFLAGS) -shared -fPIC $(LDFLAGS)

test_eterm: $(test_eterm_SOURCES) src/atom.cpp \
		    $(wildcard include/eixx/*.?pp) $(wildcard include/eixx/connect/*.?pp) \
			$(wildcard include/eixx/marshal/*.?pp)
	g++ -g -O$(if $(optimize),3 -DBOOST_DISABLE_ASSERTS,0) \
    	-o $@ $(test_eterm_SOURCES) $(CPPFLAGS) $(LDFLAGS) \
	-DBOOST_TEST_DYN_LINK -lboost_unit_test_framework -leixx -L.

test_perf: test/test_perf.cpp $(wildcard include/eixx/*.?pp) $(wildcard include/eixx/impl/*.?pp)
	g++ -g -O$(if $(optimize),3 -DBOOST_DISABLE_ASSERTS,0) -o $@ $< src/atom.cpp $(CPPFLAGS) $(LDFLAGS)

src/test_node:
	@$(MAKE) --directory=src

clean:
	rm -f $(TARGETS)

tar:
	DIR=$$PWD && cd .. && tar jcf eixx.tbz eixx/include eixx/Makefile eixx/src/ && \
	mv eixx.tbz $$DIR
