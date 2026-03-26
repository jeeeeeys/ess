#pragma once

// Broker settings
#define MQTT_HOST "your-broker-host"
#define MQTT_PORT 8883

// 1 = use TLS, 0 = plain TCP
#define MQTT_USE_TLS 1

// TLS trust anchor
static const char AWS_ROOT_CA[] = R"EOF(
-----BEGIN CERTIFICATE-----
...
-----END CERTIFICATE-----
)EOF";

// Optional mTLS client auth (define both if your broker requires client certificates)
static const char DEVICE_CERT[] = R"EOF(
-----BEGIN CERTIFICATE-----
...
-----END CERTIFICATE-----
)EOF";

static const char PRIVATE_KEY[] = R"EOF(
-----BEGIN PRIVATE KEY-----
...
-----END PRIVATE KEY-----
)EOF";
