#include "radio_lock.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

struct ti_st_radio_lock {
	int fd;
};

static pthread_mutex_t claim_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct ti_st_radio_lock *current_claim;

int ti_st_radio_claim(const char *path, enum ti_st_radio radio,
			struct ti_st_radio_lock **claim)
{
	struct ti_st_radio_lock *new_claim;
	struct flock lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
	int error;

	if (!path || !claim || (radio != TI_ST_RADIO_FM &&
				radio != TI_ST_RADIO_BLUETOOTH))
		return -EINVAL;
	*claim = NULL;
	new_claim = malloc(sizeof(*new_claim));
	if (!new_claim)
		return -ENOMEM;

	pthread_mutex_lock(&claim_mutex);
	if (current_claim) {
		error = -EBUSY;
		goto fail;
	}
	new_claim->fd = open(path, O_CREAT | O_RDWR, 0660);
	if (new_claim->fd < 0) {
		error = -errno;
		goto fail;
	}
	if (fcntl(new_claim->fd, F_SETLK, &lock) < 0) {
		error = errno == EAGAIN || errno == EACCES ? -EBUSY : -errno;
		close(new_claim->fd);
		goto fail;
	}
	if (fcntl(new_claim->fd, F_SETFD, FD_CLOEXEC) < 0) {
		error = -errno;
		close(new_claim->fd);
		goto fail;
	}
	current_claim = new_claim;
	*claim = new_claim;
	pthread_mutex_unlock(&claim_mutex);
	return 0;

fail:
	pthread_mutex_unlock(&claim_mutex);
	free(new_claim);
	return error;
}

void ti_st_radio_release(struct ti_st_radio_lock *claim)
{
	if (!claim)
		return;

	pthread_mutex_lock(&claim_mutex);
	if (claim == current_claim) {
		current_claim = NULL;
		close(claim->fd);
		free(claim);
	}
	pthread_mutex_unlock(&claim_mutex);
}
