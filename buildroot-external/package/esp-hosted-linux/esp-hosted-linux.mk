################################################################################
# ESP-Hosted Linux; source prepared by scripts/prepare-gateway.sh from saved ZIP.
################################################################################
ESP_HOSTED_LINUX_VERSION = f95889a9b62c6228654885291834e01a22610849
ESP_HOSTED_LINUX_SITE = $(BR2_EXTERNAL_F1C200S_PATH)/vendor/esp-hosted-linux
ESP_HOSTED_LINUX_SITE_METHOD = local
ESP_HOSTED_LINUX_LICENSE = GPL-2.0
ESP_HOSTED_LINUX_LICENSE_FILES = host/LICENSE
ESP_HOSTED_LINUX_MODULE_SUBDIRS = host
ESP_HOSTED_LINUX_MODULE_MAKE_OPTS = target=spi

$(eval $(kernel-module))
$(eval $(generic-package))
