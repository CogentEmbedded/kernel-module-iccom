ccflags-y+=-Werror
ccflags-y+=-I$(M)/public

DEBUG  ?= N
ifeq ($(DEBUG),Y)
	ccflags-y+=-DDEBUG
endif
obj-m += iccom.o

iccom-objs :=   \
		iccom_core.o \
		iccom_driver.o \
		iccom_kernel_api.o
clean :
		rm -f *.o
		rm -f *.ko
