# VideoReader:demux + 硬件解码(mp4 / mov / rtsp)。
import ctypes

from ._capi import ReaderOpts
from ._frame import DeviceFrame


class VideoReader:
    """for frame in VideoReader("a.mp4"): ...

    latest_only=False(默认): 每帧模式,有序有界队列,消费太慢丢最旧;
    latest_only=True: 只要最新帧(实时算法常用)。
    realtime: 文件输入是否按源帧率节奏送(默认 False=全速);RTSP 恒为实时。
    """

    def __init__(self, uri, latest_only=False, realtime=False, loop=False,
                 queue_depth=0):
        from . import _require, _err
        self._lib = _require()
        opts = ReaderOpts(int(latest_only), int(realtime), int(loop),
                          int(queue_depth))
        self._ptr = self._lib.axv_reader_open(str(uri).encode(), ctypes.byref(opts))
        if not self._ptr:
            raise RuntimeError("reader open failed: " + _err())

    @property
    def info(self):
        """(codec, width, height, fps) —— codec: "h264"/"h265";fps 可能为 0(未知)。"""
        c = ctypes.c_int32()
        w = ctypes.c_uint32()
        h = ctypes.c_uint32()
        fps = ctypes.c_double()
        self._lib.axv_reader_get_info(self._req(), ctypes.byref(c), ctypes.byref(w),
                                      ctypes.byref(h), ctypes.byref(fps))
        return ("h265" if c.value == 2 else "h264", w.value, h.value, fps.value)

    def read(self, timeout_ms=5000):
        """取下一帧。返回 DeviceFrame;超时返回 None;流结束返回 False。"""
        out = ctypes.c_void_p()
        ret = self._lib.axv_reader_next(self._req(), ctypes.byref(out),
                                        int(timeout_ms))
        if ret == 1:
            return DeviceFrame(out.value, self._lib)
        if ret == 0:
            return None
        if ret == -1:
            return False
        e = self._lib.axv_last_error()
        raise RuntimeError("reader_next failed: " +
                           (e.decode("utf-8", "replace") if e else "?"))

    def __iter__(self):
        return self

    def __next__(self):
        f = self.read(timeout_ms=0)  # 0 = 阻塞等待
        if f is False:
            raise StopIteration
        return f

    def _req(self):
        if not self._ptr:
            raise RuntimeError("reader closed")
        return self._ptr

    def close(self):
        if self._ptr:
            self._lib.axv_reader_close(self._ptr)
            self._ptr = None

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()
