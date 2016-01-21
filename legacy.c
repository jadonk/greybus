/*
 * Greybus legacy-protocol driver
 *
 * Copyright 2015 Google Inc.
 * Copyright 2015 Linaro Ltd.
 *
 * Released under the GPLv2 only.
 */

#include "greybus.h"
#include "legacy.h"
#include "protocol.h"


struct legacy_connection {
	struct gb_connection *connection;
	bool initialized;
	u8 protocol_id;
};

struct legacy_data {
	size_t num_cports;
	struct legacy_connection *connections;
};


static int legacy_connection_get_version(struct gb_connection *connection)
{
	int ret;

	ret = gb_protocol_get_version(connection);
	if (ret) {
		dev_err(&connection->hd->dev,
			"%s: failed to get protocol version: %d\n",
			connection->name, ret);
		return ret;
	}

	return 0;
}

static int legacy_connection_bind_protocol(struct legacy_connection *lc)
{
	struct gb_connection *connection = lc->connection;
	struct gb_protocol *protocol;
	u8 major, minor;

	/*
	 * The legacy protocols have always been looked up using a hard-coded
	 * version of 0.1, despite (or perhaps rather, due to) the fact that
	 * module version negotiation could not take place until after the
	 * protocol was bound.
	 */
	major = 0;
	minor = 1;

	protocol = gb_protocol_get(lc->protocol_id, major, minor);
	if (!protocol) {
		dev_err(&connection->hd->dev,
				"protocol 0x%02x version %u.%u not found\n",
				lc->protocol_id, major, minor);
		return -EPROTONOSUPPORT;
	}
	connection->protocol = protocol;

	return 0;
}

static void legacy_connection_unbind_protocol(struct gb_connection *connection)
{
	struct gb_protocol *protocol = connection->protocol;

	gb_protocol_put(protocol);

	connection->protocol = NULL;
}

static int legacy_request_handler(struct gb_operation *operation)
{
	struct gb_protocol *protocol = operation->connection->protocol;

	return protocol->request_recv(operation->type, operation);
}

static int legacy_connection_init(struct legacy_connection *lc)
{
	struct gb_connection *connection = lc->connection;
	gb_request_handler_t handler;
	int ret;

	dev_dbg(&connection->bundle->dev, "%s - %s\n", __func__,
			connection->name);

	ret = legacy_connection_bind_protocol(lc);
	if (ret)
		return ret;

	if (connection->protocol->request_recv)
		handler = legacy_request_handler;
	else
		handler = NULL;

	ret = gb_connection_enable(connection, handler);
	if (ret)
		goto err_unbind_protocol;

	ret = legacy_connection_get_version(connection);
	if (ret)
		goto err_disable;

	ret = connection->protocol->connection_init(connection);
	if (ret)
		goto err_disable;

	lc->initialized = true;

	return 0;

err_disable:
	gb_connection_disable(connection);
err_unbind_protocol:
	legacy_connection_unbind_protocol(connection);

	return ret;
}

static void legacy_connection_exit(struct legacy_connection *lc)
{
	struct gb_connection *connection = lc->connection;

	if (!lc->initialized)
		return;

	gb_connection_disable(connection);

	connection->protocol->connection_exit(connection);

	legacy_connection_unbind_protocol(connection);

	lc->initialized = false;
}

static int legacy_connection_create(struct legacy_connection *lc,
					struct gb_bundle *bundle,
					struct greybus_descriptor_cport *desc)
{
	struct gb_connection *connection;

	connection = gb_connection_create(bundle, le16_to_cpu(desc->id));
	if (IS_ERR(connection))
		return PTR_ERR(connection);

	lc->connection = connection;
	lc->protocol_id = desc->protocol_id;

	return 0;
}

static void legacy_connection_destroy(struct legacy_connection *lc)
{
	gb_connection_destroy(lc->connection);
}

static int legacy_probe(struct gb_bundle *bundle,
			const struct greybus_bundle_id *id)
{
	struct greybus_descriptor_cport *cport_desc;
	struct legacy_data *data;
	struct legacy_connection *lc;
	int i;
	int ret;

	dev_dbg(&bundle->dev,
			"%s - bundle class = 0x%02x, num_cports = %zu\n",
			__func__, bundle->class, bundle->num_cports);

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->num_cports = bundle->num_cports;
	data->connections = kcalloc(data->num_cports,
						sizeof(*data->connections),
						GFP_KERNEL);
	if (!data->connections) {
		ret = -ENOMEM;
		goto err_free_data;
	}

	for (i = 0; i < data->num_cports; ++i) {
		cport_desc = &bundle->cport_desc[i];
		lc = &data->connections[i];

		ret = legacy_connection_create(lc, bundle, cport_desc);
		if (ret)
			goto err_connections_destroy;
	}

	greybus_set_drvdata(bundle, data);

	for (i = 0; i < data->num_cports; ++i) {
		lc = &data->connections[i];

		ret = legacy_connection_init(lc);
		if (ret)
			goto err_connections_disable;
	}

	return 0;

err_connections_disable:
	for (--i; i >= 0; --i)
		legacy_connection_exit(&data->connections[i]);
err_connections_destroy:
	for (i = 0; i < data->num_cports; ++i)
		legacy_connection_destroy(&data->connections[i]);
	kfree(data->connections);
err_free_data:
	kfree(data);

	return ret;
}

static void legacy_disconnect(struct gb_bundle *bundle)
{
	struct legacy_data *data = greybus_get_drvdata(bundle);
	int i;

	dev_dbg(&bundle->dev, "%s - bundle class = 0x%02x\n", __func__,
			bundle->class);

	for (i = 0; i < data->num_cports; ++i) {
		legacy_connection_exit(&data->connections[i]);
		legacy_connection_destroy(&data->connections[i]);
	}

	kfree(data->connections);
	kfree(data);
}

static const struct greybus_bundle_id legacy_id_table[] = {
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_GPIO) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_I2C) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_UART) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_HID) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_USB) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_SDIO) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_POWER_SUPPLY) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_PWM) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_SPI) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_DISPLAY) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_CAMERA) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_SENSOR) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_LIGHTS) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_VIBRATOR) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_LOOPBACK) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_AUDIO_MGMT) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_AUDIO_DATA) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_SVC) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_FIRMWARE) },
	{ GREYBUS_DEVICE_CLASS(GREYBUS_CLASS_RAW) },
	{ }
};
MODULE_DEVICE_TABLE(greybus, legacy_id_table);

static struct greybus_driver legacy_driver = {
	.name		= "legacy",
	.probe		= legacy_probe,
	.disconnect	= legacy_disconnect,
	.id_table	= legacy_id_table,
};

int gb_legacy_init(void)
{
	return greybus_register(&legacy_driver);
}

void gb_legacy_exit(void)
{
	greybus_deregister(&legacy_driver);
}
