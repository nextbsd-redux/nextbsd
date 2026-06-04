PROG=	kextunload
SRCS=	kextunload.c

# CFBundle bundle parsing → kldunload(2). Needs the CF runtime.
LDADD+=	-lCoreFoundation -l_FoundationICU -ldispatch \
	-lsystem_kernel -lpthread -lBlocksRuntime

.include <bsd.prog.mk>
