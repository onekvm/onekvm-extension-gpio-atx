SUMMARY = "OneKVM GPIO ATX capability provider"
HOMEPAGE = "https://github.com/onekvm/onekvm-extension-gpio-atx"
LICENSE = "CLOSED"
PV = "0.1.0+git"
PR = "r3"
OPKGBUILDCMD = 'opkg-build -Z gzip'

ONEKVM_COMPONENT_GIT_URI ??= "git://github.com/onekvm/onekvm-extension-gpio-atx.git;protocol=https;branch=main"
ONEKVM_COMPONENT_SRCREV ??= ""
SRC_URI = "${ONEKVM_COMPONENT_GIT_URI}"
SRCREV = "${ONEKVM_COMPONENT_SRCREV}"

inherit pkgconfig systemd

DEPENDS = "systemd json-c"

COMPATIBLE_MACHINE = "^onekvm-nanokvm$"
PACKAGE_ARCH = "${MACHINE_ARCH}"

do_compile() {
    oe_runmake -C ${S} \
        CC="${CC}" PKG_CONFIG="pkg-config"
}

do_install() {
    oe_runmake -C ${S} install \
        DESTDIR="${D}" PREFIX="${prefix}" \
        LIBEXECDIR="${libexecdir}/onekvm" \
        SYSTEMD_UNIT_DIR="${systemd_system_unitdir}" \
        DBUS_CONFIG_DIR="${datadir}/dbus-1/system.d" \
        DBUS_SERVICE_DIR="${datadir}/dbus-1/system-services" \
        ONEKVM_DATA_DIR="${datadir}/onekvm" VERSION="0.1.0" \
        CC="${CC}" PKG_CONFIG="pkg-config"
}

SYSTEMD_SERVICE:${PN} = "onekvm-device-gpio-atx.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

RDEPENDS:${PN} += "dbus onekvm-machine-nanokvm"
RPROVIDES:${PN} += "onekvm-atx-backend"
FILES:${PN} += "${libexecdir}/onekvm ${datadir}/dbus-1 ${datadir}/onekvm/device-backends ${datadir}/onekvm/system-plugins"
