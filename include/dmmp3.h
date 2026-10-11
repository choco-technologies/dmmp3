#ifndef DMMP3_H
#define DMMP3_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmod_types.h"
#include "dmmp3_defs.h"

/**
 * Public API for the dmmp3 module.
 *
 * Functions are declared with the dmod_dmmp3_api(...) macro - dmod's
 * standard pattern for functions callable from other modules (or from this
 * module's own tests/), resolved dynamically by the loader rather than
 * through normal static linkage. See dm_sw_ring/include/dm_sw_ring.h for a
 * fully worked real-world example of the same shape.
 *
 * Definitions in src/dmmp3.c use the matching
 * dmod_dmmp3_api_declaration(...) macro - a plain C function
 * definition here will NOT satisfy these declarations at link time.
 *
 * This is an example interface using the usual "opaque handle" pattern -
 * replace the handle, functions, and struct definition in
 * src/dmmp3.c with your module's real API.
 */

/* Opaque handle - the real struct is defined in src/dmmp3.c */
typedef struct dmmp3* dmmp3_t;

/**
 * Create a new dmmp3 instance.
 *
 * @return A valid handle on success, or NULL on allocation failure.
 */
dmod_dmmp3_api(1.0, dmmp3_t, _create, ( void ));

/**
 * Destroy an instance created by dmmp3_create(). Safe to call with
 * NULL.
 */
dmod_dmmp3_api(1.0, void, _destroy, ( dmmp3_t handle ));

/**
 * Example accessor - replace with your module's real API.
 *
 * @return true if handle is a valid, non-NULL instance.
 */
dmod_dmmp3_api(1.0, bool, _is_valid, ( dmmp3_t handle ));

#endif // DMMP3_H
