include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_ha

mod_LTLIBRARIES = mod_ha.la
mod_ha_la_SOURCES  = mod_ha.c
mod_ha_la_CFLAGS   = $(AM_CFLAGS)
mod_ha_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_ha_la_LDFLAGS  = -avoid-version -module -no-undefined -shared
