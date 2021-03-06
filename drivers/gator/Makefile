ifneq ($(KERNELRELEASE),)

# Uncomment the following line to enable kernel stack unwinding within gator, or update gator_backtrace.c
# EXTRA_CFLAGS +=   -DGATOR_KERNEL_STACK_UNWINDING

CONFIG_GATOR ?= m
obj-$(CONFIG_GATOR) := gator.o

gator-y :=  gator_main.o \
            gator_events_block.o \
            gator_events_irq.o \
            gator_events_meminfo.o \
            gator_events_mmapped.o \
            gator_events_net.o \
            gator_events_sched.o \

# Convert the old GATOR_WITH_MALI_SUPPORT to the new kernel flags
ifneq ($(GATOR_WITH_MALI_SUPPORT),)
  CONFIG_GATOR_WITH_MALI_SUPPORT := y
  ifeq ($(GATOR_WITH_MALI_SUPPORT),MALI_MIDGARD)
    CONFIG_GATOR_MALI_4XXMP := n
    CONFIG_GATOR_MALI_MIDGARD := y
  else
    CONFIG_GATOR_MALI_4XXMP := y
    CONFIG_GATOR_MALI_MIDGARD := n
  endif
  EXTRA_CFLAGS += -DMALI_SUPPORT=$(GATOR_WITH_MALI_SUPPORT)
  ifneq ($(GATOR_MALI_INTERFACE_STYLE),)
    EXTRA_CFLAGS += -DGATOR_MALI_INTERFACE_STYLE=$(GATOR_MALI_INTERFACE_STYLE)
  endif
  ifneq ($(GATOR_MALI_4XXMP_PATH),)
    CONFIG_GATOR_MALI_4XXMP_PATH = $(GATOR_MALI_4XXMP_PATH)
  endif
  CONFIG_GATOR_MALI_4XXMP_PATH ?= drivers/gpu/arm/mali400
  ifneq ($(GATOR_MALI_MIDGARD_PATH),)
    CONFIG_GATOR_MALI_MIDGARD_PATH = $(GATOR_MALI_MIDGARD_PATH)
  endif
  CONFIG_GATOR_MALI_MIDGARD_PATH ?= drivers/gpu/arm/midgard
endif

ifeq ($(CONFIG_GATOR_WITH_MALI_SUPPORT),y)
  ifeq ($(CONFIG_GATOR_MALI_MIDGARD),y)
    gator-y +=  gator_events_mali_midgard.o \
		gator_events_mali_midgard_hw.o
    include $(src)/mali_midgard.mk
  else
    gator-y +=  gator_events_mali_4xx.o
  endif
  gator-y +=    gator_events_mali_common.o

  ifneq ($(CONFIG_GATOR_MALI_4XXMP_PATH),)
    ccflags-$(CONFIG_GATOR_MALI_4XXMP) += -I$(CONFIG_GATOR_MALI_4XXMP_PATH)
  endif
  ifneq ($(CONFIG_GATOR_MALI_MIDGARD_PATH),)
    ccflags-$(CONFIG_GATOR_MALI_MIDGARD) += -I$(CONFIG_GATOR_MALI_MIDGARD_PATH)
  endif
  ccflags-$(CONFIG_GATOR_MALI_4XXMP) += -DMALI_SUPPORT=MALI_4xx
  ccflags-$(CONFIG_GATOR_MALI_MIDGARD) += -DMALI_SUPPORT=MALI_MIDGARD
endif

# GATOR_TEST controls whether to include (=1) or exclude (=0) test code.
GATOR_TEST ?= 0
EXTRA_CFLAGS += -DGATOR_TEST=$(GATOR_TEST)

# Should the original or new block_rq_complete API be used?
OLD_BLOCK_RQ_COMPLETE := $(shell grep -A3 block_rq_complete $(srctree)/include/trace/events/block.h | grep nr_bytes -q; echo $$?)
EXTRA_CFLAGS += -DOLD_BLOCK_RQ_COMPLETE=$(OLD_BLOCK_RQ_COMPLETE)

gator-$(CONFIG_ARM) +=  gator_events_l2c-310.o

gator-$(CONFIG_ARM64) +=

else

all:
	@echo
	@echo "usage:"
	@echo "      make -C <kernel_build_dir> M=\`pwd\` ARCH=arm CROSS_COMPILE=<...> modules"
	@echo
	$(error)

clean:
	rm -f *.o .*.cmd modules.order Module.symvers gator.ko gator.mod.c
	rm -rf .tmp_versions

endif
