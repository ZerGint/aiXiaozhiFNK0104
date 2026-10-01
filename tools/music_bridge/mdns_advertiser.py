from __future__ import annotations

import logging
import socket
from typing import Optional

from zeroconf import ServiceInfo, Zeroconf


LOGGER = logging.getLogger("fnk_music_bridge.mdns")
SERVICE_TYPE = "_fnk-music._tcp.local."
SERVICE_NAME = "FNK Music Bridge._fnk-music._tcp.local."
SERVICE_PORT = 8765


def _lan_ipv4() -> Optional[str]:
    """Return the address selected by the active default route."""
    candidates: list[str] = []
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            candidates.append(sock.getsockname()[0])
    except OSError:
        pass

    try:
        _, _, addresses = socket.gethostbyname_ex(socket.gethostname())
        candidates.extend(addresses)
    except OSError:
        pass

    for address in candidates:
        if address.startswith(("127.", "169.254.")):
            continue
        if address.startswith(("192.168.23.", "192.168.232.")):
            continue
        try:
            socket.inet_aton(address)
        except OSError:
            continue
        return address
    return None


class MdnsAdvertiser:
    def __init__(self, port: int = SERVICE_PORT) -> None:
        self._port = port
        self._zeroconf: Optional[Zeroconf] = None
        self._info: Optional[ServiceInfo] = None

    def start(self) -> bool:
        address = _lan_ipv4()
        if address is None:
            LOGGER.warning("mDNS disabled: no suitable LAN IPv4 address")
            return False

        try:
            self._zeroconf = Zeroconf()
            self._info = ServiceInfo(
                SERVICE_TYPE,
                SERVICE_NAME,
                addresses=[socket.inet_aton(address)],
                port=self._port,
                properties={
                    b"version": b"1",
                    b"api": b"music-bridge",
                    b"path": b"/",
                },
                server="fnk-music.local.",
            )
            # The bridge instance name is fixed by the protocol. Skip the
            # conflict-probe round trip so startup does not depend on a
            # multicast responder answering within zeroconf's short timeout.
            self._zeroconf.register_service(
                self._info,
                allow_name_change=False,
                cooperating_responders=True,
            )
            LOGGER.info(
                "mDNS service registered type=%s instance=%s address=%s port=%d",
                SERVICE_TYPE,
                SERVICE_NAME,
                address,
                self._port,
            )
            return True
        except Exception:
            LOGGER.exception("mDNS registration failed")
            self.stop()
            return False

    def stop(self) -> None:
        if self._zeroconf is None:
            return
        try:
            if self._info is not None:
                self._zeroconf.unregister_service(self._info)
        except Exception:
            LOGGER.exception("mDNS unregister failed")
        finally:
            self._zeroconf.close()
            self._zeroconf = None
            self._info = None
            LOGGER.info("mDNS service stopped")
