SHELL		:= /bin/bash
CC			= gcc
CFLAGS		= -O3 -Wall
MERCURY_DIR	= $(WORKSPACE)/iof/install
PMIX_DIR	= $(WORKSPACE)/iof/install
INC			= -I$(MERCURY_DIR)/include  -I$(PMIX_DIR)/include/
LIBDIR		= -L$(MERCURY_DIR)/lib -L$(PMIX_DIR)/lib/
LIBS		= -lmercury -lna -lbmi -lmercury_util -lpthread -lmchecksum -lrt -lpmix
EXE			=test_rpc_server test_rpc_client test_pmix_client

all: $(EXE)

.SUFFIXES: .o .c
%.o: %.c my_rpc_common.h
	$(CC) $(CFLAGS) $(INC) -o $@ -c $< $(LIBDIR) $(LIBS)

.PRECIOUS: %.o
test_%: test_%.o my_rpc_common.o
	$(CC) $(CFLAGS) $(INC)  -o $@ $^ $(LIBDIR) $(LIBS)

.PHONY: clean

clean:
	rm -rf *.o $(EXE)
