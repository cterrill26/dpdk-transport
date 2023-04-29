ifeq ($(RTE_SDK),)
$(error "Please define RTE_SDK environment variable")
endif

# Default target, detect a build directory, by looking for a path with a .config
RTE_TARGET ?= $(notdir $(abspath $(dir $(firstword $(wildcard $(RTE_SDK)/*/.config)))))

include $(RTE_SDK)/mk/rte.vars.mk

# binary name
APP = latency-vs-throughput-socket

# all source are stored in SRCS-y
SRCS-y := main.cpp

# for some reason the included makefiles only work
# if cpp srcs are named *.c instead of *.cpp
SRCS-y := $(patsubst %.cpp,%.c,$(SRCS-y))

CPPFLAGS += -O3
CPPFLAGS += -Wall -Wextra

LDLIBS += -lstdc++

include $(RTE_SDK)/mk/rte.extapp.mk
