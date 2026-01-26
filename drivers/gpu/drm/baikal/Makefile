# SPDX-License-Identifier: GPL-2.0
baikal_vdu_drm-y +=	baikal_vdu_backlight.o \
		baikal_vdu_crtc.o \
		baikal_vdu_drv.o \
		baikal_vdu_panel.o \
		baikal_vdu_plane.o

baikal_bl1000_vdu-y += baikal_bl1000_vdu_drv.o \
                baikal_bl1000_crtc.o \
                baikal_bl1000_dp.o \
                baikal_bl1000_plane.o \
                baikal_bl1000_vdu.o

baikal_vdu_drm-$(CONFIG_DEBUG_FS) += baikal_vdu_debugfs.o

obj-$(CONFIG_DRM_BAIKAL_BL1000) += baikal_bl1000_vdu.o
obj-$(CONFIG_DRM_BAIKAL_HDMI) += baikal-hdmi.o
obj-$(CONFIG_DRM_BAIKAL_VDU) += baikal_vdu_drm.o
