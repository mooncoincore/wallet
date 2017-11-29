/**
 * PoW implementation of Henry Corrigan-Gibbs' Balloon Hashing Function (https://github.com/henrycg/balloon)
 * Adapted and implemented by barrystyle for mooncoin (https://github.com/mooncoindev/mooncoin)
 * - barrystyle 02112017
*/

#include <stdio.h>
#include <time.h>
#include "balloon.h"
#include "constants.h"
#include "hash_state.h"

static void balloonpow_init (struct balloon_options *opts){
  opts->s_cost = 128;
  opts->t_cost = 1;
  opts->n_threads = 1;
}

void balloonpow_hash(const void* input, const void* output)
{
  int i;
  struct balloon_options opts;
  balloonpow_init(&opts);
  struct hash_state s;
  hash_state_init(&s,&opts,input);
  hash_state_fill(&s,input,input,80);
  for(i=0; i<3; i++)
     hash_state_mix(&s);
  hash_state_extract(&s,output);
  hash_state_free (&s);
}
