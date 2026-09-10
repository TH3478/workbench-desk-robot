"""纯软件的 UART/SPI 成帧契约与确定性模拟传输。"""

from .contract import (
    MAX_PAYLOAD_BYTES,
    FrameCrcError,
    FrameError,
    FrameFormatError,
    SequenceError,
    TransportBackpressure,
    TransportClosed,
    TransportIOError,
    TransportKind,
    UartSpiFrame,
    UartSpiSession,
    crc16_ccitt,
)
from .fake_transport import FakeTransport

__all__ = [
    "MAX_PAYLOAD_BYTES",
    "FakeTransport",
    "FrameCrcError",
    "FrameError",
    "FrameFormatError",
    "SequenceError",
    "TransportBackpressure",
    "TransportClosed",
    "TransportIOError",
    "TransportKind",
    "UartSpiFrame",
    "UartSpiSession",
    "crc16_ccitt",
]
