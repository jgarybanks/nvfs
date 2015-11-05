MAKE	= make -j3
SHELL	= /bin/sh
SRCDIR	= $$PROJDIR/src/arch/linux/newdriver/nvr
GVPATH	= $$PROJDIR/src/include
DESTDIR =         $$INSTALLDIR/$$PROJECT/module/build/nvr

all:
	/bin/true

nvr: $(DESTDIR)
	cp $(SRCDIR)/*.c $(SRCDIR)/*.h $(DESTDIR)
	cp $(SRCDIR)/Makefile.linux $(DESTDIR)/Makefile
	cp $$PROJDIR/src/include/cdi_uaccess.h $(DESTDIR)
	cp $$PROJDIR/src/include/cdi_repitem.h $(DESTDIR)

install: $(DESTDIR)
	cp $(SRCDIR)/*.c $(SRCDIR)/*.h $(DESTDIR)
	cp $(SRCDIR)/Makefile.linux $(DESTDIR)/Makefile
	cp $$PROJDIR/src/include/cdi_uaccess.h $(DESTDIR)
	cp $$PROJDIR/src/include/cdi_repitem.h $(DESTDIR)

$(DESTDIR):
	mkdir -p $(DESTDIR)

clean:
	/bin/true

clobber:
	/bin/true
