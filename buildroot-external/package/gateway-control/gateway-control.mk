GATEWAY_CONTROL_VERSION = 1
GATEWAY_CONTROL_SITE = $(BR2_EXTERNAL_F1C200S_PATH)/package/gateway-control/src
GATEWAY_CONTROL_SITE_METHOD = local
GATEWAY_CONTROL_LICENSE = MIT
GATEWAY_CONTROL_LICENSE_FILES = LICENSE
GATEWAY_CONTROL_DEPENDENCIES = libgpiod cjson

define GATEWAY_CONTROL_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -Werror $(@D)/gateway-timeout.c \
		$(TARGET_LDFLAGS) -o $(@D)/gateway-timeout
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -Werror $(@D)/gateway-ctl.c \
		$(TARGET_LDFLAGS) -lgpiod -o $(@D)/gateway-ctl
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra -Werror $(@D)/gateway-upload.c \
		$(TARGET_LDFLAGS) -o $(@D)/gateway-upload
	$(TARGET_CC) $(TARGET_CFLAGS) -D_DEFAULT_SOURCE -Wall -Wextra -Werror \
		$(@D)/gateway-logger.c $(@D)/gw-common.c $(TARGET_LDFLAGS) -o $(@D)/gateway-logger
	$(TARGET_CC) $(TARGET_CFLAGS) -D_DEFAULT_SOURCE -Wall -Wextra -Werror \
		$(@D)/gateway-api.c $(@D)/gw-common.c $(TARGET_LDFLAGS) -lcjson -o $(@D)/gateway-api
endef

define GATEWAY_CONTROL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/gateway-timeout $(TARGET_DIR)/usr/bin/gateway-timeout
	$(INSTALL) -D -m 0755 $(@D)/gateway-ctl $(TARGET_DIR)/usr/sbin/gateway-ctl
	$(INSTALL) -D -m 0755 $(@D)/gateway-upload $(TARGET_DIR)/usr/bin/gateway-upload
	$(INSTALL) -D -m 0755 $(@D)/gateway-logger $(TARGET_DIR)/usr/bin/gateway-logger
	$(INSTALL) -D -m 0755 $(@D)/gateway-api $(TARGET_DIR)/www/cgi-bin/gateway-api
	ln -sf /www/cgi-bin/gateway-api $(TARGET_DIR)/usr/bin/gateway
endef

define GATEWAY_CONTROL_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(BR2_EXTERNAL_F1C200S_PATH)/package/gateway-control/S45gateway-control \
		$(TARGET_DIR)/etc/init.d/S45gateway-control
endef

$(eval $(generic-package))
