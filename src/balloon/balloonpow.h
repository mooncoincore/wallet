/**
 * PoW implementation of Henry Corrigan-Gibbs' Balloon Hashing Function (https://github.com/henrycg/balloon)
 * Adapted and implemented by barrystyle for mooncoin (https://github.com/mooncoindev/mooncoin)
 * - barrystyle 17082017
*/

#include <stdio.h>
#include <time.h>
#include "balloon.h"
#include "constants.h"
#include "hash_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void balloonpow_init (struct balloon_options *opts);

void balloonpow_hash(const void* input, const void* output);

#ifdef __cplusplus
}
#endif
