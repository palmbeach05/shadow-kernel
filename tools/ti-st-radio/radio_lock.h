#ifndef TI_ST_RADIO_LOCK_H
#define TI_ST_RADIO_LOCK_H

enum ti_st_radio {
	TI_ST_RADIO_FM,
	TI_ST_RADIO_BLUETOOTH,
};

struct ti_st_radio_lock;

/**
 * Claim a persistent lock file without waiting; keep the claim through shutdown.
 * Returns 0 on success, -EBUSY for a competing claim, or another negative errno.
 */
int ti_st_radio_claim(const char *path, enum ti_st_radio radio,
			struct ti_st_radio_lock **claim);
/** Release the claim after shutdown; a NULL claim is ignored. */
void ti_st_radio_release(struct ti_st_radio_lock *claim);

#endif
