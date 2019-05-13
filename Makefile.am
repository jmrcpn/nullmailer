localstatedir = @localstatedir@/spool/nullmailer
runlockdir = @localstatedir@/run/nullmailer
sysconfdir = @sysconfdir@/nullmailer

EXTRA_DIST = BUGS HOWTO INSTALL TODO spec \
	scripts/nullmailer.run scripts/nullmailer-log.run \
	scripts/nullmailer.service
SUBDIRS = doc lib protocols src test

install-data-local:
	@$(NORMAL_INSTALL)
	$(mkinstalldirs) $(DESTDIR)$(localstatedir)/failed
	chmod 700 $(DESTDIR)$(localstatedir)/failed
	$(mkinstalldirs) $(DESTDIR)$(localstatedir)/queue
	chmod 700 $(DESTDIR)$(localstatedir)/queue
	$(mkinstalldirs) $(DESTDIR)$(localstatedir)/tmp
	chmod 700 $(DESTDIR)$(localstatedir)/tmp
	$(mkinstalldirs) $(DESTDIR)$(sysconfdir)
	$(RM) -f $(DESTDIR)$(localstatedir)/trigger
	mkfifo $(DESTDIR)$(localstatedir)/trigger
	chmod 600 $(DESTDIR)$(localstatedir)/trigger
	$(mkinstalldirs) $(DESTDIR)$(runlockdir)
	chmod 700 $(DESTDIR)$(runlockdir)

install-root:
	chown nullmailer $(DESTDIR)$(localstatedir)/*
	chown nullmailer $(DESTDIR)$(sbindir)/nullmailer-queue
	chmod u+s $(DESTDIR)$(sbindir)/nullmailer-queue
	chown nullmailer $(DESTDIR)$(bindir)/mailq
	chmod u+s $(DESTDIR)$(bindir)/mailq
	chown nullmailer $(DESTDIR)$(runlockdir)

dist-hook:
	sed -e s/@VERSION\@/@VERSION@/ \
		<$(srcdir)/spec >$(distdir)/nullmailer-@VERSION@.spec
