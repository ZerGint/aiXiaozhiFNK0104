from __future__ import annotations

import logging
import socket
import threading
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
        self._address: Optional[str] = None
        self._lock = threading.RLock()
        self._stop_event = threading.Event()
        self._monitor_thread: Optional[threading.Thread] = None

    def _register_address(self, address: str) -> None:
        """Publish the bridge on the current LAN address."""
        with self._lock:
            if self._zeroconf is None:
                return
            if self._info is not None:
                try:
                    self._zeroconf.unregister_service(self._info)
                except Exception:
                    LOGGER.exception("mDNS unregister during address refresh failed")

            info = ServiceInfo(
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
            self._zeroconf.register_service(
                info,
                allow_name_change=False,
                cooperating_responders=True,
            )
            self._info = info
            self._address = address
            LOGGER.info(
                "mDNS service registered type=%s instance=%s address=%s port=%d",
                SERVICE_TYPE,
                SERVICE_NAME,
                address,
                self._port,
            )

    def _monitor_address(self) -> None:
        """Refresh mDNS when DHCP or the active network interface changes."""
        while not self._stop_event.wait(5.0):
            address = _lan_ipv4()
            with self._lock:
                current = self._address
            if address is None or address == current:
                continue
            LOGGER.info("LAN address changed from %s to %s; refreshing mDNS", current, address)
            try:
                self._register_address(address)
            except Exception:
                LOGGER.exception("mDNS address refresh failed")

    def start(self) -> bool:
        address = _lan_ipv4()
        if address is None:
            LOGGER.warning("mDNS disabled: no suitable LAN IPv4 address")
            return False

        try:
            self._zeroconf = Zeroconf()
            self._register_address(address)
            self._stop_event.clear()
            self._monitor_thread = threading.Thread(
                target=self._monitor_address,
                name="fnk-music-mdns",
                daemon=True,
            )
            self._monitor_thread.start()
            return True
        except Exception:
            LOGGER.exception("mDNS registration failed")
            self.stop()
            return False

    def stop(self) -> None:
        self._stop_event.set()
        monitor = self._monitor_thread
        self._monitor_thread = None
        if monitor is not None and monitor is not threading.current_thread():
            monitor.join(timeout=2.0)

        with self._lock:
            zeroconf = self._zeroconf
            info = self._info
            self._zeroconf = None
            self._info = None
            self._address = None
        if zeroconf is None:
            return
        try:
            if info is not None:
                zeroconf.unregister_service(info)
        except Exception:
            LOGGER.exception("mDNS unregister failed")
        finally:
            zeroconf.close()
            LOGGER.info("mDNS service stopped")
