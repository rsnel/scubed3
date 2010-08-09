/* pthd.h - some error checking versions of pthread()'s
 *
 * Copyright (C) 2009  Rik Snel <rik@snel.it>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdint.h>
#include <pthread.h>
#include <errno.h>
#include <assert.h>
#include "verbose.h"
#include "pthd.h"

void pthd_mutex_destroy(pthread_mutex_t *mutex) {
	int err;
	//VERBOSE("DESTROY %p", mutex);
	if ((err = pthread_mutex_destroy(mutex))) {
		if (err == EBUSY)
			FATAL("trying to destroy locked mutex %p", mutex);
		FATAL("unknown error %d trying to destroy mutex", err);
	}
}

void pthd_mutex_lock(pthread_mutex_t *mutex) {
	int err;
	//VERBOSE("LOCK %p", mutex);
	if ((err = pthread_mutex_lock(mutex))) {
		if (err == EINVAL)
			FATAL("trying to lock uninitialized mutex");
		if (err == EDEADLK)
			FATAL("trying to doubly lock an own mutex");
		FATAL("unknown error %d trying to lock mutex", err);
	}
}

void pthd_mutex_unlock(pthread_mutex_t *mutex) {
	int err;
	//VERBOSE("UNLOCK %p", mutex);
	if ((err = pthread_mutex_unlock(mutex))) {
		if (err == EINVAL)
			FATAL("trying to unlock uninitialized mutex");
		if (err == EPERM)
			FATAL("trying to unlock mutex we do not own");
		FATAL("unknown error %d trying to lock mutex", err);
	}
}

void pthd_mutex_init(pthread_mutex_t *mutex) {
	int err;
	//VERBOSE("INIT %p", mutex);
#ifdef NDEBUG /* make an errorchecking mutex if NDEBUG is not defined */
	if ((err = pthread_mutex_init(mutex, NULL)))
		FATAL("unknown error %d trying to initialize mutex", err);
#else
	pthread_mutexattr_t attr;
	if ((err = pthread_mutexattr_init(&attr)))
		FATAL("unknown error %d trying to init mutexattr", err);
	if ((err = pthread_mutexattr_settype(&attr,
					PTHREAD_MUTEX_ERRORCHECK_NP))) {
		if (err == EINVAL)
			FATAL("invalid argument to pthread_mutexattr_settype");
		FATAL("unknown error %d trying to lock mutex", err);
	}
	if ((err = pthread_mutex_init(mutex, &attr)))
		FATAL("unknown error %d trying to initialize mutex", err);

	if ((err = pthread_mutexattr_destroy(&attr)))
		FATAL("unkown error %d trying to destroy mutexattr", err);
#endif
}

void pthd_rwlock_destroy(pthread_rwlock_t *rwlock) {
	int err;
	//VERBOSE("DESTROY %p", rwlock);
	if ((err = pthread_rwlock_destroy(rwlock))) {
		if (err == EBUSY)
			FATAL("trying to destroy locked rwlock %p", rwlock);
		FATAL("unknown error %d trying to destroy rwlock", err);
	}
}

void pthd_rwlock_rdlock(pthread_rwlock_t *rwlock) {
	int err;
	//VERBOSE("LOCK %p", rwlock);
	if ((err = pthread_rwlock_rdlock(rwlock))) {
		if (err == EINVAL)
			FATAL("trying to lock uninitialized rwlock");
		if (err == EDEADLK)
			FATAL("trying to doubly lock an own rwlock");
		FATAL("unknown error %d trying to lock rwlock", err);
	}
}

void pthd_rwlock_wrlock(pthread_rwlock_t *rwlock) {
	int err;
	//VERBOSE("LOCK %p", rwlock);
	if ((err = pthread_rwlock_wrlock(rwlock))) {
		if (err == EINVAL)
			FATAL("trying to lock uninitialized rwlock");
		if (err == EDEADLK)
			FATAL("trying to doubly lock an own rwlock");
		FATAL("unknown error %d trying to lock rwlock", err);
	}
}

void pthd_rwlock_unlock(pthread_rwlock_t *rwlock) {
	int err;
	//VERBOSE("UNLOCK %p", rwlock);
	if ((err = pthread_rwlock_unlock(rwlock))) {
		if (err == EINVAL)
			FATAL("trying to unlock uninitialized rwlock");
		if (err == EPERM)
			FATAL("trying to unlock rwlock we do not own");
		FATAL("unknown error %d trying to lock rwlock", err);
	}
}

void pthd_rwlock_init(pthread_rwlock_t *rwlock) {
	int err;
	if ((err = pthread_rwlock_init(rwlock, NULL)))
		FATAL("unknown error %d trying to initialize rwlock", err);
}

void pthd_cond_init(pthread_cond_t *cond) {
	int err;
	if ((err = pthread_cond_init(cond, NULL)))
		FATAL("unknown error %d trying to initialze condition variable",
				err);
}

void pthd_cond_signal(pthread_cond_t *cond) {
	int err;
	if ((err = pthread_cond_signal(cond)))
		FATAL("unknown error %d trying to signal condition variable",
				err);
}

void pthd_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
	int err;
	if ((err = pthread_cond_wait(cond, mutex)))
		FATAL("unknown error %d trying to wait on condition variable",
				err);
}

void pthd_cond_destroy(pthread_cond_t *cond) {
	int err;
	if ((err = pthread_cond_destroy(cond))) {
		if (err == EBUSY)
			FATAL("trying to destroy condition variable on "
					"which a thread is waiting");
		FATAL("unknown error %d trying to destroy condition variable",
				err);
	}
}

