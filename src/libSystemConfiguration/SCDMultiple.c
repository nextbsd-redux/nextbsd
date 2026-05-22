/*
 * SCDMultiple.c — the SCDynamicStore batch store calls
 * (freebsd-launchd-mach port of Apple's SystemConfiguration.fproj
 * SCDGet.c SCDynamicStoreCopyMultiple + SCDSet.c
 * SCDynamicStoreSetMultiple).
 *
 * One MIG round trip moves several keys at once: configget_m fetches
 * the values for a set of keys (and pattern-matched keys); configset_m
 * sets, removes and notifies several keys together.
 *
 * Apple serializes each batch argument as a property list; this port
 * carries them in configd's length-prefixed wire encodings instead
 * (config_wire.h) — a key list for the get keys / patterns / removes /
 * notifies, and a key/value map for the values to fetch or set. Values
 * inside the map are still XML-plist blobs (the SCD.c form).
 */

#include "SCInternal.h"
#include "config_wire.h"


#pragma mark -
#pragma mark Key/value-map encoding

/* CFDictionaryApplyFunction context for encode_kvmap() */
struct kvmap_encode_ctx {
	uint8_t	*buf;
	size_t	cap;
	size_t	off;
	int	err;
};

/* encode one key (CFString) / value (CFPropertyList) pair into the map */
static void
kvmap_encode_one(const void *key, const void *value, void *context)
{
	struct kvmap_encode_ctx	*ctx	= context;
	CFDataRef		keyData;
	CFDataRef		valueData;

	if (ctx->err) {
		return;
	}
	if ((key == NULL) || (CFGetTypeID(key) != CFStringGetTypeID())) {
		ctx->err = 1;
		return;
	}

	keyData = CFStringCreateExternalRepresentation(NULL, (CFStringRef)key,
						       kCFStringEncodingUTF8, 0);
	if (keyData == NULL) {
		ctx->err = 1;
		return;
	}
	valueData = _SCSerialize((CFPropertyListRef)value);
	if (valueData == NULL) {
		CFRelease(keyData);
		ctx->err = 1;
		return;
	}

	if (wire_kvmap_put(ctx->buf, ctx->cap, &ctx->off,
			   CFDataGetBytePtr(keyData),
			   (size_t)CFDataGetLength(keyData),
			   CFDataGetBytePtr(valueData),
			   (size_t)CFDataGetLength(valueData)) != 0) {
		ctx->err = 1;
	}

	CFRelease(keyData);
	CFRelease(valueData);
}

/*
 * Encode a CFDictionary of CFString -> CFPropertyList into configd's
 * wire key/value-map form. Returns 0 with the byte count in *outLen,
 * or -1 on a bad element / buffer overflow.
 */
static int
encode_kvmap(CFDictionaryRef dict, uint8_t *buf, size_t cap, size_t *outLen)
{
	struct kvmap_encode_ctx	ctx;

	*outLen = 0;
	if (dict == NULL) {
		return 0;
	}
	if (CFGetTypeID(dict) != CFDictionaryGetTypeID()) {
		return -1;
	}

	ctx.buf = buf;
	ctx.cap = cap;
	ctx.off = 0;
	ctx.err = 0;
	CFDictionaryApplyFunction(dict, kvmap_encode_one, &ctx);
	if (ctx.err) {
		return -1;
	}

	*outLen = ctx.off;
	return 0;
}


#pragma mark -
#pragma mark Batch store access

CFDictionaryRef
SCDynamicStoreCopyMultiple(SCDynamicStoreRef	store,
			   CFArrayRef		keys,
			   CFArrayRef		patterns)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	uint8_t				keyBuf[CONFIG_DATA_MAX];
	uint8_t				patternBuf[CONFIG_DATA_MAX];
	size_t				keyLen		= 0;
	size_t				patternLen	= 0;
	xmlDataOut			reply;
	mach_msg_type_number_t		replyLen	= 0;
	int				sc_status	= kSCStatusFailed;
	kern_return_t			kr;
	CFMutableDictionaryRef		result;
	const uint8_t			*cur;
	const uint8_t			*end;
	const void			*keyBytes;
	const void			*valueBytes;
	size_t				kl;
	size_t				vl;

	if (!isA_SCDynamicStore(store)) {
		_SCErrorSet(kSCStatusNoStoreSession);
		return NULL;
	}
	if (storePrivate->server == MACH_PORT_NULL) {
		_SCErrorSet(kSCStatusNoStoreServer);
		return NULL;
	}

	if ((_SCEncodeKeyList(keys, keyBuf, sizeof(keyBuf), &keyLen) != 0) ||
	    (_SCEncodeKeyList(patterns, patternBuf, sizeof(patternBuf),
			      &patternLen) != 0)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return NULL;
	}

	kr = configget_m(storePrivate->server,
			 keyBuf, (mach_msg_type_number_t)keyLen,
			 patternBuf, (mach_msg_type_number_t)patternLen,
			 reply, &replyLen,
			 &sc_status);
	if (kr != KERN_SUCCESS) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}
	if (sc_status != kSCStatusOK) {
		_SCErrorSet(sc_status);
		return NULL;
	}

	result = CFDictionaryCreateMutable(NULL, 0,
					   &kCFTypeDictionaryKeyCallBacks,
					   &kCFTypeDictionaryValueCallBacks);
	if (result == NULL) {
		_SCErrorSet(kSCStatusFailed);
		return NULL;
	}

	/* the reply is a key/value map; each value is an XML-plist blob */
	cur = reply;
	end = reply + replyLen;
	while (wire_kvmap_next(&cur, end, &keyBytes, &kl, &valueBytes, &vl)) {
		CFStringRef		key;
		CFPropertyListRef	value;

		key   = CFStringCreateWithBytes(NULL, keyBytes, (CFIndex)kl,
						kCFStringEncodingUTF8, FALSE);
		value = _SCUnserialize(valueBytes, (CFIndex)vl);
		if ((key != NULL) && (value != NULL)) {
			CFDictionarySetValue(result, key, value);
		}
		if (key != NULL)   CFRelease(key);
		if (value != NULL) CFRelease(value);
	}

	_SCErrorSet(kSCStatusOK);
	return result;
}

Boolean
SCDynamicStoreSetMultiple(SCDynamicStoreRef	store,
			  CFDictionaryRef	keysToSet,
			  CFArrayRef		keysToRemove,
			  CFArrayRef		keysToNotify)
{
	SCDynamicStorePrivateRef	storePrivate	= (SCDynamicStorePrivateRef)store;
	uint8_t				setBuf[CONFIG_DATA_MAX];
	uint8_t				removeBuf[CONFIG_DATA_MAX];
	uint8_t				notifyBuf[CONFIG_DATA_MAX];
	size_t				setLen		= 0;
	size_t				removeLen	= 0;
	size_t				notifyLen	= 0;
	int				sc_status	= kSCStatusFailed;
	kern_return_t			kr;

	if (!isA_SCDynamicStore(store)) {
		_SCErrorSet(kSCStatusNoStoreSession);
		return FALSE;
	}
	if (storePrivate->server == MACH_PORT_NULL) {
		_SCErrorSet(kSCStatusNoStoreServer);
		return FALSE;
	}

	if ((encode_kvmap(keysToSet, setBuf, sizeof(setBuf), &setLen) != 0) ||
	    (_SCEncodeKeyList(keysToRemove, removeBuf, sizeof(removeBuf),
			      &removeLen) != 0) ||
	    (_SCEncodeKeyList(keysToNotify, notifyBuf, sizeof(notifyBuf),
			      &notifyLen) != 0)) {
		_SCErrorSet(kSCStatusInvalidArgument);
		return FALSE;
	}

	kr = configset_m(storePrivate->server,
			 setBuf, (mach_msg_type_number_t)setLen,
			 removeBuf, (mach_msg_type_number_t)removeLen,
			 notifyBuf, (mach_msg_type_number_t)notifyLen,
			 &sc_status);
	if (kr != KERN_SUCCESS) {
		_SCErrorSet(kSCStatusFailed);
		return FALSE;
	}
	if (sc_status != kSCStatusOK) {
		_SCErrorSet(sc_status);
		return FALSE;
	}
	return TRUE;
}
