PROG=	kextload
SRCS=	kextload.c

# CFBundle bundle parsing → kldload(2). Needs the CF runtime.
LDADD+=	-lCoreFoundation -l_FoundationICU -ldispatch \
	-lsystem_kernel -lpthread -lBlocksRuntime

.include <bsd.prog.mk>
