# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_SND_BAIKAL_I2S) += baikal_i2s.o
obj-$(CONFIG_SND_BAIKAL_I2S) += baikal_snd.o

baikal_i2s-y := baikal-i2s.o

ifneq ($(CONFIG_SND_BAIKAL_PIO_PCM),)
baikal_i2s-y += baikal-pio-pcm.o
endif

baikal_snd-y := baikal-snd.o

ifneq ($(CONFIG_ACPI),)
obj-$(CONFIG_SND_BAIKAL_I2S) += baikal_snd_acpi.o
baikal_snd_acpi-y := baikal-snd-simple-acpi.o
endif
