/*
 * evaluate.c - very high-level API to evaluate LABELs or UUIDs
 *
 * This is simular to blkid_get_devname() from resolve.c, but this
 * API supports udev /dev/disk/by-{label,uuid} links.
 *
 * Copyright (C) 2009 Karel Zak <kzak@redhat.com>
 *
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/types.h>
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_SYS_MKDEV_H
#include <sys/mkdev.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#include <stdint.h>
#ifdef HAVE_LIBUUID
#include <uuid/uuid.h>
#endif
#include <stdarg.h>

#include "pathnames.h"
#include "canonicalize.h"

#include "blkdev.h"
#include "blkidP.h"

/* returns zero when the device has NAME=value (LABEL/UUID) */
static int verify_tag(const char *devname, const char *name, const char *value)
{
	blkid_probe pr;
	int fd = -1, rc = -1;
	size_t len;
	const char *data;

	pr = blkid_new_probe();
	if (!pr)
		return -1;

	blkid_probe_set_request(pr, BLKID_PROBREQ_LABEL | BLKID_PROBREQ_UUID);

	fd = open(devname, O_RDONLY);
	if (fd < 0)
		goto done;
	if (blkid_probe_set_device(pr, fd, 0, 0))
		goto done;
	rc = blkid_do_safeprobe(pr);
	if (rc)
		goto done;
	rc = blkid_probe_lookup_value(pr, name, &data, &len);
	if (!rc)
		rc = memcmp(value, data, len);
done:
	DBG(DEBUG_EVALUATE, printf("%s: %s verification %s\n",
			devname, name, rc == 0 ? "PASS" : "FAILED"));
	if (fd >= 0)
		close(fd);
	blkid_free_probe(pr);
	return rc;
}

/**
 * blkid_send_uevent:
 * @devname: absolute path to the device
 *
 * Returns -1 in case of failure, or 0 on success.
 */
int blkid_send_uevent(const char *devname, const char *action)
{
	char uevent[PATH_MAX];
	struct stat st;
	FILE *f;
	int rc = -1;

	DBG(DEBUG_EVALUATE, printf("%s: uevent '%s' requested\n", devname, action));

	if (!devname || !action)
		return -1;
	if (stat(devname, &st) || !S_ISBLK(st.st_mode))
		return -1;

	snprintf(uevent, sizeof(uevent), "/sys/dev/block/%d:%d/uevent",
			major(st.st_rdev), minor(st.st_rdev));

	f = fopen(uevent, "w");
	if (f) {
		rc = 0;
		if (fputs(action, f) >= 0)
			rc = 0;
		fclose(f);
	}
	DBG(DEBUG_EVALUATE, printf("%s: send uevent %s\n",
			uevent, rc == 0 ? "SUCCES" : "FAILED"));
	return rc;
}

static char *evaluate_by_udev(const char *token, const char *value, int uevent)
{
	char dev[PATH_MAX];
	char *path = NULL;
	size_t len;
	struct stat st;

	DBG(DEBUG_EVALUATE,
	    printf("evaluating by udev %s=%s\n", token, value));

	if (!strcmp(token, "UUID"))
		strcpy(dev, _PATH_DEV_BYUUID "/");
	else if (!strcmp(token, "LABEL"))
		strcpy(dev, _PATH_DEV_BYLABEL "/");
	else {
		DBG(DEBUG_EVALUATE,
		    printf("unsupported token %s\n", token));
		return NULL;	/* unsupported tag */
	}

	len = strlen(dev);
	if (blkid_encode_string(value, &dev[len], sizeof(dev) - len) != 0)
		return NULL;

	DBG(DEBUG_EVALUATE,
	    printf("expected udev link: %s\n", dev));

	if (stat(dev, &st))
		goto failed;	/* link or device does not exist */

	if (!S_ISBLK(st.st_mode))
		return NULL;

	path = canonicalize_path(dev);
	if (!path)
		return NULL;

	if (verify_tag(path, token, value))
		goto failed;
	return path;

failed:
	DBG(DEBUG_EVALUATE, printf("failed to evaluate by udev\n"));

	if (uevent && path)
		blkid_send_uevent(path, "change");
	free(path);
	return NULL;
}

static char *evaluate_by_scan(const char *token, const char *value,
		blkid_cache *cache, struct blkid_config *conf)
{
	blkid_cache c = cache ? *cache : NULL;
	char *res;

	DBG(DEBUG_EVALUATE,
	    printf("evaluating by blkid scan %s=%s\n", token, value));

	if (!c) {
		char *cachefile = blkid_get_cache_filename(conf);
		blkid_get_cache(&c, cachefile);
		free(cachefile);
	}
	if (!c)
		return NULL;

	res = blkid_get_devname(c, token, value);

	if (cache)
		*cache = c;
	else
		blkid_put_cache(c);

	return res;
}

/**
 * blkid_evaluate_spec:
 * @token: token name (e.g "LABEL" or "UUID")
 * @value: token data
 * @cache: pointer to cache (or NULL when you don't want to re-use the cache)
 *
 * Returns allocated string with device name.
 */
char *blkid_evaluate_spec(const char *token, const char *value, blkid_cache *cache)
{
	struct blkid_config *conf = NULL;
	char *t = NULL, *v = NULL;
	char *ret = NULL;
	int i;

	if (!token)
		return NULL;

	if (!cache || !*cache)
		blkid_init_debug(0);

	DBG(DEBUG_EVALUATE,
	    printf("evaluating  %s%s%s\n", token, value ? "=" : "",
		   value ? value : ""));

	if (!value) {
		if (!strchr(token, '=')) {
			ret = blkid_strdup(token);
			goto out;
		}
		blkid_parse_tag_string(token, &t, &v);
		if (!t || !v)
			goto out;
		token = t;
		value = v;
	}

	conf = blkid_read_config(NULL);
	if (!conf)
		goto out;

	for (i = 0; i < conf->nevals; i++) {
		if (conf->eval[i] == BLKID_EVAL_UDEV)
			ret = evaluate_by_udev(token, value, conf->uevent);
		else if (conf->eval[i] == BLKID_EVAL_SCAN)
			ret = evaluate_by_scan(token, value, cache, conf);
		if (ret)
			break;
	}

	DBG(DEBUG_EVALUATE,
	    printf("%s=%s evaluated as %s\n", token, value, ret));
out:
	blkid_free_config(conf);
	free(t);
	free(v);
	return ret;
}

#ifdef TEST_PROGRAM
int main(int argc, char *argv[])
{
	blkid_cache cache = NULL;
	char *res;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <token> <value>\n", argv[0]);
		return EXIT_FAILURE;
	}

	blkid_init_debug(0);

	res = blkid_evaluate_spec(argv[1], argv[2], &cache);
	if (res)
		printf("%s\n", res);
	if (cache)
		blkid_put_cache(cache);

	return res ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
