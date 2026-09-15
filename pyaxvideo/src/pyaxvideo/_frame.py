# DeviceFrame:设备侧图像的一等对象。
# 帧留在设备上(CMM/解码池),硬件处理(convert/crop/jpeg)不下卡;
# to_numpy() 才做 device→host 拷贝。
import ctypes

from ._capi import FrameInfo

FMT_NV12 = 1
FMT_RGB24 = 2
FMT_BGR24 = 3

_FMT_NAMES = {FMT_NV12: "nv12", FMT_RGB24: "rgb", FMT_BGR24: "bgr"}
_RESIZE_MODES = {"stretch": 0, "keep_aspect": 1}


def _fmt_from_str(fmt):
    m = {"nv12": FMT_NV12, "rgb": FMT_RGB24, "rgb24": FMT_RGB24,
         "bgr": FMT_BGR24, "bgr24": FMT_BGR24}
    v = m.get(str(fmt).lower())
    if v is None:
        raise ValueError("unknown format %r (nv12/rgb/bgr)" % (fmt,))
    return v


class DeviceFrame:
    """设备侧图像。

    重要:来自 VideoReader 的帧引用解码缓冲池,攥着不放会耗尽池子导致解码停摆
    ——迭代中用完即弃(交给 GC 或 close()),需要长期持有先 clone()。
    phys_addr/virt_addr 供 NPU 推理等硬件模块直连,地址在本对象存活期内有效。
    """

    __slots__ = ("_ptr", "_lib", "_info")

    def __init__(self, ptr, lib):
        self._ptr = ptr
        self._lib = lib
        self._info = None

    # ---- 生命周期 ----
    def close(self):
        if self._ptr:
            self._lib.axv_frame_release(self._ptr)
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

    def _err(self):
        e = self._lib.axv_last_error()
        return e.decode("utf-8", "replace") if e else "unknown error"

    def _req(self):
        if not self._ptr:
            raise RuntimeError("frame already released")
        return self._ptr

    # ---- 元信息 ----
    @property
    def info(self):
        if self._info is None:
            fi = FrameInfo()
            if self._lib.axv_frame_get_info(self._req(), ctypes.byref(fi)) != 0:
                raise RuntimeError("frame_get_info failed: " + self._err())
            self._info = fi
        return self._info

    @property
    def width(self):
        return self.info.width

    @property
    def height(self):
        return self.info.height

    @property
    def format(self):
        return _FMT_NAMES.get(self.info.format, "?")

    @property
    def strides(self):
        i = self.info
        return tuple(i.strides[k] for k in range(i.plane_count))

    @property
    def phys_addr(self):
        """各 plane 的物理地址(NPU/硬件模块直连用)。"""
        i = self.info
        return tuple(i.phys_addr[k] for k in range(i.plane_count))

    @property
    def virt_addr(self):
        """库侧虚拟地址;AXCL 下为设备地址,host 不可直接读写。"""
        i = self.info
        return tuple(i.virt_addr[k] for k in range(i.plane_count))

    @property
    def nbytes(self):
        return self.info.byte_size

    def __repr__(self):
        try:
            return "<DeviceFrame %s %ux%u>" % (self.format, self.width, self.height)
        except Exception:
            return "<DeviceFrame released>"

    # ---- 硬件处理(输出新 DeviceFrame,全程在设备侧) ----
    def clone(self):
        """深拷贝到独立 CMM,生命周期与解码缓冲池解耦(长期持有必用)。"""
        ptr = self._lib.axv_frame_clone(self._req())
        if not ptr:
            raise RuntimeError("clone failed: " + self._err())
        return DeviceFrame(ptr, self._lib)

    def convert(self, fmt=None, width=0, height=0, mode="stretch", background=0):
        """IVPS resize + 颜色空间转换。fmt=None 保持原格式;mode: stretch / keep_aspect。"""
        f = _fmt_from_str(fmt) if fmt else 0
        ptr = self._lib.axv_frame_convert(self._req(), f, int(width), int(height),
                                          _RESIZE_MODES[mode], int(background))
        if not ptr:
            raise RuntimeError("convert failed: " + self._err())
        return DeviceFrame(ptr, self._lib)

    def resize(self, width, height, fmt=None, mode="stretch", background=0):
        return self.convert(fmt, width, height, mode, background)

    def crop(self, x, y, w, h, fmt=None, width=0, height=0):
        """硬件裁剪,可同时缩放/转格式(width/height=0 保持裁剪区尺寸)。"""
        f = _fmt_from_str(fmt) if fmt else 0
        ptr = self._lib.axv_frame_crop(self._req(), int(x), int(y), int(w), int(h),
                                       f, int(width), int(height))
        if not ptr:
            raise RuntimeError("crop failed: " + self._err())
        return DeviceFrame(ptr, self._lib)

    def to_jpeg(self, quality=90):
        """硬件 JPEG 编码,返回 bytes(host)。"""
        out = ctypes.POINTER(ctypes.c_uint8)()
        n = ctypes.c_size_t()
        if self._lib.axv_frame_encode_jpeg(self._req(), int(quality),
                                           ctypes.byref(out), ctypes.byref(n)) != 0:
            raise RuntimeError("to_jpeg failed: " + self._err())
        try:
            return ctypes.string_at(out, n.value)
        finally:
            self._lib.axv_buffer_free(out)

    # ---- 下卡 ----
    def to_numpy(self, fmt=None):
        """device → host,返回 numpy。
        fmt=None 按当前格式导出:RGB/BGR → (h, w, 3);NV12 → (h*3//2, w)。
        传 fmt("rgb"/"bgr")时对非匹配格式先走硬件转换再导出。"""
        import numpy as np
        if fmt and _fmt_from_str(fmt) != self.info.format:
            with self.convert(fmt) as tmp:
                return tmp.to_numpy()
        i = self.info
        buf = np.empty(i.byte_size, dtype=np.uint8)
        if self._lib.axv_frame_to_host(self._req(),
                                       buf.ctypes.data_as(ctypes.c_void_p),
                                       buf.nbytes) != 0:
            raise RuntimeError("to_host failed: " + self._err())
        h, w = i.height, i.width
        if i.format in (FMT_RGB24, FMT_BGR24):
            stride = i.strides[0]
            img = buf[: stride * h].reshape(h, stride // 3, 3)
            return np.ascontiguousarray(img[:, :w, :])
        # NV12:y plane + uv plane,按 stride 去 padding 后拼 (h*3/2, w)
        stride = i.strides[0]
        y = buf[: i.plane_sizes[0]].reshape(-1, stride)[:h, :w]
        uv_stride = i.strides[1] if i.plane_count > 1 else stride
        uv = buf[i.plane_sizes[0]: i.plane_sizes[0] + i.plane_sizes[1]]
        uv = uv.reshape(-1, uv_stride)[: h // 2, :w]
        return np.ascontiguousarray(np.vstack([y, uv]))
