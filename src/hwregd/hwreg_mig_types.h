/*
 * hwreg_mig_types.h — C typedefs for the named MIG types in hwreg.defs.
 *
 * MIG records the wire layout of a `type` declaration but does not
 * emit a C typedef for it: the generated stubs use the type name as-is
 * and expect an imported header to define it. hwreg.defs imports this
 * header so both the generated server (hwregServer.{c,h}) and user
 * (hwregUser.c / hwreg.h) sides see the types. The bounds must match
 * the array/string sizes in hwreg.defs (and the hw_node fields in
 * hwreg_registry.h).
 */
#ifndef HWREG_MIG_TYPES_H
#define HWREG_MIG_TYPES_H

#include <stdint.h>

typedef uint64_t	hwreg_id_array_t[128];	/* array[*:128] of uint64_t */
typedef char		hwreg_name_t[32];	/* c_string[32]  */
typedef char		hwreg_path_t[256];	/* c_string[256] */
typedef uint8_t		hwreg_blob_t[2048];	/* array[*:2048] of hwreg_byte_t */

#endif /* HWREG_MIG_TYPES_H */
