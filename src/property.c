#include "aqualink-internal.h"

#include <errno.h>
#include <libubox/ulog.h>
#include <limits.h>
#include <string.h>

static struct property *_prop_lookup(struct kvlist *kvl, const char *name,
				      int type)
{
	struct property *prop = kvlist_get(kvl, name);

	if (!prop || prop->type != type) {
		ULOG_ERR("OOPS, can't find property %s\n", name);
		return NULL;
	}

	return prop;
}

static int _get_int(struct kvlist *kvl, const char *name)
{
	struct property *prop = _prop_lookup(kvl, name, PROP_INT);

	return prop ? prop->datum.ival : -INT_MAX;
}

int dev_get_int(struct device *dev, const char *name)
{
	return _get_int(&dev->properties, name);
}

int prop_get_int(struct device *dev, const char *name)
{
	return _get_int(dev->context_props, name);
}

static const char *_get_string(struct kvlist *kvl, const char *name)
{
	struct property *prop = _prop_lookup(kvl, name, PROP_STRING);

	/* NULL string if property not found is an acceptable default. */
	return prop ? prop->datum.string : NULL;
}

const char *dev_get_string(struct device *dev, const char *name)
{
	return _get_string(&dev->properties, name);
}

const char *prop_get_string(struct device *dev, const char *name)
{
	return _get_string(dev->context_props, name);
}

int _set_int(struct kvlist *kvl, const char *name, int val)
{
	struct property *prop = _prop_lookup(kvl, name, PROP_INT);

	if (!prop)
		return -INT_MAX;

	prop->datum.ival = val;
	return 0;
}

int dev_set_int(struct device *dev, const char *name, int val)
{
	return _set_int(&dev->properties, name, val);
}

static int set_and_notify(struct device *dev, const char *name,
			  const struct property *new)
{
	struct property *prop = _prop_lookup(dev->context_props, name, new->type);
	struct prop_watcher *creep;

	if (!prop)
		return -EINVAL;

	/* TODO: string properties only have a pointer, and may need strcmp */
	if (!memcmp(&prop->datum, &new->datum, sizeof(prop->datum)))
		return 0;

	memcpy(&prop->datum, &new->datum, sizeof(prop->datum));

	list_for_each_entry(creep, &prop->watchers, list) {
		if (!creep->notify_change) {
			ULOG_ERR("Prop %s has watcher with no callback\n", name);
			return -ENODATA;
		}

		creep->notify_change(creep, name, prop);
	}

	return 0;
}

int prop_set_int(struct device *dev, const char *name, int val)
{
	struct property new = {
		.type = PROP_INT,
		.datum.ival = val,
	};

	return set_and_notify(dev, name, &new);
}

int prop_set_string(struct device *dev, const char *name, const char *string)
{
	struct property new = {
		.type = PROP_STRING,
		.datum.string = string,
	};

	return set_and_notify(dev, name, &new);
}

int prop_add_watcher(struct device *dev, const char *name,
		     struct prop_watcher *pw)
{
	struct property *prop = kvlist_get(dev->context_props, name);

	if (!prop)
		return -EINVAL;

	if (!pw->notify_change)
		return -EINVAL;

	/* HACK: We should not touch list fields directly */
	if (prop->watchers.next == NULL) {
		ULOG_ERR("Property \"%s\" has invalid peeper list.\n", name);
		return -EINVAL;
	}

	list_add_tail(&pw->list, &prop->watchers);
	return 0;
}

void prop_del_watcher(struct prop_watcher *pw)
{
	list_del(&pw->list);
}
