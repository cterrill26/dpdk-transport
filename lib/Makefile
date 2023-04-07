ifeq ($(RTE_SDK),)
$(error "Please define RTE_SDK environment variable")
endif

# Default target, detect a build directory, by looking for a path with a .config
RTE_TARGET ?= $(notdir $(abspath $(dir $(firstword $(wildcard $(RTE_SDK)/*/.config)))))

include $(RTE_SDK)/mk/rte.vars.mk

ifneq ($(CONFIG_RTE_EXEC_ENV_LINUX),y)
$(error This application can only operate in a linux environment, \
please change the definition of the RTE_TARGET environment variable)
endif

# library name
LIB = libdpdk_transport.a

LIBABIVER := 1

# all source are stored in SRC-Y
SRCS-y := $(wildcard $(SRCDIR)/src/*.c)

CFLAGS += -O3
CFLAGS += $(WERROR_FLAGS)
CFLAGS += -I$(SRCDIR)/include

SYMLINK-y-include += $(SRCDIR)/include/dpdk_transport.h 

include $(RTE_SDK)/mk/rte.extlib.mk
