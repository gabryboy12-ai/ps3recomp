/*
 * ps3recomp - SPU_CENSUS: where the lifted SPU programs spend their time.
 * See spu_census.c.
 */
#ifndef PS3RECOMP_SPU_CENSUS_H
#define PS3RECOMP_SPU_CENSUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bracket one run of a lifted SPU program. begin() returns 0 when
 * SPU_CENSUS is not set, and end() then does nothing. */
uint64_t spu_census_begin(void);
void     spu_census_end(int image_id, uint64_t t0);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_SPU_CENSUS_H */
