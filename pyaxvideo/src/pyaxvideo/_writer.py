# VideoWriter:硬件编码 + 封装(mp4 文件 / rtsp)。
import ctypes

from ._capi import WriterOpts
from ._frame import DeviceFrame


class VideoWriter:
    """w = VideoWriter("out.mp4", 1920, 1080); w.write(frame)

    uri: *.mp4 文件,或 rtsp://0.0.0.0:8554/live(本机起 RTSP server)/
         rtsp://远端(向流媒体服务器推流,断链自动重连)。
    write() 接受 DeviceFrame 或 numpy(自动上卡);尺寸/格式不符时硬件自动 resize/CSC。
    """

    def __init__(self, uri, width, height, codec="h264", fps=30.0,
                 bitrate_kbps=0, gop=0):
        from . import _require, _err
        self._lib = _require()
        c = {"h264": 1, "h265": 2}.get(str(codec).lower())
        if c is None:
            raise ValueError("codec must be h264/h265")
        opts = WriterOpts(c, int(width), int(height), float(fps),
                          int(bitrate_kbps), int(gop))
        self._w = int(width)
        self._h = int(height)
        self._ptr = self._lib.axv_writer_open(str(uri).encode(), ctypes.byref(opts))
        if not self._ptr:
            raise RuntimeError("writer open failed: " + _err())

    def write(self, frame):
        from . import frame_from_numpy, _err
        if not self._ptr:
            raise RuntimeError("writer closed")
        if not isinstance(frame, DeviceFrame):
            with frame_from_numpy(frame) as f:
                return self.write(f)
        # 尺寸/格式与编码目标不一致时,先走 IVPS 硬件转换,
        # 不依赖各平台编码器的内部 resize 路径
        if (frame.width, frame.height) != (self._w, self._h) or frame.format != "nv12":
            with frame.convert("nv12", self._w, self._h) as t:
                return self.write(t)
        if self._lib.axv_writer_write(self._ptr, frame._req()) != 0:
            raise RuntimeError("write failed: " + _err())

    def close(self):
        if self._ptr:
            self._lib.axv_writer_close(self._ptr)
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
