/*
 * SCPreferences.c — the SCPreferences client API (freebsd-launchd-mach
 * port of Apple's SystemConfiguration.fproj SCPOpen.c / SCPGet.c /
 * SCPSet.c / SCPRemove.c / SCPList.c / SCPCommit.c).
 *
 * SCPreferences is the persistent counterpart of SCDynamicStore: a
 * property-list file (the system network configuration lives in one)
 * read into memory, edited, then committed back. An SCPreferencesRef
 * is a CoreFoundation runtime type holding the in-memory dictionary;
 * the file is read lazily on first access and written back by
 * SCPreferencesCommitChanges.
 *
 * iter 1 is the synchronous read / edit / commit cycle. Apple also
 * routes privileged writes through SCHelper and guards the file with
 * SCPreferencesLock; this repo has no helper and runs its daemons as
 * root, so iter 1 reads and writes the file directly. The preferences
 * lock, change notifications, SCPreferencesApplyChanges and the path-
 * based accessors are later iterations.
 */

#include "SCInternal.h"
#include <SystemConfiguration/SCPreferences.h>

#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define	PREFS_DEFAULT_DIR	"/Library/Preferences/SystemConfiguration"
#define	PREFS_DEFAULT_CONFIG	"preferences.plist"

/* refuse to load a "preferences file" larger than this — treat as corrupt */
#define	SCP_MAX_PREFS_SIZE	(1 << 20)


/*
 * The SCPreferences session object. A CoreFoundation runtime type.
 */
typedef struct __SCPreferences {
	CFRuntimeBase		cfBase;

	CFStringRef		name;		/* caller's session label */
	CFStringRef		prefsID;	/* caller's preferences id */
	char			*path;		/* resolved file path (malloc'd) */

	CFMutableDictionaryRef	prefs;		/* the in-memory preferences */
	Boolean			accessed;	/* prefs loaded from the file */
	Boolean			changed;	/* prefs edited since the load */
} SCPreferencesPrivate, *SCPreferencesPrivateRef;


#pragma mark -
#pragma mark CoreFoundation runtime type

static CFTypeID		__kSCPreferencesTypeID	= _kCFRuntimeNotATypeID;

static void
__SCPreferencesDeallocate(CFTypeRef cf)
{
	SCPreferencesPrivateRef	prefsPrivate	= (SCPreferencesPrivateRef)cf;

	if (prefsPrivate->name != NULL) {
		CFRelease(prefsPrivate->name);
	}
	if (prefsPrivate->prefsID != NULL) {
		CFRelease(prefsPrivate->prefsID);
	}
	if (prefsPrivate->prefs != NULL) {
		CFRelease(prefsPrivate->prefs);
	}
	if (prefsPrivate->path != NULL) {
		free(prefsPrivate->path);
	}
}

static CFStringRef
__SCPreferencesCopyDescription(CFTypeRef cf)
{
	(void)cf;
	return CFStringCreateWithCString(NULL, "<SCPreferences>",
					 kCFStringEncodingASCII);
}

static const CFRuntimeClass __SCPreferencesClass = {
	.version	= 0,
	.className	= "SCPreferences",
	.finalize	= __SCPreferencesDeallocate,
	.copyDebugDesc	= __SCPreferencesCopyDescription,
};

static void
__SCPreferencesClassInitialize(void)
{
	__kSCPreferencesTypeID = _CFRuntimeRegisterClass(&__SCPreferencesClass);
}

CFTypeID
SCPreferencesGetTypeID(void)
{
	static pthread_once_t	once	= PTHREAD_ONCE_INIT;

	(void) pthread_once(&once, __SCPreferencesClassInitialize);
	return __kSCPreferencesTypeID;
}

static Boolean
isA_SCPreferences(SCPreferencesRef prefs)
{
	return ((prefs != NULL) &&
		(CFGetTypeID(prefs) == SCPreferencesGetTypeID()));
}


#pragma mark -
#pragma mark Session establishment

/*
 * Resolve a prefsID to a file path (malloc'd, caller frees). NULL is
 * the default preferences file; a prefsID containing a '/' is taken
 * as a path; a bare name resolves within the default directory.
 */
static char *
__SCPreferencesPath(CFStringRef prefsID)
{
	char	idbuf[PATH_MAX];
	char	pathbuf[PATH_MAX];

	if (prefsID == NULL) {
		return strdup(PREFS_DEFAULT_DIR "/" PREFS_DEFAULT_CONFIG);
	}

	if (!CFStringGetCString(prefsID, idbuf, sizeof(idbuf),
				kCFStringEncodingUTF8)) {
		return NULL;
	}

	if (strchr(idbuf, '/') != NULL) {
		/* a path — use it as given */
		return strdup(idbuf);
	}

	/* a bare name — resolve within the default directory */
	if (snprintf(pathbuf, sizeof(pathbuf), "%s/%s",
		     PREFS_DEFAULT_DIR, idbuf) >= (int)sizeof(pathbuf)) {
		return NULL;
	}
	return strdup(pathbuf);
}

/* create the directories leading up to `path` (best effort) */
static void
__SCPreferencesMakeParents(const char *path)
{
	char	dir[PATH_MAX];
	char	*slash;
	size_t	i;

	if (strlen(path) >= sizeof(dir)) {
		return;
	}
	strcpy(dir, path);

	slash = strrchr(dir, '/');
	if ((slash == NULL) || (slash == dir)) {
		return;			/* no directory part */
	}
	*slash = '\0';

	for (i = 1; dir[i] != '\0'; i++) {
		if (dir[i] == '/') {
			dir[i] = '\0';
			(void) mkdir(dir, 0755);
			dir[i] = '/';
		}
	}
	(void) mkdir(dir, 0755);
}

static SCPreferencesPrivateRef
__SCPreferencesCreatePrivate(CFAllocatorRef allocator)
{
	SCPreferencesPrivateRef	prefsPrivate;
	CFIndex			extra;

	extra = sizeof(SCPreferencesPrivate) - sizeof(CFRuntimeBase);
	prefsPrivate = (SCPreferencesPrivateRef)
		       _CFRuntimeCreateInstance(allocator,
						SCPreferencesGetTypeID(),
						extra, NULL);
	if (prefsPrivate == NULL) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	prefsPrivate->name	= NULL;
	prefsPrivate->prefsID	= NULL;
	prefsPrivate->path	= NULL;
	prefsPrivate->prefs	= NULL;
	prefsPrivate->accessed	= FALSE;
	prefsPrivate->changed	= FALSE;
	return prefsPrivate;
}

SCPreferencesRef
SCPreferencesCreate(CFAllocatorRef	allocator,
		    CFStringRef		name,
		    CFStringRef		prefsID)
{
	SCPreferencesPrivateRef	prefsPrivate;

	prefsPrivate = __SCPreferencesCreatePrivate(allocator);
	if (prefsPrivate == NULL) {
		return NULL;
	}

	if (name != NULL) {
		prefsPrivate->name = CFStringCreateCopy(allocator, name);
	}
	if (prefsID != NULL) {
		prefsPrivate->prefsID = CFStringCreateCopy(allocator, prefsID);
	}

	prefsPrivate->path = __SCPreferencesPath(prefsID);
	if (prefsPrivate->path == NULL) {
		CFRelease(prefsPrivate);
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	_SCErrorSet(kSCStatusOK);
	return (SCPreferencesRef)prefsPrivate;
}

/*
 * Lazily read the preferences file into memory on first access. A
 * missing, empty or corrupt file simply yields an empty dictionary —
 * the caller "starts fresh".
 */
static void
__SCPreferencesAccess(SCPreferencesRef prefs)
{
	SCPreferencesPrivateRef	prefsPrivate	= (SCPreferencesPrivateRef)prefs;
	int			fd;
	struct stat		sb;

	if (prefsPrivate->accessed) {
		return;
	}

	fd = open(prefsPrivate->path, O_RDONLY);
	if (fd != -1) {
		if ((fstat(fd, &sb) == 0) &&
		    (sb.st_size > 0) &&
		    (sb.st_size <= SCP_MAX_PREFS_SIZE)) {
			uint8_t	*buf	= malloc((size_t)sb.st_size);

			if ((buf != NULL) &&
			    (read(fd, buf, (size_t)sb.st_size) ==
				(ssize_t)sb.st_size)) {
				CFDataRef		data;
				CFPropertyListRef	plist	= NULL;
				CFErrorRef		error	= NULL;

				data = CFDataCreateWithBytesNoCopy(NULL, buf,
								   (CFIndex)sb.st_size,
								   kCFAllocatorNull);
				if (data != NULL) {
					plist = CFPropertyListCreateWithData(
							NULL, data,
							kCFPropertyListImmutable,
							NULL, &error);
				}
				if ((plist != NULL) &&
				    (CFGetTypeID(plist) == CFDictionaryGetTypeID())) {
					prefsPrivate->prefs =
						CFDictionaryCreateMutableCopy(
							NULL, 0,
							(CFDictionaryRef)plist);
				}
				if (plist != NULL) CFRelease(plist);
				if (error != NULL) CFRelease(error);
				if (data != NULL)  CFRelease(data);
			}
			if (buf != NULL) {
				free(buf);
			}
		}
		(void) close(fd);
	}

	if (prefsPrivate->prefs == NULL) {
		/* missing / empty / corrupt file — start fresh */
		prefsPrivate->prefs =
			CFDictionaryCreateMutable(NULL, 0,
						  &kCFTypeDictionaryKeyCallBacks,
						  &kCFTypeDictionaryValueCallBacks);
	}
	prefsPrivate->accessed = TRUE;
}


#pragma mark -
#pragma mark Preferences access

CFPropertyListRef
SCPreferencesGetValue(SCPreferencesRef prefs, CFStringRef key)
{
	SCPreferencesPrivateRef	prefsPrivate	= (SCPreferencesPrivateRef)prefs;
	CFPropertyListRef	value;

	if (!isA_SCPreferences(prefs)) {
		_SCErrorSet(kSCStatusNoPrefsSession);
		return NULL;
	}
	__SCPreferencesAccess(prefs);

	value = CFDictionaryGetValue(prefsPrivate->prefs, key);
	_SCErrorSet((value != NULL) ? kSCStatusOK : kSCStatusNoKey);
	return value;
}

Boolean
SCPreferencesSetValue(SCPreferencesRef	prefs,
		      CFStringRef	key,
		      CFPropertyListRef	value)
{
	SCPreferencesPrivateRef	prefsPrivate	= (SCPreferencesPrivateRef)prefs;

	if (!isA_SCPreferences(prefs)) {
		_SCErrorSet(kSCStatusNoPrefsSession);
		return FALSE;
	}
	if ((key == NULL) || (value == NULL)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	__SCPreferencesAccess(prefs);

	CFDictionarySetValue(prefsPrivate->prefs, key, value);
	prefsPrivate->changed = TRUE;
	return TRUE;
}

Boolean
SCPreferencesRemoveValue(SCPreferencesRef prefs, CFStringRef key)
{
	SCPreferencesPrivateRef	prefsPrivate	= (SCPreferencesPrivateRef)prefs;

	if (!isA_SCPreferences(prefs)) {
		_SCErrorSet(kSCStatusNoPrefsSession);
		return FALSE;
	}
	if (key == NULL) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	__SCPreferencesAccess(prefs);

	CFDictionaryRemoveValue(prefsPrivate->prefs, key);
	prefsPrivate->changed = TRUE;
	return TRUE;
}

CFArrayRef
SCPreferencesCopyKeyList(SCPreferencesRef prefs)
{
	SCPreferencesPrivateRef	prefsPrivate	= (SCPreferencesPrivateRef)prefs;
	CFArrayRef		keys;
	CFIndex			n;

	if (!isA_SCPreferences(prefs)) {
		_SCErrorSet(kSCStatusNoPrefsSession);
		return NULL;
	}
	__SCPreferencesAccess(prefs);

	n = CFDictionaryGetCount(prefsPrivate->prefs);
	if (n > 0) {
		const void	**keyValues;

		keyValues = malloc((size_t)n * sizeof(*keyValues));
		if (keyValues == NULL) {
			_SCErrorSet(kSCStatusFailed);
			return NULL;
		}
		CFDictionaryGetKeysAndValues(prefsPrivate->prefs, keyValues, NULL);
		keys = CFArrayCreate(NULL, keyValues, n, &kCFTypeArrayCallBacks);
		free(keyValues);
	} else {
		keys = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
	}

	_SCErrorSet(kSCStatusOK);
	return keys;
}


#pragma mark -
#pragma mark Commit

Boolean
SCPreferencesCommitChanges(SCPreferencesRef prefs)
{
	SCPreferencesPrivateRef	prefsPrivate	= (SCPreferencesPrivateRef)prefs;
	CFDataRef		xmlData;
	char			newPath[PATH_MAX];
	const uint8_t		*bytes;
	CFIndex			len;
	CFIndex			off;
	int			fd;

	if (!isA_SCPreferences(prefs)) {
		_SCErrorSet(kSCStatusNoPrefsSession);
		return FALSE;
	}
	__SCPreferencesAccess(prefs);

	if (!prefsPrivate->changed) {
		/* nothing to write */
		_SCErrorSet(kSCStatusOK);
		return TRUE;
	}

	xmlData = CFPropertyListCreateData(NULL, prefsPrivate->prefs,
					   kCFPropertyListXMLFormat_v1_0, 0, NULL);
	if (xmlData == NULL) {
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}

	/*
	 * Write to a sibling "-new" file and rename it over the target so
	 * the preferences file is replaced atomically.
	 */
	__SCPreferencesMakeParents(prefsPrivate->path);
	if (snprintf(newPath, sizeof(newPath), "%s-new",
		     prefsPrivate->path) >= (int)sizeof(newPath)) {
		CFRelease(xmlData);
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}

	fd = open(newPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd == -1) {
		CFRelease(xmlData);
		_SCErrorSet(kSCStatusAccessError);
		return FALSE;
	}

	bytes = CFDataGetBytePtr(xmlData);
	len   = CFDataGetLength(xmlData);
	off   = 0;
	while (off < len) {
		ssize_t	w;

		w = write(fd, bytes + off, (size_t)(len - off));
		if (w <= 0) {
			(void) close(fd);
			(void) unlink(newPath);
			CFRelease(xmlData);
			_SCErrorSet(kSCStatusFailed);
			return FALSE;
		}
		off += w;
	}
	CFRelease(xmlData);

	if (close(fd) == -1) {
		(void) unlink(newPath);
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}

	if (rename(newPath, prefsPrivate->path) == -1) {
		(void) unlink(newPath);
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}

	prefsPrivate->changed = FALSE;
	_SCErrorSet(kSCStatusOK);
	return TRUE;
}


#pragma mark -
#pragma mark Path-based access

/*
 * Split a '/'-rooted path into its non-empty components. Returns a
 * retained CFArray of CFString (caller releases), or NULL.
 */
static CFMutableArrayRef
normalizePath(CFStringRef path)
{
	CFStringRef		slash;
	CFArrayRef		split;
	CFMutableArrayRef	elements;
	CFIndex			n;

	if ((path == NULL) || (CFGetTypeID(path) != CFStringGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	slash = CFStringCreateWithCString(NULL, "/", kCFStringEncodingUTF8);
	if (slash == NULL) {
		return NULL;
	}
	if (!CFStringHasPrefix(path, slash)) {
		/* a path must be rooted at "/" */
		CFRelease(slash);
		return NULL;
	}

	split = CFStringCreateArrayBySeparatingStrings(NULL, path, slash);
	CFRelease(slash);
	if (split == NULL) {
		return NULL;
	}
	elements = CFArrayCreateMutableCopy(NULL, 0, split);
	CFRelease(split);
	if (elements == NULL) {
		return NULL;
	}

	/* drop the empty components the leading/trailing/doubled "/" leave */
	n = CFArrayGetCount(elements);
	while (n-- > 0) {
		CFStringRef	element = CFArrayGetValueAtIndex(elements, n);

		if (CFStringGetLength(element) == 0) {
			CFArrayRemoveValueAtIndex(elements, n);
		}
	}
	return elements;
}

/*
 * Walk `path` through the nested preferences dictionaries; on success
 * *entity is the dictionary found there (owned by the session).
 */
static Boolean
getPath(SCPreferencesRef prefs, CFStringRef path, CFDictionaryRef *entity)
{
	SCPreferencesPrivateRef	prefsPrivate	= (SCPreferencesPrivateRef)prefs;
	CFMutableArrayRef	elements;
	CFIndex			i;
	CFIndex			n;
	CFTypeRef		value		= NULL;

	elements = normalizePath(path);
	if (elements == NULL) {
		_SCErrorSet(kSCStatusNoKey);
		return FALSE;
	}

	n = CFArrayGetCount(elements);
	if (n < 1) {
		/* the root path — the whole preferences dictionary */
		__SCPreferencesAccess(prefs);
		*entity = prefsPrivate->prefs;
		CFRelease(elements);
		return TRUE;
	}

	for (i = 0; i < n; i++) {
		CFStringRef	element = CFArrayGetValueAtIndex(elements, i);

		if (i == 0) {
			value = SCPreferencesGetValue(prefs, element);
		} else {
			value = CFDictionaryGetValue((CFDictionaryRef)value,
						     element);
		}
		if ((value == NULL) ||
		    (CFGetTypeID(value) != CFDictionaryGetTypeID())) {
			_SCErrorSet(kSCStatusNoKey);
			CFRelease(elements);
			return FALSE;
		}
	}

	*entity = (CFDictionaryRef)value;
	CFRelease(elements);
	return TRUE;
}

/*
 * Store `entity` at `path` (NULL removes). Apple's algorithm: collect
 * the existing dictionary at each parent level, then rebuild the
 * affected dictionaries bottom-up — each level a mutable copy of the
 * old one with the child replaced — and set the rebuilt top level back
 * with SCPreferencesSetValue. Empty parents are pruned on removal.
 */
static Boolean
setPath(SCPreferencesRef prefs, CFStringRef path, CFDictionaryRef entity)
{
	SCPreferencesPrivateRef	prefsPrivate	= (SCPreferencesPrivateRef)prefs;
	CFMutableArrayRef	elements;
	CFMutableArrayRef	nodes		= NULL;
	CFStringRef		element;
	CFTypeRef		node		= NULL;
	CFTypeRef		newEntity	= NULL;
	CFIndex			i;
	CFIndex			n;
	Boolean			ok		= FALSE;

	if ((entity != NULL) &&
	    (CFGetTypeID(entity) != CFDictionaryGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	elements = normalizePath(path);
	if (elements == NULL) {
		_SCErrorSet(kSCStatusNoKey);
		return FALSE;
	}

	n = CFArrayGetCount(elements);
	if (n < 1) {
		/* the root path — replace the whole preferences dictionary */
		__SCPreferencesAccess(prefs);
		if (prefsPrivate->prefs != NULL) {
			CFRelease(prefsPrivate->prefs);
		}
		if (entity == NULL) {
			prefsPrivate->prefs =
				CFDictionaryCreateMutable(NULL, 0,
					&kCFTypeDictionaryKeyCallBacks,
					&kCFTypeDictionaryValueCallBacks);
		} else {
			prefsPrivate->prefs =
				CFDictionaryCreateMutableCopy(NULL, 0, entity);
		}
		prefsPrivate->changed = TRUE;
		CFRelease(elements);
		return TRUE;
	}

	/* collect the existing dictionary at each parent level */
	nodes = CFArrayCreateMutable(NULL, n - 1, &kCFTypeArrayCallBacks);
	for (i = 0; i < n - 1; i++) {
		element = CFArrayGetValueAtIndex(elements, i);
		if (i == 0) {
			node = SCPreferencesGetValue(prefs, element);
		} else {
			node = CFDictionaryGetValue((CFDictionaryRef)node,
						    element);
		}
		if (node != NULL) {
			CFArrayAppendValue(nodes, node);
		} else {
			/* a missing parent — synthesize an empty dictionary */
			CFDictionaryRef	empty;

			empty = CFDictionaryCreate(NULL, NULL, NULL, 0,
					&kCFTypeDictionaryKeyCallBacks,
					&kCFTypeDictionaryValueCallBacks);
			CFArrayAppendValue(nodes, empty);
			node = empty;
			CFRelease(empty);
		}
		if (CFGetTypeID(node) != CFDictionaryGetTypeID()) {
			_SCErrorSet(kSCStatusNoKey);
			goto done;
		}
	}

	/* the leaf must not overwrite a non-dictionary component */
	element = CFArrayGetValueAtIndex(elements, n - 1);
	if (n > 1) {
		node = CFArrayGetValueAtIndex(nodes, n - 2);
		node = CFDictionaryGetValue((CFDictionaryRef)node, element);
	} else {
		node = SCPreferencesGetValue(prefs, element);
	}
	if ((node != NULL) &&
	    (CFGetTypeID(node) != CFDictionaryGetTypeID())) {
		_SCErrorSet(kSCStatusInvalidArgument);
		goto done;
	}

	/* rebuild the affected dictionaries bottom-up */
	if (entity != NULL) {
		newEntity = CFRetain(entity);
	}
	for (i = n - 1; i >= 0; i--) {
		element = CFArrayGetValueAtIndex(elements, i);
		if (i == 0) {
			if (newEntity != NULL) {
				ok = SCPreferencesSetValue(prefs, element,
							   newEntity);
			} else {
				ok = SCPreferencesRemoveValue(prefs, element);
			}
		} else {
			CFMutableDictionaryRef	newNode;

			node    = CFArrayGetValueAtIndex(nodes, i - 1);
			newNode = CFDictionaryCreateMutableCopy(NULL, 0,
					(CFDictionaryRef)node);
			if (newEntity != NULL) {
				CFDictionarySetValue(newNode, element,
						     newEntity);
				CFRelease(newEntity);
			} else {
				CFDictionaryRemoveValue(newNode, element);
				if (CFDictionaryGetCount(newNode) == 0) {
					/* prune the now-empty parent */
					CFRelease(newNode);
					newNode = NULL;
				}
			}
			newEntity = newNode;
		}
	}
	if (newEntity != NULL) {
		CFRelease(newEntity);
	}

    done :
	if (nodes != NULL) {
		CFRelease(nodes);
	}
	CFRelease(elements);
	return ok;
}

CFDictionaryRef
SCPreferencesPathGetValue(SCPreferencesRef prefs, CFStringRef path)
{
	CFDictionaryRef	entity	= NULL;

	if (!isA_SCPreferences(prefs)) {
		_SCErrorSet(kSCStatusNoPrefsSession);
		return NULL;
	}
	if (!getPath(prefs, path, &entity)) {
		return NULL;
	}
	return entity;
}

Boolean
SCPreferencesPathSetValue(SCPreferencesRef	prefs,
			  CFStringRef		path,
			  CFDictionaryRef	value)
{
	if (!isA_SCPreferences(prefs)) {
		_SCErrorSet(kSCStatusNoPrefsSession);
		return FALSE;
	}
	if (value == NULL) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}
	return setPath(prefs, path, value);
}

Boolean
SCPreferencesPathRemoveValue(SCPreferencesRef prefs, CFStringRef path)
{
	CFDictionaryRef	entity	= NULL;

	if (!isA_SCPreferences(prefs)) {
		_SCErrorSet(kSCStatusNoPrefsSession);
		return FALSE;
	}
	if (!getPath(prefs, path, &entity)) {
		/* nothing at that path to remove */
		return FALSE;
	}
	return setPath(prefs, path, NULL);
}
