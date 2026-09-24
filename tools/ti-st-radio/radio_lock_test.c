#include "radio_lock.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int child_check(const char *program, const char *path, int expected)
{
	pid_t child = fork();
	int status;

	if (child == 0) {
		execl(program, program, "probe", path, NULL);
		_exit(2);
	}
	if (child < 0 || waitpid(child, &status, 0) != child)
		return -1;
	return WIFEXITED(status) && WEXITSTATUS(status) == expected ? 0 : -1;
}

int main(int argc, char **argv)
{
	char path[] = "/tmp/ti-st-radio-lock-XXXXXX";
	struct ti_st_radio_lock *claim = NULL;
	struct ti_st_radio_lock *duplicate = NULL;
	int fd, status = 1;

	if (argc == 3 && !strcmp(argv[1], "probe")) {
		int result = ti_st_radio_claim(argv[2], TI_ST_RADIO_BLUETOOTH,
					&claim);
		if (result == -EBUSY)
			return 0;
		if (result)
			return 2;
		ti_st_radio_release(claim);
		return 1;
	}

	fd = mkstemp(path);
	if (fd < 0)
		return 1;
	close(fd);
	if (ti_st_radio_claim("/nonexistent/ti-st-radio/lock",
			TI_ST_RADIO_FM, &duplicate) != -ENOENT || duplicate)
		goto done;
	if (ti_st_radio_claim(path, TI_ST_RADIO_FM, &claim))
		goto done;
	if (ti_st_radio_claim(path, TI_ST_RADIO_BLUETOOTH, &duplicate)
			!= -EBUSY || duplicate)
		goto done;
	ti_st_radio_release(duplicate);
	if (child_check(argv[0], path, 0))
		goto done;

	ti_st_radio_release(claim);
	claim = NULL;
	if (child_check(argv[0], path, 1))
		goto done;
	if (ti_st_radio_claim(path, TI_ST_RADIO_BLUETOOTH, &claim))
		goto done;
	if (child_check(argv[0], path, 0))
		goto done;
	status = 0;

done:
	ti_st_radio_release(claim);
	unlink(path);
	if (status)
		fprintf(stderr, "radio lock test failed\n");
	return status;
}
