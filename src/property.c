#include "aqualink-internal.h"

#include <errno.h>
#include <libubox/ulog.h>
#include <limits.h>

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

	return prop ? prop->ival : -INT_MAX;
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
	return prop ? prop->string : NULL;
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

	prop->ival = val;
	return 0;
}

int dev_set_int(struct device *dev, const char *name, int val)
{
	return _set_int(&dev->properties, name, val);
}

int _prop_set_generic(struct device *dev, const char *name, const struct property *new)
{
	struct property *prop = _prop_lookup(dev->context_props, name, new->type);
	struct prop_watcher *creep;

	if (!prop)
		return -69;

	list_for_each_entry(creep, &prop->watchers, list) {
		if (!creep->notify_change) {
			ULOG_ERR("Prop %s has watcher with no callback\n", name);
			return -ENODATA;
		}

		creep->notify_change(NULL, name, prop);
	}

	return 0;
}

int prop_set_int(struct device *dev, const char *name, int val)
{
	struct property new = {
		.type = PROP_INT,
		.ival = val,
	};

	return _prop_set_generic(dev, name, &new);
}

int prop_get_creepy(struct device *dev, const char *name, struct prop_watcher *pw)
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

void prop_del_creep(struct prop_watcher *pw)
{
	list_del(&pw->list);
}
