# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2010-2014 Intel Corporation

ifeq ($(RTE_SDK),)
$(error "Please define RTE_SDK environment variable")
endif

# Default target, can be overridden by command line or environment
RTE_TARGET ?= x86_64-native-linuxapp-gcc

include $(RTE_SDK)/mk/rte.vars.mk

# library name
LIB = libdpdk_transport.a

# all source are stored in SRCS-y
SRCS-y := $(wildcard $(SRCDIR)/src/*.c)

INCLUDES := -I$(SRCDIR)/include

SYMLINK-y-include += include/dpdk_transport.h 

CFLAGS += -O3
CFLAGS += $(WERROR_FLAGS)
CFLAGS += $(INCLUDES)

include $(RTE_SDK)/mk/rte.extlib.mk
