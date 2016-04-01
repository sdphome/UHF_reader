#ifndef __UPPER_H
#define __UPPER_H

#define NO_ERROR			0
#define FAILED				1

#define false				0
#define true				1

#define UPPER_TIMEOUT		3

typedef struct upper_info {
	int sock;
	int status;
	int verbose;

	uint64_t serial;

	pthread_t read_thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;

	pthread_mutex_t wlock;

	LLRP_tSTypeRegistry *pTypeRegistry;
	LLRP_tSConnection *pConn;

} upper_info_t;


#endif
