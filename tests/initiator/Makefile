ifeq ($(RTE_SDK),)
$(error "Please define RTE_SDK environment variable")
endif

# Default target, detect a build directory, by looking for a path with a .config
RTE_TARGET ?= $(notdir $(abspath $(dir $(firstword $(wildcard $(RTE_SDK)/*/.config)))))

include $(RTE_SDK)/mk/rte.vars.mk

# binary name
APP = initiator

# all source are stored in SRCS-y
SRCS-y := main.c

CFLAGS += -O3
CFLAGS += $(WERROR_FLAGS)
CFLAGS += -I$(subst tests/$(APP),lib,$(RTE_OUTPUT))/include

LDLIBS += -L$(subst tests/$(APP),lib,$(RTE_OUTPUT))/lib
LDLIBS += -ldpdk_transport


include $(RTE_SDK)/mk/rte.extapp.mk
