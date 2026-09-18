# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_ASYNC_CORE) += async_tx.o
obj-$(CONFIG_ASYNC_MEMCPY) += async_memcpy.o
obj-$(CONFIG_ASYNC_XOR) += async_xor.o
obj-$(CONFIG_ASYNC_PQ) += async_pq.o
obj-$(CONFIG_ASYNC_RAID6_RECOV) += async_raid6_recov.o
obj-$(CONFIG_ASYNC_RAID6_TEST) += raid6test.o

# WD My Cloud EX2 Ultra: raid456.ko must stay a module, but these async_tx
# helpers have no Kconfig prompt of their own and would otherwise only load
# as separate .ko files, none of which are in the board's fixed insmod list.
ifeq ($(CONFIG_MD_RAID456),m)
obj-y += async_tx.o async_memcpy.o async_xor.o async_pq.o async_raid6_recov.o
endif
