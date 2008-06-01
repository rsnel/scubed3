#ifndef INCLUDE_SCUBED3_UTIL_H
#define INCLUDE_SCUBED3_UTIL_H 1

#include <stdlib.h>

void *ecalloc(size_t, size_t);

char *estrdup(const char*);

/* the stuff below is stolen from /usr/include/utils.h
 * (in Debian) which is not a standard include, but part
 * of libcdparanoia (which is under GPL v2 or later and is
 * Copyright (C) 1999-2006 Monty <monty@xiph.org>) */
static inline int32_t swap32(int32_t x){
  return((((u_int32_t)x & 0x000000ffU) << 24) |
	 (((u_int32_t)x & 0x0000ff00U) <<  8) |
	 (((u_int32_t)x & 0x00ff0000U) >>  8) |
	 (((u_int32_t)x & 0xff000000U) >> 24));
}

#if BYTE_ORDER == LITTLE_ENDIAN

static inline int32_t be32_to_cpu(int32_t x){
  return(swap32(x));
}

static inline int32_t le32_to_cpu(int32_t x){
  return(x);
}

#else

static inline int32_t be32_to_cpu(int32_t x){
  return(x);
}

static inline int32_t le32_to_cpu(int32_t x){
  return(swap32(x));
}


#endif

static inline int32_t cpu_to_be32(int32_t x){
  return(be32_to_cpu(x));
}

static inline int32_t cpu_to_le32(int32_t x){
  return(le32_to_cpu(x));
}

#endif /* INCLUDE_SCUBED3_UTIL_H */
