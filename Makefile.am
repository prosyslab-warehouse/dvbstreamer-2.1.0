## DVBStreamer main Makefile

SUBDIRS = include include/dvbpsi src/dvbpsi src src/plugins


#AM: this is the file system standard
dvbstreamerdocdir = ${prefix}/share/doc/dvbstreamer

dvbstreamerdoc_DATA = \
	README\
	COPYING\
	AUTHORS\
	ChangeLog\
	INSTALL\
	NEWS\
	TODO

dvbstreamer_pkgincludedir = $(pkgincludedir)
dvbstreamer_pkginclude_DATA = \
    config.h

EXTRA_DIST = $(dvbstreamerdoc_DATA) $(dvbstreamer_pkginclude_DATA)

# Copy all the spec files. Of cource, only one is actually used.
dist-hook:
	for specfile in *.spec; do \
		if test -f $$specfile; then \
			cp -p $$specfile $(distdir); \
		fi \
	done

