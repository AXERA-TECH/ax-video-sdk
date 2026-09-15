# pyaxvideo —— ax-video-sdk 的 Python 绑定
#
# 硬件视频链路交给 AXERA 芯片:解码 / 编码 / JPEG / crop / resize / CSC 全部走
# 专用硬件,帧默认留在设备侧(DeviceFrame),只有 to_numpy() 时才下卡。
#
#   import pyaxvideo as axv
#   axv.init(device=0)                       # backend 自动探测,也可 backend="ax650"
#   with axv.VideoReader("a.mp4") as r:      # mp4 / mov / rtsp://
#       for f in r:                          # f: DeviceFrame(设备侧,零拷贝)
#           small = f.convert("rgb", 640, 384)   # IVPS 硬件 resize+CSC
#           npimg = small.to_numpy()             # 需要时才拷到 host
import ctypes
import os

from ._capi import FrameInfo, ReaderOpts, WriterOpts, load as _load
from ._frame import DeviceFrame, FMT_NV12, FMT_RGB24, FMT_BGR24, _fmt_from_str
from ._reader import VideoReader
from ._writer import VideoWriter

__version__ = "0.1.1"
__all__ = [
    "init", "deinit", "backend", "VideoReader", "VideoWriter", "DeviceFrame",
    "frame_from_numpy", "decode_jpeg",
    "FMT_NV12", "FMT_RGB24", "FMT_BGR24",
]

_lib = None
_backend = None


def _detect_backend():
    # 1) 环境变量强制
    env = os.environ.get("PYAXVIDEO_BACKEND", "").strip().lower()
    if env:
        return env
    # 2) AXCL host 运行库可加载 → axcl(650 主机插卡的机器两者都在,优先用卡)
    try:
        ctypes.CDLL("libaxcl_rt.so")
        return "axcl"
    except OSError:
        pass
    # 3) 板端 MSP
    if os.path.exists("/proc/ax_proc/version"):
        return "ax650"
    raise RuntimeError(
        "no AXERA runtime found (neither libaxcl_rt.so nor /proc/ax_proc); "
        "set backend= or PYAXVIDEO_BACKEND explicitly")


def init(device=0, backend=None):
    """初始化。device: AXCL 设备索引(板端忽略);backend: "axcl" / "ax650" / None=自动。"""
    global _lib, _backend
    if _lib is not None:
        return _backend
    b = (backend or _detect_backend()).lower()
    lib = _load(b)
    if lib.axv_init(int(device)) != 0:
        raise RuntimeError("axv_init failed: " + _err_of(lib))
    _lib = lib
    _backend = b
    return b


def deinit():
    global _lib, _backend
    if _lib is not None:
        _lib.axv_deinit()
        _lib = None
        _backend = None


def backend():
    """当前后端名("axcl"/"ax650"),未初始化返回 None。"""
    return _backend


def _err_of(lib):
    e = lib.axv_last_error()
    return e.decode("utf-8", "replace") if e else "unknown error"


def _require():
    if _lib is None:
        init()  # 全自动:未显式 init 时按默认探测
    return _lib


def _err():
    return _err_of(_require())


def frame_from_numpy(arr, fmt=None):
    """numpy(host) → DeviceFrame。
    (h, w, 3) 默认 BGR;(h*3//2, w) 视为 NV12;fmt 可显式传 "rgb"/"bgr"/"nv12"。"""
    import numpy as np
    lib = _require()
    a = np.ascontiguousarray(arr)
    if fmt is None:
        fmt = "bgr" if (a.ndim == 3 and a.shape[2] == 3) else "nv12"
    f = _fmt_from_str(fmt)
    if a.ndim == 3:
        h, w = a.shape[0], a.shape[1]
    else:
        h, w = a.shape[0] * 2 // 3, a.shape[1]
    ptr = lib.axv_frame_from_host(f, w, h, a.ctypes.data_as(ctypes.c_void_p),
                                  a.nbytes, 0)
    if not ptr:
        raise RuntimeError("frame_from_numpy failed: " + _err())
    return DeviceFrame(ptr, lib)


def decode_jpeg(data):
    """JPEG(bytes) → DeviceFrame(硬件解码,原尺寸 NV12)。"""
    lib = _require()
    buf = bytes(data)
    ptr = lib.axv_decode_jpeg(buf, len(buf))
    if not ptr:
        raise RuntimeError("decode_jpeg failed: " + _err())
    return DeviceFrame(ptr, lib)
