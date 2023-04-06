# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2010-2014 Intel Corporation

ifeq ($(RTE_SDK),)
$(error "Please define RTE_SDK environment variable")
endif

# Default target, can be overridden by command line or environment
RTE_TARGET ?= x86_64-native-linuxapp-gcc

include $(RTE_SDK)/mk/rte.vars.mk

# binary name
APP = initiator

DPDK_TRANSPORTDIR = $(SRCDIR)/../../build

# all source are stored in SRCS-y
SRCS-y := $(wildcard $(SRCDIR)/src/*.c)

INCLUDES := -I$(DPDK_TRANSPORTDIR)/include

CFLAGS += -O3
CFLAGS += $(WERROR_FLAGS)
CFLAGS += $(INCLUDES)

EXTRA_LDLIBS := -L$(DPDK_TRANSPORTDIR)/lib -ldpdk_transport

include $(RTE_SDK)/mk/rte.extapp.mk
